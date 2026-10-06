# KERNEL-BOOT Plan

**Status: in progress — K0 ✅, K1 ✅, K2 ✅, K3 ✅, K4 ✅, K5 ✅, K6+ 📋**

| Phase | Scope | Status | Deliverable |
|---|---|---|---|
| K0 | Baseline measurement: clone AuraLite-OS, tool up, build `kernel.elf`, read the boot contract from source | ✅ done | (workspace; no patch) |
| K1 | Direct kernel-load lane `--kernel=`: ELF64 placement, paging hierarchy, fabricated `boot_info_t`, CPU at `_start` | ✅ done | `patches/0021-K1-kernel-boot-lane.patch` |
| K2 | ISA-gap closure by kernel trace until the first klog banner | ✅ done | `patches/0022-K2-kernel-isa-gap.patch` |
| K3 | Hardware surface the driver probes actually touch (fw_cfg result path, PCI BAR consumption, LAPIC timer calibration ride-along) | ✅ done | `patches/0023-K3-probe-path-hardware.patch` |
| K4 | initrd (USTAR): `--initrd=` lane, `boot_info_t.initrd_*`, first userspace → shell prompt over serial | ✅ done — /hello exit(0), execve argv/envp PASS, `auralite#` prompt stable, typed `help` executes end-to-end | `patches/0024-K4-ring3-userspace.patch`, `patches/0025-K4-serial-keyboard.patch` |
| K5 | Scheduler-tick receipt paced by virtual time; SMP scoped (`--cpus=N` metering of the kernel's ICR wake-up walk against one vCPU) | ✅ done | `patches/0026-K5-smp-metering.patch` |
| K6 | Real second vCPU: INIT/SIPI wake of a parked AP context, per-CPU LAPIC/cpu multiplexing, per-CPU LAPIC timer preemption measured on the AP | ✅ done | `patches/0027-K6-second-vcpu.patch` |

This document answers: *what does it take for the emulator to boot the
north-star guest — AuraLite-OS — to a usable shell, and in what order do
the pieces land?*

Baselines (all measured, not assumed):

- Emulator baseline: commit `3bc07bb` (upstream main: CORE C0–C11 +
  CHIPSET H0–H7). Paging (1 GiB/2 MiB/4 KiB walks), long mode, CR/MSR
  traffic, LAPIC/IOAPIC/PIC/PIT/RTC/KBC, COM1 byte sink, GPU VRAM at
  0xD0000000 with the GUI framebuffer view — all present and test-gated.
- Guest baseline: AuraLite-OS @ `0ed0d29` (`origin/main`), built in this
  workspace with `clang --target=x86_64-elf` + `ld.lld` + `nasm`:
  `make kernel` → `build/kernel.elf` (3,051,504 bytes, 3 LOAD segments,
  entry `0xffffffff8018fc50`, phys `[0x100000..0x55a270)`).
- Boot contract, read from `boot/shared/boot_info.h` +
  `kernel/arch/x86_64/boot.asm` + `kernel.ld` (MIT, same author):
  long mode, paging on, higher half pre-mapped; `RDI` = *physical*
  `boot_info_t*`; `IF=0`; any temporary RSP. `boot_info_t` is 7,776 bytes
  (offsets verified on the host compiler: fb@8, mmap@40, mmap_count@6184,
  hhdm@6192, initrd@6200, cpus@6224, rsdp@7760). Kernel VMA
  `0xFFFFFFFF80100000`; HHDM `0xffff8000_00000000`; magic
  `0x4155524142544C44` checked first thing in `kmain`.
- The kernel tolerates `rsdp=0`: `kernel/arch/x86_64/ioapic.c` (RESIDUE
  R11) walks RSDP→MADT when published but keeps a measured hardcode path
  for the PC-standard LAPIC 0xFEE00000 / IOAPIC 0xFEC00000 ("MADT agree"
  log lines). A real MADT is therefore K3-deferred, not a K1 blocker.

## The problem, stated once

Everything until now ran the *sample* firmware + a 512-byte test kernel on
a USB stick. The north star is a real kernel with real expectations:
a loader-built paging hierarchy, a byte-exact handoff block, SSE online
before C runs, a LAPIC timer that ticks, an IOAPIC that routes the
keyboard. Emulators grow into kernels one unimplemented `0F 1F` at a
time — the only honest way is a lane that puts the real kernel at the
front of the machine and measures where it stops.

## Measured K1 results (the frontier, stated once)

x86emu `--kernel=AuraLite-OS/build/kernel.elf`, 5M-instruction budget:

- loader: 3 segments placed, 7 table pages (shared identity+HHDM PDPT of
  1 GiB pages + 1,115 4-KiB kernel PTEs), `boot_info_t` at 0x20000;
- `kmain` reached after **18** instructions (SSE enable + 3.7 MiB `.bss`
  zeroing via `rep stosb` all executed);
- halt at instruction **166**: `#UD 0F 1F` at `0xffffffff8017ce09` —
  multi-byte NOP emitted by clang for alignment, absent from the ISA
  table. That is K2's first measured item. CR4 at the stop shows
  `PAE|PGE|OSFXSR|OSXMMEXCPT` set by boot.asm itself; R15 holds the
  0x20000 handoff pointer exactly as boot.asm's `mov r15, rdi` intends.

## Decisions

- **D1 — the loader is a lane inside the emulator, not a separate tool.**
  `--kernel=path` reuses the full chipset (devices, PCI, COM1, fb window)
  and the one run loop; the only thing skipped is firmware execution.
- **D2 — loader structures go straight into the RAM backing store**, the
  way firmware flashes regions off-bus before reset-release. This keeps
  the lane immune to A20-gate state (measured OPEN at power-on — but not
  architected as a dependency) and to ROM-alias overlap checks.
- **D3 — identity and HHDM share one PDPT** of four 1 GiB PS entries.
  Both views are byte-identical 1:1 maps of the low 4 GiB (RAM, MMIO
  windows, ROM aliases), so sharing the table is free; kernel VMA gets
  its own PDPT/PD/PTs with 4 KiB PTEs. Total measured: 7 pages.
- **D4 — page permissions are P|RW everywhere, NXE off.** The kernel
  builds its own address space moments after `kmain` (paging.c) and owns
  NX there; the loader's transitional maps stay permissive. Documented,
  not forgotten (K2 may tighten if the kernel proves to rely on NX
  before its own VMM runs).
- **D5 — the boot_info ABI is pinned by a `_Static_assert` lattice**
  in `kloader.c` (magic, 8 offsets, two sizeofs) mirroring the AuraLite
  header with attribution, values measured with host `offsetof`. Drift
  fails the build, not the boot.
- **D6 — `rsdp=0` is deliberate at K1**, justified by the kernel's own
  measured hardcode fallback (see Baselines). K3 replaces it with a real
  RSDP/XSDT/MADT set the MADT walk can consume.
- **D7 — the test gate uses a synthetic kernel, not the real one:**
  `tests/kload_fixture.elf` is built from C at the *real* AuraLite VMA
  (`-mcmodel=kernel` + a pinned linker script) and validates the
  identity/HHDM views of the fabricated `boot_info_t` over COM1
  (`K1OK`). The real kernel.elf is the integration signal, run and
  measured per phase but never a build gate — CI cannot build the OS.

## Per-phase gates

- **K0 (done):** OS cloned and built reproducibly; ABI offsets table
  (Baselines); toolchain recipe recorded (`apt-get install clang lld nasm`;
  `make kernel` RC=0).
- **K1 (done):** DoD — `kmain` reached with zero `#UD`/`#GP` through
  `boot.asm` (measured: 18 instructions). Test gate: `make test-kload`
  (positive fixture + three negative controls: unknown path, truncated
  ELF, flipped `EI_CLASS`) wired into `make test` and `make test-sanitize`;
  full `make test`/`test-sanitize` RC=0 with all prior lanes unchanged.
- **K2 (planned):** DoD — serial log shows the kernel's own banner block
  (`Hello from AuraLite OS kernel!`) and the `[boot]` init receipts that
  precede it. Every newly needed opcode lands with a table-driven vector
  and keeps the differential fuzzer bit-identical; device-side immaturities
  surfaced on the path (e.g. COM1 DLAB awareness) land as their own
  vectors, not as hacks in the lane.
- **K2 (done):** DoD — the kernel's own banner block and the `[boot]`
  receipts that precede it on COM1. **Measured past the DoD:** PMM and
  VMM self-tests PASS, TSS loaded with IST armed, timer self-test PASS
  (10 ticks in 100 ms over the PIT→GSI2→IOAPIC→LAPIC→ISR32 chain),
  PCI scan enumerates all five synthetic devices; RC=0 to the 200M
  instruction budget, zero emulator FAULTs. Root causes closed
  (each with a before/after receipt in CHANGELOG patch 0022): eager
  RIP-relative EA anchoring (immediate-carrying stores landed short by
  the immediate length), descriptor tables read as physical instead of
  linear, CR2 not set on #PF, INVLPG treated as a memory read, TSS
  selectorism at GDT+0x10, missing PIT GSI2 board wiring, plus the
  measured opcode gaps (0F 1F hint block, BT family + 0F BA, MOVSXD).
  Test gate: table suite 344→357 rows, `make test`, `make test-sanitize`
  (ASan+UBSan), `make fuzz` (C11 differential) all RC=0.
- **K3 (done):** DoD — driver enumeration receipts measured. Results:
  `MADT agree` (fabricated RSDP/RSDT/XSDT/MADT at 0x9F000, rsdp_phys
  published, window ACPI_RECLAIM); hypervisor vendor line
  (`[vmdrv] hypervisor: AuraLite HV`); fw_cfg absence measured with an
  instrumented one-off (probe ports read open-bus 0xFF, kernel proceeds
  on build defaults); PCI BAR consumption measured (EHCI through the
  firmware BAR; AHCI reads BAR5=0 and skips). **Plan amendment (measured,
  K3):** the LAPIC timer calibration line cannot be produced with an
  honest cpu_count=1 — the kernel runs it only inside the cpu_count>1
  branch of smp_init (smp.c:228). LAPIC timer countdown/reload/vector
  latch is unit-proven; the integration receipt moves to K5.
  Collateral root causes closed with before/after receipts in CHANGELOG
  patch 0023: GS-base MSRs + SWAPGS (sched self-test 0/2 → PASS), BSF/BSR,
  FXSAVE/FXRSTOR (+MXCSR, CLFLUSH), x87 control subset, HLT-idle machine
  semantics (sti;hlt stays alive as a charged time quantum).
  Test gate: table suite 357→369, NEW test_acpi.c (checksum lattice +
  tamper-detect negative control), make test/test-sanitize/fuzz RC=0.
- **K4 ✅:** DoD met — `--initrd=archive.tar` published via
  `boot_info_t`, kernel mounts it, first userspace ELF starts (/hello
  exited code 0, execve argv/envp self-test PASS); `auralite#` prompt
  observed on COM1; keyboard input through KBC→IOAPIC→LAPIC reaches
  the shell — measured: `--keys-at=2000000000` typed `help` into the
  idle-waiting shell, the readline echoed it, the full command listing
  printed, and a fresh prompt returned (log tail, k4-keys2.txt).
  Measured fixes this phase (each pinned by a test vector proven red
  pre-fix): descriptor-table fetches are implicit supervisor accesses
  (desc_sv window) — SYSCALL far_load_cs no longer #PFs on a
  higher-half kernel GDT; IDT gate IST byte read through the LINEAR
  IDTR base; ISTn = TSS+0x24+(n-1)*8 (SDM layout); WRMSR IA32_FS_BASE
  syncs the FS shadow (userspace errno at %fs:0); COM1 is a 16550
  register file, not a 1-port 0x20 echo — a floating 0xFF LSR had
  told the kernel's uart_has_data() "yes" forever and read(0) rained
  511 spaces per call (the 100%-CPU prompt storm). 【keys】 injection
  after the shell starts needs `--keys-at=N` — the PS/2 driver's boot
  drain `while (STATUS.OBF) read DATA` eats any pre-boot keys.
- **K5 ✅:** DoD — scheduler-tick receipt paced by virtual time, SMP
  scoped by measurement. **Preemption receipt (measured):** the kernel's
  own `[sched]` self-test interleaves two threads strictly one virtual
  tick apart — `thread-A: message 0 (tid 1, tick 23)`, `thread-B:
  message 0 (tid 2, tick 24)`, ... `PASS: two threads interleaved
  correctly` — observable on COM1 with ticks counted by the PIT-derived
  virtual clock (99 Hz). K3's deferred integration receipt also landed:
  with `--cpus=2` the calibration line finally prints —
  `[smp] LAPIC bus frequency: 1317458400 Hz (1317 MHz)` = 99.982% of the
  virtual-time ideal (1104 TSC ticks × 1,193,182 Hz). **SMP scoping
  (the measured verdict):** the plan's `goto_address` assumption is
  wrong — `grep -rn goto_address kernel/ drivers/ include/` finds zero
  references; the kernel self-serves AP wake-up from smp.c via the
  LAPIC ICR (classic INIT-SIPI-SIPI, MP spec s.B.4). Metered end-to-end
  with the new `--cpus=N` lane (boot_info + MADT publish N CPUs while
  the emulator still runs one vCPU): INIT-assert dst=1, INIT-deassert,
  SIPI vec=0x08, SIPI vec=0x08 — exactly the spec sequence with the
  trampoline page vector — then the kernel's bounded wait expires
  (`AP lapic_id=1 did not respond to SIPI; skipping` → `running
  BSP-only` → `PASS: single-core system`) and the boot continues to a
  healthy `auralite#` (3778 ticks, no regression vs UP). **Plan
  amendment (measured, K5):** "scheduler on the LAPIC timer" cannot be
  produced on any faithful single-vCPU box — by kernel design the BSP's
  tick stays on the PIT and the periodic LAPIC timer is armed per-AP
  only (`ap_entry() → lapic_timer_start_periodic(100)`, smp.c:193).
  The remaining honest deliverable is a second vCPU: per-CPU LAPIC +
  cpu_local multiplexing, SIPI routing into a halted second context,
  interleaved stepping. Sized as its own phase (K6), not folded in here.
  Deliverables this phase: `--cpus=N` metering lane, MADT with N LAPIC
  entries (`acpi_build_table_set_smp`), ICR acceptance + trace for the
  INIT(5)/SIPI(6) delivery modes (previously flagged ESR send-illegal —
  those modes are architecture-legal sends). Test gate: full `make
  test` RC=0 (acpi/kload/lapic vectors all green), both boot profiles
  (UP and `--cpus=2`) boot to shell.
  `patches/0026-K5-smp-metering.patch`

- **K6 ✅:** DoD — the kernel's INIT-SIPI bring-up wakes a real second
  vCPU through the whole trampoline->long-mode walk; the AP reaches
  `[smp] AP #0 online`; per-CPU LAPIC timer interrupts are delivered,
  handled and EOI'd on the AP at 100 Hz; the UP profile is byte-for-byte
  undisturbed. Built: two `cpu_state_t` contexts swapped around a strict
  1:1 round-robin instruction step (`vcpu_load/store`, context images in
  `vcpu_slot_t`; the m->cpu/m->lapic pair is always the CURRENT context,
  so every existing device/handler path needs no churn), a machine-wide
  virtual master clock `vtime_instr` (+1 per retired instruction from
  any vCPU, +512 per HLT idle quantum — K3's pin, identity when
  n_vcpus==1) that drives PIT/RTC/LAPIC timers/RDTSC, INIT (SDM 8.4:
  state reset, park in wait-for-SIPI), SIPI (real-mode entry at
  vector<<12, discarded unless parked), per-context LAPIC id
  (boot_cpu_t.lapic_id, MADTs), and translation of the I/O APIC's rte.hi
  destination to the owning context (`lapic_set_irr_dest`). Debugging
  lanes added and retained: `--smp-probe` (AP wake-window vital signs +
  PIT ch2/udelay cadence + LAPIC intack/EOI trace), `--watch-phys=addr`
  (single-address store probe on phys RAM), KBC queue 32->64 (one
  shell command fits one burst). **Measured receipts:** AP finishes
  SIPI->online in ~20 ms of virtual time (inside the kernel's 100 ms
  bounded wait ~6x over); `intack vec32 vcpu1` + `EOI vec32 vcpu1`
  pairs recur every ~143k vtime (the AP's 100 Hz periodic LAPIC tick
  delivering into its hlt-ed idle loop and returning); kernel self-test
  `PASS: multi-core system detected`; full boot to a healthy shell with
  both threads scheduled. **Measured boundary (honest, kernel-side
  protocol, not machine state):** the kernel prints `did not respond to
  SIPI; skipping` and reports `(0 AP(s) woken)` bookkeeping even though
  the AP IS online — root-caused by the `--watch-phys` store probe:
  the AP's `cpus_online` increment lands ~0.9 s of virtual time after
  the BSP's 100 ms bounded wait expires, because the kernel's async
  kprintf UART ring serializes the AP's online report behind the BSP's
  boot chatter backlog on an instruction-paced emulated clock. Machine
  state is right (the very next printk reads `cpus_online == 2`); the
  wait-window protocol assumption (UART-ring latency << 100 ms) is what
  fails on this class of machine. RTC periodic-interrupt chain (PF/PIE,
  rate from register A RS) added so the kernel's own AP-wake receipt
  test (`/tests/irqapwake`, SYS_IRQ_AP_WAKE, an RTC GSI8 storm aimed at
  the AP) can run; receipt reproduced in the boot log.
  Test gate: full `make test` / `make test-sanitize` / `make fuzz` RC=0,
  UP boot byte-identical to K5 (`[smp] single CPU online`, same shell),
  `--cpus=2 --smp=2` boot to shell.
  `patches/0027-K6-second-vcpu.patch`

## What this plan does not do

- No BIOS stage2 / UEFI chain (both loaders end at the same `boot_info_t`
    contract, which this lane publishes directly; from `kmain` on, the
    kernel cannot tell the difference — measured by boot_info_init reading
    exactly the fields we fabricate). This absorbs the COMPAT plan's boot
    path; COMPAT's other rungs (real MADT/MP tables, SMP) remain as K3/K5.
- No UEFI path (the kernel's BIOS/UEFI loaders share one `boot_info_t`;
    we publish `boot_from_uefi=0` — the C code treats the paths uniformly
    after handoff).
- No SMP in K1–K4; one bootstrap CPU, the other 63 `boot_cpu_t` slots
    parked. K5 investigates honestly from the measured wake mechanism.
- No real ACPI firmware (`rsdp=0` until K3), no SMM, no suspend/resume.
- No persistence guarantees for guest storage: STORE/AHCI and USB-MSC
    correctness for the kernel's drivers is a separate plan riding on this
    one's lane.
- The GUI stays the emulator's host-side view of the same VRAM window the
    kernel writes through HHDM; no display-device protocol is invented.

## Ledger

| Phase | Commit | Evidence |
|---|---|---|
| K0 | (workspace) | AuraLite-OS @ `0ed0d29`, `make kernel` RC=0, ABI offsets host-measured |
| K1 | this patch | `kmain` reached @ instr 18; stop #166 = `0F 1F` (K2 input); make test / test-sanitize RC=0 |
