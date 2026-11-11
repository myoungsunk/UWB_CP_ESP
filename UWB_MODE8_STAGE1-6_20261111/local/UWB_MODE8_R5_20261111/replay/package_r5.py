import datetime, hashlib, json, os, subprocess, sys, tempfile, zipfile
from pathlib import Path
root = Path.cwd()
original = Path(sys.argv[1])
artifact = root.parent / 'UWB_DUAL_DW1000_MODE8_R5_SOURCE_HOST_REAUDIT_20261111.zip'
def sha(data):
    return hashlib.sha256(data).hexdigest()
def digest(data):
    return {'bytes': len(data), 'sha256': sha(data)}
old_sha = sha(original.read_bytes())
assert old_sha in (root / 'docs/R5_REAUDIT_KO.md').read_text(encoding='utf-8')
with zipfile.ZipFile(original) as old:
    prefix = old.namelist()[0].split('/')[0] + '/'
    assert old.testzip() is None
    entries = json.loads(old.read(prefix + 'PACKAGE_SHA256.json'))['files']
    assert all(digest(old.read(prefix + rel)) == expected for rel, expected in entries.items())
    counts = {}
    for area in ('shared', 'firmware', 'vendor'):
        old_hashes = {name: expected for name, expected in entries.items() if name.startswith(area + '/')}
        new_hashes = {p.relative_to(root).as_posix(): digest(p.read_bytes())
                      for p in (root / area).rglob('*') if p.is_file()}
        assert old_hashes == new_hashes, area
        counts[area] = len(old_hashes)
replay = Path(sys.argv[2])
fixtures = {p.name: digest(p.read_bytes()) for p in sorted((root / 'tests/fixtures').iterdir())}
assert all(digest((replay / name).read_bytes()) == expected for name, expected in fixtures.items())
proof = {'checked_at': datetime.datetime.now().astimezone().isoformat(),
         'r4_zip_sha256': old_sha, 'r4_zip_manifest_files_verified': len(entries),
         'unchanged_r4_files_by_tree': counts, 'exact_r4_replay_fixture_hashes': fixtures}
(root / 'evidence/r5/source_integrity.json').write_text(json.dumps(proof, indent=2) + '\n', encoding='utf-8')

self_files = {'PACKAGE_SHA256.json', 'SHA256SUMS.txt'}
paths = {p.relative_to(root).as_posix(): p for p in sorted(root.rglob('*')) if p.is_file()
         and '__pycache__' not in p.parts and p.suffix != '.pyc' and p.relative_to(root).as_posix() not in self_files}
assert not any(p.suffix.lower() in ('.bin', '.elf') for p in paths.values())
entries = {name: digest(p.read_bytes()) for name, p in sorted(paths.items())}
metadata = {'release': 'D8_R5_SOURCE_20261111', 'scope': 'delivered source and evidence; ESP32 target build not run',
            'files': entries}
(root / 'PACKAGE_SHA256.json').write_text(json.dumps(metadata, indent=2) + '\n', encoding='utf-8')
(root / 'SHA256SUMS.txt').write_text(''.join(f'{entry["sha256"]}  {name}\n' for name, entry in entries.items()),
                                     encoding='utf-8')
for name in self_files:
    paths[name] = root / name

pending = artifact.with_suffix('.pending.zip')
with zipfile.ZipFile(pending, 'w', compression=zipfile.ZIP_DEFLATED, compresslevel=9) as out:
    for name, path in sorted(paths.items()):
        out.write(path, root.name + '/' + name)
with zipfile.ZipFile(pending) as check:
    prefix = root.name + '/'
    assert check.testzip() is None
    assert set(check.namelist()) == {prefix + rel for rel in paths}
    assert all(digest(check.read(prefix + rel)) == expected for rel, expected in entries.items())
    with tempfile.TemporaryDirectory(prefix='mode8-r5-verify-') as tmp:
        check.extractall(tmp)
        unpacked = Path(tmp) / root.name
        for tool, args in (('verify_package.py', []), ('sync_shared.py', ['--check'])):
            subprocess.run([sys.executable, '-B', str(unpacked / 'tools' / tool), *args], cwd=unpacked, check=True)
        blocked = subprocess.run([sys.executable, '-B', str(unpacked / 'tools/offline_preflight.py')],
                                 cwd=unpacked, capture_output=True)
        assert blocked.returncode == 2 and not json.loads(blocked.stdout)['flash_preflight_pass']
os.replace(pending, artifact)
print(json.dumps({'path': str(artifact), **digest(artifact.read_bytes()), 'zip_entries': len(paths),
                  'verified_payload_files': len(entries), 'baseline_preserved': counts,
                  'archive_preflight_exit': blocked.returncode}, indent=2))

