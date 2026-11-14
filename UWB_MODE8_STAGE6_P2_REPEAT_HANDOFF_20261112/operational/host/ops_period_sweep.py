#!/usr/bin/env python3
"""Explicit Stage6 physical period sweep: 200 -> 195 -> 190 ms, no RF retry.

This is NOT approval of sub-200ms operation. All four CIRs and paired RF
timestamps remain required. The 188ms nominal code floor is not field proof.
Requires both anchor and tag directly wired to one USB host.
"""
from __future__ import annotations
import argparse
import json
from pathlib import Path
import subprocess
import sys
from ops_wired_continuous import verify_receipt

PAIR=Path(__file__).resolve().parents[2]/"firmware/anchor_p2_repeat/tools/run_pair.py"
PERIODS=(200000,195000,190000)

def command_for(anchor,tag,out,count,period,initial_anchor=921600,initial_tag=921600,
                calibration_profile=None):
    if period not in PERIODS or type(count) is not int or not 1<=count<=5000:
        raise ValueError("only explicit 200/195/190ms 1..5000 trial counts")
    if not anchor or not tag or anchor==tag:raise ValueError("two separate USB ports required")
    if initial_anchor not in (921600,2000000) or initial_tag not in (921600,2000000):
        raise ValueError("invalid boot baud")
    command = [sys.executable,str(PAIR),"--anchor",anchor,"--tag",tag,
               "--out",str(out),"--stage","6","--period-us",str(period),
               "--baud","2000000","--count",str(count),
               "--anchor-initial-baud",str(initial_anchor),
               "--tag-initial-baud",str(initial_tag)]
    if calibration_profile is not None:
        command.extend(("--calibration-profile", str(calibration_profile)))
    return command

def inspect_trial(path:Path,expected_count:int,period:int):
    verified = verify_receipt(path, expected_count, period_us=period)
    return {"period_us":period,"count":expected_count,"p1_p2_grids":verified["grids"],
            "rf_evidence":verified["evidence"],"period_field_approved":False}

def main()->int:
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument("--anchor",required=True)
    p.add_argument("--tag",required=True)
    p.add_argument("--out",type=Path,required=True)
    p.add_argument("--count",type=int,default=20)
    p.add_argument("--anchor-initial-baud",type=int,choices=(921600,2000000),default=921600)
    p.add_argument("--tag-initial-baud",type=int,choices=(921600,2000000),default=921600)
    p.add_argument("--calibration-profile", type=Path)
    p.add_argument("--execute",action="store_true")
    p.add_argument("--wifi-off-confirmed",action="store_true")
    args=p.parse_args()
    try:
        if args.out.exists():raise ValueError("output folder already exists")
        commands=[command_for(args.anchor,args.tag,args.out/f"period_{period}",
                              args.count,period,
                              args.anchor_initial_baud if i == 0 else 921600,
                              args.tag_initial_baud if i == 0 else 921600,
                              args.calibration_profile)
                  for i, period in enumerate(PERIODS)]
        if not args.execute:
            print(json.dumps({"mode":"DRY_RUN_NO_RF","commands":commands},indent=2))
            return 0
        if not args.wifi_off_confirmed:raise ValueError("explicit Wi-Fi OFF confirmation required")
        args.out.mkdir(parents=True,exist_ok=False)
        results=[]
        for period,cmd in zip(PERIODS,commands):
            with (args.out/f"period_{period}.log").open("xb") as log:
                rc=subprocess.run(cmd,stdout=log,stderr=subprocess.STDOUT,check=False,
                                  timeout=int(args.count*period/1e6+120))
            if rc.returncode:
                raise RuntimeError(f"{period}us failed; do NOT continue to shorter periods")
            results.append(inspect_trial(args.out/f"period_{period}",args.count,period))
        verdict={"status":"THREE_PERIODS_CAPTURED_NEEDS_FIELD_APPROVAL",
                 "rf_period_190ms_approved":False,"results":results}
        (args.out/"sweep_receipt.json").write_text(json.dumps(verdict,indent=2)+"\n")
        print(json.dumps(verdict,indent=2))
        return 0
    except (ValueError,OSError,RuntimeError,subprocess.TimeoutExpired) as e:
        print("HOLD: "+str(e),file=sys.stderr)
        return 2

if __name__=="__main__":raise SystemExit(main())
