#!/usr/bin/env python3
"""Coordinated Mode 8 operation: tag USB Pi capture + offline anchor + TCP retrieval.

This script uses the proven Stage 6 Link/RFB8/fragment recovery implementation
from the pinned tag firmware package. It never replaces lost RF observations
with a later RF measurement without an explicit attempt identity.
"""
from __future__ import annotations
import argparse
import hashlib
import json
import os
from pathlib import Path
import secrets
import sys
import tempfile
import time

ROOT=Path(__file__).resolve().parents[2]
TAG_TOOLS=ROOT/"firmware/tag_p2_repeat/tools"
sys.path.insert(0,str(TAG_TOOLS))
from run_pair import (Link, cir_payload_integrity, fresh_hello, restore_default_baud,
                      send_while_drained, verify_shutdown_wire)
from capture_audit import OPERATIONAL_DRAIN_FIRMWARES, P1_FRESH_FIRMWARES, P2_FRESH_FIRMWARES, diagnostic_period_us, verify_capture
from calibration_profile import check_record_calibration, load_profile
from ops_archive import MAX_RECORDS, parse, to_anchor_record
from ops_client import OperationalClient, TransportError
from ops_usb_markers import SyncSampler
from rfb8 import Decoder, read_file

def tag_start_matches(event, identity, session):
    """Reject an active HELLO from another boot, session or RF config."""
    return bool(isinstance(event, dict) and event.get("event") == "HELLO"
                and event.get("role") == "tag" and event.get("active") is True
                and event.get("boot") == identity.get("boot")
                and event.get("session") == session
                and event.get("config_id") == identity.get("config_id")
                and event.get("firmware") == identity.get("firmware")
                and event.get("hw_ok") is True and event.get("fault_latched") is False
                and event.get("drain_protocol") == 1
                and event.get("stage") == 6 and event.get("period_us") == 200000
                and event.get("baud") == 2000000)


def prepare_tag(link, initial_baud, firmware=None):
    request = secrets.randbits(32) or 1
    link.send(f"STOP {request}")
    stopped = link.wait(lambda m: m.get("event") == "STOPPED" and m.get("role") == "tag"
                        and m.get("stop_request") == request, timeout=5, check_faults=False)
    if firmware is None:
        hello_request = secrets.randbits(32) or 1
        link.send(f"HELLO {hello_request}")
        hello = link.wait(lambda m: m.get("event") == "HELLO"
                          and m.get("hello_request") == hello_request
                          and m.get("role") == "tag"
                          and m.get("boot") == stopped["boot"], timeout=5, check_faults=False)
        firmware = hello.get("firmware")
        if firmware not in {pair[1] for pair in
                            (P1_FRESH_FIRMWARES, P2_FRESH_FIRMWARES, OPERATIONAL_DRAIN_FIRMWARES)}:
            raise RuntimeError("Tag firmware is outside the verified operational families")
    identity = {"boot": stopped["boot"], "firmware": firmware}
    session = stopped["session"]
    connected = fresh_hello(link, identity, session, baud=initial_baud)["hello"]
    if connected.get("silent_baud") != 1:
        raise RuntimeError("Tag lacks loss-free baud transition support; update its firmware")
    link.send("SELFTEST")
    test = link.wait(lambda m: m.get("event") == "SELFTEST" and m.get("role") == "tag")
    if test.get("pass") is not True:
        raise RuntimeError(f"Tag SELFTEST failed: {test}")
    if initial_baud == 921600:
        diagnostic = diagnostic_period_us(firmware)
        if connected.get("period_us", 0) < diagnostic:
            send_while_drained(link, f"SET PERIOD_US {diagnostic}", lambda m: (
                m.get("event") == "HELLO" and m.get("period_us") == diagnostic))
    send_while_drained(link, "SET STAGE 6", lambda m: m.get("event") == "HELLO" and m.get("stage") == 6)
    if initial_baud != 2000000:
        send_while_drained(link, "BAUD 2000000 SILENT")
        link.enable_replay()
        time.sleep(.2)
        with link.write_lock:
            link.serial.baudrate = 2000000
        fresh_hello(link, identity, session, baud=2000000)
    send_while_drained(link, "SET PERIOD_US 200000", lambda m: (
        m.get("event") == "HELLO" and m.get("period_us") == 200000))
    ident = fresh_hello(link, identity, session, baud=2000000, period_us=200000)["hello"]
    if ident.get("stage") != 6 or ident.get("record_replay") != 1:
        raise RuntimeError("Tag did not confirm Stage 6 record replay")
    return ident


