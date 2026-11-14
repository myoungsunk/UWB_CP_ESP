import copy
from pathlib import Path
import sys
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "host"))
from ops_usb_clock import AlignmentHold, fit_usb_markers


def rows():
    offset = 1_700_000_000_000_000_000
    samples = []
    for radio in (0, 1):
        for i, phase in enumerate(("before", "during", "after")):
            mono = 1_000_000_000 + i * 1_000_000_000 + radio * 10_000_000
            ticks = (round(mono * 63.8976) // 512 * 512) % (1 << 40)
            event = dict(event="SYNC_MARK", role="tag", marker=1+radio*3+i, radio=radio,
                         boot=91, session=31 if i else 0, sys_time=ticks,
                         esp_before_us=mono//1000, esp_after_us=mono//1000 + 10,
                         active=i == 1)
            samples.append(dict(event, event=event, phase=phase, rf_session=31, status="ok",
                                send_mono_ns=mono-500_000, recv_mono_ns=mono+500_000,
                                send_clock=dict(mono_before_ns=mono-500_020, mono_after_ns=mono-500_010,
                                                realtime_ns=offset+mono-500_015),
                                recv_clock=dict(mono_before_ns=mono+500_010, mono_after_ns=mono+500_020,
                                                realtime_ns=offset+mono+500_015)))
    return samples


class UsbClockTest(unittest.TestCase):
    def test_both_domains_match_known_slope(self):
        result = fit_usb_markers(rows(), 91, 31)
        self.assertEqual(set(result), {"tagA", "tagB"})
        for domain in result.values():
            self.assertLess(abs(domain["ppm"]), 0.01)
            self.assertGreaterEqual(domain["max_sample_bound_ns"], 500_000)

    def test_duplicate_marker_and_wrong_identity_rejected(self):
        good = rows()
        for bad in (good + [good[0]], good[:-1], [dict(good[0], boot=99), *good[1:]]):
            with self.subTest(bad=bad[0]):
                with self.assertRaises(AlignmentHold):
                    fit_usb_markers(bad, 91, 31)

    def test_jump_and_large_USB_interval_rejected(self):
        samples = copy.deepcopy(rows())
        samples[4]["recv_clock"]["realtime_ns"] += 900_000_000
        with self.assertRaisesRegex(AlignmentHold, "offset changed"):
            fit_usb_markers(samples, 91, 31)
        with self.assertRaisesRegex(AlignmentHold, "asymmetry bound"):
            fit_usb_markers(rows(), 91, 31, max_bound_ns=400_000)
