"""Soak suite: no auto-retry after an ambiguous started RF epoch."""
from pathlib import Path
import tempfile
import json
import sys
import unittest
from unittest.mock import patch
sys.path.insert(0,str(Path(__file__).resolve().parents[1]/"host"))
from ops_soak_suite import run_plan,requirement_bytes,main

class SoakTests(unittest.TestCase):
    def test_full_batch_dry_run_and_valid_disk_headroom(self):
        plan=run_plan("COM4","COM7",Path("trial"),1000,18)
        self.assertFalse(plan["rf_gap_free_between_rounds"])
        self.assertEqual(len(plan["entries"]),18)
        self.assertGreaterEqual(requirement_bytes(1000),268_435_456)
        with self.assertRaises(ValueError):run_plan("COM4","COM4",Path("trial"),1,1)
        with self.assertRaises(ValueError):run_plan("COM4","COM7",Path("trial"),1,501)
    def test_dry_run_never_calls_hardware(self):
        with tempfile.TemporaryDirectory() as d:
            path=Path(d)/"run"
            argv=["ops_soak_suite.py","--anchor","COM4","--tag","COM7","--out",str(path)]
            with patch.object(sys,"argv",argv),patch("ops_soak_suite.subprocess.run") as run:
                self.assertEqual(main(),0);run.assert_not_called()
            self.assertFalse(path.exists())
    def test_bad_raw_proof_stops_after_requested_epoch(self):
        with tempfile.TemporaryDirectory() as d:
            path = Path(d) / "run"
            argv = ["ops_soak_suite.py", "--anchor", "COM4", "--tag", "COM7",
                    "--out", str(path), "--rounds", "2", "--execute", "--wifi-off-confirmed",
                    "--calibration-profile", "bench.json"]
            with patch.object(sys, "argv", argv), \
                 patch("ops_soak_suite.subprocess.run") as run, \
                 patch("ops_soak_suite.verify_receipt", side_effect=RuntimeError("SHA-256 mismatch")):
                run.return_value.returncode = 0
                self.assertEqual(main(), 2)
                run.assert_called_once()
                command = run.call_args.args[0]
                self.assertEqual(command[command.index("--calibration-profile") + 1], "bench.json")
            plan = json.loads((path / "soak_receipt.json").read_text(encoding="utf-8"))
            self.assertEqual([row["status"] for row in plan["entries"]], ["REQUESTED", "PENDING"])

    def test_failed_epoch_has_durable_requested_journal_no_retry(self):
        with tempfile.TemporaryDirectory() as d:
            path=Path(d)/"run"
            argv=["ops_soak_suite.py","--anchor","COM4","--tag","COM7",
                  "--out",str(path),"--rounds","3","--execute","--wifi-off-confirmed"]
            def fail(*args,**kwargs):
                j=json.loads((path/"soak_receipt.json").read_text())
                self.assertEqual(j["entries"][0]["status"],"REQUESTED")
                return type("Result",(),{"returncode":2})()
            with patch.object(sys,"argv",argv),patch("ops_soak_suite.subprocess.run",side_effect=fail) as run:
                self.assertEqual(main(),2)
                self.assertEqual(run.call_count,1)
            self.assertEqual(json.loads((path/"soak_receipt.json").read_text())["status"],"IN_PROGRESS")

if __name__=="__main__":unittest.main()
