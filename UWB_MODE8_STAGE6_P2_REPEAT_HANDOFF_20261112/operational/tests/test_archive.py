"""Host adversarial tests for the operational anchor archive and recovery."""
import hashlib
import hmac
import os
from pathlib import Path
import socket
import struct
import sys
import tempfile
import threading
import unittest
import zlib

sys.path.insert(0,str(Path(__file__).resolve().parents[1]/"host"))
from ops_archive import (parse, ArchiveError, HEADER, ROW_BYTES, FRESH_P1_ROW_BYTES, PREFIX, TX, RX,
                         MAGIC, VERSION, LEGACY_ROW_BYTES, LEGACY_PREFIX, LEGACY_TX, LEGACY_RX,
                         to_anchor_record)
from ops_client import OperationalClient, TransportError

KEY=bytes(range(32))
CUSTODY_KEY=bytes(reversed(range(32)))

def signed_ack(blob):
    crc=parse(blob)["crc32"]
    msg=f"MODE8-CUSTODY:ACK:00000002:00000315:{crc:08X}:00000001"
    mac=hmac.new(CUSTODY_KEY,msg.encode("ascii"),hashlib.sha256).hexdigest()
    return f"ACKFILE 789 {crc} {mac}"

def archive(count=2, session=789, *, version=2):
    legacy = version == 1
    rows=[]
    row_size=LEGACY_ROW_BYTES if legacy else FRESH_P1_ROW_BYTES if version==VERSION else ROW_BYTES
    for i in range(count):
        fields=(session,i,12345,9876,321,0,0,3,3,0)
        if legacy:
            prefix=LEGACY_PREFIX.pack(*fields)
            tx=LEGACY_TX.pack(0,0,0,0,0)*5
            rx=LEGACY_RX.pack(0,0,0,0,0,0,0,0,0,bytes(96))*4
            frames=bytes(384)
        else:
            prefix=PREFIX.pack(*fields,0)
            slots=7 if version==VERSION else 6
            tx=TX.pack(0,0,0,0,0,255,0)*slots
            rx=RX.pack(0,0,0,0,0,0,0,0,0,255,bytes(96))*(6 if version==VERSION else 4)
            frames=bytes(96*slots)
        body=prefix+tx+rx+frames
        assert len(body)==row_size-4
        rows.append(body+struct.pack("<I",zlib.crc32(body)))
    payload=b"".join(rows)
    header=HEADER.pack(MAGIC,version,row_size,session,12345,321,count,zlib.crc32(payload),0)
    header=header[:-4]+struct.pack("<I",zlib.crc32(header))
    return header+payload

def recalc(blob, *, body=True):
    copy=bytearray(blob)
    if body:
        struct.pack_into("<I",copy,24,zlib.crc32(copy[32:]))
    struct.pack_into("<I",copy,28,0)
    struct.pack_into("<I",copy,28,zlib.crc32(copy[:32]))
    return bytes(copy)

def complete_archive(version, *, has_extra):
    blob = bytearray(archive(count=1, version=version))
    row = HEADER.size
    blob[row + 24] = 1
    blob[row + 27] = 2  # Selected P2 is a fresh exchange.
    tx_offset = row + PREFIX.size
    frame_offset = tx_offset + 6 * TX.size + 4 * RX.size
    # The selected fresh P2 and earlier primary are deliberately distinct.
    for slot, (kind, tick) in enumerate(zip((1, 4, 8, 5, 5), (100, 200, 400, 500, 300))):
        if slot == 4 and not has_extra:
            continue
        TX.pack_into(blob, tx_offset + slot * TX.size, tick, tick, 0x80, kind, 1, 0, 1)
        air = bytearray(96)
        air[:8] = b"FCB8" + bytes((1, kind, 96, 0))
        struct.pack_into("<5I", air, 8, 789, 0, 321, 12345, 9876)
        air[28], air[31] = 255, 6
        pos = 32 if kind in (1, 8) else 47
        air[pos:pos + 5] = tick.to_bytes(5, "little")
        struct.pack_into("<I", air, 92, zlib.crc32(air[:92]))
        offset = frame_offset + slot * 96
        blob[offset:offset + 96] = air
    row_crc = row + ROW_BYTES - 4
    struct.pack_into("<I", blob, row_crc, zlib.crc32(blob[row:row_crc]))
    return recalc(blob)


