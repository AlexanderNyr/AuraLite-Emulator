# TOOLING plan — observe, stop, inspect, persist and watch the machine without perturbing it

> **House style** (`ROADMAP.md`, `CORE_PLAN.md` §2/§5): each phase is one
> `git format-patch` in `patches/`, carrying its implementation, regression
> vectors, exactly one negative control (NC) and the docs sync together.
> `make test` and `make test-sanitize` stay green at every phase boundary.
> Every claim below is measured against the tree, not assumed.

**Status: T0 ✅ (`patches/0056-TOOLING-T0-tooling-plan.patch`); T1 ✅ (`patches/0057-TOOLING-T1-trace-disassembly.patch`); T2 ✅ (`patches/0058-TOOLING-T2-debugger-repl.patch`); T3 ✅ (`patches/0059-TOOLING-T3-gdbstub.patch`); T4 ✅ (`patches/0060-TOOLING-T4-snapshots.patch`); T5 ✅ (`patches/0061-TOOLING-T5-live-web-monitor.patch`).**

| Phase | Deliverable | Gate | Status |
|---|---|---|---|
| T0 | Measured baseline + this plan (docs only) | baseline reproduction commands in §10 | ✅ 0056 |
| T1 | `src/disasm.c` (16/32/64-bit, NASM syntax as printed by `ndisasm`), `--trace-file=` with a built-in CPU-vs-disassembler length/branch-target self-check, `tools/x86dis` | unit vectors; ndisasm differential corpus; 5-platform full-boot trace self-check with 0 mismatches / 0 undecodable; NC | ✅ 0057 |
| T2 | Run-control core + built-in REPL: `--debug`, `--debug-script=`, `--symbols=`; breakpoints, watchpoints, step/next, side-effect-free memory/page-walk inspection | unit vectors; scripted transcript lane; **non-perturbation** byte identity (UP + SMP); NC | ✅ 0058 |
| T3 | gdbstub `--gdb=[host:]port` (GDB Remote Serial Protocol over TCP; vCPUs as threads; `monitor` = REPL) | Python RSP client lanes; real `gdb -batch` lane (SKIP without gdb); non-perturbation; NC | ✅ 0059 |
| T4 | Snapshots `--snapshot-save=` / `--snapshot-load=` and REPL `savevm`/`loadvm` | split-run identity lanes (straight run ≡ save + fresh-process load); round-trip identity; config-mismatch refusal; NC | ✅ 0060 |
| T5 | Live monitor `--monitor=[host:]port`: HTTP + WebSocket server, `web/monitor.html` live mode (registers, framebuffer, log, disassembly, run control, keyboard); plan close-out | codec vectors (RFC 6455 accept key, SHA-1, framing); HTTP/WS client lanes; non-perturbation; NC | ✅ 0061 |

Ordering is dependency-driven: the disassembler (T1) is consumed by the
REPL's `x/i` (T2), by gdb's `monitor` output (T3) and by the monitor's
disassembly panel (T5); the run-control core of T2 is the only stop/resume
machinery the gdbstub, snapshots and monitor are allowed to use.

## 1. Measured baseline (`AuraLite-Emulator@f3470e1`, i.e. after `0055`)

All numbers: O2 `make x86emu`, `--platform=haswell --max-instr=20000000
--no-fb`, the sample firmware + `disk/disk.img`. Wall times are this
machine's and only ever used as ratios.

- **Trace = the log ring.** `--trace` sets `cpu.trace`; `cpu_step` then
  calls `mlog("[trace] %s rip=%08llx op=%02x")` for every instruction.
  A boot is **4,837,677** steps; the ring holds `LOG_RING`=4096 lines, so
  the `--log=` file keeps **4,095** trace lines (0.085 % of the run) and the
  stdout "last log lines" block keeps 60. The boot receipts that
  `make test-boot` greps (`[usb-msc] READ(10)`, `bulk-IN 512 bytes`,
  `synthesized CSW`, `conventional entry reached`, `GUEST_OK`) drop from
  **5 to 0** in stdout: under `--trace`, the test gate itself would fail.
  The complete trace survives only because `mlog` mirrors every line to
  stderr: **4,837,687** stderr lines / **164,481,549** bytes, interleaved
  with device logs.
- **Trace content.** One line = mode + RIP (no CS selector or base) + the
  first opcode byte *after* prefixes were consumed (`[trace] long64
  rip=0010004b op=72`). No instruction length, prefixes, operands,
  immediates, branch targets or vCPU id. The line is logged after the
  prefix fetch, inside the instruction.
- **Trace cost and path change.** Plain boot 208–259 ms (3 runs); `--trace`
  with stderr to `/dev/null` 1,804–1,962 ms (≈8×); stderr to a file
  5,049 ms (≈24×). `cpu.trace` also forces the old slow paths:
  `--stats` shows `decode_hits` **4,802,386 → 0** and `tlb_hits`
  **6,738,320 → 15,863,692** — a traced run does not take the same
  internal path as the run it claims to describe (the guest-visible
  register dump is identical; measured).
