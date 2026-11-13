"""Txn 24 RF replay and synthetic full P2 retry under the operational host contract."""
from copy import deepcopy
import json
from pathlib import Path
import unittest
import zlib

import numpy as np

from analyze import ds_twr, packet_arrays
from capture_audit import (P1_FRESH_FIRMWARES, TICK_MODULUS, p1_selection,
                           p2_selection, probe2_response_ticks, verify_capture)
from rfb8 import CIR_BYTES, RAW_BYTES

HERE = Path(__file__).resolve().parent


def recorded_txn24():
    fixture = json.loads((HERE / "txn24_g6_rf_metadata.json").read_bytes())
    anchor, tag = fixture["anchor"], fixture["tag"]
    # Preserve recorded timestamps and frame bytes. Use independent synthetic IQ
    # for the CRC/axis test so the historical measurement does not enter the fixture.
    raw = bytes(range(256)) * (RAW_BYTES // 256)
    for slot, meta in enumerate(tag["cir"]):
        meta["crc32"] = zlib.crc32(raw[slot * CIR_BYTES:(slot + 1) * CIR_BYTES])
    return anchor, tag, raw


def six_timestamps(anchor, tag, radio):
    poll, response_rx, poll_rx, response_tx = probe2_response_ticks(anchor, tag, radio)
    _, final = p2_selection(anchor, tag)
    return [poll, response_rx, final["actual"], poll_rx, response_tx,
            tag["probe_rx"][2 + radio]["hw"]]


def timed_air(frame, *, config, **timestamps):
    offsets = {"poll_tx": 32, "resp_a": 37, "resp_b": 42, "probe_tx": 47, "poll_rx": 57, "response_tx": 62}
    data = bytearray.fromhex(frame)
    data[16:20] = config.to_bytes(4, "little")
    for name, tick in timestamps.items():
        pos = offsets[name]
        data[pos:pos + 5] = tick.to_bytes(5, "little")
    data[92:96] = zlib.crc32(data[:92]).to_bytes(4, "little")
    return data.hex()


def update_tx(event, actual):
    event.update(requested=actual, planned=actual, actual=actual)


def delayed_stamp(requested, antenna_delay):
    raw = ((requested - antenna_delay) % TICK_MODULUS) & ~511
    return (raw + antenna_delay) % TICK_MODULUS


def synthetic_fresh_exchange(anchor, tag, contract_revision=15):
    """New Poll/Response/Final events with ideal affine clocks and 128/160 DTU ToF."""
    anchor, tag = deepcopy(anchor), deepcopy(tag)
    contract = bytearray(40)
    contract[:3] = bytes((8, 1, 6))
    for i, value in enumerate((200000, 10000, 20000, 28000, 32000, 2200, 600)):
        contract[4 + i * 4:8 + i * 4] = value.to_bytes(4, "little")
    contract[32:] = bytes((5, 82, 3, 16, 0, 1, 3, contract_revision))
    config = zlib.crc32(contract)
    anchor.update(firmware="D8_ANCHOR_STAGE6_P2_FRESH_OPS_20261113", config=config, p2_retry=True,
                  p2_primary_tx=deepcopy(anchor["tx"][2]))
    tag.update(firmware="D8_TAG_STAGE6_P2_FRESH_20261113", config=config, p2_retry=True)
    anchor["p2_primary_frame"] = timed_air(anchor["frames"][2], config=config)
    del anchor["p2_repeat_tx"], anchor["p2_repeat_frame"], tag["p2_repeat_used"]
    for event in (anchor, tag):
        event["frames"][:2] = [timed_air(frame, config=config) for frame in event["frames"][:2]]
    anchor["p1_repeat_frame"] = timed_air(anchor["p1_repeat_frame"], config=config)
    for rx in (*anchor["response_rx"], *tag["poll_rx"], *tag["probe_rx"][:2]):
        rx["air"] = timed_air(rx["air"], config=config)
    origin = anchor["tx"][0]["actual"]
    anchor_delay = anchor["calibration"][0]["tx"]
    poll_tx = delayed_stamp(origin + 128000 * 638976 // 10, anchor_delay)
    final_tx = delayed_stamp(origin + 156000 * 638976 // 10, anchor_delay)
    old_poll_tx = anchor["poll2_tx"]["actual"]
    poll_air = timed_air(anchor["poll2_frame"], config=config, poll_tx=poll_tx)
    final_air = timed_air(anchor["frames"][2], config=config, probe_tx=final_tx, response_tx=poll_tx)
    anchor["poll2_frame"] = tag["poll2_frame"] = poll_air
    anchor["frames"][2] = tag["frames"][2] = final_air
    update_tx(anchor["poll2_tx"], poll_tx)
    update_tx(anchor["tx"][2], final_tx)
    for radio, tof in enumerate((128, 160)):
        old_poll = tag["poll2_rx"][radio]["hw"]
        # Keep the real A/B offset magnitudes. The ideal fixture holds the
        # clock ratio constant throughout the exchange.
        offset = (old_poll - old_poll_tx) % TICK_MODULUS
        poll_rx = (poll_tx + offset + tof) % TICK_MODULUS
        delay_us = (10000, 20000)[radio]
        response_tx = delayed_stamp(poll_rx + delay_us * 638976 // 10,
                                    tag["calibration"][radio]["tx"])
        turnaround = (response_tx - poll_rx) % TICK_MODULUS
        response_rx = (poll_tx + turnaround + 2 * tof) % TICK_MODULUS
        final_rx = (final_tx + offset + tof) % TICK_MODULUS
        tag["poll2_rx"][radio].update(
            hw=poll_rx, raw=(poll_rx + 15000) & ~511,
            air_crc=int.from_bytes(bytes.fromhex(poll_air)[92:], "little"))
        update_tx(tag["response2_tx"][radio], response_tx)
        rx = anchor["response2_rx"][radio]
        rx.update(hw=response_rx, raw=(response_rx + 15000) & ~511,
                  air=timed_air(rx["air"], config=config, poll_tx=poll_tx,
                                poll_rx=poll_rx, response_tx=response_tx))
        tag["probe_rx"][2 + radio].update(hw=final_rx, raw=(final_rx + 15000) & ~511, air=final_air)
    return anchor, tag


def synthetic_fresh_p1(anchor, tag):
    anchor, tag = synthetic_fresh_exchange(anchor, tag, contract_revision=16)
    anchor["firmware"], tag["firmware"] = P1_FRESH_FIRMWARES
    config = tag["config"]
    origin = anchor["tx"][0]["actual"]
    anchor_delay = anchor["calibration"][0]["tx"]
    poll_tx = delayed_stamp(origin + 42000 * 638976 // 10, anchor_delay)
    final_tx = delayed_stamp(poll_tx + 28000 * 638976 // 10, anchor_delay)
    poll = bytearray.fromhex(timed_air(anchor["frames"][0], config=config,
                                       poll_tx=poll_tx, probe_tx=origin))
    poll[24:28] = tag["boot"].to_bytes(4, "little")
    poll[30] = 2
    poll[92:] = zlib.crc32(poll[:92]).to_bytes(4, "little")
    anchor["p1_repeat_poll_frame"] = tag["p1_repeat_poll_frame"] = poll.hex()
    anchor["p1_repeat_poll_tx"] = deepcopy(anchor["tx"][0])
    update_tx(anchor["p1_repeat_poll_tx"], poll_tx)
    update_tx(anchor["p1_repeat_tx"], final_tx)
    tag.update(p1_repeat_used=True, p1_primary_rx_mask=1, p1_primary_cir_mask=1,
               p1_primary_frame=anchor["frames"][1], p1_repeat_poll_rx=deepcopy(tag["poll_rx"]),
               p1_repeat_response_tx=deepcopy(tag["tx"]))
    anchor["p1_repeat_responses"] = deepcopy(anchor["response_rx"])
    response_ticks = {}
    for radio, tof in enumerate((128, 160)):
        offset = (tag["poll_rx"][radio]["hw"] - origin) % TICK_MODULUS
        poll_rx = (poll_tx + offset + tof) % TICK_MODULUS
        turn = (radio + 1) * 10000 * 638976 // 10
        response_tx = delayed_stamp(poll_rx + turn, tag["calibration"][radio]["tx"])
        response_rx = (poll_tx + (response_tx - poll_rx) % TICK_MODULUS + 2 * tof) % TICK_MODULUS
        final_rx = (final_tx + offset + tof) % TICK_MODULUS
        tag["p1_repeat_poll_rx"][radio].update(hw=poll_rx, air=poll.hex())
        update_tx(tag["p1_repeat_response_tx"][radio], response_tx)
        rx = anchor["p1_repeat_responses"][radio]
        rx.update(hw=response_rx, air=timed_air(rx["air"], config=config, poll_tx=poll_tx,
                                                poll_rx=poll_rx, response_tx=response_tx))
        tag["probe_rx"][radio]["hw"] = final_rx
        tag["ranges"][radio].update(
            six_ts=[poll_tx, response_rx, final_tx, poll_rx, response_tx, final_rx],
            math_valid=True, d_m=tof * 299702547 / 63897600000)
        response_ticks["resp_a" if radio == 0 else "resp_b"] = response_rx
    final = timed_air(anchor["frames"][1], config=config, poll_tx=poll_tx, probe_tx=final_tx,
                      **response_ticks)
    anchor["p1_repeat_frame"] = tag["frames"][1] = final
    for rx in tag["probe_rx"][:2]:
        rx["air"] = final
    final2 = timed_air(anchor["frames"][2], config=config, poll_rx=final_tx)
    anchor["frames"][2] = tag["frames"][2] = final2
    anchor["p2_primary_frame"] = timed_air(anchor["p2_primary_frame"], config=config, poll_rx=final_tx)
    for rx in tag["probe_rx"][2:]:
        rx["air"] = final2
    return anchor, tag


def audit_single_pair(anchor, tag):
    anchor, tag = deepcopy(anchor), deepcopy(tag)
    def rebase_frames(value):
        if isinstance(value, list):
            return [rebase_frames(item) for item in value]
        if isinstance(value, dict):
            return {key: rebase_frames(item) for key, item in value.items()}
        if isinstance(value, str) and value.startswith("46434238") and len(value) == 192:
            frame = bytearray.fromhex(value)
            frame[12:16] = bytes(4)
            frame[92:] = zlib.crc32(frame[:92]).to_bytes(4, "little")
            return frame.hex()
        return value
    anchor, tag = rebase_frames(anchor), rebase_frames(tag)
    anchor["txn"] = tag["txn"] = 0
    crc = int.from_bytes(bytes.fromhex(tag["poll2_frame"])[92:], "little")
    for rx in tag["poll2_rx"]:
        rx["air_crc"] = crc
    tag["_raw_integrity_verified"] = True
    records = {item["role"]: [item] for item in (anchor, tag)}
    identities = {item["role"]: {**item, "config_id": item["config"], "hw_ok": True}
                  for item in (anchor, tag)}
    return verify_capture(session=tag["session"], count=1, stage=6, period_us=tag["period_us"],
                          identities=identities, records=records, stats={})


class P2FreshRetry(unittest.TestCase):
    def assert_common_grid(self, arrays, probe):
        axis = arrays[f"common_p{probe}_delay_s"]
        power = arrays[f"common_p{probe}_power_norm"]
        self.assertTrue(axis.size, f"P{probe} common axis missing")
        self.assertTrue(np.isfinite(axis).all())
        self.assertEqual(power.shape, (2, axis.size))
        self.assertTrue(np.any(np.isfinite(power).all(axis=0)))

    def test_recorded_txn24_final_only_repeat_loses_p2_grid(self):
        anchor, tag, raw = recorded_txn24()
        self.assertEqual((tag["session"], tag["txn"]), (1469613706, 24))
        self.assertEqual(tag["p2_primary_rx_mask"], 1)
        self.assertTrue(tag["p2_repeat_used"])
        self.assertFalse(tag["p2_retry"])
        six = six_timestamps(anchor, tag, radio=1)
        self.assertEqual(six, [600719628852, 601997582434, 603403328052,
                               507379089126, 508657040910, 510062783953])
        distance, valid = ds_twr(six)
        self.assertAlmostEqual(distance, -0.6669306653569785)
        self.assertFalse(valid)
        arrays, rows = packet_arrays(tag, raw, anchor)
        self.assert_common_grid(arrays, 1)
        self.assertTrue(rows[2]["axis_estimate_available"])
        self.assertTrue(rows[3]["cir_valid"])
        self.assertTrue(np.isfinite(arrays["relative_s"][3]).all())
        self.assertIsNone(rows[3]["range_m"])
        self.assertEqual(arrays["common_p2_delay_s"].size, 0)
        self.assertEqual(arrays["common_p2_power_norm"].size, 0)

    def test_fresh_poll2_and_both_response2_produce_axes(self):
        recorded_anchor, recorded_tag, raw = recorded_txn24()
        anchor, tag = synthetic_fresh_exchange(recorded_anchor, recorded_tag)
        arrays, rows = packet_arrays(tag, raw, anchor)
        for probe in (1, 2):
            self.assert_common_grid(arrays, probe)
        for radio, tof in enumerate((128, 160)):
            self.assertAlmostEqual(rows[radio]["range_m"], recorded_tag["ranges"][radio]["d_m"])
            old = six_timestamps(recorded_anchor, recorded_tag, radio)
            new = six_timestamps(anchor, tag, radio)
            self.assertTrue(all(a != b for a, b in zip(old, new)))
            self.assertAlmostEqual(rows[2 + radio]["range_m"], tof * 299702547 / 63897600000)

    def test_stale_response2_from_either_radio_cannot_create_common_axis(self):
        recorded_anchor, recorded_tag, raw = recorded_txn24()
        for radio in range(2):
            with self.subTest(radio=radio):
                anchor, tag = synthetic_fresh_exchange(recorded_anchor, recorded_tag)
                stale = recorded_anchor["response2_rx"][radio]
                # Recompute the RF CRC and use the active config so only the
                # stale POLL2/RESPONSE2 timing violates the retry contract.
                anchor["response2_rx"][radio] = {
                    **stale, "air": timed_air(stale["air"], config=anchor["config"])}
                self.assertIsNone(probe2_response_ticks(anchor, tag, radio))
                arrays, rows = packet_arrays(tag, raw, anchor)
                self.assertTrue(rows[2 + radio]["cir_valid"])
                self.assertEqual(arrays["common_p2_delay_s"].size, 0)
                self.assertEqual(arrays["common_p2_power_norm"].size, 0)


    def test_fresh_final_bound_to_old_poll2_cannot_create_common_axis(self):
        recorded_anchor, recorded_tag, raw = recorded_txn24()
        anchor, tag = synthetic_fresh_exchange(recorded_anchor, recorded_tag)
        wrong = timed_air(anchor["frames"][2], config=anchor["config"],
                          response_tx=recorded_anchor["poll2_tx"]["actual"])
        anchor["frames"][2] = tag["frames"][2] = wrong
        for rx in tag["probe_rx"][2:]:
            rx["air"] = wrong
        self.assertIsNone(p2_selection(anchor, tag))
        arrays, rows = packet_arrays(tag, raw, anchor)
        self.assertTrue(all(row["cir_valid"] for row in rows[2:]))
        self.assertEqual(arrays["common_p2_delay_s"].size, 0)
        self.assertEqual(arrays["common_p2_power_norm"].size, 0)


    def test_fresh_firmware_accepts_primary_p1_without_repeat_responses(self):
        original_anchor, original_tag, raw = recorded_txn24()
        anchor, tag = synthetic_fresh_p1(original_anchor, original_tag)
        anchor["p1_repeat_frame"] = None
        anchor["p1_repeat_tx"].update(ok=False, attempted=False)
        tag.update(p1_repeat_used=False, p1_primary_rx_mask=3, p1_primary_cir_mask=3)
        tag["frames"][1] = anchor["frames"][1]
        config = tag["config"]
        for slot in range(2):
            tag["probe_rx"][slot].update(hw=original_tag["probe_rx"][slot]["hw"],
                                         air=tag["frames"][1])
            tag["ranges"][slot] = deepcopy(original_tag["ranges"][slot])
        for field in ("p1_repeat_poll_frame", "p1_primary_frame",
                      "p1_repeat_poll_rx", "p1_repeat_response_tx"):
            del tag[field]
        anchor["p1_repeat_responses"] = [{"ok": False}, {"ok": False}]
        final2 = timed_air(anchor["frames"][2], config=config, poll_rx=0)
        anchor["frames"][2] = tag["frames"][2] = final2
        anchor["p2_primary_frame"] = timed_air(anchor["p2_primary_frame"], config=config, poll_rx=0)
        for rx in tag["probe_rx"][2:]:
            rx["air"] = final2
        self.assertIsNotNone(p1_selection(anchor, tag))
        arrays, _ = packet_arrays(tag, raw, anchor)
        for probe in (1, 2):
            self.assert_common_grid(arrays, probe)


    def test_fresh_p1_replaces_negative_final_only_range_with_fresh_ticks(self):
        old_ticks = [178104103476, 179382057450, 180787802676,
                     839652731263, 840930682894, 842336425212]
        old_range, valid = ds_twr(old_ticks)
        self.assertFalse(valid)
        self.assertAlmostEqual(old_range, -0.369319185, places=8)
        recorded_anchor, recorded_tag, raw = recorded_txn24()
        anchor, tag = synthetic_fresh_p1(recorded_anchor, recorded_tag)
        self.assertIsNotNone(p1_selection(anchor, tag))
        audit = audit_single_pair(anchor, tag)
        self.assertTrue(audit.passed, audit.problems)
        arrays, rows = packet_arrays(tag, raw, anchor)
        for probe in (1, 2):
            self.assert_common_grid(arrays, probe)
        for slot, tof in enumerate((128, 160, 128, 160)):
            self.assertAlmostEqual(rows[slot]["range_m"], tof * 299702547 / 63897600000)

    def test_fresh_p1_rejects_a_stale_response_but_keeps_independent_p2(self):
        recorded_anchor, recorded_tag, raw = recorded_txn24()
        for radio in range(2):
            with self.subTest(radio=radio):
                anchor, tag = synthetic_fresh_p1(recorded_anchor, recorded_tag)
                anchor["p1_repeat_responses"][radio] = deepcopy(anchor["response_rx"][radio])
                self.assertIsNone(p1_selection(anchor, tag))
                arrays, rows = packet_arrays(tag, raw, anchor)
                self.assertEqual(arrays["common_p1_delay_s"].size, 0)
                self.assertTrue(all(row["cir_valid"] for row in rows[:2]))
                self.assert_common_grid(arrays, 2)

    def test_fresh_p1_rejects_wrong_p2_confirmation_with_valid_crc(self):
        recorded_anchor, recorded_tag, raw = recorded_txn24()
        anchor, tag = synthetic_fresh_p1(recorded_anchor, recorded_tag)
        for name in ("frames", "p2_primary_frame"):
            old = anchor[name][2] if name == "frames" else anchor[name]
            new = timed_air(old, config=tag["config"], poll_rx=anchor["tx"][1]["actual"])
            if name == "frames":
                anchor["frames"][2] = tag["frames"][2] = new
                for rx in tag["probe_rx"][2:]:
                    rx["air"] = new
            else:
                anchor[name] = new
        self.assertIsNone(p1_selection(anchor, tag))
        arrays, _ = packet_arrays(tag, raw, anchor)
        self.assertEqual(arrays["common_p1_delay_s"].size, 0)
        self.assert_common_grid(arrays, 2)


if __name__ == "__main__":
    unittest.main()
