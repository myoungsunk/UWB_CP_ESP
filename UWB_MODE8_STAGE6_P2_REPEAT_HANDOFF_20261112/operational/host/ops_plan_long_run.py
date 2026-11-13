#!/usr/bin/env python3
"""Create a strictly Mode8 Stage6 segmented route plan from a requested count.

Offline only: produces a plan, does NOT send RF commands.
Segmenting is NOT 5 Hz continuous operation. Each point has an explicit
Wi-Fi reconnect, archive commit, paired RF audit and purge boundary.
"""
from __future__ import annotations
import argparse
import json
import os
from pathlib import Path
import re
import tempfile

MAX_POINTS=10000
MAX_COUNT=32
POINT=re.compile(r"^[A-Za-z][A-Za-z0-9_-]{0,20}$")

def make_plan(route_id:int,total:int,per_point:int=32,prefix:str="P")->dict:
    if type(route_id) is not int or not 1<=route_id<=0xffffffff:
        raise ValueError("route id must be an unsigned nonzero 32-bit integer")
    if type(per_point) is not int or not 1<=per_point<=MAX_COUNT:
        raise ValueError("per-point count must be 1..32")
    if type(total) is not int or not 1<=total<=MAX_POINTS*per_point:
        raise ValueError("total observations must fit <=10000 points")
    if not isinstance(prefix,str) or not POINT.fullmatch(prefix):
        raise ValueError("invalid point prefix")
    number=(total+per_point-1)//per_point
    width=max(4,len(str(number-1)))
    if len(prefix)+1+width>32:
        raise ValueError("point identifiers exceed 32 characters")
    points=[{"point_id":f"{prefix}_{i:0{width}d}",
             "count":min(per_point,total-i*per_point)}
            for i in range(number)]
    return {"route_id":route_id,"mode":8,"stage":6,
            "period_us":200000,"points":points}

def save_new(path:Path,plan:dict):
    # A new immutable plan; never replace a pending or used route contract.
    if path.exists():raise FileExistsError(path)
    path.parent.mkdir(parents=True,exist_ok=True)
    fd,name=tempfile.mkstemp(prefix=path.name+".",suffix=".tmp",dir=path.parent)
    try:
        with os.fdopen(fd,"w",encoding="utf-8") as file:
            json.dump(plan,file,indent=2,sort_keys=True)
            file.write("\n");file.flush();os.fsync(file.fileno())
        os.link(name,path)  # fails if a concurrent process published the plan
        if os.name=="posix":
            fd=os.open(path.parent,os.O_RDONLY|os.O_DIRECTORY)
            try:os.fsync(fd)
            finally:os.close(fd)
    finally:
        if os.path.exists(name):os.unlink(name)

def main()->int:
    ap=argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--route-id",type=int,required=True)
    ap.add_argument("--total",type=int,required=True)
    ap.add_argument("--per-point",type=int,default=32)
    ap.add_argument("--prefix",default="P")
    ap.add_argument("--out",type=Path,required=True)
    a=ap.parse_args()
    try:
        plan=make_plan(a.route_id,a.total,a.per_point,a.prefix)
        from ops_route import validate_plan
        validate_plan(plan)
        save_new(a.out,plan)
    except (ValueError,OSError) as e:
        ap.error(f"MODE8 PLAN HOLD: {e}")
    print(json.dumps({"out":str(a.out),"points":len(plan["points"]),
                      "observations":a.total,"wireless_gap_free":False},indent=2))
    return 0

if __name__=="__main__":
    raise SystemExit(main())
