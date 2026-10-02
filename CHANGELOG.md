# Changelog

All notable changes to AuraLite-Emulator are recorded here. The project
ships as a `patches/` series on top of the baseline commit `33e2c5c`; each
entry names its patch and the measured defect(s) it repaired.

## [Unreleased]

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
