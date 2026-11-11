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
    return v


def synthetic_anchor(tag):
    p1, p2 = frame_at(tag['frames'][1]), frame_at(tag['frames'][2])
    poll = frame_at(tag['frames'][0])
    to_ticks = lambda b, p: int.from_bytes(b[p:p+5], 'little')
    rx = [to_ticks(p1, 37), to_ticks(p1, 42)]
    return {
        'schema': 'dual8.record.v1', 'role': 'anchor', 'mode': 8,
        'stage': tag['stage'], 'period_us': tag['period_us'],
        'session': tag['session'], 'txn': tag['txn'],
        'boot': tag['peer_boot'], 'peer_boot': tag['boot'], 'config': tag['config'],
        'complete': True, 'errors': 0, 'metadata_conflict': False,
        'frames': tag['frames'][:],
        'tx': [
            {'ok': True, 'actual': to_ticks(poll, 32)},
            {'ok': True, 'actual': to_ticks(p1, 47)},
            {'ok': True, 'actual': to_ticks(p2, 47)},
        ],
        'response_rx': [{'ok': True, 'hw': x} for x in rx],
        'calibration': [{'id': 0}],
    }
