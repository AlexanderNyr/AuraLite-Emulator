# Implementation status

## Working now

- The sample 128 KiB firmware image can be loaded at the x86 reset vector.
- Reset execution supports real mode, protected mode, compatibility mode and long mode.
- All five selectable Intel-style platform profiles are available.
- The EHCI model reaches virtual USB Mass Storage transfers when the loaded firmware requests them.
- The physical address decoder handles overlapping ECAM and fixed BAR regions by choosing the most-specific region.
- A framebuffer is available as `framebuffer.ppm`.
- The CPU, memory, PCI and device models do not depend on a particular firmware name or binary layout.
- A PC-compatible dual 8259A PIC (ports 0x20/0x21/0xA0/0xA1) delivers maskable hardware
  interrupts at instruction boundaries: ICW1-4 init, OCW1 mask, OCW2 EOI, OCW3 IRR/ISR
  readback, slave cascade through master IRQ2, STI shadow and HLT wake semantics (H0),
  plus fixed/rotating priorities with all OCW2 rotation commands, fully-nested
  in-service gating, special mask mode, spurious IRQ7/IRQ15 vectors, the poll command,
  level-triggered lines with re-assertion, and AEOI (H1). Device-side line APIs:
  `pic_raise_irq()` (edge strobe) and `pic_set_irq()` (level hold).
- An 8254 PIT (ports 0x40-0x43, port B 0x61) runs on virtual time derived from
  `instr_count` (1193182 Hz nominal at ~99.4 virtual MIPS, so all cadence is
  deterministic): counter 0 modes 0/2/3 raise IRQ0 to the PIC, counter 2 is gated
  by 0x61 bit0 and its square-wave output is readable at 0x61 bit5; latch and
  LSB/MSB access-phase semantics included (CHIPSET H2).
- An MC146818A RTC/CMOS (ports 0x70/0x71 -- previously floating sinks,
  measured) keeps BCD clock registers on the same deterministic virtual
  timebase (epoch pinned), models the update-in-progress/commit phases
  exactly, fires the update-ended IRQ8 through the slave PIC once per
  virtual second when UIE=1, and serves the CMOS bytes BIOS-era firmware
  reads: equipment 0x14, base/ext memory KB at 0x15-0x18 and 0x30/0x31
  (16-bit capped), BCD century at 0x32, checksum area pinned zero
  (CHIPSET H3).
- An 8042 keyboard controller (ports 0x60/0x64) models the status
  register, command byte and the 0x64 command subset (read/write cmd
  byte, self-test 0xAA -> 0x55, interface test, 0xAD/0xAE inhibit,
  output port 0xD0/0xD1 -- bit1 = A20 gate, bit0 = SRST# --, 0xFE
  CPU-reset pulses). Scancode set-1 injection -- API or CLI
  `--keys=1E,9E,...` -- drives IRQ1 as a level of (OBF and cmd bit0),
  exactly like the 8042's line (CHIPSET H4).
- The A20 gate is a real bus effect (CHIPSET H5): with the gate closed,
  physical address bit 20 is forced to zero for reads, writes AND
  fetches, so every odd megabyte aliases onto the even one below (the
  1MB wrap). Effective line = (8042 output-port bit1) OR (port 0x92
  bit1); measured firmware behavior pinned the power-on state to OPEN
  (like Bochs/QEMU). System reset is a full warm reset consumed at the
  next instruction boundary, from four sources: port 0x92 bit0 edge,
  port 0xCF9 (SYS_RST rising with RST_CPU set, i.e. the canonical
  0x06/0x0E or Linux 0x02-then-0x06 dance), KBC 0xFE pulses, and the
  KBC output-port bit0 falling edge. CPU back to the reset vector,
  PIC/PIT/KBC/A20 re-initialized, RAM and the battery-backed RTC ride
  through (CHIPSET H5).
- A local APIC (xAPIC, MMIO at 0xFEE00000) serves the single vCPU
  (CHIPSET H6): ID/VERSION/TPR/computed-PPR/LDR/DFR/ESR/EOI, SVR
  software-enable gating delivery (latching continues while disabled),
  full 256-bit IRR/ISR with class arbitration, ICR fixed self-IPI
  (shorthand matrix on one vCPU), and the LAPIC timer -- one-shot and
  periodic, DCR divides, clocked by the deterministic virtual TSC,
  edge-lost-when-masked. IA32_APIC_BASE resets to 0xFEE00900. The 8259
  path stays the fallback whenever the LAPIC has nothing deliverable
  (plan D7). CPU side gained 64-bit IDT-gate IST stack switching
  through the TSS plus LTR/STR (the H6 long-mode vector measured why:
  delivery onto a register-window RSP corrupts it).
- An 82093AA I/O APIC sits at 0xFEC00000 (CHIPSET H7): IOREGSEL/IOWIN
  indirection, 24-entry 64-bit redirection table (vector, delivery/dest
  modes stored, polarity/trigger/mask live). Every ISA line reaches the
  same-numbered INTIN pin through the pic_* fan-out (the board wire),
  so the 8254's IRQ0 strobe and the 8042's held IRQ1 appear as edges
  and levels per entry programming. Edge entries latch masked edges
  until unmask; level entries carry remote_IRR with LAPIC-EOI feedback,
  redelivering held lines after each EOI. Fixed delivery targets the
  single vCPU's LAPIC; the PAIR-TEST vectors the full
  8254 -> IOAPIC -> LAPIC -> CPU chain with the 8259 pair masked out.

## Current limits

This is a general firmware-focused x86 emulator, not yet a drop-in replacement for QEMU. The CPU ISA is expanded from execution traces and tests. Some chipset blocks are behavioural models rather than cycle-accurate implementations. The descriptor/CBW/CSW path and live web monitor are still being expanded.
