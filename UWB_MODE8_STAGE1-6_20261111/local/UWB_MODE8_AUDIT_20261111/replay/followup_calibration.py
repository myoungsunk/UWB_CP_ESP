"""Reproduce a P1 calibration label sourced from a mismatched anchor record."""

import csv
import json
from pathlib import Path
import subprocess
import sys
from tempfile import TemporaryDirectory


PACKAGE = Path(__file__).resolve().parents[1] / "UWB_DUAL_DW1000_MODE8_20261111"
sys.path.insert(0, str(PACKAGE / "tools"))
from rfb8 import encode


def main():
    fixture = PACKAGE / "evidence" / "host"
    tag = json.loads((fixture / "synthetic_tag_record.json").read_bytes())
    tag["ranges"][0]["cal_id"] = 11
    raw = (fixture / "SYNTHETIC_CIR_i16.raw").read_bytes()
    anchor = {
        "schema": "dual8.record.v1", "mode": 8, "role": "anchor",
        "session": tag["session"], "txn": tag["txn"], "boot": tag["peer_boot"],
        "peer_boot": tag["boot"] + 1, "config": tag["config"] + 1,
        "tx": [{}, {}, {"ok": False}], "calibration": [{"id": 22}],
    }
    with TemporaryDirectory(prefix="mode8-cal-audit-") as tmp:
        p = Path(tmp)
        (p / "tag.rfb8").write_bytes(encode(tag, raw))
        (p / "anchor.rfb8").write_bytes(encode(anchor))
        command = [sys.executable, "-B", str(PACKAGE / "tools" / "analyze.py"),
                   str(p / "tag.rfb8"), "--anchor", str(p / "anchor.rfb8"),
                   "--out", str(p / "out")]
        subprocess.run(command, check=True, capture_output=True, text=True)
        with (p / "out" / "observations.csv").open(newline="") as file:
            row = next(csv.DictReader(file))
        result = {
            "p1_a_reported_both_endpoints_calibrated": row["both_endpoints_calibrated"],
            "anchor_config_matches": anchor["config"] == tag["config"],
            "anchor_peer_boot_matches": anchor["peer_boot"] == tag["boot"],
            "p1_a_range_m": row["range_m"],
        }
        assert result["p1_a_reported_both_endpoints_calibrated"] == "True", result
        print(json.dumps(result, indent=2))


if __name__ == "__main__":
    main()
