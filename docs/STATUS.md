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
- The `--kernel=path` lane boots an AuraLite-OS `kernel.elf` directly,
  no firmware in between (KERNEL-BOOT K1): ELF64 segments placed at
  physical (p_paddr - kernel VMA), a seven-page paging hierarchy the
  kernel's boot contract demands (identity + HHDM of the low 4 GiB as
  shared 1 GiB pages, kernel VMA as 4 KiB pages), a fabricated
  `boot_info_t` handoff (magic-pinned ABI mirror, framebuffer window,
  E820-style memmap, HHDM offset, one BSP), and the CPU parked at
  `_start` in long mode per the measured contract. The run log marks
  `kmain` reached (18 instructions in, measured on AuraLite-OS @
  0ed0d29); the measured frontier and the next phases live in
  `docs/plans/KERNEL_BOOT_PLAN.md`.

- The K4 lane brings ring-3 userspace up: `--initrd=` publishes a USTAR
  rootfs through `boot_info_t`, the kernel mounts it, /hello runs to
  exit(0), the execve argv/envp self-test passes, and the interactive
  shell banner prints on COM1. The path forced four measured CPU-core
  fixes: descriptor-table reads (GDT/IDT inside SYSCALL/exception
  delivery) are implicit supervisor accesses; the IDT gate IST byte is
  fetched through the linear IDTR base; ISTn slots live at
  TSS+0x24+(n-1)*8; WRMSR IA32_FS_BASE keeps the FS shadow in sync so
  userspace TLS (`%fs:0` errno) works. New test_ring3 suite pins every
  one of them (verified red without each fix).

- COM1 is a 16550 subset register file (CHIPSET K4): LSR reports
  TX-idle/RX-empty, RBR is silent, IER/LCR/MCR/SCR store, IIR says
  "no pending", DLAB detaches THR/RBR onto the divisor latch. The
  K4-measured consequence of the old one-port model was an infinite
  space storm into the guest shell's stdin. Scancode injection can be
  scheduled by instruction count: `--keys-at=N` (the PS/2 driver's
  boot-time drain consumes anything queued before its init).

- The K5 lane meters the kernel's SMP bring-up against the emulator's
  single vCPU: `--cpus=N` publishes N CPUs in `boot_info_t` and the
  fabricated MADT (`acpi_build_table_set_smp`), so smp_init() runs its
  real path — LAPIC-timer calibration against the PIT (measured
  1317458400 Hz, 99.982% of the virtual-time ideal), then the classic
  INIT-SIPI-SIPI ICR sequence per would-be AP, which the LAPIC model now
  accepts and traces (delivery modes INIT/SIPI are legal sends, not
  ESR-illegal). With one vCPU no AP answers; the kernel's bounded wait
  falls back to BSP-only and the boot reaches the shell unimpaired.
  Measured verdicts: the loader `goto_address` wake mechanism is unused
  by the kernel (zero source references — it self-serves via the ICR);
  the BSP's scheduler tick stays on the PIT by kernel design (periodic
  LAPIC ticks are armed per-AP only), so scheduler preemption is
  observable through the kernel's tick-paced `[sched]` self-test, and
  true per-AP LAPIC scheduling needs a second vCPU (future phase).

- The K6 lane wires that second vCPU: `--smp=2` parks a second
  `cpu_state_t` context in wait-for-SIPI (LAPIC id from the fabricated
  MADT/boot_cpu_t), runs a strict 1:1 round-robin where one step = one
  instruction of the current context, and keeps the m->cpu/m->lapic
  pair as "the current context" by swapping in/out around each step —
  every device and exception path keeps its existing shape.  INIT resets
  and parks the target context (SDM 8.4), SIPI starts it in real mode at
  vector<<12; a single virtual master clock (+1 per retired instruction
  from any vCPU, +512 per HLT idle quantum — identity when n_vcpus==1)
  drives PIT/RTC/LAPIC timers and RDTSC for both contexts.  Measured
  with kernel 0.0.1: the INIT-SIPI walk wakes the real AP, which reaches
  `[smp] AP #0 online` in ~20 ms of virtual time; the AP's periodic
  LAPIC timer interrupt (vector 32) is delivered into its hlt-ed idle
  loop and EOI'd back every ~143k vtime (100 Hz) — per-CPU preemption
  ticks on the AP are real; `PASS: multi-core system detected`; a full
  boot to the shell.  Debugging lanes kept: `--smp-probe` (AP wake-window
  vital signs, PIT ch2/udelay cadence, LAPIC intack/EOI trace) and
  `--watch-phys=addr` (single-address store probe).  The MC146818A
  periodic interrupt chain (register-A RS rate, PF+PIE) is modeled now
  so the kernel's AP-wake receipt test (SYS_IRQ_AP_WAKE, an RTC IRQ8
  storm routed to the AP through the I/O APIC) can run.

- The K8 lane made the 2-vCPU machine pass the guest's own userspace
  SMP gates (`/tests/smpstress` and `/tests/irqapwake` typed into the
  shell over the KBC lane; the same assertions the OS repo's QEMU
  -smp 2 integration case makes).  Two machine-side pacing defects
  were found red-first and fixed: an hlt-idle vcpu no longer charges
  its K3 512-instruction quantum when a peer has runnable work (busy
  instruction streams no longer pay for a halted sibling's idle
  fast-forward; guest-measured 2-CPU boot-to-shell 1031s -> ~5s), and
  the PIT/RTC/LAPIC time-settle calls now run on every idle slot so a
  halted vcpu's calibrated LAPIC watchdog still expires at true wall
  rate while its peer works (was: frozen mid-tick, stranding a fork
  child on the AP runqueue — a wake-lost deadlock).

- The K7 lane closed the wake-window boundary K6 recorded: fine-grained
  `--smp-probe` RIP histograms showed the AP spending its entire window
  in `fb_putchar`/`fb_scroll` (~5.5M retired instructions for one line's
  full-screen scroll — NOT the UART ring, which drains synchronously on
  our insta-ready LSR; K6's ring-latency hypothesis is superseded).
  The D6 pin (12 vtime/PIT tick) models a ~14-MIPS CPU, making that
  scroll ~370 ms of PIT wall time against the kernel's 100 ms SIPI
  wait.  K7 lifts the pin to 384 (~373 virtual MIPS), keeps the RTC on
  the same divisor, and sets `tsc_per_instr` = 1 on all profiles (the
  in-order single-issue model the interpreter is) — kernel receipts:
  `LAPIC bus frequency: 458163200 Hz (458 MHz)` and
  `2 CPU(s) online (1 AP(s) woken)`, deterministic (two runs
  byte-identical), `PASS: multi-core system detected`.

## Current limits

This is a general firmware-focused x86 emulator, not yet a drop-in replacement for QEMU. The CPU ISA is expanded from execution traces and tests. Some chipset blocks are behavioural models rather than cycle-accurate implementations. The descriptor/CBW/CSW path and live web monitor are still being expanded.
