
import json
import os
import sys
import tempfile
from threading import Lock
import unittest
from pathlib import Path
from unittest.mock import Mock, call, patch, sentinel

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "host"))
import ops_session
from capture_audit import DRAIN_FIRMWARES, OPERATIONAL_DRAIN_FIRMWARES, strict_shutdown_ok


class SessionAckTests(unittest.TestCase):
    def test_stale_active_hello_never_confirms_new_tag_start(self):
        ident={"boot":7,"config_id":8,"firmware":"TAG_EXACT"}
        good={"event":"HELLO","role":"tag","active":True,
              "boot":7,"session":789,"config_id":8,"firmware":"TAG_EXACT",
              "stage":6,"period_us":200000,"baud":2000000,"hw_ok":True,
              "fault_latched":False,"drain_protocol":1}
        self.assertTrue(ops_session.tag_start_matches(good,ident,789))
        for key,bad in (("boot",6),("session",788),("config_id",9),
                        ("role","anchor"),("firmware","OTHER"),("active",False),
                        ("stage",5),("period_us",300000),("baud",921600),
                        ("hw_ok",False),("fault_latched",True),("drain_protocol",0)):
            wrong=dict(good);wrong[key]=bad
            with self.subTest(key=key):
                self.assertFalse(ops_session.tag_start_matches(wrong,ident,789))
        self.assertFalse(ops_session.tag_start_matches(None,ident,789))


    def test_operational_pair_can_prove_strict_shutdown(self):
        for firmware_pair, expected in ((DRAIN_FIRMWARES, True),
                                        (OPERATIONAL_DRAIN_FIRMWARES, True),
                                        ((OPERATIONAL_DRAIN_FIRMWARES[0], "foreign"), False)):
            with self.subTest(firmware_pair=firmware_pair):
                states = {}
                receipt = {"session": 9, "baud": 2000000, "shutdown": {
                    "contract": "drain_v1", "persisted_control_proof": True,
                    "request": 10, "roles": states}}
                for boot, (role, firmware) in enumerate(zip(("anchor", "tag"), firmware_pair), 1):
                    identity = {"firmware": firmware, "boot": boot, "drain_protocol": 1}
                    receipt[f"{role}_identity"] = identity
                    receipt[f"{role}_capture"] = {"records": 1, "capture_decoder": {},
                        "transport": {"pending_records": 0, "unresolved_sequences": [],
                        "pending_fragment_bytes": 0, "partial_fragment_record": False,
                        "host_pending_batches": 0, "durable_acks_sent": 1}}
                    base = {"role": role, "boot": boot, "session": 9}
                    hello = {**identity, **base, "event": "HELLO", "hw_ok": True,
                             "fault_latched": False, "active": False, "baud": 2000000,
                             "period_us": 200000, "hello_request": 12}
                    restored = {**hello, "baud": 921600, "period_us": 300000, "hello_request": 13}
                    states[role] = {key: True for key in ("command_sent", "acknowledged",
                                                         "host_sealed", "baud_restored", "closed")}
                    states[role].update(session=9, drain_request=11, status=hello,
                        stopped={**base, "event": "STOPPED", "stop_request": 10},
                        drained={**base, "event": "DRAINED", "request": 11,
                                 "ok": True, "rf_stopped": True, "reason": "OK",
                                 "pending": 0, "awaiting_ack": 0, "failures": 0},
                        status_proof={"request": 12, "hello": hello},
                        baud_restore_proof={"request": 13, "hello": restored})
                self.assertIs(strict_shutdown_ok(receipt), expected)

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
                             "drain_protocol": 1, "fault_latched": False, "sent": 10, "drops": 0}
                    link = Mock()
                    link.records = 1
                    link.wait.return_value = ident
                    tag_stats = {"records": 1, "complete": 1, "reader_error": None,
                                 "transport": {"durable_acks_sent": 1}}
                    shutdown = {"status_proof": {"hello": {"sent": 11, "drops": 0, "period_miss": 0}}}
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
                         patch.object(ops_session, "stop_and_drain_tag", return_value=(tag_stats, shutdown)), \
                         patch.object(ops_session, "prepare_tag", return_value=ident), \
                         patch.object(ops_session, "audit_saved_pair", \
                                      side_effect=None if good_archive else RuntimeError("RF mismatch"), \
                                      return_value=({"paired_records": 1}, {}, Mock())), \
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

    def test_ack_uses_the_exact_replayed_bytes_and_rechecks_before_sending(self):
        with tempfile.TemporaryDirectory() as temp:
            directory = Path(temp)
            archive_file, tag_file = directory / "anchor_archive.bin", directory / "tag.rfb8"
            archive_file.write_bytes(b"downloaded anchor")
            tag_file.write_bytes(b"original tag")
            with patch.object(ops_session, "parse", return_value=sentinel.doc) as parse, \
                 patch.object(ops_session, "audit_archived_session", return_value=sentinel.audit) as replay:
                result, hashes, recheck = ops_session.audit_saved_pair(
                    directory, sentinel.doc, sentinel.anchor, sentinel.tag, sentinel.stats, 1, 1, 200000)
                self.assertIs(result, sentinel.audit)
                parse.assert_called_once_with(b"downloaded anchor")
                self.assertEqual(replay.call_args.kwargs["tag_bytes"], b"original tag")
                self.assertEqual(set(hashes), {archive_file.name, tag_file.name})
                tag_file.write_bytes(b"swapped after audit")
                with self.assertRaisesRegex(RuntimeError, "changed after audit"):
                    recheck()
                def swap_during_replay(*args, **kwargs):
                    tag_file.write_bytes(b"swapped during audit")
                replay.side_effect = swap_during_replay
                with self.assertRaisesRegex(RuntimeError, "changed after audit"):
                    ops_session.audit_saved_pair(directory, sentinel.doc, sentinel.anchor,
                                                 sentinel.tag, sentinel.stats, 1, 1, 200000)

    def test_saved_profile_does_not_reset_active_fragment_decoder_or_baud(self):
        link = Mock()
        link.wait.side_effect = [{"boot": 7, "session": 0},
                                 {"firmware": ops_session.P2_FRESH_FIRMWARES[1]}, {"pass": True}]
        link.serial.baudrate = 2000000
        ready = {"stage": 6, "record_replay": 1, "silent_baud": 1}
        with patch.object(ops_session, "fresh_hello", return_value={"hello": ready}) as hello, \
             patch.object(ops_session, "send_while_drained") as send:
            self.assertIs(ops_session.prepare_tag(link, 2000000), ready)
        self.assertEqual([c.args[1] for c in send.call_args_list],
                         ["SET STAGE 6", "SET PERIOD_US 200000"])
        self.assertEqual([c.kwargs["baud"] for c in hello.call_args_list], [2000000, 2000000])
        link.enable_replay.assert_not_called()
        self.assertEqual(link.serial.baudrate, 2000000)

    def test_default_baud_transition_does_not_emit_an_unreceivable_sequence(self):
        link = Mock()
        link.write_lock = Lock()
        link.wait.side_effect = [{"boot": 7, "session": 0},
                                 {"firmware": ops_session.P1_FRESH_FIRMWARES[1]}, {"pass": True}]
        ready = {"stage": 6, "record_replay": 1, "silent_baud": 1}
        with patch.object(ops_session, "fresh_hello", return_value={"hello": ready}) as hello, \
             patch.object(ops_session, "send_while_drained") as send, \
             patch.object(ops_session.time, "sleep"):
            self.assertIs(ops_session.prepare_tag(link, 921600), ready)
        self.assertEqual([c.args[1] for c in send.call_args_list],
                         ["SET STAGE 6", "BAUD 2000000 SILENT", "SET PERIOD_US 200000"])
        self.assertEqual([c.kwargs["baud"] for c in hello.call_args_list], [921600, 2000000, 2000000])
        link.enable_replay.assert_called_once()
        self.assertEqual(link.serial.baudrate, 2000000)

    def test_paired_count_cannot_hide_tag_period_overrun_or_missing_ack(self):
        before = {"sent": 41, "drops": 2}
        after = {"sent": 73, "drops": 2, "period_miss": 0}
        stats = {"records": 32, "complete": 32, "transport": {"durable_acks_sent": 32}}
        ops_session.validate_tag_completion(stats, before, after, 32)
        with self.assertRaisesRegex(RuntimeError, "Tag capture, period"):
            ops_session.validate_tag_completion(stats, before, {**after, "period_miss": 1}, 32)
        with self.assertRaisesRegex(RuntimeError, "durable record ACK"):
            ops_session.validate_tag_completion({**stats, "transport": {"durable_acks_sent": 31}},
                                                before, after, 32)


if __name__ == "__main__":
    unittest.main()
