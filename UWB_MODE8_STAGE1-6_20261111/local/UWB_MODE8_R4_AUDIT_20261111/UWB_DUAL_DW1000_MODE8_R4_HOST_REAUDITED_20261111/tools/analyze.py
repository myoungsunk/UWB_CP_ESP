#!/usr/bin/env python3
"""Fail-closed Mode 8 CIR/DS analysis with per-slot RF provenance checks.

Raw CIR and FP-relative axes remain available for a received packet even when
transaction metadata is untrusted. Physical axes are NOT generated when the
captured P1/P2 frames, firmware time intervals, or anchor log disagree.
Do not infer phase coherence or treat a distance-based axis as ground truth.
"""
from __future__ import annotations
import argparse
import csv
import json
from pathlib import Path
from fractions import Fraction
import math
import zlib
import numpy as np
from rfb8 import read_file, Decoder, CIR_BYTES, RAW_BYTES
from capture_audit import response_matches

MASK40 = (1 << 40) - 1
HZ = 63897600000
C_AIR = 299702547.0
SAMPLE_S = 1.0 / 998400000


def ds_twr(six: list[int]) -> tuple[float, bool]:
    if len(six) != 6 or any(type(x) is not int or x < 0 or x > MASK40 for x in six):
        return float('nan'), False
    ap, ar, af, tp, tr, tf = six
    r1, d1, r2, d2 = ((ar-ap) & MASK40, (tr-tp) & MASK40,
                     (tf-tr) & MASK40, (af-ar) & MASK40)
    if any(x <= 0 or x > HZ // 2 for x in (r1, d1, r2, d2)):
        return float('nan'), False
    tof = Fraction(r1 * r2 - d1 * d2, r1 + r2 + d1 + d2)
    distance = float(tof / HZ) * C_AIR
    return distance, bool(np.isfinite(distance) and 0 <= distance <= 300)


def air_frame(hex_bytes: str | None) -> dict | None:
    if not hex_bytes:
        return None
    b = bytes.fromhex(hex_bytes)
    if (len(b) != 96 or b[:4] != b'FCB8' or b[4] != 1
            or int.from_bytes(b[6:8], 'little') != 96 or any(b[75:92])):
        raise ValueError('Bad air frame header/reserved bytes')
    if int.from_bytes(b[92:96], 'little') != zlib.crc32(b[:92]) & 0xffffffff:
        raise ValueError('Bad air CRC')
    read = lambda offset, n=5: int.from_bytes(b[offset:offset+n], 'little')
    return {'type': b[5], 'radio': b[28], 'stage': b[31],
            'session': read(8, 4), 'txn': read(12, 4),
            'config': read(16, 4), 'boot': read(20, 4), 'peer_boot': read(24, 4),
            'mask': b[29], 'tx_ok_mask': b[30], 'poll_tx': read(32),
            'resp_rx': [read(37), read(42)], 'probe_tx': read(47),
            'prev_probe_actual': read(52)}


def same_identity(f: dict | None, meta: dict, *, peer: bool) -> bool:
    return bool(f is not None and
                all(f.get(k) == meta.get(k) for k in ('session', 'txn', 'config', 'stage')) and
                f['boot'] == meta['peer_boot' if peer else 'boot'] and
                f['peer_boot'] in ((meta['boot'], 0) if peer else (meta['peer_boot'],)))


def anchor_matches(anchor: dict | None, meta: dict, p1: dict, p2: dict, radio: int) -> bool:
    """Require two-sided provenance, not just a plausible TX tick in a loose log."""
    if not anchor:
        return False
    try:
        if (anchor.get('role') != 'anchor' or anchor.get('metadata_conflict') is True or
                anchor.get('errors', 0) & (1 | 16 | 32 | 64) or
                any(anchor[k] != meta[k] for k in ('session', 'txn', 'config', 'stage')) or
                anchor['boot'] != meta['peer_boot'] or anchor['peer_boot'] != meta['boot']):
            return False
        tx = anchor['tx']
        if len(tx) < 3 or not all(tx[j]['ok'] for j in (0, 1, 2)):
            return False
        if (tx[0]['actual'] != p2['poll_tx'] or
                tx[1]['actual'] != p1['probe_tx'] or
                tx[2]['actual'] != p2['probe_tx']):
            return False
        rx = anchor['response_rx']
        if (len(rx) != 2 or rx[radio].get('radio') != 0 or
                rx[radio].get('type') != 2 + radio or
                not response_matches(rx[radio], meta['tx'][radio], meta['poll_rx'][radio],
                                     meta, radio, p2['poll_tx'], p2['resp_rx'][radio])):
            return False
        if not all(anchor['frames'][j] == meta['frames'][j] for j in range(3)):
            return False
        return True
    except (KeyError, IndexError, TypeError, ValueError):
        return False


def _slot_provenance(meta: dict, frames: list[dict | None], slot: int) -> bool:
    probe = slot // 2 + 1
    o = meta['probe_rx'][slot]
    if o.get('ok') is not True or not o.get('air') or not meta['frames'][probe]:
        return False
    f = air_frame(o['air'])
    cm = meta['cir'][slot]
    return bool(f == frames[probe] and meta['frames'][probe] == o['air'] and
                o.get('radio') == slot % 2 and o.get('type') == 3 + probe and
                cm.get('slot') == slot and cm.get('radio') == slot % 2 and
                cm.get('probe') == probe and
                f['type'] == (3 + probe) and same_identity(f, meta, peer=True) and
                f['peer_boot'] == (0 if meta['stage'] == 2 else meta['boot']))


def _anchor_block_agrees(p1: dict | None, p2: dict | None) -> bool:
    if not p1 or not p2:
        return False
    return bool(p1['type'] == 4 and p2['type'] == 5 and
                p1['poll_tx'] == p2['poll_tx'] and
                p1['resp_rx'] == p2['resp_rx'] and
                p1['mask'] == p2['mask'] and
                p1['peer_boot'] == p2['peer_boot'] and
                p2['prev_probe_actual'] == p1['probe_tx'] and
                p1['tx_ok_mask'] & 1 and p2['tx_ok_mask'] & 3 == 3)


def packet_arrays(meta: dict, raw: bytes, anchor: dict | None = None,
                  assume_static: bool = False, max_speed: float | None = None) -> tuple[dict, list[dict]]:
    if len(raw) != RAW_BYTES or len(meta.get('cir', [])) != 4:
        raise ValueError('Four fixed CIR slots required')
    if len(meta.get('probe_rx', [])) != 4 or len(meta.get('poll_rx', [])) != 2 or len(meta.get('ranges', [])) != 2:
        raise ValueError('Incorrect receiver/DS data shapes')
    if len(meta.get('tx', [])) != 2 or len(meta.get('frames', [])) != 3:
        raise ValueError('Incorrect TX/frame data shapes')
    if max_speed is not None and (not math.isfinite(max_speed) or max_speed < 0):
        raise ValueError('max_speed must be finite and nonnegative')
    if (meta.get('role') != 'tag' or meta.get('mode') != 8 or
            meta.get('schema') != 'dual8.record.v1'):
        raise ValueError('Unexpected tag record')
    iq = np.frombuffer(raw, dtype='<i2').reshape(4, 992, 2).copy()
    cir = iq[:, :, 0].astype(np.float64) + 1j * iq[:, :, 1]
    normalized = np.full_like(cir, np.nan + 1j * np.nan)
    relative = np.full((4, 992), np.nan)
    physical = np.full((4, 992), np.nan)
    valid = np.zeros(4, dtype=bool)
    calibrated = np.zeros(4, dtype=bool)
    rows: list[dict] = []
    k = np.arange(992, dtype=np.float64)
    frames = [air_frame(x) for x in meta['frames']]
    poll, p1, p2 = frames
    poll_ok = bool(poll and poll['type'] == 1 and same_identity(poll, meta, peer=True)
                   and all(o.get('radio') == i and o.get('type') == 1 and
                           o.get('air') == meta['frames'][0]
                           for i, o in enumerate(meta['poll_rx'])))
    p1_ok = bool(p1 and p1['type'] == 4 and same_identity(p1, meta, peer=True) and
                 p1['peer_boot'] == meta['boot'])
    p2_ok = bool(p2 and p2['type'] == 5 and same_identity(p2, meta, peer=True) and
                 p2['peer_boot'] == meta['boot'])
    coherent = bool(poll_ok and p1_ok and p2_ok and _anchor_block_agrees(p1, p2) and
                    p1['poll_tx'] == poll['poll_tx'] and
                    meta.get('p1_tx_confirmed') is True and
                    meta.get('metadata_conflict') is False and not meta.get('errors', 0) & (1 | 64 | 256))
    slot_ok = [_slot_provenance(meta, frames, s) for s in range(4)]
    # Even when firmware neglected its conflict flag, a mismatched received RF
    # frame makes the whole transaction's cross-radio physical timing untrusted.
    all_received_match = all(not meta['probe_rx'][s].get('ok') or slot_ok[s] for s in range(4))
    coherent = coherent and all_received_match and any(slot_ok[2:])
    local_response_ok = []
    for radio, (po, tx) in enumerate(zip(meta['poll_rx'], meta['tx'])):
        actual = tx.get('actual')
        local_response_ok.append(bool(
            po.get('ok') is True and po.get('lde') is True and po.get('air') == meta['frames'][0]
            and tx.get('ok') is True and tx.get('radio') == radio and tx.get('type') == 2 + radio
            and type(actual) is int and 0 <= actual <= MASK40 and actual == tx.get('planned')))
    matched_anchor = [bool(coherent and local_response_ok[radio] and
                           anchor_matches(anchor, meta, p1, p2, radio)) for radio in range(2)]

    # P1 DS is valid only if its timestamp evidence is the same as the P1/P2
    # over-air frames and local hardware events. Never trust math_valid alone.
    p1_results: list[tuple[float, bool]] = []
    for radio in range(2):
        fw = meta['ranges'][radio]
        po, tx, rx = meta['poll_rx'][radio], meta['tx'][radio], meta['probe_rx'][radio]
        expected = ([p1['poll_tx'], p1['resp_rx'][radio], p1['probe_tx'],
                     po.get('hw'), tx.get('actual'), rx.get('hw')] if coherent else None)
        evidence_ok = bool(coherent and slot_ok[radio] and fw.get('math_valid') is True and
                           p1['mask'] & (1 << radio) and
                           local_response_ok[radio] and
                           rx.get('ok') and rx.get('lde') and
                           fw.get('radio') == radio and fw.get('six_ts') == expected)
        if evidence_ok:
            d, ok = ds_twr(expected)
            claimed = fw.get('d_m')
            if ok and (type(claimed) not in (int, float) or not math.isfinite(claimed)
                       or abs(d - claimed) > 1e-6):
                raise ValueError('Firmware/independent P1 DS calculation mismatch')
            p1_results.append((d, ok))
        else:
            p1_results.append((float('nan'), False))

    for slot in range(4):
        radio, probe = slot % 2, slot // 2 + 1
        o, cm = meta['probe_rx'][slot], meta['cir'][slot]
        slotraw = raw[slot * CIR_BYTES:(slot + 1) * CIR_BYTES]
        crc_ok = bool(cm.get('present') and cm.get('bytes') == CIR_BYTES and
                      (zlib.crc32(slotraw) & 0xffffffff) == cm.get('crc32'))
        valid[slot] = bool(cm.get('valid') and crc_ok and o.get('ok') and
                           cm.get('slot') == slot and cm.get('radio') == radio and
                           cm.get('probe') == probe and
                           o.get('radio') == radio and o.get('type') == probe + 3)
        fp_raw = o.get('fp_raw', -1)
        nacc = o.get('nacc', 0)
        fp_ok = bool(o.get('lde') and type(fp_raw) is int and type(nacc) is int and
                     nacc > 0 and 0 <= fp_raw / 64 < 992)
        distance = float('nan')
        range_ok = False
        method = 'relative_only'
        fw = meta['ranges'][radio]
        if valid[slot] and fp_ok and slot_ok[slot] and coherent:
            if probe == 1:
                distance, range_ok = p1_results[radio]
                if range_ok:
                    method = 'P1_DS_confirmed_by_P2'
            elif p2['mask'] & (1 << radio) and matched_anchor[radio]:
                po, rt = meta['poll_rx'][radio], meta['tx'][radio]
                if o.get('lde') is True:
                    six = [p2['poll_tx'], p2['resp_rx'][radio], p2['probe_tx'],
                           po['hw'], rt['actual'], o['hw']]
                    distance, range_ok = ds_twr(six)
                    if range_ok:
                        method = 'P2_DS_confirmed_by_complete_anchor_log'
            if probe == 2 and not range_ok and assume_static and p1_results[radio][1]:
                distance, range_ok = p1_results[radio]
                method = 'P1_distance_reused_EXPLICIT_STATIC_ASSUMPTION'
        if valid[slot] and fp_ok:
            relative[slot] = (k - fp_raw / 64) * SAMPLE_S
            normalized[slot] = cir[slot] / nacc
            if range_ok:
                physical[slot] = relative[slot] + distance / C_AIR
        acal = (anchor.get('calibration') or [{}])[0] if matched_anchor[radio] else {}
        tag_cal = meta.get('calibration', [])
        tag_cal_id = tag_cal[radio].get('id') if len(tag_cal) > radio else 0
        calibrated[slot] = bool(range_ok and tag_cal_id and fw.get('cal_id') == tag_cal_id and acal.get('id'))
        sigma = fw.get('range_sigma_m')
        sigma_ns = sigma / C_AIR * 1e9 if type(sigma) in (int, float) and math.isfinite(sigma) else None
        gap_s = ((p2['probe_tx'] - p1['probe_tx']) & MASK40) / HZ if coherent else None
        movement = (max_speed * gap_s if probe == 2 and 'STATIC_ASSUMPTION' in method
                    and max_speed is not None and gap_s is not None else None)
        rows.append({'session': meta['session'], 'txn': meta['txn'], 'slot': slot,
                     'radio': 'AB'[radio], 'probe': probe, 'cir_valid': bool(valid[slot]),
                     'crc_ok': crc_ok, 'frame_provenance_ok': bool(slot_ok[slot]),
                     'transaction_coherent': bool(coherent),
                     'fp_index': fp_raw / 64 if fp_ok else None,
                     'fp_basic_quality': fp_ok,
                     'range_m': float(distance) if range_ok else None, 'axis_method': method,
                     'axis_estimate_available': bool(valid[slot] and fp_ok and range_ok),
                     'both_endpoints_calibrated': bool(calibrated[slot]),
                     'axis_not_ground_truth': True, 'range_sigma_floor_ns': sigma_ns,
                     'unknown_fp_bias_and_covariance': True,
                     'static_reuse_motion_bound_m': movement,
                     'rx_dbm_raw': o.get('rx_dbm'), 'fp_dbm_raw': o.get('fp_dbm')})
    arrays = {'iq_int16': iq, 'cir_raw': cir, 'cir_nacc_normalized': normalized,
              'raw_index': np.arange(992), 'relative_s': relative, 'physical_estimate_s': physical,
              'cir_valid': valid, 'both_endpoints_calibrated': calibrated}
    for probe in (1, 2):
        pair = slice(2 * (probe - 1), 2 * probe)
        axes = physical[pair]
        if np.isfinite(axes).all():
            lo, hi = axes.min(), axes.max()
            count = int(np.ceil((hi - lo) / SAMPLE_S)) + 1
            if count > 4096:
                raise ValueError('Unreasonable common-delay extent')
            grid = lo + np.arange(count) * SAMPLE_S
            power = np.abs(normalized[pair]) ** 2
            interpolated = np.array([np.interp(grid, axes[i], power[i], left=np.nan, right=np.nan)
                                     for i in (0, 1)])
        else:
            grid = np.empty(0, dtype=float)
            interpolated = np.empty((2, 0), dtype=float)
        arrays[f'common_p{probe}_delay_s'] = grid
        arrays[f'common_p{probe}_power_norm'] = interpolated
    return arrays, rows


def main() -> None:
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('tag', type=Path)
    p.add_argument('--anchor', type=Path)
    p.add_argument('--out', type=Path, required=True)
    p.add_argument('--assume-static', action='store_true')
    p.add_argument('--max-speed-mps', type=float)
    args = p.parse_args()
    if args.max_speed_mps is not None and (not math.isfinite(args.max_speed_mps) or args.max_speed_mps < 0):
        p.error('max speed must be finite and nonnegative')
    args.out.mkdir(parents=True, exist_ok=True)
    anchor_index = {}
    anchor_decoder = Decoder()
    seen_anchor_transactions = set()
    if args.anchor:
        for packet in read_file(args.anchor, anchor_decoder):
            m = packet.meta
            if packet.kind == 2 and m['role'] == 'anchor':
                key = (m['session'], m['txn'], m['boot'], m['peer_boot'])
                tx_key = (m['session'], m['txn'])
                if tx_key in seen_anchor_transactions:
                    raise ValueError(f'Duplicate anchor transaction {tx_key}; refuse ambiguous join')
                seen_anchor_transactions.add(tx_key)
                anchor_index[key] = m
        astats = anchor_decoder.statistics()
        if any(astats[k] for k in ('crc_errors', 'schema_errors', 'sequence_gap_events', 'trailing_bytes')):
            raise ValueError(f'Anchor RFB8 integrity incomplete: {astats}')
    decoder = Decoder()
    records = 0
    seen = set()
    with (args.out / 'observations.csv').open('w', newline='', encoding='utf-8-sig') as csvfile:
        writer = None
        for packet in read_file(args.tag, decoder):
            m = packet.meta
            if packet.kind != 2 or m['role'] != 'tag':
                continue
            key = (m['session'], m['txn'], m['boot'], m['peer_boot'])
            tx_key = (m['session'], m['txn'])
            if tx_key in seen:
                raise ValueError(f'Duplicate tag transaction {tx_key}; refuse ambiguous analysis')
            seen.add(tx_key)
            anchor = anchor_index.get((m['session'], m['txn'], m['peer_boot'], m['boot']))
            arrays, rows = packet_arrays(m, packet.raw, anchor, args.assume_static, args.max_speed_mps)
            name = f"s{m['session']}_t{m['txn']}_b{m['boot']}.npz"
            np.savez_compressed(args.out / name, **arrays,
                                metadata_json=np.array(json.dumps(m, ensure_ascii=False)))
            if writer is None:
                writer = csv.DictWriter(csvfile, fieldnames=list(rows[0]))
                writer.writeheader()
            writer.writerows(rows)
            records += 1
    stats = decoder.statistics()
    if any(stats[k] for k in ('crc_errors', 'schema_errors', 'sequence_gap_events', 'trailing_bytes')):
        raise ValueError(f'Tag RFB8 integrity incomplete: {stats}')
    report = {'records': records, 'decoder': stats,
              'anchor_decoder': anchor_decoder.statistics() if args.anchor else None,
              'collection_completeness_verified': False,
              'assume_static': args.assume_static, 'phase_coherent': False,
              'p2_without_anchor_log': 'relative_only_unless_explicit_static_assumption',
              'warning': 'Physical axes are estimates using DS-TWR and FP; not ground truth or clock synchronization.'}
    (args.out / 'analysis_receipt.json').write_text(json.dumps(report, indent=2), encoding='utf-8')
    print(json.dumps(report, indent=2))


if __name__ == '__main__':
    main()
