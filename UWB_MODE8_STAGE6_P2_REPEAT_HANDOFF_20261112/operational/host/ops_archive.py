#!/usr/bin/env python3
"""Fail-closed reader for Mode 8 anchor offline compact archives (M8O1).

This is NOT an RFB8 decoder; it preserves anchor TX/RX evidence for each
transaction and does not substitute for tag raw-CIR RFB8 on the Pi.
"""
from __future__ import annotations
import argparse
import json
from pathlib import Path
import struct
import zlib

HEADER = struct.Struct("<IHHIIIIII")
TX = struct.Struct("<QQQBBBB")
RX = struct.Struct("<QQQiHHBBBB96s")
PREFIX = struct.Struct("<6I4BQ")
LEGACY_TX = struct.Struct("<QQQBB")
LEGACY_RX = struct.Struct("<QQQiHHBBB96s")
LEGACY_PREFIX = struct.Struct("<6I4B")
ROW_BYTES = PREFIX.size + 6 * TX.size + 4 * RX.size + 6 * 96 + 4
LEGACY_ROW_BYTES = 1070
MAX_RECORDS = 32
MAX_FILE_BYTES = HEADER.size + MAX_RECORDS * ROW_BYTES
MAGIC = 0x4F384D42
VERSION = 3
MASK40 = (1 << 40) - 1
assert HEADER.size == 32 and ROW_BYTES == 1312

class ArchiveError(ValueError):
    pass

def check_air(payload: bytes) -> bool:
    if payload == bytes(96):
        return False
    if len(payload) != 96 or payload[:4] != b"FCB8" or payload[4] != 1:
        raise ArchiveError("bad on-air frame header")
    if struct.unpack_from("<H", payload, 6)[0] != 96:
        raise ArchiveError("bad on-air frame length")
    if zlib.crc32(payload[:92]) != struct.unpack_from("<I", payload, 92)[0]:
        raise ArchiveError("on-air CRC mismatch")
    if any(payload[75:92]):
        raise ArchiveError("nonzero reserved air bytes")
    return True