class ArchiveTests(unittest.TestCase):
    def test_valid(self):
        got=parse(archive())
        self.assertEqual(got["count"],2)
        self.assertEqual([r["txn"] for r in got["rows"]],[0,1])
    def test_legacy_archive_is_readable(self):
        result=parse(archive(version=1))
        self.assertEqual(result["archive_schema"],"mode8.anchor.ops.v1")
        self.assertEqual(len(result["rows"][0]["air_frames"]),4)

    def test_v3_preserves_selected_fresh_and_optional_primary_p2(self):
        for has_primary in (False, True):
            with self.subTest(has_primary=has_primary):
                result = parse(complete_archive(3, has_extra=has_primary))
                self.assertEqual(result["archive_schema"], "mode8.anchor.ops.v3")
                record = to_anchor_record(result["rows"][0], "anchor", 200000)
                self.assertTrue(record["p2_retry"])
                self.assertEqual(record["poll2_tx"]["actual"], 400)
                self.assertEqual(record["tx"][2]["actual"], 500)
                self.assertEqual(record["p2_primary_tx"]["ok"], has_primary)
                self.assertEqual(record["p2_primary_tx"]["actual"], 300 if has_primary else 0)
                self.assertEqual(bool(record["p2_primary_frame"]), has_primary)
                self.assertNotIn("p2_repeat_tx", record)

    def test_v4_has_separate_recovery_poll_and_responses(self):
        result = parse(archive(version=VERSION))
        self.assertEqual(result["archive_schema"], "mode8.anchor.ops.v4")
        record = to_anchor_record(result["rows"][0], "anchor", 200000)
        self.assertFalse(record["p1_repeat_poll_tx"]["attempted"])
        self.assertIsNone(record["p1_repeat_poll_frame"])
        self.assertEqual(len(record["p1_repeat_responses"]), 2)
        self.assertEqual(len(record["response2_rx"]), 2)

    def test_v2_retains_repeat_slot_and_requires_its_evidence(self):
        with self.assertRaisesRegex(ArchiveError, "missing authenticated RF evidence"):
            parse(complete_archive(2, has_extra=False))
        result = parse(complete_archive(2, has_extra=True))
        self.assertEqual(result["archive_schema"], "mode8.anchor.ops.v2")
        record = to_anchor_record(result["rows"][0], "anchor", 200000)
        self.assertEqual(record["p2_repeat_tx"]["actual"], 300)
        self.assertNotIn("p2_primary_tx", record)

    def test_truncated_header(self):
        with self.assertRaises(ArchiveError):parse(bytes(31))
    def test_corrupt_header(self):
        b=bytearray(archive());b[8]^=1
        with self.assertRaises(ArchiveError):parse(b)
    def test_corrupt_body(self):
        b=bytearray(archive());b[48]^=1
        with self.assertRaises(ArchiveError):parse(b)
    def test_row_crc_even_when_outer_fixed(self):
        b=bytearray(archive());b[49]^=1
        with self.assertRaisesRegex(ArchiveError,"row 0"):parse(recalc(b))
    def test_duplicate_txn_even_with_correct_crcs(self):
        b=bytearray(archive());off=32+ROW_BYTES
        struct.pack_into("<I",b,off+4,0)
        struct.pack_into("<I",b,off+ROW_BYTES-4,zlib.crc32(b[off:off+ROW_BYTES-4]))
        with self.assertRaisesRegex(ArchiveError,"transaction"):parse(recalc(b))
    def test_missing_rx_carries_orphan_bytes(self):
        b=bytearray(archive());offset=32+PREFIX.size+6*TX.size
        b[offset+RX.size-1]=0xAA
        struct.pack_into("<I",b,32+ROW_BYTES-4,zlib.crc32(b[32:32+ROW_BYTES-4]))
        with self.assertRaisesRegex(ArchiveError,"orphaned"):parse(recalc(b))
    def test_complete_without_source_frames_rejected_even_with_valid_crcs(self):
        b=bytearray(archive())
        b[32 + 24] = 1  # complete=True in row 0
        off=32
        struct.pack_into("<I",b,off+ROW_BYTES-4,zlib.crc32(b[off:off+ROW_BYTES-4]))
        with self.assertRaisesRegex(ArchiveError,"missing authenticated RF evidence"):
            parse(recalc(b))

    def test_overlarge_count(self):
        b=bytearray(archive());struct.pack_into("<I",b,20,33)
        with self.assertRaises(ArchiveError):parse(recalc(b))

