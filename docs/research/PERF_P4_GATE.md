# PERF P4 — measurement gate: dispatch optimization NOT licensed

Date: 2026-10-12. Base: `6d700d2` (completed P3); the repository's production code is **unchanged**. This is a research report, **not a phase `.patch`**: P4 is not declared complete and did not receive a fictitious speedup. The plan `docs/plans/PERF_PLAN.md` requires stopping if the profile does not confirm the cost of dispatch.

## 1. Profile before any changes

A separate `-O2 -g -pg` binary was built from `src/*.c`, five Haswell boots with a 50-million limit were run (each ended at **4,837,677** attempts), then `gprof -s`/`gprof -b` summed the five `gmon` files. The flat profile holds only **0.84 s** of samples in **0.01 s** ticks: `translate` **19.05% / 0.16 s**, `cpu_step` **16.67% / 0.14 s** of self time, `perf_decode_hit` **11.31% / 0.10 s**, `decode_modrm` **3.57% / 0.03 s**. `cpu_step` includes **all instruction execution**, not just the switch branch: its percentage must not be declared "the cost of dispatch". In the call graph `cpu_step` carries another **0.49 s** of child-call time. The sampling resolution is insufficient to reliably isolate the gain from a single branch.

A separate twenty-pass `-O2 -g -pg` profile with `gprof -l` yielded **3.17 s** of samples: **0.01 s / 0.32%** was attributed to the line of the first `switch(op)`. No line records appeared for the second and third switch as separate measurable hot spots; this does not prove zero cost, but it forbids claiming the bottleneck was measured there. `_init` and line attribution under optimization is noisy, so these data indicate the method's limit, not an absolute estimate of branch cost. `perf` and `valgrind` are absent from the environment.

An O2 `gcov` run on one identical boot: the first `switch(op)` executes **4,837,677** times; its `default` — **3,876,786** times, the second `switch(op)` — **3,876,786** times; `group1` is hit **970,913** times and `really_slow` **2,905,873**. The switch traversal frequency is measured; its *cost in time* is not.

Reproducing the profile: compile `cc -std=c11 -O2 -g -pg -fno-omit-frame-pointer -Wall -Wextra -Werror -Isrc src/*.c -o /tmp/p4-profile/x86emu -lm`, run from the repository root (relative firmware/disk artifacts), save `gmon.out` after each run, aggregate with `gprof -s /tmp/p4-profile/x86emu gmon.1 ... gmon.5` and read `gprof -b /tmp/p4-profile/x86emu gmon.sum`. For coverage: `--coverage -O2` and `gcov -o <binary>-cpu.gcno src/cpu.c` **from the repository root**.

## 2. A non-shipping structural probe

In `/tmp/p4-trial/cpu.c` (outside the repository) **exactly one** hypothesis was tested: route `0x80/0x81/0x83` from the first switch straight into the existing `group1`, and the remaining `default` cases directly into `really_slow`, removing the second switch. ModRM decoding, the slow-case re-fetch, immediates, exceptions and the clock were unchanged. This is a probe, **not an accepted fix**; the full ISA/#PF gate set was not run against it. It was compiled with the same `-O2 -Wall -Wextra -Werror` as the control P3 binary and preserved the attempt counts on the seven P0 tapes.

Two alternating `pre1/post1/pre2/post2` pairs, three repeats per tape, one machine; milliseconds are medians:

| Tape | Pair 1 P3→probe | Pair 2 P3→probe |
|---|---:|---:|
| boot-haswell | 202→205 (0,985×) | 208→220 (0,945×) |
| boot-broadwell | 206→238 (0,866×) | 209→220 (0,950×) |
| boot-baytrail | 240→204 (1,176×) | 220→203 (1,084×) |
| text-no-fb | 212→212 (1,000×) | 212→205 (1,034×) |
| gfx-fb | 207→210 (0,986×) | 211→203 (1,039×) |
| vbe-800x600x32 | 213→207 (1,029×) | 212→202 (1,050×) |
| smp2 | 477→480 (0,994×) | 484→482 (1,004×) |

There is no stable win here: both Haswell and Broadwell pairs got worse, SMP is practically unchanged, the other tapes' results are mixed. The work (`executed`) matched on every tape, but by itself that is not enough to ship dispatch changes.

## 3. Decision

**NO-GO for table/threaded dispatch and for the tested two-level rearrangement**: the hot cost of dispatch itself was not established by the profile, and the narrow control probe showed no stable improvement. The plan's measurement gate fired: the original `src/cpu.c` stays free of P4 changes; a ".patch for the checkbox", an NC of an imaginary table and a speedup claim are **not created**. P4 remains stopped rather than a completed optimization. It only makes sense to return to it with a more precise profiler/controlled workload where the branching itself, not the handler execution, is shown hot. P5 (a separate phase of re-measurement and hardening) can proceed without force-shipping P4.
