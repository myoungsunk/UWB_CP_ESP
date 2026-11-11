"""Build-to-source binding for Mode-8 release artifacts (standard library only)."""
from __future__ import annotations
from pathlib import Path
import hashlib
import json


def inputs(root: Path, role: str) -> list[Path]:
    if role not in ('anchor', 'tag'):
        raise ValueError('unknown role')
    sketch = root / 'firmware' / f'{role.upper()}_DUAL_DS_CIR'
    generated = sketch / 'build'
    paths: list[Path] = []
    for folder in (root / 'shared', sketch, root / 'vendor' / role / 'DW1000Ng'):
        if not folder.is_dir():
            raise ValueError(f'compile inputs missing: {folder}')
        for file in folder.rglob('*'):
            # Arduino CLI exports images to the sketch's top-level build directory.
            # Keep every package input, including partitions.csv and bootloader.bin.
            if file != generated and generated not in file.parents and file.is_file():
                paths.append(file)
    if not paths:
        raise ValueError('compile inputs missing')
    return sorted(paths, key=lambda p: p.relative_to(root).as_posix())


def manifest(root: Path, role: str) -> dict:
    hashes = {p.relative_to(root).as_posix(): hashlib.sha256(p.read_bytes()).hexdigest()
              for p in inputs(root, role)}
    aggregate = hashlib.sha256(json.dumps(hashes, sort_keys=True,
                                          separators=(',', ':')).encode()).hexdigest()
    return {'version': 2, 'sha256': aggregate, 'files': hashes}


def matches(root: Path, role: str, expected: dict) -> bool:
    if not isinstance(expected, dict) or expected.get('version') != 2:
        return False
    try:
        return expected == manifest(root, role)
    except (OSError, ValueError):
        return False
