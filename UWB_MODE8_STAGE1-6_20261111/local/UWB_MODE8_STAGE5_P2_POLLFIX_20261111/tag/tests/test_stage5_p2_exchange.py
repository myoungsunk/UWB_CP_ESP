"""Regression for the Stage 5 P2 timestamp exchange."""
from copy import deepcopy
import json
from pathlib import Path
import sys
import unittest
import zlib

import numpy as np

from fixture_helpers import synthetic_anchor

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))
from analyze import C_AIR, HZ, ds_twr, packet_arrays


class Stage5P2Exchange(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        fixture = ROOT / "evidence/host"
        cls.tag = json.loads((fixture / "synthetic_tag_record.json").read_bytes())
        cls.raw = (fixture / "SYNTHETIC_CIR_i16.raw").read_bytes()

    def test_legacy_negative_ranges_and_fresh_exchange(self):
        # Recorded Stage 5 session 1588792285, transaction 0, A then B.
        old = (
            [680149924404, 680788899595, 683983780404, 31699599682, 32338575255, 35533455474],
            [680149924404, 681427877340, 683983780404, 29457096627, 30735048206, 33290944859],
        )
        for six, expected in zip(old, (-0.9771576043796443, -2.890048932191501)):
            distance, valid = ds_twr(six)
            self.assertFalse(valid)
            self.assertAlmostEqual(distance, expected, places=10)
        anchor = synthetic_anchor(self.tag)
        arrays, rows = packet_arrays(self.tag, self.raw, anchor)
        for slot, tof in zip((2, 3), (1066, 1090)):
            self.assertEqual(rows[slot]["axis_method"], "P2_DS_fresh_response_complete_anchor_log")
            self.assertAlmostEqual(rows[slot]["range_m"], tof * C_AIR / HZ, delta=0.01)
        self.assertGreater(arrays["common_p2_delay_s"].size, 0)

    def test_wrong_reference_in_crc_valid_second_reply(self):
        anchor = synthetic_anchor(self.tag)
        rx = anchor["response2_rx"][1]
        bad = bytearray.fromhex(rx["air"])
        bad[32:37] = bytes.fromhex(self.tag["frames"][1])[47:52]
        bad[92:96] = zlib.crc32(bad[:92]).to_bytes(4, "little")
        rx["air"] = bad.hex()
        arrays, rows = packet_arrays(self.tag, self.raw, anchor, assume_static=True)
        self.assertTrue(all(row["axis_estimate_available"] for row in rows[:3]))
        self.assertFalse(rows[3]["axis_estimate_available"])
        self.assertEqual(arrays["common_p2_delay_s"].size, 0)
        self.assertTrue(np.isfinite(arrays["relative_s"]).all())

    def test_missing_or_misidentified_poll2_invalidates_only_affected_grid(self):
        for corrupt in ('crc', 'radio', 'no_anchor_tx'):
            tag = deepcopy(self.tag)
            anchor = synthetic_anchor(tag)
            if corrupt == 'crc':
                tag['poll2_rx'][1]['air_crc'] ^= 1
            elif corrupt == 'radio':
                tag['poll2_rx'][1]['radio'] = 0
            else:
                anchor['poll2_tx']['status'] = 0
            arrays, rows = packet_arrays(tag, self.raw, anchor, assume_static=True)
            self.assertTrue(all(row['axis_estimate_available'] for row in rows[:2]))
            self.assertFalse(rows[3]['axis_estimate_available'])
            self.assertEqual(arrays['common_p2_delay_s'].size, 0)
            self.assertTrue(np.isfinite(arrays['relative_s']).all())

    def test_missing_fresh_timestamp_records_preserve_relative_axes(self):
        anchor = synthetic_anchor(self.tag)
        tag = deepcopy(self.tag)
        del tag["response2_tx"]
        del anchor["response2_rx"]
        arrays, rows = packet_arrays(tag, self.raw, anchor)
        self.assertTrue(all(row["axis_estimate_available"] for row in rows[:2]))
        self.assertFalse(any(row["axis_estimate_available"] for row in rows[2:]))
        self.assertTrue(np.isfinite(arrays["relative_s"]).all())
        self.assertEqual(arrays["common_p2_delay_s"].size, 0)
