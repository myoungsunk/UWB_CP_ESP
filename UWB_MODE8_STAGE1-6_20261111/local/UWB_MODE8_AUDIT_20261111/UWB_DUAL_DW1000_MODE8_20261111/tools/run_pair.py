#!/usr/bin/env python3
"""USB serial bring-up/capture for two Mode-8 boards. Does NOT flash firmware."""
from __future__ import annotations
import argparse
from collections import deque
import json
from pathlib import Path
import secrets
import threading
import time
import sys
from rfb8 import Decoder

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
                for packet in self.decoder.feed(data):
                    m = packet.meta
                    self.meta.write(json.dumps(m, ensure_ascii=False)+"\n")
                    with self.condition:
                        if packet.kind == 2:
                            self.records += 1
                            self.complete += bool(m.get("complete"))
                        else:
                            self.events.append(m)
                        self.condition.notify_all()
        except Exception as e:
            self.exception = repr(e)
            with self.condition:
                self.condition.notify_all()

    def send(self, command: str) -> None:
        if "\n" in command or len(command) > 180:
            raise ValueError("Invalid serial command")
        self.serial.write((command+"\n").encode("ascii"))
        self.serial.flush()

    def wait(self, predicate, timeout: float = 8) -> dict:
        end = time.monotonic()+timeout
        with self.condition:
            while True:
                if self.exception:
                    raise RuntimeError(self.exception)
                for m in list(self.events):
                    if m.get("event") in ("ERROR", "HARDWARE_FAULT_LATCHED"):
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
        self.serial.close()
        self.raw.flush(); self.raw.close()
        self.meta.flush(); self.meta.close()

    def stats(self) -> dict:
        return {"port": self.device, "records": self.records, "complete": self.complete,
                "reader_error": self.exception, "decoder": self.decoder.statistics()}


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
    if not 1 <= args.count <= 1000000 or not 100000 <= args.period_us <= 5000000:
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
        time.sleep(1)
        receipt["pass"] = all(x.records == args.count and x.complete == args.count and
                              x.decoder.crc_errors == 0 and x.decoder.schema_errors == 0 for x in links)
    except Exception as e:
        receipt["error"] = str(e)
    finally:
        for link in links:
            try:
                link.send("STOP")
            except Exception:
                pass
        time.sleep(0.5)
        for link in links:
            link.close()
            receipt[link.name+"_capture"] = link.stats()
        (args.out/"capture_receipt.json").write_text(json.dumps(receipt, indent=2, ensure_ascii=False), encoding="utf-8")
    print(json.dumps(receipt, indent=2, ensure_ascii=False))
    return 0 if receipt["pass"] else 2

if __name__ == "__main__":
    sys.exit(main())
