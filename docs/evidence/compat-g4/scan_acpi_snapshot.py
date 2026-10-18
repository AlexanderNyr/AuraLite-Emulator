#!/usr/bin/env python3
"""COMPAT G4: walk RSDP -> RSDT/table set in an x86emu AURASNP1 snapshot.

Reads only the sparse RAM record; never loads or mutates a guest.  Prints one
line per table with its checksum verdict, and "NO RSDP" when the firmware did
not publish a root pointer (the negative-control shape).
"""
from __future__ import annotations
import argparse
import struct
from pathlib import Path

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
            bitmap = exact(f, (pages + 7) // 8)
            ram = bytearray(size)
            for p in range(pages):
                if bitmap[p // 8] & (1 << (p % 8)):
                    off = p * PAGE
                    ram[off:off + min(PAGE, size - off)] = exact(f, min(PAGE, size - off))
            return ram


def checksum_ok(data: bytes) -> bool:
    return (sum(data) & 0xFF) == 0


def table(ram: bytearray, addr: int, label: str) -> bytes:
    if addr < 0 or addr + 36 > len(ram):
        raise SystemExit(f"{label} header outside RAM at 0x{addr:x}")
    length = struct.unpack_from("<I", ram, addr + 4)[0]
    if length < 36 or addr + length > len(ram):
        raise SystemExit(f"{label} length {length} outside RAM at 0x{addr:x}")
    body = bytes(ram[addr:addr + length])
    print(f"  {label:<10} @0x{addr:08x} len={length:<6} "
          f"checksum={'ok' if checksum_ok(body) else 'BAD'}")
    return body


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("snapshot", type=Path)
    args = ap.parse_args()
    ram = snapshot_ram(args.snapshot)

    start, end = 0x00080000, 0x00100000          # SeaBIOS RSDP search window
    at = ram.find(b"RSD PTR ", start)
    if at < 0 or at >= end:
        print("NO RSDP")
        return
    rsdp = bytes(ram[at:at + 20])
    if not checksum_ok(rsdp) or rsdp[15] != 0:
        raise SystemExit(f"RSDP at 0x{at:x}: bad checksum or unexpected revision")
    rsdt = struct.unpack_from("<I", ram, at + 16)[0]
    print(f"RSDP       @0x{at:08x} rev={rsdp[15]} checksum=ok -> RSDT @0x{rsdt:08x}")

    body = table(ram, rsdt, "RSDT")
    if body[:4] != b"RSDT":
        raise SystemExit(f"RSDT signature mismatch: {body[:4]!r}")
    count = (len(body) - 36) // 4
    print(f"  RSDT entries: {count}")
    present = set()
    for k in range(count):
        entry = struct.unpack_from("<I", ram, rsdt + 36 + 4 * k)[0]
        head = bytes(ram[entry:entry + 4])
        length = struct.unpack_from("<I", ram, entry + 4)[0]
        sig = head.decode("ascii", "replace")
        got = table(ram, entry, sig)
        present.add(sig)
        if sig == "APIC":
            base = struct.unpack_from("<I", got, 36)[0]
            print(f"    MADT local APIC base=0x{base:x}")
    print("tables:", " ".join(sorted(present)))


if __name__ == "__main__":
    main()
