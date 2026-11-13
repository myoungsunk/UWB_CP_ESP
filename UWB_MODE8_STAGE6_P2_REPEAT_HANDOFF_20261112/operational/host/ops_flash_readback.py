#!/usr/bin/env python3
"""READ-ONLY, exact-byte ESP32 application flash readback and source provenance.

No program/erase/write operation. The operator supplies the actual RUNNING
partition offset from a trusted boot report/partition map; a guessed offset
cannot prove which image booted. Secure-boot/flash-encrypted chips may not
allow plaintext readback and must use hardware-enforced attestation instead.
"""
from __future__ import annotations
import argparse
import hashlib
import json
from pathlib import Path
import subprocess
import sys
from ops_image_preflight import verify as verify_build, ImageHold

class FlashHold(ValueError):pass

def read_command(port,app_offset,size,out:Path):
    if not port or "\n" in port or "\r" in port:
        raise FlashHold("serial port is invalid")
    if type(app_offset) is not int or not 0x10000<=app_offset<0x400000 or app_offset%0x1000:
        raise FlashHold("operator-supplied app partition offset must be aligned, non-bootloader")
    if type(size) is not int or not 256<=size<=0x200000:
        raise FlashHold("unreasonable application image length")
    return [sys.executable,"-m","esptool","--chip","esp32","--port",port,
            "--baud","460800","read-flash",hex(app_offset),str(size),str(out)]

def match_readback(image:Path,readback:Path,proof:dict):
    expected=image.read_bytes()
    actual=readback.read_bytes()
    if actual!=expected:
        raise FlashHold("readback differs from verified application bytes (wrong partition, unflashed, or encrypted)")
    digest=hashlib.sha256(actual).hexdigest()
    if proof["bytes"]!=len(actual) or proof["file_sha256"]!=digest:
        raise FlashHold("readback differs from the exact source-bound image")
    return {"sha256":digest,"bytes":len(actual),"byte_identical":True,
            "role":proof["role"],"source_manifest_sha256":proof["source_manifest_sha256"]}

def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument("--role",required=True,choices=("tag","anchor"))
    p.add_argument("--image",required=True,type=Path)
    p.add_argument("--build-receipt",required=True,type=Path)
    p.add_argument("--serial",required=True)
    p.add_argument("--running-app-offset",required=True,type=lambda s:int(s,0),
                   help="Verified RUNNING app partition, not a guessed slot")
    p.add_argument("--out",required=True,type=Path)
    p.add_argument("--execute-readback",action="store_true",help="Perform read-only esptool read-flash")
    a=p.parse_args()
    try:
        proof=verify_build(a.image,a.build_receipt,a.role)
        bin_path=a.out.with_suffix(".readback.bin")
        if a.out.exists() or bin_path.exists():
            raise FlashHold("refuse to overwrite existing readback evidence")
        cmd=read_command(a.serial,a.running_app_offset,proof["bytes"],bin_path)
        if not a.execute_readback:
            print(json.dumps({"status":"DRY_RUN_NO_DEVICE_IO","command":cmd,"source":proof},indent=2))
            return 0
        if not a.out.parent.is_dir():
            raise FlashHold("output parent must exist for durable readback")
        with a.out.with_suffix(".readback.log").open("xb") as log:
            result=subprocess.run(cmd,stdout=log,stderr=subprocess.STDOUT,check=False,timeout=180)
        if result.returncode:
            raise FlashHold("esptool readback failed; never treat as matching")
        matched=match_readback(a.image,bin_path,proof)
        receipt={"status":"FLASH_APPLICATION_BYTES_MATCHED","running_offset_claim":a.running_app_offset,
                 "role":a.role,"readback":matched,
                 "boot_execution_attested":False,
                 "wifi_rf_ota_field_approved":False,
                 "full_partition_backup_verified":False}
        with a.out.open("x",encoding="utf-8") as f:
            json.dump(receipt,f,indent=2);f.flush()
        print(json.dumps(receipt,indent=2))
        return 0
    except (OSError,ValueError,subprocess.TimeoutExpired) as e:
        p.error(f"FLASH_HOLD: {e}")

if __name__=="__main__":
    raise SystemExit(main())
