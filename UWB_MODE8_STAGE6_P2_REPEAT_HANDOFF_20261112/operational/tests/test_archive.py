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
from ops_archive import parse, ArchiveError, HEADER, ROW_BYTES, PREFIX, TX, RX, MAGIC, VERSION
from ops_client import OperationalClient, TransportError

KEY=bytes(range(32))

def archive(count=2, session=789, *, frame=False):
    rows=[]
    for i in range(count):
        prefix=PREFIX.pack(session,i,12345,9876,321,0,1,3,3,0)
        tx=b"".join(TX.pack(0,0,0,0,0) for _ in range(5))
        rx=b"".join(RX.pack(0,0,0,0,0,0,0,0,0,bytes(96)) for _ in range(4))
        frames=bytes(384)
        body=prefix+tx+rx+frames
        assert len(body)==ROW_BYTES-4
        rows.append(body+struct.pack("<I",zlib.crc32(body)))
    payload=b"".join(rows)
    header=HEADER.pack(MAGIC,VERSION,ROW_BYTES,session,12345,321,count,zlib.crc32(payload),0)
    header=header[:-4]+struct.pack("<I",zlib.crc32(header))
    return header+payload

def recalc(blob, *, body=True):
    copy=bytearray(blob)
    if body:
        struct.pack_into("<I",copy,24,zlib.crc32(copy[32:]))
    struct.pack_into("<I",copy,28,0)
    struct.pack_into("<I",copy,28,zlib.crc32(copy[:32]))
    return bytes(copy)

class ArchiveTests(unittest.TestCase):
    def test_valid(self):
        got=parse(archive())
        self.assertEqual(got["count"],2)
        self.assertEqual([r["txn"] for r in got["rows"]],[0,1])
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
        b=bytearray(archive());offset=32+PREFIX.size+5*TX.size
        b[offset+RX.size-1]=0xAA
        struct.pack_into("<I",b,32+ROW_BYTES-4,zlib.crc32(b[32:32+ROW_BYTES-4]))
        with self.assertRaisesRegex(ArchiveError,"orphaned"):parse(recalc(b))
    def test_overlarge_count(self):
        b=bytearray(archive());struct.pack_into("<I",b,20,33)
        with self.assertRaises(ArchiveError):parse(recalc(b))

class FakeServer(threading.Thread):
    def __init__(self, send_partial=False):
        super().__init__(daemon=True)
        self.sock=socket.socket();self.sock.bind(("127.0.0.1",0));self.sock.listen(1)
        self.port=self.sock.getsockname()[1]
        self.send_partial=send_partial
        self.error=None
        self.archive=archive()
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
                ack=f.readline().decode().strip()
                if ack=="ACKFILE 789":s.sendall(b"ACKFILE_OK\n")
        except Exception as e:self.error=e
        finally:self.sock.close()
    def check(self,text):
        expected=hmac.new(KEY,b"MODE8:00000001:00000002",hashlib.sha256).hexdigest()
        if text!="AUTH "+expected:raise AssertionError("bad HMAC")
    def assert_get(self,text):
        if text!="GET":raise AssertionError("bad GET")

class TransportTests(unittest.TestCase):
    def test_durable_get_ack(self):
        server=FakeServer();server.start()
        with tempfile.TemporaryDirectory() as folder:
            dest=Path(folder)/"archive.bin"
            with OperationalClient("127.0.0.1",KEY,server.port) as cli:
                data=cli.get_archive(dest,789,ack=True)
            self.assertEqual(data["count"],2)
            self.assertEqual(dest.read_bytes(),server.archive)
            self.assertFalse(dest.with_suffix(".bin.partial").exists())
        server.join(5);self.assertIsNone(server.error)
    def test_truncated_download_never_acked(self):
        server=FakeServer(send_partial=True);server.start()
        with tempfile.TemporaryDirectory() as folder:
            dest=Path(folder)/"archive.bin"
            with self.assertRaises((TransportError,ConnectionError,TimeoutError,EOFError)):
                with OperationalClient("127.0.0.1",KEY,server.port,timeout=1) as cli:
                    cli.get_archive(dest,789,ack=True)
            self.assertFalse(dest.exists())
        server.join(5);self.assertIsNone(server.error)

if __name__=="__main__":
    unittest.main()