- **No stop/inspect facility.** No breakpoint, single-step, watchpoint or
  pause anywhere in `src/`. The existing probes are compile-time or
  hard-coded: `EMU_DBG_U=1` (watch RIPs `0xffffffff801907be`,
  `0xffffffff801907d3`, `0xffffffff80106f85`, user-stack window
  `0x7fffefffa000–0x7fffefffc000`, syscall probe file
  `/tmp/k4-probe.txt`), `--watch-phys=` (logs DW stores to one physical
  address), `--smp-probe` (vcpu1 vital signs). The final `dump_regs`
  prints GPRs, RIP, RFLAGS, CR0/CR3/CR4, EFER, CS and SS only — no
  DS/ES/FS/GS, CR2, GDTR/IDTR/TR, XMM/MXCSR, MSRs or KERNEL_GS_BASE.
- **No remote debugging.** The only socket in `src/` is the e1000 TAP UDP
  backend (`src/e1000.c`); there is no gdbstub.
- **No persistence.** No snapshot/savevm/loadvm. Every look at a late
  event replays from reset: the frozen guest's shell prompt arrives at
  vtime ≈3.0 G, 293 s wall on this host (`verify/auralite_boot.log`).
- **The monitor is a mock.** `web/monitor.html` (21 lines) hard-codes
  "Haswell / Intel profile", `RIP=0x00000000000e38d5` (the real haswell
  boot ends at `RIP=0x000000000010005a`), RAX/RSP/CR0/CR3/EFER, and
  contains **0** occurrences of `WebSocket`, `fetch(`, `XMLHttpRequest`
  or `EventSource`. It can only display a PPM or log file the user loads
  by hand.
- **Run state lives in `main()` locals.** The step-loop counter `n` (the
  thing `--max-instr` budgets) and the `--keys-at` trigger are locals of
  `main()`; the machine-wide device-cadence phase is `static uint32_t
  s_devtick` in `cpu.c`. Nothing outside `main()` can resume a run at the
  same budget point — a hard blocker for T4 and a design constraint for
  T2/T3.
- **Toolchain on this host.** `ndisasm 2.16.03` (ships with nasm, already
  a build dependency), `GNU gdb 16.3`, Python 3.13. gdb is NOT a project
  dependency: gdb lanes must SKIP without it.

## 2. Gap ledger

| ID | Measured gap | Phase |
|---|---|---|
| D0 | `--trace` evicts the whole boot history from the ring (receipts 5 → 0) and keeps 4,095 of 4,837,677 lines | T1: `--trace-file=` never touches the ring or stderr |
| D1 | Trace line has no CS, bytes, length, operands or vCPU | T1: disassembled line `vcpu vtime mode cs:rip bytes text` |
| D2 | `cpu.trace` changes the internal path (P3 predecode off, P2 TLB load ×2.35) | T1: the trace hook reads code side-effect-free, outside the CPU fetch path; `--stats` identical with and without tracing |
| D3 | No breakpoint / step / watch / pause; only hard-coded probes | T2 |
| D4 | No remote debug protocol | T3 |
| D5 | No machine-state persistence; late events need a full replay | T4 |
| D6 | `web/monitor.html` is static; values are fabricated | T5 |
| D7 | Register dump omits segments/descriptor tables/CR2/XMM/MSRs | T2 (`info sregs`, `info cr`, `info xmm`) |
| D8 | Step counter, `--keys-at` trigger and `s_devtick` live outside `machine_t` | T2 moves the run counter into `machine_t`; T4 snapshots all three |

## 3. Standing rules for this plan

1. **Opt-in only.** Without a T flag the default stdout, `--log=`, PPM and
   `--stats` bytes are unchanged; every historical lane stays byte-identical.
2. **Non-perturbation.** A traced, debugged, gdb-driven or monitored run
   that only *inspects* produces the same register dump, log ring,
   framebuffer and `--stats` work counters as the plain run. Gated in
   T1–T5 by byte comparison, not by eye.
3. **Side-effect-free inspection.** Debugger memory reads never call an
   MMIO callback, never fill the P2 TLB or P3 predecode cache and never
   raise a guest fault. The page walk used for inspection mirrors the
   CPU's `translate()` (4-level, PS leaves, same masks) without faults.
4. **One stop/resume mechanism.** All front ends (REPL, gdb, monitor) use
   the T2 run-control core; none of them calls `cpu_step` itself.
5. **No guest-visible change.** No CPU or device behaviour changes in this
   plan. Bugs found while building tools are filed, not silently fixed.

## 4. T1 — trace disassembly

**Deliverable.** `src/disasm.c`/`disasm.h`: a pure function
`dis_decode(bytes, avail, bits, pc, &insn)` → length, text, branch class
and direct target. Syntax = NASM as printed by `ndisasm` (lowercase,
`0x` hex, `byte +0x1` sign-extended imm8, `[rel …]` RIP-relative,
`jmp short`, `jz near`), so the existing nasm toolchain is the oracle.
Coverage: one-byte map, x87, the 0F map with 66/F2/F3 SSE forms, common
0F38/0F3A; VEX/EVEX/3DNow!/XOP are reported as undecoded (`db`).
`tools/x86dis.c` prints an ndisasm-style listing.

**`--trace-file=PATH`** (`--trace-from=VTIME`, `--trace-count=N`): one
line per instruction at the instruction boundary (after interrupt
delivery, before fetch), written by a hook that reads code bytes through
a side-effect-free walk — never the ring, never stderr. Every line feeds a
**self-check**: for a fall-through instruction with no intervening
exception/interrupt/reset on that vCPU, the next RIP must equal
`rip + len`; for a direct branch it must be the fall-through or the
decoded target. Mismatches and undecodable bytes are counted and written
as `!` lines; a summary goes to stderr at exit. Legacy `--trace` keeps its
exact old behaviour.

