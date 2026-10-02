# AuraLite Emulator — CPU Core Correctness & ISA Completeness Plan

## Status: DONE — C0–C11 all delivered ✅

| Phase | Title | State | Deliverable |
|---|---|---|---|
| C0 | Baseline audit, measured repro of every defect, the plans themselves | ✅ done | `patches/0001-C0-baseline-docs.patch` |
| C1 | Near Jcc: missing opcodes, never-taken conditions, unified `eval_cc()` | ✅ done | `patches/0002-C1-jcc.patch` |
| C2 | Shift/rotate group: ROL/ROR/RCL/RCR, 8-bit forms, count masks, flags | ✅ done | `patches/0003-C2-shift.patch` |
| C3 | group3: MUL/IMUL/DIV/IDIV with `#DE` | ✅ done | `patches/0004-C3-muldiv.patch` |
| C4 | Encodings: PUSH imm16, `MOV r/m,Sreg`, SGDT/SIDT, SMSW/LMSW, MOV CRn width, 16-bit stack units | ✅ done | `patches/0005-C4-encodings.patch` |
| C5 | Exceptions: hardware-correct frame, real-mode IVT, INT/INT3/INTO, IRET, PUSHF/POPF, CALLF/RETF | ✅ done | `patches/0006-C5-exceptions.patch` |
| C6 | Regression tests: ISA vectors C1–C5, negative controls | ✅ done | `patches/0007-C6-tests.patch` |
| C7 | String ops MOVS/CMPS/SCAS/LODS + INS/OUTS + REP/REPE/REPNE semantics | ✅ done | `patches/0008-C7-strings.patch` |
| C8 | XCHG/BSWAP/XADD/CMPXCHG, CMOVcc/SETcc/MOVSX, CBW family, LOOP/JCXZ | ✅ done | `patches/0009-C8-datamove.patch` |
| C9 | CPUID leaves (brand, cache, address sizes), RDTSC virtual time, PAUSE/fences | ✅ done | `patches/0010-C9-cpuid.patch` |
| C10 | Test rig: table-driven ISA vectors ≥300, ASan/UBSan CI lanes, Makefile hygiene, CHANGELOG | ✅ done | `patches/0011-C10-rig.patch` |
| C11 | Differential fuzzer vs host CPU (flags diffing, no-crash invariant) | ✅ done | `patches/0012-C11-fuzz.patch` |

This document answers:

> *What stands between today's interpreter core — which boots the sample
> firmware because the firmware was grown around it — and a CPU core we are
> honestly willing to call correct enough to boot AuraLite OS?*

It follows the structure of the AuraLite OS plans (`FIXES_PLAN.md`,
`USB_PLAN.md`, `I386_PLAN.md`, `OPT_PLAN.md`): dependency-ordered phases,
a definition of done and a test gate for every phase, one `.patch` per phase.

**Baseline:** commit `33e2c5c` ("Feature update"), the tip of `main`.
Every defect below was reproduced and **measured at the baseline** with a
flat-machine harness (`gcc -Isrc harness.c src/cpu.c src/mem.c src/io.c
src/platform.c`), not assumed:

```
JO near (OF=1, must jump):  fault=1  "#UD unsupported 0F opcode 0x0F 0x80"
JS near (SF=1, must jump):  halted=1 ax=0x3333   (expected 0x4444 — silently NOT taken)
push imm16:                 bx=0x0000            (expected 0x2222 — decode runs 2 bytes ahead)
rol al,1 (D0 C0):           fault=1  "#UD unsupported opcode 0xd0"
mul bx:                     fault=1  "#UD group3 ext=4"
```

---

## 1. The problem, stated once

The CI is green and the sample boot chain works, so the natural claim is "the
CPU core is fine". The measurements above say otherwise, and they matter
more than the green, because of *why* the green exists:

- **The test suite passes because nothing in it exercises the broken space.**
  `tests/test_cpu.c` has three assertions; none touches a conditional jump,
  a shift count above 31, multiplication, division, the stack-unit widths, or
  exception delivery. "make test: ok" describes the tested subset, not the ISA.
- **The sample firmware works because it was grown against this exact core.**
  Every broken encoding above is one the sample firmware never happens to use.
  A second, independently-written guest (AuraLite OS's own bootloader, compiled
  C code, coreboot, SeaBIOS) walks straight into all of them.

Two of the defects are the worst kind — not `#UD` faults that announce
themselves, but **silent wrong answers**: near `JS/JNS/JP/JPE/JPO` decode
correctly and then never take the branch; `ROL/ROR/RCL/RCR /0–/3` decode and
then return the operand unchanged. A guest compiled by GCC hits both within
its first thousand instructions.

