"""Pi-only tag install and Stage 6 capture wrapper for PLL trace builds.

Install and capture are separate operations. This script never writes the
COM4 anchor; install writes only the tag app partition at 0x10000.
"""
from __future__ import annotations

import argparse
from datetime import datetime, timezone
import hashlib
import json
from pathlib import Path
import shlex
import subprocess
import sys
import time

HERE = Path(__file__).resolve().parent
ARTIFACTS = HERE.parent
WORKSPACE = ARTIFACTS.parent
TAG_ROOT_DEFAULT = HERE / "tag"
TAG_ROOT = TAG_ROOT_DEFAULT
BUILD_RECEIPT = TAG_ROOT / "dist" / "target_build_receipt.json"
APP_NAME = "TAG_DUAL_DS_CIR.ino.bin"
NEW_IMAGE = TAG_ROOT / "dist" / "tag" / APP_NAME
OFFSET = "0x10000"
TAG_PORT = "/dev/serial/by-id/usb-1a86_USB_Serial-if00-port0"
TAG_MAC = "28:56:2f:73:06:54"
PI = "slam-robot-tailscale"
SSH_ARGS = ["-F", str(WORKSPACE / ".codex-robot-ssh-config"),
            "-o", "BatchMode=yes", "-o", "ConnectTimeout=8"]
ANCHOR_DEPLOYMENT = (ARTIFACTS / "UWB_MODE8_STAGE6_RELIABILITY_20261112" /
                     "evidence" / "stage6_live_20261112_121702_342" / "deployment.json")
LIVE_EVIDENCE = (ARTIFACTS / "UWB_MODE8_STAGE6_RELIABILITY_20261112" /
                 "evidence" / "stage6_live_20261112_121702_342")
REGISTER_TOOLS = ARTIFACTS / "UWB_MODE8_STAGE6_REGISTER_DIAG_20261112"


def sha(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def write_json(path: Path, data: dict) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(data, indent=2), encoding="utf-8")


def mkdir_new(path: Path) -> Path:
    path.mkdir(parents=True, exist_ok=False)
    return path


def timestamped_out(mode: str) -> Path:
    return HERE / f"{mode}_{datetime.now(timezone.utc):%Y%m%d_%H%M%S_%f}"


def resolve_firmware_id(receipt: dict, requested: str | None) -> str:
    role = receipt.get("roles", {}).get("tag", {})
    value = requested or role.get("firmware_id") or receipt.get("firmware_id")
    if not value:
        source = (TAG_ROOT / "shared" / "record_json.hpp").read_text(encoding="utf-8")
        import re
        match = re.search(r'firmwareId\(bool anchor\).*?:"([A-Z0-9_]+)";', source)
        if match:
            value = match.group(1)
    if not isinstance(value, str) or not value.startswith("D8_TAG_STAGE6_"):
        raise RuntimeError("Supply --firmware-id or add firmware_id to the successful tag build receipt")
    source = (TAG_ROOT / "shared" / "record_json.hpp").read_text(encoding="utf-8")
    if f'"{value}"' not in source:
        raise RuntimeError(f"Tag source does not embed requested firmware ID {value}")
    return value


def validate_build() -> tuple[dict, str]:
    if not BUILD_RECEIPT.is_file() or not NEW_IMAGE.is_file():
        raise RuntimeError("Sibling tag build receipt or app image is missing")
    receipt = json.loads(BUILD_RECEIPT.read_bytes())
    role = receipt.get("roles", {}).get("tag", {})
    artifact = role.get("artifacts", {}).get(APP_NAME, {})
    if receipt.get("target_build") is not True or role.get("exit_code") != 0:
        raise RuntimeError("Tag target build receipt is not successful")
    if sha(NEW_IMAGE) != artifact.get("sha256"):
        raise RuntimeError("Tag app image hash differs from build receipt")
    sys.path.insert(0, str(TAG_ROOT / "tools"))
    from source_contract import manifest
    if manifest(TAG_ROOT, "tag") != role.get("source_manifest"):
        raise RuntimeError("Tag source changed after the recorded build")
    firmware_id = resolve_firmware_id(receipt, None)
    return receipt, firmware_id


