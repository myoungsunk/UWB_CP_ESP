"""Static contract tests: fail when operational hooks or safety gates disappear.
Not a replacement for ESP32 compilation or RF hardware acceptance.
"""
from pathlib import Path
import re
import unittest

BASE=Path(__file__).resolve().parents[2]
A=BASE/"firmware/anchor_p2_repeat/shared/app.hpp"
T=BASE/"firmware/tag_p2_repeat/shared/app.hpp"
O=BASE/"firmware/anchor_p2_repeat/shared/operational_anchor.hpp"

class IntegrationGateTests(unittest.TestCase):
    def test_anchor_and_tag_drain_guards(self):
        for file in (A,T):
            s=file.read_text()
            for needle in ("opsRecordsDrained()","writerBusy","RECORD_DRAIN_PENDING",
                           "awaitingAck.load(std::memory_order_acquire)",
                           "uxQueueMessagesWaiting(freeQ)==POOL"):
                if needle=="writerBusy":
                    needle="opsWriterBusy"
                self.assertIn(needle,s,str(file))
    def test_anchor_only_included_and_spooled(self):
        s=A.read_text()
        self.assertIn("#include \"operational_anchor.hpp\"",s)
        self.assertIn("opsArchiveRecord(record)",s)
        self.assertIn("opsUsbCommand(b)",s)
        self.assertIn("opsLoop();",s)
        self.assertNotIn("#include \"operational_anchor.hpp\"",T.read_text())
    def test_sensitive_gates(self):
        s=O.read_text()
        for needle in ("opsRecordsDrained()","LittleFS.begin(false)",
                       "opsFilePending()","opsNet.authSet","mbedtls_md_hmac",
                       "opsWifiDown();","opsArchiveRecord",
                       "opsVerifyArchive","OTA_WINDOW_OPEN" if False else "opsOtaStart",
                       "opsArchiveFailure","OPS_ARCHIVE_MAX=32"):
            self.assertIn(needle,s)
    def test_compiler_guards_in_both_sketches(self):
        for d,sketch in (("anchor_p2_repeat","ANCHOR"),("tag_p2_repeat","TAG")):
            f=BASE/f"firmware/{d}/firmware/{sketch}_DUAL_DS_CIR/app.hpp"
            self.assertEqual(f.read_text().replace("\r\n","\n"),
                             (BASE/f"firmware/{d}/shared/app.hpp").read_text().replace("\r\n","\n"))

if __name__=="__main__":
    unittest.main()
