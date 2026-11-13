#!/usr/bin/env python3
"""Authenticated anchor TCP control and durable archive retrieval.

USB tag and anchor measurement must be prepared by the existing Mode 8
pair collector. Never start RF from two independent controllers.
"""
from __future__ import annotations
import argparse
import hashlib
import hmac
import os
from pathlib import Path
import socket
import sys

from ops_archive import parse, ArchiveError

class TransportError(RuntimeError):
    pass

class OperationalClient:
    def __init__(self,host:str,key:bytes,port:int=29310,timeout:float=10):
        if len(key)!=32: raise ValueError("AUTH key requires exactly 32 bytes")
        self.address=(host,port);self.key=key;self.timeout=timeout
        self.sock=None;self.stream=None

    def __enter__(self):
        self.sock=socket.create_connection(self.address,self.timeout)
        self.sock.settimeout(self.timeout)
        self.stream=self.sock.makefile("rb")
        hello=self.readline().split()
        if len(hello)!=3 or hello[0]!="CHALLENGE":
            raise TransportError(f"missing challenge: {hello!r}")
        nonce=int(hello[1],16);boot=int(hello[2],16)
        msg=f"MODE8:{nonce:08X}:{boot:08X}".encode("ascii")
        mac=hmac.new(self.key,msg,hashlib.sha256).hexdigest()
        self.send("AUTH "+mac)
        if self.readline()!="AUTH_OK":
            raise TransportError("authentication rejected")
        return self

    def __exit__(self,*_):
        if self.stream:self.stream.close()
        if self.sock:self.sock.close()

    def send(self,command:str):
        if "\n" in command or "\r" in command or len(command)>180:
            raise ValueError("invalid command")
        self.sock.sendall((command+"\n").encode("ascii"))

    def readline(self):
        result=self.stream.readline(4096)
        if not result or len(result)>=4096:
            raise TransportError("connection closed or oversized response")
        return result.rstrip(b"\r\n").decode("utf-8")

    def command(self,command:str):
        self.send(command)
        return self.readline()

    def get_archive(self,path:Path,expect_session:int|None=None,ack:bool=False):
        if path.exists():
            raise FileExistsError(path)
        temp=path.with_suffix(path.suffix+".partial")
        if temp.exists():
            raise FileExistsError(temp)
        self.send("GET")
        header=self.readline().split()
        if len(header)!=4 or header[0]!="DATA":
            raise TransportError(f"GET rejected: {header}")
        session,size,body_crc=map(int,header[1:])
        if expect_session is not None and session!=expect_session:
            raise TransportError("unexpected session")
        if not (32<=size<=32+32*1070):
            raise TransportError("invalid archive length")
        buffer=bytearray()
        with temp.open("xb") as file:
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
        temp.replace(path)
        if ack:
            answer=self.command(f"ACKFILE {session}")
            if answer!="ACKFILE_OK":
                raise TransportError("file is saved but remote ACKFILE failed")
        return doc

def main():
    a=argparse.ArgumentParser(description=__doc__)
    a.add_argument("--host",required=True)
    a.add_argument("--port",type=int,default=29310)
    a.add_argument("--key-env",default="MODE8_AUTH_KEY_HEX")
    sub=a.add_subparsers(dest="action",required=True)
    sub.add_parser("status")
    start=sub.add_parser("start");start.add_argument("session",type=int);start.add_argument("count",type=int)
    get=sub.add_parser("get");get.add_argument("path",type=Path);get.add_argument("--session",type=int);get.add_argument("--ack",action="store_true")
    sub.add_parser("purge")
    ota=sub.add_parser("ota");ota.add_argument("seconds",type=int)
    args=a.parse_args()
    secret=os.environ.get(args.key_env,"")
    try:key=bytes.fromhex(secret)
    except ValueError:raise SystemExit("invalid key hex")
    if len(key)!=32:raise SystemExit(f"Set {args.key_env} to the 64-hex-character provisioned HMAC key")
    with OperationalClient(args.host,key,args.port) as ops:
        if args.action=="status": print(ops.command("STATUS"))
        elif args.action=="start":
            if not (1<=args.session<=0xFFFFFFFF and 1<=args.count<=32):raise SystemExit("invalid count/session")
            print(ops.command(f"START {args.session} {args.count}"))
        elif args.action=="get":
            doc=ops.get_archive(args.path,args.session,args.ack)
            print(f"Saved and verified {doc['count']} anchor records from session {doc['session']}")
        elif args.action=="purge":print(ops.command("PURGE"))
        elif args.action=="ota":print(ops.command(f"OTA {args.seconds}"))

if __name__=="__main__":
    main()
