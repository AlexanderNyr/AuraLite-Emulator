# CHIPSET Plan

**Status: IN PROGRESS — H0 done ✅, H1–H7 planned 📋**

| Phase | Scope | Status | Deliverable |
|---|---|---|---|
| H0 | INTR delivery scaffold at instruction boundaries + PIC 8259 sketch (init, mask, vector via INTA, EOI, cascade) | ✅ done | `patches/0013-H0-intr-scaffold.patch` |
| H1 | PIC correctness deep-cut: priorities/rotation, special mask mode, spurious IRQ7/15, poll command, level-vs-edge, ICW4 modes | 📋 planned | `patches/0014-H1-pic-full.patch` |
| H2 | PIT 8254: counter 0 (periodic + one-shot semantics), counter 2 + port 0x61 beeper bits, virtual-time tick source | 📋 planned | `patches/0015-H2-pit.patch` |
| H3 | RTC/CMOS: clock registers, update-ended interrupt (IRQ8), CMOS RAM bytes the BIOS area reads (0x10-0x2F equipment/memory size) | 📋 planned | `patches/0016-H3-rtc.patch` |
| H4 | KBC 8042: status/command/data ports + IRQ1 delivery; scancode set-1 injection path; gate A20 command consumed by H5 | 📋 planned | `patches/0017-H4-kbc.patch` |
| H5 | A20 gate (KBC + port 0x92 fast-A20, both must agree), port 0x92 reset, port 0xCF9 reset | 📋 planned | `patches/0018-H5-a20.patch` |
| H6 | LAPIC: MMIO at 0xFEE00000, SVR, TPR, ICR/self-IPI, timer; CPUID/APICBIT already claimed — deliver before guests rely on it | 📋 planned | `patches/0019-H6-lapic.patch` |
| H7 | IOAPIC at 0xFEC00000: 24-entry redirection table writing, delivery to LAPIC; SMP-machinery prereq (MULTI-vCPU is out of scope here) | 📋 planned | `patches/0020-H7-ioapic.patch` |

This document answers: *what does it take for the emulator to deliver real
interrupts the way a PC chipset does, in the order the north-star guest
(AuraLite OS) starts needing them?*

Baseline: commit `b0a4279` (CORE C11). Measured against the tree, not
assumed:

- `grep -iE 'irq|8259|intr' src/*.c src/*.h` → **zero hits**. There is no
  IRQ line model, no PIC, and no hardware-interrupt delivery anywhere.
- `EFLAGS.IF` exists and CLI/STI (C0 baseline had them) modify it — but
  nothing ever SAMPLES it.
- `HLT` sets `cpu.halted`; `cpu_step()` returns −1 forever; no code path
  ever clears `halted` except `cpu_reset()`. A `hlt` with IF=1 is a
  one-way door today.
- The only CMOS/RTC interaction is a silent sink for ports 0x70/0x71
  (devices.c:255-257): index writes are accepted and nothing is answered.
- The sample firmware never touches PIC ports 0x20/0x21/0xA0/0xA1
  (grep of `firmware/sample_firmware.nas`), so introducing the PIC does
  not change the `make test-boot` integration contract.

## The problem, stated once

An emulator whose CPU can never receive an INTR cannot run AuraLite OS past
its very first `sti`: the OS sleeps in `hlt` waiting for the timer, the
keyboard, and the disk — none of which can ever knock. Every downstream plan
(STORE AHCI completion INT, NET e1000 RX INT, COMPAT stage2) needs the same
delivery spine; it is built once here and reused.

## Decisions

- **D1 — one delivery spine for exceptions AND interrupts.** The C5
  `raise_exception()` engine already handles IVT (real mode) and IDT
  (protected/long). PIC delivery re-enters that path with `has_err=0`
  instead of growing a second stack-frame implementation. This is the
  same shape real hardware shares (INTA cycle → vector → identical gate
  walk).
