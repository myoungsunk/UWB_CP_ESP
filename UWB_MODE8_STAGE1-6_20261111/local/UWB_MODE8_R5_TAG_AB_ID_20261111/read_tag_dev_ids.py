from datetime import datetime
from hashlib import sha256
import json
from pathlib import Path
import re
import subprocess
import sys
import time

import serial


ROOT = Path(__file__).resolve().parent
R5 = ROOT.parent / "UWB_MODE8_R5_TARGET_20261111/UWB_DUAL_DW1000_MODE8_R5_20261111"
ORIGINAL = R5 / "dist/tag/TAG_DUAL_DS_CIR.ino.bin"
DIAGNOSTIC = ROOT / "dist/TagABDevId.ino.bin"
ESPTOOL = Path.home() / "AppData/Local/Arduino15/packages/esp32/tools/esptool_py/5.1.0/esptool.exe"
MAC = "28:56:2f:73:06:54"
ORIGINAL_HASH = "3b7918e7d9174d15369d50bb01660462a552dd42a3ffc8912ba3f1259ddead1a"
EXPECTED_ID = "0xDECA0130"
OFFSET = "0x10000"
PORT = sys.argv[1]
sys.path.insert(0, str(R5 / "tools"))
from rfb8 import Decoder


def flash_command(label, *args, connected=True):
    command = [str(ESPTOOL), "--chip", "esp32"]
    if connected:
        command += ["--port", PORT, "--baud", "460800"]
    command += list(map(str, args))
    result = subprocess.run(command, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                            text=True, encoding="utf-8", errors="replace", timeout=45)
    (ROOT / f"{label}.log").write_text(result.stdout, encoding="utf-8")
    receipt["commands"][label] = {"command": command, "exit_code": result.returncode}
    if result.returncode:
        raise RuntimeError(f"{label} failed with exit code {result.returncode}")
    if connected and not re.search(rf"^MAC:\s+{MAC}$", result.stdout, re.M | re.I):
        raise RuntimeError(f"{label} did not identify the expected ESP32 MAC")
    return result.stdout


def verify(label, binary):
    output = flash_command(label, "verify-flash", OFFSET, binary)
    if "Verification successful (digest matched)." not in output:
        raise RuntimeError(f"{label} did not confirm a digest match")


def write(label, binary):
    flash_command(label, "write-flash", "-z", "--flash-mode", "keep",
                  "--flash-freq", "keep", "--flash-size", "keep", OFFSET, binary)


def serial_link(baud):
    link = serial.Serial(port=None, baudrate=baud, timeout=0.15, write_timeout=2)
    link.dtr = False
    link.rts = False
    link.port = PORT
    link.open()
    return link


def read_ids():
    with serial_link(115200) as link, (ROOT / "serial_dev_id.log").open("wb") as raw:
        time.sleep(1)
        link.reset_input_buffer()
        link.write(b"D\n")
        deadline = time.monotonic() + 6
        started = False
        while time.monotonic() < deadline:
            line = link.readline()
            raw.write(line)
            line = line.strip()
            if line == b"DW1000_AB_DIAG_BEGIN":
                started = True
            elif started and line.startswith(b"{"):
                receipt["readings"].append(json.loads(line))
            elif started and line == b"DW1000_AB_DIAG_END":
                break
        else:
            raise RuntimeError("Missing diagnostic completion marker")
    expected_keys = {
        (radio, phase, hz)
        for radio in "AB"
        for phase in ("before_reset", "after_reset")
        for hz in (1000000, 2000000)
    }
    for row in receipt["readings"]:
        key = row["radio"], row["phase"], row["spi_hz"]
        if (key not in expected_keys or row["event"] != "DW1000_AB_DEV_ID"
                or len(row["ids"]) != 5
                or any(not re.fullmatch(r"0x[0-9A-F]{8}", value) for value in row["ids"])):
            raise RuntimeError(f"Malformed or duplicate diagnostic row: {row}")
        expected_keys.remove(key)
    if expected_keys:
        raise RuntimeError(f"Missing rows: {sorted(expected_keys)}")
    receipt["ids_match"] = {
        radio: all(value == EXPECTED_ID for row in receipt["readings"] if row["radio"] == radio
                   for value in row["ids"])
        for radio in "AB"
    }
    receipt["diagnostic_complete"] = True


def restored_hello():
    decoder = Decoder()
    with serial_link(921600) as link, (ROOT / "restored_hello.rfb8").open("wb") as raw:
        time.sleep(1)
        link.write(b"HELLO\n")
        deadline = time.monotonic() + 5
        while time.monotonic() < deadline:
            chunk = link.read(link.in_waiting or 1)
            raw.write(chunk)
            for packet in decoder.feed(chunk):
                meta = packet.meta
                if (meta.get("event"), meta.get("role"), meta.get("mode")) == ("HELLO", "tag", 8):
                    return meta
    raise RuntimeError("Restored tag did not answer HELLO")


receipt = {
    "started": datetime.now().astimezone().isoformat(), "port": PORT, "esp32_mac": MAC,
    "spi_pins": {"sck": 18, "miso": 19, "mosi": 23, "A": {"cs": 21, "rst": 17},
                 "B": {"cs": 25, "rst": 27}},
    "expected_dev_id": EXPECTED_ID, "app_offset": OFFSET, "commands": {},
    "readings": [], "diagnostic_complete": False, "restore_verified": False, "errors": [],
}
write_attempted = False
try:
    receipt["original_sha256"] = sha256(ORIGINAL.read_bytes()).hexdigest()
    if receipt["original_sha256"] != ORIGINAL_HASH:
        raise RuntimeError("Original app SHA-256 differs from verified R5 tag")
    receipt["diagnostic_sha256"] = sha256(DIAGNOSTIC.read_bytes()).hexdigest()
    if DIAGNOSTIC.stat().st_size > ORIGINAL.stat().st_size:
        raise RuntimeError("Diagnostic app would extend beyond the verified R5 tag image")
    flash_command("diagnostic_image_info", "image-info", DIAGNOSTIC, connected=False)
    verify("r5_before_diagnostic", ORIGINAL)
    write_attempted = True
    write("temporary_diagnostic_write", DIAGNOSTIC)
    verify("temporary_diagnostic_verify", DIAGNOSTIC)
    read_ids()
except Exception as exc:
    receipt["errors"].append(f"Diagnostic: {exc}")
finally:
    if write_attempted:
        for attempt in (1, 2):
            try:
                flash_command(f"restore_{attempt}_identity", "flash-id")
                write(f"restore_{attempt}_write", ORIGINAL)
                verify(f"restore_{attempt}_verify", ORIGINAL)
                receipt["restore_verified"] = True
                break
            except Exception as exc:
                receipt["errors"].append(f"Restore attempt {attempt}: {exc}")
        if receipt["restore_verified"]:
            try:
                receipt["restored_hello"] = restored_hello()
            except Exception as exc:
                receipt["errors"].append(f"Restored HELLO: {exc}")
    receipt["finished"] = datetime.now().astimezone().isoformat()
    (ROOT / "tag_ab_dev_id_receipt.json").write_text(
        json.dumps(receipt, indent=2) + "\n", encoding="utf-8")

summary = {key: receipt.get(key) for key in (
    "readings", "ids_match", "diagnostic_complete", "restore_verified", "restored_hello", "errors"
)}
print(json.dumps(summary))
sys.exit(0 if receipt["diagnostic_complete"] and receipt["restore_verified"]
         and receipt.get("restored_hello") else 2)
