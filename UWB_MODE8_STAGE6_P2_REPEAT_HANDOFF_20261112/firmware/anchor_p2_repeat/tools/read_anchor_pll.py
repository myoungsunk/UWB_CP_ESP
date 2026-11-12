"""Read and retain the Mode 8 anchor's idle clock diagnostic over USB."""

import argparse
from datetime import datetime, timezone
import json
from pathlib import Path
from time import monotonic, sleep

import serial

from rfb8 import Decoder


def query(link, decoder, command):
    link.write((command + "\n").encode("ascii"))
    link.flush()
    deadline = monotonic() + 4
    while monotonic() < deadline:
        for packet in decoder.feed(link.read(link.in_waiting or 1)):
            if packet.kind == 1 and packet.meta.get("event") == command:
                return packet.meta
    raise TimeoutError(f"No {command} response on {link.port}")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port", default="COM4")
    parser.add_argument("--out", type=Path, required=True)
    parser.add_argument("--allow-fault", action="store_true")
    args = parser.parse_args()
    link = serial.Serial(port=None, baudrate=921600, timeout=0.1, write_timeout=2)
    link.dtr = link.rts = False
    link.port = args.port
    link.open()
    with link:
        sleep(0.4)
        link.reset_input_buffer()
        decoder = Decoder()
        hello = query(link, decoder, "HELLO")
        if hello.get("role") != "anchor" or hello.get("mode") != 8 or hello.get("active"):
            raise RuntimeError(f"Expected idle Mode 8 anchor, got {hello}")
        pll = query(link, decoder, "PLLSTATUS")
    report = {"utc": datetime.now(timezone.utc).isoformat(), "port": args.port,
              "hello": hello, "pll": pll, "stream": decoder.statistics()}
    with args.out.open("x", encoding="utf-8") as output:
        json.dump(report, output, indent=2)
    print(json.dumps(report, indent=2))
    if pll.get("boot") != hello.get("boot"):
        return 2
    return 0 if args.allow_fault or (hello.get("hw_ok") and pll.get("ready")
                                    and not hello.get("fault_latched")) else 2


if __name__ == "__main__":
    raise SystemExit(main())
