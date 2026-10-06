# Changelog

All notable changes to AuraLite-Emulator are recorded here. The project
ships as a `patches/` series on top of the baseline commit `33e2c5c`; each
entry names its patch and the measured defect(s) it repaired.

## [Unreleased]

### Added — patch 0023 (KERNEL-BOOT K3, probe-path hardware)
- `src/acpi.[ch]`: a fabricated ACPI set (RSDP rev 2 with both RSDT and
  XSDT, one MADT: LAPIC 0, I/O APIC @0xFEC00000 GSI base 0, ISA IRQ0→GSI2
  override). The kloader flashes it at phys `0x9F000`, publishes
  `boot_info.rsdp_phys` (was 0 → the kernel's hardcode path), and marks
  the window `BOOT_MEM_ACPI_RECLAIM`; memmap entries grow 7→8.
- WRMSR/RDMSR handling for IA32_GS_BASE (`0xC0000101`) and
  IA32_KERNEL_GS_BASE (`0xC0000102`) with the decode-time `seg[GS].base`
  shadow kept in sync; SWAPGS (`0F 01 F8`) in long mode with a pinned
  legacy #UD. Measured: the scheduler's per-CPU structures live exactly
  there.
- CPUID: leaf-1 ECX hypervisor bit (we *are* a hypervisor) and the
  `0x40000000` hypervisor interface leaf returning the `"AuraLite HV "`
  vendor string (kernel receipt: `[vmdrv] hypervisor: AuraLite HV`).
- BSF/BSR (`0F BC/0F BD`) in the two-byte switch, silicon-real src==0
  semantics: ZF=1 and the destination register preserved (the SDM says
  "undefined"; both host vendors preserve and the diff-fuzzer runs
  against host silicon).
- `0F AE` memory forms (GROUP 15): FXSAVE/FXRSTOR m512 (powered-on x87
  image + the one live strand, MXCSR, tracked in `cpu_t`; alignment
  #GP), LDMXCSR/STMXCSR, CLFLUSH as no-op. The XSAVE family stays #UD
  (CPUID-gated on real silicon; we don't advertise it).
- x87 control subset: FNINIT/FNCLEX no-ops and FLDCW/FNSTCW tracking a
  control word (`fcw`, seeded 0x037F). Every x87 data encoding stays
  #UD — the deliberate, pinned silence boundary.
- HLT idle semantics: halted-with-IF=1 stays alive as a charged virtual
  time quantum (timers tick during sleep) instead of reporting a
  full-machine halt; `hlt+cli` still reads as a real halt. Measured: the
  AuraLite idle loop does `sti;hlt` between PIT interrupts.

### Fixed — patch 0023 (measured this phase)
- **Scheduler per-CPU data was unreachable.** `get_cpu_local()` reads GS
  base; without the GS MSRs the kernel's yield path never ran a created
  thread — the sched self-test measured `0/2 threads finished`. After
  the MSR/SWAPGS work: `[sched] PASS: two threads interleaved correctly`.
- The `0F AE bad faults` table pin advanced (`0F AE 08` is now a real
  FXRSTOR): the row moved to `0F AE 30`, a standard pin-lifecycle step
  as the group widens.
- `test-kload` tracks the ACPI memmap split (7→8) and asserts
  `rsdp_phys` at its ABI offset.

### Regression rigs
- `tests/test_table.c` 357 → 369 rows (BSF/BSR incl. zero-src, GROUP-15
  #UD pins, x87 control pins, SWAPGS legacy pin).
- NEW `tests/test_acpi.c`: signature/length/checksum lattice, MADT entry
  walk (the machine we claim to be), fit guards, and one-byte tamper
  detection on all three checksummed tables (the phase's negative
  control).
- `make fuzz`: 512 programs × 24 instructions host-identical; crash
  invariant 128 garbage streams, 0 host-level crashes.
- `make test` / `make test-sanitize`: RC=0.

### Measured result (kernel lane, `--kernel=AuraLite-OS/build/kernel.elf`)
- DoD receipts: `[ioapic] base 0xfec00000 (MADT agree)`; hypervisor
  vendor line; fw_cfg absence measured with an instrumented one-off —
  the probe ports read open-bus `0xFF` and the kernel proceeds on build
  defaults (`fwcfg_selftest_probe`/`fwcfg_fsformat_probe`, kernel.c:268,
  279). PCI BARs consumed: EHCI probed through its firmware BAR,
  AHCI driver reads `BAR5=0` and skips honestly.
- LAPIC timer calibration line is **not reachable with an honest
  `cpu_count=1`** — the kernel itself gates calibration inside the
  `cpu_count>1` SMP branch (measured in `smp.c:228`). The LAPIC timer
  countdown/reload/vector-latch model is proven by the unit lanes; its
  integration receipt moves to K5's SMP investigation (this is a plan
  amendment, not a silent downgrade).
- Boot now reaches: audio online → VFS/tmpfs/VFS self-tests PASS → AHCI
  probe → USB core init (0 devices, none attached, as expected). Run of
  904M instructions ends RC=0 with zero emulator FAULTs, machine idling.
- Next measured frontier: instruction-count-costly fb rendering between
  driver-init receipts; initrd (K4) unchanged.

### Added — patch 0022 (KERNEL-BOOT K2, ISA-gap closure to the banner and past)
- `src/cpu.c`: the `0F 18–0F 1F` multi-byte NOP hint block (GROUP 16
  prefetch aliases) — decode the ModRM form and execute as NOP. The K1
  stop #166 (`0F 1F`) became the first measured K2 input.
- BT/BTS/BTR/BTC register and memory forms (`0F A3/AB/B3/BB`) and the
  `0F BA /4–7` group: bit strings with sign-extended bit-offset EA
  arithmetic per Intel SDM Vol 2A ("bit string base" may address below
  the operand address).
- MOVSXD (`0x63`) long-mode forms: REX.W sign-extends r/m32 into r64;
  without REX.W it degrades to a 16/32-bit move per the Intel table.
- `src/pit.c`: counter-0 terminal pulses are wired to BOTH boards the
  pin reaches on a PC: 8259 IRQ0 and I/O APIC INTIN2 (GSI2) — the
  standard MADT IRQ0→GSI2 override. The AuraLite kernel routes
  "PIT@GSI2" once it masks the PIC; measured delivered and consumed.

### Fixed — patch 0022 (measured on the live kernel, four root causes)
- **Descriptor tables are linear, not physical.** `read_descriptor`,
  `tss_base_from_gdt`, the IDT gate and IST reads in `raise_exception`,
  and the LTR busy-bit store now go through `read_mem_v`/`write_mem_v`
  (SDM Vol 3A §2.4.1: GDTR/IDTR hold linear addresses). Without this
  the first post-LGDT segment reload tore through garbage gates.
- **RIP-relative EA was eager.** `fixup_riprel` resolved `[rip+disp32]`
  against the decode-time PC, i.e. *before* any trailing immediate was
  consumed: every immediate-carrying instruction with a rip-relative
  operand landed its store short by the immediate length (measured on
  `gdt_init`: `movw` and `movq` immediate stores fell 2 / 4 bytes low,
  GDTR loaded `base=0xffffffff`, first `mov %eax,%ds` faulted on
  `0x10000000f`). EAs now resolve lazily at operand-use time
  (`rm_ea`); the IMUL `69/6B` fetch order was repaired to match.
- **CR2 must hold the faulting linear address** on #PF (both read and
  write translate failures). The kernel's free-lance page-fault
  machinery dispatches on CR2.
- **INVLPG is not a memory access.** The `0F 01 /7` operand is a page
  hint: real silicon never faults on an unmapped address. We model no
  TLB, so INVLPG is a pure no-op — previously it executed the group's
  shared eager read and re-entered the VMM self-test's own #PF handler
  mid-probe (measured: `paging_unmap+0xe0`, double-delivery loop).
- **No more TSS selectorism.** `tss_base_from_gdt` used to assume the
  TSS descriptor sits at GDT+0x10. AuraLite's slot 0x10 holds a data
  descriptor, which produced a bogus TSS base `0xffff00000000` and a
  spurious nested #PF inside every exception delivery. The helper now
  prefers the LTR-cached TR and validates S=0 / type∈{9,B} before
  trusting a fallback probe.
- `0F 01 /5` explicitly #UD (reserved encoding) instead of silently
  sharing the SGDT path.

### Regression rigs
- `tests/test_table.c` 344 → 357 rows: hint-NOP decode rows, the BT
  family incl. negative memory bit-string EA arithmetic, and the
  ARPL-scope #UD pin row for `0x63` in legacy modes.
- Negative controls hold per phase norm: pre-fix kernel trace receipts
  (triple fault at `gdt_flush`, GDTR `0xffffffff`, INVLPG #PF loop) vs
  post-fix receipts below.

### Measured result (kernel lane, `--kernel=AuraLite-OS/build/kernel.elf`)
- RC=0 to the 200M-instruction budget; no emulator FAULT.
- Serial receipts, in order: GDT loaded → IDT installed → PIC remapped
  → SYSCALL/SYSRET configured → **kernel banner block**
  ("Hello from AuraLite OS kernel!") → PMM self-test PASS → VMM
  self-test PASS (map/read/write/unmap) → heap/slab online → TSS loaded,
  IST armed → SMP init → IOAPIC @0xfec00000 probing → PIT 100 Hz →
  **timer self-test PASS (10 ticks in 100 ms via GSI2→LAPIC→ISR32)** →
  PCI scan enumerates all five synthetic devices (host bridge, P2P, VGA,
  EHCI, ISA bridge, AHCI).
- Next measured stop is not a semantic gap: the kernel entered its
  framebuffer bring-up and ran out of instruction budget mid-blit
  (~1.2 MIPS host speed; fb path is instruction-hungry).

### Added — patch 0021 (KERNEL-BOOT K1, direct kernel-load lane)
- `src/kloader.[ch]` + `--kernel=path` CLI: loads an AuraLite-OS
  `kernel.elf` straight into the machine — ELF64 PT_LOAD placement at
  physical (p_paddr − KERNEL_VMA), a 7-page paging hierarchy (identity and
  HHDM of the low 4 GiB share one PDPT of 1 GiB pages; kernel VMA mapped
  with 4 KiB PTEs), a fabricated 7,776-byte `boot_info_t` (ABI pinned by a
  `_Static_assert` lattice measured from the AuraLite header), and the CPU
  parked at `_start` in 64-bit long mode (CR0=PE|MP|NE|PG, CR4=PAE|PGE,
  EFER=LME|LMA, RDI=physical boot_info_t*, IF=0) per the contract read
  from `kernel/arch/x86_64/boot.asm`.
- Run marker: the log notes when RIP reaches the ELF's `kmain` symbol.
- `tests/kload_fixture.c` + `.ld`: a synthetic higher-half guest kernel,
  linked at the real AuraLite VMA (`-mcmodel=kernel`, frozen flags), which
  validates the identity/HHDM views of the fabricated `boot_info_t` and
  reports `K1OK` over COM1; `tests/test_kload.c` drives it plus three
  negative controls (unknown path, truncated ELF, flipped EI_CLASS).
  New `make test-kload` lane, wired into `make test`/`test-sanitize`.
- `docs/plans/KERNEL_BOOT_PLAN.md`: K0–K5 plan (north-star path).
- Measured on AuraLite-OS @ 0ed0d29 (`make kernel` with clang+lld):
  `kmain` reached 18 instructions in; the trace then stops at instruction
  166 on an unimplemented multi-byte NOP (`0F 1F`) — filed as K2's first
  item. `make test` and `make test-sanitize` RC=0, all prior lanes
  unchanged.

### Added — patch 0012 (CORE C11, differential fuzzer)
- `tests/diff_fuzz.c`: differential fuzzer vs the host x86-64 CPU via
  ptrace single-step. Curated mod=11 integer streams carry two encodings
  per instruction (host64 / real16 -- prefixes invert, short inc/dec are
  REX on host). Architecturally-undefined flags are masked per opcode
  class; arithmetic+DF flags are force-synced identically on both sides
  before every instruction, so CMOVcc/SETcc/ADC/SBB inputs stay
  deterministic. Part two: crash-invariant sweep over raw garbage streams
  (emulator must fault or halt cleanly, never crash the host process).
- `make fuzz` target (FUZZ_PROGRAMS/FUZZ_INSTR knobs) and a third GitHub
  Actions job. Measured: 12288 instructions bit-identical vs the host CPU
  in the default bring-up run; injected SUB-flag bug detected in 3
  instructions (negative control).

### Added — patch 0011 (CORE C10, the test rig)
- `tests/test_table.c`: table-driven ISA vector suite, 338 rows, one row
  per vector (name, machine-code hex, setup DSL, expectation DSL). Shared
  harness extracted to `tests/harness.h`.
- `make test-table` target, run by `make test`.
- `make test-sanitize`: ASan+UBSan lane (leaks and UB are hard errors),
  wired as a second GitHub Actions job.
- This changelog.

### Fixed — patch 0011 (measured by the new rig)
- ADC/SBB: CF and AF ignored the carry-in (`set_flags_add`/`set_flags_sub`
  gained a `cin` parameter). Caught by table vector `adc ax,-1 carry-in`.
- PUSH/POP Sreg (`06/07/0E/16/17/1E/1F`) raised #UD. Caught by `push cs`.
- IMUL `0F AF` (two-operand) and `69/6B` (three-operand) raised #UD.
- CLC/STC/CMC (`F8/F9/F5`) raised #UD.
- `pci.c hostbridge_write`: `cfg[0x63]<<24` in `int` was UB. Caught by UBSan.
- Leak-clean shutdown: `mem_done`, `devices_done`, `pci_done`, `main`
  teardown, shared-RAM test harness. Caught by LeakSanitizer.
- Makefile: `.DELETE_ON_ERROR`, `-Werror` on the whole tree, removed the
  duplicated `clean` line (a real defect listed in the C0 audit).

## [0001–0010] — CORE C0–C9 (previously)
- C0 audit + house-style plans; C1 near-Jcc decode; C2 shift/rotate group;
  C3 MUL/IMUL/DIV/IDIV with #DE; C4 encoding repairs; C5 exceptions,
  INT/IRET, real-mode IVT; C6 regression groups + negative controls;
  C7 string ops + INS/OUTS + REP semantics; C8 XCHG/CMOVcc/SETcc/MOVSX/
  CBW family/LOOP/BSWAP/XADD/CMPXCHG; C9 CPUID leaves, RDTSC virtual
  time, PAUSE/fences. See `git log` and `docs/plans/CORE_PLAN.md`.
