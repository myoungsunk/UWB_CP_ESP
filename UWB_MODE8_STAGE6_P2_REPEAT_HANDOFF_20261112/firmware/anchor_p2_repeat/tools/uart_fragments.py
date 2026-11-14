"""Recover Stage 6 RFB8 records from CRC-checked USB fragments."""

from dataclasses import dataclass, field
import struct
import zlib

from collections import OrderedDict
import hashlib

from rfb8 import HEADER as RECORD_HEADER, MAGIC as RECORD_MAGIC, JSON_MAX, RAW_BYTES


MAGIC = b"R8F6"
HEADER = struct.Struct("<4sIBBHI")
CHUNK = 256
MAX_DATA = (RECORD_HEADER.size + JSON_MAX + RAW_BYTES + CHUNK - 1) // CHUNK
END = 255
PACKED_IQ = 1 << 15


def parity_count(data_count):
    return min(20, data_count + 4)


def field_tables():
    exp, log = [], [0] * 256
    value = 1
    for i in range(255):
        exp.append(value)
        log[value] = i
        value <<= 1
        if value & 256:
            value ^= 0x11D
    exp += exp + exp[:2]
    multiplication = [bytes(256)]
    multiplication.extend(bytes(0 if b == 0 else exp[log[a] + log[b]] for b in range(256))
                          for a in range(1, 256))
    return multiplication, bytes([0] + [exp[255 - log[a]] for a in range(1, 256)])


MULTIPLY, INVERSE = field_tables()


def scale(chunk, coefficient):
    return int.from_bytes(chunk.translate(MULTIPLY[coefficient]), "little")


def coefficient(parity_index, data_index):
    return INVERSE[parity_index ^ data_index]


def recover(parts, data_count):
    """Return data chunks and erased indices, or raise when too few rows survive."""
    missing = [i for i in range(data_count) if i not in parts]
    if not missing:
        return [parts[i] for i in range(data_count)], missing
    parity = sorted(i for i in parts if i >= data_count)
    if len(parity) < len(missing):
        raise ValueError("insufficient valid USB fragments")
    matrix, rhs = [], []
    known = [(i, parts[i]) for i in range(data_count) if i in parts]
    for p in parity[:len(missing)]:
        value = int.from_bytes(parts[p], "little")
        for i, chunk in known:
            value ^= scale(chunk, coefficient(p, i))
        rhs.append(value)
        matrix.append([coefficient(p, i) for i in missing])
    for i, row in enumerate(matrix):
        factor = INVERSE[row[i]]
        if not factor:
            raise ValueError("singular USB fragment matrix")
        row[:] = [MULTIPLY[factor][x] for x in row]
        rhs[i] = scale(rhs[i].to_bytes(CHUNK, "little"), factor)
        for j, other in enumerate(matrix):
            factor = other[i]
            if j != i and factor:
                other[:] = [a ^ MULTIPLY[factor][b] for a, b in zip(other, row, strict=True)]
                rhs[j] ^= scale(rhs[i].to_bytes(CHUNK, "little"), factor)
    restored = dict(parts)
    restored.update((i, value.to_bytes(CHUNK, "little")) for i, value in zip(missing, rhs, strict=True))
    return [restored[i] for i in range(data_count)], missing


def valid_record(record, expected_seq=None):
    if len(record) < RECORD_HEADER.size:
        return False
    magic, version, kind, hlen, seq, json_size, raw_size, crc = RECORD_HEADER.unpack_from(record)
    return (magic == RECORD_MAGIC and version == 1 and kind in (1, 2)
            and (expected_seq is None or seq == expected_seq)
            and hlen == RECORD_HEADER.size and 2 <= json_size <= JSON_MAX
            and raw_size in (0, RAW_BYTES) and (kind != 1 or raw_size == 0)
            and hlen + json_size + raw_size == len(record)
            and zlib.crc32(record[hlen:]) == crc)


