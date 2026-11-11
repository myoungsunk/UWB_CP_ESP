#!/usr/bin/env python3
"""Independent fail-closed final receipt check for a Mode-8 paired capture.

A record count is *not* proof of complete collection: bind every record to the
requested session, each board's actual HELLO boot ID, peer boot, monotonically
numbered unique transactions and the matching counterpart's RF frames.
"""
from __future__ import annotations
from dataclasses import dataclass
import struct
import math
from fractions import Fraction
import zlib


@dataclass(frozen=True)
class CaptureAudit:
    passed: bool
    problems: list[str]
    details: dict


def _air(value) -> bytes | None:
    if not isinstance(value, str):
        return None
    try:
        b = bytes.fromhex(value)
    except ValueError:
        return None
    if (len(b) != 96 or b[:4] != b'FCB8' or b[4] != 1 or
            struct.unpack_from('<H', b, 6)[0] != 96 or any(b[75:92]) or
            struct.unpack_from('<I', b, 92)[0] != zlib.crc32(b[:92]) & 0xffffffff):
        return None
    return b


def response_matches(rx: dict, tx: dict, poll_rx: dict, tag: dict, radio: int,
                     poll_tx: int, response_rx: int, *, second: bool = False) -> bool:
    """Bind the anchor's received RESPONSE to the tag's local POLL and TX."""
    air = _air(rx.get('air'))
    if air is None:
        return False
    return bool(
        rx.get('ok') is True and rx.get('lde') is True and rx.get('hw') == response_rx
        and air[5] == (6 if second else 2) + radio and air[28] == radio and air[31] == tag['stage']
        and struct.unpack_from('<5I', air, 8) ==
        (tag['session'], tag['txn'], tag['config'], tag['boot'], tag['peer_boot'])
        and int.from_bytes(air[32:37], 'little') == poll_tx
        and int.from_bytes(air[57:62], 'little') == poll_rx.get('hw')
        and int.from_bytes(air[62:67], 'little') == tx.get('actual'))


def probe2_response_ticks(anchor: dict, tag: dict, radio: int):
    """Bind the second exchange to its own independently confirmed POLL."""
    try:
        txs, rxs, polls = tag['response2_tx'], anchor['response2_rx'], tag['poll2_rx']
        if len(txs) != 2 or len(rxs) != 2 or len(polls) != 2:
            return None
        poll_air = _air(anchor['poll2_frame'])
        if (poll_air is None or _air(tag['poll2_frame']) != poll_air or poll_air[5] != 8
                or poll_air[28] != 255 or poll_air[31] != tag['stage'] or tag['stage'] not in (5, 6)
                or struct.unpack_from('<5I', poll_air, 8) !=
                (tag['session'], tag['txn'], tag['config'], tag['peer_boot'], tag['boot'])):
            return None
        origin = int.from_bytes(poll_air[32:37], 'little')
        sent = anchor['poll2_tx']
        retry = tag.get('p2_retry', False)
        if (type(retry) is not bool or anchor.get('p2_retry', False) is not retry
                or retry and tag['stage'] != 6):
            return None
        offset = tag['period_us']//2 + (28000 if retry else 0)
        target = anchor['tx'][0]['actual'] + offset * 638976//10
        if (sent.get('ok') is not True or sent.get('type') != 8 or sent.get('radio') != 0
                or sent.get('actual') != origin or sent.get('planned') != origin
                or not sent.get('status', 0) & 0x80 or (target - origin) % (1 << 40) > 511):
            return None
        tx, rx, poll = txs[radio], rxs[radio], polls[radio]
        hw, actual = rx.get('hw'), tx.get('actual')
        ticks = (origin, hw, poll.get('hw'), actual)
        if (not all(type(tick) is int and 0 <= tick < 1 << 40 for tick in ticks)
                or rx.get('radio') != 0 or rx.get('type') != 6+radio
                or tx.get('ok') is not True or tx.get('radio') != radio or tx.get('type') != 6+radio
                or tx.get('planned') != actual or not tx.get('status', 0) & 0x80
                or poll.get('ok') is not True or poll.get('lde') is not True
                or poll.get('radio') != radio or poll.get('type') != 8
                or poll.get('air_crc') != int.from_bytes(poll_air[92:96], 'little')
                or not response_matches(rx, tx, poll, tag, radio, origin, hw, second=True)):
            return None
        return ticks
    except (KeyError, IndexError, TypeError, ValueError, AttributeError):
        return None


def anchor_tx_matches(tx: dict, slot: int, rf_tick: int) -> bool:
    """Bind an anchor POLL/P1/P2 TX slot to the timestamp in its RF frame."""
    return (isinstance(tx, dict) and tx.get('ok') is True
            and type(tx.get('radio')) is int and tx['radio'] == 0
            and type(tx.get('type')) is int and tx['type'] == (1, 4, 5)[slot]
            and type(tx.get('planned')) is int and type(tx.get('actual')) is int
            and tx['planned'] == tx['actual'] == rf_tick)


