#!/usr/bin/env python3
"""Mode 8 only: durable multi-point capture with explicit RF gaps.

Each point runs the proven <=32-transaction offline Wi-Fi Mode 8 session.
This is NOT gap-free 5 Hz recording, robot motion control, or RF retransmission.
After a lost START reply, no automatic rerun of the same point is permitted.
"""
from __future__ import annotations

import argparse
from contextlib import contextmanager
import hashlib
import hmac
import json
import os
from pathlib import Path
import re
import subprocess
import sys
import tempfile

from ops_archive import parse as parse_archive
from ops_client import OperationalClient

MAX_POINTS=10000
POINT_ID=re.compile(r"^[A-Za-z0-9_-]{1,32}$")
STATES=("PENDING","REQUESTED","VERIFIED","PURGE_INTENT","PURGED")
FILE_HASHES=("receipt_sha256","anchor_sha256","tag_sha256")
PROOF_FIELDS=(*FILE_HASHES,"archive_crc","anchor_boot","tag_boot")

class RouteHold(RuntimeError):
    """RF session or archival custody must be inspected before any new RF."""


def unique_json(text):
    def unique_object(pairs):
        fields={}
        for key,value in pairs:
            if key in fields:
                raise ValueError(f"duplicate JSON key: {key}")
            fields[key]=value
        return fields

    return json.loads(text,object_pairs_hook=unique_object)


def validate_plan(plan):
    if not isinstance(plan,dict) or type(plan.get("route_id")) is not int or not 0<plan["route_id"]<=0xffffffff:
        raise ValueError("nonzero 32-bit route_id required")
    if any(type(plan.get(key)) is not int or plan[key]!=value
           for key,value in (("mode",8),("stage",6),("period_us",200000))):
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

def check_point_file_hashes(receipt_file,archive_file,tag_file,proof):
    for field,path in zip(FILE_HASHES,(receipt_file,archive_file,tag_file)):
        try:
            current=sha256_file(path)
        except OSError as exc:
            raise RouteHold(f"cannot read verified {path.name}: {exc}") from exc
        if current!=proof[field]:
            raise RouteHold(f"{field} changed after verification")


def verify_point_files(receipt_file,archive_file,tag_file,session,count,period_us,*,expected=None):
    paths=(receipt_file,archive_file,tag_file)
    try:
        receipt_bytes,archive_bytes,tag_bytes=(p.read_bytes() for p in paths)
        if not all((receipt_bytes,archive_bytes,tag_bytes)):
            raise RouteHold("missing one of durable receipt/anchor archive/tag RFB8")
        proof={field:hashlib.sha256(data).hexdigest()
               for field,data in zip(FILE_HASHES,(receipt_bytes,archive_bytes,tag_bytes))}
        for field,value in proof.items():
            if expected is not None and expected.get(field)!=value:
                raise RouteHold(f"{field} changed after verification")
        receipt=unique_json(receipt_bytes)
        doc=parse_archive(archive_bytes)
    except (OSError,ValueError) as exc:
        raise RouteHold(f"invalid point evidence: {exc}") from exc
    if not isinstance(receipt,dict) or receipt.get("status")!="SAVED_BOTH_ENDPOINTS" or (
            type(receipt.get("requested_session")) is not int or receipt["requested_session"]!=session or
            type(receipt.get("requested_count")) is not int or receipt["requested_count"]!=count):
        raise RouteHold("point receipt incomplete or wrong session")
    if "raw_sha256" in receipt:
        raw_hashes=receipt["raw_sha256"]
        if not isinstance(raw_hashes,dict) or (
                raw_hashes.get("anchor_archive.bin")!=proof["anchor_sha256"] or
                raw_hashes.get("tag.rfb8")!=proof["tag_sha256"]):
            raise RouteHold("raw files differ from the receipt hashes recorded before ACK")
    if (doc["session"],doc["count"])!=(session,count):
        raise RouteHold("anchor archive mismatch")
    anchor=receipt.get("anchor_identity")
    tag=receipt.get("tag_identity")
    if (not isinstance(anchor,dict) or not isinstance(tag,dict) or
            doc["boot"]!=anchor.get("boot") or doc["config"]!=anchor.get("config_id") or
            type(tag.get("boot")) is not int or not 0<tag["boot"]<=0xffffffff):
        raise RouteHold("pair boot/config provenance missing")
    paired=receipt.get("paired_audit")
    required=("paired_records","p1_common_grid","p2_common_grid")
    if not isinstance(paired,dict) or any(type(paired.get(k)) is not int or paired[k]!=count for k in required):
        raise RouteHold("paired P1/P2 audit missing")
    from ops_session import audit_archived_session
    try:
        recomputed=audit_archived_session(
            tag_file.parent,doc,anchor,tag,receipt.get("tag_capture") or {},
            session,count,period_us,tag_bytes=tag_bytes)
    except Exception as exc:
        raise RouteHold(f"on-disk paired RF re-audit failed: {exc}") from exc
    if any(recomputed.get(k)!=count for k in required):
        raise RouteHold("persisted paired audit differs from fresh raw replay")
    proof.update(archive_crc=doc["crc32"],anchor_boot=doc["boot"],tag_boot=tag["boot"])
    if expected is not None:
        for field,value in proof.items():
            if expected.get(field)!=value:
                raise RouteHold(f"{field} changed after verification")
    check_point_file_hashes(*paths,proof)
    return proof


