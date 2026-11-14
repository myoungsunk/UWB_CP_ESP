"""Replay frozen paired captures without changing their files."""
import argparse
from collections import Counter
import hashlib
import json
from pathlib import Path
import sys

package = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(package / "firmware/anchor_p2_repeat/tools"))
from analyze import packet_arrays
from capture_audit import stage6_accounting, verify_capture
from rfb8 import Decoder, read_file
from run_pair import cir_payload_integrity

def verify_directory(directory):
    receipt = json.loads((directory / "capture_receipt.json").read_text(encoding="utf-8"))
    hashes = receipt.get("file_sha256", {})
    if not {"anchor.rfb8", "tag.rfb8"}.issubset(hashes):
        raise ValueError("receipt lacks paired RFB8 digests")
    for name, expected in hashes.items():
        if Path(name).name != name:
            raise ValueError(f"unexpected digest path: {name}")
        with (directory/name).open("rb") as stream:
            actual = hashlib.file_digest(stream, "sha256").hexdigest()
        if actual != expected:
            raise ValueError(f"capture digest mismatch: {name}")
    records, packets, decoders = {}, {}, {}
    for role in ("anchor", "tag"):
        decoder = Decoder()
        packets[role] = [p for p in read_file(directory/f"{role}.rfb8", decoder) if p.kind == 2]
        records[role] = [p.meta | {"_raw_integrity_verified": cir_payload_integrity(p.meta, p.raw)}
                         for p in packets[role]]
        decoders[role] = decoder.statistics()
    count = receipt["requested_count"]
    audit = verify_capture(session=receipt["session"], count=count, stage=receipt["stage"],
                           period_us=receipt["period_us"], identities={r: receipt[f"{r}_identity"] for r in records},
                           records=records, stats={r: receipt[f"{r}_capture"] for r in records})
    recalc = receipt | {"audit": {"pass": audit.passed, "problems": audit.problems, "details": audit.details}}
    account = stage6_accounting(recalc, records, legacy_shutdown=receipt["shutdown"].get("contract") != "drain_v1")
    anchors = {(m["session"], m["txn"], m["peer_boot"], m["boot"]): m for m in records["anchor"]}
    result = Counter()
    missing = {}
    for p in packets["tag"]:
        m = p.meta
        a = anchors.get((m["session"], m["txn"], m["boot"], m["peer_boot"]))
        arrays, rows = packet_arrays(m, p.raw, a)
        for probe in (1, 2):
            delay = len(arrays[f"common_p{probe}_delay_s"])
            result[f"P{probe}_axis"] += bool(delay)
            result[f"P{probe}_power_grid"] += bool(delay and arrays[f"common_p{probe}_power_norm"].size)
            if not delay:
                missing.setdefault(f"P{probe}", []).append(m["txn"])
        for row in rows:
            key = f"P{row['probe']}{row['radio']}"
            result[f"{key}_cir"] += bool(row["cir_valid"])
            result[f"{key}_axis"] += bool(row["axis_estimate_available"])
    return {"session": receipt["session"], "captured_pass": receipt["pass"], "capture_audit": recalc["audit"],
            "raw_sha256": hashes, "counts": {r: len(rec) for r,rec in records.items()}, "decoders": decoders,
            "recalculated": dict(result), "missing_axis_txns": missing, "accounting": account}

def main():
    cli = argparse.ArgumentParser(description=__doc__)
    cli.add_argument("--out", type=Path, required=True, help="New JSON evidence file")
    args = cli.parse_args()
    if args.out.exists():
        cli.error(f"output already exists: {args.out}")

    summary = {}
    failed = False
    for folder in ("01_final_pass", "02_p2_repeat_recovery"):
        try:
            result = verify_directory(package / "logs" / folder / "capture")
            if result["captured_pass"] != result["capture_audit"]["pass"]:
                failed = True
                result["replay_error"] = "recalculated verdict differs from the capture receipt"
            summary[folder] = result
        except (OSError, ValueError, KeyError) as error:
            summary[folder] = {"replay_error": repr(error)}
            failed = True
        print(folder, summary[folder].get("recalculated"), summary[folder].get("replay_error"))

    with args.out.open("x", encoding="utf-8") as destination:
        json.dump(summary, destination, indent=2)
        destination.write("\n")
    return int(failed)


if __name__ == "__main__":
    sys.exit(main())
