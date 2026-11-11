"""Replays of the R4 audit findings using host-only fixtures."""
import copy
from contextlib import redirect_stderr
from hashlib import sha256
import io
import json
from pathlib import Path
import shutil
import struct
import sys
from tempfile import TemporaryDirectory
import unittest
from unittest.mock import patch

import numpy as np

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tools'))
from analyze import packet_arrays
from fixture_helpers import synthetic_anchor
from offline_preflight import scan
from source_contract import inputs, manifest
import upload


class AnalysisRegressions(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tag = json.loads((ROOT / 'evidence/host/synthetic_tag_record.json').read_bytes())
        cls.raw = (ROOT / 'evidence/host/SYNTHETIC_CIR_i16.raw').read_bytes()

    def test_missing_poll_retains_other_radios_p1(self):
        for lost in (0, 1):
            with self.subTest(lost=lost):
                fixture = ROOT / 'tests/fixtures' / f'partial_poll_lost_{lost}'
                tag = json.loads(fixture.with_suffix('.json').read_bytes())
                raw = fixture.with_suffix('.raw').read_bytes()
                kept = 1 - lost
                self.assertIs(tag['poll_rx'][lost]['ok'], False)
                self.assertAlmostEqual(tag['ranges'][kept]['d_m'], (5.11229779897, 4.99837071270)[lost])
                arrays, rows = packet_arrays(tag, raw)
                self.assertIsNotNone(rows[kept]['range_m'])
                self.assertAlmostEqual(rows[kept]['range_m'], tag['ranges'][kept]['d_m'], places=8)
                self.assertTrue(rows[kept]['axis_estimate_available'])
                self.assertIsNone(rows[lost]['range_m'])
                self.assertTrue(np.isfinite(arrays['relative_s']).all())
                self.assertEqual(arrays['common_p1_delay_s'].size, 0)
                self.assertEqual(arrays['iq_int16'].astype('<i2').tobytes(), raw)

    def test_conflicting_poll_cannot_be_disguised_as_missed(self):
        tag = copy.deepcopy(self.tag)
        tag['poll_rx'][0]['ok'] = False  # Still carries a valid CRC and RF frame.
        arrays, rows = packet_arrays(tag, self.raw)
        self.assertTrue(all(row['range_m'] is None for row in rows))
        self.assertTrue(np.isfinite(arrays['relative_s']).all())

    def test_anchor_tx_slot_mutations_block_p2_with_or_without_static_reuse(self):
        anchor = synthetic_anchor(self.tag)
        _, baseline = packet_arrays(self.tag, self.raw, anchor)
        self.assertTrue(all(row['axis_estimate_available'] for row in baseline))
        mutations = ((0, 'planned', anchor['tx'][0]['planned'] + 1),
                     (1, 'type', 5), (2, 'radio', 1),
                     (2, 'actual', anchor['tx'][2]['actual'] + 1),
                     (0, 'ok', 1))
        for slot, field, value in mutations:
            bad = copy.deepcopy(anchor)
            bad['tx'][slot][field] = value
            for assume_static in (False, True):
                with self.subTest(slot=slot, field=field, assume_static=assume_static):
                    arrays, rows = packet_arrays(self.tag, self.raw, bad, assume_static=assume_static)
                    self.assertEqual([row['range_m'] for row in rows[:2]],
                                     [row['range_m'] for row in baseline[:2]])
                    self.assertTrue(all(row['range_m'] is None for row in rows[2:]))
                    self.assertTrue(np.isnan(arrays['physical_estimate_s'][2:]).all())
                    self.assertEqual(arrays['common_p2_delay_s'].size, 0)
                    self.assertTrue(np.isfinite(arrays['relative_s']).all())


def fixture_receipt(root):
    """Construct only the byte structure checked by offline_preflight."""
    fqbn = 'esp32:esp32:esp32doit-devkit-v1'
    receipt = {'target_build': True, 'fqbn': fqbn, 'required_core': '3.3.7', 'roles': {}}
    for role in ('anchor', 'tag'):
        for src in inputs(ROOT, role):
            dst = root / src.relative_to(ROOT)
            dst.parent.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(src, dst)
        app = bytes((0xe9, 1)) + bytes(300)
        elf = bytearray(64)
        elf[:5] = b'\x7fELF\x01'
        struct.pack_into('<H', elf, 18, 94)
        components = {'bin': app, 'bootloader.bin': b'test bootloader',
                      'partitions.bin': b'test partition table', 'elf': elf}
        merged = bytearray(0x10000 + len(app))
        for name, offset in (('bootloader.bin', 0x1000), ('partitions.bin', 0x8000), ('bin', 0x10000)):
            data = components[name]
            merged[offset:offset+len(data)] = data
        components['merged.bin'] = merged
        out = root / 'dist' / role
        out.mkdir(parents=True)
        artifacts = {}
        for suffix, data in components.items():
            name = f'{role.upper()}_DUAL_DS_CIR.ino.{suffix}'
            (out / name).write_bytes(data)
            artifacts[name] = {'bytes': len(data), 'sha256': sha256(data).hexdigest()}
        receipt['roles'][role] = {'source_manifest': manifest(root, role), 'artifacts': artifacts}
    write_receipt(root, receipt)
    return receipt


def write_receipt(root, receipt):
    (root / 'dist/target_build_receipt.json').write_text(json.dumps(receipt), encoding='utf-8')


class BuildProvenanceRegressions(unittest.TestCase):
    def test_sketch_overrides_invalidate_existing_images(self):
        overrides = (
            ('anchor', 'partitions.csv', b'spiffs,data,spiffs,0x290000,0x150000,\n'),
            ('tag', 'bootloader.bin', b'\xe9\x01boot override'),
        )
        for role, filename, data in overrides:
            with self.subTest(role=role, filename=filename), TemporaryDirectory() as tmp:
                root = Path(tmp)
                receipt = fixture_receipt(root)
                self.assertTrue(scan(root)['flash_preflight_pass'])
                sketch = root / 'firmware' / f'{role.upper()}_DUAL_DS_CIR'
                override = sketch / filename
                override.write_bytes(data)
                self.assert_stale(root, role)
                # Simulate a new receipt for the input inventory to exercise edits
                # and removals. The fixture does not compile or flash any image.
                receipt['roles'][role]['source_manifest'] = manifest(root, role)
                write_receipt(root, receipt)
                self.assertTrue(scan(root)['flash_preflight_pass'])
                override.write_bytes(data + b'\n')
                self.assert_stale(root, role)
                override.unlink()
                self.assert_stale(root, role)

    def assert_stale(self, root, role):
        result = scan(root)
        self.assertFalse(result['flash_preflight_pass'])
        self.assertFalse(result['roles'][role]['checks']['source_binding'])
        err = io.StringIO()
        with patch.object(upload, 'ROOT', root), patch.object(sys, 'argv',
             ['upload.py', '--role', role, '--port', 'FAKE_PORT']), \
             patch.object(upload.shutil, 'which', return_value='FAKE_CLI'), \
             patch.object(upload.subprocess, 'run') as called, redirect_stderr(err):
            with self.assertRaises(SystemExit) as caught:
                upload.main()
        self.assertEqual(caught.exception.code, 2)
        self.assertIn('provenance', err.getvalue())
        called.assert_not_called()

    def test_generated_exports_are_excluded_and_legacy_receipts_are_refused(self):
        with TemporaryDirectory() as tmp:
            root = Path(tmp)
            receipt = fixture_receipt(root)
            exports = root / 'firmware/ANCHOR_DUAL_DS_CIR/build/esp32.esp32.esp32doit-devkit-v1'
            exports.mkdir(parents=True)
            (exports / 'ANCHOR_DUAL_DS_CIR.ino.bin').write_bytes(b'generated image')
            (exports / 'partitions.csv').write_bytes(b'generated partition table')
            self.assertTrue(scan(root)['flash_preflight_pass'])
            receipt['roles']['anchor']['source_manifest'].pop('version', None)
            write_receipt(root, receipt)
            self.assert_stale(root, 'anchor')
