from __future__ import annotations
import copy
import json
from pathlib import Path
import struct
import sys
import unittest
import zlib
import numpy as np
ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT/"tools"))
from rfb8 import Decoder, encode, HEADER, RAW_BYTES
from analyze import packet_arrays, ds_twr, air_frame, C_AIR
from fixture_helpers import synthetic_anchor

class ToolsTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.meta = json.loads((ROOT/"evidence/host/synthetic_tag_record.json").read_text())
        cls.raw = (ROOT/"evidence/host/SYNTHETIC_CIR_i16.raw").read_bytes()
        cls.wire = encode(cls.meta, cls.raw, sequence=0)

    def test_stream_chunk_boundaries(self):
        for step in (1, 2, 3, 7, 23, 24, 25, 63, 256, 1024, 65536):
            d=Decoder();out=[]
            for i in range(0,len(self.wire),step): out += d.feed(self.wire[i:i+step])
            self.assertEqual(len(out),1)
            self.assertEqual(out[0].raw,self.raw)
            self.assertEqual(d.crc_errors,0)
            self.assertEqual(d.schema_errors,0)
            self.assertEqual(len(d.buffer),0)

    def test_crc_recovery_and_boot_noise(self):
        broken=bytearray(self.wire);broken[100]^=1
        d=Decoder();out=d.feed(b"BOOT_ROM_JUNK\r\n"+broken+self.wire)
        self.assertEqual(len(out),1);self.assertEqual(d.crc_errors,1)
        self.assertGreater(d.skipped_bytes,0)

    def test_huge_length_is_rejected(self):
        head=HEADER.pack(b"RFB8",1,2,24,0,0xffffffff,RAW_BYTES,0)
        d=Decoder();out=d.feed(head+self.wire)
        self.assertEqual(len(out),1);self.assertEqual(d.schema_errors,1)

    def test_truncated_payload_not_yielded(self):
        d=Decoder();self.assertEqual(d.feed(self.wire[:-1]),[])
        self.assertEqual(len(d.feed(self.wire[-1:])),1)

    def test_sequence_gap_count(self):
        d=Decoder();d.feed(self.wire+encode(self.meta,self.raw,sequence=2))
        self.assertEqual(d.sequence_gaps,1)

    def test_no_silent_role_raw_mismatch(self):
        m=copy.deepcopy(self.meta);m["role"]="anchor"
        d=Decoder();self.assertEqual(d.feed(encode(m,self.raw)),[])
        self.assertEqual(d.schema_errors,1)

    def test_exact_independent_ds(self):
        for r in self.meta["ranges"]:
            value,ok=ds_twr(r["six_ts"])
            self.assertTrue(ok);self.assertLess(abs(value-r["d_m"]),1e-6)

    def test_bad_ds_timestamps(self):
        self.assertFalse(ds_twr([0]*6)[1])
        self.assertFalse(ds_twr([1<<40]*6)[1])
        self.assertFalse(ds_twr([0,1])[1])

    def test_default_p2_is_relative_only(self):
        arrays,rows=packet_arrays(self.meta,self.raw)
        self.assertTrue(np.isfinite(arrays["physical_estimate_s"][:2]).all())
        self.assertTrue(np.isnan(arrays["physical_estimate_s"][2:]).all())
        self.assertTrue(np.isfinite(arrays["relative_s"]).all())
        self.assertEqual(rows[2]["axis_method"],"relative_only")
        self.assertFalse(arrays["both_endpoints_calibrated"].any())

    def test_static_assumption_is_explicit(self):
        arrays,rows=packet_arrays(self.meta,self.raw,assume_static=True,max_speed=0.2)
        self.assertTrue(np.isfinite(arrays["physical_estimate_s"]).all())
        self.assertIn("EXPLICIT_STATIC_ASSUMPTION",rows[2]["axis_method"])
        self.assertAlmostEqual(rows[2]["static_reuse_motion_bound_m"],0.03)

    def test_bad_cir_crc_not_used(self):
        raw=bytearray(self.raw);raw[0]^=1
        arrays,rows=packet_arrays(self.meta,bytes(raw))
        self.assertFalse(arrays["cir_valid"][0])
        self.assertTrue(np.isnan(arrays["physical_estimate_s"][0]).all())
        self.assertFalse(rows[0]["crc_ok"])

    def test_bad_fp_not_used_for_alignment(self):
        m=copy.deepcopy(self.meta);m["probe_rx"][0]["fp_raw"]=65535
        arrays,rows=packet_arrays(m,self.raw)
        self.assertTrue(np.isnan(arrays["relative_s"][0]).all())
        self.assertFalse(rows[0]["axis_estimate_available"])

    def test_anchor_log_confirms_p2_only_when_exact(self):
        anchor = synthetic_anchor(self.meta)
        arrays, rows = packet_arrays(self.meta, self.raw, anchor)
        self.assertTrue(np.isfinite(arrays['physical_estimate_s'][2:]).all())
        self.assertIn('complete_anchor_log', rows[2]['axis_method'])
        anchor['tx'][2]['actual'] += 1
        arrays, _ = packet_arrays(self.meta, self.raw, anchor)
        self.assertTrue(np.isnan(arrays['physical_estimate_s'][2:]).all())

    def test_wrong_anchor_boot_not_joined(self):
        anchor = synthetic_anchor(self.meta)
        anchor['boot'] = 999
        arrays, _ = packet_arrays(self.meta, self.raw, anchor)
        self.assertTrue(np.isnan(arrays['physical_estimate_s'][2:]).all())

    def test_fp_fractional_grid_and_normalization(self):
        arrays,_=packet_arrays(self.meta,self.raw)
        for i in range(4):
            o=self.meta["probe_rx"][i]
            expected=arrays["cir_raw"][i]/o["nacc"]
            np.testing.assert_allclose(arrays["cir_nacc_normalized"][i],expected)
            expected_origin=-o["fp_raw"]/64/998400000
            self.assertAlmostEqual(arrays["relative_s"][i,0],expected_origin)

    def test_common_grid_preserves_native_complex_data(self):
        arrays,_=packet_arrays(self.meta,self.raw)
        self.assertEqual(arrays["common_p1_power_norm"].shape[0],2)
        self.assertGreaterEqual(arrays["common_p1_delay_s"].size,992)
        self.assertEqual(arrays["common_p2_delay_s"].size,0)
        self.assertEqual(arrays["iq_int16"].tobytes(),self.raw)
        self.assertTrue(np.isfinite(arrays["common_p1_power_norm"][:,1:-1]).any())

    def test_malformed_air_crc(self):
        b=bytearray.fromhex(self.meta["frames"][1]);b[40]^=1
        with self.assertRaises(ValueError):air_frame(b.hex())

if __name__ == "__main__":
    unittest.main(verbosity=2)
