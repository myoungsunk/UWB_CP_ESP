"""Period sweep avoids promoting a short nominal RF schedule to field PASS."""
import hashlib
import json
from pathlib import Path
import sys
import tempfile
import unittest
from unittest.mock import patch
sys.path.insert(0,str(Path(__file__).resolve().parents[1]/"host"))
from ops_period_sweep import command_for,inspect_trial,main,PERIODS

class PeriodTests(unittest.TestCase):
    def test_order_and_bounds(self):
        self.assertEqual(PERIODS,(200000,195000,190000))
        for period in PERIODS:
            c=command_for("COM4","COM6",Path("trial"),20,period)
            self.assertEqual(c[c.index("--period-us")+1],str(period))
        for period in (189000,180000,150000,100000,201000):
            with self.assertRaises(ValueError):
                command_for("COM4","COM6",Path("trial"),20,period)
    def test_crc_valid_not_enough_without_both_polarization_grids(self):
        with tempfile.TemporaryDirectory() as d:
            p=Path(d)
            (p/"tag.rfb8").write_bytes(b"original_tag")
            (p/"anchor.rfb8").write_bytes(b"original_anchor")
            r={"pass":True,"stage":6,"period_us":195000,"baud":2000000,
               "requested_count":2,"stage6":{"common_axis_and_power":{"P1":2,"P2":1}}}
            (p/"capture_receipt.json").write_text(json.dumps(r))
            with self.assertRaises(RuntimeError):inspect_trial(p,2,195000)
    def test_rejects_replaced_raw_after_complete_trial(self):
        with tempfile.TemporaryDirectory() as d:
            path = Path(d)
            hashes = {}
            for name in ("anchor.rfb8", "tag.rfb8"):
                (path / name).write_bytes(b"original")
                hashes[name] = hashlib.sha256(b"original").hexdigest()
            receipt = {
                "pass": True, "stage": 6, "period_us": 195000, "baud": 2000000,
                "requested_count": 2, "file_sha256": hashes,
                "stage6": {"common_axis_and_power": {"P1": 2, "P2": 2}},
            }
            (path / "capture_receipt.json").write_text(json.dumps(receipt), encoding="utf-8")
            self.assertEqual(inspect_trial(path, 2, 195000)["count"], 2)
            (path / "tag.rfb8").write_bytes(b"replaced")
            with self.assertRaisesRegex(RuntimeError, "SHA-256 mismatch"):
                inspect_trial(path, 2, 195000)

    def test_dry_run_no_rf(self):
        with tempfile.TemporaryDirectory() as d:
            output=Path(d)/"data"
            args=["ops_period_sweep.py","--anchor","COM4","--tag","COM6",
                  "--out",str(output),"--anchor-initial-baud","2000000",
                  "--calibration-profile","bench.json"]
            with patch.object(sys,"argv",args),patch("ops_period_sweep.subprocess.run") as run, \
                 patch("builtins.print") as output_log:
                self.assertEqual(main(),0);run.assert_not_called()
            commands = json.loads(output_log.call_args.args[0])["commands"]
            for index, command in enumerate(commands):
                self.assertEqual(command[command.index("--anchor-initial-baud") + 1],
                                 "2000000" if index == 0 else "921600")
                self.assertEqual(command[command.index("--calibration-profile") + 1], "bench.json")
            self.assertFalse(output.exists())

if __name__=="__main__":unittest.main()
