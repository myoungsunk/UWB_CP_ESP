import argparse
from datetime import datetime
import json
from pathlib import Path
import re
import sys
import time

import serial


ROOT = Path(__file__).resolve().parent
FIX = ROOT.parent / "UWB_MODE8_R5_TAG_CS_FIX_20261111"
LIVE = ROOT.parent / "UWB_MODE8_R5_TAG_AB_LIVE_20261111"
sys.path[:0] = [str(FIX / "tools"), str(LIVE)]
from rfb8 import Decoder
from tag_dev_id import request


def check_fixed(link, raw, out):
    decoder = Decoder()
    for event in ("HELLO", "SELFTEST"):
        link.write(event.encode("ascii") + b"\n")
        deadline = time.monotonic() + 6
        match = None
        while time.monotonic() < deadline and match is None:
            data = link.read(link.in_waiting or 1)
            raw.write(data)
            for packet in decoder.feed(data):
                if packet.meta.get("event") == event and packet.meta.get("role") == "tag":
                    match = packet.meta
        if match is None:
            raise RuntimeError(f"No valid RFB8 tag {event} response")
        out[event] = match
    out["dev_id_hex"] = {module: f"0x{out['SELFTEST']['dev_' + module.lower()]:08X}"
                         for module in ("A", "B")}
    out["both_ids_expected"] = all(value == "0xDECA0130" for value in out["dev_id_hex"].values())


def check_live(link, out):
    if request(link, b"H", re.compile(rb"TAG_AB_DEV_ID_LIVE_V1"), 3) is None:
        raise RuntimeError("Raw DEV_ID diagnostic did not respond")
    out["spi_hz"] = 1000000
    out["readings"] = {module: [] for module in ("A", "B")}
    for _ in range(5):
        for module in ("A", "B"):
            command = module.encode("ascii")
            pattern = re.compile(rb"DEV_ID " + command + rb"=0x[0-9A-F]{8}")
            line = request(link, command, pattern, 0.6)
            if line is None:
                raise RuntimeError(f"No raw DEV_ID response for {module}")
            out["readings"][module].append(line.partition("=")[2])
        time.sleep(0.1)
    out["ids_expected"] = {module: all(value == "0xDECA0130" for value in readings)
                           for module, readings in out["readings"].items()}


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("app", choices=("live", "fixed"))
    parser.add_argument("label")
    args = parser.parse_args()
    baud = 115200 if args.app == "live" else 921600
    out = {"started": datetime.now().astimezone().isoformat(), "port": "COM4",
           "app": args.app, "baud": baud}
    link = serial.Serial(port=None, baudrate=baud, timeout=0.1, write_timeout=2)
    link.dtr = False
    link.rts = False
    link.port = "COM4"
    try:
        link.open()
        with link:
            time.sleep(0.6)
            link.reset_input_buffer()
            if args.app == "fixed":
                with (ROOT / f"{args.label}.rfb8").open("xb") as raw:
                    check_fixed(link, raw, out)
            else:
                check_live(link, out)
    except (OSError, RuntimeError) as exc:
        out["error"] = str(exc)
    out["finished"] = datetime.now().astimezone().isoformat()
    with (ROOT / f"{args.label}.json").open("x", encoding="utf-8") as receipt:
        json.dump(out, receipt, indent=2)
    print(json.dumps(out))
    return 2 if "error" in out else 0


if __name__ == "__main__":
    sys.exit(main())
