#!/usr/bin/env python3
"""Offline Mode8 RF-clock to robot-odom pose alignment with explicit sync pairs.

REQUIRED: time_sync.csv contains independently observed SAME-event
radio_ticks / ROS clock ns pairs PER radio domain. ESP32 UART arrival time
is NOT a substitute for a proven physical timesync event.
Every source clock is independent (anchor/tagA/tagB).
"""
from __future__ import annotations
import argparse
from bisect import bisect_left
import csv
import json
import math
from pathlib import Path

MOD=1<<40
MASK=MOD-1
HALF=MOD>>1
TICK_NS=1e9/63897600000.0

class AlignmentHold(ValueError):pass

def read_rows(path:Path,required:set[str]):
    with path.open(newline="",encoding="utf-8-sig") as f:
        reader=csv.DictReader(f)
        if not reader.fieldnames or not required.issubset(set(reader.fieldnames)):
            raise AlignmentHold(f"{path.name}: missing columns {sorted(required)}")
        rows=list(reader)
    if not rows:raise AlignmentHold(f"{path.name}: empty")
    return rows

def unwrap(raw):
    if not raw:raise AlignmentHold("empty DW tick series")
    if any(type(x) is not int or not 0<=x<MOD for x in raw):
        raise AlignmentHold("invalid 40-bit raw tick")
    result=[0]
    for a,b in zip(raw,raw[1:]):
        delta=(b-a)&MASK
        if not 0<delta<HALF:
            raise AlignmentHold("out-of-order/duplicate DW clock or ambiguous >8.6s gap")
        result.append(result[-1]+delta)
    return result

def fit_domain(rows, max_rms_ns, max_ppm):
    if len(rows)<3:raise AlignmentHold("at least three physical sync events required per radio")
    raw=[int(x["radio_ticks"]) for x in rows]
    ros=[int(x["ros_ns"]) for x in rows]
    if any(b<=a for a,b in zip(ros,ros[1:])):
        raise AlignmentHold("nonmonotonic ROS time sync")
    x=unwrap(raw)
    if ros[-1]-ros[0]<1_000_000_000:
        raise AlignmentHold("sync span under 1s; clock drift unidentifiable")
    mx=sum(x)/len(x);my=sum(ros)/len(ros)
    xx=sum((v-mx)**2 for v in x)
    if xx<=0:raise AlignmentHold("no radio timing leverage")
    slope=sum((v-mx)*(t-my) for v,t in zip(x,ros))/xx
    ppm=(slope/TICK_NS-1)*1e6
    if abs(ppm)>max_ppm:raise AlignmentHold(f"RF/ROS slope deviation {ppm:.1f} ppm")
    residual=[(my+slope*(v-mx))-t for v,t in zip(x,ros)]
    rms=math.sqrt(sum(v*v for v in residual)/len(residual))
    if rms>max_rms_ns:raise AlignmentHold(f"sync RMS {rms/1e6:.3f} ms exceeds gate")
    return {"raw0":raw[0],"span":x[-1],"mean_x":mx,"mean_ns":my,
            "slope_ns_per_tick":slope,"ppm":ppm,"rms_ns":rms,"samples":len(x)}

