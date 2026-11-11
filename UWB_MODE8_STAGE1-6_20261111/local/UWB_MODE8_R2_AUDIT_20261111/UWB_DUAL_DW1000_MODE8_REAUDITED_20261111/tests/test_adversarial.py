"""Adversarial probes: repaired failures must fail closed (synthetic only)."""
from __future__ import annotations
import copy
import json
from pathlib import Path
from queue import Queue, Empty
import sys
import tempfile
from types import SimpleNamespace
from unittest.mock import patch
import unittest
import zlib
import numpy as np

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT/'tools'))
from analyze import packet_arrays
from rfb8 import encode, RAW_BYTES
from capture_audit import verify_capture
from fixture_helpers import synthetic_anchor, with_txn
import run_pair


def altered_frame(h, offset, data):
    b = bytearray.fromhex(h)
    b[offset:offset+len(data)] = data
    b[92:96] = zlib.crc32(b[:92]).to_bytes(4, 'little')
    return b.hex()


def rows_for(meta):
    raw = (ROOT/'evidence/host/SYNTHETIC_CIR_i16.raw').read_bytes()
    anchor = synthetic_anchor(meta)
    return packet_arrays(meta, raw, anchor)


def all_invalid(arrays, rows):
    return all(not r['axis_estimate_available'] for r in rows) and np.isnan(arrays['physical_estimate_s']).all()


