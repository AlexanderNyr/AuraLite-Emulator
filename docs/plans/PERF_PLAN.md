# PERF plan — measure before optimizing the interpreter

> **House style** (`ROADMAP.md`, `CORE_PLAN.md`): each phase is one
> `git format-patch` in `patches/`, carrying its implementation, regression
> vector, negative control and docs together. A measured defect and a passing
> test, not an attractive optimization, authorize each change. `make test` and
> `make test-sanitize` remain green at each phase boundary.

**Status: P0 ✅ (`patches/0051-PERF-P0-honest-benchmark.patch`),
P1 ✅ (`patches/0052-PERF-P1-profiled-ram-read.patch`),
P2 ✅ (`patches/0053-PERF-P2-coherent-success-tlb.patch`),
P3 ✅ (`patches/0054-PERF-P3-ram-prefix-predecode.patch`);
P4 **NO-GO** (measured dispatch hypothesis rejected; no CPU patch),
P5 ✅ (`patches/0055-PERF-P5-paired-confidence-closeout.patch`).** P4's
"stop if not hot" gate is an outcome, NOT a claimed dispatch optimization.
The P5 close-out adds paired uncertainty estimates and a guest-work/clock
gate without changing the emulator binary.

| Phase | Deliverable | Gate | Status |
|---|---|---|---|
| P0 | Work counter + opt-in `--stats`; seven-tape wall-time rig; cap/idle negative control | `make test-perf`, `make bench`, `make test`, `make test-sanitize`, `make fuzz FUZZ_PROGRAMS=256` | ✅ 0051 |
| P1 | Measured-hot 8-byte full-span RAM read fast path; ROM/MMIO/A20/boundary remain on old path | six bus vectors + LE-swap NC, differential fuzz, seven P0 tapes paired pre/post | ✅ 0052 |
| P2 | Success-only 4KiB-subpage TLB; conservative RAM-store/DMA and control-operation flushes, security-context key | warm-PTE rewrite, #PF CR2/err/RIP, CPL3/WP/NX, MMIO, PS, SMP vectors, NC + differential fuzz + P0 tapes | ✅ 0053 |
| P3 | RAM-only single-page prefix/opcode predecode; dynamic ModRM/SIB/imm never cached; shared P2 write generation | SMC/PTE/MMIO/NX/mode/boundary vectors + one NC + fuzz + paired P0 tapes | ✅ 0054 |
| P4 | Profile-guided dispatch ONLY if measured; no change if gate fails | O2 `-pg`/gprof + gcov + paired seven-tape scratch probe | NO-GO; no patch |
| P5 | Paired local bootstrap CI + strict work/clock gate + history/ledger close-out | unit vectors + one NC, all gates, 9 available historical lanes, fresh-clone `git am` | ✅ 0055 |

## 1. Measured baseline (`AuraLite-Emulator@a3f76b5` → P0)

- Before P0 there was **no benchmark** in `Makefile` or `tests/` and no
  host wall-clock gate. All existing gates compared guest STATE. The boot
  run with `--platform=haswell --max-instr=20000000 --no-fb` took 0.685 s
  but halted at `instr=4837677`. Dividing the *cap* 20M by that time gave
  **28.69 MIPS**; dividing the actual count by the same time gave **7.06
  MIPS**. The first number was mistakenly quoted during plan selection;
  this phase arose because the error was caught by checking the register
  dump. The precise wall number is machine-relative; the miscount factor
  20000000/4837677 ≈ 4.13 is not.
