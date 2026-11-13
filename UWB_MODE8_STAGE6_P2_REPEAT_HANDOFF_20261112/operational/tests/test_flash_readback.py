"""Esptool read-only deployment evidence. Never write or erase flash."""
import hashlib
from pathlib import Path
import sys
import tempfile
import unittest
sys.path.insert(0,str(Path(__file__).resolve().parents[1]/"host"))
from ops_flash_readback import read_command,match_readback,FlashHold

class ReadbackTests(unittest.TestCase):
    def test_default_command_only_reads_flash(self):
        command=read_command("COM4",0x10000,4096,Path("readback.bin"))
        self.assertIn("read-flash",command)
        self.assertNotIn("write-flash",command)
        self.assertNotIn("erase-flash",command)
        self.assertEqual(command[-3],str(4096) if False else hex(0x10000))
    def test_rejects_bad_partition_and_size(self):
        for offset in (0,0x1000,0x8000,0x10001,0x400000,-1):
            with self.assertRaises(FlashHold):
                read_command("COM4",offset,512,Path("image"))
        with self.assertRaises(FlashHold):
            read_command("COM4",0x10000,999999999,Path("image"))
    def test_byte_mismatch_rejected(self):
        with tempfile.TemporaryDirectory() as d:
            root=Path(d);image=root/"app.bin";read=root/"read.bin"
            image.write_bytes(b"source");read.write_bytes(b"source")
            h=hashlib.sha256(b"source").hexdigest()
            proof={"bytes":6,"file_sha256":h,"role":"tag","source_manifest_sha256":"a"*64}
            self.assertTrue(match_readback(image,read,proof)["byte_identical"])
            read.write_bytes(b"changed")
            with self.assertRaises(FlashHold):match_readback(image,read,proof)
if __name__=="__main__":
    unittest.main()
