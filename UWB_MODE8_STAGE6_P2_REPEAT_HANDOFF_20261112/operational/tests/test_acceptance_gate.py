"""Stage gate requires actual SHA-verified evidence from every predecessor."""
from pathlib import Path
import hashlib
import tempfile
import unittest
import sys
sys.path.insert(0,str(Path(__file__).resolve().parents[1]/"host"))
from ops_acceptance_gate import assess,AcceptanceHold,GATES

SHA="1"*40
IMGHASH="a"*64
class GateTests(unittest.TestCase):
    def manifest(self,tests):
        return {"git_sha":SHA,"anchor_bin_sha256":IMGHASH,
                "tag_bin_sha256":IMGHASH,"tests":tests}
    def test_no_evidence_never_approves_hardware(self):
        with tempfile.TemporaryDirectory() as d:
            result=assess(self.manifest([]),Path(d),SHA)
            self.assertEqual(result["gates"]["G0"],"NOT_RUN")
            self.assertFalse(result["hardware_field_approved"])
            self.assertFalse(result["source_host_build_candidate"])
    def test_previous_gate_must_pass(self):
        with tempfile.TemporaryDirectory() as d:
            p=Path(d)/"proof.log";p.write_bytes(b"immutable")
            data={"path":"proof.log","sha256":hashlib.sha256(b"immutable").hexdigest()}
            result=assess(self.manifest([{"id":"G1","attempted":True,"pass":True,
                                         "evidence":[data]}]),Path(d),SHA)
            self.assertEqual(result["gates"]["G1"],"BLOCKED_BY_PREVIOUS_GATE")
    def test_full_proof_and_corrupt_proof(self):
        with tempfile.TemporaryDirectory() as d:
            p=Path(d)/"proof.log";p.write_bytes(b"actual-raw")
            data={"path":"proof.log","sha256":hashlib.sha256(b"actual-raw").hexdigest()}
            tests=[{"id":g,"attempted":True,"pass":True,"evidence":[data]} for g in GATES]
            report=assess(self.manifest(tests),Path(d),SHA)
            self.assertTrue(report["field_evidence_complete"])
            self.assertFalse(report["hardware_field_approved"])
            self.assertEqual(report["result"],"EVIDENCE_READY_NEEDS_FIELD_SIGNOFF")
            p.write_bytes(b"tampered")
            report=assess(self.manifest(tests),Path(d),SHA)
            self.assertFalse(report["hardware_field_approved"])
            self.assertTrue(report["gates"]["G0"].startswith("INVALID_EVIDENCE"))
    def test_duplicate_or_path_escape_rejected(self):
        with tempfile.TemporaryDirectory() as d:
            p=Path(d)/"file";p.write_bytes(b"a")
            e={"path":"../outside","sha256":hashlib.sha256(b"a").hexdigest()}
            result=assess(self.manifest([{"id":"G0","attempted":True,"pass":True,
                                         "evidence":[e]}]),Path(d),SHA)
            self.assertTrue(result["gates"]["G0"].startswith("INVALID_EVIDENCE"))
            with self.assertRaisesRegex(AcceptanceHold,"duplicate"):
                assess(self.manifest([{"id":"G0"},{"id":"G0"}]),Path(d),SHA)
    def test_source_identity_mismatch_is_fatal(self):
        with tempfile.TemporaryDirectory() as d:
            with self.assertRaisesRegex(AcceptanceHold,"source commit"):
                assess(self.manifest([]),Path(d),"2"*40)
