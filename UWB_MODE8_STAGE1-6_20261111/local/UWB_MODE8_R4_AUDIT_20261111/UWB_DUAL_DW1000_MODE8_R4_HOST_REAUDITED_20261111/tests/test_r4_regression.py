"""R4 targeted red-team regressions; no radio hardware required."""
from __future__ import annotations
import copy
import json
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest
import io
from contextlib import redirect_stderr
from unittest.mock import patch

ROOT=Path(__file__).resolve().parents[1]
sys.path.insert(0,str(ROOT/'tools'))
sys.path.insert(0,str(ROOT/'tests'))
from analyze import packet_arrays
from capture_audit import verify_capture
from fixture_helpers import synthetic_anchor
from run_pair import cir_payload_integrity
from source_contract import inputs,manifest,matches
from offline_preflight import scan
import upload


class ContractRegressions(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.m=json.loads((ROOT/'evidence/host/synthetic_tag_record.json').read_text())
        cls.raw=(ROOT/'evidence/host/SYNTHETIC_CIR_i16.raw').read_bytes()
        cls.anchor=synthetic_anchor(cls.m)
        cls.identity={role:{'role':role,'boot':cls.m['boot'] if role=='tag' else cls.m['peer_boot'],
                            'config_id':cls.m['config'],'hw_ok':True,'fault_latched':False,
                            'stage':cls.m['stage'],'period_us':cls.m['period_us']}
                      for role in ('tag','anchor')}
        cls.stats={role:{'capture_decoder':{k:0 for k in ('crc_errors','schema_errors',
                        'sequence_gap_events','trailing_bytes','skipped_bytes')},
                        'reader_error':None,'fault_events':0} for role in ('tag','anchor')}
    def audit(self,m,anchor=None):
        m['_raw_integrity_verified']=cir_payload_integrity(m,self.raw)
        return verify_capture(session=self.m['session'],count=1,stage=self.m['stage'],
                              period_us=self.m['period_us'],identities=self.identity,
                              records={'tag':[m],'anchor':[anchor or self.anchor]},stats=self.stats)
    def test_legitimate_capture_still_passes(self):
        self.assertTrue(self.audit(copy.deepcopy(self.m)).passed)
        arrays,rows=packet_arrays(self.m,self.raw,self.anchor)
        self.assertTrue(all(r['axis_estimate_available'] for r in rows))
    def test_adversarial_cir_slot_radio_and_probe_swap(self):
        for slot,key,value in ((1,'radio',0),(3,'probe',1),(0,'slot',3)):
            with self.subTest(slot=slot,key=key):
                m=copy.deepcopy(self.m);m['cir'][slot][key]=value
                self.assertFalse(self.audit(m).passed)
                self.assertFalse(cir_payload_integrity(m,self.raw))
                arrays,rows=packet_arrays(m,self.raw,self.anchor)
                self.assertFalse(rows[slot]['axis_estimate_available'])
                self.assertFalse(rows[slot]['cir_valid'])
    def test_adversarial_rx_radio_or_type_swap(self):
        for slot,key,value in ((3,'radio',0),(3,'type',4),(0,'type',5)):
            with self.subTest(slot=slot,key=key):
                m=copy.deepcopy(self.m);m['probe_rx'][slot][key]=value
                self.assertFalse(self.audit(m).passed)
                _,rows=packet_arrays(m,self.raw,self.anchor)
                self.assertFalse(rows[slot]['axis_estimate_available'])
    def test_poll_rx_type_and_radio_swap(self):
        for key,value in (('radio',1),('type',4)):
            m=copy.deepcopy(self.m);m['poll_rx'][0][key]=value
            self.assertFalse(self.audit(m).passed)
            _,rows=packet_arrays(m,self.raw,self.anchor)
            self.assertFalse(any(r['axis_estimate_available'] for r in rows))
    def test_anchor_tx_identity_and_response_identity(self):
        for category,slot,key,value in (('tx',2,'radio',1),('tx',1,'type',5),
                                        ('tx',0,'planned',999),('response_rx',1,'radio',1),
                                        ('response_rx',0,'type',3)):
            with self.subTest(category=category,slot=slot,key=key):
                a=copy.deepcopy(self.anchor);a[category][slot][key]=value
                self.assertFalse(self.audit(copy.deepcopy(self.m),anchor=a).passed)
                _,rows=packet_arrays(self.m,self.raw,a)
                if category == 'response_rx':
                    self.assertFalse(rows[2+slot]['axis_estimate_available'])
    def test_vendor_fcs_contract_and_fixed_buffer(self):
        for role in ('tag','anchor'):
            port=(ROOT/'shared/esp32_port.hpp').read_text()
            vendor=(ROOT/f'vendor/{role}/DW1000Ng/src/DW1000Ng.cpp').read_text()
            self.assertIn('n += 2',vendor[vendor.find('void setTransmitData(byte data[]'):][:450])
            self.assertIn('uint8_t data[AIR_BYTES+2]',port)
            self.assertNotIn('uint8_t data[AIR_BYTES],d[5]',port)
    def test_stale_flash_artifact_refused(self):
        # Prove the guard on a stale receipt, regardless of whether the developer
        # later builds the real R4 release (which should be permitted).
        with tempfile.TemporaryDirectory() as tmp:
            root=Path(tmp)
            for file in inputs(ROOT,'tag'):
                target=root/file.relative_to(ROOT)
                target.parent.mkdir(parents=True,exist_ok=True)
                shutil.copyfile(file,target)
            dist=root/'dist'
            dist.mkdir()
            (dist/'target_build_receipt.json').write_text(json.dumps({
                'target_build':True,'fqbn':'esp32:esp32:esp32doit-devkit-v1',
                'roles':{'tag':{'source_manifest':{},'artifacts':{}}}}))
            self.assertFalse(scan(root)['flash_preflight_pass'])
            err=io.StringIO()
            with patch.object(upload,'ROOT',root), patch.object(sys,'argv',
                     ['upload.py','--role','tag','--port','FAKE_PORT']), redirect_stderr(err):
                with self.assertRaises(SystemExit) as exit_info:
                    upload.main()
            self.assertEqual(exit_info.exception.code,2)
            self.assertIn('provenance',err.getvalue())
    def test_source_hash_changes_on_edit(self):
        with tempfile.TemporaryDirectory() as tmp:
            folder=Path(tmp)
            for f in inputs(ROOT,'tag'):
                dst=folder/f.relative_to(ROOT);dst.parent.mkdir(parents=True,exist_ok=True)
                shutil.copyfile(f,dst)
            original=manifest(folder,'tag')
            self.assertTrue(matches(folder,'tag',original))
            target=folder/'shared/esp32_port.hpp'
            target.write_bytes(target.read_bytes()+b'\n// regression mutation\n')
            self.assertFalse(matches(folder,'tag',original))

if __name__=='__main__':
    unittest.main()
