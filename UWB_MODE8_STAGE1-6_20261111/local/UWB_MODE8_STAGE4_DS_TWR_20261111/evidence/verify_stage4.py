"""Recompute Stage 4 distance from the two devices' RFB8 captures."""
from datetime import datetime, timezone
from fractions import Fraction
from hashlib import sha256
import argparse
import json
from math import isfinite
from pathlib import Path
from statistics import median
import sys
import zlib

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tag" / "tools"))
from capture_audit import anchor_tx_matches, response_matches, verify_capture
from rfb8 import Decoder
from run_pair import cir_payload_integrity

FIRMWARE = {"anchor": "D8_R6_ANCHOR_STAGE4_20261111", "tag": "D8_R5_TAG_STAGE4_20261111"}
COUNT = 20
PERIOD_US = 300000
TICKS_PER_SECOND = 63897600000
C_AIR = 299702547
MODULUS = 1 << 40
MAX_DELTA_M = 1e-6
TAG_FATAL_INTEGRITY = 1 | 64 | 256  # ErrHardware, ErrMetadata, ErrLate
RX_DONE_WITH_LDE = (1 << 10) | (1 << 13) | (1 << 14)
RX_FATAL_STATUS = sum(1 << bit for bit in (12, 15, 16, 18, 20, 24, 25))
CONTRACT = {
    "common_sha256": "a5cca7d8a8d5f20f4a89ea9b6509da0b8d7506626aac4a47fa1d27891ff5ebdb",
    "implementer_sha256": "2380fb1fe7302870a6c2d825cc2e65a73e91138677e095b4e51db43ab9ea90da",
}


def valid_rx(rx, radio, kind):
    status = rx.get("status", 0)
    return (rx.get("ok") is True and rx.get("lde") is True and rx.get("radio") == radio
            and rx.get("type") == kind and status & RX_DONE_WITH_LDE == RX_DONE_WITH_LDE
            and not status & RX_FATAL_STATUS)


