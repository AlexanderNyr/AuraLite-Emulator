#!/usr/bin/env python3
"""COMPAT G6 acceptance gate (patch 0065).

Re-checks the recorded receipts for the 4-vCPU BIOS SMP lane (`--lpc=ich9
--smp=4`):
  * SHA256SUMS manifest integrity across the whole evidence bundle;
  * diagnostic SeaBIOS POST (`seabios-diag-smp4.stdout.txt`) wakes all 3 APs
    over broadcast INIT+SIPI and prints `Found 4 cpu(s) max supported 4 cpu(s)`;
  * full BIOS boot (`x86emu-smp4.serial.log.gz`) reaches Stage 2, parses the
    4-CPU MADT, wakes AP #0..#2 (`4 CPU(s) online (3 AP(s) woken)`), logs the
    R5 cross-CPU scheduling receipt, reaches `[kernel] shell active; kmain
    idling.`, and passes both `/tests/irqapwake` (`RES-16 receipt`,
    `IRQAPWAKE PASS`) and `/tests/smpstress` (all three storm assertions +
    `SMPSTRESS PASS`);
  * `x86emu-smp4.stdout.txt` reports `fault=0` and `executed_steps > 0` on all
    4 vCPUs (`vcpu=0..3`) and satisfies the global work invariants
    `sum(executed_steps) == executed`, `sum(instr_count) == vtime`, and
    `executed + idle_quantum == vtime`;
  * `bootinfo-smp4.bin` reports 8 E820 entries (with the ICH9 RCRB window),
    `cpu_count == 4`, `bsp_lapic_id == 0`, `cpus[0..3]` with matching
    processor/LAPIC IDs `0..3`, and a live RSDP equal to the ACPI walk;
  * `acpi-receipt-smp4.txt` has valid checksums, a 134-byte MADT with
    `cpus=4 ioapics=1 isos=4`, and passes `tests/g5_acpi_receipt.py --check`;
  * `nc-smp.txt` records the negative control (`mutant` wakes 0 APs; `control`
    wakes all 3 APs).

Exit: 0 pass, 1 any check fails, 2 usage/IO.
"""
from __future__ import annotations

import argparse
import gzip
import hashlib
import re
import subprocess
import struct
import sys
import tempfile
from pathlib import Path

BOOT_INFO_SIZE = 7776
BOOT_MAGIC = 0x4155524142544C44
MMAP_OFF, MMAP_ENTRY_SIZE, MMAP_COUNT_OFF = 40, 24, 6184
HHDM_OFF, INITRD_PHYS_OFF, INITRD_SIZE_OFF = 6192, 6200, 6208
CPU_COUNT_OFF, BSP_LAPIC_OFF, CPUS_OFF, CPU_ENTRY_SIZE = 6216, 6220, 6224, 24
RSDP_OFF, UEFI_OFF = 7760, 7768
HHDM_VALUE = 0xFFFF800000000000
RCRB = (0xFED1C000, 0x4000)

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


def verify_diag_post(directory: Path) -> None:
    path = directory / "seabios-diag-smp4.stdout.txt"
    if not check(path.is_file(), "seabios-diag-smp4.stdout.txt present"):
        return
    text = path.read_text(errors="replace")
    check("[smp] G6: 4 vCPU contexts live; vcpu1..3 parked in wait-for-SIPI" in text,
          "diagnostic POST wires 4 vCPU contexts")
    for ap in (1, 2, 3):
        check(f"handle_smp: apic_id=0x{ap}" in text,
              f"diagnostic POST wakes AP apic_id=0x{ap}")
    check("Found 4 cpu(s) max supported 4 cpu(s)" in text,
          "SeaBIOS POST reports Found 4 cpu(s) max supported 4 cpu(s)")


def read_serial(directory: Path) -> str:
    plain = directory / "x86emu-smp4.serial.log"
    if plain.is_file():
        return plain.read_text(errors="replace")
    gz = directory / "x86emu-smp4.serial.log.gz"
    if gz.is_file():
        return gzip.decompress(gz.read_bytes()).decode("utf-8", "replace")
    return ""


