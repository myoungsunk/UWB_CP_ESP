"""Run the split Mode 8 collector and read the tag's per-radio evidence."""

import argparse
import json
from math import isfinite
from pathlib import Path
import sys

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))
import run_pair
sys.path.insert(0, str(Path(__file__).resolve().parents[3] / "UWB_MODE8_R5_PAIR_RF_20261111"))
import run_split_pair



# The SSH bridge receives ASCII command lines from this collector. A NUL
# prefix marks a bridge control line and never reaches the ESP32.
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


run_split_pair.PiSerial = SwitchablePiSerial


class DiagnosticLink(run_split_pair.SplitLink):
    tag_delay = None
    out = None
    stage = 6

    def send(self, command):
        if self.name == "tag" and self.tag_delay is not None and command == "SELFTEST":
            for radio, (ticks, sample_sd) in enumerate(self.tag_delay):
                super().send(f"ANT {radio} {ticks} {ticks} 0 {sample_sd:.8f}")
                hello = self.wait(lambda m: m.get("event") == "HELLO" and m.get("role") == "tag")
                if (hello.get("firmware") != "D8_R5_TAG_STAGE6_P2POLL_20261111"
                        or hello.get("hw_ok") is not True or hello.get("active") is not False):
                    raise RuntimeError(f"Tag antenna delay preflight failed: {hello}")
        return super().send(command)

    def wait(self, *args, **kwargs):
        message = super().wait(*args, **kwargs)
        if self.name == "tag" and self.tag_delay is not None and message.get("event") == "SELFTEST":
            for letter, (ticks, _) in zip("ab", self.tag_delay, strict=True):
                if (message.get(f"ant_{letter}_tx"), message.get(f"ant_{letter}_rx")) != (ticks, ticks):
                    raise RuntimeError(f"Tag antenna delay readback mismatch: {message}")
        return message

    def close(self):
        try:
            self.send("STATUS")
            status = self.wait(lambda m: m.get("event") == "HELLO" and m.get("role") == self.name
                               and m.get("active") is False and m.get("stage") == self.stage,
                               timeout=4, check_faults=False)
            with (self.out / f"{self.name}_post_status.json").open("x", encoding="utf-8") as file:
                json.dump(status, file, indent=2)
            if self.name == "tag":
                responses = []
                for radio in range(2):
                    self.send(f"RXSTATUS {radio}")
                    responses.append(self.wait(
                        lambda message: message.get("event") == "RXSTATUS" and message.get("radio") == radio,
                        timeout=3, check_faults=False,
                    ))
                with (self.out / "tag_rx_status.json").open("x", encoding="utf-8") as file:
                    json.dump(responses, file, indent=2)
        finally:
            try:
                if self.serial.baudrate == 2000000:
                    # Finish capture at the verified STOP and status boundary.
                    # Restoration can emit a HELLO at the other baud rate.
                    self.closing.set()
                    self.thread.join(timeout=3)
                    if self.thread.is_alive():
                        raise TimeoutError("Reader still running before baud restoration")
                    self.send("BAUD 921600")
            finally:
                super().close()


if __name__ == "__main__":
    options = argparse.ArgumentParser(add_help=False)
    options.add_argument("--tag-delay-receipt", type=Path)
    options.add_argument("--out", type=Path, required=True)
    options.add_argument("--stage", type=int, choices=(6,), default=6)
    args, remaining = options.parse_known_args()
    DiagnosticLink.out = args.out
    DiagnosticLink.stage = args.stage
    if args.tag_delay_receipt is not None:
        settings = json.loads(args.tag_delay_receipt.read_bytes())
        offsets = [(settings["modules"][r]["tag_delay_ticks"],
                    settings["modules"][r]["baseline_sample_sd_m"]) for r in "AB"]
        if any(type(ticks) is not int or not 0 < ticks < 65535 or not isfinite(sd) or not 0 < sd <= 100
               for ticks, sd in offsets):
            options.error("invalid trial antenna delay or baseline repeatability")
        DiagnosticLink.tag_delay = offsets
    sys.argv[1:] = [*remaining, "--tag", run_split_pair.TAG_PORT, "--stage", str(args.stage), "--out", str(args.out)]
    run_pair.Link = DiagnosticLink
    sys.exit(run_pair.main())
