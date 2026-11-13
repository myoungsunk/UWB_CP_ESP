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
import re
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


P2_REPEAT_ANCHOR = "D8_ANCHOR_STAGE6_P2_REPEAT_20261112"
P2_REPEAT_TAG = "D8_TAG_STAGE6_P2_REPEAT_20261112"
P1_REPEAT_ANCHOR = "D8_ANCHOR_STAGE6_P1_REPEAT_20261113"
P1_REPEAT_TAG = "D8_TAG_STAGE6_P1_REPEAT_20261113"
P1_REPEAT_FIRMWARES = (P1_REPEAT_ANCHOR, P1_REPEAT_TAG)
DRAIN_FIRMWARES = ("D8_ANCHOR_STAGE6_P1_DRAIN_20261113", "D8_TAG_STAGE6_P1_DRAIN_20261113")
OPERATIONAL_DRAIN_FIRMWARES = ("D8_ANCHOR_STAGE6_P1_DRAIN_OPS_20261113", DRAIN_FIRMWARES[1])
P1_REPEAT_PAIRS = (P1_REPEAT_FIRMWARES, DRAIN_FIRMWARES, OPERATIONAL_DRAIN_FIRMWARES)
REPEAT_PAIRS = ((P2_REPEAT_ANCHOR, P2_REPEAT_TAG), *P1_REPEAT_PAIRS)
P2_REPEAT_FIRMWARES = tuple(firmware for pair in REPEAT_PAIRS for firmware in pair)
TICK_MODULUS = 1 << 40


def p1_repeat_contract(anchor: dict, tag: dict) -> bool:
    return (anchor.get('stage') == 6 or tag.get('stage') == 6) and (
        any(anchor.get('firmware') in pair or tag.get('firmware') in pair for pair in P1_REPEAT_PAIRS) or
        'p1_repeat_tx' in anchor or 'p1_repeat_frame' in anchor or
        any(key in tag for key in ('p1_repeat_used', 'p1_primary_rx_mask', 'p1_primary_cir_mask')))


def p2_repeat_contract(anchor: dict, tag: dict) -> bool:
    return (anchor.get('stage') == 6 or tag.get('stage') == 6) and (
        anchor.get('firmware') in P2_REPEAT_FIRMWARES or
        tag.get('firmware') in P2_REPEAT_FIRMWARES or
        'p2_repeat_tx' in anchor or 'p2_repeat_frame' in anchor or
        any(key in tag for key in ('p2_repeat_used', 'p2_primary_rx_mask', 'p2_primary_cir_mask')))


def _probe2_origin(anchor: dict, tag: dict):
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
    if p2_repeat_contract(anchor, tag):
        offset -= 14000
    target = anchor['tx'][0]['actual'] + offset * 638976//10
    if (sent.get('ok') is not True or sent.get('type') != 8 or sent.get('radio') != 0
            or sent.get('actual') != origin or sent.get('planned') != origin
            or not sent.get('status', 0) & 0x80 or (target - origin) % TICK_MODULUS > 511):
        return None
    return origin, poll_air


def probe2_response_ticks(anchor: dict, tag: dict, radio: int):
    """Bind the second exchange to its own independently confirmed POLL."""
    try:
        txs, rxs, polls = tag['response2_tx'], anchor['response2_rx'], tag['poll2_rx']
        if len(txs) != 2 or len(rxs) != 2 or len(polls) != 2:
            return None
        origin_and_air = _probe2_origin(anchor, tag)
        if origin_and_air is None:
            return None
        origin, poll_air = origin_and_air
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


def _p1_tx_at(tx, frame, origin, delay):
    actual = int.from_bytes(frame[47:52], 'little')
    target = origin + delay * 638976 // 10
    return (anchor_tx_matches(tx, 1, actual) and tx.get('attempted') is True
            and tx.get('status', 0) & 0x80 and (target - actual) % TICK_MODULUS <= 511)