**Gate.** `make test-disasm` (unit vectors, sanitizer lane too);
`make test-disasm-diff` (generated corpus vs `ndisasm`: 100 % length
agreement and text agreement outside a short, reasoned allowlist; SKIP
without ndisasm); `make test-trace` (5 platforms, full boot, 0 mismatches,
0 undecodable, `--stats` identical to the untraced run, ring receipts
intact). **NC:** a scratch `disasm.c` with one seeded length defect turns
the unit, differential and trace gates red.

### 4.1 Delivered (measured)

**Code.** `src/disasm.c` (table-driven; operand-spec strings in SDM
appendix-A letters), `src/dbg.c` (the tooling core: side-effect-free
`dbg_translate`/`dbg_read_lin`/`dbg_read_phys`, the trace writer),
`tools/x86dis.c` (`make x86dis`). CPU contact is three lines: a
`cpu_dbg_boundary` hook pointer called after interrupt delivery and before
the fetch only while `m->dbg != NULL`, and a tooling-only
`cpu_t.event_count` bumped by every `raise_exception` (no instruction reads
it). The trace hook never calls the CPU's fetch path, so `--trace-file`
leaves P2/P3 untouched (D2) and never writes the log ring (D0); legacy
`--trace` is unchanged byte for byte.

**Differential** (`make test-disasm-diff`, 3.1 s): 226,884 generated
encodings (62,922 / 62,922 / 101,040 in 16/32/64-bit: every one-byte, 0F,
0F38, 0F3A opcode × 8–19 legacy/REX prefix sets × ModRM/SIB/disp/RIP-rel
shapes). **153,538 exact matches, 0 diffs, 0 ours-only, 0 desyncs**;
60,640 rejected by both; 4,278 "scope" (ndisasm decodes, T1 deliberately
emits `db`: Cyrix EMMI, SHA, GFNI, CET/MPX register forms, SSE4a, VMX
INVEPT/INVVPID, `jmpe`, `segr6/7`, 3DNow!, VEX/EVEX/XOP); 8,428 accepted
under 13 written reasons, each a place where ndisasm and the emulator's
CPU or the SDM legitimately differ — e.g. SETcc ignores ModRM.reg (4,096),
shift `/6` and F6/F7 `/1` aliases the CPU executes (278), `63 /r` without
REX.W (126), REX.W PUSHF/POPF (100), 66-prefixed near branches in 64-bit
decoded rel16 like cpu.c (AMD) where ndisasm is Intel (360), 9B executed
alone (383), MPX/CLDEMOTE/CET names for the 0F 18-1F hint-NOP block the
CPU runs as NOPs (391), NASM's sized-vs-unsized SSE templates (1,282),
`o64` on REX.W SSE forms (1,302), Cyrix EMMI decodes and one ndisasm
16-bit SSE operand bug (110). The prefix-printing rules (`o16`, `a32`,
`bnd`, `xacquire`) follow ndisasm's own `a_used`/`o_used` logic (read in
`disasm/disasm.c` of nasm-2.16.03), not guesswork.

**Unit** (`make test-disasm`, also in `make test-sanitize`): 132
ndisasm-frozen vectors, 6 CPU-consistency vectors (the allowlist reasons
pinned to cpu.c behaviour), 17 successor-class/target vectors, rejects
(VEX, 3DNow!, `82`, truncation, >15 bytes, empty), inspection vectors (4K/
2M/1G leaves; not-present PTE/PML4E; an MMIO page reads 0 bytes with the
callback count still 0; P2/P3 counters unchanged by inspection; open bus
0xFF; both ROM windows; A20 wrap; RAM-only writes bump the epoch) and the
hook on a 10-instruction real-mode program (8 checked successors). Clean
under ASan+UBSan, gcc and clang at -O0..-O3/-Os.

**Trace gate** (`make test-trace`, 8 lanes, 86 s with the guest lanes):

| Lane | lines (= executed) | fall checked | branch checked | mismatches / undecodable |
|---|---|---|---|---|
| fw sandybridge | 4,807,675 | 4,326,748 | 480,922 | 0 / 0 |
| fw ivybridge | 4,807,679 | 4,326,751 | 480,923 | 0 / 0 |
| fw haswell | 4,837,677 | 4,346,745 | 490,927 | 0 / 0 |
| fw broadwell | 4,807,694 | 4,326,761 | 480,928 | 0 / 0 |
| fw baytrail | 4,807,650 | 4,326,720 | 480,925 | 0 / 0 |
| K1 fixture | 46 | 35 | 10 | 0 / 0 |
| guest UP, 40M steps | 40,000,000 | 31,636,635 | 8,342,132 | 0 / 0 |
| guest `--smp=2`, 40M steps | 29,740,936 | 22,728,908 | 6,990,250 | 0 / 0 |

