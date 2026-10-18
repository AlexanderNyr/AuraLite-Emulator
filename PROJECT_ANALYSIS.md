# AuraLite-Emulator — Project Analysis (current tree)

> Prepared from a full audit of the repository at `main` HEAD `39f83f9`
> ("G0-G3 update", 2026-10-16) plus the local COMPAT G4 change set
> (`--lpc=ich9`, `tests/test_compat.c`, `docs/plans/COMPAT_PLAN.md`).
> Everything below was built and measured in this tree: `make test` is green,
> the full boot chain completes with exit code 0, and the guest painted the
> framebuffer.

---

## 1. What this project is

**AuraLite-Emulator** ("Universal x86 Emulator" in the README) is a
from-scratch x86 emulator written in portable C11 with no QEMU/KVM and no
external libraries beyond `-lm`. Its purpose is executing raw x86 firmware
(BIOS/boot-firmware style): the emulator loads a ROM at the reset vector and
runs it from real mode through protected mode into 64-bit long mode, while
behaviourally emulating an Intel chipset (PCI/ECAM, EHCI/UHCI USB, AHCI, e1000,
VGA + Bochs VBE, DDR controllers, PIC/PIT/RTC/KBC/LAPIC/IOAPIC).

Design philosophy: **the emulator is firmware-agnostic** — the CPU core
contains no hacks tied to a specific ROM; platform specifics live in
switchable profiles (`--platform=...`) and ROM compatibility belongs in the
ROM images themselves.

Scale of the current tree:

| Area | Files | Lines (approx.) |
|---|---|---|
| `src/` | 50 (26 `.c` + 24 `.h`) | 16,065 |
| `tests/` (+ `integration/`) | 31 `.c`, 11 `.py`, 28 `.sh` | 11,000 |
| `docs/` incl. 10 plans | — | 11,400 |
| `patches/` phase artifacts | 26 `.patch` (0008–0065) | — |

## 2. Repository layout

```
AuraLite-Emulator/
├── src/
│   ├── cpu.c / cpu.h          — the x86 interpreter (2,971 lines), the core
│   ├── mem.c                  — physical address space, Q35 PAM, MMIO decode
│   ├── io.c                   — 64K I/O port space
│   ├── pci.c / pci.h          — PCI mechanism #1 + ECAM/MMCONFIG
│   ├── devices.c / .h         — GPU/VRAM, VGA text, Bochs VBE, EHCI+UHCI,
│   │                            DDR/SPD, CAR, COM1 debugcon
│   ├── platform.c / .h        — five Intel CPUID profiles
│   ├── chipset.c              — A20, port 0x92, reset via 0xCF9
│   ├── pic.c / pit.c / pm.c / rtc.c / kbc.c — 8259A, 8254, ACPI PM
│   │                          block (PMBASE TMR, G5), MC146818A, 8042
│   ├── lapic.c / ioapic.c     — xAPIC and 82093AA
│   ├── ahci.c                 — SATA HBA: FIS/PRDT engine, two controllers
│   ├── e1000.c                — 82540EM model + TAP-over-UDP socket
│   ├── serial.c               — 16550 subset
│   ├── acpi.c                 — RSDP/RSDT/XSDT/MADT set for the --kernel lane
│   ├── kloader.c              — ELF64 kernel loader + boot_info_t fabrication
│   ├── disasm.c               — 16/32/64-bit disassembler (NASM syntax)
│   ├── dbg.c                  — run-control core, REPL, symbols, expressions
│   ├── gdbstub.c              — GDB Remote Serial Protocol frontend
│   ├── snapshot.c             — AURASNP1 whole-machine snapshots
│   ├── ws.c / monitor.c       — SHA-1 + RFC 6455 + HTTP/WS live monitor
│   ├── machine.h              — machine_t: memory map, devices, vCPU slots,
│   │                            PERF caches, event log ring
│   └── main.c                 — CLI harness, run loop, dumps
├── firmware/
│   ├── sample_firmware.nas    — GenericBootFirmware v0.2.18 (NASM)
│   └── LICENSE                — MIT (c) 2026 $yscall — firmware only
├── disk/test_kernel.asm       — 64-bit payload painted into the framebuffer
├── tests/                     — unit suites, harness, fuzzers, integration
├── tools/                     — x86dis, tap_responder.py
├── web/monitor.html           — live WebSocket monitor UI (served by --monitor)
├── docs/plans/                — CORE, CHIPSET, KERNEL_BOOT, STORE, USB,
│   │                            VIDEO, NET, PERF, TOOLING, COMPAT
│   └── evidence/              — compat-g2, compat-g3, compat-g4, compat-g5, compat-g6 bundles
├── patches/                   — one format-patch per delivered phase
├── Makefile                   — build, firmware, disk, tests, bench, fuzz
├── CHANGELOG.md / ROADMAP.md / STATUS.md / BUGS.md
└── .github/workflows/build.yml — CI: build, ASan+UBSan, differential fuzz
```