def p1_selection(anchor: dict, tag: dict):
    """Bind the selected A/B P1 observations to one independently confirmed TX."""
    try:
        primary, selected = _air(anchor['frames'][1]), _air(tag['frames'][1])
        primary_tx = anchor['tx'][1]
        if primary is None or selected is None:
            return None
        if not p1_repeat_contract(anchor, tag):
            return (primary, primary_tx) if primary == selected else None
        if ((anchor['firmware'], tag['firmware']) not in P1_REPEAT_PAIRS
                or anchor['stage'] != tag['stage'] or tag['stage'] != 6
                or anchor['period_us'] != tag['period_us']):
            return None

        poll, p2 = _air(anchor['frames'][0]), _air(tag['frames'][2])
        if poll is None or p2 is None or p2[5] != 5:
            return None
        origin = int.from_bytes(poll[32:37], 'little')
        identity = (tag['session'], tag['txn'], tag['config'], tag['peer_boot'], tag['boot'])
        if (primary[5] != 4 or primary[28] != 255 or primary[31] != 6
                or struct.unpack_from('<5I', primary, 8) != identity
                or int.from_bytes(primary[32:37], 'little') != origin
                or not _p1_tx_at(primary_tx, primary, origin, 28000)
                or p2[52:57] != primary[47:52] or p2[30] & 3 != 3):
            return None

        repeat_tx = anchor['p1_repeat_tx']
        repeat_value = anchor['p1_repeat_frame']
        repeat = _air(repeat_value)
        if repeat is None:
            if repeat_value not in ('', None) or repeat_tx.get('ok') is not False or any(p2[57:62]):
                return None
        elif (repeat[:47] != primary[:47] or repeat[52:92] != primary[52:92]
              or not _p1_tx_at(repeat_tx, repeat, origin, 42000)
              or not any(repeat[47:52]) or p2[57:62] != repeat[47:52]):
            return None

        used = tag['p1_repeat_used']
        rx_mask, cir_mask = tag['p1_primary_rx_mask'], tag['p1_primary_cir_mask']
        if (type(used) is not bool or type(rx_mask) is not int or type(cir_mask) is not int
                or not 0 <= rx_mask <= 3 or not 0 <= cir_mask <= 3 or cir_mask & ~rx_mask
                or used and rx_mask == cir_mask == 3
                or selected != (repeat if used else primary)):
            return None
        selected_rx_mask = selected_cir_mask = 0
        sent_delta = (int.from_bytes(selected[47:52], 'little') - origin) % TICK_MODULUS
        for radio in range(2):
            rx, cir, local_poll = tag['probe_rx'][radio], tag['cir'][radio], tag['poll_rx'][radio]
            if cir.get('valid') is True:
                if (cir.get('slot') != radio or cir.get('radio') != radio
                        or cir.get('probe') != 1 or cir.get('present') is not True):
                    return None
                selected_cir_mask |= 1 << radio
            if rx.get('ok') is not True:
                if rx.get('air') not in ('', None):
                    return None
                continue
            if (rx.get('lde') is not True or rx.get('radio') != radio or rx.get('type') != 4
                    or _air(rx.get('air')) != selected or local_poll.get('ok') is not True
                    or local_poll.get('lde') is not True or local_poll.get('radio') != radio
                    or local_poll.get('type') != 1 or _air(local_poll.get('air')) != poll):
                return None
            ticks = (rx.get('hw'), local_poll.get('hw'))
            if not all(type(tick) is int and 0 <= tick < TICK_MODULUS for tick in ticks):
                return None
            if abs((ticks[0] - ticks[1]) % TICK_MODULUS - sent_delta) > 200 * 638976 // 10:
                return None
            selected_rx_mask |= 1 << radio
        if (selected_cir_mask & ~selected_rx_mask or
                (selected_rx_mask, selected_cir_mask) != ((3, 3) if used else (rx_mask, cir_mask))):
            return None
        return selected, repeat_tx if used else primary_tx
    except (KeyError, IndexError, TypeError, ValueError, AttributeError):
        return None