Every lane's stdout — register dump, `--stats` incl. `tlb_*`/`decode_*`,
last log lines — is byte-identical with and without the trace, and the
traced haswell stdout keeps the `test-boot` receipts (T0: 5 → 0 under
`--trace`). Manual guest receipts: a 12M-line window opened at vtime
930M around the shell prompt with a typed `ls` (9,538 ring-3 lines at
`CS=0023`, `syscall`/`o64 sysret`, 1,261 `iretq`) and a 200M-line window
from vtime 2.9G: 0 mismatches, 0 undecodable in both. Cost: a traced
firmware boot takes ≈4.1 s against ≈0.21 s plain (it decodes and formats
every instruction, ≈77 bytes/line). Untraced cost of the hook: `make
bench-compare` (P5 paired comparator, 9 reps) against the `f3470e1`
binary gave ratios 0.976×–1.013× on all seven tapes, every 95 % CI
containing 1.0 — below the wall bench's resolution, but **not free**:
T2 measured +1.78 % host instructions with cachegrind and fixed it
(§5.1).

**Historical identity.** Ten lanes against the `f3470e1` binary — five
firmware platforms, `--no-fb`, `--vbe=800x600x32`, `--smp=2`, guest UP and
`--smp=2 --cpus=2` (40M steps) — are byte-identical in stdout incl.
`--stats`, the PPM, `--dump-text` and the full `--log=` file. `make test`
green (5m40s; was 5m15s), `make test-sanitize` green (2m06s).

**NC** (`make test-disasm-nc`): a scratch `disasm.c` decodes the group-83
immediate as imm16/32. Unit RC=1 (`FAIL A b32 83c0ff`), differential RC=1
(`diff=388`), trace self-check `mismatches=490035` on the haswell boot
(every `83`/`48 83` successor), wrapper RC=1, make RC=2.

## 5. T2 — run-control core + REPL

**Deliverable.** `src/dbg.c`/`dbg.h`. Hooks: one boundary hook in
`cpu_step` (after interrupt delivery, before fetch: breakpoints on the
*linear* PC `CS.base+RIP`, step completion, vtime targets) and one access
hook in the CPU's linear read/write helpers (watchpoints), both behind a
single `m->dbg` test so the undebugged path pays one predictable branch.
A stop enters the front end synchronously; the guest is frozen in wall
time only, so virtual time is untouched. Resuming from a breakpoint skips
that breakpoint exactly once for that vCPU. `--debug` (stdin),
`--debug-script=FILE` (commands from a file; EOF = detach),
`--symbols=ELF` (auto-loaded from `--kernel=`). Commands: `break/tbreak/
delete/info break`, `watch [r|w|a] ADDR [LEN]`, `continue`, `stepi [N]`,
`nexti [N]`, `until VTIME`, `regs/sregs/cr/xmm`, `x/FMT ADDR`, `x/Ni`,
`print EXPR`, `set $reg = EXPR`, `xlate ADDR`, `vcpu N`, `log [N]`,
`trace on FILE|off`, `detach`, `quit`. Transcript → stderr.

**Gate.** `make test-dbg` (unit: exact stop RIPs and step counts,
watchpoint on a known store, expression parser, `x/i`, side-effect-free
reads: MMIO callback count and TLB/predecode counters unchanged);
scripted firmware session with transcript assertions; **non-perturbation**:
breakpoints + steps + continue to the end ≡ plain run byte-for-byte
(stdout incl. `--stats`, `--log=`, PPM), UP and `--smp=2`. **NC:** remove
the skip-once rule → `continue` from a breakpoint makes no progress →
red.

### 5.1 Delivered (measured)

**Code.** `src/dbg.c` grows the run-control core and the REPL; `src/cpu.c`
gains one more hook, `cpu_dbg_access`, called by the linear read/write
helpers and installed only while a watchpoint exists; `machine_t.run_steps`
replaces `main()`'s step counter (D8, first half). Front ends register a
`dbg_frontend_t` (announce stop / pollable fd / service / poll while
running / detach); the stop loop `poll()`s all of them, so the gdbstub and
the monitor plug into the same loop. Breakpoints match the linear PC
`CS.base+RIP` and never patch guest memory. Debugger reads cover RAM, both
ROM windows, open bus and the RAM-backed MMIO windows now flagged
`dbg_readable` (framebuffer/CAR/DDR scratch, `make_ramwindow`); device
registers are never read. Watchpoints stop after the accessing instruction
completes (after the whole REP for string ops). `--debug` stops before the
first instruction with the REPL on stdin (Ctrl-C pauses), `--debug-script`
reads a file (EOF detaches), symbols come from `--symbols=` or the
`--kernel` ELF; the transcript goes to stderr so stdout stays the run's.

**Two stop sites.** A breakpoint/step/until stop happens *inside*
`cpu_step` at the boundary, so resuming executes that instruction; a
watchpoint/pause/start stop happens *between* steps, so the next boundary
is still in front of the instruction the user sees. Resuming from a
between-instruction stop therefore (a) skips the breakpoint it stands on
once and (b) does not count that boundary as a step. The first NC draft
assumed (a) mattered for breakpoint stops; it did not fail — the measured
behaviour is what the vectors now pin.

**Unit** (`make test-dbg`, sanitizer lane too): a scripted front end on a
37-instruction real-mode program records 10 exact stops — start, break
0x0a at steps 6 and 13, `stepi 3` → 16, `nexti` over a call → 20 (callee
ran), write watch on `[0x200]` → rip 0x07 at 23 with old=3 new=2,
`stepi 1` from that between-instruction stop → exactly the call (24),
read watch → 26, `until +3` → 29, end at 37 — and the final registers and
count equal the untooled run. A breakpoint on every one of the nine
instruction addresses stops 36 times and still leaves the run identical;
`quit` abandons the step (6 executed, RIP 0x0a); a pause request stops at
the next step; expressions (`(1+2)*3`, `$rcx+$rsp-8`, `*$rsp`) and
sub-register writes (`ax` keeps the upper bits, `eax` zero-extends).