def parse(blob: bytes) -> dict:
    if len(blob) < HEADER.size:
        raise ArchiveError("truncated header")
    magic, version, row_bytes, session, boot, config, count, body_crc, head_crc = HEADER.unpack_from(blob)
    if magic != MAGIC or (version, row_bytes) not in ((1, LEGACY_ROW_BYTES), (2, ROW_BYTES), (VERSION, ROW_BYTES)):
        raise ArchiveError("unknown archive version/layout")
    legacy = version == 1
    fresh_p2 = version == VERSION
    prefix_struct, tx_struct, rx_struct, tx_count, frame_count = (
        (LEGACY_PREFIX, LEGACY_TX, LEGACY_RX, 5, 4) if legacy else (PREFIX, TX, RX, 6, 6))
    if not 1 <= count <= MAX_RECORDS or session == 0 or boot == 0 or config == 0:
        raise ArchiveError("invalid archive identity or size")
    if zlib.crc32(blob[:28] + bytes(4)) != head_crc:
        raise ArchiveError("header CRC mismatch")
    if len(blob) != HEADER.size + row_bytes * count:
        raise ArchiveError("archive length mismatch")
    if zlib.crc32(blob[HEADER.size:]) != body_crc:
        raise ArchiveError("body CRC mismatch")
    rows = []
    for index in range(count):
        row = memoryview(blob)[HEADER.size + row_bytes * index: HEADER.size + row_bytes * (index + 1)]
        if zlib.crc32(row[:-4]) != struct.unpack_from("<I", row, row_bytes - 4)[0]:
            raise ArchiveError(f"row {index}: CRC mismatch")
        fields = prefix_struct.unpack_from(row)
        rs, txn, rb, peer, rc, errors, complete, resp_mask, resp2_mask, flags = fields[:10]
        initial_poll_tx = None if legacy else fields[10]
        if not legacy and initial_poll_tx > MASK40:
            raise ArchiveError(f"row {index}: initial POLL timestamp")
        if (rs, txn, rb, rc) != (session, index, boot, config):
            raise ArchiveError(f"row {index}: transaction/session/boot/config mismatch")
        if flags & ~(7 if legacy else 3) or resp_mask & ~3 or resp2_mask & ~3 or complete not in (0,1):
            raise ArchiveError(f"row {index}: invalid flags")
        offset = prefix_struct.size
        transmit = []
        for k in range(tx_count):
            planned, actual, status, typ, ok, *extra = tx_struct.unpack_from(row, offset)
            radio, attempted = extra if extra else (0 if ok else 255, ok)
            if (ok not in (0, 1) or attempted not in (0, 1) or ok and not attempted or
                    not legacy and (planned > MASK40 or actual > MASK40 or
                                    attempted and (radio != 0 or typ != (1, 4, 8, 5, 5, 4)[k]))):
                raise ArchiveError(f"row {index}: TX flag, slot, or timestamp")
            transmit.append({"planned": planned, "actual": actual, "status": status,
                             "type": typ, "ok": bool(ok), "radio": radio, "attempted": bool(attempted)})
            offset += tx_struct.size
        receive = []
        for k in range(4):
            hw, raw, status, ci, fp, nacc, typ, present, lde, *extra = rx_struct.unpack_from(row, offset)
            radio, air = extra if not legacy else (0 if present else 255, extra[0])
            if present not in (0, 1) or lde not in (0, 1):
                raise ArchiveError(f"row {index}: RX flag")
            if present:
                if not check_air(air):
                    raise ArchiveError(f"row {index}: RX bytes missing")
                if not legacy and (radio != 0 or typ != (2, 3, 6, 7)[k] or hw > MASK40 or raw > MASK40):
                    raise ArchiveError(f"row {index}: RX slot or timestamp")
            elif air != bytes(96):
                raise ArchiveError(f"row {index}: orphaned RX bytes")
            receive.append({"hw": hw, "raw": raw, "status": status, "ci": ci, "fp": fp,
                            "nacc": nacc, "type": typ, "ok": bool(present), "radio": radio,
                            "lde": bool(lde), "air": air.hex() if present else None,
                            "present": bool(present), "air_hex": air.hex() if present else None})
            offset += rx_struct.size
        frames = []
        for k in range(frame_count):
            air=bytes(row[offset:offset+96]);offset+=96
            if check_air(air):
                if air[5] != (1, 4, 8, 5, 5, 4)[k]:
                    raise ArchiveError(f"row {index}: wrong RF message type in frame {k}")
                origin=(int.from_bytes(air[8:12], "little"),
                        int.from_bytes(air[12:16], "little"),
                        int.from_bytes(air[16:20], "little"),
                        int.from_bytes(air[20:24], "little"))
                if origin != (session,index,config,boot):
                    raise ArchiveError(f"row {index}: RF session/txn/config/boot mismatch")
                frames.append(air.hex())
            else:
                frames.append(None)
        required_frames = 4 if legacy or fresh_p2 else 5
        if complete and (not all(frames[:required_frames]) or peer == 0):
            raise ArchiveError(f"row {index}: complete archive missing authenticated RF evidence")
        if not legacy and any(bool(frame) != tx["ok"] for frame, tx in zip(frames, transmit)):
            raise ArchiveError(f"row {index}: frame/TX availability mismatch")
        if offset != row_bytes-4:
            raise ArchiveError("parser layout mismatch")
        rows.append({"session":rs,"txn":txn,"boot":rb,"peer_boot":peer,
                     "config":rc,"errors":errors,"complete":bool(complete),
                     "resp_mask":resp_mask,"response2_mask":resp2_mask,
                     "p1_retry":bool(flags&1),"p2_retry":bool(flags&2),
                     "p2_repeat_used":bool(flags&4), "initial_poll_tx":initial_poll_tx,
                     "archive_version":version,
                     "tx":transmit,"response_rx":receive,"air_frames":frames})
    return {"archive_schema":f"mode8.anchor.ops.v{version}","session":session,"boot":boot,
            "config":config,"count":count,"crc32":body_crc,"rows":rows}

def to_anchor_record(row: dict, firmware: str, period_us: int) -> dict:
    """Project a v2 or v3 archive row onto the paired RF audit schema."""
    tx, rx, frames = row["tx"], row["response_rx"], row["air_frames"]
    if len(tx) != 6 or len(frames) != 6:
        raise ArchiveError("paired RF audit requires an operational v2 or v3 archive")
    p2_extra = "p2_primary" if row.get("archive_version") == VERSION else "p2_repeat"
    return {
        "schema": "dual8.record.v1", "firmware": firmware, "mode": 8, "role": "anchor",
        "stage": 6, "period_us": period_us,
        **{key: row[key] for key in ("session", "txn", "boot", "peer_boot", "config",
                                    "complete", "errors", "initial_poll_tx", "p1_retry", "p2_retry")},
        "resp_mask": row["resp_mask"], "response2_mask": row["response2_mask"],
        "tx": [tx[0], tx[1], tx[3]], "frames": [frames[0], frames[1], frames[3]],
        "response_rx": rx[:2], "response2_rx": rx[2:],
        "poll2_tx": tx[2], "poll2_frame": frames[2],
        f"{p2_extra}_tx": tx[4], f"{p2_extra}_frame": frames[4],
        "p1_repeat_tx": tx[5], "p1_repeat_frame": frames[5],
    }


def main()->int:
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument("archive",type=Path)
    parser.add_argument("--json",type=Path)
    args=parser.parse_args()
    result=parse(args.archive.read_bytes())
    summary={k:v for k,v in result.items() if k!="rows"}
    summary["complete"]=sum(row["complete"] for row in result["rows"])
    print(json.dumps(summary,indent=2))
    if args.json:
        args.json.write_text(json.dumps(result,indent=2),encoding="utf-8")
    return 0

if __name__=="__main__":
    raise SystemExit(main())