def validate_journal(data,plan,digest):
    if not isinstance(data,dict) or data.get("schema")!="mode8.route.v1" or (
            data.get("plan_sha256")!=digest or type(data.get("route_id")) is not int or
            data["route_id"]!=plan["route_id"]):
        raise RouteHold("route plan changed or journal identity invalid")
    points=data.get("points")
    if not isinstance(points,list) or len(points)!=len(plan["points"]):
        raise RouteHold("journal point list does not match plan")
    pending_tail=False
    for index,(entry,point) in enumerate(zip(points,plan["points"])):
        if not isinstance(entry,dict) or entry.get("point_id")!=point["point_id"] or (
                type(entry.get("count")) is not int or entry["count"]!=point["count"] or
                type(entry.get("session")) is not int or
                entry["session"]!=derive_session(plan["route_id"],index)):
            raise RouteHold("journal point identity does not match plan")
        state=entry.get("state")
        if state not in STATES or (pending_tail and state!="PENDING"):
            raise RouteHold("invalid journal point state or order")
        pending_tail=state!="PURGED"
        if state in ("VERIFIED","PURGE_INTENT","PURGED"):
            if (any(not isinstance(entry.get(key),str) or not re.fullmatch(r"[0-9a-f]{64}",entry[key])
                    for key in FILE_HASHES) or
                any(type(entry.get(key)) is not int or not low<=entry[key]<=0xffffffff
                    for key,low in (("archive_crc",0),("anchor_boot",1),("tag_boot",1)))):
                raise RouteHold("journal point is missing its verified proof")
        elif any(key in entry for key in PROOF_FIELDS):
            raise RouteHold("journal point has proof before verification")
    status=data.get("status")
    if status not in ("IN_PROGRESS","COMPLETED") or (
            status=="COMPLETED" and any(entry["state"]!="PURGED" for entry in points)):
        raise RouteHold("invalid journal completion status")


@contextmanager
def route_lock(path):
    # Keep one stable lock file so every resume locks the same inode. The OS
    # releases the byte/range automatically if the controller exits.
    with path.open("a+b") as lock:
        lock.seek(0)
        try:
            if os.name=="nt":
                import msvcrt
                msvcrt.locking(lock.fileno(),msvcrt.LK_NBLCK,1)
            else:
                import fcntl
                fcntl.flock(lock.fileno(),fcntl.LOCK_EX|fcntl.LOCK_NB)
        except OSError as exc:
            raise RouteHold("route is locked by another controller or could not be locked") from exc
        yield


def journal_signature(doc: dict, secret: bytes) -> str:
    """Authenticate the journal contents; do not embed the custody key."""
    payload={k:v for k,v in doc.items() if k!="journal_hmac_sha256"}
    data=json.dumps(payload,sort_keys=True,separators=(",",":"),ensure_ascii=False).encode("utf-8")
    return hmac.new(secret,data,hashlib.sha256).hexdigest()