**Lanes** (`make test-debug`, 25 checks): firmware transcript — start at
`f000:fff0`, `break $lin` + continue progresses, breakpoint at the guest
entry 0x100000 at vtime 35,254, `stepi 3` → 35,257, `xlate` → physical
0x100007, run ends halted at 4,837,677 like the plain run; a write
watchpoint on framebuffer pixel 0 stops after `mov [rdi],eax` at vtime
35,283 with old=0x0 new=0x200000 (the marker); five platforms with
break/continue/`stepi 1000`/delete/continue keep stdout incl. `--stats`
byte-identical to the plain run; pipe-driven `--debug` prompts, stops and
exits 0 on `quit`; frozen guest `--smp=2 --cpus=2`: 2,388 ELF symbols,
`break kmain` stops at vtime 18 on `push rbp`, backtrace, vCPU table
(`vcpu1 wait-sipi`), stdout identical to the plain run. Manual receipt:
Ctrl-C on a running guest stopped at vtime 29,446,197 in
`fb_putchar+0x2d1` with the frame-pointer chain `fb_putchar ← kprintf ←
kmain ← _start.hang`. All lanes (guest included) clean under
ASan+UBSan+LSan.

**Cost of the hooks — measured deterministically, and a T1 correction.**
The paired wall bench cannot resolve ~1–2 % on this host (the same two
binaries moved one tape from 1.078× to 0.947× between runs), so T2 counts
host instructions with cachegrind (`valgrind --tool=cachegrind
--cache-sim=no`, haswell boot, 4,837,677 guest instructions):

| Build | host Ir | Δ vs `f3470e1` | `cpu_step` | `main` | `rm_read` + `write_mem_v` |
|---|---|---|---|---|---|
| `f3470e1` | 3,476,309,458 | — | 809,441,498 | 121,013,178 | 72,954,630 |
| T1 as shipped (0057) | 3,538,237,612 | +1.78 % | 871,342,557 | 121,013,189 | 72,954,630 |
| T2, hooks inline | 3,558,593,735 | +2.37 % | 871,352,557 | 135,526,238 | 78,786,306 |
| **T2 (this patch)** | **3,467,211,197** | **−0.26 %** | 823,509,049 | 91,987,156 | 78,786,306 |

So T1's "not measurable" (§4.1) was a statement about the wall bench's
resolution, not about the cost: its inline boundary call made GCC spill
around it, +12.8 host instructions per guest instruction. T2 fixes it:
the boundary and watch calls are `cold` out-of-line helpers behind one
`__builtin_expect` test (≈2.9 instructions per step remain in
`cpu_step`); `read_mem_v` carries an `inline` hint because the hook's
bytes had pushed it over GCC's auto-inline threshold (rm_read/do_pop/
read_descriptor/raise_exception started calling it); and the UP run loop
is specialised — `run_up_step()` inlined twice with `tooled` constant —
so the copy without a tool has no tool test and came out tighter than
the original loop. Eleven historical lanes (five firmware platforms,
`--no-fb`, `--vbe`, `--smp=2`, guest UP/SMP/`--keys-at`) stay
byte-identical to `f3470e1`. `make test` green (5m48s), `make
test-sanitize` green (2m15s, now incl. `test-dbg`).

**NC** (`make test-dbg-nc`): a scratch `dbg.c` that never arms skip-once.
Unit RC=1 (`breakpoint stops 37, want 36`); the real lane script on the
scratch binary RC=1 with one spurious stop at vtime 0 eating lane A's
`continue` (5 failed checks); wrapper RC=1, make RC=2.

## 6. T3 — gdbstub

**Deliverable.** `src/gdbstub.c`: RSP over TCP (`--gdb=[host:]port`,
default host 127.0.0.1), checksums/acks and `QStartNoAckMode`,
`qSupported`, `qXfer:features:read:target.xml` (i386:x86-64 core + SSE +
`fs_base/gs_base` + a system feature with CR0/CR2/CR3/CR4/EFER),
`g/G/p/P`, `m/M/X` (linear, side-effect-free; RAM-only writes flush the
P2/P3 caches), `c/s/vCont`, `Z0/Z1` (no guest memory patching),
`Z2/Z3/Z4`, threads = vCPUs (`qfThreadInfo`, `Hg/Hc`, `T`, `qC`),
`qRcmd` → the T2 command interpreter, Ctrl-C (0x03) while running,
`k`/`D`, `W`/`X` on run end.

**Gate.** `tests/test_gdbstub.py` (pure-socket client: reset registers,
breakpoint at the guest entry 0x100000, memory equals the loaded test
kernel, stepi lengths, watchpoint, monitor command, Ctrl-C);
`gdb -batch` lane (SKIP without gdb); non-perturbation (gdb-driven run to
completion ≡ plain run). **NC:** disable the running-state Ctrl-C poll →
the interrupt vector times out → red.

### 6.1 Delivered (measured)

