"""Read-only R4 audit. Uses synthetic records and stubs the uploader process."""
import copy
from contextlib import redirect_stdout
from hashlib import sha256
import io
import json
from pathlib import Path
import shutil
import subprocess
import sys
from tempfile import TemporaryDirectory
from types import SimpleNamespace
from unittest.mock import patch

HERE = Path(__file__).resolve().parent
R4 = HERE.parent / 'UWB_DUAL_DW1000_MODE8_R4_HOST_REAUDITED_20261111'
R3 = Path(sys.argv[1]).resolve()
CORE = Path(sys.argv[2]).resolve()
sys.path[:0] = [str(R4 / 'tools'), str(R4 / 'tests')]
from analyze import packet_arrays
from capture_audit import verify_capture
from fixture_helpers import synthetic_anchor
from offline_preflight import scan
from run_pair import cir_payload_integrity
from source_contract import manifest
import upload


def load(path):
    return json.loads(path.read_bytes())


def partial_poll():
    # Use the same C++-serialized R4 record for the R3 and R4 analysis calls.
    old_path = R3 / 'tools/analyze.py'
    from importlib.util import spec_from_file_location, module_from_spec
    spec = spec_from_file_location('analyze_r3', old_path)
    old = module_from_spec(spec)
    spec.loader.exec_module(old)
    output = []
    for lost in (0, 1):
        m = load(HERE / f'partial_poll_lost_{lost}.json')
        raw = (HERE / f'partial_poll_lost_{lost}.raw').read_bytes()
        arrays, new = packet_arrays(m, raw)
        _, before = old.packet_arrays(m, raw)
        other = lost ^ 1
        assert not m['poll_rx'][lost]['ok'] and m['ranges'][other]['math_valid']
        assert before[other]['range_m'] is not None
        assert new[other]['range_m'] is None
        assert arrays['iq_int16'].astype('<i2').tobytes() == raw
        output.append({'lost_poll_radio': lost, 'retained_radio': other,
                       'firmware_range_m': m['ranges'][other]['d_m'],
                       'r3_host_range_m': before[other]['range_m'],
                       'r4_host_range_m': new[other]['range_m']})
    return output


def bad_anchor_tx():
    ev = R4 / 'evidence/host'
    tag = load(ev / 'synthetic_tag_record.json')
    raw = (ev / 'SYNTHETIC_CIR_i16.raw').read_bytes()
    tag['_raw_integrity_verified'] = cir_payload_integrity(tag, raw)
    anchor = synthetic_anchor(tag)
    identities = {m['role']: {'role': m['role'], 'boot': m['boot'],
                             'config_id': m['config'], 'stage': m['stage'],
                             'period_us': m['period_us'], 'hw_ok': True} for m in (tag, anchor)}
    _, good = packet_arrays(tag, raw, anchor)
    good_ranges = [r['range_m'] for r in good[2:]]
    assert all(x is not None for x in good_ranges)
    results = []
    for index, key, value in ((0, 'planned', anchor['tx'][0]['planned'] + 1),
                              (1, 'type', 5), (2, 'radio', 1)):
        bad = copy.deepcopy(anchor)
        bad['tx'][index][key] = value
        audited = verify_capture(session=tag['session'], count=1, stage=tag['stage'],
                                 period_us=tag['period_us'], identities=identities,
                                 records={'tag': [tag], 'anchor': [bad]}, stats={})
        arrays, rows = packet_arrays(tag, raw, bad)
        accepted = [row['range_m'] for row in rows[2:]]
        assert not audited.passed and accepted == good_ranges and arrays['common_p2_delay_s'].size
        results.append({'anchor_tx_index': index, 'field': key, 'new_value': value,
                        'capture_problems': audited.problems,
                        'analysis_p2_ranges_m': accepted,
                        'analysis_p2_grid_points': int(arrays['common_p2_delay_s'].size)})
    return results


def stale_partition():
    # Actual R3 images are paired with a test-only receipt computed from their
    # own frozen R3 sources using the R4 schema. No R4 image is inferred.
    with TemporaryDirectory(prefix='mode8-r4-offline-') as tmp:
        root = Path(tmp)
        ignore = shutil.ignore_patterns('build', '__pycache__')
        for folder in ('shared', 'firmware', 'vendor', 'dist'):
            shutil.copytree(R3 / folder, root / folder, ignore=ignore)
        receipt_file = root / 'dist/target_build_receipt.json'
        receipt = load(receipt_file)
        for role in ('anchor', 'tag'):
            receipt['roles'][role]['source_manifest'] = manifest(root, role)
        receipt_file.write_text(json.dumps(receipt), encoding='utf-8')
        assert scan(root)['flash_preflight_pass']
        original = receipt['roles']['anchor']['source_manifest']
        source = root / 'firmware/ANCHOR_DUAL_DS_CIR/esp32_port.hpp'
        original_source = source.read_bytes()
        source.write_bytes(original_source + b'\n// audit mutation\n')
        assert manifest(root, 'anchor') != original
        assert not scan(root)['flash_preflight_pass']
        source.write_bytes(original_source)
        table = root / 'firmware/ANCHOR_DUAL_DS_CIR/partitions.csv'
        default = (CORE / 'tools/partitions/default.csv').read_text(encoding='utf-8')
        changed = default.replace('0x160000', '0x150000')
        assert not table.exists() and default != changed
        table.write_text(changed, encoding='utf-8')
        partition = root / 'updated.partitions.bin'
        subprocess.run([str(CORE / 'tools/gen_esp32part.exe'), '-q', str(table),
                        str(partition)], check=True)
        old_bin = root / 'dist/anchor/ANCHOR_DUAL_DS_CIR.ino.partitions.bin'
        old_sha = sha256(old_bin.read_bytes()).hexdigest()
        new_sha = sha256(partition.read_bytes()).hexdigest()
        assert new_sha != old_sha
        assert manifest(root, 'anchor') == original
        checked = scan(root)
        assert checked['flash_preflight_pass']
        with patch.object(upload, 'ROOT', root), patch.object(sys, 'argv',
             ['upload.py', '--role', 'anchor', '--port', 'FAKE_PORT']), \
             patch.object(upload.shutil, 'which', return_value='FAKE_CLI'), \
             patch.object(upload.subprocess, 'run', return_value=SimpleNamespace(returncode=0)) as called, \
             redirect_stdout(io.StringIO()):
            code = upload.main()
        assert code == 0 and called.call_count == 1 and called.call_args.args[0][1] == 'upload'
        return {'changed_input': str(table.relative_to(root)), 'old_partition_sha256': old_sha,
                'new_partition_sha256': new_sha, 'source_manifest_unchanged': True,
                'ordinary_hpp_edit_rejected': True,
                'preflight_pass_after_input_change': True, 'upload_subprocess_requested': True,
                'uploader_executed': False}


if __name__ == '__main__':
    print(json.dumps({'partial_poll': partial_poll(), 'anchor_tx_mismatch': bad_anchor_tx(),
                      'stale_partition': stale_partition()}, indent=2))
