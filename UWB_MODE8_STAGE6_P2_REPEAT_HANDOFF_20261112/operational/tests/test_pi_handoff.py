"""Exercise real USB framing and durable record ACK with slow Pi storage."""
from collections import deque
import json
import os
from pathlib import Path
import sys
import tempfile
import threading
import time
from types import SimpleNamespace
import unittest
from unittest.mock import patch
import zlib

TAG_PACKAGE = Path(__file__).resolve().parents[2] / "firmware" / "tag_p2_repeat"
sys.path[:0] = [str(TAG_PACKAGE / "tools"), str(TAG_PACKAGE / "tests")]

from rfb8 import CIR_BYTES, RAW_BYTES, encode
from run_pair import HANDOFF_MAX_BYTES, Link
from test_uart_fragments import wire


class ChunkedSerial:
    def __init__(self):
        self.chunks = deque()
        self.ready = threading.Condition()
        self.writes = []
        self.received = bytearray()
        self.read_count = 0

    def open(self):
        pass

    def close(self):
        pass

    def push(self, data):
        with self.ready:
            self.chunks.extend(data[i:i+32] for i in range(0, len(data), 32))
            self.ready.notify_all()

    @property
    def in_waiting(self):
        return 32

    def read(self, size):
        with self.ready:
            if not self.ready.wait_for(lambda: bool(self.chunks), timeout=0.01):
                return b""
            data = self.chunks.popleft()
            self.received.extend(data)
            self.read_count += 1
            return data

    def write(self, data):
        self.writes.append((data, time.perf_counter()))
        return len(data)