def apply_tag_calibration(link, identity, profile):
    binding = profile["roles"]["tag"]
    if identity.get("hardware_profile") != binding["hardware_profile"]:
        raise ValueError("Tag calibration wiring profile mismatch")
    # prepare_tag finishes with a request-correlated HELLO, so prior unsolicited
    # acknowledgments are already queued and cannot acknowledge a new write.
    with link.condition:
        for event in list(link.events):
            if event.get("event") == "HELLO" and event.get("hello_request") == 0:
                link.events.remove(event)
    for entry in binding["radios"]:
        sigma = "none" if entry["sigma_m"] is None else entry["sigma_m"]
        hello = send_while_drained(
            link, "ANT {radio} {tx} {rx} {id} {sigma}".format(**entry, sigma=sigma),
            lambda m: (m.get("event") == "HELLO" and m.get("role") == "tag"
                       and m.get("hello_request") == 0 and m.get("boot") == identity["boot"]))
        if hello.get("hw_ok") is not True or hello.get("fault_latched") is not False:
            raise RuntimeError("Tag antenna delay write failed")
    link.send("SELFTEST")
    test = link.wait(lambda m: m.get("event") == "SELFTEST" and m.get("role") == "tag")
    if test.get("pass") is not True or any(
            test.get(f"ant_{radio}_{direction}") != entry[direction]
            for radio, entry in zip("ab", binding["radios"]) for direction in ("tx", "rx")):
        raise RuntimeError("Tag antenna delay register readback mismatch")
    proof = fresh_hello(link, identity, identity["session"], baud=2000000, period_us=200000)
    if proof["hello"].get("config_id") != identity["config_id"]:
        raise RuntimeError("Tag RF configuration changed during antenna delay setup")
    return test


def write_receipt(path, receipt):
    with tempfile.NamedTemporaryFile(mode="w", encoding="utf-8", dir=path.parent,
                                     prefix=path.name + ".", suffix=".partial", delete=False) as stream:
        temp = Path(stream.name)
        json.dump(receipt, stream, indent=2)
        stream.flush()
        os.fsync(stream.fileno())
    os.replace(temp, path)
    if os.name == "posix":
        directory = os.open(path.parent, os.O_RDONLY | os.O_DIRECTORY)
        try:
            os.fsync(directory)
        finally:
            os.close(directory)


def audit_saved_pair(directory, doc, anchor, tag, tag_stats, session, count, period_us):
    archive_path = directory / "anchor_archive.bin"
    tag_path = directory / "tag.rfb8"
    archive_bytes = archive_path.read_bytes()
    tag_bytes = tag_path.read_bytes()
    if parse(archive_bytes) != doc:
        raise RuntimeError("Persisted anchor archive differs from GET")

    def require_unchanged():
        if archive_path.read_bytes() != archive_bytes or tag_path.read_bytes() != tag_bytes:
            raise RuntimeError("Saved RF data changed after audit")

    audit = audit_archived_session(directory, doc, anchor, tag, tag_stats,
                                   session, count, period_us, tag_bytes=tag_bytes)
    require_unchanged()
    hashes = {archive_path.name: hashlib.sha256(archive_bytes).hexdigest(),
              tag_path.name: hashlib.sha256(tag_bytes).hexdigest()}
    return audit, hashes, require_unchanged


def wait_for_anchor(host,key,port,session,count,timeout):
    end=time.monotonic()+timeout
    while time.monotonic()<end:
        try:
            with OperationalClient(host,key,port,timeout=3) as ctl:
                status=json.loads(ctl.command("STATUS"))
                if (status.get("phase")=="STORED" and status.get("file") is True
                    and status.get("file_session")==session
                    and status.get("file_count")==count):
                    if status.get("boot")!=ctl.boot:
                        raise RuntimeError("Anchor STATUS boot differs from TCP challenge")
                    return status
                if status.get("session")==session and status.get("phase") in ("FAULT", "RECOVERY_REQUIRED"):
                    return status
        except (OSError,TimeoutError,ValueError,TransportError):
            pass
        time.sleep(1)
    raise TimeoutError("Anchor did not reconnect with a verified archive; retain partial evidence")

