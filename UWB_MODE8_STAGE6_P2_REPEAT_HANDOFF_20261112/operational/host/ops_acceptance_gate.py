#!/usr/bin/env python3
"""Fail-closed G0-G12 field acceptance receipt verifier (does not run hardware).

Requires SHA-256 of immutable evidence *bytes*, not handwritten PASS labels.
Missing stages, invalid hashes, inconsistent source/build identity => HOLD.
"""
from __future__ import annotations
import argparse
import hashlib
import json
from pathlib import Path
import re

GATES=tuple(f"G{i}" for i in range(13))
HASH=re.compile(r"^[a-f0-9]{64}$")
GIT=re.compile(r"^[a-f0-9]{40}$")

class AcceptanceHold(ValueError):pass

def file_digest(path):
    h=hashlib.sha256()
    with path.open("rb") as f:
        for chunk in iter(lambda:f.read(65536),b""):
            h.update(chunk)
    return h.hexdigest()

def verify_evidence(root,items):
    if not isinstance(items,list) or not items:
        raise AcceptanceHold("PASS without a nonempty evidence list")
    for item in items:
        if not isinstance(item,dict) or set(item)!={"path","sha256"}:
            raise AcceptanceHold("evidence needs path and SHA-256")
        relative=item["path"]
        if not isinstance(relative,str) or not relative or Path(relative).is_absolute():
            raise AcceptanceHold("invalid evidence path")
        p=(root/relative).resolve()
        if not p.is_relative_to(root) or not p.is_file():
            raise AcceptanceHold("missing or escaping evidence file")
        if not isinstance(item["sha256"],str) or not HASH.fullmatch(item["sha256"]):
            raise AcceptanceHold("invalid evidence SHA256 syntax")
        if file_digest(p)!=item["sha256"]:
            raise AcceptanceHold("evidence SHA256 does not match stored bytes")

def assess(doc, root:Path, expected_git:str):
    root=root.resolve()
    if not GIT.fullmatch(expected_git):
        raise AcceptanceHold("expected exact Git SHA must be 40 hex digits")
    if doc.get("git_sha")!=expected_git:
        raise AcceptanceHold("source commit differs from requested acceptance baseline")
    for name in ("anchor_bin_sha256","tag_bin_sha256"):
        if not isinstance(doc.get(name),str) or not HASH.fullmatch(doc[name]):
            raise AcceptanceHold("source-bound flashed image hash missing: "+name)
    tests=doc.get("tests")
    if not isinstance(tests,list):
        raise AcceptanceHold("tests must be a list")
    groups={}
    for item in tests:
        if not isinstance(item,dict) or item.get("id") not in GATES:
            raise AcceptanceHold("unknown verification gate")
        if item["id"] in groups:
            raise AcceptanceHold("duplicate verification gate")
        groups[item["id"]]=item
    verdict={}
    earlier_ok=True
    for gate in GATES:
        item=groups.get(gate)
        if item is None or item.get("attempted") is not True:
            verdict[gate]="NOT_RUN"
            earlier_ok=False
            continue
        if item.get("pass") is not True:
            verdict[gate]="FAILED"
            earlier_ok=False
            continue
        try:
            verify_evidence(root,item.get("evidence"))
        except (AcceptanceHold,OSError) as error:
            verdict[gate]="INVALID_EVIDENCE: "+str(error)
            earlier_ok=False
            continue
        if not earlier_ok:
            verdict[gate]="BLOCKED_BY_PREVIOUS_GATE"
            continue
        verdict[gate]="PASS"
    evidence_complete=all(verdict[g]=="PASS" for g in GATES)
    # No collection of self-supplied files can independently certify a real
    # hardware run. Physical acceptance needs external authorized sign-off.
    return {"schema":"mode8.acceptance.v1","git_sha":expected_git,
            "source_host_build_candidate":all(verdict[g]=="PASS" for g in GATES[:5]),
            "field_evidence_complete":evidence_complete,
            "hardware_field_approved":False,
            "accuracy_calibration_deferred":True,
            "gates":verdict,
            "result":("EVIDENCE_READY_NEEDS_FIELD_SIGNOFF" if evidence_complete else "FIELD_HOLD")}

def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument("--manifest",required=True,type=Path)
    p.add_argument("--evidence-root",required=True,type=Path)
    p.add_argument("--expected-git-sha",required=True)
    p.add_argument("--out",required=True,type=Path)
    a=p.parse_args()
    if a.out.exists():p.error("output exists; no overwrite")
    try:
        report=assess(json.loads(a.manifest.read_text(encoding="utf-8")),
                      a.evidence_root,a.expected_git_sha)
        a.out.write_text(json.dumps(report,indent=2)+"\n",encoding="utf-8")
        print(json.dumps(report,indent=2))
        return 0 if report["hardware_field_approved"] else 2
    except (OSError,ValueError) as error:
        p.error("FIELD_HOLD: "+str(error))

if __name__=="__main__":
    raise SystemExit(main())
