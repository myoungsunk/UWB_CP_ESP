#!/usr/bin/env python3
"""USB serial bring-up/capture for two Mode-8 boards. Does NOT flash firmware."""
from __future__ import annotations
import argparse
from collections import deque
import hashlib
import json
import os
from copy import copy
import zlib
from pathlib import Path
from queue import Empty, Full, Queue
import secrets
import threading
import time
import sys
from rfb8 import Decoder, CIR_BYTES, RAW_BYTES, read_file
from capture_audit import verify_capture, stage6_accounting, strict_shutdown_ok
from uart_fragments import ReplayReceiver
from calibration_profile import load_profile, check_record_calibration

STOP_ACK_TIMEOUT_S = 5
FINAL_UART_QUIET_S = 0.04
FINAL_STORAGE_TIMEOUT_S = 5
HANDOFF_MAX_BATCHES = 1024


def send_while_drained(link, command, accept=None):
    """Retry only the firmware's explicit pre-execution transport rejection."""
    def rejected(row):
        return row.get("event") == "ERROR" and row.get("reason") == "RECORD_DRAIN_PENDING"

    with link.condition:
        for row in list(link.events):
            if rejected(row):
                link.events.remove(row)
    for _ in range(4):
        link.send(command)
        try:
            response = link.wait(lambda m: m.get("event") in ("ERROR", "HARDWARE_FAULT_LATCHED")
                                 or accept is not None and accept(m),
                0.12 if accept is None else STOP_ACK_TIMEOUT_S, check_faults=False)
        except TimeoutError:
            if accept is None:  # BAUD success changes the wire before its HELLO.
                return None
            raise
        if response.get("event") not in ("ERROR", "HARDWARE_FAULT_LATCHED"):
            return response
        if not rejected(response):
            raise RuntimeError(f"{link.name}: {response}")
        time.sleep(FINAL_UART_QUIET_S)
    raise RuntimeError(f"{link.name}: transport still busy for {command.partition(' ')[0]}")

def fresh_hello(link, identity, session, *, baud, period_us=None):
    request = secrets.randbits(32) or 1
    link.send(f"HELLO {request}")
    proof = link.wait(lambda m: (
        m.get("event") == "HELLO" and m.get("hello_request") == request
        and m.get("role") == link.name and m.get("boot") == identity["boot"]
        and m.get("firmware") == identity["firmware"] and m.get("drain_protocol") == 1
        and m.get("hw_ok") is True and m.get("fault_latched") is False
        and m.get("active") is False and m.get("session") == session
        and m.get("baud") == baud and (period_us is None or m.get("period_us") == period_us)),
        STOP_ACK_TIMEOUT_S, check_faults=False)
    return {"request": request, "hello": proof}


def restore_default_baud(link, period_us, *, identity=None, session=None):
    if period_us < 300000:
        send_while_drained(link, "SET PERIOD_US 300000", lambda m: (
            m.get("event") == "HELLO" and m.get("period_us") == 300000))
        if identity is not None:
            fresh_hello(link, identity, session, baud=2000000, period_us=300000)
    # seal_capture disables record ACKs/NACKs before this transition.
    send_while_drained(link, "BAUD 921600")
    time.sleep(0.2)
    link.disable_replay()
    with link.write_lock:
        link.serial.baudrate = 921600
    if identity is not None:
        return fresh_hello(link, identity, session, baud=921600, period_us=max(period_us, 300000))
    link.send("HELLO")
    return link.wait(lambda m: m.get("event") == "HELLO" and m.get("baud") == 921600)


def capture_file_hashes(directory):
    hashes = {}
    for role in ("anchor", "tag"):
        for suffix in ("rfb8", "uart", "jsonl"):
            path = directory / f"{role}.{suffix}"
            if path.is_file():
                digest = hashlib.sha256()
                with path.open("rb") as stream:
                    for chunk in iter(lambda: stream.read(1024 * 1024), b""):
                        digest.update(chunk)
                hashes[path.name] = digest.hexdigest()
    return hashes


def verify_shutdown_wire(directory, shutdown):
    """Require each correlated receipt event to occur once in persisted RFB8, in order."""
    for role, state in shutdown.items():
        proofs = [state["stopped"], state["drained"], state["status_proof"]["hello"]]
        restored = state["baud_restore_proof"]["hello"]
        if restored != proofs[-1]:
            proofs.append(restored)
        found = []
        for packet in read_file(directory / f"{role}.rfb8"):
            if packet.kind == 1 and packet.meta in proofs:
                found.append(packet.meta)
        if found != proofs:
            return False
    return True

