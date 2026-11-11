import copy, csv, json, subprocess, sys
from pathlib import Path
from tempfile import TemporaryDirectory
import numpy as np
sys.path[:0] = [str(Path('tools').resolve()), str(Path('tests').resolve())]
from fixture_helpers import synthetic_anchor
from rfb8 import encode
def replay(root, label, tag, raw, anchor=None, static=False):
    tag_path = root / (label + '.tag.rfb8')
    tag_path.write_bytes(encode(tag, raw))
    out = root / label
    command = [sys.executable, '-B', '-X', 'utf8=0', 'tools/analyze.py', str(tag_path), '--out', str(out)]
    if anchor is not None:
        anchor_path = root / (label + '.anchor.rfb8')
        anchor_path.write_bytes(encode(anchor))
        command.extend(('--anchor', str(anchor_path)))
    if static:
        command.append('--assume-static')
    proc = subprocess.run(command, check=True, capture_output=True)
    with (out / 'observations.csv').open(encoding='utf-8-sig', newline='') as f:
        rows = list(csv.DictReader(f))
    with np.load(next(out.glob('*.npz'))) as a:
        assert a['iq_int16'].astype('<i2').tobytes() == raw
        assert np.isfinite(a['relative_s']).all()
        sizes = [int(a[f'common_p{p}_delay_s'].size) for p in (1, 2)]
    return {'exit_code': proc.returncode, 'ranges_m': [float(r['range_m']) if r['range_m'] else None for r in rows],
            'common_grid_points': sizes, 'raw_iq_matches': True, 'relative_axes_finite': True}
results = {}
with TemporaryDirectory(prefix='mode8-r5-analyze-') as tmp:
    root = Path(tmp)
    for lost in (0, 1):
        label = f'partial_poll_lost_{lost}'
        fixture = Path('tests/fixtures') / label
        tag = json.loads(fixture.with_suffix('.json').read_bytes())
        case = replay(root, label, tag, fixture.with_suffix('.raw').read_bytes())
        assert case['ranges_m'][lost] is None and abs(case['ranges_m'][1-lost]-tag['ranges'][1-lost]['d_m']) < 1e-8
        assert case['common_grid_points'] == [0, 0]
        results[label] = case
    tag = json.loads(Path('evidence/host/synthetic_tag_record.json').read_bytes())
    raw = Path('evidence/host/SYNTHETIC_CIR_i16.raw').read_bytes()
    anchor = synthetic_anchor(tag)
    good = replay(root, 'full', tag, raw, anchor)
    assert all(x is not None for x in good['ranges_m']) and all(good['common_grid_points'])
    results['valid_anchor'] = good
    for slot, key, val in ((0,'planned',anchor['tx'][0]['planned']+1),(1,'type',5),(2,'radio',1)):
        corrupt = copy.deepcopy(anchor)
        corrupt['tx'][slot][key] = val
        label = f'bad_anchor_tx_{slot}_{key}'
        case = replay(root, label, tag, raw, corrupt, static=True)
        assert case['ranges_m'] == good['ranges_m'][:2] + [None,None]
        assert case['common_grid_points'] == [good['common_grid_points'][0], 0]
        results[label] = case
print(json.dumps(results, indent=2))

