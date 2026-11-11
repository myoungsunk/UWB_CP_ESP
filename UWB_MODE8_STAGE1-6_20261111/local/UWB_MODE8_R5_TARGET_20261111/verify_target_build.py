from datetime import datetime, timezone
from hashlib import sha256
import json
from pathlib import Path
from tempfile import TemporaryDirectory
import re
import subprocess
import sys
from zipfile import ZIP_DEFLATED, ZipFile

root = Path.cwd()
dist = root / 'dist'
source_zip, esptool, nm = map(Path, sys.argv[1:])
expected_source_hash = '2f837598ad45f6de0cdf4462bdde29238957aadaf188fad5851c2e6aa9dbc7c0'

def digest(path):
    data = path.read_bytes()
    return {'bytes': len(data), 'sha256': sha256(data).hexdigest()}

def load(path):
    return json.loads(path.read_bytes())

assert digest(source_zip)['sha256'] == expected_source_hash
original_manifest = load(root / 'PACKAGE_SHA256.json')['files']
for rel, expected in original_manifest.items():
    if rel != 'dist/target_build_receipt.json':
        assert digest(root / rel) == expected, rel
receipt_path = dist / 'target_build_receipt.json'
receipt = load(receipt_path)
assert receipt['target_build'] is True
assert receipt['required_core'] == '3.3.7'
assert receipt['fqbn'] == 'esp32:esp32:esp32doit-devkit-v1'

preflight_path = dist / 'offline_preflight.json'
preflight_cmd = [sys.executable, '-B', 'tools/offline_preflight.py', '--out', 'dist/offline_preflight.json']
preflight_run = subprocess.run(preflight_cmd, cwd=root, capture_output=True, check=False)
print(preflight_run.stdout.decode('utf-8'))
if preflight_run.returncode:
    print(preflight_run.stderr.decode('utf-8', errors='replace'), file=sys.stderr)
    sys.exit(preflight_run.returncode)
preflight = load(preflight_path)
assert preflight['flash_preflight_pass'] is True and preflight['offline_only'] is True

role_results = {}
bundle_files = [receipt_path, preflight_path]
for role in ('anchor', 'tag'):
    info = receipt['roles'][role]
    assert info['exit_code'] == 0 and info['source_manifest']['version'] == 2
    out = dist / role
    app = out / f'{role.upper()}_DUAL_DS_CIR.ino.bin'
    elf = app.with_suffix('.elf')
    for name, expected in info['artifacts'].items():
        file = out / name
        assert digest(file) == expected, file
        bundle_files.append(file)
    assert app.name in info['artifacts'] and elf.name in info['artifacts']
    compile_log = out / 'compile.log'
    compile_text = compile_log.read_text(encoding='utf-8', errors='replace')
    bundle_files.append(compile_log)
    memory = re.search(r'Sketch uses (\d+) bytes .*?Maximum is (\d+) bytes', compile_text)
    ram = re.search(r'Global variables use (\d+) bytes .*?Maximum is (\d+) bytes', compile_text)
    assert memory and ram, compile_log
    image_cmd = [str(esptool), '--chip', 'esp32', 'image-info', str(app)]
    image = subprocess.run(image_cmd, capture_output=True, check=True)
    image_text = image.stdout.decode('utf-8', errors='replace')
    checksum_valid = bool(re.search(r'(?m)^Checksum:.*\(valid\)', image_text))
    validation_hash_valid = bool(re.search(r'(?m)^Validation hash:.*\(valid\)', image_text))
    image_log = out / 'image_info.log'
    image_log.write_text('COMMAND: ' + json.dumps(image_cmd) + '\n' + image_text, encoding='utf-8')
    bundle_files.append(image_log)
    assert checksum_valid and validation_hash_valid, image_log
    symbols_cmd = [str(nm), '-C', '--defined-only', str(elf)]
    symbols = subprocess.run(symbols_cmd, capture_output=True, check=True).stdout.decode('utf-8', errors='replace')
    rc_phase_symbols = [line for line in symbols.splitlines() if re.search(r'\b(getRCphase|readRCphase)\b', line)]
    linker_map = app.with_suffix('.map').read_text(encoding='utf-8', errors='replace')
    discarded = linker_map.split('Discarded input sections', 1)[1].split('Memory Configuration', 1)[0]
    rc_phase_discarded = all(name in discarded for name in ('getRCphase', 'readRCphase'))
    warnings = [line for line in compile_text.splitlines() if 'warning:' in line]
    role_results[role] = {
        'compile_exit': info['exit_code'], 'flash_preflight_pass': preflight['roles'][role]['flash_preflight_pass'],
        'compile_warnings': len(warnings), 'array_bounds_warnings': [line for line in warnings if '-Warray-bounds' in line],
        'rc_phase_symbol_check': symbols_cmd, 'rc_phase_symbols': rc_phase_symbols,
        'rc_phase_sections_discarded': rc_phase_discarded,
        'esptool_image_info_exit': image.returncode,
        'esptool_checksum_valid': checksum_valid, 'esptool_validation_hash_valid': validation_hash_valid,
        'flash_used_bytes': int(memory[1]), 'flash_max_bytes': int(memory[2]),
        'static_ram_used_bytes': int(ram[1]), 'static_ram_max_bytes': int(ram[2]),
        'app': {'filename': app.name, **digest(app)}, 'elf': {'filename': elf.name, **digest(elf)},
    }
