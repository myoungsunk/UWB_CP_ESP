#!/usr/bin/env python3
"""Export raw CIR, relative axes and explicitly qualified physical-delay ESTIMATES.
No A/B coherent phase alignment. No unmarked reuse of P1 distance for P2.
"""
from __future__ import annotations
import argparse
import csv
import json
from pathlib import Path
from fractions import Fraction
import zlib
import numpy as np
from rfb8 import read_file, Decoder, CIR_BYTES, RAW_BYTES

MASK40 = (1 << 40) - 1
HZ = 63897600000
C_AIR = 299702547.0
SAMPLE_S = 1.0 / 998400000


def ds_twr(six: list[int]) -> tuple[float, bool]:
    if len(six) != 6 or any(not isinstance(x, int) or x < 0 or x > MASK40 for x in six):
        return float("nan"), False
    ap, ar, af, tp, tr, tf = six
    r1, d1, r2, d2 = ((ar-ap) & MASK40, (tr-tp) & MASK40,
                     (tf-tr) & MASK40, (af-ar) & MASK40)
    if any(x <= 0 or x > HZ // 2 for x in (r1, d1, r2, d2)):
        return float("nan"), False
    # Exact integer products and rational division: independent of C++ double products.
    tof = Fraction(r1*r2-d1*d2, r1+r2+d1+d2)
    distance = float(tof / HZ) * C_AIR
    return distance, bool(np.isfinite(distance) and 0 <= distance <= 300)


def air_frame(hex_bytes: str | None) -> dict | None:
    if not hex_bytes:
        return None
    b = bytes.fromhex(hex_bytes)
    if len(b) != 96 or b[:4] != b"FCB8" or b[4] != 1:
        raise ValueError("Bad air frame")
    if int.from_bytes(b[92:], "little") != zlib.crc32(b[:92]) & 0xffffffff:
        raise ValueError("Bad air CRC")
    read = lambda offset, n=5: int.from_bytes(b[offset:offset+n], "little")
    return {"type": b[5], "session": read(8, 4), "txn": read(12, 4),
            "config": read(16, 4), "boot": read(20, 4), "peer_boot": read(24, 4),
            "mask": b[29], "tx_ok_mask": b[30], "poll_tx": read(32),
            "resp_rx": [read(37), read(42)], "probe_tx": read(47),
            "prev_probe_actual": read(52)}


def packet_arrays(meta: dict, raw: bytes, anchor: dict | None = None,
                  assume_static: bool = False, max_speed: float | None = None) -> tuple[dict, list[dict]]:
    if len(raw) != RAW_BYTES or len(meta.get("cir", [])) != 4:
        raise ValueError("Four fixed CIR slots required")
    iq = np.frombuffer(raw, dtype="<i2").reshape(4, 992, 2).copy()
    cir = iq[:, :, 0].astype(np.float64) + 1j*iq[:, :, 1]
    normalized = np.full_like(cir, np.nan + 1j*np.nan)
    relative = np.full((4, 992), np.nan)
    physical = np.full((4, 992), np.nan)
    valid = np.zeros(4, dtype=bool)
    calibrated = np.zeros(4, dtype=bool)
    rows: list[dict] = []
    k = np.arange(992, dtype=np.float64)
    frames = meta["frames"]
    p1, p2 = air_frame(frames[1]), air_frame(frames[2])
    for slot in range(4):
        radio, probe = slot % 2, slot // 2 + 1
        o, cm = meta["probe_rx"][slot], meta["cir"][slot]
        slotraw = raw[slot*CIR_BYTES:(slot+1)*CIR_BYTES]
        crc_ok = bool(cm["present"] and (zlib.crc32(slotraw) & 0xffffffff) == cm["crc32"])
        valid[slot] = bool(cm["valid"] and crc_ok and o["ok"])
        fp_ok = bool(o["lde"] and o["nacc"] > 0 and 0 <= o["fp_raw"] / 64 < 992)
        distance = np.nan
        range_ok = False
        method = "relative_only"
        fw = meta["ranges"][radio]
        if probe == 1 and fw["math_valid"] and meta["p1_tx_confirmed"]:
            distance, range_ok = ds_twr(fw["six_ts"])
            if not range_ok or abs(distance-fw["d_m"]) > 1e-6:
                raise ValueError("Firmware/independent DS calculation mismatch")
            method = "P1_DS_confirmed_by_P2"
        elif probe == 2 and anchor is not None and p2 is not None:
            tx = anchor["tx"][2]
            matching = (anchor["session"] == meta["session"] and anchor["txn"] == meta["txn"]
                        and anchor["boot"] == meta["peer_boot"] and anchor["peer_boot"] == meta["boot"]
                        and anchor["config"] == meta["config"])
            if matching and tx["ok"] and tx["actual"] == p2["probe_tx"] and p2["mask"] & (1 << radio):
                po, rt = meta["poll_rx"][radio], meta["tx"][radio]
                if po["ok"] and po["lde"] and rt["ok"] and o["ok"] and o["lde"]:
                    six = [p2["poll_tx"], p2["resp_rx"][radio], tx["actual"], po["hw"], rt["actual"], o["hw"]]
                    distance, range_ok = ds_twr(six)
                    method = "P2_DS_confirmed_by_anchor_log_correlated_with_P1"
        if probe == 2 and not range_ok and assume_static and fw["math_valid"] and meta["p1_tx_confirmed"]:
            distance, range_ok = ds_twr(fw["six_ts"])
            method = "P1_distance_reused_EXPLICIT_STATIC_ASSUMPTION"
        if valid[slot] and fp_ok:
            relative[slot] = (k-o["fp_raw"]/64)*SAMPLE_S
            normalized[slot] = cir[slot]/o["nacc"]
            if range_ok:
                physical[slot] = relative[slot] + distance/C_AIR
        anchor_cal = anchor.get("calibration", [{}])[0] if anchor else {}
        calibrated[slot] = bool(range_ok and fw.get("cal_id", 0) and anchor_cal.get("id", 0))
        # A documented uncertainty floor only; FP detection/NLoS/hardware/systematic terms are unknown.
        sigma = fw.get("range_sigma_m")
        sigma_ns = sigma/C_AIR*1e9 if sigma is not None and np.isfinite(sigma) else None
        movement = (max_speed * 0.032 if probe == 2 and "STATIC_ASSUMPTION" in method
                    and max_speed is not None else None)
        rows.append({"session": meta["session"], "txn": meta["txn"], "slot": slot,
                     "radio": "AB"[radio], "probe": probe, "cir_valid": bool(valid[slot]),
                     "crc_ok": crc_ok, "fp_index": o["fp_raw"]/64, "fp_basic_quality": fp_ok,
                     "range_m": float(distance) if range_ok else None, "axis_method": method,
                     "axis_estimate_available": bool(valid[slot] and fp_ok and range_ok),
                     "both_endpoints_calibrated": bool(calibrated[slot]),
                     "axis_not_ground_truth": True, "range_sigma_floor_ns": sigma_ns,
                     "unknown_fp_bias_and_covariance": True,
                     "static_reuse_motion_bound_m": movement,
                     "rx_dbm_raw": o.get("rx_dbm"), "fp_dbm_raw": o.get("fp_dbm")})
    arrays = {"iq_int16": iq, "cir_raw": cir, "cir_nacc_normalized": normalized,
              "raw_index": np.arange(992), "relative_s": relative, "physical_estimate_s": physical,
              "cir_valid": valid, "both_endpoints_calibrated": calibrated}
    # Resample POWER only onto a common coordinate grid; do not manufacture an
    # inter-radio phase reference by interpolating or rotating the complex CIR.
    for probe in (1, 2):
        pair = slice(2*(probe-1), 2*probe)
        axes = physical[pair]
        if np.isfinite(axes).all():
            lo, hi = axes.min(), axes.max()
            count = int(np.ceil((hi-lo)/SAMPLE_S))+1
            if count > 4096:
                raise ValueError("Unreasonable common-delay extent")
            grid = lo + np.arange(count)*SAMPLE_S
            power = np.abs(normalized[pair])**2
            interpolated = np.array([np.interp(grid, axes[i], power[i], left=np.nan, right=np.nan)
                                     for i in (0, 1)])
        else:
            grid = np.empty(0, dtype=float)
            interpolated = np.empty((2, 0), dtype=float)
        arrays[f"common_p{probe}_delay_s"] = grid
        arrays[f"common_p{probe}_power_norm"] = interpolated
    return arrays, rows


def main() -> None:
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("tag", type=Path)
    p.add_argument("--anchor", type=Path)
    p.add_argument("--out", type=Path, required=True)
    p.add_argument("--assume-static", action="store_true")
    p.add_argument("--max-speed-mps", type=float)
    args = p.parse_args()
    if args.max_speed_mps is not None and args.max_speed_mps < 0:
        p.error("max speed must be nonnegative")
    args.out.mkdir(parents=True, exist_ok=True)
    anchor_index = {}
    if args.anchor:
        for packet in read_file(args.anchor):
            m = packet.meta
            if packet.kind == 2 and m["role"] == "anchor":
                anchor_index[(m["session"], m["txn"], m["boot"])] = m
    decoder = Decoder()
    records = 0
    with (args.out / "observations.csv").open("w", newline="", encoding="utf-8-sig") as csvfile:
        writer = None
        for packet in read_file(args.tag, decoder):
            m = packet.meta
            if packet.kind != 2 or m["role"] != "tag":
                continue
            anchor = anchor_index.get((m["session"], m["txn"], m["peer_boot"]))
            arrays, rows = packet_arrays(m, packet.raw, anchor, args.assume_static, args.max_speed_mps)
            name = f"s{m['session']}_t{m['txn']}_b{m['boot']}.npz"
            np.savez_compressed(args.out / name, **arrays,
                                metadata_json=np.array(json.dumps(m, ensure_ascii=False)))
            if writer is None:
                writer = csv.DictWriter(csvfile, fieldnames=list(rows[0]))
                writer.writeheader()
            writer.writerows(rows)
            records += 1
    report = {"records": records, "decoder": decoder.statistics(),
              "assume_static": args.assume_static, "phase_coherent": False,
              "p2_without_anchor_log": "relative_only_unless_explicit_static_assumption",
              "warning": "Physical axes are estimates using DS-TWR and FP; not ground truth or clock synchronization."}
    (args.out/"analysis_receipt.json").write_text(json.dumps(report, indent=2), encoding="utf-8")
    print(json.dumps(report, indent=2))

if __name__ == "__main__":
    main()
