# AuraLite-Emulator — Full Project Analysis

> Prepared from a complete audit of the repository at commit `33e2c5c` (`main`).
> All artifacts were built and verified locally: `make test` is green, the full
> boot chain completes with exit code 0, and the guest payload paints the
> framebuffer.

---

## 1. What this project is

**AuraLite-Emulator** (called "Universal x86 Emulator" in the README) is a
from-scratch x86 emulator written in portable C11 (~1500 lines of core code),
with no dependency on QEMU/KVM. Its purpose is executing raw x86 firmware
(BIOS/boot-firmware style): the emulator loads a ROM at the reset vector and
runs it from real mode through protected mode into 64-bit long mode, while
behaviourally emulating an Intel chipset (PCI, MMIO, EHCI/USB, GPU framebuffer,
DDR controllers).

Design philosophy: **the emulator is firmware-agnostic** — the CPU core contains
no hacks tied to a specific ROM; platform specifics live in switchable profiles
(`--platform=...`) and ROM compatibility belongs in the ROM images themselves.

## 2. Repository layout

```
AuraLite-Emulator/
├── src/
│   ├── cpu.c / cpu.h      — the x86 interpreter (780 lines, the core)
│   ├── mem.c              — physical address space + MMIO dispatcher
│   ├── io.c               — 64K I/O port space
│   ├── pci.c / pci.h      — PCI mechanism #1 (0xCF8/0xCFC) + ECAM/MMCONFIG
│   ├── devices.c / .h     — "the chipset": LPC, SATA, GPU, EHCI/USB-MSC, COM1, CAR, DDR
│   ├── platform.c / .h    — 5 CPUID profiles for Intel generations
│   ├── machine.h          — machine_t, memory map, event log ring
│   └── main.c             — CLI harness, register dump, PPM framebuffer export
├── firmware/
│   ├── sample_firmware.nas — sample firmware in NASM, 848 lines (GenericBootFirmware v0.2.18)
│   └── LICENSE             — MIT (c) 2026 $yscall — covers the firmware only
├── disk/test_kernel.asm   — 64-bit test payload (the guest kernel)
├── tests/test_cpu.c, test_usb.c — unit tests
├── web/monitor.html       — static web monitor mock (loads PPM + log files)
├── docs/STATUS.md, BUGS.md — implementation status & compatibility notes
├── Makefile               — build, firmware, disk, tests
└── .github/workflows/build.yml — CI (ubuntu + gcc + nasm + make test)
```

Dependencies: **GCC/Clang, NASM, Python 3, GNU Make** — all installed and verified.

## 3. Architecture by module

### 3.1 `cpu.c` — the x86 interpreter (core)

- **Modes**: real16 → prot16/prot32 → compat32 → long64, derived from
  `CR0.PE`, `CR0.PG`, `EFER.LME/LMA` and the `L`/`D` bits of the CS descriptor.
- **Registers**: 16 GPRs (RAX–R15), RIP, RFLAGS, 6 segments (selector/base/limit/L/D),
  CR0/CR2/CR3/CR4, EFER, GDTR/IDTR, up to 64 MSRs.
- **Decoding**: a generic ModRM/SIB decoder covering all 16/32/64-bit addressing
  modes, prefixes (0x66/0x67/REX/segment/REP/LOCK), RIP-relative addressing via a
  deferred fixup (instruction length is only final after decode — see `fixup_riprel`).
- **Paging**: 4-level translation PML4→PDPT→PD→PT with 2 MiB and 1 GiB large pages.
- **Exceptions**: `raise_exception()` with a re-entrancy guard (double fault →
  halt), IDT gate delivery, `#PF` with read=0/write=2 error codes, `#UD` for
  unknown opcodes.
