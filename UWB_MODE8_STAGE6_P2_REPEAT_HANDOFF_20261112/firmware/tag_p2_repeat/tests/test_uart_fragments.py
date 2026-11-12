import json
import unittest
import zlib

from rfb8 import HEADER as RFB8_HEADER, RAW_BYTES
from uart_fragments import (
    CHUNK, END, HEADER, MAGIC, MULTIPLY, PACKED_IQ, FragmentDecoder, coefficient, parity_count,
)


def record(sequence):
    meta = json.dumps({"event": "test", "session": 77}).encode()
    raw = bytes((i * 71 + (i // 251) * 17) & 255 for i in range(RAW_BYTES))
    return RFB8_HEADER.pack(b"RFB8", 1, 2, 24, sequence, len(meta), len(raw),
                           zlib.crc32(meta + raw)) + meta + raw


def wire(record_bytes, sequence, *, packed=False):
    blocks = [record_bytes[i:i+CHUNK].ljust(CHUNK, b"\0")
              for i in range(0, len(record_bytes), CHUNK)]
    count = len(blocks)
    encoded = []

    def frame(i, body):
        h = HEADER.pack(MAGIC, sequence, i, count, len(record_bytes) | (PACKED_IQ if packed else 0), 0)
        crc = zlib.crc32(h[:12] + body)
        return h[:12] + crc.to_bytes(4, "little") + body

    for i, part in enumerate(blocks):
        encoded.append(frame(i, part))
    for i in range(count, count + parity_count(count)):
        parity = 0
        for j, part in enumerate(blocks):
            parity ^= int.from_bytes(part.translate(MULTIPLY[coefficient(i, j)]), "little")
        encoded.append(frame(i, parity.to_bytes(CHUNK, "little")))
    encoded.extend([frame(END, b"")] * 3)
    return encoded, count


class UsbFragmentTest(unittest.TestCase):
    def test_recovers_deleted_bytes_dropped_fragments_and_markers(self):
        payload = record(91)
        frames, count = wire(payload, 91)
        # Deletions like the confirmed 2 Mbps capture, including a header,
        # two consecutive damaged data fragments and a damaged parity row.
        for i, offset in ((2, 27), (3, 219), (7, 5), (count, 80)):
            frames[i] = frames[i][:offset] + frames[i][offset+1:]
        frames[15] = b""
        frames[-3] = frames[-3][:-1]
        received = b"".join(frames)
        decoder = FragmentDecoder()
        reconstructed = b"".join(decoder.feed(received[i:i+137])
                                 for i in range(0, len(received), 137))
        self.assertEqual(reconstructed, payload)
        self.assertEqual(decoder.repaired, [{"sequence": 91, "data_indices": [2, 3, 7, 15]}])
        self.assertEqual(decoder.counts["unrepairable_records"], 0)

    def test_damaged_control_fragment_is_never_parsed_as_native_rfb8(self):
        meta = json.dumps({"event": "HELLO", "firmware": "stage6"}).encode()
        payload = RFB8_HEADER.pack(b"RFB8", 1, 1, 24, 91, len(meta), 0,
                                   zlib.crc32(meta)) + meta
        frames, _ = wire(payload, 91)
        frames[0] = frames[0][:48] + frames[0][49:]
        decoder = FragmentDecoder()
        self.assertEqual(decoder.feed(b"".join(frames)), payload)
        self.assertEqual(decoder.counts["fragment_crc_errors"], 1)
        self.assertEqual(decoder.counts["repaired_records"], 1)

    def test_packed_samples_recover_exact_original_after_usb_deletion(self):
        values = (-32768, -129, -128, -127, -1, 0, 1, 127, 128, 32767) + (0,) * 30
        words = (values * ((RAW_BYTES // 2) // len(values) + 1))[:RAW_BYTES // 2]
        raw = b"".join(value.to_bytes(2, "little", signed=True) for value in words)
        meta = b'{"event":"sample"}'
        header = RFB8_HEADER.pack(b"RFB8", 1, 2, 24, 93, len(meta), len(raw), zlib.crc32(meta + raw))
        coded = b"".join(bytes((value & 255,)) if -127 <= value <= 127
                         else b"\x80" + value.to_bytes(2, "little", signed=True) for value in words)
        frames, _ = wire(header + meta + coded, 93, packed=True)
        frames[5] = frames[5][:70] + frames[5][71:]
        decoder = FragmentDecoder()
        self.assertEqual(decoder.feed(b"".join(frames)), header + meta + raw)
        self.assertEqual(decoder.counts["packed_records"], 1)
        self.assertEqual(decoder.counts["packed_bytes_saved"], len(raw) - len(coded))
        self.assertEqual(decoder.counts["repaired_records"], 1)
        frames, _ = wire(header + meta + coded[:-1] + b"\x80", 93, packed=True)
        decoder = FragmentDecoder()
        self.assertEqual(decoder.feed(b"".join(frames)), b"")
        self.assertEqual(decoder.counts["unrepairable_records"], 1)

    def test_exhausted_recovery_fails_closed(self):
        payload = record(91)
        frames, count = wire(payload, 91)
        decoder = FragmentDecoder()
        self.assertEqual(decoder.feed(b"".join(frames[parity_count(count)+1:])), b"")
        self.assertEqual(decoder.counts["unrepairable_records"], 1)

    def test_lost_end_marker_recovers_on_next_sequence(self):
        payload, later = record(91), record(92)
        first, _ = wire(payload, 91)
        second, _ = wire(later, 92)
        decoder = FragmentDecoder()
        self.assertEqual(decoder.feed(b"".join(first[:-3])), b"")
        self.assertEqual(decoder.feed(b"".join(second)), payload + later)


if __name__ == "__main__":
    unittest.main()

