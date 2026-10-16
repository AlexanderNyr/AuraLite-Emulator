#!/usr/bin/env python3
"""Replace only the reserved Stage-2 slot in the hash-pinned release ISO."""
from __future__ import annotations
import argparse
import hashlib
from pathlib import Path
import shutil

PINNED_ISO_SHA256 = "d2110f8ba93f739a0ffe1180540fe456a5b2396c3fad375cb7504cb11d8144e7"
LBA = 34
SECTOR = 512
SECTORS = 126
SLOT = SECTOR * SECTORS

def digest(path: Path) -> str:
    h = hashlib.sha256()
    with path.open("rb") as f:
        for block in iter(lambda: f.read(1024 * 1024), b""):
            h.update(block)
    return h.hexdigest()

def main() -> None:
    p = argparse.ArgumentParser()
    p.add_argument("pinned_iso", type=Path)
    p.add_argument("stage2_bin", type=Path)
    p.add_argument("output_iso", type=Path)
    a = p.parse_args()
    source_hash = digest(a.pinned_iso)
    if source_hash != PINNED_ISO_SHA256:
        raise SystemExit(f"pinned ISO hash mismatch: {source_hash}")
    stage2 = a.stage2_bin.read_bytes()
    if len(stage2) > SLOT:
        raise SystemExit(f"Stage 2 too large: {len(stage2)} > {SLOT}")
    start = LBA * SECTOR
    with a.pinned_iso.open("rb") as f:
        f.seek(start)
        original_slot = f.read(SLOT)
    if len(original_slot) != SLOT or any(original_slot[len(stage2):]):
        raise SystemExit("unexpected/nonzero reserved Stage-2 slot tail")
    shutil.copyfile(a.pinned_iso, a.output_iso)
    with a.output_iso.open("r+b") as f:
        f.seek(start)
        f.write(stage2)
        f.write(bytes(SLOT - len(stage2)))
    print(f"input_sha256={source_hash}")
    print(f"stage2_bytes={len(stage2)} stage2_sha256={hashlib.sha256(stage2).hexdigest()}")
    print(f"output_bytes={a.output_iso.stat().st_size} output_sha256={digest(a.output_iso)}")
    print(f"replaced_lba={LBA} sectors={SECTORS}")

if __name__ == "__main__":
    main()