**Code.** `src/gdbstub.c` (≈560 lines) is a T2 front end and nothing more:
it blocks in `accept()` before the first instruction, then every stop and
resume goes through the run-control core. The running-state poll reads
the socket every 64 Ki steps (the core calls it every 1 Ki) for 0x03 or a
disconnect; while stopped, the core `poll()`s the socket. Packets:
`qSupported` (PacketSize=0x4000, qXfer:features:read+, QStartNoAckMode+,
swbreak+, hwbreak+, vContSupported+), `qXfer:features:read:target.xml`
(5,393 bytes: `i386:x86-64` core, SSE, segments with fs_base/gs_base,
and an `org.auralite.x86emu.sys` feature with CR0/CR2/CR3/CR4/EFER),
`?`, `g/G/p/P` (selectors, x87 and CRs read-only — a selector write
without a descriptor load would build a state the CPU cannot reach),
`m/M/X` (side-effect-free linear reads, RAM-only writes), `c/s/vCont`
(step one thread, the others run — gdb's scheduler-locking off),
`Z0/Z1/z0/z1` (virtual), `Z2/Z3/Z4` (the T2 watchpoints), `H`, `T`,
`qfThreadInfo` (vCPUs = threads), `qThreadExtraInfo` ("vcpu0 long64"),
`qC`, `qAttached`, `qOffsets`, `qSymbol`, `qRcmd` (the T2 interpreter;
output as `O` packets), `k`, `vKill`, `D`. Stop replies: `T05…swbreak:`,
`T05watch:/rwatch:`, `T02` for Ctrl-C, `W00`/`X04` at the end of the run.

**Gate** (`make test-gdb`, 45 checks). Lane P, `tests/test_gdbstub.py`,
pure sockets (30 checks): NoAck negotiation, target.xml, reset state
`rip=0xfff0 cs=0xf000`, `Z0` at 0x100000 → `T05thread:1;swbreak:;`,
memory at 0x100000 equals `disk/test_kernel.bin`, `s` over `cli` → rip
0x100001, `Z2` on framebuffer pixel 0 → `T05watch:d0000000` and the pixel
reads 0x00200000, `monitor info vcpu`, run to the end → `W00`, exit 0,
stdout incl. `--stats` identical to the plain run; an endless `jmp $`
ROM interrupted by 0x03 → `T02` in 2 ms, `k` exits 0; a bad checksum is
NAKed, unknown packets get the empty reply, open bus reads 0xff and the
AHCI ABAR is never read. Lane G, real `gdb -batch` (GNU gdb 16.3):
`Breakpoint 1, 0x0000000000100000`, gdb's own disassembly `=> 0x100000:
cli`, `stepi` → 0x100001, `Hardware watchpoint 2: *(int*)0xd0000000 …
New value = 2097152`, `cr0 0xe0000011` from target.xml. Lane K, real gdb
on the frozen guest with `--smp=2 --cpus=2` and gdb loading the kernel
ELF: `Thread 1 hit Breakpoint 1, kmain (boot_info=0x20000) at
kernel/kernel.c:196`, source-level `bt`, `info threads` shows `Thread 1
(vcpu0 long64)` and `Thread 2 (vcpu1 real16)`, `x/2i` symbolized
(`<kmain+13>: call … <boot_info_init>`), `detach` → the run completes with
stdout identical to the plain run. All lanes clean under ASan+UBSan+LSan;
untooled cost unchanged (cachegrind: 3,467,211,412 host instructions,
+215 vs T2, all at startup). `make test` green (5m54s), `make
test-sanitize` green (2m16s); historical lanes byte-identical.

**NC** (`make test-gdbstub-nc`): a scratch `gdbstub.c` whose running-state
poll returns at once. Lane B reports no stop reply within 5 s (5,019 ms)
while lanes A and C stay green (27 ok); client RC=1, wrapper RC=1, make
RC=2.

## 7. T4 — snapshots

**Deliverable.** `src/snapshot.c`: versioned chunked file
(`AURASNP1`, build-layout fingerprint, machine-shape fingerprint), sparse
RAM pages, every device's state through per-module visitors that preserve
host pointers, the MMIO list incl. runtime ECAM registrations, PCI config
spaces, disk/SATA image contents, the log ring, the P2/P3 caches (so
`--stats` stays identical), `s_devtick` and the run-step counter. Harness
actions (`--keys`, `--keys-at`, `--vbe`) are never stored: they belong to
the invoking process. Refuses `--tap` and `--sata-writethrough` (host
state outside the machine) and any shape/layout mismatch with a clear
message.

**Gate.** split-run identity: straight run to N2 ≡ (run to N1 + save) →
(fresh process, load, run to N2) for stdout incl. `--stats`, `--log=`,
PPM and `--dump-text`, on the firmware lanes (5 platforms, several split
points), VBE, SMP, the K1 fixture; save → load → save byte identity;
refusal vectors. **NC:** drop one device's state from the file → split
run diverges → red.

### 7.1 Delivered (measured)

**Code.** `src/snapshot.c` writes `AURASNP1` + version + `sizeof(machine_t)`/
`sizeof(cpu_t)`, then tagged, length-checked records in one fixed order
that a single visitor walks for both directions: the machine shape
(platform, ROM hash, `kmain` address, vCPUs, `--cpus/--smp/--no-fb/
--no-usb-uhci`, disk and SATA sizes, PCI and RAM-window counts),
`machine_t` minus the 2 MiB I/O dispatch table (CPU, vCPU slots, LAPICs,
PIC/PIT/RTC/KBC/chipset/IOAPIC, the P2/P3 caches with their epoch, the log
ring, vtime, `run_steps`), the cpu.c statics, RAM (4 KiB page bitmap +
non-zero pages), PCI config spaces (tags carry bus:dev.fn), AHCI HBA state
(including the live BAR5 base), the MMIO list (runtime ECAM windows replayed
oldest first via `pci_ecam_map`, then an exact match and the `enabled`
flags), every device global of devices.c (EHCI, USB/BOT, UHCI, VGA, dispi,
COM1, the RAM-window buffers), the USB stick and SATA images. Modules reach the visitor
through function pointers in `snap_t`, so the narrow unit harnesses link
nothing new (the first draft broke `make test-unit`'s link — caught,
fixed). Host pointers are zeroed in the file and kept live on load, so
two processes saving the same state write the same bytes. `--tap` and
`--sata-writethrough` are refused (host state outside the machine); a
foreign file, another format version, another build, a truncation, a
trailing byte, another platform/ROM/kernel/vCPU count are refused with
the reason and exit code 1. Harness actions are not machine state: a
`--keys-at` trigger already passed at the save point fires again in the
loading process.

**Two measured catches.** The first split run differed only in `--stats`
(`tlb_hits` +1, `decode_hits` −1): cpu.c resolves `EMU_DBG_U` lazily at
the first fetch and bypasses P3 while unresolved, so a restored process
took the slow path once. That one-time flag is internal path state and is
now carried (`cpu.statics`). And `s_devtick` (the device-cadence phase) is
process-global — carried too, which the unit suite needs explicitly
because several machines share one process.

**Version 2 restore-order correction (2026-10-16).** The full emulator suite
initially exposed an AHCI snapshot-load refusal: the guest had relocated BAR5
to `0xfe044000`, but `mmio_rec` compared it against the fresh-machine window
at `0xfeb10000` before PCI config and AHCI state had been restored. Snapshot
format version 2 now loads PCI config, then `ahci_snap` rebinds the live BAR,
then validates the MMIO map. Older v1 files are refused rather than parsed
with the new record order. `make test-snapshot` now passes all 12 available
split lanes, determinism and save/load/save identity; frozen-guest lanes skip
when their separate KERNEL/INITRD fixtures are absent. Full `make test` and
`make test-sanitize` pass. This repair is separate from COMPAT G3.

**REPL.** `savevm FILE`/`loadvm FILE` (one vCPU): at a between-instruction
stop they run at once; at a boundary stop — inside `cpu_step`, interrupt
delivered, fetch pending — no consistent snapshot exists, so the op is
deferred until the current instruction completes and the debugger stops
again right after it (`savevm deferred …`). gdb reaches them through
`monitor savevm/loadvm`.

**Gate** (`make test-snapshot`). Fourteen split lanes, each straight run
≡ (save at N1 → fresh process → load → N2) in stdout incl. `--stats`, the
PPM, `--dump-text` and the full `--log`: five platforms split at step 2M,
haswell at 1,000 / 35,254 (the guest entry) / 4,837,000, `--no-fb`,
`--vbe=800x600x32`, `--smp=2`, the K1 fixture, and the frozen guest UP
and `--smp=2 --cpus=2` split at 20M of 40M (snapshots 0.76–5.55 MB). Two
processes → identical files; save → load → save byte-identical; six
refusals; REPL `savevm` deferred to vtime 35,255, `stepi 5000`, `loadvm`
→ back at 35,255, and the run ends byte-identical to the plain run.
`make test-snap` (sanitizer lane too): split identity, determinism,
sparse RAM (751,300-byte file for a 128 MiB machine), ten refusal
vectors. Untooled cost unchanged (cachegrind 3,467,211,738 host
instructions, −0.26 % vs `f3470e1`). `make test` green (6m14s),
`make test-sanitize` green (2m29s, now incl. `test-snap`); historical
lanes byte-identical.

**Manual receipts (frozen guest).** Long horizon: saved at step 100M
(vtime 177,248,381, before the shell prompt), loaded, run to step 1.2G
(vtime 401,896,658,068) with `--keys-at=prompt --keys=ls`: the keys fire
at the same vtime 938,217,927 as in the straight run, `ls` runs in ring 3,
and stdout incl. `--stats` (415,857,812 executed), the text console and
the full log are byte-identical. A first attempt saved at step 600M —
already past the prompt, because idle steps advance vtime by 512 — and
diverged exactly as documented: the passed `--keys-at` trigger fired a
second time in the loading process. Workflow: booting to the idle shell
and saving takes 16.1 s; loading that 3.27 MB snapshot and typing `ls`
prints `apps/ bin/` and the next prompt 341 ms later.

**NC** (`make test-snapshot-nc`): a scratch `devices.c` that forgets the
RAM windows. Nine lanes fail (`fw-haswell-mid … differs in: out ppm
log` — the framebuffer marker receipt disappears) while the control split
at step 1,000, before any framebuffer write, stays green; make RC=2.

## 8. T5 — live monitor + close-out

**Deliverable.** `src/ws.c`/`ws.h` provide dependency-free SHA-1,
RFC 6455 accept-key and frame helpers. `src/monitor.c`/`monitor.h` attach a
single-threaded HTTP/1.1 + WebSocket frontend to the T2 debugger poll loop;
network I/O is non-blocking while the guest runs, and all state inspection
uses the T2 side-effect-free APIs. `--monitor=[IPv4:]port` defaults to
127.0.0.1 and stops at the reset vector. JSON state frames carry platform,
mode, vtime, executed attempts, registers, vCPU state, logs, stop reason,
and disassembly. Binary framebuffer messages carry `FB`, version, keyframe
flag, little-endian dimensions/row count, then changed rows as y + RGB.
Commands: `pause/continue/step/break/exec/keys/type/quit`. When GDB is
attached, monitor run control is read-only. `web/monitor.html` adds the live
view and retains offline PPM/log loading. The endpoint has no authentication;
non-loopback binding is an explicit user choice.

**Gate.** `make test-monitor-codec`: RFC 6455 sample accept key, FIPS 180
SHA-1 vectors, masked and unmasked short/16-bit/64-bit payloads, and
malformed-header/control-frame rejection. `make test-monitor`: HTTP GET,
101 handshake, initial state and 800×600 framebuffer keyframe, 16-/64-bit
server payload lengths, run control, read-only REPL, framebuffer marker,
Set-1 and ASCII keyboard injection, GDB ownership, async pause/step/quit,
masked short/16-/64-bit client frames, unmasked-frame rejection, and
non-perturbation (stdout incl. `--stats`, PPM and log byte-identical).
Both targets are part of `make test` and `make test-sanitize`. **NC:** a
scratch `ws.c` with the wrong accept GUID; HTTP and 101 stay green while the
client's sample-key assertion alone fails.

### 8.1 Delivered (measured)

**Code and wire.** `--monitor=8080` serves `web/monitor.html` at
`http://127.0.0.1:8080/`, then upgrades `/ws`. The frontend is polled by the
T2 stop/running loop; it never creates a CPU thread. Running polls occur at
the T2 cadence (one callback per 1024 instruction attempts) and publish
state/framebuffer every 64 callbacks. The first 800×600 RGB keyframe is
1,441,210 payload bytes, so the output limits include legal RFC 64-bit
frame lengths. Later frames carry only changed rows. State JSON includes the
selected CPU's GPR/control-register dump, platform/mode, stop and execution
counters, per-vCPU RIP/state, `dbg_disas_at()` output, framebuffer size and
new log-ring entries. Commands share the T2 run-control and inspection
paths; keyboard input uses the KBC's Set-1 queue and `type` translates US
ASCII make/break codes. The monitor refuses run-control commands while a
GDB frontend is registered.

**Measured gates.** `make test-monitor-codec` passes all 19 checks.
`make test-monitor` passes 54 checks across five lanes: HTTP/handshake,
firmware control + marker, async pause/step/quit, GDB read-only ownership,
and frame lengths/protocol hygiene. The full 800×600 keyframe uses a 64-bit
WebSocket payload length; the monitor state and control replies are parsed
at short/16-bit lengths. A 2026-10-16 regression run passes the corrected
boot lane (26 checks) and framing lane (8 checks): the short case pads a valid
JSON command below 126 bytes and asserts the short-encoding marker `0`; the
boot lane compares monitored stdout, log and framebuffer bytes with the plain
run, using the same framebuffer path and preserving the plain PPM before
monitor overwrite. The firmware lane sets a breakpoint at the guest entry,
single-steps `cli`, continues to the 600,000-attempt limit, sees the
`0x00200000` marker pixel, injects keyboard input, and compares stdout
(incl. `--stats`), the complete log ring and PPM to a plain run byte for byte.
`make test` passes (355 s) and `make test-sanitize` passes (138 s); existing
frozen-guest lanes skip because KERNEL/INITRD are not supplied in this
workspace. `make test-monitor-nc` fails as designed: after corrupting only
the RFC GUID, HTTP and 101 pass, and the client accept-key check is the only
red assertion. Historical suite lanes remain green.

**NC** (`make test-monitor-nc`): wrong RFC 6455 accept GUID in a scratch
copy of `ws.c`; inner client RC=1 on the exact sample accept-key assertion,
HTTP GET and 101 upgrade pass, wrapper RC=1 as designed.

## 9. Non-goals

- Reverse execution / record-replay, and snapshots across builds (the
  layout fingerprint refuses them).
- Cycle accuracy: all stops are at instruction boundaries.
- Fixing CPU/device defects found by the tools (filed in the analysis,
  e.g. F1–F25), and the TAP backend's host state.
- Authentication: every server binds 127.0.0.1 by default; binding
  elsewhere is the user's explicit choice.

## 10. Reproduce the T0 baseline

```sh
make x86emu firmware/firmware.bin disk/disk.img
./x86emu --platform=haswell --max-instr=20000000 --no-fb --stats >b.out 2>b.err
./x86emu --platform=haswell --max-instr=20000000 --no-fb --stats --trace --log=t.log >t.out 2>t.err
grep -c 'GUEST_OK\|usb-msc\|conventional entry' b.out t.out   # 5 vs 0
wc -l t.log; grep -c '^\[trace\]' t.err                         # 4096; 4837677
grep -E 'decode_hits|tlb_hits' b.out t.out
grep -c -i 'websocket\|fetch(\|XMLHttpRequest\|EventSource' web/monitor.html   # 0
grep -l 'socket(' src/*.c                                        # src/e1000.c
```
