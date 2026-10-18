#!/usr/bin/env python3
"""COMPAT G5: ACPI acceptance on the accepted image.

Walks the live ACPI tree inside an x86emu AURASNP1 snapshot and decodes the
MADT far enough to check the two claims the guest makes on the serial line:

  * `[BL9] ACPI RSDP found` / `[BL9] ACPI MADT parsed` -- the guest found a
    root pointer and parsed one MADT;
  * `[ioapic] base 0xfec00000 (MADT agree)` and `24 redirection entries` --
    the I/O APIC the kernel programmed is the one the MADT describes.

Usage:
  g5_acpi_receipt.py --snapshot s.snap --serial run.serial.log   # print
  g5_acpi_receipt.py --check receipt.txt --serial run.serial.log # gate

Exit: 0 pass, 1 mismatch, 2 usage/IO.
"""
from __future__ import annotations

import argparse
import re
import struct
import sys
from pathlib import Path

PAGE = 4096
RSDP_START, RSDP_END = 0x00080000, 0x00100000     # SeaBIOS search window


def exact(f, n: int) -> bytes:
    b = f.read(n)
    if len(b) != n:
        raise SystemExit(f"truncated snapshot: wanted {n} bytes, got {len(b)}")
    return b


def snapshot_ram(path: Path) -> bytearray:
    with path.open("rb") as f:
        if exact(f, 8) != b"AURASNP1":
            raise SystemExit("not an x86emu AURASNP1 snapshot")
        version, _ms, _cs = struct.unpack("<III", exact(f, 12))
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


def read_table(ram: bytearray, addr: int) -> bytes:
    if addr < 0 or addr + 36 > len(ram):
        raise SystemExit(f"table header outside RAM at 0x{addr:x}")
    length = struct.unpack_from("<I", ram, addr + 4)[0]
    if length < 36 or addr + length > len(ram):
        raise SystemExit(f"table length {length} outside RAM at 0x{addr:x}")
    return bytes(ram[addr:addr + length])


def decode_madt(body: bytes) -> dict:
    lapic_base = struct.unpack_from("<I", body, 36)[0]
    out = {"lapic_base": lapic_base, "cpus": [], "ioapics": [], "overrides": []}
    off = 44
    while off + 2 <= len(body):
        etype, elen = body[off], body[off + 1]
        if elen < 2 or off + elen > len(body):
            break
        if etype == 0 and elen >= 8:                       # Processor Local APIC
            out["cpus"].append({"uid": body[off + 2], "apic_id": body[off + 3],
                                "flags": struct.unpack_from("<I", body, off + 4)[0]})
        elif etype == 1 and elen >= 12:                    # I/O APIC
            out["ioapics"].append({"id": body[off + 2],
                                   "addr": struct.unpack_from("<I", body, off + 4)[0],
                                   "gsi_base": struct.unpack_from("<I", body, off + 8)[0]})
        elif etype == 2 and elen >= 10:                    # Interrupt Source Override
            out["overrides"].append({"bus": body[off + 2], "source": body[off + 3],
                                     "gsi": struct.unpack_from("<I", body, off + 4)[0],
                                     "flags": struct.unpack_from("<H", body, off + 8)[0]})
        off += elen
    return out


SERIAL_PATTERNS = {
    "rsdp": re.compile(r"\[BL9\] ACPI RSDP found"),
    "madt": re.compile(r"\[BL9\] ACPI MADT parsed"),
    "ioapic_agree": re.compile(r"\[ioapic\] base (0x[0-9a-f]+) \(MADT agree\)"),
    "ioapic_entries": re.compile(r"I/O APIC @(0x[0-9a-f]+) ver 0x[0-9a-f]+, "
                                 r"(\d+) redirection entries"),
    "overrides": re.compile(r"\[ioapic\] overrides: (.+)$"),
}


def serial_facts(path: Path) -> dict:
    text = path.read_text(errors="replace")
    facts = {k: None for k in SERIAL_PATTERNS}
    facts["order"] = []
    for name, rx in SERIAL_PATTERNS.items():
        m = rx.search(text)
        if not m:
            continue
        if name in ("ioapic_agree", "ioapic_entries"):
            facts["order"].append(name)
        facts[name] = m.groups()
    for marker in ("rsdp", "madt"):
        if SERIAL_PATTERNS[marker].search(text):
            pos = SERIAL_PATTERNS[marker].search(text).start()
            facts["order"].append((pos, marker))
    return facts