- **ISA subset** (firmware-focused):
  - the full ALU group ADD/OR/ADC/SBB/AND/SUB/XOR/CMP in all forms with correct
    CF/OF/ZF/SF/PF/AF;
  - MOV of all forms, MOVZX, LEA, MOV moffs, MOV CRn, RDMSR/WRMSR, CPUID;
  - INC/DEC, PUSH/POP, CALL/RET, JMP near/short/far, far call/jmp through memory,
    Jcc short+near, shift group (SHL/SHR/SAR), group3 (TEST/NOT/NEG),
    group5 (INC/DEC/CALL/JMP/PUSH/far), IN/OUT, REP STOS, NOP/HLT/CLI/STI/CLD/STD,
    LGDT/LIDT.
- **Two-pass decode**: fast ALU switch first; group1 restarts decoding from
  `d.start_pc`. `--trace` prints mode+RIP+opcode per instruction.

### 3.2 `mem.c` — physical address space

- 128 MiB RAM + 128 KiB ROM. The ROM is **right-aligned** in its window (last byte
  at the top), like a real flash part: the reset vector is always at 0xFFFFFFF0.
- Two ROM aliases: `0xE0000` (legacy BIOS shadow) and `0xFFFE0000` (top-of-4GB).
- MMIO regions are a linked list with read/write callbacks. **Smallest matching
  region wins** — mirroring how a real uncore decodes narrow fixed BARs ahead of
  the wide 256 MiB ECAM window (0xE0000000–0xF0000000).
- Open bus: reads return all-ones, writes are ignored.
- `mlog()` — a 4096-line ring log (for a future GUI), mirrored to stderr.

### 3.3 `io.c` — I/O ports

A 65536-entry table of {read, write, ctx, name}; unregistered ports read as
0xFF/0xFFFF/0xFFFFFFFF.

### 3.4 `pci.c` — PCI

- Mechanism #1: ports 0xCF8 (CONFIG_ADDRESS, enable bit 0x80000000) / 0xCFC.
- ECAM/MMCONFIG: a 256 MiB window, decoding `bus<<20|dev<<15|func<<12|reg`.
- Host bridge (0:0:0): writing register 0x60 (PCIEXBAR-style) with bit 0 enables
  the ECAM window — exactly how the sample firmware initializes MMIO config space.
- Config write policy: ID/class/header-type are read-only, everything else
  (BARs, command, status) is writable (behavioural, not bit-exact).

### 3.5 `devices.c` — "the rest of the chipset"

- **PCI devices** sit at the same bus:dev:func the firmware probes (confirmed by
  the firmware source): host-bridge 0:0:0, pci-bridge-misc 0:1:0, iGPU 0:2:0
  (class 0x03 mirrored at both 0x0B and 0x0C — the firmware reads the class from
  the non-standard offset 0x0C!), EHCI 0:3:0 (BAR0=0xFEB00000), LPC 0:31:0,
  SATA/AHCI 0:31:2.
- **EHCI/USB Mass Storage** — the most elaborate model: any write to the EHCI BAR
  rings a "doorbell" → reads ASYNCLISTADDR (+0x18) → walks the QH/qTD chain
  (up to 32 entries) → interprets genuine Bulk-Only Transport:
  - qTD with PID=OUT and 31 bytes = CBW → parse 'USBC' signature and SCSI opcode;
    READ(10)/READ(12) → remember LBA/block count (kept in static state);
  - qTD with PID=IN: with a pending command — copy sectors from the disk image
    into the guest buffer (with RAM bounds checks); otherwise — synthesize a
    'USBS' CSW with status OK. The qTD token is then marked complete.
- **GPU VRAM**: window at 0xD0000000, 800×600×32bpp; doubles as the PPM-exported
  framebuffer.
- **CAR** (cache-as-RAM) scratch at 0xFEF00000, 4 MiB — the firmware's early stack.
- **Per-platform DDR models** (`devices_init_platform`): Sandy/Ivy/Bay Trail get
  an SPD window at 0xE00FB000 (byte 0 = "ready" bit, byte 5 = density code);
  Haswell/Broadwell get a DDR4 controller at 0xFED10000 whose **busy bit
  0x80000000 auto-clears on read** — the firmware's polling loop sees an
  instant "training done".