class RouteController:
    def __init__(self,plan,out,anchor_ip,tag_port,key,*,anchor_port=29310,
                 initial_baud=921600,resume=False,manual_confirm=False,timeout=90,
                 approval_key=None,calibration_profile=None):
        self.plan=validate_plan(plan)
        if len(key)!=32:raise ValueError("32-byte HMAC key required")
        self.out=Path(out);self.path=self.out/"route_receipt.json"
        self.anchor_ip=anchor_ip;self.tag_port=tag_port;self.key=key
        self.anchor_port=anchor_port;self.initial_baud=initial_baud
        self.manual_confirm=manual_confirm;self.timeout=timeout
        if approval_key is not None and (len(approval_key)!=32 or approval_key==key):
            raise ValueError("custody key must be separate from TCP login key")
        self.approval_key=approval_key
        self.calibration_profile = Path(calibration_profile) if calibration_profile is not None else None
        self._journal_key=approval_key if approval_key is not None else key
        self.digest=hashlib.sha256(json.dumps(plan,sort_keys=True,separators=(",",":")).encode()).hexdigest()
        if resume:
            if not self.path.is_file():raise RouteHold("resume requires existing durable journal")
            self.data=self.load_journal()
        else:
            if self.out.exists():raise FileExistsError(self.out)
            self.out.mkdir(parents=True)
            self.data={"schema":"mode8.route.v1","route_id":plan["route_id"],
                       "plan_sha256":self.digest,"status":"IN_PROGRESS","points":[
                {"point_id":pt["point_id"],"count":pt["count"],
                 "session":derive_session(plan["route_id"],i),"state":"PENDING"}
                for i,pt in enumerate(plan["points"])]}
            self.save()

    def load_journal(self):
        try:
            data=unique_json(self.path.read_bytes())
        except (OSError,ValueError) as exc:
            raise RouteHold(f"invalid route journal: {exc}") from exc
        signature=data.get("journal_hmac_sha256") if isinstance(data,dict) else None
        if (not isinstance(signature,str) or not re.fullmatch(r"[0-9a-f]{64}",signature)
                or not hmac.compare_digest(signature,journal_signature(data,self._journal_key))):
            raise RouteHold("route journal authentication failed; do not resume RF")
        validate_journal(data,self.plan,self.digest)
        return data

    def save(self):
        self.data["journal_hmac_sha256"]=journal_signature(self.data,self._journal_key)
        durable_json(self.path,self.data)

    def point_folder(self,index):
        p=self.data["points"][index]
        return self.out/f"{index:04d}_{p['point_id']}"

    def point_files(self,index):
        folder=self.point_folder(index)
        return (folder/"operational_receipt.json",folder/"anchor_archive.bin",folder/"tag.rfb8")

    def verify_point(self,index):
        entry=self.data["points"][index]
        expected=entry if entry["state"] in ("VERIFIED","PURGE_INTENT","PURGED") or (
            any(key in entry for key in PROOF_FIELDS)) else None
        return verify_point_files(*self.point_files(index),entry["session"],entry["count"],
                                  self.plan["period_us"],expected=expected)

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
        proof=self.verify_point(index)
        status=self.anchor_status()
        if (entry["state"]=="PURGE_INTENT" and status.get("phase")=="IDLE" and
            status.get("file") is False and status.get("boot")==entry["anchor_boot"]):
            # A previously logged PURGE may have succeeded before the host
            # updated its journal; never remeasure to discover that.
            check_point_file_hashes(*self.point_files(index),proof)
            entry["state"]="PURGED";self.save();return
        if (status.get("phase")!="STORED" or status.get("file") is not True or
            status.get("file_session")!=entry["session"] or
            status.get("file_count")!=entry["count"] or
            status.get("boot")!=entry["anchor_boot"] or
            status.get("file_acked") is not True):
            raise RouteHold("remote archive is not the ACKed original point")
        if self.approval_key is None:
            raise RouteHold("separate custody approval key required; no purge")
        entry["state"]="PURGE_INTENT";self.save()
        with OperationalClient(self.anchor_ip,self.key,self.anchor_port,timeout=8,
                               approval_key=self.approval_key) as client:
            reply=client.purge_archive(
                entry["session"],entry["archive_crc"],entry["anchor_boot"],
                before_send=lambda: check_point_file_hashes(*self.point_files(index),proof))
            if reply!="PURGE_OK":raise RouteHold(f"anchor rejected purge: {reply}")
        final=self.anchor_status()
        if (final.get("phase")!="IDLE" or final.get("file") is not False or
                final.get("boot")!=entry["anchor_boot"]):
            raise RouteHold("remote purge not durably confirmed")
        check_point_file_hashes(*self.point_files(index),proof)
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
        if self.calibration_profile is not None:
            command.extend(("--calibration-profile", str(self.calibration_profile)))
        env=os.environ.copy()
        env["MODE8_AUTH_KEY_HEX"]=self.key.hex()
        env.pop("MODE8_APPROVAL_KEY_HEX",None)
        if self.approval_key is not None:
            env["MODE8_APPROVAL_KEY_HEX"]=self.approval_key.hex()
        result=subprocess.run(command,check=False,env=env)
        if result.returncode:
            raise RouteHold(f"point {entry['point_id']} capture incomplete (exit {result.returncode}); do not automatically repeat RF")
        proof=self.verify_point(index)
        entry.update(proof);entry["state"]="VERIFIED";self.save()

    def run(self):
        with route_lock(self.out/".route.lock"):
            self.data=self.load_journal()
            self.run_locked()

    def run_locked(self):
        for index,entry in enumerate(self.data["points"]):
            state=entry["state"]
            if state=="PURGED":
                self.verify_point(index)
                continue
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
    ap.add_argument("--calibration-profile", type=Path)
    args=ap.parse_args()
    key=bytes.fromhex(os.getenv(args.key_env,""))
    approval_key=bytes.fromhex(os.getenv(args.approval_key_env,""))
    if len(approval_key)!=32 or approval_key==key:
        ap.error("distinct MODE8_APPROVAL_KEY_HEX is required before starting any route")
    try:
        ctl=RouteController(unique_json(args.plan.read_bytes()),
                            args.out,args.anchor_ip,args.tag_port,key,
                            anchor_port=args.anchor_port,initial_baud=args.tag_initial_baud,
                            resume=args.resume,manual_confirm=args.manual_confirm,
                            approval_key=approval_key,calibration_profile=args.calibration_profile)
        ctl.run()
        print(f"Mode8 route complete: {ctl.path}")
        return 0
    except (ValueError,RouteHold,FileExistsError,OSError) as exc:
        print(f"HOLD: {exc}; inspect local journal before any RF rerun",file=sys.stderr)
        return 2

if __name__=="__main__":
    raise SystemExit(main())
