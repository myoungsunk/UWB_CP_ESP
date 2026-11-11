"""Replay the R4 partition case with frozen R3 artifacts and a mocked uploader."""
from contextlib import redirect_stderr
from hashlib import sha256
import io
import json
from pathlib import Path
import shutil
import subprocess
import sys
from tempfile import TemporaryDirectory
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'tools'))
from offline_preflight import scan
from source_contract import manifest
import upload


def run(r3, core):
    with TemporaryDirectory(prefix='mode8-r5-offline-') as tmp:
        root = Path(tmp)
        for folder in ('shared', 'firmware', 'vendor', 'dist'):
            shutil.copytree(r3 / folder, root / folder,
                            ignore=shutil.ignore_patterns('build', '__pycache__'))
        receipt_file = root / 'dist/target_build_receipt.json'
        receipt = json.loads(receipt_file.read_bytes())
        assert not scan(root)['flash_preflight_pass']
        # Test-only receipt. The corresponding images and inputs are all R3.
        for role in ('anchor', 'tag'):
            receipt['roles'][role]['source_manifest'] = manifest(root, role)
        receipt_file.write_text(json.dumps(receipt), encoding='utf-8')
        assert scan(root)['flash_preflight_pass']

        table = root / 'firmware/ANCHOR_DUAL_DS_CIR/partitions.csv'
        default = (core / 'tools/partitions/default.csv').read_text(encoding='utf-8')
        changed = default.replace('0x160000', '0x150000')
        assert not table.exists() and default != changed
        table.write_text(changed, encoding='utf-8')
        partition = root / 'new.partitions.bin'
        generator = core / 'tools/gen_esp32part.exe'
        command = [str(generator), '-q', str(table), str(partition)]
        generated = subprocess.run(command, check=True, capture_output=True)
        previous = root / 'dist/anchor/ANCHOR_DUAL_DS_CIR.ino.partitions.bin'
        old_sha, new_sha = (sha256(p.read_bytes()).hexdigest() for p in (previous, partition))
        assert old_sha != new_sha
        checked = scan(root)
        checks = checked['roles']['anchor']['checks']
        assert not checked['flash_preflight_pass'] and not checks['source_binding']
        assert all(value for name, value in checks.items() if name != 'source_binding')
        assert checked['roles']['tag']['flash_preflight_pass']

        err = io.StringIO()
        with patch.object(upload, 'ROOT', root), patch.object(sys, 'argv',
             ['upload.py', '--role', 'anchor', '--port', 'FAKE_PORT']), \
             patch.object(upload.shutil, 'which', return_value='FAKE_CLI'), \
             patch.object(upload.subprocess, 'run') as called, redirect_stderr(err):
            try:
                upload.main()
            except SystemExit as exc:
                code = exc.code
            else:
                raise AssertionError('Uploader did not reject the stale image')
        called.assert_not_called()
        assert code == 2 and 'provenance' in err.getvalue()
        return {
            'scope': 'Frozen R3 images and sources in a temporary copy; mocked uploader',
            'test_receipt_baseline_pass': True,
            'generator_command': [str(generator), '-q', str(table.relative_to(root)), partition.name],
            'generator_exit': generated.returncode,
            'old_partition_sha256': old_sha,
            'new_partition_sha256': new_sha,
            'old_image_bytes_and_hashes_still_match': True,
            'anchor_source_binding_after_change': checks['source_binding'],
            'preflight_pass_after_change': checked['flash_preflight_pass'],
            'upload_exit': code,
            'upload_subprocess_calls': called.call_count,
        }


if __name__ == '__main__':
    if len(sys.argv) != 3:
        sys.exit('Usage: replay_legacy_partition.py <frozen R3 package root> <ESP32 Core 3.3.7 root>')
    print(json.dumps(run(Path(sys.argv[1]), Path(sys.argv[2])), indent=2))
