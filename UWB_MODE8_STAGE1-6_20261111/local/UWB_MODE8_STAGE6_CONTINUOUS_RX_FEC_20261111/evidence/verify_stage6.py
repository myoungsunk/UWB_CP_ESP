"""Verify live Mode 8 Stage 6 CIR grids, RFB8 continuity, and RF deadlines."""
import argparse
import csv
from datetime import datetime, timezone
from fractions import Fraction
from hashlib import sha256
import json
from pathlib import Path
import sys
import struct
import zlib

import numpy as np

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tag" / "tools"))
from rfb8 import Decoder, read_file
from uart_fragments import FragmentDecoder

def stage6_config_id(period_us):
    return zlib.crc32(bytes((8, 1, 6, 0)) +
                      struct.pack("<7I", period_us, 10000, 20000, 28000, 32000, 2200, 600) +
                      bytes((5, 82, 3, 16, 0, 1, 3, 9)))

TICKS_PER_SECOND = 63897600000
SAMPLE_S = 1 / 998400000
C_AIR = 299702547
MOD = 1 << 40
FIRMWARE = {"anchor": "D8_R6_ANCHOR_STAGE6_RX_FEC_20261111", "tag": "D8_R5_TAG_STAGE6_RX_FEC_20261111"}


def require(condition, reason):
    if not condition:
        raise ValueError(reason)


