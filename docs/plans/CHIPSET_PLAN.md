# CHIPSET Plan

**Status: ALL PHASES DONE — H0–H7 ✅**

| Phase | Scope | Status | Deliverable |
|---|---|---|---|
| H0 | INTR delivery scaffold at instruction boundaries + PIC 8259 sketch (init, mask, vector via INTA, EOI, cascade) | ✅ done | `patches/0013-H0-intr-scaffold.patch` |
| H1 | PIC correctness deep-cut: priorities/rotation, special mask mode, spurious IRQ7/15, poll command, level-vs-edge, ICW4 modes | ✅ done | `patches/0014-H1-pic-full.patch` |
| H2 | PIT 8254: counter 0 (periodic + one-shot semantics), counter 2 + port 0x61 beeper bits, virtual-time tick source | ✅ done | `patches/0015-H2-pit.patch` |
| H3 | RTC/CMOS: clock registers, update-ended interrupt (IRQ8), CMOS RAM bytes the BIOS area reads (0x10-0x2F equipment/memory size) | ✅ done | `patches/0016-H3-rtc.patch` |
| H4 | KBC 8042: status/command/data ports + IRQ1 delivery; scancode set-1 injection path; gate A20 command consumed by H5 | ✅ done | `patches/0017-H4-kbc.patch` |
| H5 | A20 gate (8042 output-port bit1 OR port 0x92 bit1), port 0x92 bit0 reset, port 0xCF9 reset, KBC 0xFE/outport-bit0 wiring, full warm-reset path | ✅ done | `patches/0018-H5-a20.patch` |
| H6 | LAPIC: MMIO at 0xFEE00000, SVR, TPR/PPR arbitration, ICR/self-IPI, timer from virtual TSC, IDT-gate IST stacks + LTR/TR cache | ✅ done | `patches/0019-H6-lapic.patch` |
| H7 | IOAPIC at 0xFEC00000: IOREGSEL/IOWIN, 24-entry redirection table, polarity/trigger semantics, delivery to LAPIC; ISA lines fanned out from pic_* (board wire) | ✅ done | `patches/0020-H7-ioapic.patch` |

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

**Shipped (0018)**, with two measured decisions on top of the phase text:
 - Effective A20 = (8042 output-port bit1) OR (port 0x92 bit1), masked on
   the bus itself (fetches included). Measured baseline: the sample
   firmware touches neither source in its first 2M instructions yet runs
   above 1MB from instruction zero, so -- like Bochs/QEMU and modern
   PCHs -- this board powers up with the gate OPEN (port-0x92 bit1 reads
   back set at reset). Close/open/wrap (incl. the odd-megabyte aliasing)
   is pinned from both sources by unit vectors.
 - All four reset sources (0x92 bit0 edge, 0xCF9 SYS_RST edge with
   RST_CPU set, KBC 0xFE, KBC output-port bit0 falling edge) funnel into
   ONE pending request consumed at the next instruction boundary
   (reset-line propagation; pulses before a boundary merge). The reset
   ritual: CPU back to F000:FFF0, PIC/PIT/KBC/A20/port state to power-on
   defaults; RAM/ROM and the battery-backed RTC ride through, board
   topology is not re-enumerated. Vectors pin the ritual per source plus
   the boundary semantics (the instruction after the trigger never runs).

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

**Shipped (0019)**, with two measured finds the phase text didn't know:
 - Real-mode guests can't reach the 0xFEE00000 window (real-mode linear
   tops out just above 1MB), so real-mode vectors program the LAPIC
   host-side and the LONG-mode vector does guest-side MMIO + guest-side
   EOI, proving the full path. That vector immediately measured a real
   gap: delivery while the interrupted RSP pointed at a register window
   corrupted the window (#PF loop) -- 64-bit IDT gates needed the IST
   stack switch through the TSS. cpu.c grew IST handling plus LTR/STR
   (0F 00 /3,/1) and a cached TR; the vector pins it with an IST=1 gate.
 - The D6 virtual timebase freezes under HLT (documented H2 property),
   so a virtual-time timer cannot wake a halted CPU -- the HLT-wake
   vector uses a self-IPI instead and pins wake-without-IF plus the
   latched-IRR delivery after STI.
Also true to the phase text: SVR soft-enable gates DELIVERY but not
latching (IRR set while disabled delivers on enable), TPR/PPR class
arbitration incl. acceptable-nesting, ICR shorthand matrix on a single
vCPU (self/broadcast deliver, all-excl-self delivers to nobody),
one-shot vs periodic with edge-lost-when-masked, IA32_APIC_BASE reset
value 0xFEE00900 seeded by cpu_reset. PIC path untouched (D7).

### H7 — IOAPIC  (`0020`)
MMIO 0xFEC00000, IOREGSEL/IOWIN window, 24-entry redirection table
(vector, delivery/dest modes, polarity/trigger bits), IOREGVER/ID,
PAIR-TEST: PIT IRQ0 routed through a programmed IOAPIC entry into the
LAPIC into the CPU. Redirection table is the entire contract — MP table
/ ACPI MADT exposure belongs to COMPAT, not here.
**Gate**: guests masking PIC and programming IOAPIC+LAPIC get timer
interrupts through the full 8254→IOAPIC→LAPIC→CPU chain; the C0-era
`make test` suites stay green.

**Shipped (0020)**, design decisions locked where the phase text left
them open:
 - The board wire fans out INSIDE pic_raise_irq/pic_set_irq (zero
   device-call-site churn, D7): strobe lines (8254 IRQ0, RTC IRQ8)
   arrive as edge events, the 8042's held IRQ1 as a real voltage --
   so a level-triggered RTE sees exactly the H4 line.
 - Edge-triggered RTEs LATCH masked edges until unmask (real 82093AA
   behavior, unlike LAPIC LVTs); level-triggered RTEs carry remote_IRR
   fed back by the LAPIC EOI hook, so a held line redelivers after each
   EOI. Non-fixed delivery modes are stored, not delivered (ExtINT is a
   H6-LINT story). Destination field ignored on the single vCPU, like
   the LAPIC's own ICR model.
 - PAIR-TEST is the gate made literal: PIC fully masked (IMR=0xFFFF,
   pic_pending()==0 pinned while the line DID hit the PIC), 8254 ch0
   period surfaces via rte[0] -> LAPIC IRR -> guest handler, twice
   (host EOI between, per the real-mode MMIO reachability truth H6
   measured). Warm reset re-masks all RTEs.

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
