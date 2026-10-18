#!/usr/bin/env python3
"""COMPAT G3 acceptance gate.

Verifies the bundled acceptance evidence for G3 (patch 0063): the
authoritative release ISO, rebuilt from the pinned AuraLite-OS commit with
only the reviewed Stage-2 FS/GS fix, must boot under a stock SeaBIOS to
`[kernel] shell active`, while the pinned image must fail ELF parsing in the
same lane; the handoff block the kernel reads must match the documented
`boot_info_t` layout.

The gate is intentionally file-based: it re-checks the recorded receipts
(serial logs, handoff block, hash manifest) instead of trusting prose.  It
needs no emulator, no ISO and no guest source, so it runs in `make test`.

Exit status: 0 all checks pass, 1 any check fails, 2 usage/IO error.

Negative control: `tests/integration/g3_acceptance_nc.sh` tampers copies of
this bundle and requires the gate to reject each mutation.
"""
from __future__ import annotations

import argparse
import gzip
import hashlib
import struct
import sys
from pathlib import Path

BOOT_INFO_SIZE = 7776
BOOT_MAGIC = 0x4155524142544C44
MMAP_MAX = 256
CPU_MAX = 64
MMAP_OFF = 40
MMAP_ENTRY_SIZE = 24
MMAP_COUNT_OFF = 6184
HHDM_OFF = 6192
INITRD_PHYS_OFF = 6200
INITRD_SIZE_OFF = 6208
CPU_COUNT_OFF = 6216
BSP_LAPIC_OFF = 6220
CPUS_OFF = 6224
CPU_ENTRY_SIZE = 24
RSDP_OFF = 7760
UEFI_OFF = 7768
HHDM_VALUE = 0xFFFF800000000000
LAYOUT_CONSTANTS = {
    "BOOT_MAGIC_OFF": 0,
    "BOOT_FB_OFF": 8,
    "BOOT_MMAP_OFF": MMAP_OFF,
    "BOOT_MMAP_CNT_OFF": MMAP_COUNT_OFF,
    "BOOT_HHDM_OFF": HHDM_OFF,
    "BOOT_INITRD_P_OFF": INITRD_PHYS_OFF,
    "BOOT_INITRD_S_OFF": INITRD_SIZE_OFF,
    "BOOT_CPUCNT_OFF": CPU_COUNT_OFF,
    "BOOT_BSP_LAPIC_OFF": BSP_LAPIC_OFF,
    "BOOT_CPUS_OFF": CPUS_OFF,
    "BOOT_RSDP_OFF": RSDP_OFF,
    "BOOT_UEFI_OFF": UEFI_OFF,
    "BOOT_INFO_SIZEOF": BOOT_INFO_SIZE,
}

# Ordered receipts.  A candidate lane must contain every marker in order.
CANDIDATE_MARKERS = [
    "[BL3] AuraLite stage2 alive",
    "[BL3] disk read OK",
    "[BL3] unreal mode OK",
    "[BL4] kernel.elf loaded to 0x00200000",
    "[BL4] ELF PT_LOAD segments copied to phys",
    "[BL4] initrd.tar loaded to 0x01800000",
    "[BL4] page tables built at 0x01000000",
    "[BL4] entering long mode; jumping to kernel _start",
    "[kernel] shell active",
]
PINNED_MARKERS = [
    "[BL3] AuraLite stage2 alive",
    "[BL4] kernel.elf loaded to 0x00200000",
    "[BL4] ELF parse FAILED",
    "[BL4] KERNEL.OLD missing or unloadable; halting",
]
QEMU_CANDIDATE_MARKERS = [
    "[BL9] ACPI RSDP found",
    "[BL9] ACPI MADT parsed",
    "[BL4] ELF PT_LOAD segments copied to phys",
    "[BL4] entering long mode; jumping to kernel _start",
    "[kernel] shell active",
]
QEMU_PINNED_MARKERS = [
    "[BL3] AuraLite stage2 alive",
    "[BL4] ELF parse FAILED",
]
# Register dump order in the debugger: GPRs first, then rip, then memory.
ENTRY_MARKERS = [
    "rdi 0x0000000000010000",
    "rip 0xffffffff80191640",
    "0x4155524142544c44",
]

