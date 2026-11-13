#!/usr/bin/env python3
"""Strict RFB8 streaming decoder. Python 3.10+, standard library only."""
from __future__ import annotations
from dataclasses import dataclass
import json
import struct
import zlib
from pathlib import Path
from typing import Iterator

MAGIC = b"RFB8"
HEADER = struct.Struct("<4sBBHIIII")
CIR_BYTES = 3968
RAW_BYTES = 4 * CIR_BYTES
JSON_MAX = 8500

@dataclass
class Packet:
    kind: int
    sequence: int
    meta: dict
    raw: bytes

class Decoder:
    def __init__(self) -> None:
        self.buffer = bytearray()
        self.skipped_bytes = 0
        self.crc_errors = 0
        self.schema_errors = 0
        self.sequence_gaps = 0
        self.last_sequence: int | None = None

    def feed(self, data: bytes) -> list[Packet]:
        self.buffer.extend(data)
        packets: list[Packet] = []
        while True:
            pos = self.buffer.find(MAGIC)
            if pos < 0:
                discard = max(0, len(self.buffer) - 3)
                self.skipped_bytes += discard
                del self.buffer[:discard]
                break
            if pos:
                self.skipped_bytes += pos
                del self.buffer[:pos]
            if len(self.buffer) < HEADER.size:
                break
            _, version, kind, hbytes, seq, nj, nr, expected_crc = HEADER.unpack_from(self.buffer)
            if (version != 1 or kind not in (1, 2) or hbytes != HEADER.size
                    or not 2 <= nj <= JSON_MAX or nr not in (0, RAW_BYTES)
                    or (kind == 1 and nr != 0)):
                self.schema_errors += 1
                del self.buffer[:1]
                continue
            total = HEADER.size + nj + nr
            if len(self.buffer) < total:
                break
            payload = bytes(self.buffer[HEADER.size:total])
            if (zlib.crc32(payload) & 0xffffffff) != expected_crc:
                self.crc_errors += 1
                del self.buffer[:1]
                continue
            try:
                meta = json.loads(payload[:nj])
                if not isinstance(meta, dict):
                    raise ValueError("JSON object required")
                if kind == 2 and (meta.get("schema") != "dual8.record.v1" or meta.get("mode") != 8):
                    raise ValueError("Unexpected record schema")
                if kind == 2 and nr != (RAW_BYTES if meta.get("role") == "tag" else 0):
                    raise ValueError("role/raw size mismatch")
                if kind == 2 and meta.get("role") not in ("tag", "anchor"):
                    raise ValueError("Invalid role")
            except (ValueError, UnicodeError):
                self.schema_errors += 1
                del self.buffer[:total]
                continue
            if self.last_sequence is not None and seq != (self.last_sequence + 1) & 0xffffffff:
                self.sequence_gaps += 1  # event count, not a guessed number of lost frames
            self.last_sequence = seq
            packets.append(Packet(kind, seq, meta, payload[nj:]))
            del self.buffer[:total]
        return packets

    def statistics(self) -> dict:
        return {"skipped_bytes": self.skipped_bytes, "crc_errors": self.crc_errors,
                "schema_errors": self.schema_errors, "sequence_gap_events": self.sequence_gaps,
                "trailing_bytes": len(self.buffer)}

def encode(meta: dict, raw: bytes = b"", *, kind: int = 2, sequence: int = 0) -> bytes:
    j = json.dumps(meta, separators=(",", ":"), allow_nan=False).encode()
    if len(j) > JSON_MAX or len(raw) not in (0, RAW_BYTES):
        raise ValueError("RFB8 payload bounds")
    payload = j + raw
    return HEADER.pack(MAGIC, 1, kind, HEADER.size, sequence, len(j), len(raw),
                       zlib.crc32(payload) & 0xffffffff) + payload

def read_file(path: Path, decoder: Decoder | None = None) -> Iterator[Packet]:
    d = decoder or Decoder()
    with path.open("rb") as f:
        while chunk := f.read(65536):
            yield from d.feed(chunk)
