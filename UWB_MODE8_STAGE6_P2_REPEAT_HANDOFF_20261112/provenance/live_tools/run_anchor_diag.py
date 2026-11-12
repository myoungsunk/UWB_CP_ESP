"""Collect a bounded Mode 8 Stage 6 diagnosis on COM4 and the Pi tag."""

import argparse
from datetime import datetime, timezone
import json
from pathlib import Path
import sys
import time


HERE = Path(__file__).resolve().parent
ARTIFACTS = HERE.parent
RELIABILITY = ARTIFACTS / "UWB_MODE8_STAGE6_RELIABILITY_20261112"
LIVE = RELIABILITY / "evidence" / "stage6_live_20261112_121702_342"
TAG_DIAG = ARTIFACTS / "UWB_MODE8_STAGE6_TAG_PLL_TRACE_20261112"
ANCHOR_ROOT = HERE / "anchor_diag"
TAG_ROOT = TAG_DIAG / "tag_clock_steps"
TAG_INSTALL = TAG_DIAG / "install_clock_steps" / "installation_receipt.json"
CALIBRATION = RELIABILITY / "calibration_stage6_temporary.json"
MAC = {"anchor": "28:56:2f:72:e7:18", "tag": "28:56:2f:73:06:54"}
FIRMWARE = {
    "anchor": "D8_ANCHOR_STAGE6_RX_DIAG_20261112",
    "tag": "D8_TAG_STAGE6_PLL_STEPS_20261112",
}

sys.path.insert(0, str(LIVE))
import live_pair as live
from recovery_retry import CurrentStatusLink


def now():
    return datetime.now(timezone.utc).isoformat()


def write_json(path, value):
    path.write_text(json.dumps(value, indent=2) + "\n", encoding="utf-8")


def check_image(role, root, installation):
    receipt_path = root / "dist" / "target_build_receipt.json"
    filename = f"{role.upper()}_DUAL_DS_CIR.ino.bin"
    image = root / "dist" / role / filename
    receipt = json.loads(receipt_path.read_bytes())
    role_build = receipt["roles"][role]
    image_hash = live.sha(image)
    build_hash = live.sha(receipt_path)
    if (receipt.get("target_build") is not True or role_build.get("exit_code") != 0
            or role_build["artifacts"][filename]["sha256"] != image_hash):
        raise RuntimeError(f"{role}: build receipt or binary mismatch")
    installed = json.loads(installation.read_bytes())
    if (installed.get("flash_readback_verified") is not True
            or installed.get("firmware_id") != FIRMWARE[role]
            or installed.get("port") != live.PORTS[role]
            or installed.get("mac", "").lower() != MAC[role]
            or installed.get("new_app_sha256") != image_hash
            or installed.get("build_receipt_sha256") != build_hash):
        raise RuntimeError(f"{role}: install receipt and build do not match")
    return {str(receipt_path): build_hash, str(image): image_hash}


def check_usb(role, directory, report):
    link = CurrentStatusLink(role, live.PORTS[role], directory)
    try:
        time.sleep(0.5)
        link.send("HELLO")
        try:
            hello = link.wait(lambda row: row.get("event") == "HELLO" and row.get("baud") == 921600,
                              timeout=3, check_faults=False)
        except TimeoutError:
            link.enable_replay()
            link.serial.baudrate = 2000000
            live.SplitLink.send(link, "")
            link.send("HELLO")
            hello = link.wait(lambda row: row.get("event") == "HELLO" and row.get("baud") == 2000000,
                              timeout=3, check_faults=False)
        report["hello"] = hello
        if not (hello.get("mode") == 8 and hello.get("wire") == "RFB8"
                and hello.get("record_replay") == 1 and hello.get("active") is False):
            raise RuntimeError(f"{role}: expected an idle Mode 8 board with record replay")
        if role == "anchor":
            link.send("PLLSTATUS")
            report["pll_before"] = link.wait(
                lambda row: row.get("event") == "PLLSTATUS" and row.get("role") == "anchor"
                and row.get("boot") == hello.get("boot"), timeout=3, check_faults=False)
        link.send("SELFTEST")
        test = report["selftest"] = link.wait(
            lambda row: row.get("event") == "SELFTEST" and row.get("role") == role,
            timeout=3, check_faults=False)
        selftest_ok = (test.get("pass") is True and test.get("dev_a") == 0xDECA0130
                       and (role == "anchor" or test.get("dev_b") == 0xDECA0130))
        idle_ok = (hello.get("hw_ok") is True and hello.get("fault_latched") is False
                   and (role == "tag" or report["pll_before"].get("ready") is True))
        if not (selftest_ok and idle_ok):
            if role == "anchor":
                try:
                    link.send("RXFAULT")
                    report["rx_fault"] = link.wait(
                        lambda row: row.get("event") == "ANCHOR_RX_FIRST_FAULT"
                        and row.get("boot") == hello.get("boot"),
                        timeout=3, check_faults=False)
                except (OSError, RuntimeError) as exc:
                    report["rx_fault_error"] = str(exc)
            reason = "hardware was not ready before SELFTEST" if selftest_ok else "SELFTEST failed"
            raise RuntimeError(f"{role}: {reason}")
        if hello["baud"] == 2000000:
            live.run_pair.restore_default_baud(link, hello["period_us"])
            report["baud_restored"] = True
        report["pass"] = True
    finally:
        link.close()