_failures: list[str] = []
_checks = 0


def check(ok: bool, label: str, detail: str = "") -> bool:
    global _checks
    _checks += 1
    if ok:
        print(f"PASS  {label}")
    else:
        print(f"FAIL  {label}" + (f" — {detail}" if detail else ""))
        _failures.append(label)
    return ok


def read_text(path: Path) -> str:
    if path.suffix == ".gz":
        with gzip.open(path, "rt", encoding="utf-8", errors="replace") as f:
            return f.read()
    return path.read_text(encoding="utf-8", errors="replace")


def has_in_order(text: str, markers: list[str]) -> tuple[bool, str]:
    pos = 0
    for marker in markers:
        found = text.find(marker, pos)
        if found < 0:
            return False, marker
        pos = found + len(marker)
    return True, ""


def verify_manifest(directory: Path) -> None:
    manifest = directory / "SHA256SUMS"
    if not check(manifest.is_file(), "SHA256SUMS present"):
        return
    listed: dict[str, str] = {}
    malformed: list[str] = []
    for line in manifest.read_text(encoding="ascii").splitlines():
        if not line.strip():
            continue
        parts = line.split(maxsplit=1)
        if len(parts) != 2 or len(parts[0]) != 64:
            malformed.append(line)
            continue
        listed[parts[1].lstrip("*")] = parts[0].lower()
    check(not malformed, "SHA256SUMS parses", f"{len(malformed)} bad line(s)")

    missing = sorted(name for name in listed if not (directory / name).is_file())
    check(not missing, "SHA256SUMS files exist", ", ".join(missing))

    bad: list[str] = []
    for name, digest in sorted(listed.items()):
        path = directory / name
        if not path.is_file():
            continue
        actual = hashlib.sha256(path.read_bytes()).hexdigest()
        if actual != digest:
            bad.append(f"{name}: {actual} != {digest}")
    check(not bad, "SHA256SUMS digests match", "; ".join(bad))

    on_disk = {
        p.name
        for p in directory.iterdir()
        if p.is_file() and p.name != "SHA256SUMS"
    }
    unlisted = sorted(on_disk - set(listed))
    check(not unlisted, "every file is hashed", ", ".join(unlisted))