def unpack_iq(record):
    if len(record) < RECORD_HEADER.size:
        raise ValueError("short packed RFB8 header")
    magic, version, kind, hlen, _, json_size, raw_size, _ = RECORD_HEADER.unpack_from(record)
    prefix = hlen + json_size
    if ((magic, version, kind, hlen, raw_size) != (RECORD_MAGIC, 1, 2, RECORD_HEADER.size, RAW_BYTES)
            or not 2 <= json_size <= JSON_MAX or not prefix < len(record) < prefix + raw_size):
        raise ValueError("invalid packed RFB8 bounds")
    raw = bytearray()
    i = prefix
    while i < len(record):
        value = record[i]
        i += 1
        if value == 0x80:
            if i + 2 > len(record):
                raise ValueError("truncated IQ escape")
            raw.extend(record[i:i+2])
            i += 2
        else:
            raw.extend((value, 255 if value & 128 else 0))
        if len(raw) > raw_size:
            raise ValueError("oversize IQ payload")
    if len(raw) != raw_size:
        raise ValueError("short IQ payload")
    return record[:prefix] + bytes(raw)


@dataclass
class Frame:
    sequence: int
    data_count: int
    size: int
    packed_iq: bool
    parts: dict = field(default_factory=dict)


class FragmentDecoder:
    def __init__(self, on_failure=None, *, allow_replay=False):
        self.on_failure = on_failure
        self.allow_replay = allow_replay
        self.buffer = bytearray()
        self.frame = None
        self.last_sequence = None
        self.counts = dict(fragment_crc_errors=0, discarded_bytes=0,
                           fragment_records=0, packed_records=0, packed_bytes_saved=0,
                           repaired_records=0, repaired_data_fragments=0,
                           unrepairable_records=0)
        self.repaired = []

    def _finalize(self):
        frame = self.frame
        self.frame = None
        self.last_sequence = frame.sequence
        try:
            chunks, missing = recover(frame.parts, frame.data_count)
            packet = b"".join(chunks)[:frame.size]
            if frame.packed_iq:
                packet = unpack_iq(packet)
            if not valid_record(packet, frame.sequence):
                raise ValueError("invalid reconstructed RFB8 record")
        except ValueError:
            self.counts["unrepairable_records"] += 1
            if self.on_failure is not None:
                self.on_failure(frame.sequence)
            return b""
        self.counts["fragment_records"] += 1
        if frame.packed_iq:
            self.counts["packed_records"] += 1
            self.counts["packed_bytes_saved"] += len(packet) - frame.size
        if missing:
            self.counts["repaired_records"] += 1
            self.counts["repaired_data_fragments"] += len(missing)
            self.repaired.append({"sequence": frame.sequence, "data_indices": missing})
        return packet

    def _consume_fragment(self, seq, idx, count, size, packed_iq, data):
        if self.last_sequence == seq and (not self.allow_replay or
                                         self.frame is None and idx == END):
            return b""
        output = b""
        if self.frame is not None and self.frame.sequence != seq:
            output = self._finalize()
        if self.frame is None:
            self.frame = Frame(seq, count, size, packed_iq)
        frame = self.frame
        if (frame.data_count, frame.size, frame.packed_iq) != (count, size, packed_iq):
            self.counts["fragment_crc_errors"] += 1
            return output
        if idx == END:
            return output + self._finalize()
        previous = frame.parts.setdefault(idx, data)
        if previous != data:
            self.counts["fragment_crc_errors"] += 1
        return output

    def feed(self, data):
        self.buffer.extend(data)
        output = bytearray()
        while self.buffer:
            position = self.buffer.find(MAGIC)
            discard = position if position >= 0 else max(0, len(self.buffer) - 3)
            if discard:
                self.counts["discarded_bytes"] += discard
                del self.buffer[:discard]
            if position < 0:
                break
            if len(self.buffer) < HEADER.size:
                break
            _, seq, idx, count, size_field, crc = HEADER.unpack_from(self.buffer)
            size = size_field & ~PACKED_IQ
            packed_iq = bool(size_field & PACKED_IQ)
            if (not 1 <= count <= MAX_DATA or not (count - 1) * CHUNK < size <= count * CHUNK
                    or not (idx == END or idx < count + parity_count(count))):
                self.counts["discarded_bytes"] += 1
                del self.buffer[:1]
                continue
            length = HEADER.size if idx == END else HEADER.size + CHUNK
            if len(self.buffer) < length:
                break
            if zlib.crc32(self.buffer[HEADER.size:length], zlib.crc32(self.buffer[:12])) != crc:
                self.counts["fragment_crc_errors"] += 1
                del self.buffer[:1]
                continue
            data = bytes(self.buffer[HEADER.size:length])
            output.extend(self._consume_fragment(seq, idx, count, size, packed_iq, data))
            del self.buffer[:length]
        return bytes(output)