### 1.1 The defect ledger (baseline `33e2c5c`)

| # | Defect | Effect on an arbitrary guest | Phase |
|---|--------|------------------------------|-------|
| 1 | Near `JO/JNO/JB/JAE` (0F 80–83) unimplemented | immediate `#UD` | C1 |
| 2 | Near `JS/JNS/JP/JPE/JPO` (0F 88–8B) never take | silent control-flow corruption | C1 |
| 3 | Near Jcc ignores 16-bit operand size (always rel32) | decode desync in real/prot16 | C1 |
| 4 | group2 ROL/ROR/RCL/RCR: `/0–/3` return operand unchanged | silent data corruption | C2 |
| 5 | group2 8-bit forms C0/D0/D2 absent | `#UD` | C2 |
| 6 | Shift count masked to 31 even for 64-bit operands; CF/OF semantics missing | wrong shifts in long mode | C2 |
| 7 | group3 MUL/IMUL/DIV/IDIV absent; `#DE` exception absent | `#UD` | C3 |
| 8 | `PUSH imm` (0x68) always decodes imm32 | decode desync (measured above) | C4 |
| 9 | `MOV r/m16,Sreg` (0x8C), SGDT/SIDT, SMSW/LMSW absent | `#UD` | C4 |
| 10 | `MOV CRn, reg` always reads 8 bytes | harmless today, wrong in general | C4 |
| 11 | Stack-unit width forced to 8 in long mode | `66 50` (`push ax`) in long mode corrupts stack | C4 |
| 12 | Exception frame pushed in wrong order (err→CS→RIP→RFLAGS) | any future IRET breaks | C5 |
| 13 | Real-mode exceptions halt instead of dispatching through the IVT | `#DE` in a BIOS kills the machine | C5 |
| 14 | INTn/INT3/INTO, IRET, PUSHF/POPF, CALLF/RETF direct absent | `#UD`; no interrupt returns at all | C5 |

## 2. Decisions

**D1. Measured repro before repair.** C0 lands only measurement harness output
and the plans; it changes no behaviour. Each fix phase then cites the baseline
fact it removes. This is `USB_PLAN.md` U0's "tell the truth first" applied to
the CPU.

**D2. Fix + regression test in the same patch.** Every C-phase ships the golden
vectors that lock its repaired behaviour (C6 collects the harness extension).
A test that cannot fail is not a test: each phase's gate is verified red by
reverting the fix hunk against the phase's own tests (negative control, done
once per phase, logged in the phase result).

**D3. One shared condition evaluator.** `eval_cc(cc, rflags)` lands in C1 and is
the only place branch logic lives; short Jcc, near Jcc, and later
CMOVcc/SETcc (C8) all consume it. This removes the whole bug class of
§1.1 #1–#3 instead of the three instances.

**D4. No semantic inventions where hardware is undefined.** Where Intel marks a
result undefined (shift counts ≥ operand size, flags after MUL/DIV), the
implementation picks the deterministic behaviour real silicon exhibits
(count masks, last-bit-out CF) and the tests lock *that* choice, so guests see
the same answer on every run.

**D5. This plan changes no guest-visible contract that the sample firmware
depends on.** `make test` (unit + boot integration + 5-platform smoke) must be
green at the end of every phase. The phases repair the core; the firmware,
devices, and CLI are untouched.

## 3. Phases

### C0 — baseline audit & measured repro — ✅ done (this document)

**DoD:** every defect in §1.1 reproduced at the baseline with a printed
measurement; `ROADMAP.md` rewritten to point at this plan; no code changed.
**Gate:** review — the §1 measurements re-run and match the table.

### C1 — near Jcc — ✅ `0002-C1-jcc.patch`

Fixes #1–#3. Adds `eval_cc()` (all 16 conditions), rewires short Jcc onto it,
extends the near-Jcc opcode range to 0F 80–8F, and honours operand-size for
rel16 vs rel32.
**DoD:** all 16 near conditions branch correctly both ways (taken/not-taken)
in real16; rel16 honoured; no regressions.
**Gate:** `make test` green; new vectors `test_near_jcc_*` pass; negative
control: reverting the single `eval_cc` call turns `test_near_jcc_with_of` red.

### C2 — shift/rotate group — ✅ `0003-C2-shift.patch`

