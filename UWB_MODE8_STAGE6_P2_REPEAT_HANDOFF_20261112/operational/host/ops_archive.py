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
TX = struct.Struct("<QQQBB")
RX = struct.Struct("<QQQiHHBBB96s")
PREFIX = struct.Struct("<6I4B")
ROW_BYTES = PREFIX.size + 5 * TX.size + 4 * RX.size + 4 * 96 + 4
MAGIC = 0x4F384D42
VERSION = 1
assert HEADER.size == 32 and ROW_BYTES == 1070

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
    if magic != MAGIC or version != VERSION or row_bytes != ROW_BYTES:
        raise ArchiveError("unknown archive version/layout")
    if count > 32 or session == 0 or boot == 0 or config == 0:
        raise ArchiveError("invalid archive identity or size")
    if zlib.crc32(blob[:28] + bytes(4)) != head_crc:
        raise ArchiveError("header CRC mismatch")
    if len(blob) != HEADER.size + ROW_BYTES * count:
        raise ArchiveError("archive length mismatch")
    if zlib.crc32(blob[HEADER.size:]) != body_crc:
        raise ArchiveError("body CRC mismatch")
    rows = []
    for index in range(count):
        row = memoryview(blob)[HEADER.size + ROW_BYTES * index: HEADER.size + ROW_BYTES * (index + 1)]
        if zlib.crc32(row[:-4]) != struct.unpack_from("<I", row, ROW_BYTES - 4)[0]:
            raise ArchiveError(f"row {index}: CRC mismatch")
        fields = PREFIX.unpack_from(row)
        rs, txn, rb, peer, rc, errors, complete, resp_mask, resp2_mask, flags = fields
        if (rs, txn, rb, rc) != (session, index, boot, config):
            raise ArchiveError(f"row {index}: transaction/session/boot/config mismatch")
        if flags & ~7 or resp_mask & ~3 or resp2_mask & ~3 or complete not in (0,1):
            raise ArchiveError(f"row {index}: invalid flags")
        offset = PREFIX.size
        transmit = []
        for k in range(5):
            planned, actual, status, typ, ok = TX.unpack_from(row, offset)
            if ok not in (0,1):
                raise ArchiveError(f"row {index}: TX flag")
            transmit.append({"planned": planned, "actual": actual, "status": status,
                            "type": typ, "ok": bool(ok)})
            offset += TX.size
        receive = []
        for k in range(4):
            hw, raw, status, ci, fp, nacc, typ, present, lde, air = RX.unpack_from(row,offset)
            if present not in (0,1) or lde not in (0,1):
                raise ArchiveError(f"row {index}: RX flag")
            if present:
                check_air(air)
            elif air != bytes(96):
                raise ArchiveError(f"row {index}: orphaned RX bytes")
            receive.append({"hw":hw,"raw":raw,"status":status,"ci":ci,"fp":fp,
                            "nacc":nacc,"type":typ,"present":bool(present),
                            "lde":bool(lde),"air_hex":air.hex() if present else None})
            offset += RX.size
        frames=[]
        for k in range(4):
            air=bytes(row[offset:offset+96]);offset+=96
            if check_air(air):
                if air[5] != (1, 4, 8, 5)[k]:
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
        if complete and (not all(frames) or peer == 0):
            raise ArchiveError(f"row {index}: complete archive missing authenticated RF evidence")
        if offset != ROW_BYTES-4:
            raise ArchiveError("parser layout mismatch")
        rows.append({"session":rs,"txn":txn,"boot":rb,"peer_boot":peer,
                     "config":rc,"errors":errors,"complete":bool(complete),
                     "resp_mask":resp_mask,"response2_mask":resp2_mask,
                     "p1_retry":bool(flags&1),"p2_retry":bool(flags&2),
                     "p2_repeat_used":bool(flags&4),
                     "tx":transmit,"response_rx":receive,"air_frames":frames})
    return {"archive_schema":"mode8.anchor.ops.v1","session":session,"boot":boot,
            "config":config,"count":count,"crc32":body_crc,"rows":rows}

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
