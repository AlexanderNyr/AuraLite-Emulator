#!/usr/bin/env python3
"""Compare two captured 7,776-byte boot_info_t blocks without causal claims."""
from __future__ import annotations
import argparse
import hashlib
from pathlib import Path

from compare_bootinfo_runs import fields, fmt_map, load


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("left", type=Path)
    ap.add_argument("right", type=Path)
    ap.add_argument("--left-label", default="left")
    ap.add_argument("--right-label", default="right")
    args = ap.parse_args()

    left_data = load(args.left)
    right_data = load(args.right)
    left = fields(left_data)
    right = fields(right_data)
    stable = ("magic", "fb", "hhdm", "initrd", "cpu_count", "bsp", "cpus", "uefi")
    mismatched = [name for name in stable if left[name] != right[name]]

    print(f"{args.left_label}: {args.left} sha256={hashlib.sha256(left_data).hexdigest()}")
    print(f"{args.right_label}: {args.right} sha256={hashlib.sha256(right_data).hexdigest()}")
    raw_differences = sum(a != b for a, b in zip(left_data, right_data))
    print(f"Raw block: {'byte-identical' if raw_differences == 0 else 'different'}; differing_bytes={raw_differences}")
    print(f"Selected stable fields: {'MATCH' if not mismatched else 'DIFFER: ' + ', '.join(mismatched)}")
    print(f"  magic={left['magic']:#018x} / {right['magic']:#018x}")
    print(f"  fb={left['fb']} / {right['fb']}")
    print(f"  HHDM={left['hhdm']:#x} / {right['hhdm']:#x}")
    print(f"  initrd={left['initrd'][0]:#x}+{left['initrd'][1]:#x} / "
          f"{right['initrd'][0]:#x}+{right['initrd'][1]:#x}")
    print(f"  CPUs/BSP/entries={left['cpu_count']}/{left['bsp']}/{left['cpus']} / "
          f"{right['cpu_count']}/{right['bsp']}/{right['cpus']}")
    print(f"  boot_from_uefi={left['uefi']} / {right['uefi']}")
    print(f"RSDP: {left['rsdp']:#x} / {right['rsdp']:#x}")
    print(f"E820 count: {left['mmap_count']} / {right['mmap_count']}")
    print(f"{args.left_label} E820:  {fmt_map(left['mmap'])}")
    print(f"{args.right_label} E820: {fmt_map(right['mmap'])}")
    print("Scope: selected boot_info_t fields only. Do not infer firmware equivalence, "
          "allocator-zone cause, or boot acceptance from this comparison.")


if __name__ == "__main__":
    main()
