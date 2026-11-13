"""Mode8 route custody: deterministic sessions, journal and no double START."""
import json
from pathlib import Path
import sys
import tempfile
import unittest
from unittest.mock import patch

sys.path.insert(0,str(Path(__file__).resolve().parents[1]/"host"))
from ops_route import (validate_plan,derive_session,durable_json,RouteController,RouteHold)

PLAN={"route_id":777,"mode":8,"stage":6,"period_us":200000,
      "points":[{"point_id":"A1","count":32},{"point_id":"A2","count":20}]}
KEY=b"x"*32

class PlanTests(unittest.TestCase):
    def test_unique_sessions(self):
        sessions=[derive_session(777,i) for i in range(10000)]
        self.assertEqual(len(set(sessions)),10000)
        self.assertTrue(all(x>0 for x in sessions))
        self.assertEqual(validate_plan(PLAN),PLAN)
    def test_invalid_plan_fail_closed(self):
        for bad in (
            {**PLAN,"mode":7},{**PLAN,"period_us":190000},
            {**PLAN,"points":[{"point_id":"a/b","count":20}]},
            {**PLAN,"points":[{"point_id":"A","count":33}]},
            {**PLAN,"points":[{"point_id":"A","count":True}]},
            {**PLAN,"points":[{"point_id":"A","count":1},{"point_id":"A","count":1}]}
        ):
            with self.assertRaises(ValueError):validate_plan(bad)

class RouteTests(unittest.TestCase):
    def test_resume_rejects_changed_spec_and_missing_journal(self):
        with tempfile.TemporaryDirectory() as d:
            root=Path(d)
            with self.assertRaises(RouteHold):
                RouteController(PLAN,root/"route","127.0.0.1","COM4",KEY,resume=True)
            RouteController(PLAN,root/"route","127.0.0.1","COM4",KEY)
            with self.assertRaises(FileExistsError):
                RouteController(PLAN,root/"route","127.0.0.1","COM4",KEY)
            with self.assertRaises(RouteHold):
                RouteController({**PLAN,"route_id":778},root/"route","127.0.0.1","COM4",KEY,resume=True)
    def test_request_is_durable_before_external_rf_and_never_auto_restarted(self):
        with tempfile.TemporaryDirectory() as d:
            target=Path(d)/"route"
            ctl=RouteController(PLAN,target,"127.0.0.1","COM4",KEY)
            def failed(_cmd,check):
                data=json.loads((target/"route_receipt.json").read_text())
                self.assertEqual(data["points"][0]["state"],"REQUESTED")
                self.assertFalse(check)
                return type("Result",(),{"returncode":2})()
            with patch("ops_route.subprocess.run",side_effect=failed) as proc:
                with self.assertRaises(RouteHold):
                    ctl.run()
                self.assertEqual(proc.call_count,1)
            resumed=RouteController(PLAN,target,"127.0.0.1","COM4",KEY,resume=True)
            with patch("ops_route.subprocess.run") as forbidden:
                with self.assertRaises(RouteHold):resumed.run()
                forbidden.assert_not_called()
    def test_purge_intent_makes_lost_reply_idempotent(self):
        with tempfile.TemporaryDirectory() as d:
            target=Path(d)/"route"
            ctl=RouteController(PLAN,target,"127.0.0.1","COM4",KEY)
            row=ctl.data["points"][0];row.update(state="PURGE_INTENT",anchor_boot=5)
            ctl.save()
            status={"phase":"IDLE","file":False,"boot":5}
            with patch.object(ctl,"verify_point",return_value={"anchor_boot":5}) as verify:
                with patch.object(ctl,"anchor_status",return_value=status):
                    with patch("ops_route.OperationalClient") as client:
                        ctl.purge(0)
                        client.assert_not_called()
                        verify.assert_called_once_with(0)
            self.assertEqual(row["state"],"PURGED")
    def test_preverified_receipt_does_not_replace_raw_replay(self):
        # Before this fix the route trusted "paired_audit" from JSON and
        # would accept arbitrary bytes in tag.rfb8 if the file was nonempty.
        with tempfile.TemporaryDirectory() as d:
            one={**PLAN,"points":[{"point_id":"A1","count":1}]}
            ctl=RouteController(one,Path(d)/"route","127.0.0.1","COM4",KEY)
            entry=ctl.data["points"][0]
            folder=ctl.point_folder(0);folder.mkdir()
            (folder/"tag.rfb8").write_bytes(b"NOT_A_VALID_RFB8_STREAM")
            (folder/"anchor_archive.bin").write_bytes(b"archive-test")
            receipt={"status":"SAVED_BOTH_ENDPOINTS",
                     "requested_session":entry["session"],"requested_count":1,
                     "anchor_identity":{"boot":5,"config_id":6,"firmware":"anchor"},
                     "tag_identity":{"boot":7,"config_id":6,"firmware":"tag"},
                     "tag_capture":{"records":1},
                     "paired_audit":{"paired_records":1,"p1_common_grid":1,"p2_common_grid":1}}
            (folder/"operational_receipt.json").write_text(json.dumps(receipt))
            doc={"session":entry["session"],"count":1,"boot":5,"config":6,
                 "crc32":123,"rows":[]}
            with patch("ops_route.parse_archive",return_value=doc):
                with self.assertRaisesRegex(RouteHold,"on-disk paired RF re-audit failed"):
                    ctl.verify_point(0)

    def test_verified_route_rejects_changed_hash_before_purge(self):
        with tempfile.TemporaryDirectory() as d:
            ctl=RouteController(PLAN,Path(d)/"route","127.0.0.1","COM4",KEY)
            entry=ctl.data["points"][0]
            entry.update(state="VERIFIED",tag_sha256="original")
            with patch.object(ctl,"verify_point",side_effect=RouteHold("tag_sha256 changed")):
                with patch.object(ctl,"anchor_status") as status:
                    with self.assertRaises(RouteHold):ctl.purge(0)
                    status.assert_not_called()

    def test_reboot_does_not_fake_lost_purge_completion(self):
        with tempfile.TemporaryDirectory() as d:
            ctl=RouteController(PLAN,Path(d)/"route","127.0.0.1","COM4",KEY)
            row=ctl.data["points"][0]
            row.update(state="PURGE_INTENT",anchor_boot=5)
            with patch.object(ctl,"anchor_status",return_value={
                    "phase":"IDLE","file":False,"boot":6}):
                with self.assertRaises(RouteHold):ctl.purge(0)
            self.assertNotEqual(row["state"],"PURGED")

    def test_bad_remote_identity_prevents_purge(self):
        with tempfile.TemporaryDirectory() as d:
            ctl=RouteController(PLAN,Path(d)/"route","127.0.0.1","COM4",KEY)
            row=ctl.data["points"][0]
            row.update(state="VERIFIED",anchor_boot=5)
            with patch.object(ctl,"anchor_status",return_value={
                "phase":"STORED","file":True,"file_session":row["session"],
                "file_count":row["count"],"boot":6,"file_acked":True
            }):
                with patch("ops_route.OperationalClient") as client:
                    with self.assertRaises(RouteHold):ctl.purge(0)
                    client.assert_not_called()
            self.assertEqual(row["state"],"VERIFIED")

if __name__=="__main__":unittest.main()
