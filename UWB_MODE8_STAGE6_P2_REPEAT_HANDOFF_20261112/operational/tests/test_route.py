"""Mode8 route custody: deterministic sessions, journal and no double START."""
import hashlib
import json
from pathlib import Path
import sys
import tempfile
import unittest
from unittest.mock import patch

sys.path.insert(0,str(Path(__file__).resolve().parents[1]/"host"))
from ops_route import (validate_plan,derive_session,durable_json,unique_json,RouteController,RouteHold,
                       journal_signature)

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
            {**PLAN,"mode":7},{**PLAN,"period_us":190000},{**PLAN,"mode":8.0},
            {**PLAN,"stage":6.0},{**PLAN,"period_us":200000.0},
            {**PLAN,"points":[{"point_id":"a/b","count":20}]},
            {**PLAN,"points":[{"point_id":"A","count":33}]},
            {**PLAN,"points":[{"point_id":"A","count":True}]},
            {**PLAN,"points":[{"point_id":"A","count":1},{"point_id":"A","count":1}]}
        ):
            with self.assertRaises(ValueError):validate_plan(bad)

    def test_duplicate_nested_json_key_rejected(self):
        with self.assertRaisesRegex(ValueError,"duplicate JSON key: count"):
            unique_json('{"points":[{"count":1,"count":32}]}')