def render(ram: bytearray) -> tuple[list[str], dict]:
    lines: list[str] = []
    at = ram.find(b"RSD PTR ", RSDP_START)
    if at < 0 or at >= RSDP_END:
        raise SystemExit("NO RSDP in the SeaBIOS window")
    rsdp = bytes(ram[at:at + 20])
    if not checksum_ok(rsdp):
        raise SystemExit(f"RSDP at 0x{at:x}: bad checksum")
    rsdt = struct.unpack_from("<I", ram, at + 16)[0]
    lines.append(f"rsdp addr=0x{at:08x} rev={rsdp[15]} checksum=ok rsdt=0x{rsdt:08x}")

    body = read_table(ram, rsdt)
    if body[:4] != b"RSDT":
        raise SystemExit(f"RSDT signature mismatch: {body[:4]!r}")
    if not checksum_ok(body):
        raise SystemExit("RSDT checksum bad")
    count = (len(body) - 36) // 4
    lines.append(f"rsdt sig=RSDT entries={count} checksum=ok")
    facts = {"rsdp": at, "rsdt": rsdt, "entries": count, "tables": {}, "madt": None}
    for k in range(count):
        entry = struct.unpack_from("<I", ram, rsdt + 36 + 4 * k)[0]
        tbl = read_table(ram, entry)
        sig = tbl[:4].decode("ascii", "replace")
        ok = checksum_ok(tbl)
        facts["tables"][sig] = {"addr": entry, "len": len(tbl), "checksum_ok": ok}
        lines.append(f"table sig={sig} addr=0x{entry:08x} len={len(tbl)} "
                     f"checksum={'ok' if ok else 'BAD'}")
        if sig == "APIC":
            madt = decode_madt(tbl)
            madt["len"] = len(tbl)
            madt["checksum_ok"] = ok
            facts["madt"] = madt
    if facts["madt"] is None:
        raise SystemExit("no MADT in the RSDT")
    m = facts["madt"]
    lines.append(f"madt lapic_base=0x{m['lapic_base']:08x} cpus={len(m['cpus'])} "
                 f"ioapics={len(m['ioapics'])} isos={len(m['overrides'])}")
    for io in m["ioapics"]:
        lines.append(f"madt_ioapic id={io['id']} addr=0x{io['addr']:08x} "
                     f"gsi_base=0x{io['gsi_base']:x}")
    for ov in m["overrides"]:
        lines.append(f"madt_iso bus={ov['bus']} source={ov['source']} "
                     f"gsi={ov['gsi']} flags=0x{ov['flags']:04x}")
    return lines, facts


def check(receipt: Path, serial: Path) -> int:
    kv: dict[str, str] = {}
    for line in receipt.read_text().splitlines():
        if not line.strip() or line.startswith("#"):
            continue
        parts = line.split()
        kv[parts[0]] = line
    fails = []

    def need(cond: bool, msg: str) -> None:
        if not cond:
            fails.append(msg)

    need("rsdp" in kv and "checksum=ok" in kv["rsdp"], "receipt: RSDP missing/bad")
    need("rsdt" in kv and "checksum=ok" in kv["rsdt"], "receipt: RSDT missing/bad")
    entries = int(re.search(r"entries=(\d+)", kv.get("rsdt", "")).group(1)) \
        if "rsdt" in kv else 0
    tables = [l for l in receipt.read_text().splitlines() if l.startswith("table sig=")]
    need(len(tables) == entries, f"receipt: {len(tables)} table lines for {entries} RSDT entries")
    need(all("checksum=ok" in t for t in tables), "receipt: a table checksum is BAD")
    need(any("sig=APIC " in l for l in tables), "receipt: no MADT/APIC table")
    madt_line = kv.get("madt", "")
    need("lapic_base=0x" in madt_line, "receipt: MADT line missing")
    ioapic_lines = [l for l in receipt.read_text().splitlines() if l.startswith("madt_ioapic ")]
    need(len(ioapic_lines) >= 1, "receipt: MADT has no I/O APIC")

    s = serial.read_text(errors="replace")
    for key, rx in (("rsdp", SERIAL_PATTERNS["rsdp"]), ("madt", SERIAL_PATTERNS["madt"])):
        need(rx.search(s) is not None, f"serial: missing {key} marker")
    ioa = SERIAL_PATTERNS["ioapic_agree"].search(s)
    need(ioa is not None, "serial: missing '[ioapic] base ... (MADT agree)'")
    if ioa and ioapic_lines:
        receipt_addr = re.search(r"addr=(0x[0-9a-f]+)", ioapic_lines[0]).group(1)
        need(int(ioa.group(1), 16) == int(receipt_addr, 16),
             f"serial ioapic {ioa.group(1)} != MADT {receipt_addr}")
    ent = SERIAL_PATTERNS["ioapic_entries"].search(s)
    need(ent is not None, "serial: missing I/O APIC redirection-entry count")
    if ent and ioapic_lines:
        need(ent.group(1) == ioapic_lines[0].split("addr=")[1].split()[0],
             "serial: I/O APIC address differs from MADT")
        madt_line = kv.get("madt", "")
        isos = int(re.search(r"isos=(\d+)", madt_line).group(1)) if "isos=" in madt_line else 0
        need(isos > 0, "receipt: MADT carries no ISO entries")
    # Marker order: RSDP before MADT before the ioapic programming line.
    pos = [s.find(m) for m in ("[BL9] ACPI RSDP found", "[BL9] ACPI MADT parsed",
                               "[ioapic] base")]
    need(all(p >= 0 for p in pos) and pos == sorted(pos),
         "serial: RSDP/MADT/ioapic markers out of order")

    for f in fails:
        print("FAIL:", f, file=sys.stderr)
    if fails:
        return 1
    print(f"g5 acpi: PASS ({len(tables)} tables, MADT ioapic "
          f"{ioapic_lines[0].split('addr=')[1].split()[0]})")
    return 0


def main(argv: list[str]) -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--snapshot", type=Path)
    ap.add_argument("--serial", type=Path, required=True)
    ap.add_argument("--check", type=Path)
    ap.add_argument("--out", type=Path)
    a = ap.parse_args(argv)
    if a.check:
        return check(a.check, a.serial)
    if not a.snapshot:
        ap.error("need --snapshot (or --check RECEIPT)")
    lines, _facts = render(snapshot_ram(a.snapshot))
    text = "\n".join(lines) + "\n"
    if a.out:
        a.out.write_text(text)
    sys.stdout.write(text)
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
