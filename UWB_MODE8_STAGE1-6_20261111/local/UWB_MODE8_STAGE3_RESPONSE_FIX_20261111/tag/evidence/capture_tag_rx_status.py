"""Run the split Mode 8 collector and read the tag's per-radio evidence."""

import json
from pathlib import Path
import sys

sys.path.insert(0, str(Path(__file__).resolve().parents[3] / "UWB_MODE8_R5_PAIR_RF_20261111"))
import run_split_pair


class DiagnosticLink(run_split_pair.SplitLink):
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
    run_split_pair.SplitLink = DiagnosticLink
    sys.exit(run_split_pair.main())
