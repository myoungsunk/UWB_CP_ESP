#!/usr/bin/env python3
"""Authenticated anchor TCP control and durable archive retrieval.

USB tag and anchor measurement must be prepared by the existing Mode 8
pair collector. Never start RF from two independent controllers.
"""
from __future__ import annotations
import argparse
import hashlib
import hmac
import json
import os
from pathlib import Path
import socket
import tempfile

from ops_archive import MAX_FILE_BYTES, parse, ArchiveError

class TransportError(RuntimeError):
    pass

class OperationalClient:
    def __init__(self,host:str,key:bytes,port:int=29310,timeout:float=10,
                 approval_key:bytes|None=None):
        if len(key)!=32: raise ValueError("AUTH key requires exactly 32 bytes")
        if approval_key is not None and (len(approval_key)!=32 or approval_key==key):
            raise ValueError("separate 32-byte custody key required")
        self.address=(host,port);self.key=key;self.timeout=timeout
        self.approval_key=approval_key;self.nonce=None
        self.sock=None;self.stream=None

    def __enter__(self):
        self.sock=socket.create_connection(self.address,self.timeout)
        try:
            self.sock.settimeout(self.timeout)
            self.stream=self.sock.makefile("rb")
            hello=self.readline().split()
            if len(hello)!=3 or hello[0]!="CHALLENGE":
                raise TransportError(f"missing challenge: {hello!r}")
            nonce=int(hello[1],16)
            self.nonce=nonce
            self.boot=int(hello[2],16)
            msg=f"MODE8:{nonce:08X}:{self.boot:08X}".encode("ascii")
            mac=hmac.new(self.key,msg,hashlib.sha256).hexdigest()
            self.send("AUTH "+mac)
            if self.readline()!="AUTH_OK":
                raise TransportError("authentication rejected")
            return self
        except BaseException:
            self.__exit__()
            raise

    def __exit__(self,*_):
        if self.stream:self.stream.close()
        if self.sock:self.sock.close()

    def send(self,command:str):
        if "\n" in command or "\r" in command or len(command)>180:
            raise ValueError("invalid command")
        self.sock.sendall((command+"\n").encode("ascii"))

    def readline(self):
        result=self.stream.readline(4096)
        if not result or len(result)>=4096 or not result.endswith(b"\n"):
            raise TransportError("connection closed or response line incomplete or oversized")
        return result.rstrip(b"\r\n").decode("utf-8")

    def command(self,command:str):
        self.send(command)
        return self.readline()

    def request_start(self, session: int, count: int) -> tuple[str, str]:
        response = self.command(f"START {session} {count}")
        fields = response.split(" ")
        if (len(fields) != 3 or fields[:2] != ["START_READY", str(session)]
                or len(fields[2]) != 8 or any(c not in "0123456789ABCDEF" for c in fields[2])
                or fields[2] == "00000000"):
            raise TransportError(f"invalid START offer: {response!r}")
        return response, fields[2]

    def acknowledge_start(self, session: int, nonce: str) -> None:
        # The anchor switches Wi-Fi off after consuming this echo. Its next
        # authenticated STATUS carries the anchor-side confirmation.
        self.send(f"START_ACK {session} {nonce}")

    def custody_proof(self,verb:str,session:int,body_crc:int)->str:
        if self.approval_key is None or self.nonce is None:
            raise TransportError("separate custody key required")
        if not (1<=session<=0xffffffff and 0<=body_crc<=0xffffffff):
            raise TransportError("invalid custody session/CRC")
        message=f"MODE8-CUSTODY:{verb}:{self.boot:08X}:{session:08X}:{body_crc:08X}:{self.nonce:08X}"
        return hmac.new(self.approval_key,message.encode("ascii"),hashlib.sha256).hexdigest()

    def acknowledge_archive(self, session: int, body_crc: int) -> str:
        """Only call after complete tag+anchor paired RF audit."""
        proof=self.custody_proof("ACK",session,body_crc)
        return self.command(f"ACKFILE {session} {body_crc} {proof}")

    def purge_archive(self, session:int,body_crc:int,boot:int,*,before_send=None)->str:
        if self.boot!=boot:
            raise TransportError("anchor rebooted before purge")
        status=json.loads(self.command("STATUS"))
        if (status.get("boot")!=boot or status.get("file") is not True or
            status.get("file_session")!=session or status.get("file_crc")!=body_crc or
            status.get("file_boot")!=boot or status.get("file_acked") is not True):
            raise TransportError("remote identity/ACK does not match local verified file")
        proof=self.custody_proof("PURGE",session,body_crc)
        if before_send is not None:
            before_send()
        return self.command(f"PURGE {session} {body_crc} {proof}")

    def get_archive(self,path:Path,expect_session:int|None=None,ack:bool=False):
        # Saving an anchor file is not a paired RF verification. Do not let
        # the standalone GET shortcut authorize a remote deletion.
        if ack:
            raise TransportError("unpaired ACK forbidden: use verified ops_session")
        self.send("GET")
        header=self.readline().split()
        if len(header)!=4 or header[0]!="DATA":
            raise TransportError(f"GET rejected: {header}")
        session,size,body_crc=map(int,header[1:])
        if expect_session is not None and session!=expect_session:
            raise TransportError("unexpected session")
        if not 32<=size<=MAX_FILE_BYTES:
            raise TransportError("invalid archive length")
        buffer=bytearray()
        # Retain interrupted evidence and use a fresh file on each retry.
        with tempfile.NamedTemporaryFile(mode="wb", dir=path.parent,
                                         prefix=path.name+".", suffix=".partial",
                                         delete=False) as file:
            temp=Path(file.name)
            remaining=size
            while remaining:
                chunk=self.stream.read(min(4096,remaining))
                if not chunk:raise TransportError("incomplete archive stream")
                buffer.extend(chunk);file.write(chunk);remaining-=len(chunk)
            file.flush();os.fsync(file.fileno())
        if self.readline()!="END":
            raise TransportError("archive end marker missing")
        try:
            doc=parse(bytes(buffer))
        except ArchiveError as e:
            raise TransportError(f"invalid archive: {e}") from e
        if (doc["session"],doc["crc32"])!=(session,body_crc):
            raise TransportError("transfer identity/CRC mismatch")
        try:
            # A hard link atomically publishes the synced bytes without
            # overwriting a concurrent or previously completed capture.
            os.link(temp,path)
        except FileExistsError:
            if path.read_bytes()!=buffer:
                raise TransportError(f"archive differs from existing file: {path}")
        temp.unlink()
        if os.name=="posix":
            # On Pi, also persist the directory entry before authorizing purge.
            directory=os.open(path.parent,os.O_RDONLY|os.O_DIRECTORY)
            try:
                os.fsync(directory)
            finally:
                os.close(directory)
        return doc

