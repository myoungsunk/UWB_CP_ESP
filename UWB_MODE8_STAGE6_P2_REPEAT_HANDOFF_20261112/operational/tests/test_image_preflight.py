"""Fail-closed host OTA provenance checker (not device rollback proof)."""
from pathlib import Path
import sys
import hashlib
import json
import tempfile
import struct
import unittest

sys.path.insert(0,str(Path(__file__).resolve().parents[1]/"host"))
from ops_image_preflight import verify,ImageHold,signed_host_intent,ROLE_MARKERS

class ImageTests(unittest.TestCase):
    def fixture(self,dir,role="anchor",appended=False, *, marker_role=None, corrupt_segment=False):
        name="ANCHOR_DUAL_DS_CIR.ino.bin" if role=="anchor" else "TAG_DUAL_DS_CIR.ino.bin"
        image=dir/name
        data=bytearray(24);data[0]=0xe9;data[1]=1;data[23]=int(appended)
        marker=ROLE_MARKERS[marker_role or role].ljust(256,b"\0")
        checksum=0xef
        for byte in marker:
            checksum^=byte
        data+=struct.pack("<II",0x3f400020,len(marker))+marker
        data+=bytes(15-(len(data)%16))+bytes([checksum])
        if corrupt_segment:data[100]^=1
        if appended:data+=hashlib.sha256(data).digest()
        image.write_bytes(data)
        sha=hashlib.sha256(data).hexdigest()
        receipt=dir/"receipt.json"
        receipt.write_text(json.dumps({
          "target_build":True,"fqbn":"esp32:esp32:esp32doit-devkit-v1",
          "required_core":"3.3.7",
          "roles":{role:{"exit_code":0,"source_manifest":{"a":"b"},
                         "artifacts":{name:{"sha256":sha,"bytes":len(data)}}}}
        }),encoding="utf-8")
        return image,receipt
    def test_valid_image_and_appended_digest(self):
        with tempfile.TemporaryDirectory() as folder:
            d=Path(folder);image,receipt=self.fixture(d,appended=True)
            result=verify(image,receipt,"anchor")
            self.assertEqual(result["partition_sha256"],image.read_bytes()[-32:].hex())
            self.assertEqual(len(signed_host_intent(result,b"k"*32)["host_intent_hmac_sha256"]),64)
    def test_role_and_tampering(self):
        with tempfile.TemporaryDirectory() as folder:
            d=Path(folder);image,receipt=self.fixture(d)
            self.assertEqual(verify(image,receipt,"anchor")["role"],"anchor")
            with self.assertRaises(ImageHold):verify(image,receipt,"tag")
            image.write_bytes(image.read_bytes()+b"oops")
            with self.assertRaises(ImageHold):verify(image,receipt,"anchor")
    def test_unverified_receipt_rejected(self):
        with tempfile.TemporaryDirectory() as folder:
            d=Path(folder);image,receipt=self.fixture(d)
            v=json.loads(receipt.read_text())
            v["target_build"]=False;receipt.write_text(json.dumps(v))
            with self.assertRaises(ImageHold):verify(image,receipt,"anchor")
    def test_modified_appended_digest_rejected(self):
        with tempfile.TemporaryDirectory() as folder:
            d=Path(folder);image,receipt=self.fixture(d,appended=True)
            data=bytearray(image.read_bytes());data[-1]^=1;image.write_bytes(data)
            with self.assertRaises(ImageHold):verify(image,receipt,"anchor")
    def test_renamed_other_role_rejected_even_with_matching_receipt(self):
        with tempfile.TemporaryDirectory() as folder:
            image,receipt=self.fixture(Path(folder),marker_role="tag",appended=True)
            with self.assertRaisesRegex(ImageHold,"firmware role mismatch"):
                verify(image,receipt,"anchor")

    def test_recomputed_hash_does_not_hide_bad_esp32_checksum(self):
        with tempfile.TemporaryDirectory() as folder:
            image,receipt=self.fixture(Path(folder),appended=True,corrupt_segment=True)
            with self.assertRaisesRegex(ImageHold,"padding or checksum"):
                verify(image,receipt,"anchor")

if __name__=="__main__":unittest.main()