def p2_selection(anchor: dict, tag: dict):
    """Return the selected over-air frame and its independent anchor TX record."""
    try:
        primary = _air(anchor['frames'][2])
        selected = _air(tag['frames'][2])
        primary_tx = anchor['tx'][2]
        if primary is None or selected is None:
            return None
        if not p2_repeat_contract(anchor, tag):
            return (primary, primary_tx) if selected == primary else None
        if ((anchor['firmware'], tag['firmware']) not in REPEAT_PAIRS
                or anchor['stage'] != 6 or tag['stage'] != 6
                or anchor['period_us'] != tag['period_us']):
            return None

        repeat = _air(anchor['p2_repeat_frame'])
        repeat_tx = anchor['p2_repeat_tx']
        identity = (tag['session'], tag['txn'], tag['config'], tag['peer_boot'], tag['boot'])
        if (repeat is None or primary[5] != 5 or primary[28] != 255 or primary[31] != 6
                or struct.unpack_from('<5I', primary, 8) != identity
                or repeat[:47] != primary[:47] or repeat[52:92] != primary[52:92]):
            return None

        origin_and_air = _probe2_origin(anchor, tag)
        if origin_and_air is None:
            return None
        origin, poll_air = origin_and_air
        for delay, tx, frame in ((28000, primary_tx, primary), (42000, repeat_tx, repeat)):
            actual = int.from_bytes(frame[47:52], 'little')
            target = origin + delay * 638976//10
            if (not anchor_tx_matches(tx, 2, actual) or tx.get('attempted') is not True
                    or not tx.get('status', 0) & 0x80 or (target - actual) % TICK_MODULUS > 511):
                return None

        used = tag['p2_repeat_used']
        rx_mask, cir_mask = tag['p2_primary_rx_mask'], tag['p2_primary_cir_mask']
        if (type(used) is not bool or type(rx_mask) is not int or type(cir_mask) is not int
                or not 0 <= rx_mask <= 3 or not 0 <= cir_mask <= 3 or cir_mask & ~rx_mask
                or used and rx_mask == cir_mask == 3
                or selected != (repeat if used else primary)):
            return None

        selected_rx_mask = selected_cir_mask = 0
        sent_delta = (int.from_bytes(selected[47:52], 'little') - origin) % TICK_MODULUS
        for radio in range(2):
            rx, cir, poll = tag['probe_rx'][2 + radio], tag['cir'][2 + radio], tag['poll2_rx'][radio]
            if cir.get('valid') is True:
                if (cir.get('slot') != 2 + radio or cir.get('radio') != radio or cir.get('probe') != 2
                        or cir.get('present') is not True):
                    return None
                selected_cir_mask |= 1 << radio
            if rx.get('ok') is not True:
                if rx.get('air') not in ('', None):
                    return None
                continue
            if (rx.get('lde') is not True or rx.get('radio') != radio or rx.get('type') != 5
                    or _air(rx.get('air')) != selected
                    or poll.get('ok') is not True or poll.get('lde') is not True
                    or poll.get('radio') != radio or poll.get('type') != 8
                    or poll.get('air_crc') != int.from_bytes(poll_air[92:96], 'little')):
                return None
            ticks = (rx.get('hw'), poll.get('hw'))
            if not all(type(tick) is int and 0 <= tick < TICK_MODULUS for tick in ticks):
                return None
            # Both observations are on one tag clock. The slack covers clock
            # offset and path change while keeping the two emissions distinct.
            observed = (ticks[0] - ticks[1]) % TICK_MODULUS
            if abs(observed - sent_delta) > 200 * 638976//10:
                return None
            selected_rx_mask |= 1 << radio

        if (selected_cir_mask & ~selected_rx_mask or
                (selected_rx_mask, selected_cir_mask) != ((3, 3) if used else (rx_mask, cir_mask))):
            return None
        return selected, repeat_tx if used else primary_tx
    except (KeyError, IndexError, TypeError, ValueError, AttributeError):
        return None


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
        repeat_firmware = stage == 6 and any(
            identities[role].get('firmware') in P2_REPEAT_FIRMWARES for role in expected_roles)
        firmware_pair = tuple(identities[role].get('firmware') for role in ('anchor', 'tag'))
        if repeat_firmware and firmware_pair not in REPEAT_PAIRS:
            errors.append('IDENTITY_P2_REPEAT_MISMATCH')
        if any(type(boots[r]) is not int or not 0 < boots[r] <= 0xffffffff for r in expected_roles):
            errors.append('INVALID_IDENTITY_BOOT')
        for role in expected_roles:
            h = identities[role]
            if (h.get('role') != role or h.get('hw_ok') is not True or
                    h.get('fault_latched') is True or h.get('stage') != stage or
                    h.get('period_us') != period_us):
                errors.append(f'{role.upper()}_HELLO_MISMATCH')
    except (KeyError, TypeError, ValueError):
        config, boots, repeat_firmware = None, {}, False
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
            if repeat_firmware and m.get('firmware') != identities[role].get('firmware'):
                errors.append(f'{role.upper()}_P2_REPEAT_FIRMWARE_PROVENANCE_TXN_{tx}')
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
                if stage == 6 and (not isinstance(cms, list) or len(cms) != 4 or
                                   not all(isinstance(c, dict) and c.get('valid') is True for c in cms)):
                    errors.append(f'TAG_CIR_INCOMPLETE_TXN_{tx}')
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
        p1_contract = p1_repeat_contract(a, t)
        p1_proof = p1_selection(a, t) if p1_contract else None
        if p1_contract:
            if p1_proof is None:
                errors.append(f'P1_REPEAT_PROVENANCE_MISMATCH_TXN_{txn}')
            else:
                rf_frames[1] = p1_proof[0]
        repeat_contract = p2_repeat_contract(a, t)
        selection = p2_selection(a, t) if repeat_contract or repeat_firmware else None
        if repeat_contract or repeat_firmware:
            if not repeat_contract or selection is None:
                errors.append(f'P2_REPEAT_PROVENANCE_MISMATCH_TXN_{txn}')
            else:
                rf_frames[2] = selection[0]
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
                    (not p1_contract and p2[52:57] != p1[47:52]) or p2[30] & 3 != 3 or
                    (stage >= 3 and p1[29] != 3)):
                errors.append(f'PAIR_RF_ANCHOR_BLOCK_INCOHERENT_TXN_{txn}')
        # Independent endpoint timing evidence for every RF packet included.
        tt = a.get('tx', [])
        if not isinstance(tt, list) or len(tt) != 3:
            errors.append(f'ANCHOR_TX_SHAPE_TXN_{txn}')
        else:
            for index, field_off in ((0, 32), (1, 47), (2, 47)):
                frame = rf_frames[index]
                selected_tx = {1: p1_proof, 2: selection}.get(index)
                if frame is None or not anchor_tx_matches(
                        selected_tx[1] if selected_tx else tt[index],
                        index, int.from_bytes(frame[field_off:field_off+5], 'little')):
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

