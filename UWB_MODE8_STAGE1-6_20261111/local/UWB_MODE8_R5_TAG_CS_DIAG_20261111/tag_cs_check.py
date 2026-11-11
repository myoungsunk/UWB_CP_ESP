"""Install, run, and remove the tag CS feedback diagnostic."""

import argparse
from datetime import datetime
from hashlib import sha256
from itertools import count
import json
from pathlib import Path
import re
import subprocess
import sys
import time

import serial


ROOT = Path(__file__).resolve().parent
LIVE = ROOT.parent / "UWB_MODE8_R5_TAG_AB_LIVE_20261111"
LIVE_IMAGE = LIVE / "dist/TagABLive.ino.bin"
LIVE_HASH = "1af298fb4fea29b391760900cfa1dbc8cc3a99ab83b105d5b4d38c30ec13075d"
DIAGNOSTIC = ROOT / "dist/TagCSCheck.ino.bin"
HELLO = re.compile(rb"TAG_AB_CS_CHECK_V1")
sys.path.insert(0, str(LIVE))
import tag_dev_id as flash


def watch(port, radio, repeats):
    link = serial.Serial(port=None, baudrate=115200, timeout=0.1, write_timeout=1)
    link.dtr = False
    link.rts = False
    link.port = port
    link.open()
    with link:
        time.sleep(0.5)
        link.reset_input_buffer()
        if flash.request(link, b"H", HELLO, 2) is None:
            raise RuntimeError("CS diagnostic firmware did not respond")
        for repeat in count() if repeats == 0 else range(repeats):
            started = time.monotonic()
            for module in radio:
                command = module.encode("ascii")
                pattern = re.compile(rb'\{"event":"CS_CHECK","selected":"' + command + rb'",.*\}')
                line = flash.request(link, command, pattern, 0.4)
                if line is None:
                    raise RuntimeError(f"No CS diagnostic response for module {module}")
                now = datetime.now().strftime("%H:%M:%S")
                print(f"{now}  {json.dumps(json.loads(line), separators=(',', ':'))}", flush=True)
            if repeats == 0 or repeat + 1 < repeats:
                time.sleep(max(0.0, started + 1.0 - time.monotonic()))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("action", choices=("install", "watch", "restore"))
    parser.add_argument("--port", default="COM4")
    parser.add_argument("--radio", choices=("A", "B", "AB"), default="AB")
    parser.add_argument("--count", type=int, default=0, help="0 watches until Ctrl+C")
    parser.add_argument("--baseline", choices=("live", "r5"), default="live")
    args = parser.parse_args()
    if args.count < 0:
        parser.error("--count must be zero or positive")
    try:
        if args.action == "watch":
            watch(args.port, args.radio, args.count)
            return 0
        baseline, digest = ((LIVE_IMAGE, LIVE_HASH) if args.baseline == "live"
                            else (flash.R5, flash.R5_SHA256))
        if sha256(baseline.read_bytes()).hexdigest() != digest:
            raise RuntimeError("Baseline application SHA-256 mismatch")
        if args.action == "install":
            if DIAGNOSTIC.stat().st_size > flash.R5.stat().st_size:
                raise RuntimeError("Diagnostic exceeds the R5 application footprint")
            flash.verify(args.port, baseline)
            flash.write_app(args.port, DIAGNOSTIC)
        else:
            flash.esptool(args.port, "flash-id")
            flash.write_app(args.port, baseline)
    except KeyboardInterrupt:
        return 0 if args.action == "watch" else 130
    except (OSError, RuntimeError, ValueError, subprocess.TimeoutExpired) as exc:
        print(exc, file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
