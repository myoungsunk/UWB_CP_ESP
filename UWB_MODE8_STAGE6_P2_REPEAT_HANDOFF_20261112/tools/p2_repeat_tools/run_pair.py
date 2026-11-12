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
import secrets
import threading
import time
import sys
from rfb8 import Decoder, CIR_BYTES, RAW_BYTES, read_file
from capture_audit import verify_capture, stage6_accounting
from uart_fragments import ReplayReceiver
from calibration_profile import load_profile, check_record_calibration

STOP_ACK_TIMEOUT_S = 5

def restore_default_baud(link, period_us):
    if period_us < 300000:
        # 921600 baud needs the longer period to pass the firmware UART budget.
        link.send("SET PERIOD_US 300000")
        link.wait(lambda m: m.get("event") == "HELLO" and m.get("period_us") == 300000)
    link.send("BAUD 921600")
    time.sleep(0.2)
    link.disable_replay()
    link.serial.baudrate = 921600
    link.send("HELLO")
    link.wait(lambda m: m.get("event") == "HELLO" and m.get("baud") == 921600)


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
    def __init__(self, name: str, device: str, directory: Path):
        import serial
        self.name, self.device = name, device
        self.serial = serial.Serial(port=None, baudrate=921600, timeout=0.1, write_timeout=2)
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
        self.capture_baseline: dict = {}
        self.fault_events = 0
        self.capture_overflow = False
        self.exception: str | None = None
        self.closing = threading.Event()
        self.uart = (directory/f"{name}.uart").open("wb")
        self.raw = (directory/f"{name}.rfb8").open("wb")
        self.meta = (directory/f"{name}.jsonl").open("w", encoding="utf-8")
        self.thread = threading.Thread(target=self._read, daemon=True)
        self.thread.start()

    def _read(self) -> None:
        try:
            while not self.closing.is_set():
                data = self.serial.read(max(1, min(self.serial.in_waiting, 65536)))
                if not data and self.replay is None:
                    continue
                with self.condition:
                    if data:
                        self.uart.write(data)
                    data = self.filter_wire(data)
                    if self.replay_identity:
                        missing = self.replay.request(time.monotonic())
                        if missing is not None:
                            boot, session = self.replay_identity
                            self.send(f"REC_NACK {boot} {session} {missing}")
                    if not data:
                        continue
                    self.raw.write(data)
                    for packet in self.decoder.feed(data):
                        m = packet.meta
                        self.meta.write(json.dumps(m, ensure_ascii=False)+"\n")
                        if packet.kind == 2 and self.capture_active:
                            m = dict(m)
                            m['_raw_integrity_verified'] = cir_payload_integrity(m, packet.raw)
                            if self.replay_identity:
                                boot, session = self.replay_identity
                                if (m.get('boot'), m.get('session'), m.get('role')) != (boot, session, self.name):
                                    raise ValueError(f"{self.name}: RFB8 replay record identity mismatch")
                                if not m['_raw_integrity_verified']:
                                    raise ValueError(f"{self.name}: RFB8 CIR integrity failure")
                                self.raw.flush()
                                os.fsync(self.raw.fileno())
                                self.send(f"REC_ACK {boot} {session} {packet.sequence}")
                            self.records += 1
                            self.complete += bool(m.get("complete"))
                            if len(self.capture_records) < 100000:
                                self.capture_records.append(m)
                            else:
                                self.capture_overflow = True
                        elif packet.kind == 1:
                            self.events.append(m)
                            if (self.capture_active and
                                    m.get('event') in ('ERROR', 'HARDWARE_FAULT_LATCHED', 'SESSION_DEADLINE',
                                                       'SERIALIZE_OVERFLOW', 'BACKPRESSURE_DROP_POLL',
                                                       'BACKPRESSURE_NO_RF')):
                                self.fault_events += 1
                        self.condition.notify_all()
        except Exception as e:
            self.exception = repr(e)
            with self.condition:
                self.condition.notify_all()

    def filter_wire(self, data: bytes) -> bytes:
        return self.replay.feed(data) if self.replay is not None else data

    def enable_replay(self):
        with self.condition:
            self.replay = ReplayReceiver()

    def disable_replay(self):
        with self.condition:
            self.replay_identity = None
            self.replay = None

    def begin_capture(self, boot=None, session=None) -> None:
        with self.condition:
            self.capture_records.clear()
            self.records = self.complete = 0
            self.fault_events = 0
            self.capture_overflow = False
            self.capture_baseline = self.decoder.statistics()
            self.capture_active = True
            if self.replay is not None:
                self.replay.begin()
                self.replay_identity = (boot, session)
                self.transport_base = dict(self.replay.decoder.counts)

    def send(self, command: str) -> None:
        if "\n" in command or len(command) > 180:
            raise ValueError("Invalid serial command")
        data = (command+"\n").encode("ascii")
        with self.write_lock:
            if self.serial.write(data) != len(data):
                raise OSError(f"{self.name}: incomplete serial command write")
            self.serial.flush()

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

    def close(self) -> None:
        self.closing.set()
        self.thread.join(timeout=3)
        try:
            self.serial.close()
        finally:
            self.thread.join(timeout=1)
            if self.thread.is_alive():
                raise TimeoutError(f"{self.name}: serial reader did not finish")
            with self.raw, self.meta, self.uart:
                pass

    def stats(self) -> dict:
        with self.condition:
            current = self.decoder.statistics()
            base = self.capture_baseline
            delta = {k: (v - base.get(k, 0) if k != 'trailing_bytes' else v)
                     for k, v in current.items()}
            transport = self.replay.statistics() if self.replay else {}
            if transport:
                transport['fragment'] = {k: v - self.transport_base.get(k, 0)
                                         for k, v in transport['fragment'].items()}
            return {"transport": transport, "port": self.device, "records": self.records, "complete": self.complete,
                    "reader_error": self.exception, "fault_events": self.fault_events,
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
            link.send(f"SET STAGE {args.stage}")
            link.wait(lambda m: m.get("event") == "HELLO" and m.get("stage") == args.stage)
            if args.baud != 921600:
                if args.stage == 6:
                    if h.get("record_replay") != 1:
                        raise RuntimeError(f"{name}: Stage 6 replay firmware required")
                    link.enable_replay()
                link.send(f"BAUD {args.baud}")
                time.sleep(0.2)
                link.serial.baudrate = args.baud
                link.send("HELLO")
                link.wait(lambda m: m.get("event") == "HELLO" and m.get("baud") == args.baud)
            link.send(f"SET PERIOD_US {args.period_us}")
            h = link.wait(lambda m: m.get("event") == "HELLO" and m.get("period_us") == args.period_us and m.get("stage") == args.stage)
            if profile:
                binding = profile["roles"][name]
                if h.get("hardware_profile") != binding["hardware_profile"]:
                    raise ValueError(f"{name}: calibration wiring profile mismatch")
                for setting in binding["radios"]:
                    sigma = "none" if setting["sigma_m"] is None else setting["sigma_m"]
                    link.send("ANT {radio} {tx} {rx} {id} {sigma}".format(**setting, sigma=sigma))
                    h = link.wait(lambda m: m.get("event") == "HELLO")
                    if not h.get("hw_ok") or h.get("fault_latched"):
                        raise RuntimeError(f"{name}: calibration register readback failed")
            receipt[name+"_identity"] = h
        tag, anchor = links
        if receipt["tag_identity"]["config_id"] != receipt["anchor_identity"]["config_id"]:
            raise RuntimeError("Paired config IDs do not match")
        for link in links:
            if args.stage == 6 and args.baud == 2000000:
                link.begin_capture(receipt[link.name+"_identity"]["boot"], session)
            else:
                link.begin_capture()
        tag.send(f"START {session} {args.count}")
        tag.wait(lambda m: m.get("event") == "HELLO" and m.get("active"))
        anchor.send(f"START {session} {args.count}")
        anchor.wait(lambda m: m.get("event") == "HELLO" and m.get("active"))
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
        # STOPPED is queued by the firmware after all preceding records. A fresh
        # request ID distinguishes it from the automatic end-of-session event.
        request = secrets.randbits(32) or 1
        shutdown = {link.name: {"command_sent": False, "acknowledged": False} for link in links}
        expected_session = None if 'error' in receipt else session
        for link in links:
            state = shutdown[link.name]
            try:
                link.send(f"STOP {request}")
                state['command_sent'] = True
            except Exception as e:
                state['send_error'] = repr(e)
        for link in links:
            state = shutdown[link.name]
            if state['command_sent']:
                try:
                    ack = link.wait(
                        lambda m: (m.get('event') == 'STOPPED' and m.get('role') == link.name
                                   and m.get('stop_request') == request and
                                   (expected_session is None or m.get('session') == expected_session)),
                        STOP_ACK_TIMEOUT_S, check_faults=False)
                    state.update(acknowledged=True, session=ack.get('session'))
                    if args.stage == 6:
                        link.send("STATUS")
                        state["status"] = link.wait(lambda m: m.get("event") == "HELLO"
                                                    and m.get("role") == link.name
                                                    and m.get("active") is False,
                                                    STOP_ACK_TIMEOUT_S, check_faults=False)
                except Exception as e:
                    state['ack_error'] = repr(e)
            if not link.exception:
                state["capture_snapshot"] = link.stats()
            if args.baud == 2000000 and getattr(link.serial, "baudrate", None) == 2000000 and "status" in state:
                try:
                    restore_default_baud(link, state["status"]["period_us"])
                    state["baud_restored"] = True
                except Exception as e:
                    state["baud_restore_error"] = repr(e)
            try:
                link.close()
            except Exception as e:
                state['close_error'] = repr(e)
            if not link.thread.is_alive():
                receipt[link.name+"_capture"] = state.pop("capture_snapshot", None) or link.stats()
        shutdown_ok = len(links) == 2 and all(
            state['acknowledged'] and (args.stage != 6 or 'status' in state) and 'close_error' not in state for state in shutdown.values())
        receipt['shutdown'] = {'pass': shutdown_ok, 'request': request, 'roles': shutdown}
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
                axes = stage6_grids(args.out, session, by_role.get("anchor", []))
                receipt["stage6"]["common_axis_and_power"] = axes
            except (ImportError, OSError, ValueError, KeyError, TypeError) as exc:
                receipt["stage6"]["grid_error"] = repr(exc)
                axes = {}
            receipt["pass"] = (receipt["pass"] and receipt["stage6"]["strict_gate"] and
                               axes == {"P1": args.count, "P2": args.count})
        try:
            receipt["file_sha256"] = capture_file_hashes(args.out)
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
