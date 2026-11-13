"""Check firmware integration points alongside behavioral tests."""
from pathlib import Path
import unittest

BASE = Path(__file__).resolve().parents[2]
A = BASE/"firmware/anchor_p2_repeat/shared/app.hpp"
T = BASE/"firmware/tag_p2_repeat/shared/app.hpp"
O = BASE/"firmware/anchor_p2_repeat/shared/operational_anchor.hpp"


class IntegrationGateTests(unittest.TestCase):
    def test_anchor_and_tag_use_p1_drain(self):
        for path in (A, T):
            app = path.read_text(encoding="utf-8")
            for snippet in ("transportChangeAllowed()", "RECORD_DRAIN_PENDING",
                            "serviceDrain();", "noticeReplay({b,s,configId(config),seq}"):
                self.assertIn(snippet, app, str(path))

    def test_only_anchor_spools_for_operations(self):
        app = A.read_text(encoding="utf-8")
        self.assertIn('#include "operational_anchor.hpp"', app)
        self.assertIn("opsArchiveRecord(record)", app)
        self.assertIn("opsUsbCommand(b)", app)
        self.assertNotIn("operational_anchor.hpp", T.read_text(encoding="utf-8"))

    def test_snapshot_headers_match(self):
        for role in ("anchor", "tag"):
            firmware = BASE/"firmware"/f"{role}_p2_repeat"
            for source in (firmware/"shared").glob("*.hpp"):
                for sketch in ("ANCHOR", "TAG"):
                    copy = firmware/"firmware"/f"{sketch}_DUAL_DS_CIR"/source.name
                    self.assertEqual(source.read_bytes(), copy.read_bytes(), str(copy))

    def test_rf_start_checks_wifi_even_when_internally_dispatched(self):
        ops = O.read_text(encoding="utf-8")
        start = ops[ops.index("static void opsLoop(){"):]
        self.assertLess(start.index("WiFi.getMode()!=WIFI_OFF"), start.index("opsInternalStart=true"))
        self.assertIn('if(!opsWifiDown()){opsFail("WIFI_OFF_FAILED");return;}', ops)
        app = A.read_text(encoding="utf-8")
        self.assertIn("if(WiFi.getMode()!=WIFI_OFF)", app)
        self.assertIn("opsCancelScheduledStart();", app)
