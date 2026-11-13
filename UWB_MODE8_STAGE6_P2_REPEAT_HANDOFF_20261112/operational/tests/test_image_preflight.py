"""Fail-closed host OTA provenance checker (not device rollback proof)."""
from pathlib import Path
import sys
import hashlib
import json
import tempfile
import unittest

sys.path.insert(0,str(Path(__file__).resolve().parents[1]/"host"))
from ops_image_preflight import verify,ImageHold,signed_host_intent

class ImageTests(unittest.TestCase):
    def fixture(self,dir,role="anchor",appended=False):
        name="ANCHOR_DUAL_DS_CIR.ino.bin" if role=="anchor" else "TAG_DUAL_DS_CIR.ino.bin"
        image=dir/name
        data=bytearray(256);data[0]=0xe9;data[23]=1 if appended else 0
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
if __name__=="__main__":unittest.main()