def _is_nonce(value):
    return type(value) is int and 0 < value <= 0xffffffff


def strict_shutdown_ok(receipt):
    """Recompute the shutdown verdict from both devices' correlated wire evidence."""
    try:
        shutdown = receipt["shutdown"]
        stop_request, session = shutdown["request"], receipt["session"]
        if (shutdown["contract"] != "drain_v1" or shutdown.get("persisted_control_proof") is not True
                or not _is_nonce(stop_request)
                or not _is_nonce(session) or set(shutdown["roles"]) != {"anchor", "tag"}):
            return False
        firmware_pair = tuple(receipt[f"{role}_identity"]["firmware"] for role in ("anchor", "tag"))
        if firmware_pair not in (DRAIN_FIRMWARES, OPERATIONAL_DRAIN_FIRMWARES):
            return False
        for role in ("anchor", "tag"):
            state = shutdown["roles"][role]
            identity, stats = receipt[f"{role}_identity"], receipt[f"{role}_capture"]
            boot, firmware = identity["boot"], identity["firmware"]
            if (not _is_nonce(boot)
                    or identity.get("drain_protocol") != 1
                    or any(state.get(k) is not True for k in
                           ("command_sent", "acknowledged", "host_sealed", "baud_restored", "closed"))
                    or any(k in state for k in
                           ("send_error", "ack_error", "drain_or_restore_error", "close_error"))
                    or stats.get("reader_error") or stats.get("writer_error")
                    or stats.get("capture_overflow")):
                return False

            stopped, drained = state["stopped"], state["drained"]
            drain_request = state["drain_request"]
            if (not _is_nonce(drain_request) or state["session"] != session
                    or (stopped["event"], stopped["role"], stopped["boot"], stopped["session"],
                        stopped["stop_request"]) != ("STOPPED", role, boot, session, stop_request)
                    or (drained["event"], drained["role"], drained["boot"], drained["session"],
                        drained["request"]) != ("DRAINED", role, boot, session, drain_request)
                    or drained["ok"] is not True or drained["rf_stopped"] is not True
                    or drained["reason"] != "OK"
                    or any(type(drained.get(k)) is not int or drained[k] != 0
                           for k in ("pending", "awaiting_ack", "failures"))):
                return False

            status_proof = state["status_proof"]
            restore_proof = state["baud_restore_proof"]
            for proof, baud in ((status_proof, receipt["baud"]), (restore_proof, 921600)):
                hello, request = proof["hello"], proof["request"]
                if (not _is_nonce(request) or
                        (hello["event"], hello["role"], hello["boot"], hello["firmware"],
                         hello["session"], hello["baud"], hello["hello_request"]) !=
                        ("HELLO", role, boot, firmware, session, baud, request)
                        or hello["active"] is not False or hello.get("drain_protocol") != 1
                        or hello.get("hw_ok") is not True or hello.get("fault_latched") is not False):
                    return False
            if (state["status"] != status_proof["hello"] or
                    (receipt["baud"] == 2000000 and
                     (restore_proof["request"] == status_proof["request"] or
                      restore_proof["hello"].get("period_us") !=
                      max(300000, status_proof["hello"]["period_us"])))):
                return False
            transport = stats["transport"]
            if (transport.get("pending_records") or transport.get("unresolved_sequences")
                    or transport.get("pending_fragment_bytes") or transport.get("partial_fragment_record")
                    or transport.get("host_pending_batches") or any(stats["capture_decoder"].values())):
                return False
            if receipt["baud"] == 2000000 and (
                    transport.get("pending_records") != 0
                    or transport.get("unresolved_sequences") != []
                    or transport.get("pending_fragment_bytes") != 0
                    or transport.get("partial_fragment_record") is not False
                    or transport.get("host_pending_batches") != 0
                    or transport.get("durable_acks_sent") != stats.get("records")
                    or type(transport.get("durable_acks_sent")) is not int):
                return False
        return True
    except (KeyError, TypeError, ValueError, AttributeError):
        return False