assert role_results['anchor']['app']['sha256'] != role_results['tag']['app']['sha256']
summary = {
    'verified_utc': datetime.now(timezone.utc).isoformat(), 'question_id': 'UWB-MODE8-SOURCE-HOST-QA-20261111',
    'release': 'D8_R5_SOURCE_20261111', 'source_archive': source_zip.name,
    'source_archive_sha256': expected_source_hash, 'audited_files_reverified_after_build': len(original_manifest) - 1,
    'arduino_cli': receipt['arduino_cli'], 'core': receipt['required_core'], 'fqbn': receipt['fqbn'],
    'target_build_pass': True, 'preflight_command': ['python', *preflight_cmd[1:]],
    'preflight_exit': preflight_run.returncode, 'flash_preflight_pass': True,
    'flash_performed': False, 'hardware_tested': False,
    'research_governance_sha256': {
        'COMMON_CONTRACT.md': 'a5cca7d8a8d5f20f4a89ea9b6509da0b8d7506626aac4a47fa1d27891ff5ebdb',
        'ROLE_IMPLEMENTER.md': '2380fb1fe7302870a6c2d825cc2e65a73e91138677e095b4e51db43ab9ea90da',
    },
    'roles': role_results,
}
summary_path = dist / 'target_build_summary.json'
summary_path.write_text(json.dumps(summary, indent=2) + '\n', encoding='utf-8')
bundle_files.append(summary_path)

archive = root.parent / 'UWB_DUAL_DW1000_MODE8_R5_ESP32_TARGET_BUILD_20261111.zip'
assert not archive.exists(), archive
pending = archive.with_suffix('.pending.zip')
entries = {file.relative_to(root).as_posix(): file for file in bundle_files}
with ZipFile(pending, 'w', ZIP_DEFLATED, compresslevel=6) as z:
    for name, file in sorted(entries.items()):
        z.write(file, name)
with ZipFile(pending) as z:
    assert z.testzip() is None
    assert set(z.namelist()) == set(entries)
    assert all(sha256(z.read(name)).hexdigest() == digest(file)['sha256'] for name, file in entries.items())
    with TemporaryDirectory(prefix='mode8-r5-target-verify-') as tmp:
        with ZipFile(source_zip) as frozen:
            frozen.extractall(tmp)
        unpacked = Path(tmp) / root.name
        z.extractall(unpacked)
        bundle_preflight = subprocess.run([sys.executable, '-B', 'tools/offline_preflight.py'],
                                          cwd=unpacked, capture_output=True, check=True)
        assert json.loads(bundle_preflight.stdout)['flash_preflight_pass'] is True
pending.rename(archive)
print(json.dumps({'archive': str(archive), **digest(archive), 'entries': len(entries), 'archive_preflight_exit': bundle_preflight.returncode, 'roles': role_results}, indent=2))

