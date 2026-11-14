#!/usr/bin/env python3
"""Explicit wired-only continuous Mode8 Stage6 paired capture (two USB links).

This is an alternative to the <=32-record Wi-Fi-off anchor archive. It
requires two *physical* UART/USB connections to the same host; it does NOT
make the remote wireless anchor capable of gap-free 1000-transaction storage.
No RF remeasurement is performed automatically.
"""
from __future__ import annotations
import argparse
import hashlib
import json
from pathlib import Path
import subprocess
import sys

ROOT=Path(__file__).resolve().parents[2]
PAIR_SCRIPT=ROOT/"firmware/tag_p2_repeat/tools/run_pair.py"

def command_for(anchor, tag, out, count, anchor_baud, tag_baud, calibration_profile=None):
    if not anchor or not tag or anchor==tag:
        raise ValueError("two different USB serial ports are required")
    if type(count) is not int or not 1<=count<=100000:
        raise ValueError("record count must be 1..100000")
    if anchor_baud not in (921600,2000000) or tag_baud not in (921600,2000000):
        raise ValueError("invalid boot baud")
    command = [sys.executable,str(PAIR_SCRIPT),"--anchor",anchor,"--tag",tag,
               "--out",str(out),"--stage","6","--period-us","200000",
               "--baud","2000000","--count",str(count),"--durable-tag-ack",
               "--anchor-initial-baud",str(anchor_baud),
               "--tag-initial-baud",str(tag_baud)]
    if calibration_profile is not None:
        command.extend(("--calibration-profile", str(calibration_profile)))
    return command

def digest_file(path):
    h=hashlib.sha256()
    with path.open("rb") as f:
        for chunk in iter(lambda:f.read(65536),b""):
            h.update(chunk)
    return h.hexdigest()

def verify_receipt(out, count, *, period_us=200000):
    receipt=json.loads((out/"capture_receipt.json").read_text(encoding="utf-8"))
    if any((receipt.get("pass") is not True,receipt.get("stage")!=6,
            receipt.get("period_us")!=period_us,receipt.get("baud")!=2000000,
            receipt.get("requested_count")!=count)):
        raise RuntimeError("Stage6 paired USB strict gate failed")
    grids=(receipt.get("stage6") or {}).get("common_axis_and_power") or {}
    if grids.get("P1")!=count or grids.get("P2")!=count:
        raise RuntimeError("P1 or P2 common grids not complete")
    recorded_hashes = receipt.get("file_sha256")
    if not isinstance(recorded_hashes, dict):
        raise RuntimeError("paired collector receipt is missing original RFB8 hashes")
    evidence={}
    for name in ("anchor.rfb8","tag.rfb8"):
        p=out/name
        if not p.is_file() or not p.stat().st_size:
            raise RuntimeError("original RFB8 missing: "+name)
        digest = digest_file(p)
        if digest != recorded_hashes.get(name):
            raise RuntimeError("original RFB8 SHA-256 mismatch: "+name)
        evidence[name]={"sha256":digest,"bytes":p.stat().st_size}
    return {"schema":"mode8.wired.v1","status":"VERIFIED","count":count,
            "stage":6,"period_us":period_us,"grids":grids,"evidence":evidence,
            "rf_automated_retry":False,"accuracy_validated":False}

def main():
    ap=argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--anchor",required=True)
    ap.add_argument("--tag",required=True)
    ap.add_argument("--out",required=True,type=Path)
    ap.add_argument("--count",type=int,default=1000)
    ap.add_argument("--anchor-initial-baud",type=int,default=921600,choices=(921600,2000000))
    ap.add_argument("--tag-initial-baud",type=int,default=921600,choices=(921600,2000000))
    ap.add_argument("--calibration-profile", type=Path,
                    help="Existing paired collector antenna-delay profile for this board setup")
    ap.add_argument("--execute",action="store_true",
                    help="RF run is prohibited unless explicitly requested")
    ap.add_argument("--wifi-off-confirmed",action="store_true",
                    help="Operator has verified anchor Wi-Fi OFF for this wired RF run")
    args=ap.parse_args()
    try:
        cmd=command_for(args.anchor,args.tag,args.out,args.count,
                        args.anchor_initial_baud,args.tag_initial_baud,args.calibration_profile)
        if args.out.exists():
            raise ValueError("destination already exists; original capture cannot be overwritten")
        if not args.execute:
            print(json.dumps({"status":"DRY_RUN_NO_RF","command":cmd},indent=2))
            return 0
        if not args.wifi_off_confirmed:
            raise ValueError("wifi-off-confirmed is required before RF")
        args.out.mkdir(parents=True,exist_ok=False)
        (args.out/"wired_intent.json").write_text(
            json.dumps({"status":"REQUESTED","command":cmd,"count":args.count,
                        "wifi_off_operator_asserted":True},indent=2)+"\n",encoding="utf-8")
        # Existing run_pair.py owns both serial links, RF START/STOP, CRC/FEC,
        # durable original capture and strict P1/P2 audit.
        with (args.out/"collector.log").open("wb") as log:
            result=subprocess.run(cmd,stdout=log,stderr=subprocess.STDOUT,
                                  check=False,timeout=int(args.count*.2+150))
        if result.returncode!=0:
            raise RuntimeError("paired USB collector failed; preserve raw evidence")
        proof=verify_receipt(args.out,args.count)
        (args.out/"wired_verified.json").write_text(json.dumps(proof,indent=2)+"\n",encoding="utf-8")
        print(json.dumps(proof,indent=2))
        return 0
    except (ValueError,RuntimeError,OSError,subprocess.TimeoutExpired) as err:
        print("HOLD (NO AUTOMATIC RF RESTART): "+str(err),file=sys.stderr)
        return 2

if __name__=="__main__":
    raise SystemExit(main())
