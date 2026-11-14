"""SYSSTAT-1 must run in each tag radio's reset/XTI initialization path."""
from pathlib import Path
import sys
import unittest

ROOT = Path(__file__).resolve().parents[1]
VENDOR = ROOT / "vendor/tag/DW1000Ng/src"


class PllCalibrationContract(unittest.TestCase):
    def test_calibration_precedes_first_automatic_clock_transition(self):
        driver = (VENDOR / "DW1000Ng.cpp").read_text(encoding="utf-8")
        init = driver.split("static void initializeWithClockTrace(", 1)[1]
        init = init.split("void initialize(uint8_t ss", 1)[0]
        steps = (
            "reset();",
            "SPIporting::setSPIspeed(SPIClock::SLOW);",
            "_enableClock(SYS_XTI_CLOCK);",
            "_fsxtalt();",
            "_recordClockStep(samples, PllCalibrationWritten);",
            "(clock_control & 0x03U) == SYS_XTI_CLOCK",
            "0xE7950448UL",
            "if(xti_forced) _writeValueToRegister(FS_CTRL, FS_RES1_SUB, pll_calibration, 4);",
            "_manageLDE(samples);",
            "_enableClock(SYS_AUTO_CLOCK);",
        )
        cursor = 0
        for step in steps:
            cursor = init.index(step, cursor) + len(step)
        registers = (VENDOR / "DW1000NgRegisters.hpp").read_text(encoding="utf-8")
        self.assertIn("FS_CTRL = 0x2B;", registers)
        self.assertIn("FS_RES1_SUB = 0x00;", registers)

    def test_both_radios_require_calibration_before_pll_qualification(self):
        port = (ROOT / "shared/esp32_port.hpp").read_text()
        init = port.split("bool initialize(){", 1)[1].split("bool setCalibration(", 1)[0]
        self.assertIn("for(unsigned i=0;i<N;++i)", init)
        self.assertIn("pins(i).cs,pins(i).rst,SPI,clock_steps_[i]", init)
        self.assertLess(init.index("SPIporting::RxSpeedScope slow(true);"),
                        init.index("SPI,clock_steps_[i]"))
        failure = init.split("if(!clock_steps_[i][DW1000Ng::PllCalibrationWritten]", 1)[1]
        failure = failure.split("if(!settleInitClock", 1)[0]
        self.assertIn("tripRegister", failure)
        self.assertIn("initialized=false;continue;", failure)


class ClockDiagnosticTransport(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        # Exercise the same USB collector that runs G6 on the PC.
        sys.path.insert(0, str(ROOT / "tools"))
        try:
            from run_pair import read_clock_history, verify_pll_calibration
        finally:
            sys.path.pop(0)
        cls.read_history = staticmethod(read_clock_history)
        cls.verify_calibration = staticmethod(verify_pll_calibration)

    class Link:
        def __init__(self, wrong_radio=None, changed_fault=False, missing_pll_flag=False):
            self.wrong_radio = wrong_radio
            self.changed_fault = changed_fault
            self.missing_pll_flag = missing_pll_flag
            self.sent = []

        def send(self, command):
            self.sent.append(command.split())

        def wait(self, predicate, timeout, *, check_faults=True):
            command, radio, offset = self.sent[-1]
            radio, offset = int(radio), int(offset)
            if command == "PLLSTEP":
                row = dict(event="PLL_INIT_STEP", boot=77, radio=radio, index=offset,
                           step="pll_calibration_written", pmsc_ctrl0=1,
                           sysstat1_command=0 if radio == self.wrong_radio else 0xE7950448,
                           dev_id=0xDECA0130)
            else:
                status = 0x02801302 if radio == 1 and not self.missing_pll_flag else 0x00800002
                data = [[0, 29, 1000+i, 1010+i, status, 1, 63900*i] for i in range(3)]
                data.extend([[phase, 29, 1005+phase, 1015+phase, status, 1, 322777+phase]
                             for phase in (1, 2)])
                row = dict(event="CLOCK_TRACE", boot=77, session=9, radio=radio, offset=offset,
                           count=len(data), clock_fault=True,
                           fault_radio=0 if self.changed_fault and radio == 1 else 1,
                           samples=data[offset:offset+4])
            if not predicate(row):
                raise TimeoutError("Clock diagnostic response did not correlate")
            return row

    def test_requires_calibration_of_both_radios(self):
        self.assertEqual([r["radio"] for r in self.verify_calibration(self.Link(), 77)], [0, 1])
        with self.assertRaisesRegex(ValueError, "radio 1"):
            self.verify_calibration(self.Link(wrong_radio=1), 77)

    def test_reads_fault_history_in_order_and_rejects_mixed_fault_identity(self):
        stopped = {"boot": 77, "session": 9}
        history = self.read_history(self.Link(), stopped)
        self.assertEqual([row[0] for row in history["radios"]["1"]["samples"]], [0, 0, 0, 1, 2])
        self.assertEqual(history["radios"]["0"]["fault_radio"], 1)
        with self.assertRaisesRegex(ValueError, "fault identity"):
            self.read_history(self.Link(changed_fault=True), stopped)

    def test_rejects_trace_without_pll_flag_on_faulting_radio(self):
        with self.assertRaisesRegex(ValueError, "missing CLKPLL_LL"):
            self.read_history(self.Link(missing_pll_flag=True), {"boot": 77, "session": 9})