def main():
    a=argparse.ArgumentParser(description=__doc__)
    a.add_argument("--host",required=True)
    a.add_argument("--port",type=int,default=29310)
    a.add_argument("--key-env",default="MODE8_AUTH_KEY_HEX")
    a.add_argument("--approval-key-env",default="MODE8_APPROVAL_KEY_HEX")
    sub=a.add_subparsers(dest="action",required=True)
    sub.add_parser("status")
    start=sub.add_parser("start");start.add_argument("session",type=int);start.add_argument("count",type=int)
    get=sub.add_parser("get");get.add_argument("path",type=Path);get.add_argument("--session",type=int)
    # CLI GET never ACKs: only the paired controller runs the cross-endpoint audit.
    purge=sub.add_parser("purge")
    purge.add_argument("--archive",type=Path,required=True)
    purge.add_argument("--receipt",type=Path,required=True)
    purge.add_argument("--tag",type=Path,required=True)
    ota=sub.add_parser("ota");ota.add_argument("seconds",type=int)
    args=a.parse_args()
    secret=os.environ.get(args.key_env,"")
    try:key=bytes.fromhex(secret)
    except ValueError:raise SystemExit("invalid key hex")
    if len(key)!=32:raise SystemExit(f"Set {args.key_env} to the 64-hex-character provisioned HMAC key")
    approval=None
    if args.action=="purge":
        try:approval=bytes.fromhex(os.environ.get(args.approval_key_env,""))
        except ValueError:raise SystemExit("invalid custody key")
        if len(approval)!=32 or approval==key:
            raise SystemExit("distinct MODE8_APPROVAL_KEY_HEX required for purge")
    with OperationalClient(args.host,key,args.port,approval_key=approval) as ops:
        if args.action=="status": print(ops.command("STATUS"))
        elif args.action=="start":
            if not (1<=args.session<=0xFFFFFFFF and 1<=args.count<=32):raise SystemExit("invalid count/session")
            if json.loads(ops.command("STATUS")).get("start_ack_protocol") != 1:
                raise TransportError("Anchor does not advertise confirmed START")
            reply, nonce = ops.request_start(args.session, args.count)
            ops.acknowledge_start(args.session, nonce)
            print(reply)
        elif args.action=="get":
            doc=ops.get_archive(args.path,args.session,ack=False)
            print(f"Saved and verified {doc['count']} anchor records from session {doc['session']}")
        elif args.action=="purge":
            from ops_route import check_point_file_hashes, verify_point_files
            doc = parse(args.archive.read_bytes())
            proof = verify_point_files(args.receipt, args.archive, args.tag,
                                       doc["session"], doc["count"], 200000)
            reply = ops.purge_archive(doc["session"], proof["archive_crc"], proof["anchor_boot"],
                                      before_send=lambda: check_point_file_hashes(
                                          args.receipt, args.archive, args.tag, proof))
            if reply != "PURGE_OK":
                raise SystemExit(f"Anchor rejected purge: {reply}")
            check_point_file_hashes(args.receipt, args.archive, args.tag, proof)
            print(reply)
        elif args.action=="ota":print(ops.command(f"OTA {args.seconds}"))

if __name__=="__main__":
    main()