def range_from_endpoints(anchor, tag, probe, radio):
    ap = anchor["tx"][0] if probe == 1 else anchor["poll2_tx"]
    af = anchor["tx"][probe]
    ar = anchor["response_rx" if probe == 1 else "response2_rx"][radio]
    tp = tag["poll_rx"][radio] if probe == 1 else tag["poll2_rx"][radio]
    tr = tag["tx" if probe == 1 else "response2_tx"][radio]
    tf = tag["probe_rx"][2 * (probe - 1) + radio]
    require(all(tx["ok"] and tx["planned"] == tx["actual"] and tx["status"] & 0x80
                for tx in (ap, af, tr)), "hardware TX confirmation")
    require(all(rx["ok"] and rx["lde"] for rx in (ar, tp, tf)), "hardware RX confirmation")
    if probe == 1:
        require(tag["p1_tx_confirmed"], "P1 actual TX confirmation in P2")
    else:
        poll = bytes.fromhex(tag["poll2_frame"])
        require(len(poll) == 96 and poll[5] == 8 and poll == bytes.fromhex(anchor["poll2_frame"])
                and ap["type"] == 8 and ap["radio"] == 0
                and int.from_bytes(poll[32:37], "little") == ap["actual"]
                and zlib.crc32(poll[:92]) == int.from_bytes(poll[92:], "little") == tp["air_crc"]
                and (tp["type"], tp["radio"]) == (8, radio),
                "independently confirmed POLL2")
        air = bytes.fromhex(ar["air"])
        require(len(air) == 96 and air[5] == 6 + radio and air[28] == radio
                and zlib.crc32(air[:92]) == int.from_bytes(air[92:], "little")
                and int.from_bytes(air[32:37], "little") == ap["actual"]
                and int.from_bytes(air[57:62], "little") == tp["hw"]
                and int.from_bytes(air[62:67], "little") == tr["actual"],
                "fresh RESPONSE RF binding to POLL2 and the actual tag TX")
    ticks = [ap["actual"], ar["hw"], af["actual"], tp["hw"], tr["actual"], tf["hw"]]
    require(all(type(x) is int and 0 <= x < MOD for x in ticks), "40-bit tick bounds")
    ap, ar, af, tp, tr, tf = ticks
    intervals = [(ar-ap) % MOD, (tr-tp) % MOD, (tf-tr) % MOD, (af-ar) % MOD]
    require(all(0 < i <= TICKS_PER_SECOND // 2 for i in intervals), "DS-TWR interval bounds")
    r1, d1, r2, d2 = intervals
    distance = float(Fraction((r1*r2-d1*d2)*C_AIR, sum(intervals)*TICKS_PER_SECOND))
    return ticks, distance


def check_probe(anchor, tag, raw, probe, paired, tag_only, rows, tag_rows):
    slot0 = 2 * (probe - 1)
    delay = paired["physical_estimate_s"][slot0:slot0+2]
    ranges = []
    powers = []
    invalid_range = False
    for radio, letter in enumerate("AB"):
        slot = slot0 + radio
        o, cm = tag["probe_rx"][slot], tag["cir"][slot]
        payload = raw[slot*3968:(slot+1)*3968]
        iq = np.frombuffer(payload, dtype="<i2").reshape(992, 2)
        require((cm["slot"], cm["radio"], cm["probe"], o["radio"], o["type"])
                == (slot, radio, probe, radio, probe+3), "CIR slot mapping")
        require(cm["valid"] and cm["repeat_equal"] and cm["bytes"] == len(payload) == 3968
                and cm["crc32"] == zlib.crc32(payload), "CIR payload CRC and repeat read")
        np.testing.assert_array_equal(paired["iq_int16"][slot], iq)
        np.testing.assert_array_equal(tag_only["iq_int16"][slot], iq)
        require(0 <= o["fp_raw"] < 992*64 and o["nacc"] > 0, "FP/NACC diagnostic bounds")
        rel = (np.arange(992) - o["fp_raw"]/64) * SAMPLE_S
        np.testing.assert_allclose(paired["relative_s"][slot], rel, rtol=0, atol=2e-18)
        np.testing.assert_allclose(tag_only["relative_s"][slot], rel, rtol=0, atol=2e-18)
        six, distance = range_from_endpoints(anchor, tag, probe, radio)
        method = "P1_DS_confirmed_by_P2" if probe == 1 else "P2_DS_fresh_response_complete_anchor_log"
        row = rows[(tag["txn"], slot)]
        require(row["frame_provenance_ok"] == "True", "paired frame provenance")
        range_valid = 0 <= distance <= 300
        delta_m = None
        if range_valid:
            require(row["axis_method"] == method and row["axis_estimate_available"] == "True",
                    "paired axis method")
            delta_m = abs(float(row["range_m"]) - distance)
            require(delta_m <= 1e-6, "independent DS-TWR mismatch")
            np.testing.assert_allclose(delay[radio], rel + distance/C_AIR, rtol=0, atol=5e-17)
        else:
            invalid_range = True
            require(row["axis_method"] == "relative_only" and row["axis_estimate_available"] == "False"
                    and row["range_m"] == "" and np.isnan(delay[radio]).all(),
                    "out-of-bounds DS-TWR created a physical axis")
        tr = tag_rows[(tag["txn"], slot)]
        if probe == 1 and range_valid:
            require(tr["axis_method"] == method and tr["axis_estimate_available"] == "True",
                    "P1 unavailable in tag-only analysis")
            np.testing.assert_array_equal(tag_only["physical_estimate_s"][slot], delay[radio])
        else:
            require(tr["axis_method"] == "relative_only" and tr["axis_estimate_available"] == "False"
                    and np.isnan(tag_only["physical_estimate_s"][slot]).all(), "tag-only safety gate")
        ranges.append({"radio": letter, "six_ts": six, "signed_range_m": distance, "range_valid": range_valid,
                       "axis_distance_delta_m": delta_m, "fp_index": o["fp_raw"]/64})
        powers.append(np.sum(iq.astype(np.float64)**2, axis=1) / o["nacc"]**2)

    grid_key = f"common_p{probe}_delay_s"
    grid = paired[grid_key]
    power = paired[f"common_p{probe}_power_norm"]
    if probe == 2:
        require(tag_only[grid_key].size == 0, "tag-only P2 common grid was produced")
    if invalid_range:
        require(grid.size == power.size == tag_only[grid_key].size == 0,
                "out-of-bounds DS-TWR created a common grid")
        return {"pass": False, "cir_slots_verified": 2, "range": ranges,
                "problem": "DS-TWR distance outside bounds"}
    require(grid.ndim == 1 and grid.size >= 992 and power.shape == (2, grid.size),
            "common grid shape")
    np.testing.assert_allclose(np.diff(grid), SAMPLE_S, rtol=0, atol=2e-18)
    require(abs(grid[0] - delay.min()) <= 2e-18 and 0 <= grid[-1] - delay.max() + 2e-18 <= SAMPLE_S + 4e-18,
            "common grid coverage")
    for axis, samples, observed in zip(delay, powers, power, strict=True):
        valid = (grid >= axis[0]) & (grid <= axis[-1])
        np.testing.assert_array_equal(np.isfinite(observed), valid)
        x = (grid[valid] - axis[0]) / SAMPLE_S
        lower = np.clip(np.floor(x).astype(int), 0, 990)
        expected = samples[lower] + (x - lower) * (samples[lower+1] - samples[lower])
        np.testing.assert_allclose(observed[valid], expected, rtol=1e-8, atol=1e-8)
    overlap = int(np.isfinite(power).all(axis=0).sum())
    require(overlap > 0, "common power grid has no overlap")
    if probe == 1:
        np.testing.assert_array_equal(tag_only[grid_key], grid)
    return {"pass": True, "cir_slots_verified": 2, "range": ranges,
            "grid_points": int(grid.size), "overlap_points": overlap}


def read_rows(path, session, count):
    with path.open(encoding="utf-8-sig", newline="") as file:
        rows = list(csv.DictReader(file))
    keys = [(int(r["txn"]), int(r["slot"])) for r in rows]
    require(keys == [(txn, slot) for txn in range(count) for slot in range(4)]
            and all(int(r["session"]) == session for r in rows), "observations CSV coverage")
    return dict(zip(keys, rows, strict=True))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("capture", type=Path)
    parser.add_argument("--collector-exit", type=int, required=True)
    parser.add_argument("--tag-delay-receipt", type=Path, required=True)
    parser.add_argument("--period-us", type=int, required=True)
    parser.add_argument("--baud", type=int, choices=(921600, 2000000), required=True)
    parser.add_argument("--count", type=int, default=100)
    parser.add_argument("--verdict-name", default="stage6_verdict.json")
    args = parser.parse_args()
    out = args.capture
    config = stage6_config_id(args.period_us)
    count = args.count
    receipt = json.loads((out / "capture_receipt.json").read_bytes())
    settings = json.loads(args.tag_delay_receipt.read_bytes())
    session = receipt["session"]
    problems, packets = [], {}
    try:
        require(receipt["stage"] == 6 and receipt["requested_count"] == count, "capture stage and count")
        require(receipt["pass"] is True and receipt["audit"]["pass"] is True and args.collector_exit == 0,
                "paired capture integrity gate")
        require(receipt["shutdown"]["pass"] is True, "both STOP acknowledgements")
    except (ValueError, KeyError) as error:
        problems.append(str(error))
    transport = {}
    for role, firmware in FIRMWARE.items():
        messages = list(read_file(out / f"{role}.rfb8"))
        uart_stats = None
        if args.baud == 2000000:
            stream = FragmentDecoder()
            replayed = Decoder()
            replay_records = []
            with (out / f"{role}.uart").open("rb") as raw_wire:
                while data := raw_wire.read(65536):
                    replay_records.extend(p for p in replayed.feed(stream.feed(data)) if p.kind == 2)
            stored_records = [p for p in messages if p.kind == 2]
            if replay_records != stored_records or len(replay_records) != count:
                problems.append(f"{role} outer UART capture does not reproduce the stored RFB8 records")
            uart_stats = receipt[role + "_capture"].get("uart_fragment_transport", {})
            if (uart_stats.get("unrepairable_records") != 0 or uart_stats.get("native_crc_errors") != 0
                    or uart_stats.get("fragment_records", 0) < count):
                problems.append(f"{role} fragment transport has unrecovered or unverified records")

        packets[role] = {p.meta["txn"]: p for p in messages if p.kind == 2}
        identity = receipt.get(role + "_identity", {})
        if (identity.get("firmware"), identity.get("stage"), identity.get("config_id"),
                identity.get("period_us"), identity.get("baud")) != (
                firmware, 6, config, args.period_us, args.baud):
            problems.append(f"{role} HELLO identity")
        keys = [(p.meta["session"], p.meta["txn"], p.meta["firmware"]) for p in messages if p.kind == 2]
        if keys != [(session, txn, firmware) for txn in range(count)]:
            problems.append(f"{role} transaction identities")
        delays = {"a": 16436} if role == "anchor" else {
            r.lower(): settings["modules"][r]["tag_delay_ticks"] for r in "AB"}
        if not any(p.kind == 1 and p.meta.get("event") == "SELFTEST" and p.meta.get("pass") is True
                   and p.meta.get("dev_a") == 0xDECA0130 and (role == "anchor" or p.meta.get("dev_b") == 0xDECA0130)
                   and all(p.meta.get(f"ant_{r}_{d}") == ticks for r, ticks in delays.items() for d in ("tx", "rx"))
                   for p in messages):
            problems.append(f"{role} SELFTEST and antenna delay readback")
        counters = receipt[role + "_capture"]
        decoder = counters["capture_decoder"]
        status_file = out / f"{role}_post_status.json"
        status = json.loads(status_file.read_bytes()) if status_file.exists() else {}
        drops = status.get("drops", -1) - identity.get("drops", 0)
        sent = status.get("sent", -1) - identity.get("sent", 0)
        period_miss = status.get("period_miss", -1)
        durations = [m.meta["end_us"] - m.meta["begin_us"] for m in packets[role].values()]
        if (counters["records"] != count
                or counters["fault_events"] or counters["reader_error"] or counters["capture_overflow"]
                or any(decoder.get(k) != 0 for k in (
                    "skipped_bytes", "crc_errors", "schema_errors", "sequence_gap_events", "trailing_bytes"))):
            problems.append(f"{role} RFB8 continuity or record count")
        if counters["complete"] != count:
            problems.append(f"{role} incomplete RF transactions")
        if (status.get("boot") != identity.get("boot") or status.get("config_id") != config
                or status.get("stage") != 6 or status.get("period_us") != args.period_us
                or status.get("baud") != args.baud or status.get("active") is not False
                or status.get("fault_latched") is not False or status.get("hw_ok") is not True
                or sent != count or drops != 0 or period_miss != 0):
            problems.append(f"{role} post-run counters or identity")
        if len(durations) != count or any(not 0 < duration < args.period_us for duration in durations):
            problems.append(f"{role} transaction handling exceeds period")
        transport[role] = {"records": counters["records"], "complete": counters["complete"],
                           "decoder": decoder, "uart_fragment_transport": uart_stats, "sent_delta": sent, "drops_delta": drops,
                           "period_miss": period_miss, "fault_events": counters["fault_events"],
                           "deadline_latch_txns": [i for i, p in packets[role].items() if p.meta.get("rx_deadline_latch")],
                           "processing_us_min_max": [min(durations), max(durations)] if durations else None}
    anchors = [packets["anchor"][txn].meta for txn in range(count)]
    polls = [m["tx"][0]["actual"] for m in anchors]
    intervals_us = [((b-a) % MOD) * 1e6 / TICKS_PER_SECOND for a, b in zip(polls, polls[1:])]
    max_interval_error = max((abs(d-args.period_us) for d in intervals_us), default=float("inf"))
    # Firmware's deadline counter is separate from bounded RF scheduling jitter.
    if len(intervals_us) != count-1 or max_interval_error > 4000:
        problems.append("anchor on-air POLL cadence")
    cadence = {"expected_period_us": args.period_us, "interval_count": len(intervals_us),
               "interval_us_min_max": [min(intervals_us), max(intervals_us)] if intervals_us else None,
               "max_abs_interval_error_us": max_interval_error if intervals_us else None,
               "allowed_jitter_us": 4000,
               "first_to_last_poll_s": sum(intervals_us) / 1e6}
    paired_path, tag_path = out / "paired", out / "tag_only"
    rows, tag_rows = (read_rows(p / "observations.csv", session, count) for p in (paired_path, tag_path))
    for path in (paired_path, tag_path):
        analysis = json.loads((path / "analysis_receipt.json").read_bytes())
        if analysis["assume_static"] or analysis["phase_coherent"] or analysis["records"] != count:
            problems.append(f"{path.name} analysis settings")
    trials = []
    for txn in range(count):
        tag, anchor = (packets[r][txn] for r in ("tag", "anchor"))
        name = f"s{session}_t{txn}_b{tag.meta['boot']}.npz"
        probes = {}
        with np.load(paired_path / name) as paired, np.load(tag_path / name) as tag_only:
            for probe in (1, 2):
                try:
                    probes[f"P{probe}"] = check_probe(anchor.meta, tag.meta, tag.raw, probe,
                                                       paired, tag_only, rows, tag_rows)
                except (ValueError, AssertionError, KeyError) as error:
                    probes[f"P{probe}"] = {"pass": False, "problem": str(error).strip()[:550]}
        trials.append({"txn": txn, **probes})
    summary = {}
    for key in ("P1", "P2"):
        passed = [t[key] for t in trials if t[key]["pass"]]
        signed = {radio: [r["signed_range_m"] for t in trials for r in t[key].get("range", [])
                          if r["radio"] == radio] for radio in "AB"}
        summary[key] = {
            "common_grids_verified": len(passed),
            "CIR_slots_verified": sum(t[key].get("cir_slots_verified", 0) for t in trials),
            "signed_range_m_min_max": {r: [min(v), max(v)] for r, v in signed.items() if v},
            "overlap_points_min": min((p["overlap_points"] for p in passed), default=None),
            "overlap_points_max": max((p["overlap_points"] for p in passed), default=None),
            "max_independent_range_delta_m": max((r["axis_distance_delta_m"]
                                                   for p in passed for r in p["range"]), default=None),
        }
    passed = not problems and all(s["common_grids_verified"] == count for s in summary.values())
    inputs = ["tag.rfb8", "anchor.rfb8", "capture_receipt.json", "tag_post_status.json", "anchor_post_status.json"]
    if args.baud == 2000000:
        inputs.extend(("anchor.uart", "tag.uart"))
    evidence = {
        "question_id": "MODE8-STAGE6-FAST-RX-USB", "utc": datetime.now(timezone.utc).isoformat(),
        "pass": passed, "session": session, "stage": 6, "config_id": config, "count": count,
        "baud": args.baud, "transport": transport, "cadence": cadence,
        "firmware": FIRMWARE, "problems": problems, "pair_audit": receipt.get("audit"),
        "governance_sha256": {
            "common": "a5cca7d8a8d5f20f4a89ea9b6509da0b8d7506626aac4a47fa1d27891ff5ebdb",
            "implementer": "2380fb1fe7302870a6c2d825cc2e65a73e91138677e095b4e51db43ab9ea90da"},
        "collector_exit_code": args.collector_exit, "input_sha256": {
            name: sha256((out / name).read_bytes()).hexdigest() for name in inputs if (out / name).exists()},
        "tag_delay_settings": settings, "tag_delay_receipt_sha256": sha256(args.tag_delay_receipt.read_bytes()).hexdigest(),
        "phase_coherent": False, "absolute_axis_accuracy_validated": False,
        "summary": summary, "transactions": trials,
    }
    with (out / args.verdict_name).open("x", encoding="utf-8") as file:
        json.dump(evidence, file, indent=2, allow_nan=False)
    print(json.dumps({k: evidence[k] for k in ("session", "pass", "summary", "transport", "cadence", "problems")}, indent=2))
    for trial in trials:
        for key in ("P1", "P2"):
            if not trial[key]["pass"]:
                print(f"{trial['txn']} {key}: {trial[key]['problem']}")
    return 0 if passed else 2


if __name__ == "__main__":
    sys.exit(main())