- **COM1 (0x3F8)**: line-buffered serial output into the log (`[serial] GUEST_OK`).

### 3.6 `platform.c` — CPUID profiles

5 profiles with real Intel CPUID.1.EAX signatures the firmware branches on:
Sandy Bridge 0x206A7 (0x2A), Ivy 0x306A9 (0x3A), Haswell 0x306C3 (0x3C, default),
Broadwell 0x306D4 (0x3D), Bay Trail 0x30678 (0x37). CPUID.0 returns
"GenuineIntel", 0x80000001.EDX has the long-mode bit.

### 3.7 `main.c` — CLI

Options: `--rom=`, `--disk=`, `--platform=`, `--max-instr=`, `--trace`,
`--dump-fb=`, `--log=`. Exit codes: **0** normal/instruction-limit stop,
**2** CPU fault, **1** input-file error. Dumps all registers/mode, the log tail,
tracks "guest entered at 0x100000" and the framebuffer marker
`pixel[0]==0x00200000`.

## 4. The sample firmware and the boot chain

GenericBootFirmware v0.2.18, 128 KiB, org 0xFFFE0000. The full execution path,
confirmed by tracing and tests:

1. **Reset**: CS:IP = 0xF000:0xFFF0 → physical 0xFFFFFFF0 → the last ROM bytes:
   `jmp far 0xE000:0` → legacy alias 0xE0000 → `startcli`.
2. **Real mode**: load a 3-entry GDT, set `CR0.PE`, far jump → **prot32**.
3. **prot32**: stack in CAR (0xFEF0FFFC), `CPUID.1` → hand-rolled family/model
   decode → branch to 5 init paths (Sandy/Ivy: CAR+MTRR+PCI+DDR3-SPD; Haswell:
   ECAM+LPC+PMBASE+CAR+DDR4-polling; Broadwell/Bay Trail: variants). Each path
   enables MMCONFIG via host bridge 0x60, "trains" DDR, pokes CMOS/SuperIO.
4. **Common tail `loff`**: SATA stubs, speaker, GPU discovery by scanning ECAM
   (class read at offset 0x0C!), BAR assignment 0xD0000000, HDMI MMIO writes.
5. **Long mode**: build 4-level page tables at 0x10000 (identity map incl. a
   1 GiB PDPTE covering MMIO and 2 MiB pages for APIC at 0xFEC00000/0xFEE00000),
   CR3, EFER.LME, CR0.PG → far jump into a 64-bit segment → **long64**.
6. **long64**: fill the IDT (256 gates to `fault: hlt`), LIDT, SuperIO/COM.
7. **USB boot**: read the EHCI BAR via PCI#1 (0x80001810), build a QH at 0x20000
   and a qTD chain, CBW READ(10) LBA=0, one 512-byte block into **0x00100000**,
   data-IN, CSW → poll the Active bit.
8. **Handover**: `mov rax,0x00100000; jmp rax` — the guest takes over.
9. **Guest (`disk/test_kernel.asm`)**: writes "GUEST_OK" to COM1, paints the
   800×600 framebuffer with a gradient (pixel = col | row<<8 | 0x00200000), hlt.

`launch_mode db 3` selects the guest launch mode (0=halt, 2=32-bit, 3=64-bit);
default 3 — jump to 0x100000 in long mode.

## 5. Build and verification (performed locally)

```
make          → cc -std=c11 -O2 -Wall -Wextra  (zero warnings)
make firmware/firmware.bin disk/disk.img
make test     → ✅ unit tests: ok  ✅ usb tests: ok  ✅ boot integration test: ok
                ✅ platform smoke tests x5
./x86emu --platform=haswell --max-instr=600000 --dump-fb=framebuffer.ppm
     exit 0; log: ECAM enabled → READ(10) LBA=0 → bulk-IN 512B → CSW OK
     → guest entry 0x100000 → [serial] GUEST_OK → framebuffer marker OK
     → mode long64, CR3=0x10000, EFER=0x500 (LME+LMA)
```