def verify_logs(directory: Path) -> None:
    x_pin = directory / "x86emu-stock-pinned.serial.log.gz"
    x_can = directory / "x86emu-stock-candidate.serial.log.gz"
    x_pin_stats = directory / "x86emu-stock-pinned.stdout"
    x_can_stats = directory / "x86emu-stock-candidate.stdout"
    q_pin = directory / "qemu-stock-pinned.serial.log.gz"
    q_can = directory / "qemu-stock-candidate.serial.log.gz"
    entry = directory / "x86emu-kernel-entry.log.gz"
    x_can_replay = directory / "x86emu-stock-candidate-replay.serial.log.gz"
    x_can_replay_stats = directory / "x86emu-stock-candidate-replay.stdout"

    for path in (x_pin, x_can, x_pin_stats, x_can_stats, q_pin, q_can, entry,
                 x_can_replay, x_can_replay_stats):
        if not check(path.is_file(), f"{path.name} present"):
            return

    pinned = read_text(x_pin)
    ok, marker = has_in_order(pinned, PINNED_MARKERS)
    check(ok, "x86emu pinned reaches ELF parse failure", f"missing {marker}")
    check(
        "ELF PT_LOAD segments copied to phys" not in pinned
        and "shell active" not in pinned,
        "x86emu pinned never loads the ELF (negative control)",
    )

    candidate = read_text(x_can)
    ok, marker = has_in_order(candidate, CANDIDATE_MARKERS)
    check(ok, "x86emu candidate reaches the kernel shell", f"missing {marker}")
    check(
        "ELF parse FAILED" not in candidate,
        "x86emu candidate has no ELF parse failure",
    )

    replay = read_text(x_can_replay)
    check(replay == candidate,
          "second candidate run is byte-identical on the serial stream")
    check(read_text(x_can_replay_stats) == read_text(x_can_stats),
          "second candidate run has an identical stats block")

    pinned_stats = read_text(x_pin_stats)
    candidate_stats = read_text(x_can_stats)
    check("fault=0" in pinned_stats, "x86emu pinned fault=0")
    check("fault=0" in candidate_stats, "x86emu candidate fault=0")
    check("cap_reached=1" in candidate_stats, "x86emu candidate ran to the cap")
    check(
        "real-mode services complete; halting" in pinned,
        "x86emu pinned halts in real mode",
    )
    check(
        "mode=long64" in candidate_stats and "STATE: HALTED" in candidate_stats,
        "x86emu candidate halts in long mode",
    )

    qemu_pinned = read_text(q_pin)
    ok, marker = has_in_order(qemu_pinned, QEMU_PINNED_MARKERS)
    check(ok, "QEMU pinned reproduces the ELF failure", f"missing {marker}")
    check("shell active" not in qemu_pinned, "QEMU pinned never reaches the shell")

    qemu_candidate = read_text(q_can)
    ok, marker = has_in_order(qemu_candidate, QEMU_CANDIDATE_MARKERS)
    check(ok, "QEMU candidate reaches the shell with ACPI", f"missing {marker}")

    entry_text = read_text(entry)
    ok, marker = has_in_order(entry_text, ENTRY_MARKERS)
    check(ok, "kernel entry stop shows rdi=0x10000 and BOOT_MAGIC", f"missing {marker}")


def read_layout(path: Path) -> dict[str, int]:
    values: dict[str, int] = {}
    for line in path.read_text(encoding="ascii").splitlines():
        words = line.split()
        if len(words) == 3 and words[0] == "%define":
            values[words[1]] = int(words[2], 0)
    return values


