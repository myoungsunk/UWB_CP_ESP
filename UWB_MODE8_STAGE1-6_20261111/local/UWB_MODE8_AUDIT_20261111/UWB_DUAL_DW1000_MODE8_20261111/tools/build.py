#!/usr/bin/env python3
"""Actual ESP32 Arduino build. Never treats a host compile as a target build."""
from __future__ import annotations
import argparse
import hashlib
import json
from pathlib import Path
import shutil
import subprocess
import sys
from datetime import datetime, timezone
from sync_shared import sync
ROOT = Path(__file__).resolve().parents[1]
FQBN = "esp32:esp32:esp32doit-devkit-v1"
CORE = "3.3.7"
INDEX = "https://espressif.github.io/arduino-esp32/package_esp32_index.json"

def sha(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()

def main() -> int:
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--cli", default="arduino-cli")
    p.add_argument("--role", choices=("all", "anchor", "tag"), default="all")
    p.add_argument("--install", action="store_true", help="Install exact core 3.3.7, requires network")
    args = p.parse_args()
    output = ROOT/"dist";output.mkdir(exist_ok=True)
    receipt = {"created_utc": datetime.now(timezone.utc).isoformat(), "target_build": False,
               "hardware_tested": False, "fqbn": FQBN, "required_core": CORE, "roles": {}}
    try:
        cli = shutil.which(args.cli)
        if not cli:
            raise RuntimeError("arduino-cli not installed/on PATH; see docs/BUILD_AND_FLASH_KO.md")
        sync()
        receipt["arduino_cli"] = subprocess.check_output([cli, "version"], text=True).strip()
        if args.install:
            with (output/"core_install.log").open("w", encoding="utf-8") as f:
                for cmd in ([cli, "core", "update-index", "--additional-urls", INDEX],
                            [cli, "core", "install", "esp32:esp32@"+CORE, "--additional-urls", INDEX]):
                    subprocess.run(cmd, stdout=f, stderr=subprocess.STDOUT, check=True)
        listing = subprocess.check_output([cli, "core", "list", "--json"], text=True)
        (output/"installed_cores.json").write_text(listing, encoding="utf-8")
        cores = json.loads(listing)
        # Arduino CLI 1.x wraps the list in 'platforms'; older versions return a list.
        platforms = cores.get("platforms", []) if isinstance(cores, dict) else cores
        matches = [c for c in platforms if c.get("id") == "esp32:esp32"]
        def installed_version(c: dict) -> str:
            for key in ("installed", "installed_version"):
                v = c.get(key)
                if isinstance(v, str): return v
                if isinstance(v, dict): return str(v.get("version", ""))
            release = c.get("installed_release", {})
            return str(release.get("version", "")) if isinstance(release, dict) else ""
        if not matches or installed_version(matches[0]) != CORE:
            raise RuntimeError(f"Exact esp32:esp32@{CORE} required. Run again with --install.")
        roles = ("anchor", "tag") if args.role == "all" else (args.role,)
        for role in roles:
            sketch = ROOT/"firmware"/f"{role.upper()}_DUAL_DS_CIR"
            build = ROOT/"build"/role
            out = output/role
            # Remove previous products: never let an old app image satisfy this run.
            if build.exists(): shutil.rmtree(build)
            if out.exists(): shutil.rmtree(out)
            out.mkdir(exist_ok=True)
            cmd = [cli, "compile", "--fqbn", FQBN, "--warnings", "all",
                   "--library", str(ROOT/"vendor"/role/"DW1000Ng"),
                   "--build-path", str(build), "--output-dir", str(out), str(sketch)]
            with (out/"compile.log").open("w", encoding="utf-8") as f:
                f.write("COMMAND: "+json.dumps(cmd)+"\n");f.flush()
                run = subprocess.run(cmd, stdout=f, stderr=subprocess.STDOUT)
            result = {"command": cmd, "exit_code": run.returncode, "artifacts": {}}
            receipt["roles"][role] = result
            if run.returncode != 0:
                raise RuntimeError(f"{role} target compile failed; see {out/'compile.log'}")
            # Retain the ELF/map from the build directory if CLI did not copy them.
            for pattern in ("*.elf", "*.map"):
                for file in build.glob(pattern): shutil.copy2(file, out/file.name)
            for file in sorted(out.iterdir()):
                if file.is_file() and file.suffix in (".bin", ".elf", ".map"):
                    result["artifacts"][file.name] = {"bytes": file.stat().st_size, "sha256": sha(file)}
            app = out/f"{sketch.name}.ino.bin"
            if not app.exists() or not app.stat().st_size:
                raise RuntimeError(f"Missing application image: {app}")
        receipt["target_build"] = True
    except Exception as e:
        receipt["error"] = str(e)
    (output/"target_build_receipt.json").write_text(json.dumps(receipt, indent=2), encoding="utf-8")
    print(json.dumps(receipt, indent=2))
    return 0 if receipt["target_build"] else 2

if __name__ == "__main__":
    sys.exit(main())