def packet(sequence, *, corrupt_inner_crc=False):
    raw = bytes(range(256)) * (RAW_BYTES // 256)
    cir = []
    for slot in range(4):
        iq = raw[slot*CIR_BYTES:(slot+1)*CIR_BYTES]
        cir.append({"slot": slot, "radio": slot % 2, "probe": slot // 2 + 1,
                    "present": True, "valid": True, "bytes": CIR_BYTES,
                    "crc32": zlib.crc32(iq) ^ (slot == 0 and corrupt_inner_crc)})
    return encode({"schema": "dual8.record.v1", "mode": 8, "role": "tag",
                   "complete": True, "boot": 7, "session": 42, "cir": cir,
                   "fixture": "x" * 4700}, raw, sequence=sequence)


def usb(frame, seq):
    return b"".join(wire(frame, seq)[0])


class PiHandoffTest(unittest.TestCase):
    def open_link(self, directory, limit=HANDOFF_MAX_BYTES, *, durable_group_commit=False):
        serial = ChunkedSerial()
        with patch.dict(sys.modules, {"serial": SimpleNamespace(Serial=lambda **_: serial)}):
            link = Link("tag", "test-usb", directory, 2000000, handoff_limit_bytes=limit,
                        durable_group_commit=durable_group_commit)
        hello = encode({"event": "HELLO"}, kind=1, sequence=10)
        prefix = usb(hello, 10)
        serial.push(prefix)
        link.wait(lambda event: event.get("event") == "HELLO")
        link.wait_persisted()
        link.begin_capture(7, 42)
        return link, serial, hello, prefix

    def test_500_ms_fsync_keeps_usb_reading_and_ack_follows_fsync(self):
        frames = [packet(seq) for seq in range(11, 15)]
        incoming = [usb(frame, seq) for seq, frame in enumerate(frames, start=11)]
        real_fsync = os.fsync
        fsync_started = threading.Event()
        completed_fsyncs = []
        with tempfile.TemporaryDirectory() as folder:
            directory = Path(folder)
            link, serial, hello, prefix = self.open_link(directory)

            def delay_first_fsync(fd):
                in_writer = threading.current_thread() is link.writer
                if in_writer and not completed_fsyncs:
                    fsync_started.set()
                    time.sleep(0.5)
                real_fsync(fd)
                if in_writer:
                    completed_fsyncs.append((serial.read_count, time.perf_counter()))

            with patch("run_pair.os.fsync", side_effect=delay_first_fsync):
                try:
                    serial.push(incoming[0])
                    self.assertTrue(fsync_started.wait(2))
                    reads_at_stall = serial.read_count
                    serial.push(b"".join(incoming[1:]))
                    with link.condition:
                        self.assertTrue(link.condition.wait_for(
                            lambda: link.records == 4 or link.exception, timeout=3))
                    self.assertIsNone(link.exception)
                    self.assertEqual(link.records, 4)
                    link.wait_persisted()
                finally:
                    link.close()

            self.assertGreater(completed_fsyncs[0][0] - reads_at_stall, 1024)
            for index, seq in enumerate(range(11, 15)):
                attempts = [stamp for command, stamp in serial.writes
                            if command == f"REC_ACK 7 42 {seq}\n".encode()]
                self.assertGreaterEqual(attempts[0], completed_fsyncs[index][1])
            self.assertEqual((directory / "tag.uart").read_bytes(), prefix + b"".join(incoming))
            self.assertEqual((directory / "tag.rfb8").read_bytes(), hello + b"".join(frames))
            stats = link.stats()
            self.assertFalse(stats["capture_overflow"])
            self.assertFalse(any(stats["capture_decoder"].values()))
            self.assertEqual(stats["transport"]["durable_acks_sent"], 4)
            self.assertEqual(stats["storage_handoff"]["host_pending_buffer_bytes"], 0)
            self.assertGreater(stats["storage_handoff"]["host_peak_buffer_bytes"], 32768)
            self.assertLessEqual(stats["storage_handoff"]["host_peak_buffer_bytes"], HANDOFF_MAX_BYTES)
            self.assertGreaterEqual(stats["storage_io"]["rfb8_fsync"]["max_us"], 500000)
            self.assertGreaterEqual(stats["storage_io"]["record_to_ack"]["max_us"], 500000)
            self.assertEqual(stats["storage_io"]["ack_send"]["count"], 4)
            print(json.dumps({"test": "500ms fsync", "originals_equal": True,
                              "durable_acks": 4, "buffer": stats["storage_handoff"],
                              "timing": stats["storage_io"]}, sort_keys=True))

    def test_group_commit_keeps_200ms_stream_durable_under_repeated_sd_delays(self):
        sequences = list(range(11, 43))
        frames = [packet(seq) for seq in sequences]
        incoming = [usb(frame, seq) for frame, seq in zip(frames, sequences)]
        real_fsync = os.fsync
        completed = []
        sent_at = []
        with tempfile.TemporaryDirectory() as folder:
            directory = Path(folder)
            link, serial, hello, prefix = self.open_link(directory, durable_group_commit=True)

            def slow_disk(fd):
                in_writer = threading.current_thread() is link.writer
                if in_writer:
                    # The failed G8 point had a 721 ms maximum and 260 ms mean
                    # per-record fsync. Apply a similar delay profile per sync.
                    time.sleep(0.721 if not completed else 0.245)
                real_fsync(fd)
                if in_writer:
                    completed.append((os.lseek(fd, 0, os.SEEK_CUR), time.perf_counter()))

            with patch("run_pair.os.fsync", side_effect=slow_disk):
                try:
                    start = time.perf_counter()
                    for index, frame in enumerate(incoming):
                        time.sleep(max(0, start + index * 0.2 - time.perf_counter()))
                        sent_at.append(time.perf_counter())
                        serial.push(frame)
                    with link.condition:
                        self.assertTrue(link.condition.wait_for(
                            lambda: link.records == len(sequences) or link.exception, timeout=4))
                    self.assertIsNone(link.exception)
                    link.wait_persisted()
                finally:
                    link.close()

            end = len(hello)
            first_ack = []
            for seq, frame in zip(sequences, frames):
                end += len(frame)
                fsync_at = next(stamp for offset, stamp in completed if offset >= end)
                sends = [stamp for command, stamp in serial.writes
                         if command == f"REC_ACK 7 42 {seq}\n".encode()]
                self.assertTrue(sends, f"missing ACK for sequence {seq}")
                self.assertTrue(all(stamp >= fsync_at for stamp in sends))
                first_ack.append(sends[0])
            # Reserve must see a free slot before each new POLL. At most the
            # record being emitted plus five older unacknowledged records fit.
            peak_unacked = max(sum(acked > start for acked in first_ack[:index + 1])
                               for index, start in enumerate(sent_at))
            self.assertLessEqual(peak_unacked, 6)
            stats = link.stats()
            timing = stats["storage_io"]
            timeline = stats["durable_ack_timeline"]
            self.assertEqual([entry["sequence"] for entry in timeline], sequences)
            for entry in timeline:
                self.assertLessEqual(entry["handoff_ns"], entry["fsync_start_ns"])
                self.assertLessEqual(entry["fsync_start_ns"], entry["fsync_end_ns"])
                self.assertLessEqual(entry["fsync_end_ns"], entry["ack_write_end_ns"])
            self.assertLessEqual(timing["rfb8_fsync"]["count"], 20)
            self.assertGreaterEqual(timing["rfb8_fsync"]["max_us"], 721000)
            self.assertLess(timing["record_to_ack"]["max_us"], 1500000)
            self.assertEqual(stats["transport"]["durable_acks_sent"], 32)
            self.assertFalse(stats["capture_overflow"])
            self.assertFalse(any(stats["capture_decoder"].values()))
            self.assertLessEqual(stats["storage_handoff"]["host_peak_buffer_bytes"], HANDOFF_MAX_BYTES)
            self.assertEqual(stats["storage_handoff"]["host_pending_buffer_bytes"], 0)
            self.assertEqual((directory / "tag.uart").read_bytes(), prefix + b"".join(incoming))
            self.assertEqual((directory / "tag.rfb8").read_bytes(), hello + b"".join(frames))
            print(json.dumps({"test": "200ms grouped durability with 721ms and 245ms sync stalls",
                              "originals_equal": True, "durable_acks": 32,
                              "peak_simulated_unacked": peak_unacked,
                              "buffer": stats["storage_handoff"], "timing": timing}, sort_keys=True))

    def test_group_commits_pending_records_at_control_and_eof(self):
        with tempfile.TemporaryDirectory() as folder:
            directory = Path(folder)
            link, serial, hello, prefix = self.open_link(directory, durable_group_commit=True)
            first = packet(11)
            event = encode({"event": "STOPPED"}, kind=1, sequence=12)
            last = packet(13)
            incoming = usb(first, 11) + usb(event, 12)
            final_wire = usb(last, 13)
            try:
                serial.push(incoming)
                link.wait(lambda row: row.get("event") == "STOPPED")
                self.assertIn(b"REC_ACK 7 42 11\n", [cmd for cmd, _ in serial.writes])
                serial.push(final_wire)
                with link.condition:
                    self.assertTrue(link.condition.wait_for(
                        lambda: link.replay.expected == 14, timeout=2))
            finally:
                link.close()
            self.assertEqual(link.stats()["transport"]["durable_acks_sent"], 2)
            self.assertEqual((directory / "tag.uart").read_bytes(), prefix + incoming + final_wire)
            self.assertEqual((directory / "tag.rfb8").read_bytes(), hello + first + event + last)

    def test_group_failure_acknowledges_none_of_the_uncommitted_records(self):
        real_fsync = os.fsync
        for injected in ("fsync", "CIR"):
            with self.subTest(injected=injected), tempfile.TemporaryDirectory() as folder:
                directory = Path(folder)
                link, serial, hello, prefix = self.open_link(directory, durable_group_commit=True)
                first = packet(11)
                second = packet(12, corrupt_inner_crc=(injected == "CIR"))
                incoming = usb(first, 11) + usb(second, 12)

                def fail_writer_fsync(fd):
                    if injected == "fsync" and threading.current_thread() is link.writer:
                        raise OSError("injected group fsync failure")
                    real_fsync(fd)

                with patch("run_pair.os.fsync", side_effect=fail_writer_fsync):
                    try:
                        serial.push(incoming)
                        with link.condition:
                            self.assertTrue(link.condition.wait_for(lambda: link.exception, timeout=3))
                    finally:
                        with self.assertRaisesRegex(RuntimeError, "fsync failure|CIR integrity"):
                            link.close()
                self.assertFalse(serial.writes)
                self.assertEqual(link.records, 0)
                self.assertEqual((directory / "tag.uart").read_bytes(), prefix + incoming)
                self.assertEqual((directory / "tag.rfb8").read_bytes(), hello + first + second)
                self.assertEqual(link.stats()["storage_handoff"]["host_pending_buffer_bytes"], 0)

    def test_failed_fsync_or_bad_cir_never_acks_record(self):
        real_fsync = os.fsync
        for injected in ("fsync", "CIR"):
            with self.subTest(injected=injected), tempfile.TemporaryDirectory() as folder:
                directory = Path(folder)
                link, serial, hello, prefix = self.open_link(directory)
                frame = packet(11, corrupt_inner_crc=(injected == "CIR"))
                incoming = usb(frame, 11)

                def fail_writer_fsync(fd):
                    if injected == "fsync" and threading.current_thread() is link.writer:
                        raise OSError("injected fsync failure")
                    real_fsync(fd)

                with patch("run_pair.os.fsync", side_effect=fail_writer_fsync):
                    try:
                        serial.push(incoming)
                        with link.condition:
                            self.assertTrue(link.condition.wait_for(lambda: link.exception, timeout=2))
                    finally:
                        with self.assertRaisesRegex(RuntimeError, "fsync failure|CIR integrity"):
                            link.close()
                self.assertFalse(serial.writes)
                self.assertEqual(link.records, 0)
                self.assertEqual((directory / "tag.uart").read_bytes(), prefix + incoming)
                self.assertEqual((directory / "tag.rfb8").read_bytes(), hello + frame)

    def test_byte_limit_rejects_record_and_keeps_received_wire(self):
        with tempfile.TemporaryDirectory() as folder:
            directory = Path(folder)
            link, serial, hello, _ = self.open_link(directory, limit=32768)
            try:
                serial.push(usb(packet(11), 11))
                with link.condition:
                    self.assertTrue(link.condition.wait_for(lambda: link.exception, timeout=2))
                self.assertTrue(link.capture_overflow)
            finally:
                link.close()
            self.assertEqual((directory / "tag.uart").read_bytes(), serial.received)
            self.assertEqual((directory / "tag.rfb8").read_bytes(), hello)
            self.assertFalse(serial.writes)
            stats = link.stats()["storage_handoff"]
            self.assertLessEqual(stats["host_peak_buffer_bytes"], 32768)
            self.assertGreater(stats["host_rejected_buffer_bytes"], 32768)