def verify_bootinfo(directory: Path) -> None:
    block = directory / "bootinfo.bin"
    if not check(block.is_file(), "bootinfo.bin present"):
        return
    raw = block.read_bytes()
    if not check(len(raw) == BOOT_INFO_SIZE, "boot_info_t size is 7776", f"{len(raw)}"):
        return

    layout_path = directory.parent / "bootinfo-layout.inc"
    if layout_path.is_file():
        emitted = read_layout(layout_path)
        ok = all(emitted.get(k) == v for k, v in LAYOUT_CONSTANTS.items())
        check(ok, "generated boot_info_t offsets match the parser contract")
    else:
        check(False, "bootinfo-layout.inc present", str(layout_path))

    magic = struct.unpack_from("<Q", raw, 0)[0]
    check(magic == BOOT_MAGIC, "BOOT_MAGIC", f"{magic:#018x}")
    fb = struct.unpack_from("<QIII4B", raw, 8)
    check(fb[0] == 0, "BIOS path carries no linear framebuffer", f"phys={fb[0]:#x}")

    mmap_count = struct.unpack_from("<I", raw, MMAP_COUNT_OFF)[0]
    mmap_pad = struct.unpack_from("<I", raw, MMAP_COUNT_OFF + 4)[0]
    check(mmap_count == 7, "E820 map has 7 entries", f"{mmap_count}")
    check(0 < mmap_count <= MMAP_MAX, "E820 count within bounds")
    check(mmap_pad == 0, "E820 count padding is zero")

    entries = []
    for i in range(min(mmap_count, MMAP_MAX)):
        base, length, typ, pad = struct.unpack_from(
            "<QQII", raw, MMAP_OFF + i * MMAP_ENTRY_SIZE
        )
        entries.append((base, length, typ, pad))
    check(all(pad == 0 for _, _, _, pad in entries), "E820 entries have zero padding")
    check(all(length > 0 and typ in (1, 2) for _, length, typ, _ in entries),
          "E820 entries are non-empty RAM/reserved types")
    ordered = all(
        entries[i][0] + entries[i][1] <= entries[i + 1][0]
        for i in range(len(entries) - 1)
    )
    check(ordered, "E820 entries are sorted and non-overlapping")

    hhdm = struct.unpack_from("<Q", raw, HHDM_OFF)[0]
    check(hhdm == HHDM_VALUE, "HHDM offset", f"{hhdm:#x}")
    initrd_phys = struct.unpack_from("<Q", raw, INITRD_PHYS_OFF)[0]
    initrd_size = struct.unpack_from("<Q", raw, INITRD_SIZE_OFF)[0]
    check(initrd_phys == 0x1800000, "initrd physical address", f"{initrd_phys:#x}")
    check(initrd_size == 0xB22000, "initrd size", f"{initrd_size:#x}")

    cpu_count = struct.unpack_from("<I", raw, CPU_COUNT_OFF)[0]
    bsp_lapic = struct.unpack_from("<I", raw, BSP_LAPIC_OFF)[0]
    check(cpu_count == 1, "one CPU in the BIOS handoff", f"{cpu_count}")
    check(0 < cpu_count <= CPU_MAX, "CPU count within bounds")
    check(bsp_lapic == 0, "BSP LAPIC id is 0", f"{bsp_lapic}")
    proc_id, lapic_id = struct.unpack_from("<II", raw, CPUS_OFF)
    check(proc_id == 0 and lapic_id == 0, "cpu[0] is processor/APIC id 0")

    # Read the size field of the CPU entry table to prove CPUS_OFF is usable.
    check(CPUS_OFF + CPU_ENTRY_SIZE <= BOOT_INFO_SIZE, "CPU table inside the block")

    rsdp = struct.unpack_from("<Q", raw, RSDP_OFF)[0]
    check(rsdp == 0, "stock-BIOS lane reports no RSDP", f"{rsdp:#x}")
    uefi = raw[UEFI_OFF]
    check(uefi == 0, "boot_from_uefi is 0", f"{uefi}")
    check(
        not any(raw[UEFI_OFF + 1:]),
        "reserved padding after boot_from_uefi is zero",
    )


def verify_stage2_patch(directory: Path) -> None:
    """The reviewed patch must reassemble to the blob the ISO carried."""
    blob = directory.parent / "stage2-fs-gs-restore.bin"
    if not check(blob.is_file(), "stage2-fs-gs-restore.bin present"):
        return
    digest = hashlib.sha256(blob.read_bytes()).hexdigest()
    check(
        digest == "51ecc150f6eefd84f570f2fbdc808c4f3edd7ac7bf342559c19f652638b85963",
        "reviewed Stage-2 blob hash",
        digest,
    )
    receipt = (directory / "inputs.txt").read_text(encoding="utf-8")
    check(
        digest in receipt,
        "receipt records the same Stage-2 blob hash",
    )


def main(argv: list[str]) -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument(
        "--evidence",
        type=Path,
        default=Path(__file__).resolve().parent.parent
        / "docs/evidence/compat-g3/acceptance",
        help="acceptance bundle directory",
    )
    args = ap.parse_args(argv)

    directory: Path = args.evidence
    if not directory.is_dir():
        print(f"error: {directory} is not a directory", file=sys.stderr)
        return 2

    print(f"G3 acceptance gate: {directory}")
    verify_manifest(directory)
    verify_logs(directory)
    verify_bootinfo(directory)
    verify_stage2_patch(directory)

    print()
    if _failures:
        print(f"G3 acceptance: FAIL ({len(_failures)} of {_checks} checks)")
        for name in _failures:
            print(f"  - {name}")
        return 1
    print(f"G3 acceptance: PASS ({_checks} checks)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
