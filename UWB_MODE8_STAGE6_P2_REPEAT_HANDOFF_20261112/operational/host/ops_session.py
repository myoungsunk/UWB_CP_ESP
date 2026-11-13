#!/usr/bin/env python3
"""Coordinated Mode 8 operation: tag USB Pi capture + offline anchor + TCP retrieval.

This script uses the proven Stage 6 Link/RFB8/fragment recovery implementation
from the pinned tag firmware package. It never replaces lost RF observations
with a later RF measurement without an explicit attempt identity.
"""
from __future__ import annotations
import argparse
import json
import os
from pathlib import Path
import sys
import time

ROOT=Path(__file__).resolve().parents[2]
TAG_TOOLS=ROOT/"firmware/tag_p2_repeat/tools"
sys.path.insert(0,str(TAG_TOOLS))
from run_pair import Link, restore_default_baud
from ops_client import OperationalClient

def wait_for_anchor(host,key,port,session,count,timeout):
    end=time.monotonic()+timeout
    while time.monotonic()<end:
        try:
            with OperationalClient(host,key,port,timeout=3) as ctl:
                status=json.loads(ctl.command("STATUS"))
                if (status.get("phase")=="STORED" and status.get("file") is True
                    and status.get("file_session")==session
                    and status.get("file_count")==count):
                    return
        except (OSError,TimeoutError,ValueError,RuntimeError):
            pass
        time.sleep(1)
    raise TimeoutError("Anchor did not reconnect with a verified archive; retain partial evidence")

def main()->int:
    ap=argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--anchor-ip",required=True)
    ap.add_argument("--anchor-port",type=int,default=29310)
    ap.add_argument("--tag-port",required=True)
    ap.add_argument("--session",type=int,required=True)
    ap.add_argument("--count",type=int,default=20)
    ap.add_argument("--period-us",type=int,default=200000)
    ap.add_argument("--baud",type=int,default=2000000,choices=(921600,2000000))
    ap.add_argument("--out",type=Path,required=True)
    ap.add_argument("--key-env",default="MODE8_AUTH_KEY_HEX")
    ap.add_argument("--anchor-reconnect-timeout",type=int,default=90)
    args=ap.parse_args()
    if not 1<=args.count<=32 or not 1<=args.session<=0xffffffff:
        ap.error("session or count out of bounds; operational archive capacity is 32 transactions")
    if args.baud!=2000000 or args.period_us!=200000:
        ap.error("This initial operational controller pins Stage 6 to 200 ms and 2 Mbps")
    raw=os.environ.get(args.key_env,"")
    try:key=bytes.fromhex(raw)
    except ValueError:ap.error("Invalid authentication key hex")
    if len(key)!=32:ap.error(f"Set {args.key_env} to the provisioned 32-byte key")
    args.out.mkdir(parents=True,exist_ok=False)
    receipt={"requested_session":args.session,"requested_count":args.count,
             "tag_port":args.tag_port,"anchor":args.anchor_ip,"status":"INCOMPLETE"}
    link=None
    try:
        link=Link("tag",args.tag_port,args.out)
        time.sleep(.3)
        link.send("STOP")
        link.wait(lambda m:m.get("event")=="STOPPED",timeout=5,check_faults=False)
        link.send("HELLO")
        ident=link.wait(lambda m:m.get("event")=="HELLO" and m.get("role")=="tag")
        if not ident.get("hw_ok") or ident.get("fault_latched"):
            raise RuntimeError("Tag hardware not healthy")
        link.send("SELFTEST")
        test=link.wait(lambda m:m.get("event")=="SELFTEST")
        if not test.get("pass"):raise RuntimeError("Tag SELFTEST failed")
        link.send("SET STAGE 6")
        link.wait(lambda m:m.get("event")=="HELLO" and m.get("stage")==6)
        if args.baud==2000000:
            link.enable_replay()
            link.send("BAUD 2000000")
            time.sleep(.2);link.serial.baudrate=2000000
            link.send("HELLO")
            link.wait(lambda m:m.get("event")=="HELLO" and m.get("baud")==2000000)
        link.send("SET PERIOD_US 200000")
        ident=link.wait(lambda m:m.get("event")=="HELLO" and m.get("period_us")==200000)
        receipt["tag_identity"]=ident
        with OperationalClient(args.anchor_ip,key,args.anchor_port) as ctl:
            status=json.loads(ctl.command("STATUS"))
            if status.get("phase")!="IDLE" or status.get("file") is not False:
                raise RuntimeError(f"Anchor not ready: {status}")
            if status.get("config")!=ident.get("config_id"):
                raise RuntimeError("Anchor and tag Config ID mismatch; do not start")
        link.begin_capture(ident["boot"],args.session)
        link.send(f"START {args.session} {args.count}")
        link.wait(lambda m:m.get("event")=="HELLO" and m.get("active") is True)
        with OperationalClient(args.anchor_ip,key,args.anchor_port) as ctl:
            response=ctl.command(f"START {args.session} {args.count}")
            if response!="START_ACCEPTED_WIFI_OFF":
                raise RuntimeError(f"Anchor rejected offline start: {response}")
        deadline=time.monotonic()+args.count*args.period_us/1e6+20
        while link.records<args.count:
            if link.exception:raise RuntimeError("Tag serial reader: "+link.exception)
            if time.monotonic()>=deadline:raise TimeoutError("Tag capture deadline")
            time.sleep(.04)
        link.send("STOP")
        link.wait(lambda m:m.get("event")=="STOPPED" and m.get("session")==args.session,timeout=5,check_faults=False)
        tag_stats=link.stats()
        receipt["tag_capture"]=tag_stats
        if tag_stats["records"]!=args.count or tag_stats["complete"]!=args.count or tag_stats["reader_error"]:
            raise RuntimeError("Tag capture incomplete; preserve partial raw data")
        # Durable close of tag files occurs in Link.close; do not ACK anchor
        # until the local tag capture is finalized and verified.
        restore_default_baud(link,args.period_us)
        link.close();link=None
        wait_for_anchor(args.anchor_ip,key,args.anchor_port,args.session,args.count,
                        args.anchor_reconnect_timeout)
        with OperationalClient(args.anchor_ip,key,args.anchor_port) as ctl:
            doc=ctl.get_archive(args.out/"anchor_archive.bin",args.session,ack=False)
            if doc["count"]!=args.count or any(not row["complete"] for row in doc["rows"]):
                raise RuntimeError("Anchor archival records incomplete; do not ACK/PURGE")
            if any((row["config"],row["session"])!=(ident["config_id"],args.session) for row in doc["rows"]):
                raise RuntimeError("Anchor/tag configuration or session mismatch")
            if not all(row["peer_boot"]==ident["boot"] for row in doc["rows"]):
                raise RuntimeError("Anchor/tag peer boot mismatch")
            answer=ctl.command(f"ACKFILE {args.session}")
            if answer!="ACKFILE_OK":raise RuntimeError("Anchor archive safely saved but ACK failed")
        receipt["archive"]={k:v for k,v in doc.items() if k!="rows"}
        receipt["status"]="SAVED_BOTH_ENDPOINTS"
        receipt["note"]="Archive remains on anchor until separate explicit PURGE."
        return 0
    except Exception as e:
        receipt["error"]=repr(e)
        return 2
    finally:
        if link:
            try:link.send("STOP")
            except Exception:pass
            try:link.close()
            except Exception as e:receipt["close_error"]=repr(e)
        (args.out/"operational_receipt.json").write_text(json.dumps(receipt,indent=2),encoding="utf-8")

if __name__=="__main__":
    raise SystemExit(main())
