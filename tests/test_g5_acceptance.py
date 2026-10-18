#!/usr/bin/env python3
"""COMPAT G5 acceptance gate (patch 0064).

Re-checks the recorded receipts for the ICH9 lane: the boot reaches
`[kernel] shell active` with `fault=0` inside the instruction budget, the
handoff block reports eight E820 entries including the ICH9 RCRB window and a
live RSDP, the ACPI receipt agrees with the serial log, and the negative
control shows that a frozen PM timer reproduces the stall.

File-based like the G3 gate: no emulator, no ISO, no snapshot needed, so it
runs inside `make test`.

Exit: 0 pass, 1 any check fails, 2 usage/IO.
"""
from __future__ import annotations

import argparse
import gzip
import hashlib
import re
import struct
import sys
from pathlib import Path

BOOT_INFO_SIZE = 7776
BOOT_MAGIC = 0x4155524142544C44
MMAP_OFF, MMAP_ENTRY_SIZE, MMAP_COUNT_OFF = 40, 24, 6184
HHDM_OFF, INITRD_PHYS_OFF, INITRD_SIZE_OFF = 6192, 6200, 6208
CPU_COUNT_OFF, BSP_LAPIC_OFF, RSDP_OFF, UEFI_OFF = 6216, 6220, 7760, 7768
HHDM_VALUE = 0xFFFF800000000000
RCRB = (0xFED1C000, 0x4000)
SHELL_MARKER = "[kernel] shell active; kmain idling."

FAILS: list[str] = []


def check(ok: bool, label: str, detail: str = "") -> bool:
    if ok:
        print(f"ok   {label}" + (f" ({detail})" if detail else ""))
    else:
        print(f"FAIL {label}" + (f" ({detail})" if detail else ""), file=sys.stderr)
        FAILS.append(label)
    return ok


def verify_manifest(directory: Path) -> None:
    manifest = directory / "SHA256SUMS"
    if not check(manifest.is_file(), "SHA256SUMS present"):
        return
    listed = 0
    for line in manifest.read_text().splitlines():
        if not line.strip():
            continue
        digest, name = line.split(None, 1)
        name = name.lstrip("*")
        target = directory / name
        if not check(target.is_file(), f"bundle file {name}"):
            continue
        got = hashlib.sha256(target.read_bytes()).hexdigest()
        check(got == digest, f"sha256 {name}", got[:16])
        listed += 1
    present = {p.name for p in directory.iterdir() if p.is_file()} - {"SHA256SUMS"}
    check(listed == len(present), "manifest covers every file",
          f"{listed}/{len(present)}")


def read_serial(directory: Path) -> str:
    plain = directory / "x86emu-ich9.serial.log"
    if plain.is_file():
        return plain.read_text(errors="replace")
    gz = directory / "x86emu-ich9.serial.log.gz"
    if gz.is_file():
        return gzip.decompress(gz.read_bytes()).decode("utf-8", "replace")
    return ""


def verify_serial(directory: Path) -> str:
    text = read_serial(directory)
    if not check(bool(text), "serial log present"):
        return ""
    order = [
        "[BL3] AuraLite stage2 alive",
        "[BL3] unreal mode OK",
        "[BL10] CPU supports long mode",
        "[BL9] ACPI RSDP found",
        "[BL9] ACPI MADT parsed",
        "[BL4] ELF PT_LOAD segments copied to phys",
        "[BL4] entering long mode; jumping to kernel _start",
        SHELL_MARKER,
    ]
    pos = [text.find(m) for m in order]
    missing = [m for m, p in zip(order, pos) if p < 0]
    check(not missing, "every boot marker present", str(missing))
    if not missing:
        check(pos == sorted(pos), "boot markers in order")
    check("panic" not in text.lower(), "no kernel panic in the serial log")
    check("[ioapic] base 0xfec00000 (MADT agree)" in text,
          "guest agrees with the MADT's I/O APIC base")
    return text


def verify_stats(directory: Path) -> None:
    path = directory / "x86emu-ich9.stdout.txt"
    if not check(path.is_file(), "stats stdout present"):
        return
    text = path.read_text(errors="replace")
    m = re.search(r"\[stats\] vcpu=0 executed_steps=(\d+) instr_count=(\d+) "
                  r"halted=(\d+) fault=(\d+)", text)
    if not check(m is not None, "stats line parses"):
        return
    steps, _instr, _halted, fault = (int(x) for x in m.groups())
    check(fault == 0, "fault=0", str(fault))
    cap = re.search(r"\[stats\] cap=(\d+) cap_reached=(\d+)", text)
    check(cap is not None, "cap line present")
    if cap:
        cap_n, reached = int(cap.group(1)), int(cap.group(2))
        check(cap_n == 2_000_000_000, "budget is 2 G", str(cap_n))
        check(reached == 1, "run ended by the cap, not by a fault", str(reached))
    check("mode=long64" in text, "CPU finished in long mode")
    check(steps > 0, "executed_steps recorded", str(steps))