def stage6_accounting(receipt, records, *, legacy_shutdown=False):
    """Keep first RF, RF retry, CIR, FEC, and transport results distinct."""
    count = receipt["requested_count"]
    expected = set(range(count))
    anchors = {m["txn"]: m for m in records.get("anchor", ()) if type(m.get("txn")) is int}
    tags = {m["txn"]: m for m in records.get("tag", ()) if type(m.get("txn")) is int}
    roles = receipt.get("shutdown", {}).get("roles", {})
    issues = receipt.get("audit", {}).get("problems", [])
    rf, transport, health = {}, {}, {}
    misses = set()
    timings = set()
    timing_errors = {1: set(), 2: set()}
    for issue in issues:
        matched = re.search(r"_TXN_(\d+)(?:_|$)", issue)
        if not matched:
            continue
        if issue.startswith(("TAG_PROBE_AIR_MISMATCH", "RESPONSE2_TIMESTAMP", "P2_REPEAT_PROVENANCE")):
            slot = re.search(r"_SLOT_([0-3])$", issue)
            affected = (1 + int(slot[1]) // 2,) if slot else (2,)
        elif issue.startswith(("TAG_DS_", "RESPONSE_TIMESTAMP", "P1_REPEAT_PROVENANCE")):
            affected = (1,)
        elif any(key in issue for key in ("TIMESTAMP", "RF_", "AIR_MISMATCH", "PROVENANCE",
                                          "SHAPE", "FRAME_MISMATCH", "MISSING_FRAMES")):
            affected = (1, 2)
        else:
            continue
        for probe in affected:
            timing_errors[probe].add(int(matched[1]))
    for probe in (1, 2):
        field = f"p{probe}_retry"
        retry = {txn for txn in expected if anchors.get(txn, {}).get(field) is True
                 or tags.get(txn, {}).get(field) is True}
        received, probe_misses, repeat_used = set(), set(), set()
        for txn in expected & anchors.keys() & tags.keys():
            tag, anchor = tags[txn], anchors[txn]
            rx = tag.get("probe_rx", [])
            offset = 2*(probe-1)
            if len(rx) != 4:
                continue
            mask = sum(1 << i for i, slot in enumerate(rx[offset:offset+2])
                       if slot.get("ok") is True and slot.get("lde") is True
                       and slot.get("type") == probe + 3 and slot.get("radio") == i)
            if mask == 3:
                received.add(txn)
                selector = p1_selection if probe == 1 else p2_selection
                if tag.get(f"p{probe}_repeat_used") is True and selector(anchor, tag) is not None:
                    repeat_used.add(txn)
            elif (tag.get("_raw_integrity_verified") is True and
                  not tag.get("errors", 0) & (1 | 64 | 256) and
                  len(anchor.get("tx", [])) == 3 and anchor["tx"][probe].get("ok") is True):
                probe_misses.add(txn)
        misses.update(probe_misses)
        verified = received - timing_errors[probe] if "audit" in receipt else set()
        timings.update((probe, txn) for txn in verified)
        rf[f"P{probe}"] = {
            "initial_confirmed": len(verified - retry - repeat_used),
            "rf_retry_attempted_txns": sorted(retry),
            "rf_retry_confirmed": len(verified & retry),
            "probe_miss_txns": sorted(probe_misses),
            "final_received": len(received),
            "both_endpoint_evidence_confirmed": len(verified),
        }
        if probe == 2 or any(tag.get("firmware") in (P1_REPEAT_TAG, DRAIN_FIRMWARES[1]) for tag in tags.values()):
            rf[f"P{probe}"]["primary_complete"] = len(verified - repeat_used)
            rf[f"P{probe}"]["repeat_restored_txns"] = sorted(verified & repeat_used)
    for role, indexed in (("anchor", anchors), ("tag", tags)):
        stats = receipt.get(f"{role}_capture", {})
        t = stats.get("transport", {})
        status = roles.get(role, {}).get("status", {})
        before = receipt.get(f"{role}_identity", {})
        health[role] = bool(status and status.get("boot") == before.get("boot")
                            and status.get("hw_ok") is True and status.get("fault_latched") is False
                            and status.get("drops") == before.get("drops")
                            and status.get("period_miss") == 0 and
                            type(status.get("sent")) is int and type(before.get("sent")) is int
                            and (status["sent"] - before["sent"]) % (1 << 32) == count)
        sent_replays = status.get("record_retransmissions")
        prior_replays = before.get("record_retransmissions")
        transport[role] = {
            "firmware_retransmissions": ((sent_replays - prior_replays) % (1 << 32)
                                         if type(sent_replays) is int and type(prior_replays) is int else None),
            "records_stored": len(indexed), "missing_txns": sorted(expected - indexed.keys()),
            "fec": t.get("fragment", {}), "fec_repaired_sequences": t.get("fec_repaired_sequences", []),
            "first_failed_sequences": t.get("first_failed_sequences", []),
            "unresolved_sequences": t.get("unresolved_sequences", []),
            "pending_records": t.get("pending_records", 0),
            "stream": stats.get("capture_decoder", {}),
            "period_miss": status.get("period_miss"),
        }
        if legacy_shutdown:
            transport[role].update(replayed_sequences=t.get("replayed_sequences", []),
                                   replay_requests=t.get("replay_requests", 0))
        else:
            transport[role].update(
                gaps_repaired_after_nack_scheduled=t.get("gaps_repaired_after_nack_scheduled", []),
                replay_nacks_scheduled=t.get("replay_nacks_scheduled", 0),
                replay_nacks_sent=t.get("replay_nacks_sent", 0))
    transport_complete = all(x["records_stored"] == count and not x["missing_txns"]
                             and not x["unresolved_sequences"] and not x["pending_records"]
                             and not any(x["stream"].values()) for x in transport.values())
    shutdown = receipt.get("shutdown", {})
    restore = all("baud_restore_error" not in v and v.get("baud_restored") is not False
                  for v in roles.values())
    if receipt.get("baud") == 2000000:
        restore = (restore and set(roles) == {"anchor", "tag"}
                   and all(v.get("baud_restored") is True for v in roles.values()))
    stop = (shutdown.get("pass") is True and
            (strict_shutdown_ok(receipt) if shutdown.get("contract") == "drain_v1"
             else legacy_shutdown and shutdown.get("contract") in (None, "legacy_stop") and restore))
    cir = sum(c.get("valid") is True for txn, m in tags.items() if txn in expected and
              m.get("_raw_integrity_verified") is True for c in m.get("cir", ()))
    ticks = [anchors.get(txn, {}).get("initial_poll_tx") for txn in range(count)]
    intervals = [((later-earlier) % (1 << 40))*1e6/63897600000
                 for earlier, later in zip(ticks, ticks[1:])
                 if type(earlier) is int and type(later) is int and
                 0 <= earlier < 1 << 40 and 0 <= later < 1 << 40]
    cadence_ok = (len(intervals) == count-1 and
                  all(abs(value-receipt["period_us"]) <= 4000 for value in intervals))
    return {
        "rf": rf, "probe_miss_txns": sorted(misses),
        "cir_integrity_valid_slots": cir, "cir_expected_slots": 4*count,
        "transport": transport, "firmware_health_ok": health, "stop_confirmed": stop,
        "absolute_accuracy_validated": False,
        "cadence": {"pass": cadence_ok, "interval_count": len(intervals),
                    "min_us": min(intervals, default=None), "max_us": max(intervals, default=None)},
        "followup_safe": transport_complete and stop and cadence_ok and all(health.values()) and restore and
                         all(not receipt.get(f"{r}_capture", {}).get("fault_events") for r in roles),
        "strict_gate": transport_complete and stop and cadence_ok and all(health.values()) and cir == 4*count
                       and len(timings) == 2*count,
    }
