"""Synthetic paired RF evidence for Stage 6 P2 repeat selection."""
from copy import deepcopy
import json
from pathlib import Path
import sys
import unittest
import zlib

import numpy as np

from analyze import packet_arrays
from capture_audit import (P2_REPEAT_ANCHOR, P2_REPEAT_TAG, TICK_MODULUS,
                           p2_selection, stage6_accounting, verify_capture)
from run_pair import cir_payload_integrity

LEGACY = Path(__file__).resolve().parents[2] / "UWB_MODE8_STAGE6_RELIABILITY_20261112" / "tag"
sys.path.append(str(LEGACY / "tests"))
from fixture_helpers import synthetic_anchor


def frame_shift(frame, offsets, change):
    data = bytearray.fromhex(frame)
    for offset in offsets:
        tick = int.from_bytes(data[offset:offset + 5], "little")
        data[offset:offset + 5] = ((tick + change) % TICK_MODULUS).to_bytes(5, "little")
    data[92:96] = zlib.crc32(data[:92]).to_bytes(4, "little")
    return data.hex()


def shift_event(event, fields, change):
    for field in fields:
        event[field] = (event[field] + change) % TICK_MODULUS


def paired_fixture(use_repeat):
    directory = LEGACY / "evidence" / "host"
    tag = json.loads((directory / "synthetic_tag_stage6.json").read_bytes())
    raw = (directory / "SYNTHETIC_CIR_i16.raw").read_bytes()
    anchor = synthetic_anchor(tag)
    tag.update(firmware=P2_REPEAT_TAG, p2_repeat_used=use_repeat,
               p2_primary_rx_mask=2 if use_repeat else 3,
               p2_primary_cir_mask=2 if use_repeat else 3,
               _raw_integrity_verified=cir_payload_integrity(tag, raw))
    anchor.update(firmware=P2_REPEAT_ANCHOR, p1_retry=tag["p1_retry"],
                  initial_poll_tx=tag["initial_poll_tx"])
    shift = -(14000 * 638976 // 10)
    anchor["p2_repeat_frame"] = anchor["frames"][2]
    anchor["frames"][2] = frame_shift(anchor["frames"][2], (47,), shift)
    anchor["tx"][2].update(attempted=True, status=0x80)
    anchor["p2_repeat_tx"] = deepcopy(anchor["tx"][2])
    shift_event(anchor["tx"][2], ("planned", "actual"), shift)
    tag["poll2_frame"] = anchor["poll2_frame"] = frame_shift(anchor["poll2_frame"], (32,), shift)
    shift_event(anchor["poll2_tx"], ("actual", "planned"), shift)
    crc = int.from_bytes(bytes.fromhex(tag["poll2_frame"])[92:96], "little")
    for radio in range(2):
        poll, tx, rx = tag["poll2_rx"][radio], tag["response2_tx"][radio], anchor["response2_rx"][radio]
        shift_event(poll, ("hw",), shift)
        poll["air_crc"] = crc
        shift_event(tx, ("actual", "planned"), shift)
        shift_event(rx, ("hw",), shift)
        rx["air"] = frame_shift(rx["air"], (32, 57, 62), shift)
        if not use_repeat:
            tag["frames"][2] = anchor["frames"][2]
            tag["probe_rx"][2 + radio]["air"] = tag["frames"][2]
            shift_event(tag["probe_rx"][2 + radio], ("hw",), shift)
    return anchor, tag, raw


def audit(anchor, tag):
    identities = {row["role"]: {**row, "config_id": row["config"], "hw_ok": True}
                  for row in (anchor, tag)}
    return verify_capture(session=tag["session"], count=1, stage=6, period_us=tag["period_us"],
                          identities=identities, records={"anchor": [anchor], "tag": [tag]}, stats={})


class P2Repeat(unittest.TestCase):
    def test_primary_and_repeat_axes_and_receipt(self):
        for used in (False, True):
            with self.subTest(used=used):
                anchor, tag, raw = paired_fixture(used)
                proof = p2_selection(anchor, tag)
                self.assertIsNotNone(proof)
                result = audit(anchor, tag)
                self.assertTrue(result.passed, result.problems)
                arrays, rows = packet_arrays(tag, raw, anchor)
                self.assertTrue(all(row["axis_estimate_available"] for row in rows))
                self.assertTrue(np.isfinite(arrays["common_p2_power_norm"]).any())
                accounting = stage6_accounting(
                    {"requested_count": 1, "period_us": tag["period_us"],
                     "audit": {"problems": result.problems}},
                    {"tag": [tag], "anchor": [anchor]})
                self.assertEqual(accounting["rf"]["P2"]["initial_confirmed"], int(not used))
                self.assertEqual(accounting["rf"]["P2"]["primary_complete"], int(not used))
                self.assertEqual(accounting["rf"]["P2"]["repeat_restored_txns"], [tag["txn"]] if used else [])

    def test_invalid_repeat_never_creates_common_p2_axis(self):
        for corrupt in ("mixed_air", "mixed_ticks", "forged_timestamp", "missing_tx", "missing_marker",
                        "primary_already_complete", "corrupted_cir"):
            with self.subTest(corrupt=corrupt):
                anchor, tag, raw = paired_fixture(True)
                if corrupt == "mixed_air":
                    tag["probe_rx"][2]["air"] = anchor["frames"][2]
                elif corrupt == "mixed_ticks":
                    shift_event(tag["probe_rx"][2], ("hw",), -(14000 * 638976 // 10))
                elif corrupt == "forged_timestamp":
                    forged = frame_shift(anchor["p2_repeat_frame"], (47,), 2000)
                    anchor["p2_repeat_frame"] = tag["frames"][2] = forged
                    for slot in tag["probe_rx"][2:]:
                        slot["air"] = forged
                    shift_event(anchor["p2_repeat_tx"], ("actual", "planned"), 2000)
                elif corrupt == "missing_tx":
                    del anchor["p2_repeat_tx"]
                elif corrupt == "missing_marker":
                    del tag["p2_repeat_used"]
                elif corrupt == "primary_already_complete":
                    tag["p2_primary_cir_mask"] = tag["p2_primary_rx_mask"] = 3
                else:
                    raw = raw[:10000] + bytes([raw[10000] ^ 1]) + raw[10001:]
                    tag["_raw_integrity_verified"] = cir_payload_integrity(tag, raw)
                result = audit(anchor, tag)
                self.assertFalse(result.passed, result.problems)
                arrays, _ = packet_arrays(tag, raw, anchor)
                self.assertEqual(arrays["common_p2_delay_s"].size, 0)
                self.assertEqual(arrays["common_p2_power_norm"].size, 0)


if __name__ == "__main__":
    unittest.main()
