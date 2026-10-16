#!/usr/bin/env python3
"""Compare compact boot_info_t captures from QEMU and x86emu G3 diagnostics."""
from __future__ import annotations
import argparse
from pathlib import Path
import struct

SIZE = 7776
MAGIC = 0x4155524142544C44
MMAP_OFF = 40
MMAP_COUNT_OFF = 6184
HHDM_OFF = 6192
INITRD_OFF = 6200
CPU_COUNT_OFF = 6216
BSP_OFF = 6220
CPUS_OFF = 6224
RSDP_OFF = 7760
UEFI_OFF = 7768


def load(path: Path) -> bytes:
    data = path.read_bytes()
    if len(data) != SIZE:
        raise SystemExit(f"{path}: expected {SIZE} bytes, got {len(data)}")
    magic = struct.unpack_from("<Q", data, 0)[0]
    if magic != MAGIC:
        raise SystemExit(f"{path}: bad BOOT_MAGIC {magic:#x}")
    mmap_count = struct.unpack_from("<I", data, MMAP_COUNT_OFF)[0]
    cpu_count = struct.unpack_from("<I", data, CPU_COUNT_OFF)[0]
    if mmap_count > 256 or cpu_count > 64:
        raise SystemExit(f"{path}: out-of-range counts mmap={mmap_count}, cpu={cpu_count}")
    return data


def fields(data: bytes) -> dict[str, object]:
    fb = struct.unpack_from("<QIII4B", data, 8)
    mmap_count = struct.unpack_from("<I", data, MMAP_COUNT_OFF)[0]
    mmap = [struct.unpack_from("<QQII", data, MMAP_OFF + i * 24)[:3]
            for i in range(mmap_count)]
    hhdm = struct.unpack_from("<Q", data, HHDM_OFF)[0]
    initrd = struct.unpack_from("<QQ", data, INITRD_OFF)
    cpu_count, bsp = struct.unpack_from("<II", data, CPU_COUNT_OFF)
    cpus = [struct.unpack_from("<IIQQ", data, CPUS_OFF + i * 24)
            for i in range(cpu_count)]
    rsdp = struct.unpack_from("<Q", data, RSDP_OFF)[0]
    uefi = data[UEFI_OFF]
    return {
        "magic": struct.unpack_from("<Q", data, 0)[0],
        "fb": fb,
        "mmap_count": mmap_count,
        "mmap": mmap,
        "hhdm": hhdm,
        "initrd": initrd,
        "cpu_count": cpu_count,
        "bsp": bsp,
        "cpus": cpus,
        "rsdp": rsdp,
        "uefi": uefi,
    }


def fmt_map(entries: list[tuple[int, int, int]]) -> str:
    return "; ".join(f"{b:#x}+{n:#x}:t{t}" for b, n, t in entries)


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("qemu_pinned", type=Path)
    ap.add_argument("qemu_candidate", type=Path)
    ap.add_argument("x86emu_candidate", type=Path)
    args = ap.parse_args()

    qemu_pinned = load(args.qemu_pinned)
    qemu_candidate = load(args.qemu_candidate)
    xemu_candidate = load(args.x86emu_candidate)
    q = fields(qemu_pinned)
    qc = fields(qemu_candidate)
    x = fields(xemu_candidate)

    print(f"QEMU pinned vs QEMU candidate: exact_block_match={qemu_pinned == qemu_candidate}")
    print(f"  bytes={SIZE}; pinned_sha256={__import__('hashlib').sha256(qemu_pinned).hexdigest()}")
    if qemu_pinned != qemu_candidate:
        print("  NOTE: QEMU blocks differ; see per-path serial/run receipts.")

    stable = ("magic", "fb", "hhdm", "initrd", "cpu_count", "bsp", "cpus", "uefi")
    mismatched = [name for name in stable if qc[name] != x[name]]
    print(f"QEMU candidate vs x86emu candidate stable fields: "
          f"{'MATCH' if not mismatched else 'DIFFER: ' + ', '.join(mismatched)}")
    print(f"  magic={qc['magic']:#018x}; HHDM={qc['hhdm']:#018x}; "
          f"initrd={qc['initrd'][0]:#x}+{qc['initrd'][1]:#x}")
    print(f"  cpu_count={qc['cpu_count']} bsp_lapic={qc['bsp']} cpus={qc['cpus']}; "
          f"boot_from_uefi={qc['uefi']}")
    print(f"  RSDP differs (firmware-specific): QEMU={qc['rsdp']:#x}, x86emu={x['rsdp']:#x}")
    print(f"  mmap_count differs: QEMU={q['mmap_count']}, x86emu={x['mmap_count']}")
    print(f"  QEMU E820:   {fmt_map(q['mmap'])}")
    print(f"  x86emu E820: {fmt_map(x['mmap'])}")
    print("Scope: selected boot_info_t fields from the pinned ISO and the candidate ISO in QEMU, compared with the candidate ISO in x86emu. QEMU/x86emu used different firmware IDs and disk transports; map/RSDP differences are not interpreted here as an allocator-zone result or acceptance pass.")


if __name__ == "__main__":
    main()
