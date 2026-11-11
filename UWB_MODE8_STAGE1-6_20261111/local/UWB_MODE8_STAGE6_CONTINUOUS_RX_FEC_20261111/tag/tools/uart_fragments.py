"""Recover Stage 6 RFB8 records from CRC-checked USB fragments."""

from dataclasses import dataclass, field
import struct
import zlib

from rfb8 import HEADER as RECORD_HEADER, MAGIC as RECORD_MAGIC, JSON_MAX, RAW_BYTES


MAGIC = b"R8F6"
HEADER = struct.Struct("<4sIBBHI")
CHUNK = 256
MAX_DATA = 96
END = 255
LOW_MASK = (1 << 32) - 1


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


@dataclass
class Frame:
    sequence: int
    data_count: int
    size: int
    parts: dict = field(default_factory=dict)


class FragmentDecoder:
    def __init__(self):
        self.buffer = bytearray()
        self.frame = None
        self.last_sequence = None
        self.counts = dict(fragment_crc_errors=0, discarded_bytes=0, native_crc_errors=0,
                           fragment_records=0, repaired_records=0, repaired_data_fragments=0,
                           unrepairable_records=0)
        self.repaired = []

    def _finalize(self):
        frame = self.frame
        self.frame = None
        self.last_sequence = frame.sequence
        try:
            chunks, missing = recover(frame.parts, frame.data_count)
            packet = b"".join(chunks)[:frame.size]
            if not valid_record(packet, frame.sequence):
                raise ValueError("invalid reconstructed RFB8 record")
        except ValueError:
            self.counts["unrepairable_records"] += 1
            return b""
        self.counts["fragment_records"] += 1
        if missing:
            self.counts["repaired_records"] += 1
            self.counts["repaired_data_fragments"] += len(missing)
            self.repaired.append({"sequence": frame.sequence, "data_indices": missing})
        return packet

    def _consume_fragment(self, seq, idx, count, size, data):
        if self.last_sequence == seq:
            return b""
        output = b""
        if self.frame is not None and self.frame.sequence != seq:
            output = self._finalize()
        if self.frame is None:
            self.frame = Frame(seq, count, size)
        frame = self.frame
        if (frame.data_count, frame.size) != (count, size):
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
            positions = [i for i in (self.buffer.find(MAGIC), self.buffer.find(RECORD_MAGIC)) if i >= 0]
            discard = min(positions) if positions else max(0, len(self.buffer) - 3)
            if discard:
                self.counts["discarded_bytes"] += discard
                del self.buffer[:discard]
            if not positions:
                break
            if self.buffer.startswith(RECORD_MAGIC):
                if len(self.buffer) < RECORD_HEADER.size:
                    break
                _, ver, kind, hlen, _, nj, nr, _ = RECORD_HEADER.unpack_from(self.buffer)
                if (ver, kind, hlen, nr) != (1, 1, RECORD_HEADER.size, 0) or not 2 <= nj <= JSON_MAX:
                    self.counts["discarded_bytes"] += 1
                    del self.buffer[:1]
                    continue
                length = hlen + nj
                if len(self.buffer) < length:
                    break
                record = bytes(self.buffer[:length])
                if not valid_record(record):
                    self.counts["native_crc_errors"] += 1
                    del self.buffer[:1]
                    continue
                if self.frame is not None:
                    output.extend(self._finalize())
                output.extend(record)
                del self.buffer[:length]
                continue
            if len(self.buffer) < HEADER.size:
                break
            _, seq, idx, count, size, crc = HEADER.unpack_from(self.buffer)
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
            output.extend(self._consume_fragment(seq, idx, count, size, data))
            del self.buffer[:length]
        return bytes(output)