- **D2 — sampling at the instruction boundary, inside `cpu_step()`.**
  Before decode: if an unmasked IRR bit exists, IF=1, and the inhibit
  counter is 0 → `pic_intack()` and raise. STI and MOV/POP SS shadowing
  is a one-instruction delay counter, exactly as silicon does. NMI is
  explicitly out of scope until COMPAT demands it (S5/SMP).
- **D3 — edge-triggered IRR in H0, level-sensitive in H1.** Edge is the
  8259A power-on default and what PIT/KBC/RTC need. Level sensitivity
  (and its re-assertion rules) is a small delta but only meaningful once
  real level-driven sources (PCI INTx sharing) exist — that is H1+.
- **D4 — the PIC is a `machine_t` citizen, not a per-platform device.**
  8259s exist identically on all five platforms (even Haswell/Broadwell
  keep them for legacy mode). One `pic_t` struct, ports 0x20/0x21 and
  0xA0/0xA1 registered in `devices_init_common()`.
- **D5 — H0 numbers pin the PC defaults, proven in tests.** Master base
  vector 0x08, slave 0x70, IMR 0xFF after reset, cascade through master
  IRQ2, fixed priority 0>1>…>7. Tests assert each value measured from a
  raised IRQ, not from struct internals.
- **D6 — tick source is virtual time, owned by H2.** PIT gets the same
  `instr_count * tsc_per_instr` timebase RDTSC uses (C9), so timer IRQ
  cadence is deterministic and testable; wall-clock scaling, if ever,
  belongs in PERF, not here.
- **D7 — LAPIC/IOAPIC do not replace the PIC.** AuraLite OS boots through
  the legacy PIC first (its BL-stage drivers program 8259s), then switches
  to APIC mode for SMP; MSIs come even later. H6/H7 add the APIC world
  WITHOUT touching PIC behavior — guests pick via the usual IMASK/CMP
  (PIC mask-all) transition.
- **D8 — `-Werror` + ASan/UBSan lanes + the table/fuzz rigs from C10–C11
  stay green at every phase boundary.** New IRQ sources immediately get
  crash-invariant coverage automatically (garbage streams now also poke
  PIC ports).

## Per-phase definition of done

### H0 — INTR scaffold + PIC sketch  (`0013`)
**Deliver**: `cpu_step()` samples INTR at the boundary (IF=1, no inhibit);
STI/POP SS one-instruction shadow; PIC 8259 pair with ICW1–4, OCW1 mask,
OCW2 EOI (non-specific + specific), OCW3 IRR/ISR read select, fixed
priority, edge IRR, cascade through master IRQ2, power-on defaults
(0x08/0x70/IMR=0xFF) **measured in tests**; `pic_raise_irq()` public API.
HLT wakes on a pending unmasked IRQ (regardless of IF) and resumes; with
IF=1 the pending vector is taken at the next boundary.
**Test gate**: new `tests/test_pic.c` — init sequence through real port
writes, mask gating, EOI releasing a raised line, priority order between
two raised lines, slave cascade vector mapping, CLI gate, HLT wake. All
into `make test`; sanitize and fuzz lanes green; boot integration
unchanged (measured above).
**Negative control**: patch applied without the `cpu_step()` sampling edit
→ `test_pic` vectors red (nothing ever delivers).

### H1 — full PIC  (`0014`)
Fixed/rotating priorities, auto-rotation on EOI, special mask mode, ISR
nesting semantics, spurious IRQ7/IRQ15 vector return with no ISR set,
poll command, level-triggered lines (ICW1 LTIM) with re-assertion, ICW4
modes (AEOI, buffered/sfnm stored not necessarily acted on).
**Gate**: vector-per-scenario unit tests incl. nested same-source IRQs,
spurious cases, rotation sequences. Negative control: rotation vectors
red with H0 fixed-priority-only PIC.