def cir_payload_integrity(meta: dict, raw: bytes) -> bool:
    """RFB8 CRC authenticates only envelope integrity; verify each CIR slot too."""
    if meta.get('role') == 'anchor':
        return len(raw) == 0
    if meta.get('role') != 'tag' or len(raw) != RAW_BYTES:
        return False
    cms = meta.get('cir')
    if not isinstance(cms, list) or len(cms) != 4:
        return False
    for slot, cm in enumerate(cms):
        if (not isinstance(cm, dict) or cm.get('slot') != slot or
                cm.get('radio') != slot % 2 or cm.get('probe') != slot // 2 + 1):
            return False
        data = raw[slot*CIR_BYTES:(slot+1)*CIR_BYTES]
        if cm.get('present') is True:
            if (cm.get('bytes') != CIR_BYTES or cm.get('crc32') !=
                    zlib.crc32(data) & 0xffffffff):
                return False
            if cm.get('valid') is True and (not any(data) or all(x == 255 for x in data)):
                return False
        elif cm.get('present') is False:
            if cm.get('valid') is not False or any(data):
                return False
        else:
            return False
    return True


class Link:
    def __init__(self, name: str, device: str, directory: Path, initial_baud: int = 921600):
        import serial
        self.name, self.device = name, device
        self.serial = serial.Serial(port=None, baudrate=initial_baud, timeout=0.1, write_timeout=2)
        self.serial.dtr = False
        self.serial.rts = False
        self.serial.port = device
        self.serial.open()
        self.decoder = Decoder()
        self.replay = None
        self.replay_identity = None
        self.write_lock = threading.Lock()
        self.transport_base = {}
        self.condition = threading.Condition()
        self.events: deque[dict] = deque(maxlen=512)
        self.records = self.complete = 0
        self.capture_records: list[dict] = []
        self.capture_active = False
        self.capture_sealed = False
        self.capture_baseline: dict = {}
        self.fault_events = 0
        self.capture_overflow = False
        self.exception: str | None = None
        self.closing = threading.Event()
        self.uart = (directory/f"{name}.uart").open("wb")
        self.raw = (directory/f"{name}.rfb8").open("wb")
        self.meta = (directory/f"{name}.jsonl").open("w", encoding="utf-8")
        self.handoff = Queue(maxsize=HANDOFF_MAX_BATCHES)
        self.handoff_peak = 0
        self.reader_done = threading.Event()
        self.writer_error = None
        self.files_finalized = False
        self.overflow_wire = b""
        self.durable_records = {}
        self.ack_schedule = {}
        self.nack_pending = None
        self.nacks_sent = 0
        self.last_wire_at = time.monotonic()
        self.acks_sent = self.ack_retries_sent = 0
        self.accept_acks = False
        self.writer = threading.Thread(target=self._persist, daemon=True)
        self.thread = threading.Thread(target=self._read, daemon=True)
        self.writer.start()
        self.thread.start()

    def _fail(self, exc, *, writer=False):
        with self.condition:
            if writer:
                self.writer_error = repr(exc)
            if self.exception is None:
                self.exception = repr(exc)
            self.condition.notify_all()

    def _read(self) -> None:
        try:
            while not self.closing.is_set():
                wire = self.serial.read(max(1, min(self.serial.in_waiting, 65536)))
                if not wire and self.replay is None:
                    continue
                error = None
                with self.condition:
                    restored, packets, duplicates, missing = b"", [], [], None
                    identity = self.replay_identity
                    try:
                        restored = self.filter_wire(wire)
                        packets = self.decoder.feed(restored) if restored else []
                        if identity:
                            duplicates = self.replay.take_duplicates()
                            missing = self.replay.request(time.monotonic())
                    except Exception as exc:
                        error = exc
                    if wire or restored or duplicates:
                        try:
                            self.handoff.put_nowait((wire, restored, packets, duplicates,
                                                     self.capture_active, self.capture_sealed,
                                                     identity))
                            self.handoff_peak = max(self.handoff_peak, self.handoff.qsize())
                        except Full as exc:
                            self.capture_overflow = True
                            self.overflow_wire = wire
                            raise RuntimeError(f"{self.name}: durable handoff queue is full") from exc
                    if wire:
                        self.last_wire_at = time.monotonic()
                        self.condition.notify_all()
                    if missing is not None:
                        # A later missing sequence can only supersede a repaired gap.
                        self.nack_pending = identity, missing
                if error is not None:
                    raise error
        except Exception as exc:
            self._fail(exc)
        finally:
            self.reader_done.set()

    def _send_replay(self, command, identity):
        # Baud transitions acquire the same lock and clear permission before
        # disabling fragment parsing. A pending old-baud command then drops.
        with self.write_lock:
            if not self.accept_acks or self.replay_identity != identity:
                return False
            self._write_command(command)
            return True

    def _ack(self, sequence, identity, *, retry=False):
        boot, session = identity
        if self._send_replay(f"REC_ACK {boot} {session} {sequence}", identity):
            with self.condition:
                if retry:
                    self.ack_retries_sent += 1
                else:
                    self.acks_sent += 1
            return True
        return False

    def _retry_acks(self):
        now = time.monotonic()
        with self.condition:
            identity = self.replay_identity
            due = [(seq, attempt) for seq, (deadline, attempt) in self.ack_schedule.items()
                   if now >= deadline]
        for seq, attempt in due:
            sent = self._ack(seq, identity, retry=True) if identity else False
            with self.condition:
                if seq in self.ack_schedule:
                    if sent and attempt < 2:
                        self.ack_schedule[seq] = (time.monotonic() + 0.08, attempt + 1)
                    else:
                        self.ack_schedule.pop(seq)
                self.condition.notify_all()

    def _request_replay(self):
        with self.condition:
            pending, self.nack_pending = self.nack_pending, None
            if pending is None or self.replay is None or pending[1] != self.replay.expected:
                return
        identity, missing = pending
        boot, session = identity
        if self._send_replay(f"REC_NACK {boot} {session} {missing}", identity):
            with self.condition:
                self.nacks_sent += 1

    def _persist(self) -> None:
        try:
            while not self.reader_done.is_set() or not self.handoff.empty():
                try:
                    batch = self.handoff.get(timeout=0.02)
                except Empty:
                    self._request_replay()
                    self._retry_acks()
                    continue
                try:
                    self._persist_batch(*batch)
                    self._request_replay()
                    self._retry_acks()
                finally:
                    with self.condition:
                        self.handoff.task_done()
                        self.condition.notify_all()
        except Exception as exc:
            self._fail(exc, writer=True)

    def _persist_batch(self, wire, restored, packets, duplicates, active, sealed, identity):
        if wire:
            self.uart.write(wire)
        if restored:
            self.raw.write(restored)
        for packet in packets:
            m = packet.meta
            self.meta.write(json.dumps(m, ensure_ascii=False) + "\n")
            if packet.kind == 2 and active:
                m = dict(m)
                m['_raw_integrity_verified'] = cir_payload_integrity(m, packet.raw)
                if identity:
                    boot, session = identity
                    if (m.get('boot'), m.get('session'), m.get('role')) != (boot, session, self.name):
                        raise ValueError(f"{self.name}: RFB8 replay record identity mismatch")
                    if not m['_raw_integrity_verified']:
                        raise ValueError(f"{self.name}: RFB8 CIR integrity failure")
                    self.raw.flush()
                    os.fsync(self.raw.fileno())
                    if not self._ack(packet.sequence, identity):
                        raise RuntimeError(f"{self.name}: durable record ACK disabled")
                    with self.condition:
                        self.durable_records[packet.sequence] = identity
                        if len(self.durable_records) > 64:
                            del self.durable_records[next(iter(self.durable_records))]
                        self.ack_schedule[packet.sequence] = (time.monotonic() + 0.08, 1)
                with self.condition:
                    self.records += 1
                    self.complete += bool(m.get("complete"))
                    if len(self.capture_records) < 100000:
                        self.capture_records.append(m)
                    else:
                        self.capture_overflow = True
                    self.condition.notify_all()
            elif packet.kind == 2 and sealed:
                raise RuntimeError(f"{self.name}: new RFB8 record arrived after host drain seal")
            elif packet.kind == 1:
                with self.condition:
                    self.events.append(m)
                    if ((active or sealed)
                            and m.get("reason") != "RECORD_DRAIN_PENDING"
                            and m.get('event') in
                            ('ERROR', 'HARDWARE_FAULT_LATCHED', 'SESSION_DEADLINE',
                             'SERIALIZE_OVERFLOW', 'BACKPRESSURE_DROP_POLL', 'BACKPRESSURE_NO_RF')):
                        self.fault_events += 1
                    self.condition.notify_all()
        for sequence in duplicates:
            if self.durable_records.get(sequence) != identity:
                raise RuntimeError(f"{self.name}: duplicate has no durable RFB8 record")
            if active and not self._ack(sequence, identity, retry=True):
                raise RuntimeError(f"{self.name}: duplicate ACK disabled")

    def filter_wire(self, data: bytes) -> bytes:
        return self.replay.feed(data) if self.replay is not None else data

    def enable_replay(self):
        with self.condition:
            self.replay = ReplayReceiver()

    def disable_replay(self):
        with self.write_lock, self.condition:
            self.accept_acks = False
            self.ack_schedule.clear()
            self.nack_pending = None
            self.replay_identity = None
            self.replay = None

    def wait_persisted(self, timeout=5):
        deadline = time.monotonic() + timeout
        with self.condition:
            while self.handoff.unfinished_tasks:
                if self.exception:
                    raise RuntimeError(self.exception)
                remaining = deadline - time.monotonic()
                if remaining <= 0:
                    raise TimeoutError(f"{self.name}: durable handoff drain timeout")
                self.condition.wait(remaining)

    def seal_capture(self):
        """Seal replay after correlated firmware drain proof and host persistence."""
        deadline = time.monotonic() + STOP_ACK_TIMEOUT_S
        with self.condition:
            while not self.exception:
                transport = self.replay.statistics() if self.replay else {}
                quiet = FINAL_UART_QUIET_S - (time.monotonic() - self.last_wire_at)
                if (quiet <= 0 and not self.handoff.unfinished_tasks
                        and not transport.get("pending_fragment_bytes")
                        and not transport.get("partial_fragment_record")):
                    break
                left = deadline - time.monotonic()
                if left <= 0:
                    raise TimeoutError(f"{self.name}: host transport tail did not drain")
                self.condition.wait(min(left, max(quiet, FINAL_UART_QUIET_S)))
        with self.write_lock, self.condition:
            state = self.stats()
            transport = state['transport']
            if (self.exception or self.capture_overflow or self.handoff.unfinished_tasks
                    or transport.get('pending_records') or transport.get('unresolved_sequences')
                    or transport.get('pending_fragment_bytes') or transport.get('partial_fragment_record')
                    or any(state['capture_decoder'].values())):
                raise RuntimeError(f"{self.name}: host transport is incomplete after DRAINED")
            self.accept_acks = False
            self.ack_schedule.clear()
            self.nack_pending = None
            self.capture_active = False
            self.capture_sealed = True
            return state

    def begin_capture(self, boot=None, session=None) -> None:
        with self.write_lock, self.condition:
            self.capture_records.clear()
            self.records = self.complete = 0
            self.fault_events = 0
            self.capture_overflow = False
            self.capture_baseline = self.decoder.statistics()
            self.nacks_sent = 0
            self.capture_active = True
            self.capture_sealed = False
            if self.replay is not None:
                self.replay.begin()
                self.replay_identity = (boot, session)
                self.accept_acks = True
                self.transport_base = dict(self.replay.decoder.counts)

    def _write_command(self, command: str) -> None:
        if "\n" in command or len(command) > 180:
            raise ValueError("Invalid serial command")
        data = (command+"\n").encode("ascii")
        if self.serial.write(data) != len(data):
            raise OSError(f"{self.name}: incomplete serial command write")

    def send(self, command: str) -> None:
        with self.write_lock:
            self._write_command(command)

    def wait(self, predicate, timeout: float = 8, *, check_faults: bool = True) -> dict:
        end = time.monotonic()+timeout
        with self.condition:
            while True:
                if self.exception:
                    raise RuntimeError(self.exception)
                for m in list(self.events):
                    if check_faults and m.get("event") in ("ERROR", "HARDWARE_FAULT_LATCHED"):
                        self.events.remove(m)
                        raise RuntimeError(f"{self.name}: {m}")
                    if predicate(m):
                        self.events.remove(m)
                        return m
                left = end-time.monotonic()
                if left <= 0:
                    raise TimeoutError(f"{self.name}: event timeout")
                self.condition.wait(min(left, 0.2))

    def _finish_files(self, errors) -> None:
        try:
            with self.raw, self.meta, self.uart:
                while not self.handoff.empty():
                    self.uart.write(self.handoff.get_nowait()[0])
                self.uart.write(self.overflow_wire)
                for stream in (self.raw, self.meta, self.uart):
                    stream.flush()
                    os.fsync(stream.fileno())
            with self.condition:
                self.files_finalized = True
                self.condition.notify_all()
        except (OSError, ValueError) as exc:
            errors.append(exc)

    def close(self) -> None:
        self.closing.set()
        self.thread.join(timeout=3)
        # Keep command writes available while the final durable ACKs are sent.
        if not self.thread.is_alive():
            self.writer.join(timeout=5)
        try:
            self.serial.close()
        finally:
            if self.thread.is_alive():
                self.thread.join(timeout=1)
            if self.writer.is_alive():
                self.writer.join(timeout=1)
            if self.thread.is_alive() or self.writer.is_alive():
                raise TimeoutError(f"{self.name}: serial reader or durable writer did not finish")
            errors = []
            finalizer = threading.Thread(target=self._finish_files, args=(errors,), daemon=True)
            finalizer.start()
            finalizer.join(timeout=FINAL_STORAGE_TIMEOUT_S)
            if finalizer.is_alive():
                raise TimeoutError(f"{self.name}: final capture storage timed out")
            if not self.files_finalized:
                raise RuntimeError(f"{self.name}: final capture storage failed: {errors}")
            if self.writer_error:
                raise RuntimeError(self.writer_error)

    def stats(self) -> dict:
        with self.condition:
            current = self.decoder.statistics()
            base = self.capture_baseline
            delta = {k: (v - base.get(k, 0) if k != 'trailing_bytes' else v)
                     for k, v in current.items()}
            transport = self.replay.statistics() if self.replay else {}
            if transport:
                transport.update(durable_acks_sent=self.acks_sent,
                                 durable_ack_retries_sent=self.ack_retries_sent,
                                 replay_nacks_sent=self.nacks_sent,
                                 host_pending_batches=self.handoff.unfinished_tasks,
                                 host_peak_batches=self.handoff_peak)
                transport['fragment'] = {k: v - self.transport_base.get(k, 0)
                                         for k, v in transport['fragment'].items()}
            return {"transport": transport, "port": self.device, "records": self.records, "complete": self.complete,
                    "reader_error": self.exception, "writer_error": self.writer_error, "fault_events": self.fault_events,
                    "capture_overflow": self.capture_overflow,
                    "decoder": current, "capture_decoder": delta}


def main() -> int:
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--anchor", required=True)
    p.add_argument("--tag", required=True)
    p.add_argument("--out", type=Path, required=True)
    p.add_argument("--count", type=int, default=20)
    p.add_argument("--stage", type=int, choices=range(2, 7), default=5)
    p.add_argument("--period-us", type=int, default=300000)
    p.add_argument("--baud", type=int, choices=(921600, 2000000), default=921600)
    p.add_argument("--calibration-profile", type=Path)
    p.add_argument("--rf-remeasure-limit", type=int, default=0, choices=range(0, 9),
                   help="After STOP, take at most this many separate Stage 6 exchanges for observed Probe misses")
    args = p.parse_args()
    if not 1 <= args.count <= 100000 or not 100000 <= args.period_us <= 5000000:
        p.error("count or period out of bounds")
    if args.anchor == args.tag:
        p.error("anchor and tag require different serial ports")
    if args.rf_remeasure_limit and args.stage != 6:
        p.error("RF remeasurement requires Stage 6")
    try:
        profile = load_profile(args.calibration_profile) if args.calibration_profile else None
    except (ValueError, OSError) as exc:
        p.error(str(exc))
    receipt = capture(args, profile)
    print(json.dumps(receipt, indent=2, ensure_ascii=False))
    return 0 if receipt["pass"] else 2


def stage6_grids(directory, session, anchors):
    import numpy as np
    from analyze import packet_arrays
    anchors_by_txn = {row["txn"]: row for row in anchors}
    counts = {"P1": 0, "P2": 0}
    for packet in read_file(directory / "tag.rfb8"):
        if packet.kind != 2 or packet.meta.get("session") != session:
            continue
        anchor = anchors_by_txn.get(packet.meta.get("txn"))
        if not anchor:
            continue
        arrays, _ = packet_arrays(packet.meta, packet.raw, anchor)
        for probe in ("P1", "P2"):
            axis = arrays[f"common_{probe.lower()}_delay_s"]
            power = arrays[f"common_{probe.lower()}_power_norm"]
            if axis.size and np.isfinite(axis).all() and np.any(np.isfinite(power).all(axis=0)):
                counts[probe] += 1
    return counts


def capture(args, profile):
    args.out.mkdir(parents=True, exist_ok=True)
    if any((args.out/f"{x}.rfb8").exists() for x in ("anchor", "tag")):
        raise FileExistsError("Output directory already contains a capture; choose a new directory")
    session = secrets.randbelow(0xffffffff)+1
    strict = args.stage == 6
    links: list[Link] = []
    receipt = {"session": session, "source": "serial_hardware_capture",
               "stage": args.stage, "period_us": args.period_us, "baud": args.baud,
               "requested_count": args.count,
               "accuracy_validated": False, "pass": False,
               "calibration_profile": profile,
               "rf_remeasurement": []}
    try:
        for name in ("tag", "anchor"):
            link = Link(name, getattr(args, name), args.out)
            links.append(link)
            time.sleep(0.3)
            link.send("STOP")
            link.wait(lambda m: m.get("event") == "STOPPED")
            link.send("HELLO")
            h = link.wait(lambda m: m.get("event") == "HELLO" and m.get("role") == name)
            if not h.get("hw_ok") or h.get("fault_latched"):
                raise RuntimeError(f"{name}: hardware health failed; inspect wiring/power, do not repeatedly reset blindly")
            link.send("SELFTEST")
            st = link.wait(lambda m: m.get("event") == "SELFTEST")
            if not st.get("pass"):
                raise RuntimeError(f"{name}: SELFTEST failed: {st}")
            send_while_drained(link, f"SET STAGE {args.stage}",
                               lambda m: m.get("event") == "HELLO" and m.get("stage") == args.stage)
            if args.baud != 921600:
                if args.stage == 6:
                    if h.get("record_replay") != 1:
                        raise RuntimeError(f"{name}: Stage 6 replay firmware required")
                send_while_drained(link, f"BAUD {args.baud}")
                if args.stage == 6:
                    link.enable_replay()
                time.sleep(0.2)
                link.serial.baudrate = args.baud
                link.send("HELLO")
                link.wait(lambda m: m.get("event") == "HELLO" and m.get("baud") == args.baud)
            h = send_while_drained(link, f"SET PERIOD_US {args.period_us}", lambda m: (
                m.get("event") == "HELLO" and m.get("period_us") == args.period_us
                and m.get("stage") == args.stage))
            if profile:
                binding = profile["roles"][name]
                if h.get("hardware_profile") != binding["hardware_profile"]:
                    raise ValueError(f"{name}: calibration wiring profile mismatch")
                for setting in binding["radios"]:
                    sigma = "none" if setting["sigma_m"] is None else setting["sigma_m"]
                    h = send_while_drained(link, "ANT {radio} {tx} {rx} {id} {sigma}".format(
                        **setting, sigma=sigma), lambda m: m.get("event") == "HELLO")
                    if not h.get("hw_ok") or h.get("fault_latched"):
                        raise RuntimeError(f"{name}: calibration register readback failed")
            receipt[name+"_identity"] = h
        tag, anchor = links
        if strict and any(receipt[link.name+"_identity"].get("drain_protocol") != 1 for link in links):
            raise RuntimeError("Stage 6 requires both boards to advertise the shutdown drain protocol")
        if receipt["tag_identity"]["config_id"] != receipt["anchor_identity"]["config_id"]:
            raise RuntimeError("Paired config IDs do not match")
        for link in links:
            if args.stage == 6 and args.baud == 2000000:
                link.begin_capture(receipt[link.name+"_identity"]["boot"], session)
            else:
                link.begin_capture()
        for link in (tag, anchor):
            send_while_drained(link, f"START {session} {args.count}", lambda m: (
                m.get("event") == "HELLO" and m.get("active")
                and (not strict or (m.get("session"), m.get("boot")) ==
                     (session, receipt[link.name + "_identity"]["boot"]))))
        deadline = time.monotonic()+args.count*args.period_us/1e6+20
        while anchor.records < args.count:
            if time.monotonic() > deadline:
                raise TimeoutError("Run deadline; partial capture preserved")
            if any(x.exception for x in links):
                raise RuntimeError("Serial reader failed")
            for link in links:
                with link.condition:
                    if any(m.get("event") == "HARDWARE_FAULT_LATCHED" for m in link.events):
                        raise RuntimeError(f"{link.name}: hardware integrity fault")
            time.sleep(0.1)
        while any(link.records < args.count for link in links):
            if time.monotonic() > deadline + 10:
                raise TimeoutError('Paired record drain deadline; partial capture preserved')
            if any(x.exception for x in links):
                raise RuntimeError('Serial reader failed while draining records')
            time.sleep(0.1)
        time.sleep(0.3)
    except Exception as e:
        receipt["error"] = str(e)
    finally:
        request = secrets.randbits(32) or 1
        shutdown = {link.name: {"command_sent": False, "acknowledged": False} for link in links}
        expected_session = None if 'error' in receipt else session
        for link in links:
            state = shutdown[link.name]
            try:
                link.send(f"STOP {request}")
                state['command_sent'] = True
            except Exception as exc:
                state['send_error'] = repr(exc)
        for link in links:
            state = shutdown[link.name]
            identity = receipt.get(link.name+"_identity")
            capture_snapshot = None
            if state['command_sent']:
                try:
                    ack = link.wait(
                        lambda m: (m.get('event') == 'STOPPED' and m.get('role') == link.name
                                   and m.get('stop_request') == request
                                   and (expected_session is None or m.get('session') == expected_session)
                                   and (not strict or identity is not None and m.get('boot') == identity.get('boot'))),
                        STOP_ACK_TIMEOUT_S, check_faults=False)
                    state.update(acknowledged=True, session=ack.get('session'), stopped=ack)
                except Exception as exc:
                    state['ack_error'] = repr(exc)
            if strict and state['acknowledged'] and identity:
                drain_session = state['session']
                drain_request = secrets.randbits(32) or 1
                state['drain_request'] = drain_request
                try:
                    link.send(f"DRAIN {drain_session} {identity['boot']} {drain_request}")
                    state['drained'] = link.wait(lambda m: (
                        m.get('event') == 'DRAINED' and m.get('role') == link.name
                        and m.get('session') == drain_session and m.get('boot') == identity['boot']
                        and m.get('request') == drain_request), STOP_ACK_TIMEOUT_S + 2,
                        check_faults=False)
                    drain = state['drained']
                    if (drain.get('ok') is not True or drain.get('rf_stopped') is not True
                            or drain.get('reason') != 'OK' or any(
                                type(drain.get(k)) is not int or drain[k] != 0
                                for k in ('pending', 'awaiting_ack', 'failures'))):
                        raise RuntimeError(f"{link.name}: firmware did not prove a clean drain: {drain}")
                    capture_snapshot = link.seal_capture()
                    state['host_sealed'] = True
                    baud = link.serial.baudrate
                    state['status_proof'] = fresh_hello(link, identity, drain_session, baud=baud)
                    state['status'] = state['status_proof']['hello']
                    if baud == 2000000:
                        state['baud_restore_proof'] = restore_default_baud(
                            link, state['status']['period_us'], identity=identity, session=drain_session)
                        state['baud_restored'] = True
                    else:
                        state['baud_restore_proof'] = state['status_proof']
                        state['baud_restored'] = baud == 921600
                except Exception as exc:
                    state['drain_or_restore_error'] = repr(exc)
            elif not strict and args.baud == 2000000 and link.serial.baudrate == 2000000 and state['acknowledged']:
                try:
                    restore_default_baud(link, args.period_us)
                    state['baud_restored'] = True
                except Exception as exc:
                    state['baud_restore_error'] = repr(exc)
            try:
                link.close()
                state['closed'] = True
            except Exception as exc:
                state['close_error'] = repr(exc)
                print(f"{link.name}: capture close failed: {exc}", file=sys.stderr, flush=True)
            if not link.thread.is_alive() and not link.writer.is_alive():
                final = link.stats()
                if capture_snapshot is not None:
                    final["transport"] = capture_snapshot["transport"]
                    final["capture_decoder"] = capture_snapshot["capture_decoder"]
                receipt[link.name+"_capture"] = final
        receipt['shutdown'] = {'contract': 'drain_v1' if strict else 'legacy_stop',
                               'request': request, 'roles': shutdown}
        files_finalized = all(state.get('closed') for state in shutdown.values())
        if strict:
            receipt['shutdown']['persisted_control_proof'] = False
            if len(shutdown) == 2 and all(state.get('closed') and 'baud_restore_proof' in state
                                          for state in shutdown.values()):
                try:
                    receipt['shutdown']['persisted_control_proof'] = verify_shutdown_wire(args.out, shutdown)
                except OSError as exc:
                    receipt['shutdown']['persisted_control_error'] = repr(exc)
            shutdown_ok = strict_shutdown_ok(receipt)
        else:
            shutdown_ok = len(links) == 2 and all(
                state['acknowledged'] and state.get('closed') and 'baud_restore_error' not in state
                for state in shutdown.values())
        receipt['shutdown']['pass'] = shutdown_ok
        if all(r+"_capture" in receipt and r+"_identity" in receipt for r in ("tag", "anchor")):
            try:
                audited = verify_capture(
                    session=session, count=args.count, stage=args.stage, period_us=args.period_us,
                    identities={r: receipt[r+"_identity"] for r in ("tag", "anchor")},
                    records={link.name: link.capture_records for link in links},
                    stats={link.name: receipt[link.name+"_capture"] for link in links})
                receipt['audit'] = {'pass': audited.passed,
                                    'problems': audited.problems, 'details': audited.details}
                receipt['pass'] = ('error' not in receipt and audited.passed and shutdown_ok and
                                   not any(link.capture_overflow for link in links))
                if profile:
                    for link in links:
                        check_record_calibration(profile, link.name, link.capture_records)
            except Exception as e:
                receipt['audit'] = {'pass': False, 'problems': ['AUDIT_EXCEPTION'],
                                    'exception': repr(e)}
                receipt['pass'] = False
            if any(link.capture_overflow for link in links):
                receipt['audit']['problems'].append('CAPTURE_AUDIT_MEMORY_LIMIT')
                receipt['pass'] = False
        if args.stage == 6:
            by_role = {link.name: link.capture_records for link in links}
            receipt["stage6"] = stage6_accounting(receipt, by_role)
            try:
                if not files_finalized:
                    raise ValueError("Capture files did not finish closing")
                axes = stage6_grids(args.out, session, by_role.get("anchor", []))
                receipt["stage6"]["common_axis_and_power"] = axes
            except (ImportError, OSError, ValueError, KeyError, TypeError) as exc:
                receipt["stage6"]["grid_error"] = repr(exc)
                axes = {}
            receipt["pass"] = (receipt["pass"] and receipt["stage6"]["strict_gate"] and
                               axes == {"P1": args.count, "P2": args.count})
        try:
            if files_finalized:
                receipt["file_sha256"] = capture_file_hashes(args.out)
            else:
                receipt["file_hash_error"] = "Capture files did not finish closing"
        except OSError as exc:
            receipt["file_hash_error"] = repr(exc)
            receipt["pass"] = False
        (args.out/"capture_receipt.json").write_text(json.dumps(receipt, indent=2, ensure_ascii=False), encoding="utf-8")
    if args.rf_remeasure_limit and receipt.get("stage6", {}).get("followup_safe"):
        for parent_txn in receipt["stage6"]["probe_miss_txns"][:args.rf_remeasure_limit]:
            retry = copy(args)
            retry.count = 1
            retry.rf_remeasure_limit = 0
            retry.out = args.out / "rf_remeasure" / str(parent_txn)
            result = capture(retry, profile)
            receipt["rf_remeasurement"].append({
                "original_txn": parent_txn, "session": result["session"],
                "verified": result["pass"], "receipt": str(retry.out / "capture_receipt.json"),
            })
            recovered = {row["original_txn"] for row in receipt["rf_remeasurement"] if row["verified"]}
            receipt["stage6"]["confirmed_after_separate_rf_remeasurement"] = {
                probe: row["both_endpoint_evidence_confirmed"] + len(recovered.intersection(row["probe_miss_txns"]))
                for probe, row in receipt["stage6"]["rf"].items()
            }
            (args.out/"capture_receipt.json").write_text(json.dumps(receipt, indent=2, ensure_ascii=False), encoding="utf-8")
            if not result.get("stage6", {}).get("followup_safe"):
                break
    return receipt

if __name__ == "__main__":
    sys.exit(main())