def verify_serial(directory: Path) -> str:
    text = read_serial(directory)
    if not check(bool(text), "serial log present"):
        return ""
    order = [
        "[smp] G6: 4 vCPU contexts live; vcpu1..3 parked in wait-for-SIPI",
        "[BL3] AuraLite stage2 alive",
        "[BL9] ACPI RSDP found",
        "[BL9] ACPI MADT parsed",
        "[BL4] ELF PT_LOAD segments copied to phys",
        "[BL4] entering long mode; jumping to kernel _start",
        "[smp] BSP lapic_id=0, 4 total CPUs detected",
        "[smp] AP #0 online (lapic_id=1)",
        "[smp] AP #1 online (lapic_id=2)",
        "[smp] AP #2 online (lapic_id=3)",
        "[smp] 4 CPU(s) online (3 AP(s) woken)",
        "[smp] PASS: multi-core system detected",
        "[ioapic] base 0xfec00000 (MADT agree)",
        "[sched] R5 receipt: user thread pid=",
        "[kernel] shell active; kmain idling.",
        "[kbc] --keys-at fired",
        "[smpwake] PASS:",
        "RES-16 receipt",
        "IRQAPWAKE PASS: kernel receipt returned 0",
        "SMPSTRESS: O_APPEND 6 x 200 x 32 B intact",
        "SMPSTRESS: 100 fork/wait cycles precise",
        "SMPSTRESS: 8 x 10 signal deliveries counted",
        "SMPSTRESS PASS",
    ]
    pos = [text.find(m) for m in order]
    missing = [m for m, p in zip(order, pos) if p < 0]
    check(not missing, "every G6 boot/SMP/test marker present", str(missing))
    if not missing:
        check(pos == sorted(pos), "boot, AP wake, irqapwake, and smpstress markers in order")
    check("panic" not in text.lower(), "no kernel panic in the serial log")
    check("did not respond" not in text.lower(), "no AP timeout in smp_init()")
    r5 = re.search(r"\[sched\] R5 receipt: user thread pid=(\d+) on AP cpu=(\d+)", text)
    check(r5 is not None and int(r5.group(2)) in (1, 2, 3),
          "R5 receipt scheduled a user thread on an AP",
          r5.group(0) if r5 else "missing")
    return text


def verify_stats(directory: Path) -> None:
    path = directory / "x86emu-smp4.stdout.txt"
    if not check(path.is_file(), "stats stdout present"):
        return
    text = path.read_text(errors="replace")
    vcpus = {}
    for m in re.finditer(
        r"\[stats\] vcpu=(\d+) executed_steps=(\d+) instr_count=(\d+) "
        r"halted=(\d+) fault=(\d+)", text
    ):
        idx = int(m.group(1))
        vcpus[idx] = {
            "steps": int(m.group(2)),
            "instr": int(m.group(3)),
            "halted": int(m.group(4)),
            "fault": int(m.group(5)),
        }
    check(sorted(vcpus.keys()) == [0, 1, 2, 3], "stats has vcpu=0..3",
          str(sorted(vcpus.keys())))
    for idx in range(4):
        if idx in vcpus:
            check(vcpus[idx]["fault"] == 0, f"vcpu={idx} fault=0",
                  str(vcpus[idx]["fault"]))
            check(vcpus[idx]["steps"] > 0, f"vcpu={idx} executed_steps > 0",
                  str(vcpus[idx]["steps"]))

    g = re.search(
        r"\[stats\] executed=(\d+) idle_quantum=(\d+) "
        r"instr_count_bsp=(\d+) vtime=(\d+)", text
    )
    check(g is not None, "global stats line present")
    if g and len(vcpus) == 4:
        executed, idle_q, bsp_instr, vtime = (int(x) for x in g.groups())
        sum_steps = sum(v["steps"] for v in vcpus.values())
        sum_instr = sum(v["instr"] for v in vcpus.values())
        check(sum_steps == executed, "sum(vcpu executed_steps) == executed",
              f"{sum_steps} == {executed}")
        check(sum_instr == vtime, "sum(vcpu instr_count) == vtime",
              f"{sum_instr} == {vtime}")
        check(executed + idle_q == vtime, "executed + idle_quantum == vtime",
              f"{executed} + {idle_q} == {vtime}")
        check(vcpus[0]["instr"] == bsp_instr, "vcpu0 instr_count == instr_count_bsp",
              f"{vcpus[0]['instr']} == {bsp_instr}")

    cap = re.search(r"\[stats\] cap=(\d+) cap_reached=(\d+) n_vcpus=(\d+)", text)
    check(cap is not None, "cap/n_vcpus line present")
    if cap:
        cap_n, reached, nv = (int(x) for x in cap.groups())
        check(cap_n == 4_000_000_000, "budget is 4 G", str(cap_n))
        check(reached == 1, "run ended by the cap, not by a fault", str(reached))
        check(nv == 4, "n_vcpus=4", str(nv))
    check("mode=long64" in text, "CPU finished in long mode")


