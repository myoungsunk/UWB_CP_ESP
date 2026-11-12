from __future__ import annotations

import argparse
import hashlib
import importlib.util
import json
from pathlib import Path
import sys

ROOT = Path(__file__).resolve().parent
ROLES = ("anchor", "tag")
RUNS = ("01_final_pass", "02_p2_repeat_recovery")


def load(path):
    return json.loads(path.read_bytes())


def sha256(path):
    with path.open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest() if hasattr(hashlib, "file_digest") else hash_stream(stream)


def hash_stream(stream):
    digest = hashlib.sha256()
    for block in iter(lambda: stream.read(1024 * 1024), b""):
        digest.update(block)
    return digest.hexdigest()


def require(condition, message):
    if not condition:
        raise ValueError(message)


def verify_builds():
    for role in ROLES:
        firmware = ROOT / "firmware" / f"{role}_p2_repeat"
        receipt = load(firmware / "dist" / "target_build_receipt.json")
        spec = importlib.util.spec_from_file_location("source_contract", firmware / "tools" / "source_contract.py")
        contract = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(contract)
        build = receipt["roles"][role]
        require(receipt["target_build"], f"{role}: target build failed")
        require(contract.manifest(firmware, role) == build["source_manifest"], f"{role}: build source mismatch")
        for name, info in build["artifacts"].items():
            path = firmware / "dist" / role / name
            require(path.stat().st_size == info["bytes"] and sha256(path) == info["sha256"], f"{role}: build artifact mismatch {name}")
        print(f"{role}: ESP32 build source and artifacts verified")


def verify_capture_files(run):
    directory = run / "capture"
    receipt_path = directory / "capture_receipt.json"
    execution = load(run / "execution.json")
    require(sha256(receipt_path) == execution["capture_receipt_sha256"], f"{run.name}: receipt mismatch")
    receipt = load(receipt_path)
    for name, digest in receipt["file_sha256"].items():
        require(sha256(directory / name) == digest, f"{run.name}: capture file mismatch {name}")
    return receipt


def replay(run, receipt):
    sys.path.insert(0, str(ROOT / "tools" / "p2_repeat_tools"))
    from capture_audit import stage6_accounting, verify_capture
    from run_pair import cir_payload_integrity, read_file, stage6_grids

    directory = run / "capture"
    session = receipt["session"]
    count = receipt["requested_count"]
    records = {}
    for role in ROLES:
        records[role] = [
            dict(packet.meta, _raw_integrity_verified=cir_payload_integrity(packet.meta, packet.raw))
            for packet in read_file(directory / f"{role}.rfb8")
            if packet.kind == 2 and packet.meta.get("session") == session
        ]
    audit = verify_capture(session=session, count=count, stage=receipt["stage"], period_us=receipt["period_us"],
                           identities={role: receipt[f"{role}_identity"] for role in ROLES}, records=records,
                           stats={role: receipt[f"{role}_capture"] for role in ROLES})
    require(audit.passed == receipt["audit"]["pass"], f"{run.name}: audit verdict mismatch")
    require(list(audit.problems) == receipt["audit"]["problems"], f"{run.name}: audit problems mismatch")
    grids = stage6_grids(directory, session, records["anchor"])
    saved = receipt["stage6"]
    require(grids == saved["common_axis_and_power"], f"{run.name}: grid counts mismatch")
    require(stage6_accounting(receipt, records) == {key: value for key, value in saved.items()
                                                  if key != "common_axis_and_power"}, f"{run.name}: accounting mismatch")
    print(f"{run.name}: audit={audit.passed}, P1={grids['P1']}/{count}, P2={grids['P2']}/{count}")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--replay", action="store_true", help="replay both recorded sessions offline (NumPy required)")
    args = parser.parse_args()
    expected = load(ROOT / "SHA256SUMS.json")
    actual = {path.relative_to(ROOT).as_posix() for path in ROOT.rglob("*")
              if path.is_file() and path != ROOT / "SHA256SUMS.json"
              and "__pycache__" not in path.parts}
    require(actual == set(expected), "package file list mismatch")
    for name, digest in expected.items():
        require(sha256(ROOT / name) == digest, f"package hash mismatch {name}")
    print(f"{len(expected)} package file hashes verified")
    verify_builds()
    for name in RUNS:
        run = ROOT / "logs" / name
        receipt = verify_capture_files(run)
        if args.replay:
            replay(run, receipt)
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except (OSError, ValueError, ImportError) as error:
        sys.exit(str(error))