def rf_to_ros(raw,model,approx_ns=None,max_approx_ns=3_000_000_000):
    if not 0<=raw<MOD:raise AlignmentHold("RF observation outside DW40")
    base=(raw-model["raw0"])&MASK
    end=model["span"]
    choices=[base+k*MOD for k in range(end//MOD+1) if base+k*MOD<=end]
    if not choices:raise AlignmentHold("RF event outside measured sync interval")
    def ns_for(x):return model["mean_ns"]+model["slope_ns_per_tick"]*(x-model["mean_x"])
    if len(choices)>1 and approx_ns is None:
        raise AlignmentHold("40-bit wrap ambiguous: coarse host time required")
    if approx_ns is None:chosen=choices[0]
    else:
        chosen=min(choices,key=lambda x:abs(ns_for(x)-approx_ns))
        if abs(ns_for(chosen)-approx_ns)>max_approx_ns:
            raise AlignmentHold("RF wrap selection outside coarse ROS time gate")
    return round(ns_for(chosen))

def interpolate_pose(poses,ros_ns):
    stamps=[row[0] for row in poses]
    i=bisect_left(stamps,ros_ns)
    if i==len(stamps) or (i==0 and stamps[0]!=ros_ns):
        raise AlignmentHold("observation outside odom bounds; extrapolation forbidden")
    if stamps[i]==ros_ns:return poses[i][1:]
    l,r=poses[i-1],poses[i]
    weight=(ros_ns-l[0])/(r[0]-l[0])
    yaw_diff=math.atan2(math.sin(r[3]-l[3]),math.cos(r[3]-l[3]))
    yaw=l[3]+weight*yaw_diff
    return (l[1]+weight*(r[1]-l[1]),
            l[2]+weight*(r[2]-l[2]),
            math.atan2(math.sin(yaw),math.cos(yaw)))

def align(sync_rows,pose_rows,obs_rows,max_rms_ms=2.0,max_ppm=200.0,max_coarse_ms=3000.0):
    if not 0<max_rms_ms<=100 or not 0<max_ppm<=10000 or not 0<max_coarse_ms<=8000:
        raise AlignmentHold("unsafe gate settings")
    grouped={}
    for row in sync_rows:
        domain=row["domain"].strip()
        if not domain:raise AlignmentHold("empty radio domain")
        grouped.setdefault(domain,[]).append(row)
    models={key:fit_domain(rows,max_rms_ms*1e6,max_ppm) for key,rows in grouped.items()}
    poses=[]
    for row in pose_rows:
        stamp=int(row["ros_ns"])
        p=(stamp,float(row["x_m"]),float(row["y_m"]),float(row["yaw_rad"]))
        if not all(math.isfinite(x) for x in p[1:]):raise AlignmentHold("nonfinite odom")
        poses.append(p)
    if any(b[0]<=a[0] for a,b in zip(poses,poses[1:])):
        raise AlignmentHold("unsorted or duplicate odom timestamps")
    if len(poses)<2:raise AlignmentHold("odom requires at least two poses")
    results=[]
    for row in obs_rows:
        domain=row["domain"].strip()
        if domain not in models:raise AlignmentHold("unknown RF clock domain")
        raw=int(row["radio_ticks"])
        approx=row.get("approx_ros_ns","").strip()
        ns=rf_to_ros(raw,models[domain],int(approx) if approx else None,
                     max_coarse_ms*1e6)
        x,y,yaw=interpolate_pose(poses,ns)
        results.append({"domain":domain,"session":row["session"],"txn":row["txn"],
                        "probe":row["probe"],"radio_ticks":raw,"rf_ros_ns":ns,
                        "x_m":x,"y_m":y,"yaw_rad":yaw,
                        "sync_rms_ms":models[domain]["rms_ns"]/1e6,
                        "sync_ppm":models[domain]["ppm"]})
    return results,models

def main():
    ap=argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--sync",type=Path,required=True,help="domain,radio_ticks,ros_ns")
    ap.add_argument("--odom",type=Path,required=True,help="ros_ns,x_m,y_m,yaw_rad")
    ap.add_argument("--rf",type=Path,required=True,help="domain,session,txn,probe,radio_ticks[,approx_ros_ns]")
    ap.add_argument("--out",type=Path,required=True)
    ap.add_argument("--max-rms-ms",type=float,default=2.0)
    ap.add_argument("--max-ppm",type=float,default=200.0)
    ap.add_argument("--max-coarse-ms",type=float,default=3000.0)
    a=ap.parse_args()
    try:
        if a.out.exists() or a.out.with_suffix(".verdict.json").exists():
            raise AlignmentHold("output exists; no overwrite")
        data,models=align(read_rows(a.sync,{"domain","radio_ticks","ros_ns"}),
                          read_rows(a.odom,{"ros_ns","x_m","y_m","yaw_rad"}),
                          read_rows(a.rf,{"domain","session","txn","probe","radio_ticks"}),
                          a.max_rms_ms,a.max_ppm,a.max_coarse_ms)
        with a.out.open("x",newline="",encoding="utf-8") as f:
            writer=csv.DictWriter(f,fieldnames=list(data[0]))
            writer.writeheader();writer.writerows(data)
        verdict={"status":"SYNC_MODEL_VALIDATED","rows":len(data),
                 "domains":{name:{"rms_ns":m["rms_ns"],"ppm":m["ppm"],
                                  "samples":m["samples"]} for name,m in models.items()},
                 "physical_sync_pairs_user_supplied":True,
                 "absolute_antenna_delay_calibrated":False}
        a.out.with_suffix(".verdict.json").write_text(json.dumps(verdict,indent=2)+"\n",encoding="utf-8")
        print(json.dumps(verdict,indent=2));return 0
    except (OSError,ValueError,KeyError) as e:
        ap.error("TIMESYNC_HOLD: "+str(e))

if __name__=="__main__":
    raise SystemExit(main())