def stop_and_drain_tag(link, identity, session, directory):
    request=secrets.randbits(32) or 1
    def correlated(event, field):
        return lambda m: (m.get("event")==event and m.get("role")=="tag" and
                          m.get("boot")==identity["boot"] and m.get("session")==session and
                          m.get(field)==request)
    link.send(f"STOP {request}")
    stopped=link.wait(correlated("STOPPED", "stop_request"), timeout=5, check_faults=False)
    link.send(f"DRAIN {session} {identity['boot']} {request}")
    drained=link.wait(correlated("DRAINED", "request"), timeout=7, check_faults=False)
    if (drained.get("ok") is not True or drained.get("rf_stopped") is not True or
            drained.get("reason")!="OK" or any(
                type(drained.get(k)) is not int or drained[k]!=0
                for k in ("pending", "awaiting_ack", "failures"))):
        raise RuntimeError(f"Tag drain failed: {drained}")
    stats=link.seal_capture()
    proof=fresh_hello(link, identity, session, baud=2000000, period_us=200000)
    restored=restore_default_baud(link, 200000, identity=identity, session=session)
    link.close()
    stats.update({k: v for k, v in link.stats().items() if k in ("storage_handoff", "storage_io")})
    shutdown={"stopped":stopped, "drained":drained, "status_proof":proof, "baud_restore_proof":restored}
    if not verify_shutdown_wire(directory, {"tag":shutdown}):
        raise RuntimeError("Tag shutdown proof missing or out of order in persisted RFB8")
    return stats, shutdown


def validate_tag_completion(stats, before, after, count):
    if (stats["records"] != count or stats["complete"] != count
            or any(stats.get(k) for k in ("reader_error", "writer_error", "fault_events", "capture_overflow"))
            or type(stats["transport"].get("durable_acks_sent")) is not int
            or stats["transport"]["durable_acks_sent"] != count
            or type(after.get("period_miss")) is not int or after["period_miss"] != 0
            or type(before.get("drops")) is not int or after.get("drops") != before["drops"]
            or type(before.get("sent")) is not int or type(after.get("sent")) is not int
            or (after["sent"] - before["sent"]) % (1 << 32) != count):
        raise RuntimeError("Tag capture, period or durable record ACK incomplete; preserve raw data")


def audit_archived_session(directory, doc, anchor, tag, tag_stats, session, count, period_us,
                           *, tag_bytes: bytes | None = None):
    """Recheck the durable RFB8 and exact archived RF evidence before ACKFILE."""
    from analyze import packet_arrays
    import numpy as np

    if (doc["session"], doc["boot"], doc["config"], doc["count"])!=(
            session, anchor["boot"], anchor["config_id"], count):
        raise RuntimeError("Anchor archive identity or record count mismatch")
    decoder=Decoder()
    stream = read_file(directory / "tag.rfb8", decoder) if tag_bytes is None else decoder.feed(tag_bytes)
    packets = [packet for packet in stream if packet.kind == 2]
    tag_rows=[{**p.meta, "_raw_integrity_verified":cir_payload_integrity(p.meta,p.raw)} for p in packets]
    if any(decoder.statistics().get(field) for field in ("crc_errors", "schema_errors", "sequence_gap_events", "trailing_bytes", "skipped_bytes")):
        raise RuntimeError("Persisted tag RFB8 failed stream validation")
    anchors=[to_anchor_record(row, anchor["firmware"], period_us) for row in doc["rows"]]
    audit=verify_capture(session=session, count=count, stage=6, period_us=period_us,
                         identities={"anchor":anchor, "tag":tag},
                         records={"anchor":anchors, "tag":tag_rows},
                         stats={"tag":tag_stats})
    if not audit.passed:
        raise RuntimeError(f"Paired RF audit failed: {audit.problems}")
    intervals=[((b["initial_poll_tx"]-a["initial_poll_tx"]) % (1<<40))*1e6/63897600000
               for a,b in zip(anchors, anchors[1:])]
    if any(abs(value-period_us)>4000 for value in intervals):
        raise RuntimeError(f"Measured POLL cadence outside existing Stage 6 tolerance: {intervals}")
    for anchor_row, packet in zip(anchors, packets):
        arrays,_=packet_arrays(packet.meta, packet.raw, anchor_row)
        for probe in (1,2):
            axis=arrays[f"common_p{probe}_delay_s"]
            power=arrays[f"common_p{probe}_power_norm"]
            # The shared grid spans both windows. Missing power outside a
            # radio's own coverage is expected; require real A/B overlap.
            if (not axis.size or power.shape!=(2,axis.size) or not np.isfinite(axis).all() or
                    not np.isfinite(power).all(axis=0).any()):
                raise RuntimeError(f"Missing P{probe} common axis or power grid for txn {anchor_row['txn']}")
    return {"paired_records":count, "p1_common_grid":count, "p2_common_grid":count,
            "interval_count":len(intervals), "min_period_us":min(intervals,default=None),
            "max_period_us":max(intervals,default=None), "absolute_accuracy_validated":False}


