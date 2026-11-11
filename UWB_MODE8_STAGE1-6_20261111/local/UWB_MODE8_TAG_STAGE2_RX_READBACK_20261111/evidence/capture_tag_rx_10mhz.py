from pathlib import Path
import sys
from capture_tag_rx_status import DiagnosticLink, run_split_pair
class Cir10Link(DiagnosticLink):
    def send(self, command):
        if command.startswith("SET PERIOD_US "):
            super().send("SET CIR_HZ 10000000")
            self.wait(lambda m: m.get("event") == "HELLO" and m.get("role") == self.name and m.get("stage") == 2)
        super().send(command)
run_split_pair.SplitLink = Cir10Link
if __name__ == "__main__":
    sys.exit(run_split_pair.main())
