"""Test R2 analysis and capture with controlled synthetic records and serial I/O."""

import copy
import json
from pathlib import Path
import sys
from tempfile import TemporaryDirectory
from types import SimpleNamespace
from unittest.mock import patch

import numpy as np


REPLAY = Path(__file__).resolve().parent
PACKAGE = REPLAY.parent / "UWB_DUAL_DW1000_MODE8_REAUDITED_20261111"
sys.path[:0] = [str(PACKAGE / "tools"), str(PACKAGE / "tests")]
from analyze import air_frame, packet_arrays
from capture_audit import verify_capture
from fixture_helpers import synthetic_anchor
from test_adversarial import GoodRunPairSerial, altered_frame
import run_pair


def identities(tag, anchor):
    return {m["role"]: {"role": m["role"], "hw_ok": True, "boot": m["boot"],
                        "config_id": m["config"], "stage": m["stage"],
                        "period_us": m["period_us"]} for m in (tag, anchor)}


def stage2_false_rejection(tag, raw):
    anchor = json.loads((REPLAY / "stage2_anchor.json").read_bytes())
    tag = copy.deepcopy(tag)
    tag.update(stage=2, config=anchor["config"], frames=anchor["frames"],
               p1_tx_confirmed=True, resp_mask=0)
    for rx in tag["poll_rx"]:
        rx["air"] = tag["frames"][0]
    for slot, rx in enumerate(tag["probe_rx"]):
        rx["air"] = tag["frames"][1 + slot // 2]
    for entry in tag["tx"]:
        entry.update(attempted=False, ok=False, actual=0)
    for entry in tag["ranges"]:
        entry.update(math_valid=False, d_m=None, six_ts=[0] * 6)
    tag["_raw_integrity_verified"] = run_pair.cir_payload_integrity(tag, raw)
    result = verify_capture(session=tag["session"], count=1, stage=2,
                            period_us=tag["period_us"], identities=identities(tag, anchor),
                            records={"tag": [tag], "anchor": [anchor]}, stats={})
    expected = {"ANCHOR_PROVENANCE_MISMATCH_TXN_0", "PAIR_RF_IDENTITY_MISMATCH_TXN_0_INDEX_1",
                "PAIR_RF_IDENTITY_MISMATCH_TXN_0_INDEX_2"}
    assert not result.passed and set(result.problems) == expected, result
    return {"anchor_complete": anchor["complete"], "tag_complete": tag["complete"],
            "anchor_peer_boot": anchor["peer_boot"], "tag_boot": tag["boot"],
            "p1_p2_peer_boot": [air_frame(f)["peer_boot"] for f in anchor["frames"][1:]],
            "pass": result.passed, "problems": result.problems}


def analyze_paths(tag, raw):
    anchor = synthetic_anchor(tag)
    mutated = copy.deepcopy(tag)
    shared = air_frame(mutated["frames"][2])
    mutated["probe_rx"][3]["air"] = altered_frame(
        mutated["frames"][2], 42, (shared["resp_rx"][1] + 256).to_bytes(5, "little"))
    mutated.update(metadata_conflict=True, p1_tx_confirmed=False, complete=False, errors=64)
    arrays, rows = packet_arrays(mutated, raw, anchor)
    assert rows[3]["range_m"] is None and not rows[3]["axis_estimate_available"]
    assert arrays["common_p2_delay_s"].size == 0 and np.isfinite(arrays["relative_s"][3]).all()
    assert np.array_equal(arrays["iq_int16"].astype("<i2").tobytes(), raw)

    wrong_anchor = copy.deepcopy(anchor)
    wrong_anchor.update(config=tag["config"] + 1, peer_boot=tag["boot"] + 1,
                        calibration=[{"id": 22}])
    tagged_cal = copy.deepcopy(tag)
    tagged_cal["ranges"][0]["cal_id"] = 11
    _, wrong_cal = packet_arrays(tagged_cal, raw, wrong_anchor)
    assert wrong_cal[0]["both_endpoints_calibrated"] is True
    assert wrong_cal[2]["range_m"] is None

    no_p2 = copy.deepcopy(tag)
    no_p2.update(complete=False, errors=32)
    no_p2_raw = raw[:len(raw)//2] + bytes(len(raw)//2)
    for slot in (2, 3):
        no_p2["probe_rx"][slot].update(ok=False, air=None)
        no_p2["cir"][slot].update(present=False, valid=False, bytes=0, crc32=0)
    arrays, no_p2_rows = packet_arrays(no_p2, no_p2_raw, anchor)
    assert all(r["axis_estimate_available"] for r in no_p2_rows[:2])
    assert arrays["common_p1_delay_s"].size > 0

    wrong_tx = copy.deepcopy(tag)
    wrong_tx["tx"][1].update(radio=0, type=2)
    _, wrong_tx_rows = packet_arrays(wrong_tx, raw, anchor)
    assert wrong_tx_rows[1]["range_m"] is None and wrong_tx_rows[3]["axis_estimate_available"]

    return {
        "M8-01_conflicting_p2_rejected_raw_relative_preserved": True,
        "M8-05_unmatched_anchor_calibration_reported_valid": wrong_cal[0]["both_endpoints_calibrated"],
        "both_p2_rx_missing_p1_reported_ranges_m": [row["range_m"] for row in no_p2_rows[:2]],
        "wrong_local_response_identity_p1_b_range_m": wrong_tx_rows[1]["range_m"],
        "wrong_local_response_identity_p2_b_range_m": wrong_tx_rows[3]["range_m"],
    }


class FinalStopFails(GoodRunPairSerial):
    attempts = []

    def write(self, data):
        if data.strip() == b"STOP":
            if getattr(self, "initial_stop", False):
                self.attempts.append(self.port)
                raise OSError("audit injection: final STOP write failed")
            self.initial_stop = True
        return super().write(data)


def stop_false_pass():
    with TemporaryDirectory(prefix="mode8-r2-stop-audit-") as tmp:
        argv = ["run_pair.py", "--anchor", "anchor", "--tag", "tag", "--out", tmp, "--count", "2"]
        with (patch.object(sys, "argv", argv),
              patch.dict(sys.modules, {"serial": SimpleNamespace(Serial=FinalStopFails)}),
              patch.object(run_pair.secrets, "randbelow", return_value=76),
              patch("builtins.print")):
            exit_code = run_pair.main()
        receipt = json.loads((Path(tmp) / "capture_receipt.json").read_bytes())
    assert sorted(FinalStopFails.attempts) == ["anchor", "tag"]
    assert exit_code == 0 and receipt["pass"] and "error" not in receipt
    return {"failed_final_stop_ports": FinalStopFails.attempts,
            "exit_code": exit_code, "pass": receipt["pass"], "error": receipt.get("error")}


if __name__ == "__main__":
    fixture = PACKAGE / "evidence" / "host"
    tag = json.loads((fixture / "synthetic_tag_record.json").read_bytes())
    raw = (fixture / "SYNTHETIC_CIR_i16.raw").read_bytes()
    print(json.dumps({"stage2_false_rejection": stage2_false_rejection(tag, raw),
                      "analysis": analyze_paths(tag, raw),
                      "stop_false_pass": stop_false_pass()}, indent=2))