def verify_bootinfo(directory: Path) -> None:
    block = directory / "bootinfo-smp4.bin"
    if not check(block.is_file(), "bootinfo-smp4.bin present"):
        return
    raw = block.read_bytes()
    if not check(len(raw) == BOOT_INFO_SIZE, "boot_info_t size", str(len(raw))):
        return
    check(struct.unpack_from("<Q", raw, 0)[0] == BOOT_MAGIC, "BOOT_MAGIC")
    count = struct.unpack_from("<I", raw, MMAP_COUNT_OFF)[0]
    check(count == 8, "E820 has 8 entries under --lpc=ich9", str(count))
    entries = [struct.unpack_from("<QQII", raw, MMAP_OFF + i * MMAP_ENTRY_SIZE)
               for i in range(min(count, 256))]
    check(any((base, length) == RCRB and typ == 2
              for base, length, typ, _ in entries),
          "ICH9 RCRB 0xfed1c000+0x4000 is reserved")
    check(struct.unpack_from("<Q", raw, HHDM_OFF)[0] == HHDM_VALUE, "HHDM offset")
    ip = struct.unpack_from("<Q", raw, INITRD_PHYS_OFF)[0]
    isz = struct.unpack_from("<Q", raw, INITRD_SIZE_OFF)[0]
    check(ip == 0x1800000 and isz == 0xB22000, "initrd address and size")

    cpu_count = struct.unpack_from("<I", raw, CPU_COUNT_OFF)[0]
    bsp_id = struct.unpack_from("<I", raw, BSP_LAPIC_OFF)[0]
    check(cpu_count == 4, "boot_info_t cpu_count is 4", str(cpu_count))
    check(bsp_id == 0, "BSP APIC id is 0", str(bsp_id))
    for i in range(4):
        pid, lid, goto_addr, extra = struct.unpack_from(
            "<IIQQ", raw, CPUS_OFF + i * CPU_ENTRY_SIZE
        )
        check(pid == i and lid == i and goto_addr == 0 and extra == 0,
              f"boot_info_t cpu[{i}] has processor_id={i} lapic_id={i}")

    rsdp = struct.unpack_from("<Q", raw, RSDP_OFF)[0]
    check(rsdp != 0, "boot_info_t carries a live RSDP", f"{rsdp:#x}")
    check(raw[UEFI_OFF] == 0, "boot_from_uefi is 0")
    check(not any(raw[UEFI_OFF + 1:]), "reserved padding is zero")
    receipt = (directory / "acpi-receipt-smp4.txt").read_text().splitlines()
    at = int(re.search(r"rsdp addr=0x([0-9a-f]+)", receipt[0]).group(1), 16)
    check(rsdp == at, "handoff RSDP == RSDP found in memory",
          f"{rsdp:#x} vs {at:#x}")


def verify_acpi(directory: Path, tool: Path) -> None:
    receipt = directory / "acpi-receipt-smp4.txt"
    if not check(receipt.is_file(), "acpi-receipt-smp4.txt present"):
        return
    rtext = receipt.read_text()
    check("table sig=APIC " in rtext and "len=134 checksum=ok" in rtext,
          "MADT length is 134 bytes (4 LAPIC entries) with valid checksum")
    check("madt lapic_base=0xfee00000 cpus=4 ioapics=1 isos=4" in rtext,
          "MADT receipt reports cpus=4 ioapics=1 isos=4")
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
    path = directory / "nc-smp.txt"
    if not check(path.is_file(), "nc-smp.txt present"):
        return
    text = path.read_text()
    mut = re.search(r"mutant:\s+woken=(\d+)\s+found=(\S+)\s+ap_steps=([0-9,]+)", text)
    ctl = re.search(r"control:\s+woken=(\d+)\s+found=(\S+)\s+ap_steps=([0-9,]+)", text)
    check(mut is not None and mut.group(1) == "0" and mut.group(3) == "0,0,0",
          "mutant without broadcast INIT/SIPI wakes 0 APs")
    if ctl is not None:
        steps = [int(x) for x in ctl.group(3).split(",")]
        check(ctl.group(1) == "3" and ctl.group(2) == "4/4" and all(s > 0 for s in steps),
              "control wakes all 3 APs and reports Found 4/4")
    else:
        check(False, "control line present in nc-smp.txt")


def main(argv: list[str]) -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--evidence", type=Path,
                    default=Path(__file__).resolve().parent.parent /
                    "docs/evidence/compat-g6")
    args = ap.parse_args(argv)
    d = args.evidence
    if not d.is_dir():
        print(f"no such evidence directory: {d}", file=sys.stderr)
        return 2
    verify_manifest(d)
    verify_diag_post(d)
    verify_serial(d)
    verify_stats(d)
    verify_bootinfo(d)
    verify_acpi(d, Path(__file__).resolve().parent / "g5_acpi_receipt.py")
    verify_negative_control(d)
    if FAILS:
        print(f"\n{len(FAILS)} check(s) failed", file=sys.stderr)
        return 1
    print("\nG6 acceptance: PASS")
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
