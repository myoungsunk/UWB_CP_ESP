"""Restore original paths for byte-identical files after extracting the compact ZIP."""
import csv
from hashlib import sha256
from pathlib import Path, PurePosixPath
import shutil

ROOT = Path(__file__).resolve().parent


def checked_path(name):
    parts = PurePosixPath(name).parts
    if not parts or name.startswith("/") or any(x in {".", ".."} or ":" in x or "\\" in x for x in parts):
        raise ValueError(f"Unsafe archive path {name!r}")
    path = ROOT.joinpath(*parts)
    if not path.resolve().is_relative_to(ROOT):
        raise ValueError(f"Path outside extracted archive: {name}")
    return path


def digest(path):
    with path.open("rb") as file:
        h = sha256()
        while data := file.read(1024 * 1024):
            h.update(data)
    return h.hexdigest()


def main():
    done = 0
    checked = {}
    with (ROOT/"DUPLICATE_PATHS.csv").open(encoding="utf-8-sig", newline="") as file:
        for row in csv.DictReader(file):
            target, original = (checked_path(row[k]) for k in ("original_path","stored_at"))
            expected = row["sha256"]
            if original not in checked:
                checked[original] = digest(original)
            if checked[original] != expected:
                raise ValueError(f"Stored file checksum mismatch: {original}")
            if target.exists():
                if digest(target) != expected:
                    raise FileExistsError(f"Existing file has different bytes: {target}")
                continue
            target.parent.mkdir(parents=True, exist_ok=True)
            with original.open("rb") as src, target.open("xb") as dst:
                shutil.copyfileobj(src,dst)
            done += 1
    print(f"Restored {done} byte-identical original paths.")


if __name__ == "__main__":
    main()