class AdversarialAnalysis(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.m = json.loads((ROOT/'evidence/host/synthetic_tag_record.json').read_text())
        cls.raw = (ROOT/'evidence/host/SYNTHETIC_CIR_i16.raw').read_bytes()

    def test_clean_both_radios_have_physical_axes(self):
        arrays, rows = rows_for(self.m)
        self.assertTrue(all(x['axis_estimate_available'] for x in rows))
        self.assertGreater(arrays['common_p2_delay_s'].size, 992)

    def test_reported_p2_collision_blocks_axes_and_common_grid(self):
        m = copy.deepcopy(self.m)
        b = bytearray.fromhex(m['probe_rx'][3]['air'])
        b[42:47] = (int.from_bytes(b[42:47], 'little')+256).to_bytes(5, 'little')
        b[92:96] = zlib.crc32(b[:92]).to_bytes(4, 'little')
        m['probe_rx'][3]['air'] = b.hex()
        m.update(metadata_conflict=True, p1_tx_confirmed=False, complete=False, errors=64)
        for r in m['ranges']:
            r.update(math_valid=False, d_m=None, six_ts=[0]*6)
        arrays, rows = packet_arrays(m, self.raw, synthetic_anchor(self.m))
        self.assertTrue(all_invalid(arrays, rows))
        self.assertTrue(np.isfinite(arrays['relative_s']).all())
        self.assertEqual(arrays['common_p2_delay_s'].size, 0)

    def test_unreported_p2_collision_also_blocks_axes(self):
        m = copy.deepcopy(self.m)
        m['probe_rx'][3]['air'] = altered_frame(m['probe_rx'][3]['air'], 42, b'\x00'*5)
        arrays, rows = packet_arrays(m, self.raw, synthetic_anchor(self.m))
        self.assertTrue(all_invalid(arrays, rows))
        self.assertFalse(rows[3]['frame_provenance_ok'])

    def test_unreported_p1_collision_also_blocks_axes(self):
        m = copy.deepcopy(self.m)
        m['probe_rx'][1]['air'] = altered_frame(m['probe_rx'][1]['air'], 37, b'\x00'*5)
        arrays, rows = packet_arrays(m, self.raw, synthetic_anchor(self.m))
        self.assertTrue(all_invalid(arrays, rows))

    def test_inconsistent_shared_p2_and_individual_received_frames(self):
        m = copy.deepcopy(self.m)
        m['frames'][2] = altered_frame(m['frames'][2], 42, b'\x00'*5)
        arrays, rows = packet_arrays(m, self.raw, synthetic_anchor(m))
        self.assertTrue(all_invalid(arrays, rows))

    def test_flag_alone_blocks_p1_and_p2_physical_axes(self):
        m = copy.deepcopy(self.m)
        m['metadata_conflict'] = True
        arrays, rows = rows_for(m)
        self.assertTrue(all_invalid(arrays, rows))

    def test_invalid_p1_six_ts_not_accepted(self):
        m = copy.deepcopy(self.m)
        m['ranges'][0]['six_ts'][1] += 256
        arrays, rows = rows_for(m)
        self.assertFalse(rows[0]['axis_estimate_available'])
        self.assertTrue(rows[1]['axis_estimate_available'])

    def test_missing_anchor_rx_evidence_blocks_p2_only(self):
        anchor = synthetic_anchor(self.m)
        anchor.pop('response_rx')
        arrays, rows = packet_arrays(self.m, self.raw, anchor)
        self.assertTrue(rows[0]['axis_estimate_available'])
        self.assertTrue(rows[1]['axis_estimate_available'])
        self.assertFalse(rows[2]['axis_estimate_available'])
        self.assertFalse(rows[3]['axis_estimate_available'])

    def test_changed_p2_actual_tx_blocks_p2_only(self):
        anchor = synthetic_anchor(self.m)
        anchor['tx'][2]['actual'] += 1
        arrays, rows = packet_arrays(self.m, self.raw, anchor)
        self.assertTrue(all(r['axis_estimate_available'] for r in rows[:2]))
        self.assertTrue(all(not r['axis_estimate_available'] for r in rows[2:]))

    def test_false_antenna_footprint_static_reuse_cannot_bypass_conflict(self):
        m = copy.deepcopy(self.m)
        m['metadata_conflict'] = True
        arrays, rows = packet_arrays(m, self.raw, synthetic_anchor(m), assume_static=True)
        self.assertTrue(all_invalid(arrays, rows))

    def test_missing_one_p2_radio_keeps_other_partial_result(self):
        m = copy.deepcopy(self.m)
        m['probe_rx'][3].update(ok=False, air='')
        m['cir'][3].update(valid=False, present=False)
        arrays, rows = packet_arrays(m, self.raw, synthetic_anchor(m))
        self.assertTrue(rows[0]['axis_estimate_available'])
        self.assertTrue(rows[1]['axis_estimate_available'])
        self.assertTrue(rows[2]['axis_estimate_available'])
        self.assertFalse(rows[3]['axis_estimate_available'])


class AdversarialReceipt(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tag = json.loads((ROOT/'evidence/host/synthetic_tag_record.json').read_text())
        cls.tag['_raw_integrity_verified'] = True  # independent known synthetic CIR fixture
        cls.hellos = {
            'tag': {'role': 'tag', 'boot': cls.tag['boot'], 'config_id': cls.tag['config'],
                    'hw_ok': True, 'fault_latched': False, 'stage': 5, 'period_us': 300000},
            'anchor': {'role': 'anchor', 'boot': cls.tag['peer_boot'], 'config_id': cls.tag['config'],
                       'hw_ok': True, 'fault_latched': False, 'stage': 5, 'period_us': 300000}}
        cls.stats = {role: {'capture_decoder': {k: 0 for k in
                     ('crc_errors', 'schema_errors', 'sequence_gap_events', 'trailing_bytes', 'skipped_bytes')},
                     'reader_error': None, 'fault_events': 0} for role in ('tag', 'anchor')}

    def audit(self, tag=None, anchor=None, hello=None, stats=None, count=1):
        tags = [self.tag] if tag is None else tag
        anchors = [synthetic_anchor(x) for x in tags] if anchor is None else anchor
        return verify_capture(session=self.tag['session'], count=count, stage=5, period_us=300000,
                              identities=hello or self.hellos,
                              records={'tag': tags, 'anchor': anchors}, stats=stats or self.stats)

    def test_good_pair_and_two_sequential_txns(self):
        self.assertTrue(self.audit().passed)
        tags = [self.tag, with_txn(self.tag, 1)]
        self.assertTrue(self.audit(tag=tags, count=2).passed)

    def test_different_session_rejected(self):
        t = copy.deepcopy(self.tag)
        t['session'] = 76
        self.assertFalse(self.audit(tag=[t]).passed)

    def test_changed_boot_and_peer_boot_rejected(self):
        t = copy.deepcopy(self.tag)
        t['boot'] = 999
        self.assertFalse(self.audit(tag=[t]).passed)
        t = copy.deepcopy(self.tag)
        t['peer_boot'] = 999
        self.assertFalse(self.audit(tag=[t]).passed)

    def test_duplicate_and_missing_transaction(self):
        tags = [self.tag, self.tag]
        a = self.audit(tag=tags, count=2)
        self.assertFalse(a.passed)
        self.assertTrue(any('DUPLICATE' in x for x in a.problems))
        tags = [with_txn(self.tag, 1)]
        self.assertFalse(self.audit(tag=tags, count=2).passed)

    def test_out_of_order_and_extra(self):
        tags = [with_txn(self.tag, 1), self.tag]
        self.assertFalse(self.audit(tag=tags, count=2).passed)
        self.assertFalse(self.audit(tag=[self.tag, self.tag]).passed)

    def test_only_one_side(self):
        self.assertFalse(self.audit(anchor=[]).passed)
        self.assertFalse(self.audit(tag=[]).passed)

    def test_incomplete_nonzero_errors_metadata_collision(self):
        for key, value in [('complete', False), ('errors', 32), ('metadata_conflict', True)]:
            t = copy.deepcopy(self.tag)
            t[key] = value
            self.assertFalse(self.audit(tag=[t]).passed)

    def test_decoder_gaps_crc_trailing_and_fault_event(self):
        for key in ('sequence_gap_events', 'crc_errors', 'schema_errors', 'trailing_bytes', 'skipped_bytes'):
            stat = copy.deepcopy(self.stats)
            stat['tag']['capture_decoder'][key] = 1
            self.assertFalse(self.audit(stats=stat).passed)
        stat = copy.deepcopy(self.stats)
        stat['tag']['fault_events'] = 1
        self.assertFalse(self.audit(stats=stat).passed)
        stat = copy.deepcopy(self.stats)
        stat['anchor']['reader_error'] = 'OSError'
        self.assertFalse(self.audit(stats=stat).passed)

    def test_wrong_counterpart_air_payload(self):
        anchor = synthetic_anchor(self.tag)
        anchor['frames'][2] = altered_frame(anchor['frames'][2], 42, b'\x00'*5)
        self.assertFalse(self.audit(anchor=[anchor]).passed)

    def test_record_air_different_than_shared_frame(self):
        t = copy.deepcopy(self.tag)
        t['probe_rx'][3]['air'] = altered_frame(t['probe_rx'][3]['air'], 42, b'\x00'*5)
        self.assertFalse(self.audit(tag=[t]).passed)

    def test_raw_cir_crc_mismatch_rejected(self):
        t = copy.deepcopy(self.tag)
        t['_raw_integrity_verified'] = False
        self.assertFalse(self.audit(tag=[t]).passed)

    def test_anchor_claims_wrong_actual_tx(self):
        anchor = synthetic_anchor(self.tag)
        anchor['tx'][2]['actual'] += 1
        self.assertFalse(self.audit(anchor=[anchor]).passed)


class AdversarialRunPair(unittest.TestCase):
    class InMemorySerial:
        def __init__(self, **_kwargs):
            self.port = None
            self.incoming = Queue()
            self.sequence = 0
            self.stage = 5
            self.period = 300000

        @property
        def in_waiting(self):
            return 65536

        def open(self):
            pass

        def close(self):
            pass

        def flush(self):
            pass

        def read(self, _size):
            try:
                return self.incoming.get(timeout=0.02)
            except Empty:
                return b''

        def push(self, meta, *, kind=1, raw=b''):
            self.incoming.put(encode(meta, raw, kind=kind, sequence=self.sequence))
            self.sequence += 1

        def hello(self, *, active=False):
            self.push({'event': 'HELLO', 'role': self.port, 'hw_ok': True,
                       'boot': 101 if self.port == 'anchor' else 202,
                       'config_id': 42, 'active': active,
                       'stage': self.stage, 'period_us': self.period})

        def write(self, data):
            t = data.decode('ascii').split()
            if t == ['STOP']:
                self.push({'event': 'STOPPED', 'role': self.port})
            elif t == ['HELLO']:
                self.hello()
            elif t == ['SELFTEST']:
                self.push({'event': 'SELFTEST', 'role': self.port, 'pass': True})
            elif t[:2] == ['SET', 'STAGE']:
                self.stage = int(t[2]); self.hello()
            elif t[:2] == ['SET', 'PERIOD_US']:
                self.period = int(t[2]); self.hello()
            elif t == ['START', '77', '2']:
                self.hello(active=True)
                raw = bytes(RAW_BYTES) if self.port == 'tag' else b''
                record = {'schema': 'dual8.record.v1', 'mode': 8, 'role': self.port,
                          'session': 76, 'txn': 0, 'boot': 999, 'config': 42,
                          'complete': True}
                self.push(record, kind=2, raw=raw)
                self.sequence += 1
                self.push(copy.copy(record), kind=2, raw=raw)
            else:
                raise AssertionError(t)
            return len(data)

    def test_original_false_pass_reproduction_is_rejected(self):
        with tempfile.TemporaryDirectory(prefix='dual8-adversarial-') as tmp:
            argv = ['run_pair.py', '--anchor', 'anchor', '--tag', 'tag', '--out', tmp, '--count', '2']
            with patch.object(sys, 'argv', argv), \
                 patch.dict(sys.modules, {'serial': SimpleNamespace(Serial=self.InMemorySerial)}), \
                 patch.object(run_pair.secrets, 'randbelow', return_value=76), \
                 patch('builtins.print'):
                exit_code = run_pair.main()
            result = json.loads((Path(tmp)/'capture_receipt.json').read_text())
        self.assertEqual(exit_code, 2)
        self.assertFalse(result['pass'])
        self.assertGreater(result['tag_capture']['capture_decoder']['sequence_gap_events'], 0)
        self.assertIn('audit', result)
        self.assertFalse(result['audit']['pass'])


class GoodRunPairSerial(AdversarialRunPair.InMemorySerial):
    """Two complete transactions from matching boots, RF frames and serial IDs."""
    def hello(self, *, active=False):
        fixture = json.loads((ROOT/'evidence/host/synthetic_tag_record.json').read_text())
        self.push({'event': 'HELLO', 'role': self.port, 'hw_ok': True,
                   'boot': 101 if self.port == 'anchor' else 202,
                   'config_id': fixture['config'], 'active': active,
                   'stage': self.stage, 'period_us': self.period})

    def write(self, data):
        t = data.decode('ascii').split()
        if t == ['START', '77', '2']:
            self.hello(active=True)
            source = json.loads((ROOT/'evidence/host/synthetic_tag_record.json').read_text())
            raw = (ROOT/'evidence/host/SYNTHETIC_CIR_i16.raw').read_bytes()
            for txn in (0, 1):
                tag = with_txn(source, txn)
                record = tag if self.port == 'tag' else synthetic_anchor(tag)
                self.push(record, kind=2, raw=raw if self.port == 'tag' else b'')
            return len(data)
        return super().write(data)


class GoodRunPairSmoke(unittest.TestCase):
    def test_legitimate_hardware_capture_simulation_passes(self):
        with tempfile.TemporaryDirectory(prefix='dual8-clean-pair-') as tmp:
            argv = ['run_pair.py', '--anchor', 'anchor', '--tag', 'tag', '--out', tmp, '--count', '2']
            with patch.object(sys, 'argv', argv), \
                 patch.dict(sys.modules, {'serial': SimpleNamespace(Serial=GoodRunPairSerial)}), \
                 patch.object(run_pair.secrets, 'randbelow', return_value=76), \
                 patch('builtins.print'):
                exit_code = run_pair.main()
            result = json.loads((Path(tmp)/'capture_receipt.json').read_text())
        self.assertEqual(exit_code, 0, result)
        self.assertTrue(result['pass'])
        self.assertTrue(result['audit']['pass'])


if __name__ == "__main__":
    unittest.main(verbosity=2)
