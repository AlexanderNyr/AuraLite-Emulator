#!/usr/bin/env python3
"""Inspect boot_info_t bytes in a diagnostic x86emu sparse-RAM snapshot.

The snapshot is not modified. Layout offsets are from the pinned
boot/shared/boot_info.h via tools/gen_boot_offsets.c; see bootinfo-layout.txt.
"""
from __future__ import annotations
import argparse
from pathlib import Path
import struct

from inspect_acpi_snapshot import range_checked, snapshot_ram

BOOT_INFO_SIZE = 7776
BOOT_MAGIC = 0x4155524142544C44
BOOT_INFO_PHYS_DEFAULT = 0x10000
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


def read_layout(path: Path) -> dict[str, int]:
    values: dict[str, int] = {}
    for line in path.read_text(encoding="ascii").splitlines():
        words = line.split()
        if len(words) == 3 and words[0] == "%define":
            values[words[1]] = int(words[2], 0)
    return values


def checksum_ok(data: bytes) -> bool:
    return (sum(data) & 0xFF) == 0


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("snapshot", type=Path)
    ap.add_argument("--phys", type=lambda s: int(s, 0), default=BOOT_INFO_PHYS_DEFAULT)
    ap.add_argument("--write-raw", type=Path, help="optionally save the validated boot_info_t block")
    ap.add_argument(
        "--scope",
        default="candidate Stage-2 + diagnostic-ROM snapshot only; this output alone is not pinned-image acceptance or the separate QEMU reference comparison.",
        help="explicit provenance/scope note printed with the inspection",
    )
    args = ap.parse_args()

    layout_path = Path(__file__).with_name("bootinfo-layout.inc")
    emitted_layout = read_layout(layout_path)
    if any(emitted_layout.get(name) != value for name, value in LAYOUT_CONSTANTS.items()):
        raise SystemExit(f"generated header offsets/size do not match parser contract: {emitted_layout}")

    with args.snapshot.open("rb") as f:
        magic = f.read(8)
        version_bytes = f.read(4)
    if magic != b"AURASNP1" or len(version_bytes) != 4:
        raise SystemExit("not a complete AURASNP1 snapshot header")
    (version,) = struct.unpack("<I", version_bytes)
    if version != 2:
        raise SystemExit(f"expected snapshot v2, got v{version}")

    ram = snapshot_ram(args.snapshot)
    raw = range_checked(ram, args.phys, BOOT_INFO_SIZE, "boot_info_t")
    magic = struct.unpack_from("<Q", raw, 0)[0]
    if magic != BOOT_MAGIC:
        raise SystemExit(f"bad BOOT_MAGIC: {magic:#018x}")

    fb_phys, fb_w, fb_h, fb_pitch, fb_bpp, fb_r, fb_g, fb_b = struct.unpack_from("<QIII4B", raw, 8)
    mmap_count = struct.unpack_from("<I", raw, MMAP_COUNT_OFF)[0]
    mmap_pad = struct.unpack_from("<I", raw, MMAP_COUNT_OFF + 4)[0]
    hhdm = struct.unpack_from("<Q", raw, HHDM_OFF)[0]
    initrd_phys = struct.unpack_from("<Q", raw, INITRD_PHYS_OFF)[0]
    initrd_size = struct.unpack_from("<Q", raw, INITRD_SIZE_OFF)[0]
    cpu_count = struct.unpack_from("<I", raw, CPU_COUNT_OFF)[0]
    bsp_lapic = struct.unpack_from("<I", raw, BSP_LAPIC_OFF)[0]
    rsdp_phys = struct.unpack_from("<Q", raw, RSDP_OFF)[0]
    boot_from_uefi = raw[UEFI_OFF]

    if mmap_count > MMAP_MAX:
        raise SystemExit(f"mmap_count {mmap_count} exceeds {MMAP_MAX}")
    if cpu_count > CPU_MAX:
        raise SystemExit(f"cpu_count {cpu_count} exceeds {CPU_MAX}")
    if hhdm != HHDM_VALUE:
        raise SystemExit(f"unexpected HHDM offset {hhdm:#x}")
    if mmap_pad != 0 or any(raw[UEFI_OFF + 1:BOOT_INFO_SIZE]):
        raise SystemExit("nonzero boot_info_t reserved padding")
    if boot_from_uefi not in (0, 1):
        raise SystemExit(f"invalid boot_from_uefi value {boot_from_uefi}")

    mmap = []
    last_end = 0
    for i in range(mmap_count):
        off = MMAP_OFF + i * MMAP_ENTRY_SIZE
        base, length, typ, pad = struct.unpack_from("<QQII", raw, off)
        if length == 0 or typ not in range(1, 9) or pad != 0:
            raise SystemExit(f"invalid mmap entry {i}: {base:#x}+{length:#x}, type={typ}, pad={pad}")
        if i and base < last_end:
            raise SystemExit(f"overlapping/unsorted mmap entry {i}: base={base:#x} previous-end={last_end:#x}")
        last_end = base + length
        mmap.append((base, length, typ))

    cpus = []
    for i in range(cpu_count):
        off = CPUS_OFF + i * CPU_ENTRY_SIZE
        processor_id, lapic_id, goto_address, extra_argument = struct.unpack_from("<IIQQ", raw, off)
        cpus.append((processor_id, lapic_id, goto_address, extra_argument))
    if cpu_count and cpus[0][1] != bsp_lapic:
        raise SystemExit("BSP APIC ID disagrees with cpus[0].lapic_id")

    if rsdp_phys:
        rsdp = range_checked(ram, rsdp_phys, 20, "RSDP")
        if rsdp[:8] != b"RSD PTR " or not checksum_ok(rsdp):
            raise SystemExit(f"invalid RSDP v1 signature/checksum at {rsdp_phys:#x}")
        revision = rsdp[15]
        if revision >= 2:
            ext_head = range_checked(ram, rsdp_phys, 36, "extended RSDP")
            ext_len = struct.unpack_from("<I", ext_head, 20)[0]
            ext = range_checked(ram, rsdp_phys, ext_len, "extended RSDP")
            if ext_len < 36 or not checksum_ok(ext):
                raise SystemExit(f"invalid extended RSDP length/checksum at {rsdp_phys:#x}")
        rsdp_check = f"signature/checksum OK (revision {revision})"
    else:
        rsdp_check = "absent"

    if args.write_raw:
        args.write_raw.parent.mkdir(parents=True, exist_ok=True)
        args.write_raw.write_bytes(raw)

    print(f"snapshot version={version}; RAM={len(ram)} bytes; boot_info phys={args.phys:#x} size={BOOT_INFO_SIZE}")
    print("Generated C-header offsets/size: bootinfo-layout.inc MATCH")
    print(f"magic={magic:#018x} OK; boot_from_uefi={boot_from_uefi}; reserved padding=zero")
    print(f"fb phys={fb_phys:#x} {fb_w}x{fb_h} pitch={fb_pitch} bpp={fb_bpp} rgb-shift={fb_r},{fb_g},{fb_b}")
    print(f"mmap_count={mmap_count} (max {MMAP_MAX}); hhdm_offset={hhdm:#018x}")
    for i, (base, length, typ) in enumerate(mmap):
        print(f"  mmap[{i}] base={base:#010x} length={length:#010x} type={typ}")
    print(f"initrd phys={initrd_phys:#x} size={initrd_size:#x} ({initrd_size} bytes)")
    print(f"cpu_count={cpu_count} (max {CPU_MAX}); bsp_lapic_id={bsp_lapic}")
    for i, (processor, lapic, go, arg) in enumerate(cpus):
        print(f"  cpu[{i}] processor_id={processor} lapic_id={lapic} goto={go:#x} extra={arg:#x}")
    print(f"rsdp_phys={rsdp_phys:#x} ({rsdp_check})")
    print("Checks: generated C-header offsets/size, magic, array counts, sorted non-overlapping E820 entries, field padding, HHDM, CPU/BSP agreement, and RSDP.")
    print(f"Scope: {args.scope}")


if __name__ == "__main__":
    main()
