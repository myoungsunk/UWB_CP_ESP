#!/usr/bin/env python3
"""Guarded upload of locally built binaries; never erases the whole flash."""
from __future__ import annotations
import argparse
import hashlib
import json
from pathlib import Path
import shutil
import subprocess
import sys
ROOT = Path(__file__).resolve().parents[1]
from source_contract import matches
from offline_preflight import scan

def main() -> int:
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--role", choices=("anchor", "tag"), required=True)
    p.add_argument("--port", required=True)
    p.add_argument("--cli", default="arduino-cli")
    a = p.parse_args()
    receipt_path = ROOT/"dist/target_build_receipt.json"
    if not receipt_path.exists():
        p.error("No target build receipt. Compile first; host objects cannot be flashed.")
    receipt = json.loads(receipt_path.read_text())
    if not receipt.get("target_build") or a.role not in receipt.get("roles", {}):
        p.error("Successful target build for this role required")
    role_receipt = receipt["roles"][a.role]
    if not matches(ROOT, a.role, role_receipt.get("source_manifest")):
        p.error("Build source/hash provenance missing or changed. Recompile this role before flashing.")
    offline = scan(ROOT)
    if not offline.get('flash_preflight_pass'):
        p.error('Both roles must pass offline image/hash/source preflight before any flash')
    folder = ROOT/"dist"/a.role
    for name, meta in role_receipt["artifacts"].items():
        file = folder/name
        if not file.exists() or hashlib.sha256(file.read_bytes()).hexdigest() != meta["sha256"]:
            p.error(f"Artifact changed: {name}")
    cli = shutil.which(a.cli)
    if not cli:
        p.error("arduino-cli not found")
    command = [cli, "upload", "--fqbn", receipt["fqbn"], "--port", a.port,
               "--input-dir", str(folder), str(ROOT/"firmware"/f"{a.role.upper()}_DUAL_DS_CIR")]
    with (folder/"upload.log").open("w") as log:
        rc = subprocess.run(command, stdout=log, stderr=subprocess.STDOUT).returncode
    result = {"role": a.role, "port": a.port, "command": command, "exit_code": rc,
              "independent_flash_readback_verified": False, "hardware_rf_tested": False}
    (folder/"upload_receipt.json").write_text(json.dumps(result, indent=2))
    print(json.dumps(result, indent=2))
    return rc

if __name__ == "__main__":
    sys.exit(main())