def verify_capture(*, session: int, count: int, stage: int, period_us: int,
                   identities: dict, records: dict, stats: dict) -> CaptureAudit:
    errors = []
    detail = {}
    expected_roles = ('tag', 'anchor')
    if not 0 < session <= 0xffffffff or count < 1 or not 2 <= stage <= 6:
        errors.append('INVALID_REQUEST')
    try:
        config = identities['tag']['config_id']
        if not config or identities['anchor']['config_id'] != config:
            errors.append('IDENTITY_CONFIG_MISMATCH')
        boots = {role: identities[role]['boot'] for role in expected_roles}
        if any(type(boots[r]) is not int or not 0 < boots[r] <= 0xffffffff for r in expected_roles):
            errors.append('INVALID_IDENTITY_BOOT')
        for role in expected_roles:
            h = identities[role]
            if (h.get('role') != role or h.get('hw_ok') is not True or
                    h.get('fault_latched') is True or h.get('stage') != stage or
                    h.get('period_us') != period_us):
                errors.append(f'{role.upper()}_HELLO_MISMATCH')
    except (KeyError, TypeError, ValueError):
        config, boots = None, {}
        errors.append('MISSING_DEVICE_IDENTITIES')

    indexed = {}
    anchor_peer = 0 if stage == 2 else boots.get('tag')
    for role in expected_roles:
        expected_peer = boots.get('anchor') if role == 'tag' else anchor_peer
        rows = records.get(role, [])
        if len(rows) != count:
            errors.append(f'{role.upper()}_COUNT_EXPECTED_{count}_ACTUAL_{len(rows)}')
        indices = []
        lookup = {}
        for index, m in enumerate(rows):
            if not isinstance(m, dict):
                errors.append(f'{role.upper()}_RECORD_NOT_OBJECT_{index}')
                continue
            tx = m.get('txn')
            indices.append(tx)
            if type(tx) is not int or not 0 <= tx < count:
                errors.append(f'{role.upper()}_OUT_OF_RANGE_TXN_{index}')
                continue
            if tx in lookup:
                errors.append(f'{role.upper()}_DUPLICATE_TXN_{tx}')
            lookup[tx] = m
            if (m.get('schema') != 'dual8.record.v1' or m.get('mode') != 8 or
                    m.get('role') != role or m.get('session') != session or
                    m.get('boot') != boots.get(role) or
                    m.get('peer_boot') != expected_peer or
                    m.get('config') != config or m.get('stage') != stage or
                    m.get('period_us') != period_us):
                errors.append(f'{role.upper()}_PROVENANCE_MISMATCH_TXN_{tx}')
            if m.get('complete') is not True or m.get('errors') != 0 or m.get('metadata_conflict') is True:
                errors.append(f'{role.upper()}_INCOMPLETE_OR_ERROR_TXN_{tx}')
            if role == 'tag':
                # Each physical CIR slot belongs to exactly one radio/probe;
                # metadata may not relabel A/B while keeping the air CRC valid.
                cms = m.get('cir', [])
                if not isinstance(cms, list) or len(cms) != 4 or any(
                        not isinstance(c, dict) or c.get('slot') != s or
                        c.get('radio') != s % 2 or c.get('probe') != s // 2 + 1
                        for s, c in enumerate(cms)):
                    errors.append(f'TAG_CIR_SLOT_IDENTITY_TXN_{tx}')
                if m.get('_raw_integrity_verified') is not True:
                    errors.append(f'TAG_RAW_CIR_INTEGRITY_TXN_{tx}')
                if (stage >= 4 and (m.get('p1_tx_confirmed') is not True or
                                    len(m.get('ranges', [])) != 2 or
                                    not all(x.get('math_valid') is True for x in m['ranges']))):
                    errors.append(f'TAG_DS_INVALID_TXN_{tx}')
                if (len(m.get('cir', [])) != 4 or
                        not all(c.get('valid') is True for c in m['cir'])):
                    errors.append(f'TAG_CIR_INVALID_TXN_{tx}')
        # No hidden extra records, no duplicate, no gaps and no out-of-order txn.
        if indices != list(range(count)):
            errors.append(f'{role.upper()}_TXN_SEQUENCE_INVALID')
        if set(lookup) != set(range(count)):
            errors.append(f'{role.upper()}_TXN_COVERAGE_INVALID')
        indexed[role] = lookup
        ds = stats.get(role, {})
        stream = ds.get('capture_decoder', ds.get('decoder', {}))
        for field in ('crc_errors', 'schema_errors', 'sequence_gap_events', 'trailing_bytes', 'skipped_bytes'):
            if stream.get(field, 0):
                errors.append(f'{role.upper()}_STREAM_{field.upper()}')
        if ds.get('reader_error'):
            errors.append(f'{role.upper()}_READER_ERROR')
        if ds.get('fault_events'):
            errors.append(f'{role.upper()}_FAULT_EVENT')
        detail[role] = {'records': len(rows), 'unique_txn': len(lookup),
                        'received_txn_order': indices, 'decoder': stream}

    # Compare both endpoints' actual on-air Poll and Probe payloads, not merely
    # JSON counters. A valid RFB8 CRC can enclose inconsistent RF metadata.
    for txn in range(count):
        a = indexed.get('anchor', {}).get(txn)
        t = indexed.get('tag', {}).get(txn)
        if a is None or t is None:
            errors.append(f'MISSING_PAIR_TXN_{txn}')
            continue
        af, tf = a.get('frames'), t.get('frames')
        if not isinstance(af, list) or not isinstance(tf, list) or len(af) != 3 or len(tf) != 3:
            errors.append(f'PAIR_MISSING_FRAMES_TXN_{txn}')
            continue
        rf_frames = [_air(f) for f in af]
        for index, expected_type in enumerate((1, 4, 5)):
            ab, tb = rf_frames[index], _air(tf[index])
            if ab is None or ab != tb or tb[5] != expected_type:
                errors.append(f'PAIR_FRAME_MISMATCH_TXN_{txn}_INDEX_{index}')
            if ab is not None:
                # Wrapper identity alone is not trusted; cross-check each RF frame.
                if (ab[5] != expected_type or ab[31] != stage or
                        int.from_bytes(ab[8:12], 'little') != session or
                        int.from_bytes(ab[12:16], 'little') != txn or
                        int.from_bytes(ab[16:20], 'little') != config or
                        int.from_bytes(ab[20:24], 'little') != boots.get('anchor') or
                        int.from_bytes(ab[24:28], 'little') != (0 if index == 0 else anchor_peer)):
                    errors.append(f'PAIR_RF_IDENTITY_MISMATCH_TXN_{txn}_INDEX_{index}')
        if all(rf_frames):
            poll, p1, p2 = rf_frames
            if stage == 6 and ('p1_retry' in a or 'p1_retry' in t):
                retry = a.get('p1_retry')
                start = a.get('initial_poll_tx')
                selected = int.from_bytes(poll[32:37], 'little')
                if (type(retry) is not bool or t.get('p1_retry') is not retry or
                        type(start) is not int or not 0 <= start < 1 << 40 or
                        t.get('initial_poll_tx') != start or
                        poll[30] != int(retry) or
                        int.from_bytes(poll[47:52], 'little') != (start if retry else 0) or
                        (start + (28000 * 638976 // 10 if retry else 0) - selected) % (1 << 40) > (511 if retry else 0)):
                    errors.append(f'P1_RETRY_PROVENANCE_MISMATCH_TXN_{txn}')
            if stage == 2 and (p1[29] or p2[29] or any(p1[37:47]) or any(p2[37:47]) or
                               a.get('resp_mask') != 0 or t.get('resp_mask') != 0 or
                               len(t.get('tx', [])) != 2 or
                               any(tx.get('ok') or tx.get('attempted') for tx in t['tx']) or
                               len(a.get('response_rx', [])) != 2 or
                               any(rx.get('ok') for rx in a['response_rx'])):
                errors.append(f'STAGE2_UNEXPECTED_RESPONSE_TXN_{txn}')
            if (p1[32:37] != poll[32:37] or p2[32:37] != poll[32:37] or
                    p1[37:47] != p2[37:47] or p1[29] != p2[29] or
                    p2[52:57] != p1[47:52] or p2[30] & 3 != 3 or
                    (stage >= 3 and p1[29] != 3)):
                errors.append(f'PAIR_RF_ANCHOR_BLOCK_INCOHERENT_TXN_{txn}')
        # Independent endpoint timing evidence for every RF packet included.
        tt = a.get('tx', [])
        if not isinstance(tt, list) or len(tt) != 3:
            errors.append(f'ANCHOR_TX_SHAPE_TXN_{txn}')
        else:
            for index, field_off in ((0, 32), (1, 47), (2, 47)):
                frame = rf_frames[index]
                if frame is None or not anchor_tx_matches(
                        tt[index], index, int.from_bytes(frame[field_off:field_off+5], 'little')):
                    errors.append(f'ANCHOR_TX_TIMESTAMP_MISMATCH_TXN_{txn}_INDEX_{index}')
        if all(rf_frames) and stage >= 3:
            from_ranging = t.get('tx', [])
            receive_anchor = a.get('response_rx', [])
            if len(from_ranging) != 2 or len(receive_anchor) != 2 or len(t.get('poll_rx', [])) != 2:
                errors.append(f'RESPONSE_SHAPE_TXN_{txn}')
            else:
                p1 = rf_frames[1]
                poll_tx = int.from_bytes(p1[32:37], 'little')
                for radio in range(2):
                    arx = int.from_bytes(p1[37 + radio*5:42 + radio*5], 'little')
                    tx = from_ranging[radio]
                    if (tx.get('ok') is not True or tx.get('radio') != radio or
                            tx.get('type') != 2+radio or
                            receive_anchor[radio].get('radio') != 0 or
                            receive_anchor[radio].get('type') != 2+radio or
                            type(tx.get('actual')) is not int or
                            tx.get('actual') != tx.get('planned') or
                            not response_matches(receive_anchor[radio], tx, t['poll_rx'][radio],
                                                 t, radio, poll_tx, arx)):
                        errors.append(f'RESPONSE_TIMESTAMP_MISMATCH_TXN_{txn}_RADIO_{radio}')
        if stage in (5, 6) and all(rf_frames):
            for radio in range(2):
                if probe2_response_ticks(a, t, radio) is None:
                    errors.append(f'RESPONSE2_TIMESTAMP_MISMATCH_TXN_{txn}_RADIO_{radio}')
        if len(t.get('probe_rx', [])) != 4 or len(t.get('poll_rx', [])) != 2:
            errors.append(f'TAG_RECEIVE_SHAPE_TXN_{txn}')
            continue
        for slot in range(4):
            expected = _air(tf[1 + slot // 2])
            got = _air(t['probe_rx'][slot].get('air'))
            rx = t['probe_rx'][slot]
            if (rx.get('ok') is not True or rx.get('lde') is not True or
                    rx.get('radio') != slot % 2 or rx.get('type') != 4 + slot // 2 or
                    got is None or got != expected):
                errors.append(f'TAG_PROBE_AIR_MISMATCH_TXN_{txn}_SLOT_{slot}')
        for radio in range(2):
            got = _air(t['poll_rx'][radio].get('air'))
            rx = t['poll_rx'][radio]
            if (rx.get('ok') is not True or rx.get('lde') is not True or
                    type(rx.get('radio')) is not int or rx['radio'] != radio or
                    type(rx.get('type')) is not int or rx['type'] != 1 or
                    got is None or got != rf_frames[0]):
                errors.append(f'TAG_POLL_AIR_MISMATCH_TXN_{txn}_RADIO_{radio}')
        if stage >= 4 and all(rf_frames) and len(t.get('tx', [])) == 2:
            p1 = rf_frames[1]
            rf_poll = int.from_bytes(p1[32:37], 'little')
            rf_probe = int.from_bytes(p1[47:52], 'little')
            for radio in range(2):
                rr = t.get('ranges', [])
                if len(rr) != 2:
                    errors.append(f'TAG_RANGES_SHAPE_TXN_{txn}')
                    break
                expected = [rf_poll, int.from_bytes(p1[37+5*radio:42+5*radio], 'little'), rf_probe,
                            t['poll_rx'][radio].get('hw'), t['tx'][radio].get('actual'),
                            t['probe_rx'][radio].get('hw')]
                if (rr[radio].get('radio') != radio or rr[radio].get('six_ts') != expected or
                        not all(type(x) is int and 0 <= x <= (1 << 40)-1 for x in expected)):
                    errors.append(f'TAG_DS_TIMESTAMPS_MISMATCH_TXN_{txn}_RADIO_{radio}')
                    continue
                ap, ar, af, tp, tr, tfp = expected
                r1, d1, r2, d2 = ((ar-ap) & ((1 << 40)-1), (tr-tp) & ((1 << 40)-1),
                                   (tfp-tr) & ((1 << 40)-1), (af-ar) & ((1 << 40)-1))
                if any(v == 0 or v > 63897600000 // 2 for v in (r1, d1, r2, d2)):
                    errors.append(f'TAG_DS_INTERVAL_INVALID_TXN_{txn}_RADIO_{radio}')
                    continue
                independent = float(Fraction(r1*r2-d1*d2, r1+d1+r2+d2) /
                                    63897600000) * 299702547.0
                claimed = rr[radio].get('d_m')
                if (type(claimed) not in (int, float) or not math.isfinite(claimed) or
                        not math.isfinite(independent) or
                        not 0 <= independent <= 300 or abs(independent - claimed) > 1e-6):
                    errors.append(f'TAG_DS_RANGE_MISMATCH_TXN_{txn}_RADIO_{radio}')
    return CaptureAudit(not errors, sorted(set(errors)), detail)