Framebuffer after 600k instructions: the guest painted ~70 top rows — visual
proof of real guest execution (the rest is the instruction limit, not an error).

## 6. Findings — deviations, quirks, limits

### Notable deviations from real x86 (addressed by ROADMAP v0.3–v0.5)

1. **Near Jcc 0F 80–83 missing → #UD**; **0F 88–8B (JS/JNS/JP/JPE/JPO) decode but
   never branch** (silent logic corruption). *(fixed in patch 0002)*
2. **Exception stack frame pushed in the wrong order** (err→CS→RIP→RFLAGS vs
   hardware's RFLAGS→CS→RIP→err); no IRET. *(fixed in patch 0006)*
3. **64-bit shifts**: count mask is 31 instead of 63. *(fixed in patch 0003)*
4. **group3** lacks MUL/IMUL/DIV/IDIV → #UD. *(fixed in patch 0004)*
5. **group2** lacks ROL/ROR/RCL/RCR and the 8-bit forms 0xC0/0xD0/0xD2.
   *(fixed in patch 0003)*
6. `PUSH imm` (0x68) always decodes imm32 — wrong for 16-bit operand size.
   *(fixed in patch 0005)*
7. No CPL/privilege checks, no segment limit checks, no TSS/IST, no IVT delivery
   in real mode (any real-mode exception without an IDT halts). *(IVT added in
   patch 0006; CPL/limits planned for v0.5)*
8. No CPL, APIC, PIC/PIT/RTC, FPU/SSE; paging lacks accessed/dirty/NX/U-S checks;
   REP STOS is atomic; no hardware interrupts at all.

### Minor code notes

- `main.c`: the `guest_entry_seen` check is duplicated before/after `cpu_step`;
  `machine_t.max_instructions` is unused (the limit is kept local in main).
- `devices.c`: USB state (`s_lba`/`s_blocks`/`s_have`) and `g_ehci`/`g_serial`
  are file/global statics → one machine per process; `superio_idx`/`cmos_idx`
  are dead code; CMOS/SuperIO ports aren't registered (open bus suffices for
  the sample firmware).
- In the 20000-instruction smoke test Haswell shows `prot32`: the firmware is
  still inside the DDR4 delay loop (~30k iterations) — expected, not a bug.
- `Makefile clean` has a duplicated `rm` line.
- **Licensing**: the core is Apache-2.0 (root LICENSE); `firmware/` is MIT and
  keeps its own license; `NOTICE.md`'s "no repo-level license" note is outdated.
- `web/monitor.html` is a static mock with hardcoded register values, but it can
  load a PPM framebuffer and a text log client-side.

### Author-declared limits (docs/STATUS.md)

The ISA grows "from execution traces and tests", the chipset is behavioural
(not cycle-accurate), and the CSW/descriptor path plus the web monitor are
still being expanded.

## 7. Bottom line

A compact, readable, **fully working** DIY PC emulator: from the reset vector to
a 64-bit guest kernel with real USB mass-storage boot and graphics. The layering
(CPU/memory/IO/PCI/devices/profiles/CLI) is clean and the code is well annotated
with hardware rationale. Growth per `ROADMAP.md` and `docs/plans/CORE_PLAN.md`
(written in the AuraLite OS house plan style: dependency-ordered phases,
definition of done and test gate per phase, one `.patch` per phase): the CPU
correctness ledger is being closed first (C0–C6 landed), then the chipset
interrupt hardware (PIC/PIT/RTC/A20), real AHCI, and performance — with the
**north star of booting the AuraLite OS kernel to its shell** on this emulator
(long mode, TSS, SYSCALL, PIC/LAPIC, AHCI, e1000, framebuffer).