- Worse, `cpu.instr_count` is itself **not** the work count. On `STI;HLT`
  followed by two idle slots, `cpu_step` charges `+512` per slot to both
  `cpu.instr_count` and `m->vtime_instr` (`src/cpu.c`'s all-idle branch)
  while executing **zero** further instructions. A measured tiny C harness
  (`/tmp/t_idle.c`) returned `instr_count=vtime=1026` for just two actual
  steps. That identity is the K6 guest-clock contract; altering either
  clock to manufacture a performance numerator would break PIT/RTC/LAPIC/
  RDTSC and the historical boot tapes.
- `cpu_step` reaches a single `done:` block for a dispatched instruction,
  including faults (`src/cpu.c` near the end of the function), but returns
  earlier for HLT idle slots and reset bookkeeping. P0 adds
  `cpu.executed_steps++` beside `instr_count++` **only** in that block. A
  faulting attempt counts once: this is an instruction-*attempt* metric,
  **not** architectural retirement. `--stats` prints a per-vCPU count,
  a machine-wide sum (`machine_executed_total`), `idle_quantum`, BSP clock,
  virtual clock and cap flag; without the option, stdout is unchanged.
- `m->n_vcpus==0` is the **normal single-context machine** (`main.c` only
  sets it to 2 for `--smp>=2`). P0's first `--stats` draft looped 0 times
  and printed `executed=0` on UP despite `instr_count=4837677`. It was
  caught by running the built binary and repaired by making the effective
  context count max(1, n) in a shared helper, with an explicit unit vector.
- First bench draft accidentally appended `--no-fb` to **every** tape;
  this made `gfx-fb` and `text-no-fb` the same workload. Identical receipts
  exposed it. P0 now passes each tape's arguments verbatim. No timing
  assertion gates on a fixed ns/instruction or MIPS: this would fail on a
  slower or noisier runner without a code regression.
- The local frozen guest (`AuraLite-OS@0ed0d29`) has no `build/kernel.elf`
  or `build/initrd.tar`; clang, ld.lld, nasm, mformat/mcopy, lld-link,
  llvm-rc and rustc are unavailable. P0's seven tapes use the existing
  sample firmware plus three pre-existing nasm artifacts. `--kernel=`
  guest lanes remain SKIP here and are not silently replaced with a fake
  kernel measurement.

## 2. Defect ledger

| ID | Symptom, repro and source | Remedy / later work |
|---|---|---|
| D0 | 20M cap / 0.685 s → false 28.69 MIPS though `instr=4837677` / 0.685 s → ~7.06. No bench existed. | P0 `make bench`: parse actual `executed=`, reject `cap_reached=1`, print ratio; NC uses cap as numerator and fails (RC 1). |
| D1 | `STI;HLT` + two idle slots: `instr_count=vtime=1026` while only 2 instructions were attempted; K3 `+512` is intentional. | P0 separate `executed_steps=2`, `idle_quantum=1024`, K6 identity unchanged; deterministic unit vector. |
| D2 | `m->n_vcpus=0` for a normal UP run; naive summation counted zero work. | P0 `machine_executed_total` counts BSP for 0/1 contexts; vector tests 0,1,2. |
| D3 | Every gate in `Makefile` compared state; a slowdown could pass all of them. | P0 wall-time median from five runs of each tape; report only. Later P1–P5 compare paired builds on same host, not absolute machine-external thresholds. |
| D4 | Appending `--no-fb` to all tapes made `gfx-fb` identical to `text-no-fb`. | P0 per-tape argv verbatim; printed labels correspond to distinct CLI paths. |
| D5 | Five `-O2 -pg` firmware runs: `mem_read` **313,666,250** calls, **45.66%** of profile samples, **235,535,030** calls from `translate`'s 8-byte page-table walk. The old RAM case loops `size` times, bounds-checking every byte. | P1 one 8-byte `memcpy` (no alignment/aliasing UB) after ROM checks, only if A20 open and all 8 RAM bytes in bounds; big-endian hosts use the old LE loop. Post profile: same 313,666,250 calls, 14.37% of samples. |
| D6 | After P1, five O2 gprof boots: `translate` **79,798,615** calls, **22.10%** of samples (0.40/1.81 s); page-table reads still dominate. An unconditional cache would stale after guest/DMA page-table writes or a permission switch and could suppress #PF. | P2 success-only, RAM-backed 256-slot cache: global generation bump on every RAM `mem_write` + explicit INVLPG/CR/EFER/reset; key includes CR3, CPL/descriptor override, mode, WP/SMEP/SMAP/NXE and AC. Dynamic MMIO/ROM tables and A20-off walks never cached. NC drops RAM flush and reddens the exact PTE-rewrite assertion. |

## 3. P0 — one patch, measured gates

### 3.1 Code and vector

`src/cpu.h`: `executed_steps`, reset by the existing `cpu_reset` `memset`.
`src/cpu.c`: increment once alongside `instr_count` at `done:` only; **never**
in the K3 all-idle branch. `src/machine.h`:
`machine_executed_total()` — same implementation shared by the regression
vector and the product. `src/main.c`: opt-in `--stats`; it emits nothing
when the flag is absent, after the historical CPU register dump, before
last log lines. `tests/test_perf.c`: six deterministic vectors: NOP×1000
+ HLT = 1001 steps; STI+HLT+two idle slots = 2 steps/1026 vtime; K6 identity
at 514; faulting UD2 attempt counted once; context counts 0/1/2 =
10/10/17; bigger step budget cannot inflate an already halted tape.
`test-perf` joins `make test` **and** `make test-sanitize`.

### 3.2 Bench and negative control

`make bench [BENCH_REPS=5]` compiles a separate O2 `x86emu-bench`
with `BENCH_CFLAGS` and runs seven firmware tapes. The separate binary is
necessary: `make test-sanitize` replaces `x86emu` with O1 ASan+UBSan and
make cannot infer changed CFLAGS from timestamps. `bench-binary` is phony
and rebuilds before each bench run, so the default measurement does not
silently inherit the last test's flags. `tests/perf_bench.sh` can still be
run with `EMUX=path/to/other-build` to compare an explicit build.
The tapes are: haswell, broadwell,
baytrail, text-no-fb, gfx-fb, VBE 800x600x32 and two-vCPU. Each tape is
repeated an odd number of times (default 5), and the **actual sample
median** of the outside-the-process wall time is reported in ms, with
Msteps/s (labeled `mips` for familiar comparison), ns/attempt, and the
cap-based *wrong* number so the difference cannot hide. It gates the
non-timing facts: every repetition must report identical
executed/vtime/idle/cap status, executed > 0, no tape hits its cap. Even
sample counts are rejected. The ratio between cap and actual work is a
property of the workload, not of the stopwatch.

**NC P0, one for the phase:** `make bench-nc BENCH_REPS=3` / `bash
 tests/perf_bench.sh --reps=3 --nc` measures exactly the same tapes but
**presents the cap as the MIPS numerator**. Receipt: `perf bench NC:
FAILED AS DESIGNED (cap-based MIPS is not a measurement)`; the direct
script exits **RC=1**, while `make bench-nc` reports **RC=2** (`make` wraps
the failed recipe: `Error 1`). Both forms are intentionally red;
for haswell boot the inflation factor was **10.34×** (50M / 4,837,677).
This NC fails for a semantic reason, not because of wall-time jitter.
`make bench` on the same tape uses 4,837,677; it passes RC=0.

### 3.3 Measured local baseline (O2, 5 reps; machine-relative)

| Tape | Steps attempted | Median (ms) | Msteps/s | ns/step | Cap-based / actual |
|---|---:|---:|---:|---:|---:|
| boot-haswell | 4,837,677 | 678 | 7.14 | 140.1 | 10.34× |
| boot-broadwell | 4,807,694 | 666 | 7.22 | 138.5 | 10.40× |
| boot-baytrail | 4,807,650 | 668 | 7.20 | 138.9 | 10.40× |
| text-no-fb | 4,837,677 | 676 | 7.16 | 139.7 | 10.34× |
| gfx-fb | 4,837,677 | 671 | 7.21 | 138.7 | 10.34× |
| vbe-800x600x32 | 4,837,677 | 671 | 7.21 | 138.7 | 10.34× |
| smp2 | 4,837,677 | 985 | 4.91 | 203.6 | 4.13× |

These are not golden wall-time asserts; workloads may differ by platform,
and 2-vCPU scheduling overhead is not an optimization result. Profiles,
CPU governor, available cores and thermal state affect any host timing.
The printed *work counts* were identical over all 5 repetitions. Do not
compare these ms values to another machine. For a future change, benchmark
before and after on the **same** runner, preferably alternating builds to
control host drift, while separately asserting byte-identical guest output
and a fixed count of work per tape.

### 3.4 Historical receipts and gates

Built pre-P0 from `HEAD=a3f76b5` in `/tmp/p0_pre`, built P0 separately.
Four historical 600k-cap tapes (firmware boot, `--vbe=800x600x32`,
`--vbe=off`, `--no-fb`) with the **same** `--log` and `--dump-fb` paths:
pre/post stdout+log+PPM were byte-identical, RC=0/0 for each. First
comparison used different filenames for the output PPM, causing one
*path-echo line* in stdout to differ while bytes of log+PPM matched; a
controlled rerun with the same pathname fixed the harness artifact and
proved byte identity. PPM SHA-256 prefixes: first three
`15c96625cfec5acc`, `--no-fb` `c3b3d73f6bbb08ab` (same as VIDEO V4).
`make test`: RC=0 (`test-perf: all vectors passed`, `boot integration test:
ok`, N2/N3 receipts ALL PASS); `make test-sanitize`: RC=0; `make fuzz
FUZZ_PROGRAMS=256`: RC=0 (`256 programs, 6144 instructions compared --
all states match`, `0 host-level crashes`). SKIP on nasm-driven lanes and
the frozen guest: the required tools/artifacts are absent, not a passing
claim about those lanes.

### 3.5 Fresh-clone patch verification (measured)

Clone at **`a3f76b5`**; set git committer identity in the clone, then apply
`git am "$REPO/patches/0051-PERF-P0-honest-benchmark.patch"` (`$REPO` is
the source checkout). `git am` applied cleanly. `git rev-parse HEAD^{tree}`
was **identical** to the development tree. Seed the three nasm-generated
files using `NET_PLAN.md` §3.9; `make -n firmware/firmware.bin disk/disk.img`
reported both **up to date**. In this fresh clone: `make test` **RC=0**
(`test-perf: all vectors passed`, boot integration ok, N2/N3 ALL PASS),
`make test-sanitize` **RC=0**, `make bench BENCH_REPS=3` **RC=0** (all seven
work counts deterministic, none capped), `make bench-nc BENCH_REPS=3`
**RC=2** (`perf bench NC: FAILED AS DESIGNED`, inner script RC=1).
The clone required local committer identity for `git am`; without it,
Git returns `fatal: empty ident name` **before applying the patch**.
Do not bake a patch size, tree hash or host timing into this document:
the patch itself must not contain itself, and quoting its own sha/size
creates an amend loop. Historical tapes do not move with `--stats` absent.

## 4. P1 — measured 8-byte RAM read, one patch

### 4.1 Profile that authorized the change

The P0 O2 build was compiled with `-pg -g` and run five times on the same
firmware-boot tape (`--platform=haswell --max-instr=50000000`). `gprof -b`
aggregated five `gmon.out` files: `mem_read` **313,666,250** calls,
**45.66%** of 0.01-second flat-profile samples (1.21 of 2.65 sampled
seconds); **235,535,030** calls from `translate`, which walks four levels
of page tables via *8-byte* reads. `mem_write` saw only **2,514,365** calls,
**0.00%** of samples; optimizing writes on a hunch would not follow the
measured hotspot. `translate` itself consumed 14.72% of the remaining
sampled time. This justifies the narrow 8-byte RAM *read* fast path, not
a general rewrite of the physical bus or a translation-cache claim.

`src/mem.c`: after existing A20 mask and **both ROM alias checks**, an
unaligned-safe `memcpy` loads exactly 8 bytes if `a20` is open,
`size == 8`, and `phys <= RAM_SIZE - 8`; the `#if` restricts it to a
little-endian host. This condition is subtraction-based (no overflow from
`phys+8`). Disabled A20, ROM at 0xE0000 (inside RAM!), top-of-4GB ROM,
partial reads at RAM end, all non-8 widths, and MMIO retain the original
path. Writes are untouched. `memcpy` avoids a misaligned `uint64_t *` load
and C strict-aliasing UB. It does not change the guest clock or instruction
work count.

### 4.2 Regression vector and negative control (same patch)

`make test-perf-mem` builds `tests/test_perf_mem.c` against production
`mem.c`: six groups cover (1) unaligned 8-byte LE values, (2) last 8/7/1
RAM bytes with 0xFF partial fill and clipped writes, (3) both ROM aliases,
ROM wrap and the existing *start-address* decode across the low ROM
boundary, (4) closed A20 and start-only bit-20 mask, (5) widths 1/2/4,
(6) BAR read callback versus the existing RAM-priority decode. All groups
passed RC=0, with `test-perf-mem: all vectors passed`. The P1 unit target
is in both `make test` and `make test-sanitize`.

A test-harness correction happened before shipping: the first A20 vector
used `0x1FFFFE`, which after the bit-20 mask becomes `0x0FFFFE` inside
the low ROM alias. Its RAM expectation was wrong (RC=134); the shipped
vector uses `0x3FFFFE` → `0x2FFFFE`, crossing 3 MiB in ordinary RAM.
That correction is an input-fixture fix, **not** an emulator defect.

**Exactly one NC for P1:** `tests/integration/p1_mem_nc.sh` copies `mem.c`
to a temporary directory, changes only the fast-path return to
`__builtin_bswap64(v)`, compiles the **same** P1 vector and requires the
first assertion text `mem_read(&g_m, 0x123, 8) ==
0x1716151413121110ULL` to fail. It does: inner binary **RC=134**, NC
script **RC=1**, `make test-perf-mem-nc` **RC=2** (`make` reports recipe
Error 1), receipt `P1 NC: FAILED AS DESIGNED (swapped-endian RAM fast path
is detected)`. The checkout is not mutated. The same vector also guards
RAM/ROM/A20 boundaries; the NC deliberately targets endian fidelity,
which a generic memcpy on the wrong host could silently violate.

### 4.3 Performance and guest identity (measured, not a CI threshold)

Five post-change `-O2 -pg` runs on the same firmware workload: `mem_read`
**313,666,250 calls** (unchanged), **14.37%** of sampled time (0.26 of
1.81 sampled seconds). gprof sampling is noisy; do not treat its
percentages as a portable speedup. `make bench` was run as alternating
**pre1 → post1 → pre2 → post2**, each seven tapes ×3 repeats, same O2
compiler and firmware/disk inputs. Every one of the seven work counts was
identical across all four passes; no run reached its cap. The paired
wall-time ratios pre/post were:

| Tape | Attempts | Pair 1 (ms; ratio) | Pair 2 (ms; ratio) |
|---|---:|---:|---:|
| boot-haswell | 4,837,677 | 691→514; 1.34× | 783→427; 1.83× |
| boot-broadwell | 4,807,694 | 680→507; 1.34× | 701→433; 1.62× |
| boot-baytrail | 4,807,650 | 679→470; 1.44× | 670→419; 1.60× |
| text-no-fb | 4,837,677 | 674→570; 1.18× | 687→421; 1.63× |
| gfx-fb | 4,837,677 | 676→486; 1.39× | 680→420; 1.62× |
| vbe-800x600x32 | 4,837,677 | 690→467; 1.48× | 688→427; 1.61× |
| smp2 | 4,837,677 | 1116→747; 1.49× | 1004→710; 1.41× |

The two pre1/pre2 haswell medians alone differ 13%, and post1/post2
medians differ 17%; host load/frequency clearly moved. The limited
local evidence supports a real reduction of `mem_read` cost and faster
*sampled* tapes, **not** a universal fixed 1.5× speedup. There is no
absolute wall-clock gate, and a regression claim must compare builds on
one host with the same input and compile flags.

Four historical 600k-cap lanes with identical paths for `--log` and
`--dump-fb`: firmware boot, `--vbe=800x600x32`, `--vbe=off`, `--no-fb`.
Pre/post stdout, ring log, PPM byte-identical, each RC=0/0; PPM SHA256
prefixes `15c96625cfec5acc` (first three) and `c3b3d73f6bbb08ab`
(no-fb). `make test` **RC=0** (P1 vector + boot + N2/N3 PASS),
`make test-sanitize` **RC=0**, `make fuzz FUZZ_PROGRAMS=256` **RC=0**
(256 programs / 6,144 instructions, states match, 0 host crashes).
Frozen guest lanes remain SKIP because its toolchain/build are absent;
this is not a claim of fresh guest end-to-end performance.

### 4.4 Fresh-clone patch gate (measured)

From parent `2a10cf4` (`PERF P0`), `git am
"$REPO/patches/0052-PERF-P1-profiled-ram-read.patch"` applied cleanly;
the resulting `HEAD^{tree}` was identical to the development tree. The
three ignored nasm artifacts (firmware.bin, test_kernel.bin, disk.img)
were seeded as in `NET_PLAN.md` §3.9; `make -n` said both build targets
up to date. In that fresh clone: `make test` **RC=0** (`test-perf-mem:
all vectors passed`, boot integration ok, N2/N3 ALL PASS), `make
test-sanitize` **RC=0**, `make fuzz FUZZ_PROGRAMS=256` **RC=0**
(6,144 differential instructions, all states match, 0 host crashes),
`make bench BENCH_REPS=3` **RC=0** (seven tapes with stable work, none
capped), and the one P1 NC `make test-perf-mem-nc` **RC=2** (inner binary
RC=134, wrapper RC=1, exact LE assertion in §4.2). Frozen-guest lanes
SKIP; no claim is made about a new guest build.

Do not put a checksum or size of this patch into itself: those numbers
change when its documentation changes, causing an amend loop.

## 5. P2 — coherent success-only translation cache, one patch

### 5.1 Evidence and narrow contract

After P1, five O2 `-pg` firmware boots aggregated by `gprof -b` measured
`translate` **79,798,615** calls and **22.10%** of sampled time (0.40 of
1.81 s). Each page-table walk rereads 2–4 entries via `mem_read` even on
unmodified code fetches. P2 is justified by *that* hotspot, not by a
hypothetical guest TLB requirement. The first P2 boot with `--stats` kept
`executed=4837677`, `vtime=4837677`, and measured **15,863,692** TLB hits,
**4,668** misses and **4,669** global flushes; numbers are for this exact
firmware lane only.

`src/machine.h` holds 256 direct-mapped entries, shared by the machine so
K6 vCPU load/store does **not** copy a large array each instruction. The
entry key is 4KiB VA subpage, full CR3/CR0/CR4/EFER, RFLAGS.AC, CPL,
`desc_sv` and read/write/execute mode. This also handles 2MiB and 1GiB PS
leaves by caching each touched 4KiB subpage's physical base. Only successful
walks are inserted **after** every old permission check. Faulting/nonpresent
walks are **never** negative-cached: the original walker still raises every
#PF, with its original CR2, error bits, RIP and IDT delivery ordering.
No virtual clock or `cpu.executed_steps` semantics changed. `--stats`
adds `tlb_hits/tlb_misses/tlb_flushes` opt-in; without it, stdout is
unchanged.

For a cacheable walk, all PML4E/PDPTE/PDE/PTE reads must have started in
ordinary RAM, outside the low BIOS ROM alias (`0xE0000..0xFFFFF`), with
A20 open. A dynamic MMIO-backed PML4E is NOT cached (regression vector
switches callback root without a RAM write); closed-A20 walks always use
the old path. `mem_write` to RAM -- from the guest or from device DMA
(USB, AHCI, e1000; measured all use this bus API) -- increments a global
generation. This invalidates **all** contexts in O(1), including writes to
ordinary data, because table ownership is not tracked. ROM/MMIO writes
never mutate cached *ordinary-RAM* tables. MOV CR0/CR3/CR4, LMSW, WRMSR
EFER, INVLPG and machine reset explicitly flush; INVLPG remains a
no-fault operand hint (K2), with a conservative *whole-TLB* flush.
`kloader.c` invalidates after its direct pre-execution table construction.
Direct raw `m->ram` writes by an external *host harness after CPU start*
are outside the guest/DMA bus API; such a harness must call
`perf_tlb_flush` after modifying a live page table. Production devices
only read raw VGA text RAM; their DMA writes use `mem_write`.

### 5.2 Same-patch vectors and one negative control

`tests/test_tlb.c` shares the production `translate` path through actual
long-mode instructions, not a copied paging model. Its groups assert:

1. warm cache hit; guest `mem_write` PTE remaps VA from 5MiB to 6MiB;
   next `MOV RAX,[RBX]` MUST return `NEW_VALUE`;
2. guest `MOV CR3` switches root; `INVLPG [unmapped]` flushes without
   reading/faulting its operand; same-value guest MOV CR0/MOV CR4,
   WRMSR EFER and LMSW also flush;
3. warmed successful read → nonpresent #PF (CR2=VA, err=0, RIP=handler),
   NX fetch (err=0x11), supervisor WP write (err=0x3), CPL3 U/S read
   (err=0x5), with the original delivered handler RIP;
4. 2MiB/1GiB PS leaves preserve the right 4KiB offset; closed-A20
   bypasses cached entries; a dynamic MMIO page-table root is re-read
   every time (uncacheable); shared SMP contexts with distinct CR3/AC
   cannot borrow each other's result.

`make test-tlb` joins both `make test` and `make test-sanitize`, RC=0 with
`test-tlb: all vectors passed`. **Exactly one NC for P2:**
`tests/integration/p2_tlb_nc.sh` copies `mem.c` into a scratch directory
and removes ONLY the RAM-store generation bump, then builds/runs the
**same** P2 vector. On the warmed PTE rewrite its first `assert(c->gpr[RAX]
== NEW_VALUE)` fails: inner binary **RC=134**, NC script **RC=1**,
`make test-tlb-nc` **RC=2** (`make` wraps recipe Error 1). Receipt:
`P2 NC: FAILED AS DESIGNED (RAM-store flush is necessary for PTE
coherence)`. No production source is mutated. No timing-dependent assert
is used to prove correctness.

### 5.3 Measured performance and historical identity

Five post-P2 O2 `-pg` firmware boots: `translate` calls remained
**79,798,615**, but its self time fell from 0.40 to **0.21** sampled seconds
(22.10%→18.92% of the sampled profile). `mem_read` fell from
**313,666,250** to **78,188,375** calls: cache hits removed the repeated
page-table walks; the guest work count stayed 4,837,677 on every run.
Sampling percentages and host wall time are not cross-machine performance
contracts. Two alternating pre1/post1/pre2/post2 O2 series, seven tapes
×3 repetitions each, produced identical work counts on all tapes:

| Tape | Work | Pair 1 pre→post (ms; ratio) | Pair 2 pre→post (ms; ratio) |
|---|---:|---:|---:|
| boot-haswell | 4,837,677 | 429→328; 1.31× | 420→333; 1.26× |
| boot-broadwell | 4,807,694 | 422→326; 1.29× | 417→325; 1.28× |
| boot-baytrail | 4,807,650 | 418→342; 1.22× | 419→343; 1.22× |
| text-no-fb | 4,837,677 | 447→332; 1.35× | 419→348; 1.20× |
| gfx-fb | 4,837,677 | 428→372; 1.15× | 420→330; 1.27× |
| vbe-800x600x32 | 4,837,677 | 421→354; 1.19× | 420→362; 1.16× |
| smp2 | 4,837,677 | 767→568; 1.35× | 708→566; 1.25× |

Four historical 600k-cap tapes (firmware boot, VBE on/off and `--no-fb`)
using the SAME `--log` and `--dump-fb` paths: pre/post stdout, ring logs
and PPM were byte-identical, RC=0/0; PPM SHA256 prefixes
`15c96625cfec5acc` (first three) and `c3b3d73f6bbb08ab` (no-fb).
`make test`, `make test-sanitize`, `make fuzz FUZZ_PROGRAMS=256` and
`make bench BENCH_REPS=3` all **RC=0**; 6,144 differential instructions
matched, zero host crashes; N2/N3 ALL PASS. Frozen guest lanes SKIP
(missing its build toolchain), so this does not assert guest QEMU-parity
speed. No wall-time threshold is gated in CI.

### 5.4 Fresh-clone patch gate (measured)

From parent `f7c3bd7` (P1), `git am
"$REPO/patches/0053-PERF-P2-coherent-success-tlb.patch"` applied cleanly;
`HEAD^{tree}` was **identical** to the development tree. The three
ignored nasm artifacts were seeded as in `NET_PLAN.md` §3.9; `make -n`
reported both firmware and disk targets up to date. In that fresh clone:
`make test` **RC=0** (`test-tlb: all vectors passed`, boot integration
ok, N2/N3 ALL PASS), `make test-sanitize` **RC=0**, `make fuzz
FUZZ_PROGRAMS=256` **RC=0** (6,144 instructions, all states match,
zero host crashes), `make bench BENCH_REPS=3` **RC=0** (all seven tapes
executed deterministic work, none capped), and the one P2 NC `make
test-tlb-nc` **RC=2** (scratch binary RC=134, script RC=1,
`assert(c->gpr[RAX] == NEW_VALUE)`). Frozen guest lanes SKIP without
its absent toolchain; no claim of guest speed.

Do not embed the patch's own SHA or size inside it; doing so creates an
amend loop rather than additional evidence.

## 6. P3 — RAM-backed prefix/opcode predecode, one patch

### 6.1 Evidence, scope and coherence

Post-P2 O2 five-boot `-pg` profile: `decode_modrm` accounted for 8% of
sampled time (19,283,235 calls / five), `fetch8` 6% (72,477,350 calls /
five); O2 coverage counted 970,913 `group1` entries per 4,837,677
instruction-attempt boot, and that arm re-fetches prefixes/opcode from
`start_pc` before live ModRM/imm. Caching `rm_t` would be WRONG because
GPR-dependent effective addresses change even when instruction bytes do not.

A 512-slot machine-wide P3 cache therefore stores ONLY prefixes/opcode and
the cursor just after opcode. It is keyed by RIP, CS base/selector/L/D_B,
CR0/CR3/CR4/EFER, AC and descriptor-supervisor override; it uses P2's
RAM-store generation. Install requires a successful P2 T_EXEC entry for
ordinary RAM, A20 open, paging active, physical page fully in RAM and not
the legacy ROM alias, and the entire prefix/opcode stream within ONE
4KiB page. This rejects faulting/MMIO-table/MMIO-code/ROM, A20-off and
page-crossing fetches. `--trace` and EMU_DBG_U use the original fetch path.
Any guest/DMA `mem_write` to RAM invalidates both P2 and P3 (including
PTEs and self-modifying code); paging/control/reset flushes share the epoch.
Direct host writes through `m->ram` after execution begins must explicitly
call `perf_tlb_flush()` (test_ring3's instruction injector now does this);
pre-execution fixture writes need no flush. Dynamic ModRM/SIB/displacement,
all immediates, operands and 0F extension bytes are always fetched/decoded
live. The historical `group1` re-fetch is bypassed ONLY on a P3 hit;
all old slow paths retain its original behavior and fetch ordering.

### 6.2 Regression and negative control

`make test-decode` checks warm hits and RAM opcode rewrite (STC→CLC),
PTE remap 5→6MiB, dynamic SIB with changed RCX, a cached RAM opcode
with a live side-effecting MMIO imm32 on the next page, cross-page
prefix/opcode rejecting cache, CS.L/D_B mode key, trace bypass, warm EXEC
then NX #PF (`CR2=0x400000`, err `0x11`, handler RIP `0x9070`, one attempt),
A20-closed/nonpaging bypass and side-effecting MMIO execute bytes (STC then
CLC). The single NC (`make test-decode-nc`) removes ONLY P3's generation
comparison in a scratch `cpu.c` while P2 translation coherence remains
intact: warmed self-modifying opcode fires the exact
`assert(!(c->rflags & CF))`, scratch RC=134, wrapper RC=1, make RC=2.

### 6.3 Paired wall measurement and identity

Two alternating pre1/post1/pre2/post2 O2 series, seven tapes × three
repetitions, each with identical deterministic instruction counts:

| Tape | Work | Pair 1 P2→P3 (ms; ratio) | Pair 2 P2→P3 (ms; ratio) |
|---|---:|---:|---:|
| boot-haswell | 4,837,677 | 285→216; 1.32× | 264→208; 1.27× |
| boot-broadwell | 4,807,694 | 253→206; 1.23× | 259→213; 1.22× |
| boot-baytrail | 4,807,650 | 264→212; 1.25× | 270→231; 1.17× |
| text-no-fb | 4,837,677 | 269→215; 1.25× | 262→213; 1.23× |
| gfx-fb | 4,837,677 | 266→208; 1.28× | 264→195; 1.35× |
| vbe-800x600x32 | 4,837,677 | 265→197; 1.35× | 266→204; 1.30× |
| smp2 | 4,837,677 | 478→470; 1.02× | 482→471; 1.02× |

SMP benefit is near zero, not claimed as a speedup. On a P3 Haswell
boot, `--stats` reports 4,802,386 decode hits and 5,014 misses, but this
is an internal counter, NOT an instruction-retirement metric. Four 600k
historical boot/VBE-on/VBE-off/no-fb lanes, pre/post with the SAME
`--log`/`--dump-fb` paths: stdout, stderr, ring log and PPM all
byte-identical (RC=0/0); boot PPM SHA256 begins `15c96625cfec5acc`,
no-fb `c3b3d73f6bbb08ab`. No cross-host wall-time threshold in CI.

### 6.4 Phase gates and fresh clone

From parent `e760db0` (P2), `git am` of the P3 patch in a fresh clone
applied cleanly; `HEAD^{tree}` matched the development tree exactly.
The ignored firmware/disk nasm artifacts were seeded from the development
checkout; `make -n` reported both targets up to date (nasm is absent).
In that clone, **with the final MMIO-immediate vector included**:
`make test` RC=0 (P3 vectors, N2/N3 ALL PASS), `make test-sanitize`
RC=0 (P3 vector under ASan+UBSan), `make fuzz FUZZ_PROGRAMS=256` RC=0
(256 programs, 6,144 instructions compared, zero host crashes), `make
bench BENCH_REPS=3` RC=0 (all seven deterministic, uncapped). The one
P3 NC returned **make RC=2** (scratch binary RC=134, script RC=1;
`assert(!(c->rflags & CF))` after the warmed opcode rewrite).
The final patch's docs-only receipt amendment was re-applied in a new
fresh clone and its source tree checked again; the full gate run above
used the identical code and vectors. Frozen AuraLite-OS lanes SKIP
without its toolchain, so no guest speed is claimed.

## 7. P4 — profile gate closed (NO-GO; no implementation patch)

The reproducible primary evidence is in
[`docs/research/PERF_P4_GATE.md`](../research/PERF_P4_GATE.md). On P3,
five O2 `-pg` boot runs gave `cpu_step` 16.67% *inclusive of all instruction
execution*, `perf_decode_hit` 11.31%, `translate` 19.05%; the profile did
NOT isolate switch dispatch as a hot operation. Coverage counted 3,876,786
visits to the second switch per 4,837,677 executed attempts (970,913
`group1`), but counts alone are not time. A scratch-only direct group1
route removed that extra switch; two alternating 3-rep seven-tape series
were mixed (Haswell 202→205 and 208→220 ms; Broadwell 206→238 and
209→220 ms, Baytrail improved, SMP ~unchanged). There is no robust
speedup and no CPU dispatch change. The conditional "swap a dispatch table
entry" NC in the original P4 scope is **not applicable**: no table was
introduced, and neither a false patch nor a fabricated NC is shipped.
P4 is an explicit measured NO-GO, not a delivered optimization.

## 8. P5 — paired confidence + deterministic close-out, one patch

### 8.1 Measured defect and the measurement contract

**D8 (measurement gap):** P0–P3 quoted medians from 3–5 repetitions;
these are useful local measurements but don't show sampling uncertainty.
P5 adds `tests/perf_compare.py` (invoked by `make bench-compare` with two
**explicit O2 binary paths**; it never silently rebuilds or compares an
O1 sanitizer binary with an O2 binary). It runs all seven P0 argv/caps,
nine alternating-order pre/post pairs each by default, times processes
externally with `perf_counter_ns`, and requires exit=0, nonzero attempted
work, no cap, deterministic per-tape repetitions, and equality across builds
of executed steps, idle quantum, BSP `instr_count`, `vtime`, and vCPU count.
A different K6 clock is a **hard failure** even if stdout is otherwise
unchanged. Only after this deterministic gate does it report the geometric
mean of within-pair pre/post elapsed-time ratios and a seeded 10,000-draw
paired-bootstrap 2.5/97.5-percentile **95% descriptive interval** on log
ratios. No CI endpoint is a CI test threshold: nine pairs on one machine
can't guarantee cross-host speed. Prefixes/operand bytes and P3/P4 CPU
code are unchanged by P5.

Nine-pair remeasurement on one host, same `cc -std=c11 -O2 -Wall -Wextra
-Werror -Isrc src/*.c -lm`, shared firmware/disk bytes, P2→P3:

| P0 tape | Work | Geometric mean ratio | Local paired-bootstrap 95% interval |
|---|---:|---:|---:|
| boot-haswell | 4,837,677 | 1.257× | [1.227×, 1.292×] |
| boot-broadwell | 4,807,694 | 1.268× | [1.242×, 1.305×] |
| boot-baytrail | 4,807,650 | 1.237× | [1.174×, 1.280×] |
| text-no-fb | 4,837,677 | 1.279× | [1.231×, 1.365×] |
| gfx-fb | 4,837,677 | 1.252× | [1.213×, 1.293×] |
| vbe-800x600x32 | 4,837,677 | 1.269× | [1.225×, 1.311×] |
| smp2 | 4,837,677 | 1.002× | [0.977×, 1.026×] |

The SMP interval includes one: **no SMP speed claim**. P3→P5 was
recompiled with the same flags: the two x86emu binaries are bit-for-bit
identical (SHA256 `04ebac0ea66a5d58…`). Nine-pair ratios by tape are
1.001, 0.996, 1.014, 0.967, 0.987, 1.014, 1.004; **every** corresponding
interval includes 1. The remaining wall variation is host noise, not P5
speedup or regression. All 7×9 pairs had identical guest work and clocks;
none capped. The actual sample counts and exact ratios are the local
receipt, not portable constant-speed promises.

### 8.2 Regression vectors, defect ledger, and one NC

`make test-perf-compare` runs five deterministic Python vectors: SMP idle
steps (2) distinct from K6 vtime (1026), reject empty/capped runs, reject
missing/duplicate global stats, reject mismatched guest work/clock, and
seeded bootstrap behavior without a timing assertion. The **one P5 NC**
(`make test-perf-compare-nc`) changes only the equality guard in a scratch
copy of the comparator; the SAME clock-mismatch vector fails with
`AssertionError: ValueError not raised` (inner RC=1, wrapper RC=1,
make RC=2). It does not mutate `src/`.

| Defect | Concrete vector / gate (all in this patch series) | Closure |
|---|---|---|
| D0 cap-as-numerator | `make test-perf`, `make bench-nc` | P0 separately counts executed steps; cap NC red. |
| D1 K3 idle inflation | `make test-perf`, P5 `test_paired_idle_not_cap` | 2 attempts vs 1026 vtime pinned. |
| D2 UP n_vcpus=0 | `make test-perf` | BSP sum tested for 0/1/2 slots. |
| D3 no time measurement | `make bench`, P5 `make bench-compare` | medians and local paired intervals; timing not a CI threshold. |
| D4 accidental no-fb everywhere | `make bench` seven distinct argv and P5 history no-fb vs gfx | PPM SHA prefixes differ: `c3b3…` vs `15c9…`. |
| D5 slow 8-byte RAM reads | `make test-perf-mem`, one P1 LE-swap NC | P1 fast path only for entire A20-open RAM span. |
| D6 repeated successful page walks | `make test-tlb`, one P2 stale-PTE NC | RAM/PTE/DMA coherence, exact #PF err/RIP/CR2. |
| D7 repeated group1 prefix re-fetch | `make test-decode`, one P3 stale-opcode NC | P3 RAM-only predecode; dynamic ModRM/imm, MMIO, NX still live. |
| D8 unquantified local timing uncertainty / mismatched work | P5 `make test-perf-compare`, one P5 work-equality NC | Paired CI report only after the guest-work/clock gate. |

P4 did **not** establish a defect in opcode dispatch; it is a no-go study,
not silently added to this table as a fixed defect.

### 8.3 Historical identity and gates

`make test-perf-history PERF_BEFORE=... PERF_AFTER=...` replays nine
available 600k-cap firmware/video/SMP lanes using the SAME `--log` and
`--dump-fb` paths. Pre/post stdout, stderr, log and PPM were **byte-
identical** on all nine (Haswell, VBE on/off, no-fb, Sandybridge,
Ivybridge, Broadwell, Baytrail, SMP2; RC=0/0). N2/N3 wire tests in
`make test` each run x2 with byte-identical receipts (N2/N3 ALL PASS).
Frozen AuraLite-OS guest kernel/initrd and build toolchain are absent,
so guest STORE/USB/VIDEO/NET end-to-end lanes legitimately **SKIP**; the
firmware and protocol fixtures are not misrepresented as a fresh guest
boot. The one P5 NC fails by design, and default emulator stdout/clock
remains unchanged because no runtime source is modified.

### 8.4 Final fresh-clone `git am` receipt

From P3 parent `6d700d2`, the P5 patch applied with `git am` in a
fresh clone, and `HEAD^{tree}` matched the development tree. The ignored
nasm firmware/disk artifacts were seeded from the same development
checkout; `make -n` showed both up to date (nasm unavailable). In that
clone, `make test` **RC=0** (five P5 vectors, N2/N3 ALL PASS; frozen OS
lanes SKIP), `make test-sanitize` **RC=0**, `make fuzz
FUZZ_PROGRAMS=256` **RC=0** (6,144 compared instructions; zero host
crashes), `make bench BENCH_REPS=3` **RC=0** (all seven deterministic),
`make test-perf-history` **RC=0** (nine byte-identical pairs, distinct
no-fb vs gfx), and `make bench-compare` with nine pairs **RC=0**.
A second independent local P2→P5 comparator series yielded UP point
ratios 1.176–1.308× (all local intervals above 1) and SMP 0.994×
[0.951×, 1.036×] (still no SMP speed claim). The single P5 NC gave
scratch Python **RC=1**, wrapper **RC=1**, make **RC=2** on
`test_work_mismatch_rejected: AssertionError: ValueError not raised`.

The final patch's receipt-only documentation amendment was then applied
via `git am` in another fresh clone, checked for identical final tree,
and its P5 vectors re-run. The full-gate fresh clone used the identical
source, scripts and vectors. No CPU, bus or K6 clock changes were made in
P5, and no out-of-host speed guarantee is claimed.

## 9. Standing constraints

1. Do not change `instr_count`/`vtime_instr` semantics: they are the K6
   guest clock and include K3 idle quanta. An optimization that returns
   the same stdout but a different guest clock is a **regression**.
2. `--stats` is opt-in. The default stdout and every historical firmware,
   video, STORE, USB, NET tape must remain byte-identical; e1000 remains
   opt-in with `--tap=`, GPU 0:2:0 `8086:0412`, BAR0 `0xD0000000` and
   `pixel[0]=0x00200000` stay unchanged.
3. `make test` and `make test-sanitize` RC=0 at every phase; `make fuzz`
   paired before/after for CPU paths; deterministic work counts. Wall time
   is a **report** and is never compared to a cross-host constant in CI.
4. One phase = one patch under `patches/` (`git format-patch`); fixes,
   vectors, one NC with RC and assert, CHANGELOG/STATUS/ROADMAP/plan
   updates in that same patch. The patch file does **not** contain itself.