Dependencies: **GCC/Clang, NASM, Python 3, GNU Make**; optional **gdb** and
**ndisasm** for the debugging/differential lanes. Verified locally with
gcc 14.2.0, make 4.4.1, NASM 2.16.03, Python 3.13.16, gdb 16.3.

## 3. Architecture by module

### 3.1 `cpu.c` — the x86 interpreter

- **Modes**: real16 → prot16/prot32 → compat32 → long64, derived from
  `CR0.PE`, `CR0.PG`, `EFER.LME/LMA` and the `L`/`D` bits of CS.
- **Registers**: 16 GPRs, RIP, RFLAGS, six segments (sel/base/limit/D-B/L/P/
  type), CR0/2/3/4, EFER, GDTR/IDTR, TR, up to 64 MSRs; SSE media file
  (16×128-bit XMM) for the scalar/packed moves the ring-3 guest uses.
- **Decoding**: generic ModRM/SIB over all addressing modes, 0x66/0x67/REX/
  segment/REP/LOCK prefixes, lazy RIP-relative resolution after the trailing
  immediate is known.
- **Paging**: 4-level walk, 2 MiB / 1 GiB large pages, U/S + R/W + NX +
  CR4.SMEP/SMAP enforcement, architectural `#PF` error codes, `#DF` guard.
- **Exceptions**: IVT in real mode, IDT gates otherwise, IST stack switching
  through the TSS (`ISTn = TSS+0x24+(n-1)*8`), ring-3 → ring-0 stack switch,
  hardware frame order `[SS,RSP,] RFLAGS, CS, RIP, err`.
- **Interrupt priority**: LAPIC first, then 8259; STI/MOV-SS shadow; HLT wake
  by line level even with IF=0.
- **ISA**: the firmware/OS integer set is closed (ALU, MOV family, Jcc/CMOVcc/
  SETcc, shifts/rotates, MUL/DIV family, BT/BTS/BTR/BTC, BSF/BSR, BSWAP,
  XADD, CMPXCHG, LOOP/JCXZ, string ops + REP, IN/OUT, system instructions,
  CPUID/RDTSC/MSR, SYSCALL/SYSRET, SWAPGS); SSE is deliberately a measured
  subset; anything else raises `#UD` instead of silently misbehaving.
- **Performance caches** (PERF P2/P3): 256-entry success-only translation TLB
  and a 512-slot RAM-only prefix/opcode predecode, both keyed on the full
  security context and invalidated by one global RAM-write generation. Only
  successful walks are cached; faults always take the original path.

### 3.2 `mem.c` — physical address space

128 MiB RAM + 128 KiB ROM, right-aligned in its window (reset vector at
`0xFFFFFFF0`), aliases at `0xE0000` and `0xFFFE0000`. The Q35 host bridge's
seven PAM bytes are the single source of truth for the `C0000–FFFFF` decoder;
writes go to the RAM shadow, reads fall back to immutable flash or open bus.
MMIO regions are a linked list where the **smallest matching region wins**,
mirroring uncore precedence of fixed BARs over the 256 MiB ECAM window. A20
masks bit 20 for reads, writes and fetches. `mem_debug_peek_byte()` is the
side-effect-free, PAM-aware inspection path used by tools.

### 3.3 Chipset

8259A pair (full ICW/OCW, priorities, spurious IRQs, level lines, AEOI), 8254
PIT and MC146818A RTC on a virtual instruction-count timebase (determinism),
8042 KBC with scancode set-1 injection, A20 gate and four reset sources,
xAPIC with 256-bit IRR/ISR and timers, 82093AA I/O APIC with remote-IRR
feedback, and a 16550-subset COM1.

### 3.4 Devices

GPU VRAM at `0xD0000000` (800×600×32, doubles as the PPM framebuffer), VGA
text mode 3 at `0xB8000` with the guest's own 8×16 PSF2 font, Bochs VBE dispi
with QEMU's `vbe_fixup_regs` transcribed in full (the V4 heap-overflow fix),
EHCI + UHCI with BOT/SCSI mass storage, AHCI with command-list/FIS engine and
up to two controllers, e1000 with TAP-over-UDP ingress/egress, DDR3/DDR4
training registers, and cache-as-RAM.

### 3.5 Tooling

`--trace-file` with a CPU-vs-disassembler successor self-check; `x86dis`;
run-control REPL (`--debug`, `--debug-script`, `--symbols`); gdbstub
(`--gdb`, target.xml, threads, hardware watchpoints); v2 snapshots with shape
refusals; live HTTP/WebSocket monitor with framebuffer deltas. All of it is
opt-in and non-perturbing: stdout/`--stats`/PPM/log stay byte-identical to a
plain run, and each tool has a negative control.

## 4. Boot paths