### H2 — PIT 8254  (`0015`)
Counter 0 modes 0/2/3 with 16-bit write-then-read latches, OUT output
status, counter 2 + port 0x61 bits 0/1; tick period derived from virtual
time (`instr_count`, 1193182 Hz nominal); mode-change semantics per Intel
8254 (not "close enough"); IRQ0 line asserted to the PIC.
**Gate**: BIOS-style `mov al,0x36; out 0x43; out 0x40` program then wait:
IRQ0 must fire within the programmed period ±1 tick, repeatedly; latch
readback matches the same virtual-time model (deterministic); table-style
tests for mode edges. Negative control: PIT with tick-source cut → no
IRQs (test reads back stagnant counter).

### H3 — RTC/CMOS  (`0016`)
Clock regs BCD (A/ B /C), divider/update-in-progress phases, register C
flags + IRQ8 update-ended path through the slave PIC, CMOS bytes the
firmware-era BIOS reads (IPS/equipment word/base+ext memory KB at
0x15-0x17 per RM RBIL, checksum area left zero but documented).
**Gate**: deterministic BCD time from virtual epoch; IRQ8 fires once per
second of virtual time after enabling; port 0x70/0x71 reads now return
real data instead of sinking (measured: they sink today).

### H4 — KBC 8042  (`0017`)
Status register (IBF/OBF), command byte, data port, `0x60/0x64` command
subset (read/write cmd byte, self-test 0xAA→0x55, interface test),
IRQ1 with scancode-set-1 injection from the CLI (`--keys=...` or
monitor hookup in TOOLING), reset-via-KBC `0xFE` → machine reset request
(lands in H5 with port92/CF9).
**Gate**: injected key produces IRQ1, guest read drains OBF, IRQ1
deasserted until next byte; self-test sequence observed by unit test.

### H5 — A20 gate + reset ports  (`0018`)
A20 line state in `mem` subsystem: address bit 20 masked when disabled
(real wrap at 1MB, including the megabyte-aliasing every DOS extender
tests), KBC A20 command and port 0x92 bit1 tracked together (OR
semantics), port 0x92 bit0 + port 0xCF9 = system reset (full machine
reset path, not a halt).
**Gate**: write 0xFFFF:0x0010 with A20 off → observed at 0x00000-aliased
address; on → distinct. Reset through each of the three sources returns
the CPU to the reset vector with devices reinitialized.

### H6 — LAPIC  (`0019`)
MMIO window 0xFEE00000 (CPUID.1 already advertises APIC — the window is
absent today, **measured**), SVR enable, ID/version registers, TPR gating,
task-priority arbitration, IPI to self (single-vCPU self-IPI only),
LAPIC timer (initial/current count, divide, LVT, TMICT from virtual
time) asserting through the LAPIC into the CPU's INT pin. PIC path stays
untouched (D7); `IA32_APIC_BASE` MSR read.
**Gate**: POST-style enable + timer-downcount fires an LVT-vector
interrupt in real mode and in long mode; TPR masking proven; self-IPI
delivers.

### H7 — IOAPIC  (`0020`)
MMIO 0xFEC00000, IOREGSEL/IOWIN window, 24-entry redirection table
(vector, delivery/dest modes, polarity/trigger bits), IOREGVER/ID,
PAIR-TEST: PIT IRQ0 routed through a programmed IOAPIC entry into the
LAPIC into the CPU. Redirection table is the entire contract — MP table
/ ACPI MADT exposure belongs to COMPAT, not here.
**Gate**: guests masking PIC and programming IOAPIC+LAPIC get timer
interrupts through the full 8254→IOAPIC→LAPIC→CPU chain; the C0-era
`make test` suites stay green.

## What this plan does not do

- NMI/SMI delivery (COMPAT G-phase may add NMI for the SMP protocol).
- MSI/MSI-X (NET e1000 will use INTx sharing first; MSI lands with NET
  or later).
- Multi-vCPU and TLB shootdown (H6/H7 build single-CPU APIC plumbing only).
- QEMU-compatible firmware interfaces (fw_cfg, ACPI tables) — COMPAT.
- Speed: PIT/RTC timers count *virtual* instructions, not wall time (D6).

## Ledger

- Phase rows above are updated in the same patch that ships the phase
  (docs-sync-as-DoD, house rule 5).
- Follow-on patches numbering continues the CORE series: 0013…0020.
