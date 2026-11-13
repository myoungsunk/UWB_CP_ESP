"""Wired Mode8 safety: explicit arming and raw byte evidence."""
import hashlib
import json
from pathlib import Path
import sys
import tempfile
import unittest
from unittest.mock import patch
sys.path.insert(0,str(Path(__file__).resolve().parents[1]/"host"))
from ops_wired_continuous import command_for,verify_receipt,main

class WiredTests(unittest.TestCase):
    def test_requires_two_ports_and_stage6(self):
        command=command_for("COM4","COM7",Path("run"),1000,2000000,921600)
        for key,expected in (("--stage","6"),("--baud","2000000"),("--period-us","200000"),
                             ("--anchor-initial-baud","2000000")):
            self.assertEqual(command[command.index(key)+1],expected)
        self.assertNotIn("--calibration-profile", command)
        for anchor,tag,count,b1,b2 in (
            ("COM4","COM4",1,921600,921600),("COM4","COM7",0,921600,921600),
            ("COM4","COM7",100001,921600,921600),("COM4","COM7",1,38400,921600)):
            with self.assertRaises(ValueError):
                command_for(anchor,tag,Path("run"),count,b1,b2)

    def test_rejects_incomplete_grids_even_with_pass_receipt(self):
        with tempfile.TemporaryDirectory() as d:
            out=Path(d)
            (out/"anchor.rfb8").write_bytes(b"raw_anchor")
            (out/"tag.rfb8").write_bytes(b"raw_tag")
            hashes = {name: hashlib.sha256((out / name).read_bytes()).hexdigest()
                      for name in ("anchor.rfb8", "tag.rfb8")}
            receipt={"pass":True,"stage":6,"baud":2000000,"period_us":200000,
                     "requested_count":2,"stage6":{"common_axis_and_power":{"P1":2,"P2":2}},
                     "file_sha256":hashes}
            (out/"capture_receipt.json").write_text(json.dumps(receipt))
            self.assertEqual(verify_receipt(out,2)["status"],"VERIFIED")
            receipt["stage6"]["common_axis_and_power"]["P2"]=1
            (out/"capture_receipt.json").write_text(json.dumps(receipt))
            with self.assertRaisesRegex(RuntimeError,"grids"):verify_receipt(out,2)

    def test_rejects_changed_rfb8_and_missing_original_hash(self):
        with tempfile.TemporaryDirectory() as d:
            out = Path(d)
            original = b"original RFB8"
            hashes = {}
            for name in ("anchor.rfb8", "tag.rfb8"):
                (out / name).write_bytes(original)
                hashes[name] = hashlib.sha256(original).hexdigest()
            receipt = {
                "pass": True, "stage": 6, "baud": 2000000, "period_us": 200000,
                "requested_count": 1,
                "stage6": {"common_axis_and_power": {"P1": 1, "P2": 1}},
                "file_sha256": hashes,
            }
            path = out / "capture_receipt.json"
            path.write_text(json.dumps(receipt), encoding="utf-8")
            self.assertEqual(verify_receipt(out, 1)["status"], "VERIFIED")
            for name in hashes:
                with self.subTest(name=name):
                    raw = out / name
                    raw.write_bytes(b"changed! RFB8")
                    with self.assertRaisesRegex(RuntimeError, f"SHA-256 mismatch: {name}"):
                        verify_receipt(out, 1)
                    raw.write_bytes(original)
            del hashes["tag.rfb8"]
            path.write_text(json.dumps(receipt), encoding="utf-8")
            with self.assertRaisesRegex(RuntimeError, "SHA-256 mismatch: tag.rfb8"):
                verify_receipt(out, 1)
            del receipt["file_sha256"]
            path.write_text(json.dumps(receipt), encoding="utf-8")
            with self.assertRaisesRegex(RuntimeError, "missing original RFB8 hashes"):
                verify_receipt(out, 1)

    def test_dry_run_never_uses_serial(self):
        with tempfile.TemporaryDirectory() as d:
            target=Path(d)/"capture"
            profile = target.parent / "bench profile.json"
            argv=["ops_wired_continuous.py","--anchor","COM4","--tag","COM7",
                  "--out",str(target),"--calibration-profile",str(profile)]
            with patch.object(sys,"argv",argv),patch("ops_wired_continuous.subprocess.run") as run, \
                 patch("builtins.print") as output:
                self.assertEqual(main(),0)
                run.assert_not_called()
            command = json.loads(output.call_args.args[0])["command"]
            self.assertEqual(command[command.index("--calibration-profile") + 1], str(profile))
            self.assertFalse(target.exists())

    def test_execute_requires_wifi_off_and_nonexisting_directory(self):
        with tempfile.TemporaryDirectory() as d:
            out=Path(d)/"capture"
            argv=["ops_wired_continuous.py","--anchor","COM4","--tag","COM7","--out",str(out),"--execute"]
            with patch.object(sys,"argv",argv),patch("ops_wired_continuous.subprocess.run") as run:
                self.assertEqual(main(),2)
                run.assert_not_called()
            self.assertFalse(out.exists())