Fixes #4–#6. Adds ROL/ROR/RCL/RCR (through-CF rotations computed on a 128-bit
scaffold so 64-bit RCL/RCR are exact), the 8-bit forms C0/D0/D2, count masking
63/31 by operand width, and CF/OF-where-defined semantics.
**DoD:** measured baseline failures fixed: `rol al,1` → 0x03 with CF=1;
count ≥ operand size is deterministic and locked by tests.
**Gate:** vectors for all four rotates, both widths, counts {0, 1, 31, 63};
full `make test`.

### C3 — MUL/IMUL/DIV/IDIV — ✅ `0004-C3-muldiv.patch`

Fixes #7. All four operations in all four widths (128-bit intermediates for the
64-bit forms), CF/OF per the manual for multiplies, `#DE` on divide-by-zero and
quotient overflow, with the `INT_MIN/-1` UB traps guarded before division.
**DoD:** `mul bx` → DX:AX = 0x0012:0x3400, CF=OF=1; `div` quotient/remainder
correct; divide-by-zero raises vector 0 through the C5 IVT path once C5 lands
(and through `raise_exception` identically before).
**Gate:** per-width multiply/divide vectors; overflow → `#DE` vectors;
`make test`.

### C4 — encodings — ✅ `0005-C4-encodings.patch`

Fixes #8–#11. `PUSH imm` honours operand size; `MOV r/m16,Sreg` (0x8C);
SGDT/SIDT (`0F 01 /0 /1`); SMSW/LMSW (`0F 01 /4 /6`, PE sticky under LMSW);
`MOV CRn,reg` reads the architected width; stack-unit width honours the 0x66
override in long mode.
**DoD:** baseline `push imm16` case now yields bx=0x2222 AND [0x7FFE]=0x1234;
SGDT/SIDT round-trips LGDT/LIDT values.
**Gate:** encoding vectors; `make test`.

### C5 — exceptions & interrupt instructions — ✅ `0006-C5-exceptions.patch`

Fixes #12–#14. Exception frame now matches hardware (RFLAGS, CS, RIP, then
error code lowest); real-mode delivery through the IVT at physical 0 with the
3-word frame and IF/TF clear; 16-bit frames use 2-byte units; INTn/INT3/INTO
push the *next* instruction as the return RIP; IRET/IRETQ, PUSHF/POPF,
CALLF direct, RETF(+imm).
**DoD:** an `int 0x80` round trip through the IVT (handler MOV+IRET) resumes
correctly with registers intact; a divide-by-zero reaches the IVT0 handler
instead of halting the machine.
**Gate:** IVT round-trip vectors in both directions; `make test`.

### C6 — ISA regression vectors — ✅ `0007-C6-tests.patch`

Extends `tests/test_cpu.c` with a small program-runner and the golden vectors
locking C1–C5 (JO/JS taken+not-taken, shift masks, rotates, MUL/DIV,
PUSH imm16, PUSHF/POPF, INT/IRET round trip, `#DE` via IVT).
**DoD:** suite grows from 3 assertions to the full vector set; every vector was
observed red against the baseline (§1 measurements) or against a reverted fix.
**Gate:** `make test` green; negative controls logged in the commit message.

---
### Planned phases (rough sketches; each expands to full DoD when scheduled)

- ~~**C7 — string ops**~~ **done** (`0008`): MOVS/CMPS/SCAS/LODS/STOS plus
  INS/OUTS, full REP/REPE/REPNE semantics (ZF-qualified loop exit on CMPS/SCAS,
  zero count performs no access), segment override honoured on the source,
  16/32/64-bit pointer widths that wrap at their natural boundary, mid-loop
  exceptions resume through the IRET path. Vectors: `test_rep_*`,
  `test_*_scasb/cmpsb`, `test_ins_outs`.
- ~~**C8 — data movement**~~ **done** (`0009`): XCHG r/m8..64 (`0x86/87`)
  plus the `0x90–0x97` accumulator short forms (0x90 stays the degenerate
  NOP), BSWAP (32-bit lane, 64-bit lane under REX.W), XADD, CMPXCHG (LOCK
  is a no-op — the machine is single-threaded), CMOVcc/SETcc on the C1
  `eval_cc()` (CMOV reads its memory operand even when the cc is false),
  MOVSX, CBW/CWDE/CDQE and CWD/CDQ/CQO, LOOP/LOOPE/LOOPNE/JCXZ with the
  count register following the *address* size. Vectors: `test_xchg_forms`,
  `test_cmovcc_setcc`, `test_movsx_cbw_cwd`, `test_loop_jcxz`,
  `test_bswap_xadd_cmpxchg`.