def verify_bootinfo(directory: Path) -> None:
    block = directory / "bootinfo-ich9.bin"
    if not check(block.is_file(), "bootinfo-ich9.bin present"):
        return
    raw = block.read_bytes()
    if not check(len(raw) == BOOT_INFO_SIZE, "boot_info_t size", str(len(raw))):
        return
    check(struct.unpack_from("<Q", raw, 0)[0] == BOOT_MAGIC, "BOOT_MAGIC")
    count = struct.unpack_from("<I", raw, MMAP_COUNT_OFF)[0]
    check(count == 8, "E820 has 8 entries under --lpc=ich9", str(count))
    entries = [struct.unpack_from("<QQII", raw, MMAP_OFF + i * MMAP_ENTRY_SIZE)
               for i in range(min(count, 256))]
    check(all(length > 0 and typ in (1, 2) for _, length, typ, _ in entries),
          "E820 entries are non-empty RAM/reserved")
    check(all(pad == 0 for *_, pad in entries), "E820 entry padding is zero")
    ordered = all(entries[i][0] + entries[i][1] <= entries[i + 1][0]
                  for i in range(len(entries) - 1))
    check(ordered, "E820 entries sorted and non-overlapping")
    check(any((base, length) == RCRB and typ == 2
              for base, length, typ, _ in entries),
          "ICH9 RCRB 0xfed1c000+0x4000 is reserved")
    check(struct.unpack_from("<Q", raw, HHDM_OFF)[0] == HHDM_VALUE, "HHDM offset")
    ip = struct.unpack_from("<Q", raw, INITRD_PHYS_OFF)[0]
    isz = struct.unpack_from("<Q", raw, INITRD_SIZE_OFF)[0]
    check(ip == 0x1800000 and isz == 0xB22000, "initrd address and size")
    check(struct.unpack_from("<I", raw, CPU_COUNT_OFF)[0] == 1, "one CPU")
    check(struct.unpack_from("<I", raw, BSP_LAPIC_OFF)[0] == 0, "BSP APIC id 0")
    rsdp = struct.unpack_from("<Q", raw, RSDP_OFF)[0]
    check(rsdp != 0, "boot_info_t carries a live RSDP", f"{rsdp:#x}")
    check(raw[UEFI_OFF] == 0, "boot_from_uefi is 0")
    check(not any(raw[UEFI_OFF + 1:]), "reserved padding is zero")
    # The RSDP the handoff carries must be the one the ACPI walk found.
    receipt = (directory / "acpi-receipt.txt").read_text().splitlines()
    at = int(re.search(r"rsdp addr=0x([0-9a-f]+)", receipt[0]).group(1), 16)
    check(rsdp == at, "handoff RSDP == RSDP found in memory",
          f"{rsdp:#x} vs {at:#x}")


def verify_acpi(directory: Path, tool: Path) -> None:
    receipt = directory / "acpi-receipt.txt"
    if not check(receipt.is_file(), "acpi-receipt.txt present"):
        return
    import subprocess
    import tempfile
    text = read_serial(directory)
    with tempfile.NamedTemporaryFile("w", suffix=".log", delete=False) as tmp:
        tmp.write(text)
        tmp_path = Path(tmp.name)
    try:
        proc = subprocess.run([sys.executable, str(tool), "--check", str(receipt),
                               "--serial", str(tmp_path)], capture_output=True, text=True)
        check(proc.returncode == 0, "ACPI receipt agrees with the serial log",
              (proc.stdout or proc.stderr).strip()[:120])
    finally:
        tmp_path.unlink(missing_ok=True)


def verify_negative_control(directory: Path) -> None:
    path = directory / "nc-pm.txt"
    if not check(path.is_file(), "nc-pm.txt present"):
        return
    text = path.read_text()
    frozen = re.search(r"frozen:\s+BL3=absent steps=(\d+)", text)
    control = re.search(r"control:\s+BL3=present steps=(\d+)", text)
    check(frozen is not None and int(frozen.group(1)) == 300_000_000,
          "frozen PM timer burns the whole 300 M budget")
    check(control is not None and int(control.group(1)) < 300_000_000,
          "control reaches Stage 2 before the budget")


def main(argv: list[str]) -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--evidence", type=Path,
                    default=Path(__file__).resolve().parent.parent /
                    "docs/evidence/compat-g5")
    args = ap.parse_args(argv)
    d = args.evidence
    if not d.is_dir():
        print(f"no such evidence directory: {d}", file=sys.stderr)
        return 2
    verify_manifest(d)
    verify_serial(d)
    verify_stats(d)
    verify_bootinfo(d)
    verify_acpi(d, Path(__file__).resolve().parent / "g5_acpi_receipt.py")
    verify_negative_control(d)
    if FAILS:
        print(f"\n{len(FAILS)} check(s) failed", file=sys.stderr)
        return 1
    print("\nG5 acceptance: PASS")
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