class RouteTests(unittest.TestCase):
    def point_evidence(self,ctl,index=0):
        entry=ctl.data["points"][index]
        folder=ctl.point_folder(index)
        folder.mkdir()
        receipt,archive,tag=ctl.point_files(index)
        tag.write_bytes(b"audited tag")
        archive.write_bytes(b"audited anchor")
        count=entry["count"]
        paired={k:count for k in ("paired_records","p1_common_grid","p2_common_grid")}
        receipt.write_text(json.dumps({
            "status":"SAVED_BOTH_ENDPOINTS","requested_session":entry["session"],
            "requested_count":count,"anchor_identity":{"boot":5,"config_id":6},
            "tag_identity":{"boot":7,"config_id":6},"tag_capture":{"records":count},
            "paired_audit":paired}),encoding="utf-8")
        doc={"session":entry["session"],"count":count,"boot":5,"config":6,"crc32":123}
        proof={field:hashlib.sha256(path.read_bytes()).hexdigest()
               for field,path in zip(("receipt_sha256","anchor_sha256","tag_sha256"),
                                     (receipt,archive,tag))}
        proof.update(archive_crc=123,anchor_boot=5,tag_boot=7)
        return doc,paired,proof

    def test_journal_authentication_rejects_modified_purged_state(self):
        with tempfile.TemporaryDirectory() as d:
            path=Path(d)/"route"
            ctl=RouteController(PLAN,path,"127.0.0.1","COM4",KEY)
            saved=json.loads(ctl.path.read_text())
            saved["points"][0]["state"]="PURGED"
            ctl.path.write_text(json.dumps(saved))
            with self.assertRaisesRegex(RouteHold,"authentication"):
                RouteController(PLAN,path,"127.0.0.1","COM4",KEY,resume=True)

    def test_signed_impossible_point_order_is_rejected(self):
        with tempfile.TemporaryDirectory() as d:
            path=Path(d)/"route"
            ctl=RouteController(PLAN,path,"127.0.0.1","COM4",KEY)
            saved=json.loads(ctl.path.read_text())
            saved["points"][1]["state"]="REQUESTED"
            # Even a bad client holding a key cannot bypass semantic order.
            saved["journal_hmac_sha256"]=journal_signature(saved,KEY)
            ctl.path.write_text(json.dumps(saved))
            with self.assertRaisesRegex(RouteHold,"state or order"):
                RouteController(PLAN,path,"127.0.0.1","COM4",KEY,resume=True)

    def test_resume_with_wrong_signing_key_fails(self):
        with tempfile.TemporaryDirectory() as d:
            path=Path(d)/"route"
            RouteController(PLAN,path,"127.0.0.1","COM4",KEY)
            with self.assertRaisesRegex(RouteHold,"authentication"):
                RouteController(PLAN,path,"127.0.0.1","COM4",b"y"*32,resume=True)


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
            approval=b"a"*32
            ctl=RouteController(PLAN,target,"127.0.0.1","COM4",KEY,approval_key=approval)
            def failed(cmd,check,env):
                self.assertEqual(env["MODE8_AUTH_KEY_HEX"],KEY.hex())
                self.assertEqual(env["MODE8_APPROVAL_KEY_HEX"],approval.hex())
                self.assertNotIn(KEY.hex(),cmd)
                self.assertNotIn(approval.hex(),cmd)
                data=json.loads((target/"route_receipt.json").read_text())
                self.assertEqual(data["points"][0]["state"],"REQUESTED")
                self.assertFalse(check)
                return type("Result",(),{"returncode":2})()
            with patch("ops_route.subprocess.run",side_effect=failed) as proc:
                with self.assertRaises(RouteHold):
                    ctl.run()
                self.assertEqual(proc.call_count,1)
            resumed=RouteController(PLAN,target,"127.0.0.1","COM4",KEY,resume=True,approval_key=approval)
            with patch("ops_route.subprocess.run") as forbidden:
                with self.assertRaises(RouteHold):resumed.run()
                forbidden.assert_not_called()
    def test_calibration_profile_forwarded_to_each_point(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            profile = root / "profile.json"
            controller = RouteController(PLAN, root / "route", "127.0.0.1", "COM4", KEY,
                                         calibration_profile=profile)
            with patch("ops_route.subprocess.run") as launch, \
                    patch.object(controller, "verify_point", return_value={}):
                launch.return_value.returncode = 0
                for index in range(len(PLAN["points"])):
                    controller.run_point(index)
                self.assertEqual(launch.call_count, len(PLAN["points"]))
                for invocation in launch.call_args_list:
                    command = invocation.args[0]
                    self.assertEqual(command[-2:], ["--calibration-profile", str(profile)])

    def test_purge_intent_makes_lost_reply_idempotent(self):
        with tempfile.TemporaryDirectory() as d:
            target=Path(d)/"route"
            ctl=RouteController(PLAN,target,"127.0.0.1","COM4",KEY)
            row=ctl.data["points"][0];row.update(state="PURGE_INTENT",anchor_boot=5)
            ctl.save()
            status={"phase":"IDLE","file":False,"boot":5}
            with patch.object(ctl,"verify_point",return_value={"anchor_boot":5}) as verify:
                with patch.object(ctl,"anchor_status",return_value=status):
                    with patch("ops_route.OperationalClient") as client, \
                            patch("ops_route.check_point_file_hashes"):
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

    def test_route_replays_both_endpoints_and_detects_new_disk_corruption(self):
        # Positive replay proof is required even when the prior receipt claims
        # PASS. The stored journal hash then protects against later bit rot.
        with tempfile.TemporaryDirectory() as d:
            one={**PLAN,"points":[{"point_id":"A1","count":1}]}
            ctl=RouteController(one,Path(d)/"route","127.0.0.1","COM4",KEY)
            entry=ctl.data["points"][0]
            folder=ctl.point_folder(0);folder.mkdir()
            tag=folder/"tag.rfb8";tag.write_bytes(b"original-valid-test-placeholder")
            (folder/"anchor_archive.bin").write_bytes(b"anchor-test-placeholder")
            receipt={"status":"SAVED_BOTH_ENDPOINTS",
                     "requested_session":entry["session"],"requested_count":1,
                     "anchor_identity":{"boot":5,"config_id":6,"firmware":"anchor"},
                     "tag_identity":{"boot":7,"config_id":6,"firmware":"tag"},
                     "tag_capture":{"records":1},
                     "paired_audit":{"paired_records":1,"p1_common_grid":1,"p2_common_grid":1}}
            (folder/"operational_receipt.json").write_text(json.dumps(receipt))
            doc={"session":entry["session"],"count":1,"boot":5,"config":6,
                 "crc32":123,"rows":[]}
            verified={"paired_records":1,"p1_common_grid":1,"p2_common_grid":1}
            with patch("ops_route.parse_archive",return_value=doc):
                with patch("ops_session.audit_archived_session",return_value=verified) as replay:
                    proof=ctl.verify_point(0)
                    replay.assert_called_once()
                    entry.update(proof)
                    tag.write_bytes(b"corrupted-after-first-verified-receipt")
                    with self.assertRaisesRegex(RouteHold,"tag_sha256 changed"):
                        ctl.verify_point(0)
                    with patch.object(ctl,"anchor_status") as remote:
                        entry["state"]="VERIFIED"
                        with self.assertRaises(RouteHold):
                            ctl.purge(0)
                        remote.assert_not_called()

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

    def test_replay_cannot_pin_bytes_replaced_during_audit(self):
        with tempfile.TemporaryDirectory() as d:
            ctl=RouteController(PLAN,Path(d)/"route","127.0.0.1","COM4",KEY)
            doc,paired,_=self.point_evidence(ctl)
            _,_,tag=ctl.point_files(0)

            def replay(*args,tag_bytes):
                self.assertEqual(tag_bytes,b"audited tag")
                tag.write_bytes(b"replacement not replayed")
                return paired

            with patch("ops_route.parse_archive",return_value=doc), \
                    patch("ops_session.audit_archived_session",side_effect=replay):
                with self.assertRaisesRegex(RouteHold,"tag_sha256 changed"):
                    ctl.verify_point(0)

    def test_present_receipt_hashes_must_bind_both_raw_snapshots(self):
        with tempfile.TemporaryDirectory() as d:
            ctl=RouteController(PLAN,Path(d)/"route","127.0.0.1","COM4",KEY)
            doc,paired,proof=self.point_evidence(ctl)
            receipt=ctl.point_files(0)[0]
            data=json.loads(receipt.read_bytes())
            with patch("ops_route.parse_archive",return_value=doc), \
                    patch("ops_session.audit_archived_session",return_value=paired) as replay:
                for hashes in (None, {"tag.rfb8":proof["tag_sha256"]}, {
                        "anchor_archive.bin":proof["anchor_sha256"],"tag.rfb8":"0"*64}):
                    with self.subTest(hashes=hashes):
                        data["raw_sha256"]=hashes
                        durable_json(receipt,data)
                        with self.assertRaisesRegex(RouteHold,"receipt hashes"):
                            ctl.verify_point(0)
                replay.assert_not_called()
                data["raw_sha256"]={"anchor_archive.bin":proof["anchor_sha256"],
                                    "tag.rfb8":proof["tag_sha256"]}
                durable_json(receipt,data)
                self.assertEqual(ctl.verify_point(0)["tag_sha256"],proof["tag_sha256"])
                replay.assert_called_once()

    def test_resume_rejects_changed_journal_point_order_and_proof(self):
        with tempfile.TemporaryDirectory() as d:
            target=Path(d)/"route"
            ctl=RouteController(PLAN,target,"127.0.0.1","COM4",KEY)
            original=json.dumps(ctl.data)
            for point,field,value in ((0,"session",ctl.data["points"][1]["session"]),
                                      (0,"point_id","replacement"),(0,"count",31),
                                      (1,"state","REQUESTED"),(0,"state","PURGED")):
                with self.subTest(point=point,field=field):
                    ctl.data=json.loads(original)
                    ctl.data["points"][point][field]=value
                    ctl.save()
                    with self.assertRaises(RouteHold):
                        RouteController(PLAN,target,"127.0.0.1","COM4",KEY,resume=True)

    def test_resume_rechecks_purged_original_before_next_point(self):
        with tempfile.TemporaryDirectory() as d:
            target=Path(d)/"route"
            ctl=RouteController(PLAN,target,"127.0.0.1","COM4",KEY)
            _,_,proof=self.point_evidence(ctl)
            ctl.data["points"][0].update(proof,state="PURGED")
            ctl.save()
            ctl.point_files(0)[2].write_bytes(b"corrupted original")
            resumed=RouteController(PLAN,target,"127.0.0.1","COM4",KEY,resume=True)
            with patch("ops_route.subprocess.run") as proc:
                with self.assertRaisesRegex(RouteHold,"tag_sha256 changed"):
                    resumed.run()
                proc.assert_not_called()

    def test_concurrent_and_stale_resume_cannot_repeat_start(self):
        with tempfile.TemporaryDirectory() as d:
            target=Path(d)/"route"
            first=RouteController(PLAN,target,"127.0.0.1","COM4",KEY)
            stale=RouteController(PLAN,target,"127.0.0.1","COM4",KEY,resume=True)

            def failed(_cmd,check,env):
                with self.assertRaisesRegex(RouteHold,"route is locked"):
                    stale.run()
                return type("Result",(),{"returncode":2})()

            with patch("ops_route.subprocess.run",side_effect=failed) as proc:
                with self.assertRaisesRegex(RouteHold,"capture incomplete"):
                    first.run()
                proc.assert_called_once()
            with patch("ops_route.subprocess.run") as proc:
                with self.assertRaisesRegex(RouteHold,"invalid point evidence"):
                    stale.run()
                proc.assert_not_called()

    def test_purge_rechecks_after_remote_status_and_confirms_same_boot(self):
        for mutate in (True,False):
            with self.subTest(mutate=mutate), tempfile.TemporaryDirectory() as d:
                ctl=RouteController(PLAN,Path(d)/"route","127.0.0.1","COM4",KEY,
                                    approval_key=b"a"*32)
                doc,paired,proof=self.point_evidence(ctl)
                row=ctl.data["points"][0]
                row.update(proof,state="VERIFIED")
                status={"phase":"STORED","file":True,"file_session":row["session"],
                        "file_count":row["count"],"boot":5,"file_acked":True}
                final={"phase":"IDLE","file":False,"boot":6}
                sent=[]

                def purge(session,crc,boot,*,before_send):
                    if mutate:
                        ctl.point_files(0)[2].write_bytes(b"replaced during remote status")
                    before_send()
                    sent.append((session,crc,boot))
                    return "PURGE_OK"

                with patch("ops_route.parse_archive",return_value=doc), \
                        patch("ops_session.audit_archived_session",return_value=paired), \
                        patch.object(ctl,"anchor_status",side_effect=[status,final]), \
                        patch("ops_route.OperationalClient") as client:
                    client.return_value.__enter__.return_value.purge_archive.side_effect=purge
                    message="tag_sha256 changed" if mutate else "purge not durably confirmed"
                    with self.assertRaisesRegex(RouteHold,message):
                        ctl.purge(0)
                self.assertEqual(row["state"],"PURGE_INTENT")
                self.assertEqual(len(sent),0 if mutate else 1)

if __name__=="__main__":unittest.main()
