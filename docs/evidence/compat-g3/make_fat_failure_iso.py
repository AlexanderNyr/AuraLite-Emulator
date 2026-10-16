#!/usr/bin/env python3
"""Create an INT 13h error-injection copy of the Stage-2 diagnostic ISO.

The input must be the exact temporary ISO built with the candidate Stage 2.
Only the KERNEL.ELF first-cluster FAT link is changed, in both FAT copies,
to a bad/out-of-range cluster so the following AHCI read fails. The pinned
release ISO is never opened for writing.
"""
from __future__ import annotations
import argparse
import hashlib
from pathlib import Path
import shutil
import struct

INPUT_SHA256 = "b9d650b93a3894be4c5ea0ddcf8f3cd1bbae9958a00fc0743ee5aef56d0bb905"
SECTOR = 512
KERNEL_NAME = b"KERNEL  ELF"
BAD_CLUSTER = 0x0FFFFFF7


def digest(path: Path) -> str:
    h = hashlib.sha256()
    with path.open("rb") as f:
        for block in iter(lambda: f.read(1024 * 1024), b""):
            h.update(block)
    return h.hexdigest()


def read_at(f, off: int, n: int) -> bytes:
    f.seek(off)
    b = f.read(n)
    if len(b) != n:
        raise SystemExit(f"short read at {off:#x}: wanted {n}, got {len(b)}")
    return b


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("candidate_iso", type=Path)
    ap.add_argument("output_iso", type=Path)
    a = ap.parse_args()
    if a.candidate_iso.resolve() == a.output_iso.resolve():
        raise SystemExit("input and output paths must differ")
    src_hash = digest(a.candidate_iso)
    if src_hash != INPUT_SHA256:
        raise SystemExit(f"candidate ISO hash mismatch: {src_hash}")

    with a.candidate_iso.open("rb") as f:
        part = None
        for i in range(4):
            p = read_at(f, 0x1BE + 16 * i, 16)
            typ = p[4]
            start, count = struct.unpack_from("<II", p, 8)
            if typ in (0x0B, 0x0C):
                part = (start, count)
                break
        if part is None:
            raise SystemExit("FAT32 MBR partition not found")
        part_lba, part_sectors = part
        bpb = read_at(f, part_lba * SECTOR, SECTOR)
        if bpb[510:512] != b"\x55\xaa":
            raise SystemExit("FAT32 BPB sector signature mismatch")
        bps = struct.unpack_from("<H", bpb, 11)[0]
        spc = bpb[13]
        reserved = struct.unpack_from("<H", bpb, 14)[0]
        nfats = bpb[16]
        fatsz = struct.unpack_from("<I", bpb, 36)[0]
        root_cluster = struct.unpack_from("<I", bpb, 44)[0]
        if bps != SECTOR or not spc or nfats != 2 or not fatsz:
            raise SystemExit("unexpected FAT32 geometry")
        if part_sectors * SECTOR > a.candidate_iso.stat().st_size - part_lba * SECTOR:
            raise SystemExit("FAT32 partition extends beyond candidate ISO")
        fat_lba = part_lba + reserved
        data_lba = fat_lba + nfats * fatsz

        def fat_value(cluster: int, copy: int = 0) -> int:
            off = (fat_lba + copy * fatsz) * SECTOR + cluster * 4
            return struct.unpack("<I", read_at(f, off, 4))[0] & 0x0FFFFFFF

        # Locate the root-directory entry, following the root FAT chain if needed.
        cluster = root_cluster
        seen: set[int] = set()
        kernel_first = None
        while cluster < 0x0FFFFFF8 and cluster not in seen:
            seen.add(cluster)
            cluster_off = (data_lba + (cluster - 2) * spc) * SECTOR
            data = read_at(f, cluster_off, spc * SECTOR)
            for off in range(0, len(data), 32):
                e = data[off:off + 32]
                if e[0] == 0:
                    break
                if e[0] == 0xE5 or e[11] == 0x0F or e[11] & 0x08 or e[11] & 0x10:
                    continue
                if e[:11] == KERNEL_NAME:
                    kernel_first = ((struct.unpack_from("<H", e, 20)[0] << 16)
                                    | struct.unpack_from("<H", e, 26)[0])
                    break
            if kernel_first is not None:
                break
            cluster = fat_value(cluster)
        if kernel_first is None:
            raise SystemExit("KERNEL.ELF root entry not found")
        old_values = [fat_value(kernel_first, c) for c in range(nfats)]
        if any(v != 6 for v in old_values):
            raise SystemExit(f"unexpected KERNEL.ELF first link: {old_values}")

    shutil.copyfile(a.candidate_iso, a.output_iso)
    offsets = []
    with a.output_iso.open("r+b") as f:
        for copy in range(nfats):
            off = (fat_lba + copy * fatsz) * SECTOR + kernel_first * 4
            with a.candidate_iso.open("rb") as src:
                old_raw = struct.unpack("<I", read_at(src, off, 4))[0]
            new_raw = (old_raw & 0xF0000000) | BAD_CLUSTER
            f.seek(off)
            f.write(struct.pack("<I", new_raw))
            offsets.append(off)

    with a.output_iso.open("rb") as f:
        for off in offsets:
            raw = struct.unpack("<I", read_at(f, off, 4))[0]
            if (raw & 0x0FFFFFFF) != BAD_CLUSTER:
                raise SystemExit("FAT error injection did not stick")
    print(f"input_sha256={src_hash}")
    print(f"partition_lba={part_lba} fat_lba={fat_lba} fats={nfats} fatsz={fatsz}")
    print(f"kernel_first_cluster={kernel_first} old_next={old_values} bad_next={BAD_CLUSTER:#x}")
    print(f"changed_offsets={[hex(o) for o in offsets]}")
    print(f"output_bytes={a.output_iso.stat().st_size} output_sha256={digest(a.output_iso)}")


if __name__ == "__main__":
    main()
