#!/usr/bin/env python3
"""Bounded two-USB Mode8 repeated-run soak with durable no-retry custody.

Each run itself can stream 1000 transactions continuously. BETWEEN runs RF
stops; a 24h batch is NOT claimed as one uninterrupted RF epoch.
Host/device failure or power loss remains HOLD, never auto-restarted.
"""
from __future__ import annotations
import argparse
import json
from pathlib import Path
import shutil
import subprocess
import sys
import time

from ops_route import durable_json, route_lock, RouteHold
from ops_wired_continuous import verify_receipt

def requirement_bytes(count):
    if type(count) is not int or not 1<=count<=10000:raise ValueError("1..10000 records/epoch")
    # Conservative disk headroom: raw original RFB8, UART, JSONL, replay and logs.
    return max(268_435_456,count*60000+134_217_728)

def run_plan(anchor,tag,out,count,rounds):
    if not anchor or not tag or anchor==tag:raise ValueError("distinct USB connections required")
    if type(rounds) is not int or not 1<=rounds<=500:raise ValueError("rounds 1..500")
    need=requirement_bytes(count)
    return {"schema":"mode8.soa.v1","status":"NOT_STARTED","count":count,
            "rounds":rounds,"bytes_reserved_per_round":need,
            "rf_gap_free_between_rounds":False,
            "entries":[{"epoch":i,"status":"PENDING"} for i in range(rounds)]}

def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument("--anchor",required=True)
    p.add_argument("--tag",required=True)
    p.add_argument("--out",required=True,type=Path)
    p.add_argument("--count",type=int,default=1000)
    p.add_argument("--rounds",type=int,default=18,help="18 * 1000 * 200ms ~= 1h excluding gaps")
    p.add_argument("--calibration-profile", type=Path)
    p.add_argument("--execute",action="store_true")
    p.add_argument("--wifi-off-confirmed",action="store_true")
    p.add_argument("--anchor-initial-baud",type=int,choices=(921600,2000000),default=921600)
    p.add_argument("--tag-initial-baud",type=int,choices=(921600,2000000),default=921600)
    a=p.parse_args()
    try:
        plan=run_plan(a.anchor,a.tag,a.out,a.count,a.rounds)
        if a.out.exists():raise RouteHold("existing output directory; no automated retry")
        if not a.execute:
            print(json.dumps({**plan,"status":"DRY_RUN_NO_RF"},indent=2))
            return 0
        if not a.wifi_off_confirmed:
            raise RouteHold("operator must verify anchor Wi-Fi OFF")
        with route_lock(a.out.with_name('.'+a.out.name+'.soak.lock')):
            a.out.mkdir(parents=True,exist_ok=False)
            journal=a.out/"soak_receipt.json"
            plan["status"]="IN_PROGRESS"
            durable_json(journal,plan)
            for i,row in enumerate(plan["entries"]):
                if shutil.disk_usage(a.out).free<plan["bytes_reserved_per_round"]:
                    raise RouteHold(f"insufficient disk space before epoch {i}")
                row["status"]="REQUESTED"
                row["requested_monotonic_ns"]=time.monotonic_ns()
                durable_json(journal,plan) # Must persist BEFORE an RF START.
                round_dir=a.out/f"epoch_{i:04d}"
                command=[sys.executable,
                         str(Path(__file__).with_name("ops_wired_continuous.py")),
                         "--anchor",a.anchor,"--tag",a.tag,
                         "--out",str(round_dir),"--count",str(a.count),
                         "--anchor-initial-baud",str(a.anchor_initial_baud if i == 0 else 921600),
                         "--tag-initial-baud",str(a.tag_initial_baud if i == 0 else 921600),
                         "--execute","--wifi-off-confirmed"]
                if a.calibration_profile is not None:
                    command.extend(("--calibration-profile", str(a.calibration_profile)))
                with (a.out/f"epoch_{i:04d}.log").open("xb") as log:
                    rc=subprocess.run(command,stdout=log,stderr=subprocess.STDOUT,
                                      timeout=int(a.count*.2+220),check=False)
                if rc.returncode:
                    raise RouteHold(f"epoch {i} failed; not starting another")
                proof=verify_receipt(round_dir,a.count)
                row.update({"status":"VERIFIED","proof":proof,
                            "finished_monotonic_ns":time.monotonic_ns()})
                durable_json(journal,plan)
            plan["status"]="COMPLETE_BATCH_WITH_EXPLICIT_GAPS"
            durable_json(journal,plan)
        print(json.dumps({"status":plan["status"],"verified_rounds":a.rounds},indent=2))
        return 0
    except (OSError,ValueError,RuntimeError,subprocess.TimeoutExpired) as e:
        print("SOAK_HOLD (do not re-run a requested epoch): "+str(e),file=sys.stderr)
        return 2

if __name__=="__main__":raise SystemExit(main())