def main()->int:
    ap=argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--anchor-ip",required=True)
    ap.add_argument("--anchor-port",type=int,default=29310)
    ap.add_argument("--tag-port",required=True)
    ap.add_argument("--tag-initial-baud",type=int,choices=(921600,2000000),default=921600)
    ap.add_argument("--session",type=int,required=True)
    ap.add_argument("--count",type=int,default=20)
    ap.add_argument("--period-us",type=int,default=200000)
    ap.add_argument("--baud",type=int,default=2000000,choices=(921600,2000000))
    ap.add_argument("--out",type=Path,required=True)
    ap.add_argument("--key-env",default="MODE8_AUTH_KEY_HEX")
    ap.add_argument("--approval-key-env",default="MODE8_APPROVAL_KEY_HEX")
    ap.add_argument("--anchor-reconnect-timeout",type=int,default=90)
    ap.add_argument("--calibration-profile", type=Path)
    ap.add_argument("--usb-clock-markers", action="store_true",
                    help="record tag A/B USB clock brackets on the host collecting ROS2")
    args=ap.parse_args()
    if not 1<=args.count<=MAX_RECORDS or not 1<=args.session<=0xffffffff:
        ap.error("session or count out of bounds; operational archive capacity is 32 transactions")
    if args.baud!=2000000 or args.period_us!=200000:
        ap.error("This initial operational controller pins Stage 6 to 200 ms and 2 Mbps")
    raw=os.environ.get(args.key_env,"")
    try:key=bytes.fromhex(raw)
    except ValueError:ap.error("Invalid authentication key hex")
    if len(key)!=32:ap.error(f"Set {args.key_env} to the provisioned 32-byte key")
    try:approval_key=bytes.fromhex(os.environ.get(args.approval_key_env,""))
    except ValueError:ap.error("invalid custody key hex")
    if len(approval_key)!=32 or approval_key==key:
        ap.error("distinct MODE8_APPROVAL_KEY_HEX required before RF START")
    args.out.mkdir(parents=True,exist_ok=False)
    receipt={"requested_session":args.session,"requested_count":args.count,
             "tag_port":args.tag_port,"anchor":args.anchor_ip,"status":"INCOMPLETE"}
    link=None
    sampler=None
    tag_start_sent=False
    shutdown_attempted=False
    try:
        profile = load_profile(args.calibration_profile) if args.calibration_profile else None
        if args.usb_clock_markers:
            sampler=SyncSampler(args.out)
        link=Link("tag",args.tag_port,args.out,initial_baud=args.tag_initial_baud,
                  separate_tag_boot_preamble=True, durable_group_commit=True,
                  **({"clock_sampler": sampler} if sampler else {}))
        time.sleep(1.2)
        ident = prepare_tag(link, args.tag_initial_baud)
        receipt["tag_identity"]=ident
        if profile:
            receipt["calibration_profile"] = profile
            receipt["tag_calibration_selftest"] = apply_tag_calibration(link, ident, profile)
        with OperationalClient(args.anchor_ip,key,args.anchor_port) as ctl:
            status=json.loads(ctl.command("STATUS"))
            if status.get("phase")!="IDLE" or status.get("file") is not False:
                raise RuntimeError(f"Anchor not ready: {status}")
            if (status.get("config")!=ident.get("config_id") or status.get("boot")!=ctl.boot or
                    (status.get("firmware"), ident.get("firmware")) not in (P1_FRESH_FIRMWARES, P2_FRESH_FIRMWARES, OPERATIONAL_DRAIN_FIRMWARES) or
                    status.get("hw_ok") is not True or status.get("fault_latched") is not False or
                    status.get("stage")!=6 or status.get("period_us")!=args.period_us or
                    status.get("baud")!=args.baud or status.get("drained") is not True):
                raise RuntimeError("Anchor and tag configuration or runtime identity mismatch; do not start")
            if status.get("start_ack_protocol") != 1:
                raise RuntimeError("Anchor does not advertise confirmed START; do not start")
            anchor={**status, "config_id":status["config"], "role":"anchor"}
            receipt["anchor_identity"]=anchor
        if sampler:
            sampler.control(f"\0SYNC before {ident['boot']} {ident.get('session', 0)} {args.session}")
            while sampler.job is not None:
                sampler.pump(link)
                time.sleep(.01)
        link.begin_capture(ident["boot"],args.session)
        tag_start_sent=True
        link.send(f"START {args.session} {args.count}")
        link.wait(lambda m: tag_start_matches(m, ident, args.session))
        with OperationalClient(args.anchor_ip,key,args.anchor_port) as ctl:
            if ctl.boot!=anchor["boot"]:
                raise RuntimeError("Anchor restarted before START")
            try:
                response, nonce = ctl.request_start(args.session, args.count)
            except (OSError,TransportError) as error:
                receipt["anchor_start_reply_lost"]=True
                raise RuntimeError(f"START offer unavailable: {error}") from error
            receipt["anchor_start_reply"] = response
            try:
                ctl.acknowledge_start(args.session, nonce)
            except OSError as error:
                # Delivery may already have succeeded. Continue collecting,
                # but only the same boot/session may prove the ACK was accepted.
                receipt["anchor_start_ack_send_error"] = str(error)
            else:
                receipt["anchor_start_ack_sent"] = True
        if sampler:
            sampler.control(f"\0SYNC during {ident['boot']} {args.session} {args.session}")
        deadline=time.monotonic()+args.count*args.period_us/1e6+20
        while link.records<args.count:
            if link.exception:raise RuntimeError("Tag serial reader: "+link.exception)
            if link.fault_events:raise RuntimeError("Tag capture reported a fault; see tag.jsonl for the event")
            if time.monotonic()>=deadline:raise TimeoutError("Tag capture deadline")
            if sampler:
                sampler.pump(link)
            time.sleep(.04)
        if sampler:
            sampler.control(f"\0SYNC after {ident['boot']} {args.session} {args.session}")
            while sampler.job is not None:
                sampler.pump(link)
                time.sleep(.01)
        finished_link=link
        shutdown_attempted=True
        try:
            tag_stats,receipt["tag_shutdown"]=stop_and_drain_tag(
                finished_link, ident, args.session, args.out)
        finally:
            if finished_link.files_finalized:
                link=None
        receipt["tag_capture"]=tag_stats
        validate_tag_completion(tag_stats, ident, receipt["tag_shutdown"]["status_proof"]["hello"],
                                args.count)
        if profile:
            check_record_calibration(profile, "tag", finished_link.capture_records)
        stored=wait_for_anchor(args.anchor_ip,key,args.anchor_port,args.session,args.count,
                               args.anchor_reconnect_timeout)
        receipt["anchor_shutdown"]=stored
        if (stored.get("phase")!="STORED" or stored.get("boot")!=anchor["boot"] or
                stored.get("session")!=args.session or stored.get("drained") is not True or
                stored.get("hw_ok") is not True or stored.get("fault_latched") is not False or
                stored.get("period_miss")!=0 or stored.get("failure")):
            raise RuntimeError(f"Anchor did not prove clean archive finalization: {stored}")
        if stored.get("start_ack_session") != args.session:
            raise RuntimeError("Anchor did not prove receipt of this session's START_ACK")
        receipt["anchor_start_ack_confirmed"] = True
        with OperationalClient(args.anchor_ip,key,args.anchor_port,approval_key=approval_key) as ctl:
            if ctl.boot!=anchor["boot"]:
                raise RuntimeError("Anchor restarted before GET")
            doc=ctl.get_archive(args.out/"anchor_archive.bin",args.session,ack=False)
            audit, hashes, require_unchanged = audit_saved_pair(
                args.out, doc, anchor, ident, tag_stats, args.session, args.count, args.period_us)
            receipt["paired_audit"] = audit
            receipt["raw_sha256"] = hashes
            receipt["archive"] = {k: v for k, v in doc.items() if k != "rows"}
            write_receipt(args.out / "operational_receipt.json", receipt)
            require_unchanged()
            answer=ctl.acknowledge_archive(args.session, doc["crc32"])
            if answer!="ACKFILE_OK":raise RuntimeError("Anchor archive safely saved but ACK failed")
            receipt["file_ack"] = answer
        receipt["status"]="SAVED_BOTH_ENDPOINTS"
        receipt["note"]="Archive remains on anchor until separate explicit PURGE."
        return 0
    except Exception as e:
        receipt["error"]=repr(e)
        return 2
    finally:
        if link and tag_start_sent and not shutdown_attempted:
            try:
                receipt["tag_capture"], receipt["tag_shutdown"] = stop_and_drain_tag(
                    link, ident, args.session, args.out)
                link = None
            except Exception as e:
                receipt["shutdown_error"] = repr(e)
        if link:
            if not link.files_finalized:
                try:link.send("STOP")
                except Exception:pass
                try:link.close()
                except Exception as e:receipt["close_error"]=repr(e)
            if "tag_capture" not in receipt:
                receipt["tag_capture_diagnostics"] = link.stats()
        if sampler:
            sampler.close()
        write_receipt(args.out / "operational_receipt.json", receipt)

if __name__=="__main__":
    raise SystemExit(main())