class ReplayReceiver:
    """Reassemble and order exact RFB8 packets, requesting bounded repairs."""
    REORDER_LIMIT = 16
    NACK_LIMIT = 2
    RETRY_INTERVAL_S = 0.32
    TIMEOUT_S = 1.4
    MASK = (1 << 32) - 1

    def __init__(self):
        self.decoder = FragmentDecoder(self._note_failure, allow_replay=True)
        self.expected = None
        self.pending = {}
        self.failed = set()
        self.repaired_gaps = set()
        self.first_failed = set()
        self.requests = {}
        self.active = False
        self.committed = OrderedDict()
        self.duplicates = []
        self.duplicate_count = 0

    def begin(self):
        if self.expected is None:
            raise RuntimeError("RFB8 replay requires a verified initial sequence")
        self.active = True
        self.first_failed.clear()
        self.repaired_gaps.clear()
        self.requests.clear()
        self.failed.clear()
        self.committed.clear()
        self.duplicates.clear()
        self.duplicate_count = 0

    def _distance(self, sequence):
        return (sequence - self.expected) & self.MASK

    def _note_failure(self, sequence):
        if not self.active:
            return
        if self._distance(sequence) <= self.REORDER_LIMIT:
            self.failed.add(sequence)
            self.first_failed.add(sequence)

    def feed(self, data):
        restored = self.decoder.feed(data)
        output = bytearray()
        offset = 0
        while offset < len(restored):
            _, _, kind, header, sequence, meta, raw, _ = RECORD_HEADER.unpack_from(restored, offset)
            end = offset + header + meta + raw
            packet = restored[offset:end]
            offset = end
            if self.expected is None or not self.active:
                self.expected = (sequence + 1) & self.MASK
                output.extend(packet)
                continue
            distance = self._distance(sequence)
            if distance > self.MASK // 2:
                digest = self.committed.get(sequence)
                if digest is None or digest != hashlib.sha256(packet).digest():
                    raise RuntimeError("Unverifiable or conflicting late RFB8 replay")
                self.duplicate_count += 1
                if kind == 2:
                    self.duplicates.append(sequence)
                continue
            if distance > self.REORDER_LIMIT:
                raise RuntimeError("RFB8 replay reorder window exhausted")
            previous = self.pending.setdefault(sequence, packet)
            if previous != packet:
                raise RuntimeError("Conflicting RFB8 packets for one sequence")
        while self.expected in self.pending:
            sequence = self.expected
            packet = self.pending.pop(sequence)
            output.extend(packet)
            self.committed[sequence] = hashlib.sha256(packet).digest()
            if len(self.committed) > 64:
                self.committed.popitem(last=False)
            if sequence in self.requests:
                self.repaired_gaps.add(sequence)
            self.failed.discard(sequence)
            self.expected = (sequence + 1) & self.MASK
        return bytes(output)

    def request(self, now):
        if not self.active or not (self.pending or self.expected in self.failed):
            return None
        sequence = self.expected
        first, last, attempts = self.requests.get(sequence, (now, None, 0))
        if now - first >= self.TIMEOUT_S:
            raise TimeoutError(f"RFB8 replay deadline for sequence {sequence}")
        if attempts >= self.NACK_LIMIT or (last is not None and now - last < self.RETRY_INTERVAL_S):
            return None
        self.first_failed.add(sequence)
        self.requests[sequence] = (first, now, attempts + 1)
        return sequence

    def take_duplicates(self):
        duplicates, self.duplicates = self.duplicates, []
        return duplicates

    def statistics(self):
        return {
            "duplicate_records": self.duplicate_count,
            "pending_fragment_bytes": len(self.decoder.buffer),
            "partial_fragment_record": self.decoder.frame is not None,
            "fragment": dict(self.decoder.counts),
            "fec_repaired_sequences": [x["sequence"] for x in self.decoder.repaired],
            "first_failed_sequences": sorted(self.first_failed),
            "gaps_repaired_after_nack_scheduled": sorted(self.repaired_gaps),
            "replay_nacks_scheduled": sum(attempts for _, _, attempts in self.requests.values()),
            "unresolved_sequences": sorted(self.failed | (self.requests.keys() - self.repaired_gaps)),
            "pending_records": len(self.pending),
        }