def run_logged(command: list[str], path: Path, expected_mac: str | None = None) -> None:
    with path.open("xb") as log:
        rc = subprocess.run(command, stdout=log, stderr=subprocess.STDOUT, timeout=120).returncode
    output = path.read_text(encoding="utf-8", errors="replace").lower()
    if rc or (expected_mac and expected_mac.lower() not in output):
        raise RuntimeError(f"{path.name} failed (exit {rc}; expected MAC {expected_mac})")


def extract_pll_trace_events(tag_jsonl: Path, destination: Path) -> int:
    selected = {"ANT_INIT", "HARDWARE_FAULT_LATCHED", "STOPPED", "HELLO", "SELFTEST", "ERROR"}
    count = 0
    with tag_jsonl.open(encoding="utf-8") as source, destination.open("x", encoding="utf-8") as target:
        for line in source:
            try:
                item = json.loads(line)
            except json.JSONDecodeError:
                continue
            event = str(item.get("event", ""))
            contains_pll = any("pll" in str(key).lower() or
                               (isinstance(value, str) and "pll" in value.lower())
                               for key, value in item.items())
            if event in selected or contains_pll:
                target.write(json.dumps(item, ensure_ascii=False) + "\n")
                count += 1
    return count


def install(prior_image: Path, firmware_arg: str | None, output: Path) -> Path:
    mkdir_new(output)
    build_receipt, firmware_id = validate_build()
    prior_image = prior_image.resolve()
    if not prior_image.is_file():
        raise RuntimeError(f"Expected currently installed tag app not found: {prior_image}")
    if WORKSPACE not in prior_image.parents:
        raise RuntimeError("Prior image must be inside the workspace so its backup can be staged safely")
    if firmware_arg and firmware_arg != firmware_id:
        raise RuntimeError(f"Build embeds {firmware_id}; requested {firmware_arg}")

    prior_copy = output / "prior_tag_app.bin"
    new_copy = output / "new_tag_app.bin"
    prior_copy.write_bytes(prior_image.read_bytes())
    new_copy.write_bytes(NEW_IMAGE.read_bytes())
    prior_hash, new_hash = sha(prior_copy), sha(new_copy)
    if new_hash != build_receipt["roles"]["tag"]["artifacts"][APP_NAME]["sha256"]:
        raise RuntimeError("Staged new image hash mismatch")

    remote = "/tmp/uwb-mode8-pll-trace-" + output.name.removeprefix("install_")
    ssh = ["ssh", "-T", *SSH_ARGS, PI]
    scp = ["scp", *SSH_ARGS]
    run_logged(ssh + ["umask 077; mkdir " + shlex.quote(remote)], output / "pi_stage.log")
    for source, name in ((prior_copy, "prior.bin"), (new_copy, "new.bin")):
        relative = str(source.relative_to(WORKSPACE))
        run_logged(scp + [relative, f"{PI}:{remote}/{name}"], output / f"pi_copy_{name}.log")
    manifest = f"{prior_hash}  prior.bin\n{new_hash}  new.bin"
    check = (f"cd {shlex.quote(remote)} && printf '%s\\n' {shlex.quote(manifest)}"
             " | sha256sum -c -")
    run_logged(ssh + [check], output / "pi_images_hash.log")
    esptool = ["python3", "-m", "esptool", "--chip", "esp32", "--port", TAG_PORT,
               "--baud", "460800"]
    run_logged(ssh + [check + " && " + shlex.join(esptool + ["verify-flash", OFFSET, "prior.bin"])],
               output / "prior_verify.log", TAG_MAC)
    receipt = {"mode": "install", "started_utc": datetime.now(timezone.utc).isoformat(),
               "tag_only": True, "anchor_write": False, "host": PI, "port": TAG_PORT,
               "mac": TAG_MAC, "offset": OFFSET, "firmware_id": firmware_id,
               "tag_package": str(TAG_ROOT),
               "prior_app_sha256": prior_hash, "new_app_sha256": new_hash,
               "build_receipt_sha256": sha(BUILD_RECEIPT),
               "source_manifest_sha256": build_receipt["roles"]["tag"]["source_manifest"]["sha256"],
               "prior_verified_on_device": True, "write_attempted": True,
               "flash_readback_verified": False}
    write_json(output / "installation_receipt.json", receipt)
    flags = ["--flash-mode", "keep", "--flash-freq", "keep", "--flash-size", "keep"]
    run_logged(ssh + [check + " && " + shlex.join(
        esptool + ["write-flash", *flags, OFFSET, "new.bin"])], output / "tag_write.log", TAG_MAC)
    run_logged(ssh + [check + " && " + shlex.join(
        esptool + ["verify-flash", OFFSET, "new.bin"])], output / "tag_readback_verify.log", TAG_MAC)
    receipt["flash_readback_verified"] = True
    receipt["finished_utc"] = datetime.now(timezone.utc).isoformat()
    write_json(output / "installation_receipt.json", receipt)
    print(json.dumps({"receipt": str(output / "installation_receipt.json"), **receipt}, indent=2))
    return output / "installation_receipt.json"