- ~~**C9 — CPUID & time**~~ **done** (`0010`): leaves 2 (cache descriptors
  punting to leaf 4), 4 (deterministic L1D/L1I/L2, terminating), 7.0 (ERMS),
  brand string `AuraLite Virtual CPU, model 0x<profile>` (48B, space-padded),
  0x80000006 (L2: 64B/8-way/256KB), 0x80000008 (40-bit PA / 48-bit VA);
  RDTSC from virtual time (`instr_count × platform_t::tsc_per_instr`, new
  per-profile field), keeping runs deterministic; PAUSE (`F3 90`) and
  LFENCE/MFENCE/SFENCE (`0F AE /5-7` mod=11) as documented NOPs, other
  `0F AE` encodings stay #UD. Vectors: `test_cpuid_leaves`,
  `test_rdtsc_pause_fences`.
- ~~**C10 — the rig**~~ **done** (`0011`): `tests/test_table.c`, 338 rows in
  a one-line-per-vector DSL (name / machine-code hex / setup / expect),
  shared harness extracted to `tests/harness.h`, `make test-table` wired
  into `make test`. `make test-sanitize` (ASan+UBSan, leaks and UB are
  hard errors) runs as a second GitHub Actions job; all four suites pass
  under it. Makefile got `.DELETE_ON_ERROR`, the duplicated `clean` line
  is gone, `-Werror` is on. `CHANGELOG.md` started. **The rig paid for
  itself in the same patch**, measured: ADC/SBB carry-in flags, PUSH/POP
  Sreg, IMUL `0F AF` + `69/6B`, CLC/STC/CMC all missing/buggy; UBSan
  caught `cfg<<24` UB in pci.c; LSan caught ~6GB of exit-time leaks
  across the suites (now `mem_done`/`devices_done`/`pci_done` and a
  shared-RAM harness).
- ~~**C11 — differential fuzzer**~~ **done** (`0012`): random flat mod=11
  integer streams executed instruction-by-instruction on the host x86-64
  (ptrace single-step) and in the emulator; GPRs (low 16) and the
  architecturally-defined flag subset compared after every instruction.
  Each generated instruction carries two encodings (host64 / real16 --
  operand-size prefixes invert, 0x40-0x4F are REX on the host). Undefined
  outputs are masked per opcode class and both sides' flags are force-
  synced to the same random value before every instruction, keeping
  CMOVcc/SETcc/ADC/SBB inputs deterministic. Crash-invariant sweep over
  raw garbage streams in the same binary. `make fuzz` + third CI job.
  Measured: 12288 instructions bit-identical at bring-up; an injected
  SUB-flags defect caught in 3 instructions (negative control).

## 4. What this plan does not do

- **No interrupt *hardware*.** PIC/PIT/RTC/KBC/A20/LAPIC belong to
  `CHIPSET_PLAN.md` (planned). C5 only builds the CPU-side door
  (frame + IRET + IVT) they will walk through.
- **No performance work.** Owned by `PERF_PLAN.md` (planned); correctness lands
  first so the differential rig (C11) protects the fast paths.
- **No 32/64-bit guest mode fixes beyond the ledger.** The sample firmware's
  long-mode path is the integration test; deeper mode semantics (CPL, limits,
  TSS) are `COMPAT_PLAN.md` material on the road to AuraLite OS.
- **`web/`, devices, and the CLI are untouched** except where a phase's own
  gate requires a `Makefile` line.

## 5. Definition of done for *every* phase

Landing a phase means, in the same patch where applicable:

- the fix plus its regression vectors (D2);
- `make test` green (unit + USB + boot integration + 5-platform smoke);
- the phase row here flipped with the measured evidence;
- no known-defect silently re-scoped — deviations are written into the phase
  result, the way `I386_PLAN.md` does ("scope per phase results").

## 6. Ledger

| Commit | Phase | Adds |
|---|---|---|
| `0001` | C0 | this plan, ROADMAP (index), PROJECT_ANALYSIS (audit) |
| `0002` | C1 | `eval_cc`, near-Jcc opcodes 0F 80–8F, rel16, short-Jcc rewire |
| `0003` | C2 | group2 complete (C0/C1/D0–D3, ROL/ROR/RCL/RCR, flags) |
| `0004` | C3 | group3 MUL/IMUL/DIV/IDIV, `#DE` |
| `0005` | C4 | encodings (#8–#11) |
| `0006` | C5 | exception frame, IVT, INT/IRET/PUSHF/CALLF/RETF |
| `0007` | C6 | ISA regression vectors, README scope note |