1. **Firmware** (`--rom=`, default): reset → `0xE000:0` alias → protected
   mode → CPUID dispatch across five chipset paths → ECAM/PAM setup → DDR
   training → GPU BAR → page tables → long mode → USB READ(10) → jump to
   `0x00100000`. Measured: `GUEST_OK`, framebuffer marker, `HALTED` at
   4,837,677 instructions (Haswell).
2. **Direct kernel** (`--kernel=`, K1–K8): ELF64 `ET_EXEC`, segments placed by
   `p_paddr − KERNEL_VMA`, seven-page paging hierarchy, fabricated
   `boot_info_t` + ACPI set, parked at `_start`. This is the lane that meets
   the north star: AuraLite OS to its shell on 1 and 2 vCPUs.
3. **BIOS compatibility** (COMPAT G0–G6, closed): SeaBIOS → Stage 1 → Stage 2 →
   flat kernel. G2 fixed the real-mode `SGDT/SIDT` store width; G3 diagnosed
   and closed the Stage-2 FS/GS unreal-mode assumption; G4 makes the LPC
   identity pair with the Q35 host bridge (`--lpc=ich9`) so stock SeaBIOS
   publishes `RSDP/RSDT/FACP/SSDT/MADT/MCFG`; G5 models the ACPI PM timer at
   `PMBASE+8` so the ICH9 lane boots to `shell active` with an 8-entry E820 and
   live RSDP; G6 scales `--smp=N` up to 8 vCPUs (`CMOS[0x5F]`, broadcast
   INIT/SIPI, `CPUID(1).EBX[31:24]` APIC ID, AP counter preservation across
   second-stage INIT) and boots `--lpc=ich9 --smp=4` through `irqapwake` and
   `smpstress`.

## 5. Build and verification

```
make                         # -std=c11 -O2 -Wall -Wextra -Werror, zero warnings
make firmware/firmware.bin disk/disk.img x86dis
make test                    # full gate, ~6.5 min, green
make test-sanitize           # ASan+UBSan as hard errors
make fuzz                    # differential vs the host CPU via ptrace
make test-compat             # COMPAT G4 identity vectors (7/7)
```

Measured in this tree: `make test` green; `test-gdbstub` 30/30 and real
`gdb -batch` lanes green; fuzz 32 programs / 384 instructions all matching,
0 host crashes; ASan spot-runs of `test_table` and `test_snapshot` clean;
full boot 4,837,677 steps in 270 ms median (~17.9 Msteps/s); snapshot split
loads byte-identical; TLB 6,738,321 hits.

## 6. Findings, quirks and limits

### Deliberate scope

- Not a drop-in QEMU replacement; ISA grows from traces and tests.
- SSE is a measured subset, x87 has control/status state only (no data model),
  no AVX, no SMM/VMX/SVM/NMI.
- Segment limit/CPL checks are modelled where the guest path needed them, not
  as a complete protection unit.
- Up to eight vCPU contexts (`EM_MAX_VCPU = 8`), strict 1:1 round-robin.
- USB is EHCI/UHCI with a BOT/SCSI subset; no xHCI.
- Networking is IPv4-only TAP-over-UDP, DHCP stateless, DNS A/IN only, no
  TCP window scaling/SACK/TIME_WAIT, e1000 INTx unmounted.
- VRAM is 1.9 MB (not QEMU's 16 MiB); no display panning/`vbe_start_addr`;
  BPP 4 is a documented divergence.
- The live monitor has no authentication; loopback is the default bind.

### Known documentation/code notes (state after this pass)

- `NOTICE.md` now matches the tree: Apache-2.0 at the root (`LICENSE`), MIT
  for `firmware/` only.
- `PROJECT_ANALYSIS.md` (this file) is current; the earlier revision described
  commit `33e2c5c` and a ~1,500-line core.
- `docs/plans/COMPAT_PLAN.md` now exists and records G0–G6 with measured
  receipts and explicit non-claims.
- `cpu.h` documents the K6 SMP invariant that `cpu_step()` always operates on
  `m->cpu` (the current context), which the internal helpers rely on; the
  merged `kgs_base`/XMM comment block was split.
- `[pci-probe]` / `[ahci-mmio-probe]` lines in the log are diagnostic
  instrumentation from the G2 era and are part of the recorded receipts;
  they are intentionally left in place.
- The sample firmware's device contract (LPC `8086:1e55`) is unchanged; the
  ICH9 identity is an explicit `--lpc=ich9` opt-in.

## 7. Bottom line

A compact, readable, **fully working** DIY PC emulator: reset vector to a
64-bit guest kernel, real USB mass-storage boot, graphics, networking, SMP and
source-level debugging. Its distinguishing feature is the evidence discipline:
every plan phase ships with its implementation, a regression vector and a
negative control, and `docs/STATUS.md` records receipts rather than promises.
All ten plans in `ROADMAP.md` (CORE, CHIPSET, KERNEL_BOOT, STORE, USB, VIDEO,
NET, PERF, TOOLING, COMPAT G0–G6) are delivered and gated by `make test`.