def verify_range(anchor, tag, radio, expected_delay):
    poll_tx, p1_tx, p2_tx = anchor["tx"]
    resp_rx = anchor["response_rx"][radio]
    poll_rx = tag["poll_rx"][radio]
    resp_tx = tag["tx"][radio]
    p1_rx = tag["probe_rx"][radio]
    p2_hex = anchor["frames"][2]
    six = [poll_tx["actual"], resp_rx["hw"], p1_tx["actual"],
           poll_rx["hw"], resp_tx["actual"], p1_rx["hw"]]
    result = {"six_ts": six, "rx_raw_register_ticks": {
        "anchor_response": resp_rx["raw"], "tag_poll": poll_rx["raw"], "tag_probe1": p1_rx["raw"]}}
    reported = tag["ranges"][radio]
    calibration = tag["calibration"]
    tag_cal, anchor_cal = calibration[radio], anchor["calibration"][0]
    result.update(reported=reported, tag_calibration=tag_cal, anchor_calibration=anchor_cal)
    if (not all(type(t) is int and 0 <= t < MODULUS for t in six)
            or tag.get("p1_tx_confirmed") is not True or tag.get("metadata_conflict") is True
            or tag["errors"] & TAG_FATAL_INTEGRITY
            or not valid_rx(resp_rx, 0, 2 + radio) or not valid_rx(poll_rx, radio, 1)
            or not valid_rx(p1_rx, radio, 4)
            or not response_matches(resp_rx, resp_tx, poll_rx, tag, radio, six[0], six[1])
            or not resp_tx["ok"] or resp_tx["radio"] != radio or resp_tx["type"] != 2 + radio
            or resp_tx["planned"] != resp_tx["actual"] or not resp_tx["status"] & 0x80
            or poll_rx["air"] != anchor["frames"][0] or p1_rx["air"] != anchor["frames"][1]
            or not any(valid_rx(rx, i, 5) and rx["air"] == p2_hex
                       for i, rx in enumerate(tag["probe_rx"][2:4]))):
        return result | {"pass": False, "problem": "local RF timestamp provenance"}
    p1_air, p2_air = (bytes.fromhex(value) for value in anchor["frames"][1:3])
    p1_wire = [int.from_bytes(p1_air[offset:offset + 5], "little")
               for offset in (32, 37 + 5*radio, 47)]
    anchor_tx_ticks = (six[0], six[2], int.from_bytes(p2_air[47:52], "little"))
    if (p1_wire != six[:3] or int.from_bytes(p2_air[52:57], "little") != six[2]
            or p2_air[30] & 3 != 3 or p1_air[29] & (1 << radio) == 0
            or any(not anchor_tx_matches(tx, i, tick) or not tx["status"] & 0x80
                   for i, (tx, tick) in enumerate(zip(anchor["tx"], anchor_tx_ticks, strict=True)))
            or [tag_cal["tx"], tag_cal["rx"]] != [expected_delay, expected_delay]
            or [anchor_cal["tx"], anchor_cal["rx"]] != [16436, 16436]):
        return result | {"pass": False, "problem": "RF frame, hardware TX, or antenna delay mismatch"}
    ap, ar, af, tp, tr, tf = six
    intervals = [(end - start) % MODULUS for start, end in ((ap, ar), (tp, tr), (tr, tf), (ar, af))]
    result["intervals"] = dict(zip(("r1", "d1", "r2", "d2"), intervals, strict=True))
    if any(value <= 0 or value > TICKS_PER_SECOND // 2 for value in intervals):
        return result | {"pass": False, "problem": "invalid 40-bit interval"}
    r1, d1, r2, d2 = intervals
    tof = Fraction(r1*r2 - d1*d2, sum(intervals))
    distance = float(tof * Fraction(C_AIR, TICKS_PER_SECOND))
    claimed = reported["d_m"]
    delta = abs(distance - claimed) if type(claimed) in (int, float) and isfinite(claimed) else None
    match = (reported["radio"] == radio and reported["six_ts"] == six
             and [reported[key] for key in ("r1", "d1", "r2", "d2")] == intervals)
    problem = None
    if not 0 <= distance <= 300:
        problem = "distance outside firmware validity limits"
    elif not match or reported["math_valid"] is not True or delta is None or delta > MAX_DELTA_M:
        problem = "firmware/independent distance mismatch"
    return result | {"pass": problem is None, "problem": problem, "tof_ticks": float(tof),
                     "distance_m": distance, "abs_delta_m": delta, "firmware_inputs_match": match}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("capture", type=Path)
    parser.add_argument("--collector-exit", type=int, required=True)
    parser.add_argument("--tag-delay-receipt", type=Path)
    args = parser.parse_args()
    path = args.capture
    capture_receipt = (path / "capture_receipt.json").read_bytes()
    receipt = json.loads(capture_receipt)
    session = receipt["session"]
    delay_settings = None
    tag_delays = [16436, 16436]
    if args.tag_delay_receipt is not None:
        delay_bytes = args.tag_delay_receipt.read_bytes()
        delay_settings = json.loads(delay_bytes)
        delay_settings["receipt_sha256"] = sha256(delay_bytes).hexdigest()
        tag_delays = [delay_settings["modules"][r]["tag_delay_ticks"] for r in "AB"]
    identities = {role: receipt.get(role + "_identity", {}) for role in FIRMWARE}
    stats = {role: receipt.get(role + "_capture", {}) for role in FIRMWARE}
    config = zlib.crc32(bytes([8, 1, 4, 0]) +
                       b"".join(n.to_bytes(4, "little") for n in (300000, 10000, 20000, 28000, 32000, 2200, 600)) +
                       bytes([5, 82, 3, 16, 0, 1, 3, 4]))
    problems = []
    if receipt["stage"] != 4 or receipt["requested_count"] != COUNT or type(session) is not int or session <= 0:
        problems.append("stage, session, or transaction count mismatch")
    if delay_settings and delay_settings["baseline_session"] == session:
        problems.append("delay fit and verification share the same capture session")
    records, events, hashes, skipped_prefix = {}, {}, {}, {}
    for role, version in FIRMWARE.items():
        h = identities[role]
        if (h.get("firmware"), h.get("stage"), h.get("mode"), h.get("config_id"), h.get("hw_ok"),
                h.get("fault_latched")) != (version, 4, 8, config, True, False):
            problems.append(f"{role} HELLO mismatch")
        raw = (path / f"{role}.rfb8").read_bytes()
        hashes[role] = sha256(raw).hexdigest()
        first = raw.find(b"RFB8")
        skipped_prefix[role] = first
        decoder = Decoder()
        packets = decoder.feed(raw[max(0, first):])
        rows = [p.meta | {"_raw_integrity_verified": cir_payload_integrity(p.meta, p.raw)}
                for p in packets if p.kind == 2]
        events[role] = [p.meta for p in packets if p.kind == 1]
        records[role] = rows
        if first < 0 or any(decoder.statistics().values()):
            problems.append(f"{role} RFB8 stream integrity failure")
        if [m.get("txn") for m in rows] != list(range(COUNT)):
            problems.append(f"{role} missing, duplicate, or out-of-order records")
        peer = identities["tag" if role == "anchor" else "anchor"].get("boot")
        if any((m.get("role"), m.get("firmware"), m.get("stage"), m.get("session"), m.get("boot"),
                m.get("peer_boot"), m.get("config"), m.get("period_us"), m.get("cir_hz"))
               != (role, version, 4, session, h.get("boot"), peer, config, PERIOD_US, 20000000) for m in rows):
            problems.append(f"{role} record identity mismatch")
        delays = {"a": 16436} if role == "anchor" else dict(zip("ab", tag_delays, strict=True))
        if not any(e.get("event") == "SELFTEST" and e.get("pass") is True and e.get("role") == role
                   and e.get("dev_a") == 0xDECA0130 and (role == "anchor" or e.get("dev_b") == 0xDECA0130)
                   and all(e.get(f"ant_{letter}_{direction}") == ticks
                           for letter, ticks in delays.items() for direction in ("tx", "rx"))
                   for e in events[role]):
            problems.append(f"{role} SELFTEST or antenna delay readback missing or failed")
    audit = verify_capture(session=session, count=COUNT, stage=4, period_us=PERIOD_US,
                           identities=identities, records=records, stats=stats)
    pairs = {role: {m["txn"]: m for m in rows} for role, rows in records.items()}
    trials = []
    for txn in range(COUNT):
        if not all(txn in pairs[role] for role in FIRMWARE):
            continue
        anchor, tag = pairs["anchor"][txn], pairs["tag"][txn]
        trial = {"txn": txn, "anchor_errors": anchor["errors"], "tag_errors": tag["errors"], "modules": {}}
        for radio, letter in enumerate("AB"):
            try:
                trial["modules"][letter] = verify_range(anchor, tag, radio, tag_delays[radio])
            except (KeyError, ValueError, TypeError, IndexError) as error:
                trial["modules"][letter] = {"pass": False, "problem": f"malformed timestamp record: {error}"}
        trials.append(trial)
    summary = {}
    for letter in "AB":
        results = [trial["modules"][letter] for trial in trials]
        checked = [r["distance_m"] for r in results if "distance_m" in r]
        summary[letter] = {"valid": sum(r["pass"] for r in results), "recomputed": len(checked)}
        if checked:
            summary[letter].update(median_m=median(checked), min_m=min(checked), max_m=max(checked))
        summary[letter]["max_abs_delta_m"] = max((r["abs_delta_m"] for r in results
                                                   if r.get("abs_delta_m") is not None), default=None)
    passed = (not problems and audit.passed and args.collector_exit == 0 and receipt["pass"] is True
              and receipt["shutdown"]["pass"] is True and all(r["valid"] == COUNT for r in summary.values()))
    outcome = {
        "question_id": "MODE8-STAGE4-DS20", "utc": datetime.now(timezone.utc).isoformat(),
        "governance_sha256": CONTRACT, "stage": 4, "session": session, "config_id": config, "count": COUNT,
        "firmware": FIRMWARE, "input_sha256": hashes, "collector_receipt_sha256": sha256(capture_receipt).hexdigest(),
        "collector_exit_code": args.collector_exit, "pre_rfb8_serial_bytes": skipped_prefix,
        "period_us": PERIOD_US, "cir_hz": 20000000, "reference_m_approx": 0.8,
        "tag_delay_ticks": tag_delays, "delay_fit": delay_settings, "range_accuracy_validated": False,
        "max_allowed_abs_delta_m": MAX_DELTA_M, "result": summary,
        "source": "Local DW1000 TX_TIME.actual and RX_TIME.hw; RX_RAWST is preserved as diagnostic data.",
        "math": "r1=AR-AP, d1=TR-TP, r2=TF-TR, d2=AF-AR, each mod 2^40. ToF=(r1*r2-d1*d2)/(r1+d1+r2+d2).",
        "pass": passed, "problems": problems, "pair_audit_problems": audit.problems, "transactions": trials,
    }
    with (path / "stage4_distance_verdict.json").open("x", encoding="utf-8") as file:
        json.dump(outcome, file, indent=2, allow_nan=False)
    print(json.dumps({k: outcome[k] for k in ("session", "config_id", "pass", "result", "problems")}, indent=2))
    print("Pair audit problems:", audit.problems[:10], f"(total {len(audit.problems)})")
    return 0 if passed else 2


if __name__ == "__main__":
    sys.exit(main())
