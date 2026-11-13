#!/usr/bin/env python3
"""Mode 8 only: durable multi-point capture with explicit RF gaps.

Each point runs the proven <=32-transaction offline Wi-Fi Mode 8 session.
This is NOT gap-free 5 Hz recording, robot motion control, or RF retransmission.
After a lost START reply, no automatic rerun of the same point is permitted.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import secrets
import subprocess
import sys
import tempfile
import time

from ops_archive import parse as parse_archive
from ops_client import OperationalClient

MAX_POINTS=10000
POINT_ID=re.compile(r"^[A-Za-z0-9_-]{1,32}$")
STATES=("PENDING","REQUESTED","VERIFIED","PURGE_INTENT","PURGED")

class RouteHold(RuntimeError):
    """RF session or archival custody must be inspected before any new RF."""


def validate_plan(plan):
    if not isinstance(plan,dict) or type(plan.get("route_id")) is not int or not 0<plan["route_id"]<=0xffffffff:
        raise ValueError("nonzero 32-bit route_id required")
    if plan.get("mode") != 8 or plan.get("stage") != 6 or plan.get("period_us") != 200000:
        raise ValueError("route must explicitly select Mode8 Stage6 200ms")
    points=plan.get("points")
    if not isinstance(points,list) or not 1<=len(points)<=MAX_POINTS:
        raise ValueError("route requires 1..10000 points")
    names=set()
    for point in points:
        if not isinstance(point,dict) or set(point)!={"point_id","count"}:
            raise ValueError("point fields must be point_id and count")
        pid,count=point["point_id"],point["count"]
        if not isinstance(pid,str) or not POINT_ID.fullmatch(pid) or pid in names:
            raise ValueError("duplicate or unsafe point_id")
        if type(count) is not int or not 1<=count<=32:
            raise ValueError("point count must be 1..32")
        names.add(pid)
    sessions=[derive_session(plan["route_id"],i) for i in range(len(points))]
    if len(set(sessions))!=len(sessions):
        raise ValueError("session collision")
    return plan

def derive_session(route_id,index):
    # Odd multiplier gives a bijection modulo 2^32 for distinct indices.
    result=(route_id^(((index+1)*0x9e3779b1)&0xffffffff))&0xffffffff
    if not result: raise ValueError("zero session: choose another route_id")
    return result

def sha256_file(path):
    h=hashlib.sha256()
    with path.open("rb") as f:
        while block:=f.read(65536):h.update(block)
    return h.hexdigest()

def durable_json(path,data):
    path.parent.mkdir(parents=True,exist_ok=True)
    fd,name=tempfile.mkstemp(prefix=path.name+".",suffix=".tmp",dir=path.parent)
    try:
        with os.fdopen(fd,"w",encoding="utf-8") as f:
            json.dump(data,f,indent=2,sort_keys=True)
            f.write("\n");f.flush();os.fsync(f.fileno())
        os.replace(name,path)
        if os.name=="posix":
            dir_fd=os.open(path.parent,os.O_RDONLY|os.O_DIRECTORY)
            try:os.fsync(dir_fd)
            finally:os.close(dir_fd)
    finally:
        if os.path.exists(name):os.unlink(name)

class RouteController:
    def __init__(self,plan,out,anchor_ip,tag_port,key,*,anchor_port=29310,
                 initial_baud=921600,resume=False,manual_confirm=False,timeout=90,
                 approval_key=None):
        self.plan=validate_plan(plan)
        if len(key)!=32:raise ValueError("32-byte HMAC key required")
        self.out=Path(out);self.path=self.out/"route_receipt.json"
        self.anchor_ip=anchor_ip;self.tag_port=tag_port;self.key=key
        self.anchor_port=anchor_port;self.initial_baud=initial_baud
        self.manual_confirm=manual_confirm;self.timeout=timeout
        if approval_key is not None and (len(approval_key)!=32 or approval_key==key):
            raise ValueError("custody key must be separate from TCP login key")
        self.approval_key=approval_key
        digest=hashlib.sha256(json.dumps(plan,sort_keys=True,separators=(",",":")).encode()).hexdigest()
        if resume:
            if not self.path.is_file():raise RouteHold("resume requires existing durable journal")
            self.data=json.loads(self.path.read_text(encoding="utf-8"))
            if (self.data.get("plan_sha256")!=digest or self.data.get("route_id")!=plan["route_id"] or
                len(self.data.get("points",[]))!=len(plan["points"])):
                raise RouteHold("route plan changed; do not resume with a different contract")
        else:
            if self.out.exists():raise FileExistsError(self.out)
            self.out.mkdir(parents=True)
            self.data={"schema":"mode8.route.v1","route_id":plan["route_id"],
                       "plan_sha256":digest,"status":"IN_PROGRESS","points":[
                {"point_id":pt["point_id"],"count":pt["count"],
                 "session":derive_session(plan["route_id"],i),"state":"PENDING"}
                for i,pt in enumerate(plan["points"])]}
            durable_json(self.path,self.data)

    def save(self):
        durable_json(self.path,self.data)

    def point_folder(self,index):
        p=self.data["points"][index]
        return self.out/f"{index:04d}_{p['point_id']}"

    def verify_point(self,index):
        entry=self.data["points"][index]
        folder=self.point_folder(index)
        receipt_file=folder/"operational_receipt.json"
        archive_file=folder/"anchor_archive.bin"
        tag_file=folder/"tag.rfb8"
        if not all(f.is_file() and f.stat().st_size>0 for f in (receipt_file,archive_file,tag_file)):
            raise RouteHold("missing one of durable receipt/anchor archive/tag RFB8")
        receipt=json.loads(receipt_file.read_text(encoding="utf-8"))
        if receipt.get("status")!="SAVED_BOTH_ENDPOINTS" or receipt.get("requested_session")!=entry["session"] or receipt.get("requested_count")!=entry["count"]:
            raise RouteHold("point receipt incomplete or wrong session")
        doc=parse_archive(archive_file.read_bytes())
        if (doc["session"],doc["count"])!=(entry["session"],entry["count"]):
            raise RouteHold("anchor archive mismatch")
        anchor=receipt.get("anchor_identity") or {}
        tag=receipt.get("tag_identity") or {}
        if doc["boot"]!=anchor.get("boot") or doc["config"]!=anchor.get("config_id") or not tag.get("boot"):
            raise RouteHold("pair boot/config provenance missing")
        paired=receipt.get("paired_audit") or {}
        required=("paired_records","p1_common_grid","p2_common_grid")
        if any(paired.get(k)!=entry["count"] for k in required):
            raise RouteHold("paired P1/P2 audit missing")
        # A receipt created earlier is NOT proof the raw files are still good.
        # Reparse tag.rfb8 and recompute both-endpoint P1/P2 provenance before
        # resuming a route or authorizing the anchor to erase its only copy.
        from ops_session import audit_archived_session
        try:
            recomputed=audit_archived_session(
                folder,doc,anchor,tag,receipt.get("tag_capture") or {},
                entry["session"],entry["count"],self.plan["period_us"])
        except Exception as exc:
            raise RouteHold(f"on-disk paired RF re-audit failed: {exc}") from exc
        if any(recomputed.get(k)!=entry["count"] or recomputed.get(k)!=paired.get(k)
               for k in required):
            raise RouteHold("persisted paired audit differs from fresh raw replay")
        proof={"anchor_sha256":sha256_file(archive_file),
               "tag_sha256":sha256_file(tag_file),
               "receipt_sha256":sha256_file(receipt_file),
               "archive_crc":doc["crc32"],"anchor_boot":doc["boot"],
               "tag_boot":tag["boot"]}
        # If a prior route journal was already verified, its immutable proof
        # must match the bytes just read. Never silently refresh a changed SHA.
        for field,value in proof.items():
            if field in entry and entry[field]!=value:
                raise RouteHold(f"point {entry['point_id']} {field} changed after verification")
        return proof

    def anchor_status(self):
        with OperationalClient(self.anchor_ip,self.key,self.anchor_port,timeout=5) as client:
            status=json.loads(client.command("STATUS"))
            if status.get("boot")!=client.boot:raise RouteHold("TCP challenge/STATUS boot mismatch")
            return status

    def purge(self,index):
        entry=self.data["points"][index]
        if entry["state"] not in ("VERIFIED","PURGE_INTENT"):
            raise RouteHold("point is not verified and cannot be purged")
        # Check the actual durable raw bytes AGAIN immediately before purge.
        # The first audit in ops_session was performed before ACKFILE; a local
        # disk error or alteration after that must stop destructive cleanup.
        self.verify_point(index)
        status=self.anchor_status()
        if (entry["state"]=="PURGE_INTENT" and status.get("phase")=="IDLE" and
            status.get("file") is False and status.get("boot")==entry["anchor_boot"]):
            # A previously logged PURGE may have succeeded before the host
            # updated its journal; never remeasure to discover that.
            entry["state"]="PURGED";self.save();return
        if (status.get("phase")!="STORED" or status.get("file") is not True or
            status.get("file_session")!=entry["session"] or
            status.get("file_count")!=entry["count"] or
            status.get("boot")!=entry["anchor_boot"] or
            status.get("file_acked") is not True):
            raise RouteHold("remote archive is not the ACKed original point")
        entry["state"]="PURGE_INTENT";self.save()
        if self.approval_key is None:
            raise RouteHold("separate custody approval key required; no purge")
        with OperationalClient(self.anchor_ip,self.key,self.anchor_port,timeout=8,
                               approval_key=self.approval_key) as client:
            reply=client.purge_archive(entry["session"],entry["archive_crc"],entry["anchor_boot"])
            if reply!="PURGE_OK":raise RouteHold(f"anchor rejected purge: {reply}")
        final=self.anchor_status()
        if final.get("phase")!="IDLE" or final.get("file") is not False:
            raise RouteHold("remote purge not durably confirmed")
        entry["state"]="PURGED";self.save()

    def run_point(self,index):
        entry=self.data["points"][index]
        folder=self.point_folder(index)
        if folder.exists():raise RouteHold("untracked point path already exists; no automatic RF rerun")
        if self.manual_confirm:
            input(f"Move robot to point {entry['point_id']} and press Enter to begin: ")
        # Journal REQUESTED BEFORE the command can reach the anchor.
        entry["state"]="REQUESTED";self.save()
        script=Path(__file__).with_name("ops_session.py")
        command=[sys.executable,str(script),
                 "--anchor-ip",self.anchor_ip,"--anchor-port",str(self.anchor_port),
                 "--tag-port",self.tag_port,"--session",str(entry["session"]),
                 "--count",str(entry["count"]),"--out",str(folder),
                 "--tag-initial-baud",str(self.initial_baud if index==0 else 921600)]
        result=subprocess.run(command,check=False)
        if result.returncode:
            raise RouteHold(f"point {entry['point_id']} capture incomplete (exit {result.returncode}); do not automatically repeat RF")
        proof=self.verify_point(index)
        entry.update(proof);entry["state"]="VERIFIED";self.save()

    def run(self):
        for index,entry in enumerate(self.data["points"]):
            state=entry["state"]
            if state=="PURGED":continue
            if state=="PENDING":
                self.run_point(index)
            elif state=="REQUESTED":
                # Recover ONLY proven successful data; never rerun an uncertain
                # START after crash, loss of reply or partial capture.
                proof=self.verify_point(index)
                entry.update(proof);entry["state"]="VERIFIED";self.save()
            elif state not in ("VERIFIED","PURGE_INTENT"):
                raise RouteHold("unknown point state; operator inspection required")
            self.purge(index)
        self.data["status"]="COMPLETED";self.save()

def main():
    ap=argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--plan",type=Path,required=True)
    ap.add_argument("--out",type=Path,required=True)
    ap.add_argument("--anchor-ip",required=True)
    ap.add_argument("--tag-port",required=True)
    ap.add_argument("--anchor-port",type=int,default=29310)
    ap.add_argument("--key-env",default="MODE8_AUTH_KEY_HEX")
    ap.add_argument("--approval-key-env",default="MODE8_APPROVAL_KEY_HEX")
    ap.add_argument("--tag-initial-baud",type=int,choices=(921600,2000000),default=921600)
    ap.add_argument("--resume",action="store_true")
    ap.add_argument("--manual-confirm",action="store_true")
    args=ap.parse_args()
    key=bytes.fromhex(os.getenv(args.key_env,""))
    approval_key=bytes.fromhex(os.getenv(args.approval_key_env,""))
    if len(approval_key)!=32 or approval_key==key:
        ap.error("distinct MODE8_APPROVAL_KEY_HEX is required before starting any route")
    try:
        ctl=RouteController(json.loads(args.plan.read_text(encoding="utf-8")),
                            args.out,args.anchor_ip,args.tag_port,key,
                            anchor_port=args.anchor_port,initial_baud=args.tag_initial_baud,
                            resume=args.resume,manual_confirm=args.manual_confirm,
                            approval_key=approval_key)
        ctl.run()
        print(f"Mode8 route complete: {ctl.path}")
        return 0
    except (ValueError,RouteHold,FileExistsError,OSError) as exc:
        print(f"HOLD: {exc}; inspect local journal before any RF rerun",file=sys.stderr)
        return 2

if __name__=="__main__":
    raise SystemExit(main())