def summarize_faults(output, collector):
    for folder in ("usb", "capture"):
        path = output / folder / "anchor.rfb8"
        if path.is_file():
            for packet in collector.read_file(path):
                if packet.kind == 1 and packet.meta.get("event") in {
                        "ANCHOR_RX_FIRST_FAULT", "HARDWARE_FAULT_LATCHED"}:
                    yield {"source": folder, "event": packet.meta}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--count", type=int, choices=(1, 20, 1000), required=True)
    parser.add_argument("--out", type=Path, required=True, help="New evidence directory")
    parser.add_argument("--anchor-install-receipt", type=Path, required=True,
                        help="Anchor app write/readback receipt from deployment")
    args = parser.parse_args()
    output = args.out.resolve()
    output.mkdir(parents=True, exist_ok=False)
    usb = output / "usb"
    usb.mkdir()
    live.FIRMWARE.update(FIRMWARE)
    collector = live.run_pair
    settings = argparse.Namespace(anchor=live.PORTS["anchor"], tag=live.PORTS["tag"],
                                  stage=6, count=args.count, period_us=200000,
                                  baud=2000000, rf_remeasure_limit=0,
                                  out=output / "capture")
    execution = {"question_id": "MODE8-ANCHOR-RX-FAULT-20261112", "role": "IMPLEMENTER",
                 "started_utc": now(), "command": [sys.executable, *sys.argv],
                 "collector_settings": {**vars(settings), "out": str(settings.out),
                                        "calibration_profile": str(CALIBRATION)},
                 "firmware": FIRMWARE, "input_sha256": {}}
    governance = Path.home() / ".codex" / "research-governance"
    precheck = {"roles": {}, "pass": False}
    code = 2
    try:
        for path in (Path(__file__), Path(live.__file__), Path(collector.__file__),
                     Path(live.run_split_pair.__file__), CALIBRATION, TAG_INSTALL,
                     args.anchor_install_receipt.resolve(),
                     governance / "COMMON_CONTRACT.md", governance / "ROLE_IMPLEMENTER.md"):
            execution["input_sha256"][str(path)] = live.sha(path)
        execution["input_sha256"].update(check_image("anchor", ANCHOR_ROOT, args.anchor_install_receipt))
        execution["input_sha256"].update(check_image("tag", TAG_ROOT, TAG_INSTALL))
        profile = collector.load_profile(CALIBRATION)
        for role in ("anchor", "tag"):
            entry = precheck["roles"][role] = {"port": live.PORTS[role], "pass": False}
            try:
                check_usb(role, usb, entry)
            except (OSError, RuntimeError) as exc:
                entry["error"] = str(exc)
                entry["pass"] = False
        precheck["pass"] = all(entry["pass"] for entry in precheck["roles"].values())
        write_json(output / "hello_selftest.json", precheck)
        if precheck["pass"]:
            collector.Link = CurrentStatusLink
            result = collector.capture(settings, profile)
            code = 0 if result["pass"] else 2
    except (OSError, ValueError, KeyError, RuntimeError) as exc:
        execution["error"] = str(exc)
    finally:
        execution.update(finished_utc=now(), exit_code=code, preflight_pass=precheck["pass"])
        receipt = settings.out / "capture_receipt.json"
        if receipt.is_file():
            execution["capture_receipt_sha256"] = live.sha(receipt)
        write_json(output / "anchor_faults.json", list(summarize_faults(output, collector)))
        write_json(output / "execution.json", execution)
        print(json.dumps({"evidence": str(output), "exit_code": code,
                          "preflight": precheck, "error": execution.get("error")}, indent=2), flush=True)
    return code


if __name__ == "__main__":
    raise SystemExit(main())
