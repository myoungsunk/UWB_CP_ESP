#!/usr/bin/env python3
from pathlib import Path
import argparse
import shutil
ROOT = Path(__file__).resolve().parents[1]

def sync(check: bool = False) -> None:
    for role in ("ANCHOR", "TAG"):
        target = ROOT/"firmware"/f"{role}_DUAL_DS_CIR"
        for src in (ROOT/"shared").glob("*.hpp"):
            dst = target/src.name
            if check:
                if not dst.exists() or dst.read_bytes() != src.read_bytes():
                    raise RuntimeError(f"Shared-header mismatch: {dst}")
            else:
                shutil.copyfile(src, dst)

if __name__ == "__main__":
    p = argparse.ArgumentParser()
    p.add_argument("--check", action="store_true")
    sync(p.parse_args().check)
