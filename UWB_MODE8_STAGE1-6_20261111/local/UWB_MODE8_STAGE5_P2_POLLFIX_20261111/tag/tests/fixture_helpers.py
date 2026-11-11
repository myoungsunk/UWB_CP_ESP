"""Synthetic anchor record built from synthetic tag RF frames (tests only)."""
import copy
import zlib


def frame_at(hexstr):
    return bytearray.fromhex(hexstr)


def reframe_txn(frame_hex, txn):
    b = frame_at(frame_hex)
    b[12:16] = txn.to_bytes(4, 'little')
    b[92:96] = zlib.crc32(b[:92]).to_bytes(4, 'little')
    return b.hex()


def with_txn(m, txn):
    v = copy.deepcopy(m)
    v['txn'] = txn
    v['frames'] = [reframe_txn(f, txn) if f is not None else None for f in v['frames']]
    for kind in ('poll_rx', 'probe_rx'):
        for r in v[kind]:
            if r['ok']:
                idx = 0 if kind == 'poll_rx' else 1 + v['probe_rx'].index(r) // 2
                r['air'] = v['frames'][idx]
    if v.get('poll2_frame'):
        v['poll2_frame'] = reframe_txn(v['poll2_frame'], txn)
        for r in v['poll2_rx']:
            if r['ok']:
                r['air_crc'] = int.from_bytes(bytes.fromhex(v['poll2_frame'])[92:], 'little')
    return v


def synthetic_anchor(tag):
    p1, p2 = frame_at(tag['frames'][1]), frame_at(tag['frames'][2])
    poll = frame_at(tag['frames'][0])
    to_ticks = lambda b, p: int.from_bytes(b[p:p+5], 'little')
    rx = [to_ticks(p1, 37), to_ticks(p1, 42)]
    responses = []
    for radio in range(2):
        if tag['stage'] == 2:
            responses.append({'ok': False, 'lde': False, 'radio': 255, 'type': 0, 'hw': 0, 'air': ''})
            continue
        response = poll[:]
        response[5], response[28] = 2 + radio, radio
        response[20:24] = tag['boot'].to_bytes(4, 'little')
        response[24:28] = tag['peer_boot'].to_bytes(4, 'little')
        response[57:62] = tag['poll_rx'][radio]['hw'].to_bytes(5, 'little')
        response[62:67] = tag['tx'][radio]['actual'].to_bytes(5, 'little')
        response[92:96] = zlib.crc32(response[:92]).to_bytes(4, 'little')
        responses.append({'ok': True, 'lde': True, 'radio': 0, 'type': 2+radio, 'hw': rx[radio], 'air': response.hex()})
    responses2 = []
    poll2_tx = None
    if tag['stage'] == 5 and 'response2_tx' in tag:
        mask = (1 << 40)-1
        ap, af = to_ticks(frame_at(tag['poll2_frame']), 32), to_ticks(p2, 47)
        poll2_tx = {'ok': True, 'radio': 0, 'type': 8, 'planned': ap, 'actual': ap, 'status': 0x80}
        for radio, tx in enumerate(tag['response2_tx']):
            tp = tag['poll2_rx'][radio]['hw']
            tf = tag['probe_rx'][2+radio]['hw']
            # Synthetic fixed distance with independent affine clock mapping.
            tof = (1066, 1090)[radio]
            turn = (tx['actual'] - tp) & mask
            ar = (ap + round(turn * ((af-ap) & mask) / ((tf-tp) & mask)) + 2*tof) & mask
            frame = frame_at(responses[radio]['air'])
            frame[5] = 6+radio
            frame[32:37] = ap.to_bytes(5, 'little')
            frame[57:62] = tp.to_bytes(5, 'little')
            frame[62:67] = tx['actual'].to_bytes(5, 'little')
            frame[92:96] = zlib.crc32(frame[:92]).to_bytes(4, 'little')
            responses2.append({'ok': True, 'lde': True, 'radio': 0, 'type': 6+radio, 'hw': ar, 'air': frame.hex()})
    return {
        'schema': 'dual8.record.v1', 'role': 'anchor', 'mode': 8,
        'stage': tag['stage'], 'period_us': tag['period_us'],
        'session': tag['session'], 'txn': tag['txn'],
        'boot': tag['peer_boot'], 'peer_boot': 0 if tag['stage'] == 2 else tag['boot'],
        'config': tag['config'], 'resp_mask': p1[29],
        'complete': True, 'errors': 0, 'metadata_conflict': False,
        'frames': tag['frames'][:],
        'tx': [
            {'ok': True, 'radio': 0, 'type': 1, 'planned': to_ticks(poll, 32), 'actual': to_ticks(poll, 32)},
            {'ok': True, 'radio': 0, 'type': 4, 'planned': to_ticks(p1, 47), 'actual': to_ticks(p1, 47)},
            {'ok': True, 'radio': 0, 'type': 5, 'planned': to_ticks(p2, 47), 'actual': to_ticks(p2, 47)},
        ],
        'response_rx': responses,
        'response2_rx': responses2,
        'poll2_frame': tag.get('poll2_frame'),
        'poll2_tx': poll2_tx,
        'calibration': [{'id': 0}],
    }
