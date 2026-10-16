#!/usr/bin/env python3
"""Extract and validate RSDP/RSDT/MADT bytes from an x86emu AURASNP1 snapshot.

This reads only the sparse RAM record; it does not load or mutate a guest.
The snapshot itself is intentionally kept outside the compact evidence bundle.
"""
from __future__ import annotations
import argparse
from pathlib import Path
import struct

PAGE = 4096

def exact(f, n: int) -> bytes:
    b = f.read(n)
    if len(b) != n:
        raise SystemExit(f"truncated snapshot: wanted {n} bytes, got {len(b)}")
    return b

def snapshot_ram(path: Path) -> bytearray:
    with path.open("rb") as f:
        if exact(f, 8) != b"AURASNP1":
            raise SystemExit("not an x86emu AURASNP1 snapshot")
        version, _machine_size, _cpu_size = struct.unpack("<III", exact(f, 12))
        if version not in (1, 2):
            raise SystemExit(f"unsupported snapshot version {version}")
        while True:
            tag = exact(f, 16).split(b"\0", 1)[0].decode("ascii", "replace")
            (size,) = struct.unpack("<Q", exact(f, 8))
            if tag != "ram":
                f.seek(size, 1)
                continue
            pages = (size + PAGE - 1) // PAGE
            bitmap_len = (pages + 7) // 8
            bitmap = exact(f, bitmap_len)
            ram = bytearray(size)
            for p in range(pages):
                if bitmap[p // 8] & (1 << (p % 8)):
                    off = p * PAGE
                    ram[off:off + min(PAGE, size - off)] = exact(f, min(PAGE, size - off))
            return ram

def checksum_ok(data: bytes) -> bool:
    return (sum(data) & 0xFF) == 0

def range_checked(ram: bytearray, addr: int, length: int, label: str) -> bytes:
    if addr < 0 or length < 0 or addr + length > len(ram):
        raise SystemExit(f"{label} range outside RAM: {addr:#x}+{length:#x}")
    return bytes(ram[addr:addr + length])

def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("snapshot", type=Path)
    ap.add_argument("--rsdp-phys", type=lambda s: int(s, 0), default=0xF6210)
    args = ap.parse_args()
    ram = snapshot_ram(args.snapshot)

    rsdp = range_checked(ram, args.rsdp_phys, 36, "RSDP")
    if rsdp[:8] != b"RSD PTR ":
        raise SystemExit(f"RSDP signature mismatch at {args.rsdp_phys:#x}: {rsdp[:8]!r}")
    revision = rsdp[15]
    if not checksum_ok(rsdp[:20]):
        raise SystemExit("RSDP v1 checksum failed")
    if revision >= 2:
        length = struct.unpack_from("<I", rsdp, 20)[0]
        rsdp_ext = range_checked(ram, args.rsdp_phys, length, "extended RSDP")
        if length < 36 or not checksum_ok(rsdp_ext):
            raise SystemExit("RSDP extended checksum/length failed")
        xsdt = struct.unpack_from("<Q", rsdp, 24)[0]
    else:
        xsdt = 0

    rsdt = struct.unpack_from("<I", rsdp, 16)[0]
    if xsdt:
        sdt_addr, width, sdt_sig = xsdt, 8, b"XSDT"
    else:
        sdt_addr, width, sdt_sig = rsdt, 4, b"RSDT"
    sdt_head = range_checked(ram, sdt_addr, 36, sdt_sig.decode())
    sdt_len = struct.unpack_from("<I", sdt_head, 4)[0]
    if sdt_head[:4] != sdt_sig or sdt_len < 36:
        raise SystemExit(f"bad {sdt_sig.decode()} header at {sdt_addr:#x}")
    sdt = range_checked(ram, sdt_addr, sdt_len, sdt_sig.decode())
    if (sdt_len - 36) % width or not checksum_ok(sdt):
        raise SystemExit(f"bad {sdt_sig.decode()} length/checksum")
    entries = [int.from_bytes(sdt[o:o + width], "little")
               for o in range(36, sdt_len, width)]

    madt_addr = None
    madt = b""
    for ptr in entries:
        if ptr + 8 > len(ram):
            continue
        if ram[ptr:ptr + 4] == b"APIC":
            madt_addr = ptr
            length = struct.unpack_from("<I", ram, ptr + 4)[0]
            madt = range_checked(ram, ptr, length, "MADT")
            break
    if madt_addr is None:
        raise SystemExit(f"no APIC/MADT pointer in {sdt_sig.decode()}")
    if len(madt) < 44 or not checksum_ok(madt):
        raise SystemExit("MADT length/checksum failed")

    entries_seen: list[tuple[int, int]] = []
    local_apics: list[tuple[int, int, int]] = []
    off = 44
    while off < len(madt):
        if off + 2 > len(madt):
            raise SystemExit("short MADT entry header")
        typ, length = madt[off], madt[off + 1]
        if length < 2 or off + length > len(madt):
            raise SystemExit(f"malformed MADT entry type={typ} length={length} at {off:#x}")
        entries_seen.append((typ, length))
        if typ == 0 and length >= 8:
            processor, apic_id = madt[off + 2], madt[off + 3]
            flags = struct.unpack_from("<I", madt, off + 4)[0]
            local_apics.append((processor, apic_id, flags))
        off += length

    print(f"RSDP phys={args.rsdp_phys:#x} signature=RSD PTR  revision={revision} checksum20=OK")
    print(f"{sdt_sig.decode()} phys={sdt_addr:#x} length={sdt_len} checksum=OK entries={len(entries)}")
    print(f"MADT phys={madt_addr:#x} length={len(madt)} checksum=OK "
          f"lapic={struct.unpack_from('<I', madt, 36)[0]:#x} "
          f"flags={struct.unpack_from('<I', madt, 40)[0]:#x}")
    print(f"MADT structures={entries_seen}")
    print(f"enabled/local APIC candidates (processor, APIC ID, flags): {local_apics}")
    print("Scope: structural/checksum inspection only; not FADT/PM validation or boot_info ABI acceptance.")

if __name__ == "__main__":
    main()
