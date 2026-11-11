#!/usr/bin/env python3
"""Verify the delivered source/evidence snapshot, not ESP32 build success."""
from __future__ import annotations
import hashlib
import json
from pathlib import Path
import sys

ROOT = Path(__file__).resolve().parents[1]

def main() -> int:
    manifest = ROOT / "PACKAGE_SHA256.json"
    if not manifest.exists():
        print("Missing PACKAGE_SHA256.json", file=sys.stderr)
        return 2
    data = json.loads(manifest.read_text(encoding="utf-8"))
    failures = []
    for name, item in data["files"].items():
        path = (ROOT / name).resolve()
        if ROOT not in path.parents or not path.is_file():
            failures.append({"file": name, "reason": "missing or unsafe path"})
            continue
        raw = path.read_bytes()
        if len(raw) != item["bytes"] or hashlib.sha256(raw).hexdigest() != item["sha256"]:
            failures.append({"file": name, "reason": "content differs"})
    print(json.dumps({"pass": not failures, "checked": len(data["files"]),
                      "scope": "DELIVERED_SNAPSHOT_INTEGRITY_ONLY",
                      "failures": failures}, indent=2))
    return 1 if failures else 0

if __name__ == "__main__":
    sys.exit(main())