def capture(install_receipt_path: Path, firmware_arg: str | None, output: Path) -> int:
    mkdir_new(output)
    install_receipt_path = install_receipt_path.resolve()
    receipt = json.loads(install_receipt_path.read_bytes())
    build, source_firmware_id = validate_build()
    firmware_id = firmware_arg or receipt.get("firmware_id") or source_firmware_id
    if (receipt.get("mode") != "install" or receipt.get("tag_only") is not True
            or receipt.get("flash_readback_verified") is not True
            or receipt.get("mac", "").lower() != TAG_MAC.lower()
            or receipt.get("new_app_sha256") != sha(NEW_IMAGE)
            or receipt.get("tag_package") != str(TAG_ROOT)
            or receipt.get("build_receipt_sha256") != sha(BUILD_RECEIPT)
            or receipt.get("firmware_id") != firmware_id
            or firmware_id != source_firmware_id):
        raise RuntimeError("Install receipt, current build image, MAC, or firmware ID mismatch")

    sys.path.insert(0, str(LIVE_EVIDENCE))
    sys.path.insert(0, str(REGISTER_TOOLS))
    sys.path.insert(0, str(TAG_ROOT / "tools"))
    from watch_tag_dev_id import load_watcher
    from rerun_register_diag import prepare_tag, verify
    import live_pair as live

    anchor_deployment = json.loads(ANCHOR_DEPLOYMENT.read_bytes())["roles"]["anchor"]
    anchor_build_receipt = (ARTIFACTS / "UWB_MODE8_STAGE6_RELIABILITY_20261112" /
                            "anchor" / "dist" / "target_build_receipt.json")
    anchor_image = anchor_build_receipt.parent / "anchor" / "ANCHOR_DUAL_DS_CIR.ino.bin"
    if (not anchor_deployment.get("flash_readback_verified")
            or sha(anchor_build_receipt) != anchor_deployment.get("build_receipt_sha256")
            or sha(anchor_image) != anchor_deployment.get("new_app_sha256")):
        raise RuntimeError("COM4 anchor baseline receipt/image mismatch; abort before paired capture")

    deployment = {"roles": {
        "anchor": {**anchor_deployment, "write_attempted": False, "anchor_write": False},
        "tag": {"port": TAG_PORT, "mac": TAG_MAC,
                "new_app_sha256": receipt["new_app_sha256"],
                "build_receipt_sha256": receipt["build_receipt_sha256"],
                "flash_readback_verified": True, "write_attempted": False,
                "firmware_id": firmware_id}},
        "tag_install_receipt": str(install_receipt_path), "tag_only_deploy": True}
    write_json(output / "deployment.json", deployment)

    watcher = load_watcher()
    live.FIRMWARE["tag"] = firmware_id
    live.OUT = output
    live.write_json = lambda name, data: write_json(output / name, data)
    live.SplitLink = watcher.CurrentStatusLink
    if not prepare_tag(watcher, output) or live.smoke() != 0:
        return 2
    result = 2
    strict_ok = False
    try:
        result = live.run_capture()
    finally:
        execution = output / "execution.json"
        if execution.is_file():
            data = json.loads(execution.read_bytes())
            data["command"] = [sys.executable, "-B", "-X", "utf8", str(Path(__file__).resolve()),
                               "--mode", "capture", "--tag-package", str(TAG_ROOT),
                               "--install-receipt", str(install_receipt_path), "--firmware-id", firmware_id,
                               "--out", str(output)]
            for path in (Path(__file__), install_receipt_path, BUILD_RECEIPT,
                         output / "deployment.json", output / "tag_precheck.json"):
                data.setdefault("input_sha256", {})[str(path)] = sha(path)
            write_json(execution, data)
        if (output / "capture" / "capture_receipt.json").is_file():
            trace_events = extract_pll_trace_events(
                output / "capture" / "tag.jsonl", output / "pll_trace_events.jsonl")
            strict_ok = verify(output)
            write_json(output / "run_status.json", {
                "collector_exit_code": result, "register_grid_strict_pass": strict_ok,
                "capture_receipt": str(output / "capture" / "capture_receipt.json"),
                "firmware_id": firmware_id,
                "pll_trace_event_count": trace_events,
                "pll_trace_events": str(output / "pll_trace_events.jsonl"),
                "tag_fault_events": "preserved in capture/tag.jsonl and capture/tag.rfb8",
                "register_samples": str(output / "tag_radio_registers.csv"),
                "register_summary": str(output / "register_summary.json")})
    return 0 if result == 0 and strict_ok else 2


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--mode", choices=("install", "capture"), required=True)
    parser.add_argument("--prior-image", type=Path,
                        help="Expected current tag app image; required for --mode install")
    parser.add_argument("--install-receipt", type=Path,
                        help="Readback-verified installation_receipt.json; required for capture")
    parser.add_argument("--firmware-id", help="Expected D8_TAG_STAGE6_* identity; source/receipt otherwise")
    parser.add_argument("--tag-package", type=Path, default=TAG_ROOT_DEFAULT,
                        help="Tag source/build package; use a separate path such as tag_fixed")
    parser.add_argument("--out", type=Path, help="New output directory; defaults to a timestamped folder")
    args = parser.parse_args()
    global TAG_ROOT, BUILD_RECEIPT, NEW_IMAGE
    TAG_ROOT = args.tag_package.resolve()
    output = args.out.resolve() if args.out else timestamped_out(args.mode)
    if WORKSPACE not in TAG_ROOT.parents:
        parser.error("--tag-package must remain inside the workspace")
    if WORKSPACE not in output.parents:
        parser.error("--out must remain inside the workspace")
    BUILD_RECEIPT = TAG_ROOT / "dist" / "target_build_receipt.json"
    NEW_IMAGE = TAG_ROOT / "dist" / "tag" / APP_NAME
    if args.mode == "install":
        if args.install_receipt:
            parser.error("--install-receipt is only for --mode capture")
        if not args.prior_image:
            parser.error("--prior-image is required for --mode install")
        install(args.prior_image, args.firmware_id, output)
        return 0
    if args.prior_image:
        parser.error("--prior-image is only for --mode install")
    if not args.install_receipt:
        parser.error("--install-receipt is required for --mode capture")
    return capture(args.install_receipt, args.firmware_id, output)


if __name__ == "__main__":
    sys.exit(main())
