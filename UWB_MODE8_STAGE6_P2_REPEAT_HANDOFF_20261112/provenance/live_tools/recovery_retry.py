"""Recover the idle USB link after the recorded fault and retry the same 1000-run once."""
from collections import deque
from datetime import datetime, timezone
import json
from pathlib import Path
import shutil
import sys
import time

import live_pair as live

ROOT = live.OUT
OUT = ROOT / "retry_after_reset"
TAG = "tag"


class CurrentStatusLink(live.SplitLink):
    def send(self, command):
        if command.split()[0] in {"HELLO", "STATUS", "RESET", "SET", "ANT", "BAUD", "START"}:
            with self.condition:
                self.events = deque((event for event in self.events if event.get("event") != "HELLO"),
                                    maxlen=self.events.maxlen)
                return super().send(command)
        return super().send(command)


def recover():
    usb = OUT / "recovery_usb"
    usb.mkdir()
    result = {"started_utc": datetime.now(timezone.utc).isoformat(), "pass": False}
    link = CurrentStatusLink(TAG, live.PORTS[TAG], usb)
    def hello(command="HELLO", **want):
        link.send(command)
        return link.wait(lambda m: m.get("event") == "HELLO" and m.get("role") == TAG
                         and all(m.get(key) == value for key, value in want.items()),
                         timeout=3, check_faults=False)
    try:
        try:
            link.enable_replay()
            link.serial.baudrate = 2000000
            time.sleep(0.2)
            result["before"] = hello(active=False, baud=2000000)
            fragmented = True
        except TimeoutError:
            link.disable_replay()
            link.serial.baudrate = 921600
            time.sleep(0.2)
            result["before"] = hello(active=False, baud=921600)
            fragmented = False

        for radio in range(2):
            link.send(f"RXSTATUS {radio}")
            result[f"radio_{radio}"] = link.wait(
                lambda m: m.get("event") == "RXSTATUS" and m.get("radio") == radio,
                timeout=3, check_faults=False)
        result["reset"] = hello("RESET", active=False)
        if not result["reset"]["hw_ok"] or result["reset"]["fault_latched"]:
            raise RuntimeError("Tag reset did not restore hardware health")
        link.send("SELFTEST")
        result["selftest"] = link.wait(lambda m: m.get("event") == "SELFTEST" and m.get("role") == TAG,
                                       timeout=3, check_faults=False)
        if not (result["selftest"]["pass"] and result["selftest"]["dev_a"] == 0xDECA0130
                and result["selftest"]["dev_b"] == 0xDECA0130):
            raise RuntimeError("Tag SELFTEST failed after reset")
        if fragmented:
            hello("SET PERIOD_US 300000", period_us=300000, baud=2000000, active=False)
            link.send("BAUD 921600")
            time.sleep(0.2)
            link.disable_replay()
            link.serial.baudrate = 921600
            result["baud_restored"] = hello(baud=921600, active=False, period_us=300000)
        result["pass"] = True
    except Exception as exc:
        result["error"] = repr(exc)
    finally:
        try:
            link.close()
        finally:
            result["finished_utc"] = datetime.now(timezone.utc).isoformat()
            (OUT / "recovery.json").write_text(json.dumps(result, indent=2), encoding="utf-8")
    print(json.dumps(result), flush=True)
    return result["pass"]


def main():
    OUT.mkdir()
    if not recover():
        return 2
    for name in ("deployment.json", "hello_selftest.json"):
        shutil.copyfile(ROOT / name, OUT / name)
    live.OUT = OUT
    live.SplitLink = CurrentStatusLink
    try:
        return live.run_capture()
    finally:
        path = OUT / "execution.json"
        if path.is_file():
            receipt = json.loads(path.read_bytes())
            receipt["command"] = [sys.executable, "-B", "-X", "utf8", str(Path(__file__).resolve())]
            for input_path in (Path(__file__), OUT / "recovery.json"):
                receipt["input_sha256"][str(input_path)] = live.sha(input_path)
            path.write_text(json.dumps(receipt, indent=2), encoding="utf-8")


if __name__ == "__main__":
    sys.exit(main())

