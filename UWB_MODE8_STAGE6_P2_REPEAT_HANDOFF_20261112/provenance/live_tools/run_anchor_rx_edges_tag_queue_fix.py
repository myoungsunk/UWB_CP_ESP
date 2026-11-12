"""Pin the RX-edges Stage 6 runner to a verified queue-fix Pi tag install."""
from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path
import sys

HERE = Path(__file__).resolve().parent
TAG_ROOT = HERE / "tag_queue_fix"
FIRMWARE = "D8_TAG_STAGE6_QUEUE_FIX_20261112"
EXPECTED_PORT = "/dev/serial/by-id/usb-1a86_USB_Serial-if00-port0"
EXPECTED_MAC = "28:56:2f:73:06:54"

import run_anchor_rx_edges as runner


def sha(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def validate_tag_receipt(path: Path, tag_root: Path, firmware: str) -> None:
    receipt = json.loads(path.read_bytes())
    if (receipt.get("mode") != "install" or receipt.get("tag_only") is not True
            or receipt.get("anchor_write") is not False
            or receipt.get("flash_readback_verified") is not True
            or receipt.get("firmware_id") != firmware
            or receipt.get("host") != "slam-robot-tailscale"
            or receipt.get("port") != EXPECTED_PORT
            or receipt.get("mac", "").lower() != EXPECTED_MAC
            or str(Path(receipt.get("tag_package", "")).resolve()).casefold()
            != str(tag_root.resolve()).casefold()):
        raise RuntimeError(f"Tag install receipt does not verify {firmware} on the Pi")


def main(*, tag_root=TAG_ROOT, tag_firmware=FIRMWARE, anchor_root=None,
         anchor_firmware=None, analysis_root=None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--tag-install-receipt", type=Path, required=True,
                        help="Verified receipt from run_pi_pll_trace.py install")
    args, forwarded = parser.parse_known_args()
    receipt_path = args.tag_install_receipt.resolve()
    validate_tag_receipt(receipt_path, tag_root, tag_firmware)

    base = runner.base
    base.TAG_ROOT = tag_root
    base.TAG_INSTALL = receipt_path
    base.FIRMWARE = {**base.FIRMWARE, "tag": tag_firmware}
    base.live.FIRMWARE = {**base.live.FIRMWARE, "tag": tag_firmware}

    run_parser = argparse.ArgumentParser(add_help=False)
    run_parser.add_argument("--out", type=Path, required=True)
    run_args, _ = run_parser.parse_known_args(forwarded)
    old_argv = sys.argv
    try:
        sys.argv = [str(Path(runner.__file__).resolve()), *forwarded]
        if anchor_root is None:
            result = runner.main()
        else:
            result = runner.main(anchor_root=anchor_root, anchor_firmware=anchor_firmware,
                                 analysis_root=analysis_root)
    finally:
        sys.argv = old_argv

    output = run_args.out.resolve()
    execution_path = output / "execution.json"
    record = json.loads(execution_path.read_bytes())
    for wrapper in {Path(__file__).resolve(), Path(old_argv[0]).resolve()}:
        record.setdefault("input_sha256", {})[str(wrapper)] = sha(wrapper)
    record["command"] = [sys.executable, *old_argv]
    record["tag_install_receipt"] = {"path": str(receipt_path), "sha256": sha(receipt_path),
                                     "firmware_id": tag_firmware}
    runner.base.write_json(execution_path, record)
    return result


if __name__ == "__main__":
    raise SystemExit(main())
