"""Install, monitor, or remove the read-only DW1000 ID diagnostic on the R5 tag."""

import argparse
from datetime import datetime
from hashlib import sha256
from pathlib import Path
import re
import subprocess
import sys
import time

import serial


ROOT = Path(__file__).resolve().parent
R5 = ROOT.parent / ("UWB_MODE8_R5_TARGET_20261111/UWB_DUAL_DW1000_MODE8_R5_20261111"
                    "/dist/tag/TAG_DUAL_DS_CIR.ino.bin")
R5_SHA256 = "3b7918e7d9174d15369d50bb01660462a552dd42a3ffc8912ba3f1259ddead1a"
DIAGNOSTIC = ROOT / "dist/TagABLive.ino.bin"
ESPTOOL = Path.home() / "AppData/Local/Arduino15/packages/esp32/tools/esptool_py/5.1.0/esptool.exe"
TAG_MAC = "28:56:2f:73:06:54"
APP_OFFSET = "0x10000"
HELLO = re.compile(rb"TAG_AB_DEV_ID_LIVE_V1")


def esptool(port, *arguments):
    command = [str(ESPTOOL), "--chip", "esp32", "--port", port, "--baud", "460800"]
    result = subprocess.run(command + list(map(str, arguments)), stdout=subprocess.PIPE,
                            stderr=subprocess.STDOUT, text=True, encoding="utf-8",
                            errors="replace", timeout=45)
    print(result.stdout, end="", flush=True)
    if result.returncode:
        raise RuntimeError(f"esptool {arguments[0]} failed (exit {result.returncode})")
    if not re.search(rf"^MAC:[ \t]+{TAG_MAC}[ \t]*$", result.stdout, re.M | re.I):
        raise RuntimeError(f"Expected tag ESP32 {TAG_MAC} on {port}")
    return result.stdout


def verify(port, image):
    output = esptool(port, "verify-flash", APP_OFFSET, image)
    if "Verification successful (digest matched)." not in output:
        raise RuntimeError("Flash verification did not report a digest match")


def write_app(port, image):
    esptool(port, "write-flash", "-z", "--flash-mode", "keep", "--flash-freq", "keep",
            "--flash-size", "keep", APP_OFFSET, image)
    verify(port, image)


def install(port):
    if DIAGNOSTIC.stat().st_size > R5.stat().st_size:
        raise RuntimeError("Diagnostic extends beyond the R5 application")
    verify(port, R5)
    write_app(port, DIAGNOSTIC)


def restore(port):
    esptool(port, "flash-id")
    write_app(port, R5)


def request(link, command, pattern, timeout):
    link.write(command + b"\n")
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        line = link.readline().strip()
        if pattern.fullmatch(line):
            return line.decode("ascii")
    return None


def watch(port, radio):
    command = {"A": b"A", "B": b"B", "AB": b"I"}[radio]
    reply = re.compile(rb"DEV_ID" + b"".join(
        rb" " + module.encode("ascii") + rb"=0x[0-9A-F]{8}" for module in radio
    ))
    link = serial.Serial(port=None, baudrate=115200, timeout=0.1, write_timeout=1)
    link.dtr = False
    link.rts = False
    link.port = port
    link.open()
    with link:
        time.sleep(0.5)
        link.reset_input_buffer()
        if request(link, b"H", HELLO, 2) is None:
            raise RuntimeError("Diagnostic firmware did not respond. Check the port and run install.")
        while True:
            started = time.monotonic()
            now = datetime.now().strftime("%H:%M:%S")
            line = request(link, command, reply, 0.6)
            value = line.removeprefix("DEV_ID ") if line else f"{radio}: no response"
            print(f"{now}  {value}", flush=True)
            time.sleep(max(0.0, started + 1.0 - time.monotonic()))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("action", choices=("install", "watch", "restore"))
    parser.add_argument("--port", default="COM4")
    parser.add_argument("--radio", choices=("A", "B", "AB"), default="AB")
    args = parser.parse_args()
    try:
        if args.action == "watch":
            watch(args.port, args.radio)
        else:
            if sha256(R5.read_bytes()).hexdigest() != R5_SHA256:
                raise RuntimeError("R5 tag image SHA-256 mismatch")
            {"install": install, "restore": restore}[args.action](args.port)
    except KeyboardInterrupt:
        if args.action != "watch":
            print("Flash operation interrupted. Run restore with the tag connected.", file=sys.stderr)
            return 130
    except (OSError, RuntimeError, subprocess.TimeoutExpired) as exc:
        print(exc, file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
