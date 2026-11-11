"""Re-audit R3 against the frozen Stage 2 reproducer and RFB8 file inputs."""
from copy import deepcopy
import csv
import json
from pathlib import Path
import subprocess
import sys
from tempfile import TemporaryDirectory

import numpy as np

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]
sys.path[:0] = [str(ROOT / 'tools'), str(ROOT / 'tests')]
from capture_audit import verify_capture
from fixture_helpers import synthetic_anchor
from rfb8 import CIR_BYTES, encode
from test_adversarial import altered_frame, stage2_record
from run_pair import cir_payload_integrity


def check_stage2(source, raw):
    anchor = json.loads((HERE / 'r2_stage2_anchor.json').read_bytes())
    tag = stage2_record(source)
    tag.update(config=anchor['config'], frames=anchor['frames'][:],
               _raw_integrity_verified=cir_payload_integrity(tag, raw))
    for index, rx in enumerate(tag['poll_rx'] + tag['probe_rx']):
        rx['air'] = tag['frames'][index // 2]
    identities = {
        m['role']: {'role': m['role'], 'boot': m['boot'], 'hw_ok': True,
                    'config_id': m['config'], 'stage': 2, 'period_us': m['period_us']}
        for m in (tag, anchor)
    }

    def audit(t, a):
        return verify_capture(session=tag['session'], count=1, stage=2,
                              period_us=tag['period_us'], identities=identities,
                              records={'tag': [t], 'anchor': [a]}, stats={})

    result = audit(tag, anchor)
    assert result.passed, result.problems
    # Alter the same received P2 bytes at both ends and recompute air CRC, so
    # peer identity validation must catch the mutation without byte inequality.
    a, t = deepcopy(anchor), deepcopy(tag)
    a['frames'][2] = t['frames'][2] = altered_frame(t['frames'][2], 24, (202).to_bytes(4, 'little'))
    for rx in t['probe_rx'][2:]:
        rx['air'] = t['frames'][2]
    bad = audit(t, a)
    assert not bad.passed and bad.problems == ['PAIR_RF_IDENTITY_MISMATCH_TXN_0_INDEX_2'], bad
    return {'original_stage2_pass': result.passed, 'crc_valid_forged_peer_rejected': bad.problems}


def analyze_files(tag, raw, anchor, *, assume_static=False):
    with TemporaryDirectory(prefix='dual8-r3-audit-') as tmp:
        folder = Path(tmp)
        t, a, output = folder / 'tag.rfb8', folder / 'anchor.rfb8', folder / 'out'
        t.write_bytes(encode(tag, raw, kind=2, sequence=0))
        a.write_bytes(encode(anchor, b'', kind=2, sequence=0))
        args = [sys.executable, '-B', str(ROOT / 'tools/analyze.py'), str(t),
                '--anchor', str(a), '--out', str(output)]
        if assume_static:
            args.append('--assume-static')
        subprocess.run(args, check=True, capture_output=True)
        with (output / 'observations.csv').open(newline='', encoding='utf-8-sig') as f:
            rows = list(csv.DictReader(f))
        with np.load(next(output.glob('*.npz'))) as arrays:
            assert arrays['iq_int16'].astype('<i2').tobytes() == raw
            assert np.isfinite(arrays['relative_s'][:2]).all()
            grids = [arrays[f'common_p{i}_delay_s'].size for i in (1, 2)]
        return {'range_m': [float(r['range_m']) if r['range_m'] else None for r in rows],
                'both_calibrated': [r['both_endpoints_calibrated'] == 'True' for r in rows],
                'common_grid_lengths': grids}


def main():
    source = json.loads((ROOT / 'evidence/host/synthetic_tag_record.json').read_bytes())
    raw = (ROOT / 'evidence/host/SYNTHETIC_CIR_i16.raw').read_bytes()
    results = {'stage2': check_stage2(source, raw)}
    tag = deepcopy(source)
    tag['ranges'][0]['cal_id'] = tag['calibration'][0]['id'] = 11
    anchor = synthetic_anchor(tag)
    anchor['calibration'][0]['id'] = 22
    good = analyze_files(tag, raw, anchor)
    assert all(x is not None for x in good['range_m']) and good['both_calibrated'][0]
    results['matched'] = good
    bad = analyze_files(tag, raw, {**anchor, 'config': anchor['config'] + 1})
    assert bad['range_m'][:2] == good['range_m'][:2] and not any(bad['both_calibrated'])
    assert bad['range_m'][2:] == [None, None]
    results['wrong_anchor_config'] = bad
    no_p2 = deepcopy(tag)
    for slot in (2, 3):
        no_p2['probe_rx'][slot].update(ok=False, air=None)
        no_p2['cir'][slot].update(valid=False, present=False)
    no_p2.update(errors=32, complete=False)
    missing = analyze_files(no_p2, raw[:2*CIR_BYTES] + bytes(2*CIR_BYTES), anchor, assume_static=True)
    assert missing['range_m'] == [None] * 4 and missing['common_grid_lengths'] == [0, 0]
    results['no_p2_slot_with_shared_p2_retained'] = missing
    wrong = deepcopy(tag)
    wrong['tx'][1].update(type=2, radio=0)
    wrong_tx = analyze_files(wrong, raw, anchor, assume_static=True)
    assert wrong_tx['range_m'][::2] == good['range_m'][::2] and wrong_tx['range_m'][1::2] == [None, None]
    results['wrong_local_response_identity'] = wrong_tx
    print(json.dumps({'status': 'PASS', 'scope': 'HOST_SYNTHETIC', 'cases': results}, indent=2))


if __name__ == '__main__':
    main()