class FakeServer(threading.Thread):
    def __init__(self, send_partial=False, lose_ack=False, blob=None):
        super().__init__(daemon=True)
        self.sock=socket.socket();self.sock.bind(("127.0.0.1",0));self.sock.listen(1)
        self.port=self.sock.getsockname()[1]
        self.send_partial=send_partial
        self.error=None
        self.archive=archive() if blob is None else blob
        self.lose_ack=lose_ack
        self.ack=None
    def run(self):
        try:
            s,_=self.sock.accept()
            with s:
                s.settimeout(5)
                s.sendall(b"CHALLENGE 00000001 00000002\n")
                f=s.makefile("rb")
                got=f.readline().decode().strip()
                self.check(got)
                s.sendall(b"AUTH_OK\n")
                self.assert_get(f.readline().decode().strip())
                prefix=f"DATA 789 {len(self.archive)} {parse(self.archive)['crc32']}\n".encode()
                if self.send_partial:
                    s.sendall(prefix+self.archive[:18]);return
                s.sendall(prefix+self.archive+b"END\n")
                self.ack=f.readline().decode().strip()
                if not self.lose_ack and self.ack==signed_ack(self.archive):
                    s.sendall(b"ACKFILE_OK\n")
        except Exception as e:self.error=e
        finally:self.sock.close()
    def check(self,text):
        expected=hmac.new(KEY,b"MODE8:00000001:00000002",hashlib.sha256).hexdigest()
        if text!="AUTH "+expected:raise AssertionError("bad HMAC")
    def assert_get(self,text):
        if text!="GET":raise AssertionError("bad GET")

class TransportTests(unittest.TestCase):
    def test_standalone_get_ack_cannot_bypass_paired_audit(self):
        # No network call is made even if the caller asks for ACK=True.
        client=OperationalClient('127.0.0.1',KEY)
        with tempfile.TemporaryDirectory() as folder:
            with self.assertRaisesRegex(TransportError,'unpaired ACK forbidden'):
                client.get_archive(Path(folder)/'unverified.bin',789,ack=True)

    def test_durable_get_ack(self):
        server=FakeServer();server.start()
        with tempfile.TemporaryDirectory() as folder:
            dest=Path(folder)/"archive.bin"
            with OperationalClient("127.0.0.1",KEY,server.port,approval_key=CUSTODY_KEY) as cli:
                data=cli.get_archive(dest,789,ack=False)
                # Transport-only fixture: paired proof is checked by ops_session.
                self.assertEqual(cli.acknowledge_archive(789,data["crc32"]),"ACKFILE_OK")
            self.assertEqual(data["count"],2)
            self.assertEqual(dest.read_bytes(),server.archive)
            self.assertEqual(list(dest.parent.glob("*.partial")),[])
        server.join(5);self.assertIsNone(server.error)
    def test_truncated_download_never_acked(self):
        server=FakeServer(send_partial=True);server.start()
        with tempfile.TemporaryDirectory() as folder:
            dest=Path(folder)/"archive.bin"
            with self.assertRaises((TransportError,ConnectionError,TimeoutError,EOFError)):
                with OperationalClient("127.0.0.1",KEY,server.port,timeout=1,approval_key=CUSTODY_KEY) as cli:
                    cli.get_archive(dest,789,ack=False)
            self.assertFalse(dest.exists())
            self.assertEqual(len(list(dest.parent.glob("*.partial"))),1)
        server.join(5);self.assertIsNone(server.error)

    def test_retry_after_ack_reply_is_lost(self):
        first=FakeServer(lose_ack=True)
        retry=FakeServer()
        with tempfile.TemporaryDirectory() as folder:
            dest=Path(folder)/"archive.bin"
            first.start()
            with self.assertRaises(TransportError):
                with OperationalClient("127.0.0.1",KEY,first.port,approval_key=CUSTODY_KEY) as cli:
                    doc=cli.get_archive(dest,789,ack=False)
                    cli.acknowledge_archive(789,doc["crc32"])
            self.assertEqual(dest.read_bytes(),first.archive)
            retry.start()
            with OperationalClient("127.0.0.1",KEY,retry.port,approval_key=CUSTODY_KEY) as cli:
                doc=cli.get_archive(dest,789,ack=False)
                self.assertEqual(cli.acknowledge_archive(789,doc["crc32"]),"ACKFILE_OK")
            self.assertEqual(dest.read_bytes(),first.archive)
            self.assertEqual(list(dest.parent.glob("*.partial")),[])
        for server in (first,retry):
            server.join(5)
            self.assertIsNone(server.error)
            self.assertEqual(server.ack,signed_ack(server.archive))

    def test_different_archive_cannot_replace_prior_evidence(self):
        server=FakeServer(blob=archive(count=1))
        with tempfile.TemporaryDirectory() as folder:
            dest=Path(folder)/"archive.bin"
            original=archive(count=2)
            dest.write_bytes(original)
            server.start()
            with self.assertRaisesRegex(TransportError,"differs"):
                with OperationalClient("127.0.0.1",KEY,server.port,approval_key=CUSTODY_KEY) as cli:
                    cli.get_archive(dest,789,ack=False)
            self.assertEqual(dest.read_bytes(),original)
            self.assertEqual(len(list(dest.parent.glob("*.partial"))),1)
        server.join(5)
        self.assertIsNone(server.error)
        self.assertEqual(server.ack,"")

if __name__=="__main__":
    unittest.main()
