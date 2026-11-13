
import json
import os
import sys
import tempfile
import unittest
from pathlib import Path
from unittest.mock import Mock, call, patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "host"))
import ops_session


class SessionAckTests(unittest.TestCase):
    def test_stale_active_hello_never_confirms_new_tag_start(self):
        ident={"boot":7,"config_id":8,"firmware":"TAG_EXACT"}
        good={"event":"HELLO","role":"tag","active":True,
              "boot":7,"session":789,"config_id":8,"firmware":"TAG_EXACT",
              "stage":6,"period_us":200000,"baud":2000000}
        self.assertTrue(ops_session.tag_start_matches(good,ident,789))
        for key,bad in (("boot",6),("session",788),("config_id",9),
                        ("role","anchor"),("firmware","OTHER"),("active",False),
                        ("stage",5),("period_us",300000),("baud",921600)):
            wrong=dict(good);wrong[key]=bad
            with self.subTest(key=key):
                self.assertFalse(ops_session.tag_start_matches(wrong,ident,789))
        self.assertFalse(ops_session.tag_start_matches(None,ident,789))

    def test_session_acks_verified_archive_with_crc_on_get_connection(self):
        for good_archive, ack_reply, expected_status in (
            (True, "ACKFILE_OK", "SAVED_BOTH_ENDPOINTS"),
            (True, "ERROR ACKFILE", "INCOMPLETE"),
            (False, "ACKFILE_OK", "INCOMPLETE"),
        ):
            with self.subTest(good_archive=good_archive, ack_reply=ack_reply):
                with tempfile.TemporaryDirectory() as temp:
                    out = Path(temp) / "run"
                    args = ["ops_session.py", "--anchor-ip", "127.0.0.1",
                            "--tag-port", "unused", "--session", "789", "--count", "1",
                            "--out", str(out)]
                    anchor_fw, tag_fw = ops_session.OPERATIONAL_DRAIN_FIRMWARES
                    ident = {"boot": 7, "config_id": 8, "hw_ok": True, "firmware": tag_fw,
                             "drain_protocol": 1, "fault_latched": False}
                    link = Mock()
                    link.records = 1
                    link.wait.side_effect = [None, ident, {"pass": True}, None, None]
                    tag_stats = {"records": 1, "complete": 1, "reader_error": None}
                    stored = {"phase": "STORED", "boot": 9, "session": 789, "drained": True,
                              "hw_ok": True, "fault_latched": False, "period_miss": 0}
                    status_ctl, start_ctl, archive_ctl = Mock(), Mock(), Mock()
                    status_ctl.boot = start_ctl.boot = archive_ctl.boot = 9
                    status_ctl.command.return_value = json.dumps(
                        {"phase": "IDLE", "file": False, "config": 8, "boot": 9,
                         "firmware": anchor_fw, "hw_ok": True, "fault_latched": False,
                         "stage": 6, "period_us": 200000, "baud": 2000000, "drained": True})
                    start_ctl.command.return_value = "START_ACCEPTED_WIFI_OFF"
                    archive_ctl.get_archive.return_value = {
                        "session": 789, "crc32": 0x89ABCDEF, "count": 1,
                        "rows": [{"complete": good_archive, "config": 8, "session": 789,
                                  "peer_boot": 7}],
                    }
                    calls = Mock()
                    calls.attach_mock(archive_ctl.get_archive, "get")
                    calls.attach_mock(archive_ctl.acknowledge_archive, "ack")
                    archive_ctl.acknowledge_archive.return_value = ack_reply
                    contexts = []
                    for ctl in (status_ctl, start_ctl, archive_ctl):
                        cm = Mock()
                        cm.__enter__ = Mock(return_value=ctl)
                        cm.__exit__ = Mock(return_value=False)
                        contexts.append(cm)
                    with patch.object(sys, "argv", args), \
                         patch.dict(os.environ, {"MODE8_AUTH_KEY_HEX": "00" * 32,
                                                   "MODE8_APPROVAL_KEY_HEX": "11" * 32}), \
                         patch.object(ops_session, "Link", return_value=link), \
                         patch.object(ops_session, "OperationalClient", side_effect=contexts), \
                         patch.object(ops_session, "wait_for_anchor", return_value=stored), \
                         patch.object(ops_session, "stop_and_drain_tag", return_value=(tag_stats, {})), \
                         patch.object(ops_session, "send_while_drained", return_value=ident), \
                         patch.object(ops_session, "audit_archived_session", \
                                      side_effect=None if good_archive else RuntimeError("RF mismatch"), \
                                      return_value={"paired_records": 1}), \
                         patch.object(ops_session.time, "sleep"):
                        code = ops_session.main()
                    receipt = json.loads((out / "operational_receipt.json").read_text(encoding="utf-8"))
                    self.assertEqual(receipt["status"], expected_status)
                    self.assertEqual(code, 0 if expected_status == "SAVED_BOTH_ENDPOINTS" else 2)
                    if good_archive:
                        self.assertEqual(calls.mock_calls, [
                            call.get(out / "anchor_archive.bin", 789, ack=False),
                            call.ack(789, 0x89ABCDEF),
                        ])
                    else:
                        archive_ctl.acknowledge_archive.assert_not_called()
                    archive_ctl.command.assert_not_called()


if __name__ == "__main__":
    unittest.main()
