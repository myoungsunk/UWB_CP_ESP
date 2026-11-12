import unittest
from pathlib import Path
from collections import defaultdict
from types import SimpleNamespace
from unittest.mock import patch

import numpy as np

from analyze import packet_arrays
from calibration_profile import check_record_calibration, load_profile
from capture_audit import stage6_accounting
from rfb8 import encode, read_file
from run_pair import cir_payload_integrity, restore_default_baud
from test_uart_fragments import wire
from uart_fragments import ReplayReceiver


class Stage6Reliability(unittest.TestCase):
    def test_replay_requires_initial_sequence(self):
        with self.assertRaisesRegex(RuntimeError, "verified initial sequence"):
            ReplayReceiver().begin()

    def test_restore_baud_increases_period_before_lowering_uart_rate(self):
        class MockLink:
            def __init__(self):
                self.serial = SimpleNamespace(baudrate=2000000)
                self.commands = []
                self.period = 200000
                self.reported_baud = 2000000

            def send(self, command):
                self.commands.append(command)
                if command == "SET PERIOD_US 300000":
                    self.period = 300000
                elif command == "BAUD 921600":
                    if self.period < 300000:
                        raise RuntimeError("PERIOD_UART_BUDGET")
                    self.reported_baud = 921600

            def wait(self, predicate):
                if not predicate({"event": "HELLO", "period_us": self.period,
                                  "baud": self.reported_baud}):
                    raise AssertionError("unexpected confirmation")

            def disable_replay(self):
                self.commands.append("disable_replay")

        link = MockLink()
        with patch("run_pair.time.sleep"):
            restore_default_baud(link, 200000)
        self.assertEqual(link.commands, ["SET PERIOD_US 300000", "BAUD 921600", "disable_replay", "HELLO"])
        self.assertEqual(link.serial.baudrate, 921600)

    def test_temporary_calibration_validates_each_readback(self):
        path = Path(__file__).resolve().parents[2]/"calibration_stage6_temporary.json"
        profile = load_profile(path)
        for role, binding in profile["roles"].items():
            readbacks = [{"range_sigma_m": cal["sigma_m"], **cal} for cal in binding["radios"]]
            self.assertTrue(all(cal["quality"] == "temporary" and cal["id"] == 0 for cal in readbacks))
            check_record_calibration(profile, role, [{"calibration": readbacks}])
            readbacks[-1]["rx"] += 1
            with self.assertRaisesRegex(ValueError, "calibration mismatch"):
                check_record_calibration(profile, role, [{"calibration": readbacks}])

    def test_txn_944_retransmit_exact_original_in_stream_order(self):
        original = encode({"role": "anchor", "txn": 944, "pad": "x"*4600}, sequence=1412)
        fragments, count = wire(original, 1412)
        self.assertEqual(count, 19)
        data_loss = {1, 4, 8, 10, 11, 12, 15, 17, 18}
        parity_ok = {19, 21, 23, 30, 32, 35}
        damaged = b"".join(x for i, x in enumerate(fragments) if
                           (i < count and i not in data_loss) or i in parity_ok or i >= count+20)
        rx = ReplayReceiver()
        before = encode({"event": "prior"}, kind=1, sequence=1411)
        after = encode({"event": "next"}, kind=1, sequence=1413)
        self.assertEqual(rx.feed(b"".join(wire(before, 1411)[0])), before)
        rx.begin()
        self.assertEqual(rx.feed(damaged + b"".join(wire(after, 1413)[0])), b"")
        self.assertEqual(rx.request(0.0), 1412)
        self.assertIsNone(rx.request(0.1))
        self.assertEqual(rx.feed(b"".join(fragments)), original + after)
        self.assertEqual(rx.statistics()["replayed_sequences"], [1412])
        self.assertEqual(rx.statistics()["unresolved_sequences"], [])
        self.assertEqual(rx.statistics()["fragment"]["unrepairable_records"], 1)
        self.assertEqual(rx.feed(b"".join(fragments)), b"")

    def test_full_record_loss_and_explicit_deadline(self):
        rx = ReplayReceiver()
        before = encode({"event": "prior"}, kind=1, sequence=10)
        rx.feed(b"".join(wire(before, 10)[0]))
        rx.begin()
        rx.feed(b"".join(wire(encode({"event": "next"}, kind=1, sequence=12), 12)[0]))
        self.assertEqual(rx.request(0), 11)
        self.assertEqual(rx.request(0.35), 11)
        self.assertIsNone(rx.request(0.7))
        with self.assertRaises(TimeoutError):
            rx.request(1.5)
        self.assertEqual(rx.statistics()["unresolved_sequences"], [11])

    def test_real_b_probe_misses_preserve_a_and_reject_unsupported_grid(self):
        fixture = Path(__file__).parent/"fixtures/stage6_probe_misses_29_634.rfb8"
        by_role = defaultdict(dict)
        records = defaultdict(list)
        for packet in read_file(fixture):
            meta = dict(packet.meta, _raw_integrity_verified=cir_payload_integrity(packet.meta, packet.raw))
            by_role[meta["role"]][meta["txn"]] = packet
            records[meta["role"]].append(meta)
        self.assertEqual(set(by_role["tag"]), {29, 634})
        for txn, probe in ((29, 1), (634, 2)):
            anchor, tag = by_role["anchor"][txn], by_role["tag"][txn]
            arrays, rows = packet_arrays(tag.meta, tag.raw, anchor.meta)
            a, b = rows[(probe-1)*2:probe*2]
            self.assertTrue(a["cir_valid"])
            self.assertTrue(np.isfinite(arrays["relative_s"][a["slot"]]).all())
            self.assertFalse(b["cir_valid"])
            self.assertEqual(arrays[f"common_p{probe}_delay_s"].size, 0)
            self.assertEqual(arrays[f"common_p{probe}_power_norm"].size, 0)
            self.assertFalse(a["transaction_complete"])
        receipt = {"requested_count": 1000, "period_us": 200000, "audit": {"problems": [
            "TAG_PROBE_AIR_MISMATCH_TXN_29_SLOT_1", "TAG_PROBE_AIR_MISMATCH_TXN_634_SLOT_3",
        ]}}
        result = stage6_accounting(receipt, records)
        self.assertEqual(result["probe_miss_txns"], [29, 634])
        self.assertEqual(result["rf"]["P1"]["probe_miss_txns"], [29])
        self.assertEqual(result["rf"]["P2"]["probe_miss_txns"], [634])
        self.assertEqual(result["rf"]["P1"]["both_endpoint_evidence_confirmed"], 1)
        self.assertEqual(result["rf"]["P2"]["both_endpoint_evidence_confirmed"], 1)
        self.assertFalse(result["strict_gate"])
