"""Build-to-source binding for Mode-8 release artifacts (standard library only)."""
from __future__ import annotations
from pathlib import Path
import hashlib
import json


def inputs(root: Path, role: str) -> list[Path]:
    if role not in ('anchor', 'tag'):
        raise ValueError('unknown role')
    relative = ['shared', f'firmware/{role.upper()}_DUAL_DS_CIR', f'vendor/{role}/DW1000Ng']
    paths: list[Path] = []
    for dirname in relative:
        folder = root / dirname
        for file in folder.rglob('*'):
            if file.is_file() and file.suffix.lower() in ('.cpp', '.h', '.hpp', '.ino', '.c', '.s', '.S'):
                paths.append(file)
    if not paths:
        raise ValueError('compile inputs missing')
    return sorted(paths, key=lambda p: p.relative_to(root).as_posix())


def manifest(root: Path, role: str) -> dict:
    hashes = {p.relative_to(root).as_posix(): hashlib.sha256(p.read_bytes()).hexdigest()
              for p in inputs(root, role)}
    aggregate = hashlib.sha256(json.dumps(hashes, sort_keys=True,
                                          separators=(',', ':')).encode()).hexdigest()
    return {'sha256': aggregate, 'files': hashes}


def matches(root: Path, role: str, expected: dict) -> bool:
    return (isinstance(expected, dict) and
            expected == manifest(root, role))
