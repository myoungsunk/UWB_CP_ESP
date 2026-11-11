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


class DiagnosticLink(run_split_pair.SplitLink):
    tag_delay = None

    def send(self, command):
        if self.name == "tag" and self.tag_delay is not None and command == "SELFTEST":
            for radio, (ticks, sample_sd) in enumerate(self.tag_delay):
                super().send(f"ANT {radio} {ticks} {ticks} 0 {sample_sd:.8f}")
                hello = self.wait(lambda m: m.get("event") == "HELLO" and m.get("role") == "tag")
                if (hello.get("firmware") != "D8_R5_TAG_STAGE5_P2_20261111"
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
            if self.name == "tag":
                responses = []
                for radio in range(2):
                    self.send(f"RXSTATUS {radio}")
                    responses.append(self.wait(
                        lambda message: message.get("event") == "RXSTATUS" and message.get("radio") == radio,
                        timeout=3, check_faults=False,
                    ))
                with (Path(sys.argv[sys.argv.index("--out") + 1]) / "tag_rx_status.json").open("x", encoding="utf-8") as out:
                    json.dump(responses, out, indent=2)
        finally:
            super().close()


if __name__ == "__main__":
    options = argparse.ArgumentParser(add_help=False)
    options.add_argument("--tag-delay-receipt", type=Path)
    args, remaining = options.parse_known_args()
    if args.tag_delay_receipt is not None:
        settings = json.loads(args.tag_delay_receipt.read_bytes())
        offsets = [(settings["modules"][r]["tag_delay_ticks"],
                    settings["modules"][r]["baseline_sample_sd_m"]) for r in "AB"]
        if any(type(ticks) is not int or not 0 < ticks < 65535 or not isfinite(sd) or not 0 < sd <= 100
               for ticks, sd in offsets):
            options.error("invalid trial antenna delay or baseline repeatability")
        DiagnosticLink.tag_delay = offsets
    sys.argv[1:] = remaining
    run_split_pair.SplitLink = DiagnosticLink
    sys.exit(run_split_pair.main())
