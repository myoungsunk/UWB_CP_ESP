#!/usr/bin/env python3
"""Fail-closed *host-side* ESP32 image identity check before a maintenance update.

This does NOT substitute for ESP32 secure boot, signed-OTA enforcement or
bootloader rollback. No physical flash operation is performed here.
"""
from __future__ import annotations
import argparse
import hashlib
import json
from pathlib import Path
import os
import hmac

FQBN="esp32:esp32:esp32doit-devkit-v1"
CORE="3.3.7"
IMAGES={
 "anchor":"ANCHOR_DUAL_DS_CIR.ino.bin",
 "tag":"TAG_DUAL_DS_CIR.ino.bin"
}
class ImageHold(ValueError):pass

def verify(image:Path, receipt:Path, role:str)->dict:
    if role not in IMAGES:raise ImageHold("invalid role")
    if image.name!=IMAGES[role]:raise ImageHold("image file name/role mismatch")
    doc=json.loads(receipt.read_text(encoding="utf-8"))
    if (doc.get("target_build") is not True or doc.get("fqbn")!=FQBN or
        doc.get("required_core")!=CORE):
        raise ImageHold("unverified build target/core")
    manifest=(doc.get("roles") or {}).get(role) or {}
    entry=(manifest.get("artifacts") or {}).get(image.name) or {}
    data=image.read_bytes()
    if len(data)<256 or data[0]!=0xe9:
        raise ImageHold("invalid ESP32 application image header")
    sha=hashlib.sha256(data).hexdigest()
    if (manifest.get("exit_code")!=0 or not isinstance(manifest.get("source_manifest"),dict)
        or entry.get("sha256")!=sha or entry.get("bytes")!=len(data)):
        raise ImageHold("image SHA/size/compile provenance mismatch")
    # For ESP-IDF images with hash_appended, the partition SHA API returns
    # the 32-byte appended image hash, not sha256(full-image-with-hash).
    has_hash=data[23]==1
    if has_hash:
        if len(data)<56 or hashlib.sha256(data[:-32]).digest()!=data[-32:]:
            raise ImageHold("invalid ESP32 appended image digest")
        partition_sha=data[-32:].hex()
    else:
        partition_sha=sha
    return {"role":role,"file_sha256":sha,"partition_sha256":partition_sha,
            "bytes":len(data),"core":CORE,"fqbn":FQBN,
            "source_manifest_sha256":hashlib.sha256(json.dumps(
                manifest["source_manifest"],sort_keys=True,
                separators=(",",":")).encode()).hexdigest()}

def signed_host_intent(proof:dict,key:bytes)->dict:
    # Management receipt for offline operator evidence. The target does not
    # enforce this HMAC unless a separately implemented device verifier exists.
    if len(key)!=32:raise ImageHold("32-byte independent operator key required")
    canonical=json.dumps(proof,sort_keys=True,separators=(",",":")).encode()
    return {**proof,"host_intent_hmac_sha256":hmac.new(key,canonical,hashlib.sha256).hexdigest()}

def main():
    ap=argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--role",choices=tuple(IMAGES),required=True)
    ap.add_argument("--image",type=Path,required=True)
    ap.add_argument("--receipt",type=Path,required=True)
    ap.add_argument("--out",type=Path)
    ap.add_argument("--intent-key-env",default="MODE8_DEPLOY_INTENT_KEY_HEX")
    args=ap.parse_args()
    try:
        proof=verify(args.image,args.receipt,args.role)
        token=os.getenv(args.intent_key_env)
        if token:
            key=bytes.fromhex(token)
            proof=signed_host_intent(proof,key)
    except (ImageHold,ValueError,OSError) as err:
        ap.error(f"OTA/flash HOLD: {err}")
    data=json.dumps(proof,indent=2)
    if args.out:args.out.write_text(data+"\n",encoding="utf-8")
    print(data)
    return 0
if __name__=="__main__":raise SystemExit(main())
