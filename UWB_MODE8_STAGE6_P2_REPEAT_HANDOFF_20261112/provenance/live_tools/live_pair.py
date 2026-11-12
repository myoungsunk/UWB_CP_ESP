"""Verify the deployed boards and run the pinned Stage 6 collector on the split USB links."""
import argparse
from datetime import datetime, timezone
import json
from pathlib import Path
import secrets
import sys
import time

import serial
from deploy import ARTIFACTS, OUT, PACKAGE, PORTS, sha, write_json

sys.path.insert(0, str(PACKAGE / "tag" / "tools"))
import run_pair

sys.path.append(str(ARTIFACTS / "UWB_MODE8_R5_PAIR_RF_20261111"))
import run_split_pair

FIRMWARE = {
    role: f"D8_{role.upper()}_STAGE6_RELIABILITY_20261112" for role in PORTS
}

# Reuse the established Pi SSH serial bridge with its switchable baud control.
run_split_pair.BRIDGE = run_split_pair.BRIDGE.replace(
    '    while True:\n', '    commands = bytearray()\n    while True:\n', 1
).replace(
    '            link.write(data)\n            link.flush()',
    """            commands.extend(data)
            while b"\\n" in commands:
                line, _, rest = commands.partition(b"\\n")
                commands[:] = rest
                if line.startswith(b"\\x00"):
                    if line not in (b"\\x00BAUD 921600", b"\\x00BAUD 2000000"):
                        raise ValueError("Invalid bridge baud control")
                    link.baudrate = int(line.split()[1])
                else:
                    link.write(line + b"\\n")
                    link.flush()""",
)


class SwitchablePiSerial(run_split_pair.PiSerial):
    @property
    def baudrate(self):
        return self._baudrate

    @baudrate.setter
    def baudrate(self, value):
        if value not in (921600, 2000000):
            raise ValueError("Unsupported Pi serial baud")
        if getattr(self, "process", None) is not None:
            data = f"\0BAUD {value}\n".encode("ascii")
            if self.write(data) != len(data):
                raise OSError("Incomplete Pi baud control write")
            self.flush()
        self._baudrate = value


class SplitLink(run_pair.Link):
    def __init__(self, name, device, directory):
        if name == "anchor":
            super().__init__(name, device, directory)
            return
        native = serial.Serial
        try:
            serial.Serial = lambda **kwargs: SwitchablePiSerial(
                directory / "pi_ssh_serial.log", **kwargs)
            super().__init__(name, device, directory)
        finally:
            serial.Serial = native

    def wait(self, *args, **kwargs):
        result = super().wait(*args, **kwargs)
        if result.get("event") == "HELLO" and (
                result.get("role"), result.get("firmware")) != (self.name, FIRMWARE[self.name]):
            raise RuntimeError(f"{self.name}: deployed firmware identity mismatch")
        return result


def smoke():
    directory = OUT / "usb"
    directory.mkdir()
    result = {"utc": datetime.now(timezone.utc).isoformat(), "roles": {}, "pass": False}
    for role, device in PORTS.items():
        data = result["roles"][role] = {"port": device, "pass": False}
        link = None
        try:
            link = SplitLink(role, device, directory)
            time.sleep(0.5)
            request = secrets.randbits(32) or 1
            link.send(f"STOP {request}")
            data["stop"] = link.wait(lambda m: m.get("event") == "STOPPED"
                                     and m.get("role") == role and m.get("stop_request") == request)
            link.send("HELLO")
            hello = data["hello"] = link.wait(lambda m: m.get("event") == "HELLO")
            link.send("SELFTEST")
            test = data["selftest"] = link.wait(lambda m: m.get("event") == "SELFTEST")
            data["pass"] = (hello.get("mode") == 8 and hello.get("wire") == "RFB8"
                            and hello.get("record_replay") == 1 and hello.get("hw_ok") is True
                            and hello.get("fault_latched") is False and hello.get("active") is False
                            and test.get("role") == role and test.get("pass") is True
                            and test.get("dev_a") == 0xDECA0130
                            and (role == "anchor" or test.get("dev_b") == 0xDECA0130))
        except Exception as exc:
            data["error"] = repr(exc)
        finally:
            if link is not None:
                link.close()
    result["pass"] = all(entry["pass"] for entry in result["roles"].values())
    write_json("hello_selftest.json", result)
    print(json.dumps(result, indent=2), flush=True)
    return 0 if result["pass"] else 2


def run_capture():
    deployment = json.loads((OUT / "deployment.json").read_bytes())
    usb = json.loads((OUT / "hello_selftest.json").read_bytes())
    if not usb["pass"] or not all(d.get("flash_readback_verified")
                                  for d in deployment["roles"].values()):
        raise RuntimeError("Deployment and USB checks must pass before RF")
    output = OUT / "capture"
    calibration = PACKAGE / "calibration_stage6_temporary.json"
    governance = Path.home() / ".codex" / "research-governance"
    arguments = ["--anchor", PORTS["anchor"], "--tag", PORTS["tag"], "--stage", "6",
                 "--count", "1000", "--period-us", "200000", "--baud", "2000000",
                 "--rf-remeasure-limit", "0", "--calibration-profile", str(calibration),
                 "--out", str(output)]
    old = ARTIFACTS / "UWB_MODE8_STAGE6_CONTINUOUS_RX_FEC_PACKED_20261111" / "evidence"
    old /= "stage6_200ms_2mbps_1000_20261111_220602_883.contract.json"
    files = [old, OUT / "deployment.json", OUT / "hello_selftest.json", calibration,
             Path(__file__), Path(run_pair.__file__), Path(run_split_pair.__file__),
             governance / "COMMON_CONTRACT.md", governance / "ROLE_IMPLEMENTER.md"]
    receipt = {
        "question_id": "MODE8-STAGE6-200MS-2MBPS-1000",
        "role": "IMPLEMENTER", "started_utc": datetime.now(timezone.utc).isoformat(),
        "command": [sys.executable, "-B", "-X", "utf8", str(Path(__file__).resolve()), "run"],
        "collector_arguments": arguments,
        "input_sha256": {str(file): sha(file) for file in files},
        "acceptance": "One session. Both roles 1000 ordered records, P1/P2 grids 1000 each with matching endpoint evidence, zero final RFB8 CRC/gaps/missing/restoration failures, zero period misses, STOP confirmed. Report initial RF and USB faults separately.",
        "absolute_accuracy_validated": False,
    }
    write_json("execution.json", receipt)
    sys.argv = [str(run_pair.__file__), *arguments]
    run_pair.Link = SplitLink
    code = 2
    try:
        code = run_pair.main()
        return code
    finally:
        receipt["finished_utc"] = datetime.now(timezone.utc).isoformat()
        receipt["exit_code"] = code
        capture_receipt = output / "capture_receipt.json"
        if capture_receipt.is_file():
            receipt["capture_receipt_sha256"] = sha(capture_receipt)
        write_json("execution.json", receipt)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("phase", choices=("smoke", "run"))
    args = parser.parse_args()
    sys.exit(smoke() if args.phase == "smoke" else run_capture())

