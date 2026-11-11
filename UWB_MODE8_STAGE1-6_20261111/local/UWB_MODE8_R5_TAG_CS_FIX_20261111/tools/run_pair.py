#!/usr/bin/env python3
"""USB serial bring-up/capture for two Mode-8 boards. Does NOT flash firmware."""
from __future__ import annotations
import argparse
from collections import deque
import json
import zlib
from pathlib import Path
import secrets
import threading
import time
import sys
from rfb8 import Decoder, CIR_BYTES, RAW_BYTES
from capture_audit import verify_capture

STOP_ACK_TIMEOUT_S = 5

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
        self.raw = (directory/f"{name}.rfb8").open("wb")
        self.meta = (directory/f"{name}.jsonl").open("w", encoding="utf-8")
        self.thread = threading.Thread(target=self._read, daemon=True)
        self.thread.start()

    def _read(self) -> None:
        try:
            while not self.closing.is_set():
                data = self.serial.read(max(1, min(self.serial.in_waiting, 65536)))
                if not data:
                    continue
                self.raw.write(data)
                with self.condition:
                    for packet in self.decoder.feed(data):
                        m = packet.meta
                        self.meta.write(json.dumps(m, ensure_ascii=False)+"\n")
                        if packet.kind == 2 and self.capture_active:
                            m = dict(m)
                            m['_raw_integrity_verified'] = cir_payload_integrity(m, packet.raw)
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

    def begin_capture(self) -> None:
        with self.condition:
            self.capture_records.clear()
            self.records = self.complete = 0
            self.fault_events = 0
            self.capture_overflow = False
            self.capture_baseline = self.decoder.statistics()
            self.capture_active = True

    def send(self, command: str) -> None:
        if "\n" in command or len(command) > 180:
            raise ValueError("Invalid serial command")
        data = (command+"\n").encode("ascii")
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
            with self.raw, self.meta:
                pass

    def stats(self) -> dict:
        with self.condition:
            current = self.decoder.statistics()
            base = self.capture_baseline
            delta = {k: (v - base.get(k, 0) if k != 'trailing_bytes' else v)
                     for k, v in current.items()}
            return {"port": self.device, "records": self.records, "complete": self.complete,
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
    args = p.parse_args()
    if not 1 <= args.count <= 100000 or not 100000 <= args.period_us <= 5000000:
        p.error("count or period out of bounds")
    if args.anchor == args.tag:
        p.error("anchor and tag require different serial ports")
    args.out.mkdir(parents=True, exist_ok=True)
    if any((args.out/f"{x}.rfb8").exists() for x in ("anchor", "tag")):
        p.error("Output directory already contains a capture; choose a new directory")
    session = secrets.randbelow(0xffffffff)+1
    links: list[Link] = []
    receipt = {"session": session, "source": "serial_hardware_capture",
               "stage": args.stage, "requested_count": args.count,
               "accuracy_validated": False, "pass": False}
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
            if args.baud != 921600:
                link.send(f"BAUD {args.baud}")
                time.sleep(0.2)
                link.serial.baudrate = args.baud
                link.send("HELLO")
                link.wait(lambda m: m.get("event") == "HELLO" and m.get("baud") == args.baud)
            link.send(f"SET STAGE {args.stage}")
            link.wait(lambda m: m.get("event") == "HELLO" and m.get("stage") == args.stage)
            link.send(f"SET PERIOD_US {args.period_us}")
            h = link.wait(lambda m: m.get("event") == "HELLO" and m.get("period_us") == args.period_us and m.get("stage") == args.stage)
            receipt[name+"_identity"] = h
        tag, anchor = links
        if receipt["tag_identity"]["config_id"] != receipt["anchor_identity"]["config_id"]:
            raise RuntimeError("Paired config IDs do not match")
        for link in links:
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
                except Exception as e:
                    state['ack_error'] = repr(e)
            try:
                link.close()
            except Exception as e:
                state['close_error'] = repr(e)
            if not link.thread.is_alive():
                receipt[link.name+"_capture"] = link.stats()
        shutdown_ok = len(links) == 2 and all(
            state['acknowledged'] and 'close_error' not in state for state in shutdown.values())
        receipt['shutdown'] = {'pass': shutdown_ok, 'request': request, 'roles': shutdown}
        if 'error' not in receipt and all(r+"_capture" in receipt for r in ("tag", "anchor")):
            try:
                audited = verify_capture(
                    session=session, count=args.count, stage=args.stage, period_us=args.period_us,
                    identities={r: receipt[r+"_identity"] for r in ("tag", "anchor")},
                    records={link.name: link.capture_records for link in links},
                    stats={link.name: receipt[link.name+"_capture"] for link in links})
                receipt['audit'] = {'pass': audited.passed,
                                    'problems': audited.problems, 'details': audited.details}
                receipt['pass'] = audited.passed and shutdown_ok and not any(link.capture_overflow for link in links)
            except Exception as e:
                receipt['audit'] = {'pass': False, 'problems': ['AUDIT_EXCEPTION'],
                                    'exception': repr(e)}
                receipt['pass'] = False
            if any(link.capture_overflow for link in links):
                receipt['audit']['problems'].append('CAPTURE_AUDIT_MEMORY_LIMIT')
                receipt['pass'] = False
        (args.out/"capture_receipt.json").write_text(json.dumps(receipt, indent=2, ensure_ascii=False), encoding="utf-8")
    print(json.dumps(receipt, indent=2, ensure_ascii=False))
    return 0 if receipt["pass"] else 2

if __name__ == "__main__":
    sys.exit(main())
