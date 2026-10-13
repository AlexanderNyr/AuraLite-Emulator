# Changelog

All notable changes to AuraLite-Emulator are recorded here. The recent
`patches/` series (NET N0 onward) is based on the actual GitHub `main`
commit `e79b7be`, not the older historical `33e2c5c` milestone. Each
patch has its own immediate parent; entries name measured defects and gates.

## [Unreleased]

### Changed — patch 0055 (PERF P5, paired confidence + close-out)

- P4's required profile gate was **NO-GO**, not a delivered dispatch
  optimization: O2 gprof did not isolate switch cost, and the scratch
  two-switch collapse had mixed paired results (Haswell/Broadwell both
  slower in two series). Evidence in `docs/research/PERF_P4_GATE.md`; no
  CPU change and no fictional dispatch-table NC.
- New `make bench-compare PERF_BEFORE=... PERF_AFTER=...` reports seeded
  paired-bootstrap 95% intervals after checking every seven-tape pair
  for identical executed steps, idle, BSP/K6 clock, vCPU count, and
  uncapped deterministic work. Nine-pair local P2→P3 remeasurement:
  six UP lanes 1.237–1.279× point estimates; SMP 1.002× with interval
  [0.977,1.026], so no SMP speed claim. No wall-time CI thresholds.
- P5 adds five deterministic comparator vectors and one NC: scratch
  equality-guard removal causes the SAME virtual-clock mismatch vector
  to fail `AssertionError: ValueError not raised` (binary RC=1, wrapper
  RC=1, make RC=2). P3→P5 binaries are bit-identical; nine available
  historical firmware/video/SMP stdout/stderr/log/PPM pairs are byte-
  identical. Defect ledger D0–D8 cross-references concrete vectors.
  Frozen AuraLite-OS end-to-end lanes SKIP without its toolchain.

### Changed — patch 0054 (PERF P3, RAM-only prefix/opcode predecode)

- Measured post-P2 `decode_modrm` 8% and `fetch8` 6% of sampled CPU time;
  `group1` took 970,913 redundant prefix/opcode re-fetch paths in one
  4,837,677-attempt boot. A 512-slot machine-level prefix/opcode-only cache
  now avoids those fetches on a successful same-page plain-RAM EXEC walk;
  P2's RAM-write epoch and full paging/mode key provide coherence. Live
  ModRM/SIB/imm, ROM/MMIO, cross-page and trace/debug stay on original
  paths. `--stats` adds decode hits/misses without changing default output.
- `test-decode` exercises opcode SMC, PTE remap, dynamic SIB/imm, boundary,
  mode, trace, NX #PF with exact err/CR2/RIP, A20, nonpaging, MMIO
  execute and a live MMIO immediate under a cached RAM opcode;
  one scratch NC disables only the P3 epoch comparison: inner RC=134,
  wrapper RC=1, make RC=2 at `assert(!(c->rflags & CF))`. The ring3
  test's post-start raw instruction injector now explicitly flushes the
  shared epoch after each host-side memcpy (as required for direct RAM
  writes outside `mem_write`); its guest asserts remain unchanged.
- Paired three-rep local P2→P3 medians: UP tapes 1.17–1.35× across two
  series; SMP2 only ~1.02×, no meaningful claim. Work counts unchanged;
  four historical stdout/stderr/log/PPM tapes byte-identical with the same
  paths. Frozen guest toolchain absent (SKIP); no guest speed claim.

### Changed — patch 0053 (PERF P2, coherent success-only TLB)

- **Measured D6:** after P1, five O2 gprof boots gave `translate`
  79,798,615 calls, **22.10%** of sampled CPU time (0.40 / 1.81 s).
  A 256-slot, 4KiB-subpage successful-translation TLB now lives in
  `machine_t` (NOT in the K6 vCPU copies). Key: VA page, full CR3/CR0/
  CR4/EFER, RFLAGS.AC, CPL, descriptor-supervisor override and access
  mode. Only a *successful* old walk through ordinary RAM-backed PTEs
  with A20 open is cached; faults, dynamic MMIO/ROM tables, disabled A20
  and all nonpaging paths still take the old walk. 2MiB/1GiB PS leaves
  cache correct per-4KiB subpage physical bases.
- Coherence is intentionally conservative: **every guest or device-DMA
  RAM `mem_write`** advances a global generation (O(1)); MOV CR0/CR3/
  CR4, LMSW, WRMSR EFER, INVLPG and machine reset also flush. The direct
  *pre-execution* K1 loader table writes have an explicit flush before
  CPU start. No device DMA bypass was found in `src/`: USB, AHCI and
  e1000 writes use `mem_write`; VGA's raw text pointer is read-only.
  INVLPG remains a **no-fault operand hint** (K2); it flushes globally.
  `--stats` now adds internal-only TLB hits/misses/flushes, opt-in;
  default stdout, K6 virtual clock and P0 work count do not change.
- `tests/test_tlb.c`: warm hit followed by 5MiB→6MiB PTE rewrite and
  exact new data; MOV CR3 / CR0 / CR4, LMSW, WRMSR EFER and no-fault
  INVLPG; warmed read→nonpresent #PF (err=0) / NX fetch (err=0x11) /
  supervisor WP write (err=0x3) / CPL3 U/S read (err=0x5), all with
  matching CR2 and handler RIP; 2MiB/1GiB PS offsets; disabled A20;
  dynamic MMIO page-table root that changes **without** RAM write;
  SMP contexts with CR3/AC keys. `test-tlb: all vectors passed`.
- **NC P2 (one per phase):** `tests/integration/p2_tlb_nc.sh` mutates a
  scratch copy of `mem.c`, removes only the RAM-store TLB flush and runs
  the **same** P2 vector. After a warmed PTE is rewritten,
  `assert(c->gpr[RAX] == NEW_VALUE)` fails: binary **RC=134**, wrapper
  **RC=1**, `make test-tlb-nc` **RC=2** (`make` reports Error 1). Receipt
  `P2 NC: FAILED AS DESIGNED (RAM-store flush is necessary for PTE
  coherence)`; production checkout untouched.
- Firmware boot measured 15,863,692 hits / 4,668 misses / 4,669 flushes,
  4,837,677 executed attempts; five post-P2 gprof boots kept 79,798,615
  `translate` calls but cut `mem_read` calls from 313,666,250 to
  78,188,375. Alternating O2 pre/post triples across seven tapes kept
  identical work counts and gave local paired ratios **1.15–1.35×**
  (haswell 429→328 and 420→333 ms). No fixed host-time CI threshold or
  portable speedup claim. Four historical 600k-cap firmware/video stdout,
  ring-log and PPM pairs are byte-identical RC=0/0, framebuffer SHA
  prefixes `15c96625cfec5acc` / `c3b3d73f6bbb08ab`.
  `make test`, `make test-sanitize`, `make fuzz FUZZ_PROGRAMS=256`, and
  `make bench BENCH_REPS=3` all **RC=0** (N2/N3 ALL PASS; 6,144 diff-fuzz
  instructions match, 0 host crashes). Frozen guest lanes SKIP due
  missing build toolchain, not counted as passing guest receipts.
  In a fresh clone at parent `f7c3bd7`, `git am` applied cleanly to the
  identical tree; after seeding nasm outputs, `make test`,
  `make test-sanitize`, `make fuzz FUZZ_PROGRAMS=256`, `make bench
  BENCH_REPS=3` each passed RC=0, while `make test-tlb-nc` failed RC=2
  on the exact stale-PTE assertion as designed.


### Changed — patch 0052 (PERF P1, profile-guided 8-byte RAM read)

- **Measured hotspot D5, not a speculative refactor:** five O2 `-pg`
  firmware runs aggregated by gprof: `mem_read` 313,666,250 calls,
  45.66% of sampled time (1.21 / 2.65 s), including 235,535,030 calls
  from `translate`'s 8-byte page-table walks. `mem_write` had only
  2,514,365 calls / 0.00% of samples and was **not** changed. Only an
  8-byte read *entirely inside RAM*, with A20 open, after both ROM alias
  checks, takes an unaligned-safe `memcpy` on little-endian hosts. Closed
  A20, partial reads at RAM end, ROM/BIOS (low alias lives *inside* RAM!),
  MMIO, other widths and all writes follow the exact old path. Post-change
  five-run profile: same 313,666,250 calls, 14.37% (0.26 / 1.81 s);
  percentages are noisy samples, not a portable performance guarantee.
- `tests/test_perf_mem.c` six vector groups: unaligned LE read; last 8/7/1
  RAM bytes + FF fill/clipped writes; low/top ROM aliases and wrap;
  closed-A20 start-only masking; untouched 1/2/4 widths; high MMIO BAR
  callback versus existing RAM priority. `test-perf-mem: all vectors
  passed` in both `make test` and `make test-sanitize`. Initial test
  draft expected RAM at `0x1FFFFE` with A20 closed (which masks to the
  **ROM** alias `0x0FFFFE`) and reddened RC=134; fixed the vector to
  `0x3FFFFE` → `0x2FFFFE` (RAM), not the emulator.
- **NC P1, one for the phase:** `tests/integration/p1_mem_nc.sh` makes a
  scratch copy of `mem.c`, changes only the fast-path return to
  `__builtin_bswap64(v)`, compiles the *same* regression vector. Inner
  binary **RC=134** on `assert(mem_read(&g_m, 0x123, 8) ==
  0x1716151413121110ULL)`, script **RC=1**, `make test-perf-mem-nc`
  **RC=2**. Receipt: `P1 NC: FAILED AS DESIGNED (swapped-endian RAM fast
  path is detected)`; production code and test checkout untouched.
- O2 alternate pre1/post1/pre2/post2 on the same host, seven tapes ×3
  repetitions: work counts identical on every tape (haswell
  **4,837,677**), none reached cap. Haswell medians were 691→514 ms and
  783→427 ms (paired ratios 1.34× and 1.83×); all seven tapes' paired
  ratios fell between 1.18× and 1.83×. Even pre1/pre2 and post1/post2
  differed substantially: **do not quote a universal speedup** or assert
  a fixed wall-time gate. Four historical 600k-cap firmware/video tape
  stdout, ring log and PPM were byte-identical before/after; SHA-256 PPM
  prefixes `15c96625cfec5acc` (boot/VBE/on-off) and
  `c3b3d73f6bbb08ab` (no-fb). `make test` **RC=0**, `make test-sanitize`
  **RC=0**, `make fuzz FUZZ_PROGRAMS=256` **RC=0** (6,144 instructions,
  all states match; zero host crashes). Frozen guest lanes SKIP: missing
  toolchain and build artifacts; not silently counted as passing.
  `docs/plans/PERF_PLAN.md` §4 records the before/after profile, paired
  timing table, boundary proof and fresh-clone verification: `git am`
  cleanly on P0 produced the same tree; after seeding the three ignored
  nasm artifacts, `make test`, `make test-sanitize`, `make fuzz
  FUZZ_PROGRAMS=256`, `make bench BENCH_REPS=3` each passed RC=0; the
  P1 NC failed by design with `make` RC=2.


### Added — patch 0051 (PERF P0, honest benchmark before optimization)

- The missing performance baseline is now *measured*, not inferred from a
  `--max-instr` ceiling. During P0 scoping a 20M cap / 0.685s was mistakenly
  reported as **28.69 MIPS**; the built binary actually stopped at
  `instr=4837677`, or **7.06 Msteps/s** on that run (cap/actual = 4.13×).
  Independently, `STI;HLT` + two idle cpu_step slots yields
  `instr_count=vtime=1026` but only **2** instruction attempts: the K3
  `+512` all-idle virtual-time quantum is the K6 *guest clock*, not work.
  Changing this clock to "fix" MIPS would violate the frozen guest timing
  contract, so the repair adds `cpu.executed_steps` alongside it. The new
  metric counts a faulting attempt too and is **not** ISA retirement.
- `--stats` is opt-in, reporting per-vCPU executed steps, machine total,
  idle delta, BSP instruction clock, virtual clock and cap-reached flag;
  without it stdout does not change. `machine_executed_total` handles
  `n_vcpus=0` (historical UP case) and sums slot 1 when `--smp=2`.
  `tests/test_perf.c`: six deterministic vectors, including `STI;HLT` with
  two idle slots (`executed_steps=2`, `instr_count=vtime=1026`), K6 identity,
  faulting UD2, context-count 0/1/2 and a bigger budget on a stopped tape.
- `make bench` rebuilds **separate O2 `x86emu-bench`** even after
  `make test-sanitize` has replaced `x86emu` with O1 ASan+UBSan; seven
  firmware tapes × odd number of repetitions (default 5) yield the median
  whole-process wall time, Msteps/s and ns/attempt. It rejects non-
  deterministic work counts, a run cut off by its cap or a zero-work tape;
  it never gates on a fixed host timing. Measured here at O2 with 5 reps:
  boot-haswell 4,837,677 steps / 678 ms / 7.14 Msteps/s; boot-broadwell
  4,807,694 / 666 ms / 7.22; boot-baytrail 4,807,650 / 668 ms / 7.20;
  text-no-fb 4,837,677 / 676 ms / 7.16; gfx-fb 4,837,677 / 671 ms /
  7.21; VBE 800x600x32 4,837,677 / 671 ms / 7.21; SMP2 4,837,677 /
  985 ms / 4.91. **These wall times belong to this machine only**.
- **NC P0 (one per phase):** `make bench-nc BENCH_REPS=3` uses the *same*
  wall measurements but divides the cap instead of the executed count;
  receipt `perf bench NC: FAILED AS DESIGNED (cap-based MIPS is not a
  measurement)`. Direct `tests/perf_bench.sh --reps=3 --nc` exits
  **RC=1**; `make bench-nc BENCH_REPS=3` exits **RC=2** because make
  reports the recipe's Error 1 as its own failure. On haswell boot: 50M / 4,837,677 = **10.34×**
  overstatement. The shipped harness uses 4,837,677 and passes RC=0.
- Pre/post at the same `--log`/`--dump-fb` paths: four historical 600k-
  cap lanes (boot, VBE on, VBE off, no-fb), stdout, ring-log and PPM all
  **byte-identical**, each RC=0/0; framebuffer SHA prefixes
  `15c96625cfec5acc` (boot/on/off) and `c3b3d73f6bbb08ab` (no-fb).
  `make test` **RC=0** (`test-perf: all vectors passed`, N2/N3 receipts),
  `make test-sanitize` **RC=0**, `make fuzz FUZZ_PROGRAMS=256` **RC=0**
  (256 programs, 6144 instructions, all states match, 0 host crashes).
  `docs/plans/PERF_PLAN.md` begins the phase ledger and records exact gates,
  remaining P1–P5, constraints and the known missing guest toolchain.
  The phase patch also applied cleanly with `git am` on `a3f76b5` in a fresh
  clone, producing the identical tree; after seeding the nasm artifacts,
  that clone passed `make test` and `make test-sanitize` RC=0, `make bench
  BENCH_REPS=3` RC=0; `make bench-nc BENCH_REPS=3` RC=2 as designed.


### Fixed — patch 0050 (VIDEO V4, dispi mode clamp: host heap overflow)

- **A host heap overflow reachable from the CLI and from any guest**, found
  by ASan after `VIDEO_PLAN.md` was closed and filed as defect **D6**.
  `dispi_fixup` (`src/devices.c`) was a *partial* transcription of QEMU's
  `vbe_fixup_regs`: it raised `VIRT_WIDTH` to `XRES` and derived
  `VIRT_HEIGHT`, but it never clamped `YRES` — the one adjustment that
  bounds QEMU's clear-to-screen. So `vbe_ioport_write_data`'s
  `memset(vram, 0, YRES * line_offset)` ran on the guest's own unclamped
  height, out of a `calloc` sized to the published 800×600 framebuffer
  (1920000 B). Measured on the built emulator, no guest needed:

  ```
  ./x86emu --platform=haswell --vbe=1024x768x32  --max-instr=1000
    unsanitized: RC=134, `free(): invalid pointer`
    ASan+UBSan:  RC=1,   heap-buffer-overflow WRITE of size 3145728
                         at dispi_data_write, src/devices.c:1055
                         "0 bytes after 1920000-byte region"
  ./x86emu --platform=haswell --vbe=1600x1200x32   -> WRITE of size 7680000,   RC=1
  ./x86emu --platform=haswell --vbe=16000x12000x32 -> WRITE of size 768000000, RC=139
  ./x86emu --platform=haswell --vbe=800x600x32     -> RC=0 (the documented mode)
  ```

  ~400× the window in the extreme case, and 1024×768 — an entirely
  ordinary resolution — was enough. Same class as QEMU's CVE-2016-3712,
  whose commit note makes the point: the purpose of `vbe_fixup_regs` is
  that "the sanity checks … are effective", and without them "which is
  good for one or another buffer overflow".
- **The fix is the whole QEMU function**, fetched for this phase
  (`gitlab.com/qemu-project/qemu/-/raw/master/hw/display/vga.c`) rather
  than reconstructed, transcribed in QEMU's order: the depth switch
  (whose default arm *assigns* 8, so an impossible BPP cannot reach the
  width math), `XRES &= ~7` / `== 0 → 8` / `> MAX → MAX`, the same three
  for `VIRT_WIDTH`, `linelength = VIRT_WIDTH * bits / 8`,
  `maxy = vbe_size / linelength`, `YRES == 0 → 1` / `> MAX → MAX` /
  `> maxy → maxy`, the three-rung offset ladder, then
  `VIRT_HEIGHT = maxy` and `line_offset = linelength`. The `DISPI_ENABLE`
  arm moved to QEMU's shape too — fixup, `VGA_GFX_MISC`, the display flip
  and the clear all **inside the 0→1 edge**, with `bank_offset = 0` in
  the `else` (which covers 1→0 and a 1→1 flag rewrite). The clear is
  additionally bounded by an explicit `min(clear, vram)` so a future edit
  to the fixup cannot silently become heap corruption again; that is
  belt-and-braces, not the fix, and the vector does not depend on it.
- **Two deliberate divergences, both recorded in the source**: no
  `vbe_start_addr` (QEMU folds the ladder's result into it for its
  display code to pan inside VRAM; this model renders from `m->fb` at
  offset 0, so there is no consumer — but the clamps still run, being
  observable through the register readbacks), and **BPP 4** (QEMU's write
  handler accepts only 8/15/16/24/32 so its fixup `case 4:` is dead code;
  this model's handler has accepted 4 since V2, which stays — following
  QEMU's expression literally into a zero linelength would divide by zero
  in `maxy`).
- **Regression vector in the same patch**: `test_dispi_mode_clamped_to_vram`
  in `tests/test_video.c` (10 → **11 vectors**, RC=0 plain and under
  ASan+UBSan). Every expected number is QEMU's arithmetic on the
  1920000-byte window, computed independently first: 1024×768×32 →
  YRES **468** (clear 1916928 ≤ VRAM); 16000×12000×32 → YRES **30**
  (clear 1920000, **exactly** the window); 800×600×32 → YRES **600**
  (also exactly the window, i.e. the documented mode is untouched);
  YRES 0 → **1**; XRES 0 → **8** with VIRT_HEIGHT **60000**; BPP 33 →
  **8**; VIRT_WIDTH 65535 → **16000**; VIRT_WIDTH 9 → **800** (8 then
  raised to XRES) — plus all three rungs of the offset ladder, each in
  the mode where it is actually reachable: rung 1 at 8×1×8
  (`100008 ≤ VRAM`, offsets read back 16000/12000), rung 2 at 40×1×32
  (`12000*160 + 160 = 1920160 > VRAM`, retest `185` fits → Y_OFFSET 0,
  X_OFFSET survives at 100), rung 3 at 800×600×32 (both zeroed). Rung 2
  needs `linelength ≥ 160` *because* Y_OFFSET is clamped to 12000 first —
  at 8×1×8 or 8×1×32 it is unreachable by construction.
- **The phase's NC, measured**: the pre-fix `src/devices.c`
  (`git show HEAD:src/devices.c`) with the new vector compiled against it
  gives **RC=1** — `heap-buffer-overflow WRITE of size 3145728`,
  `ABORTING` before the vector's first readback; with the fix, **RC=0**
  and all 11 vectors green. The vector is the thing that fails before the
  fix, not merely something that passes after it. A one-off exhaustive
  sweep under ASan+UBSan backs it up: **5236 mode combinations** (17
  hostile XRES × 14 YRES × 11 BPP, each with and without
  `DISPI_NOCLEARMEM`) plus **146672 register pokes** (every index
  0x00–0x09 written as 65535/1/0 while enabled, re-running the fixup
  each time, plus a 64-value BANK sweep), asserting after each step that
  `YRES * line_offset <= VRAM` with QEMU's own expression → **0
  violations, RC=0** post-fix; **RC=1 on the first combination** pre-fix.
- **Historical receipts proved stable, not assumed**: `test_video_modes.sh`
  SKIPs here (it needs the frozen `AuraLite-OS` build), so the pre-fix and
  post-fix emulators were run side by side on four lanes and every
  `--log` + `--dump-fb` pair compared — firmware boot, `--vbe=800x600x32`,
  `--vbe=off` and `--no-fb` are all **byte-identical** (rc 0/0), and the
  `--no-fb` framebuffer is `c3b3d73f6bbb…`, the sha **V3 recorded** for
  that lane. All ten pre-existing `test_video.c` vectors pass unmodified,
  including the write-handler gates and the GETCAPS maxima
  (16000/12000/32 — QEMU returns those unconditionally too).
- **Measured**: `make test` **RC=0** (`boot integration test: ok`, so the
  firmware GPU contract 0:2:0 `0x8086:0x0412` / BAR0 `0xD0000000` /
  `pixel[0]=0x00200000` did not move; and the NET lanes just closed stay
  green against the changed `src/` — `test-tap-responder: all tests
  passed`, `N2 responder receipts: ALL PASS`, `N3 end-to-end receipts:
  ALL PASS`); `make test-sanitize` **RC=0**; `make fuzz
  FUZZ_PROGRAMS=256` — `256 programs, 6144 instructions compared -- all
  states match`, `crash invariant: 64 garbage streams, 33 clean faults,
  2 halts, 0 host-level crashes`. Changed: `src/devices.c` and
  `tests/test_video.c` only — no CLI flag, no register map, no documented
  mode.
- **Not in V4** (measured, deferred): display panning / `vbe_start_addr`
  (no consumer); QEMU's `vbe_update_vgaregs` and the CVE-2016-3712
  re-assertion of VGA registers on every GR/CR write — this model's
  `VGA_GFX_MISC` path (`src/devices.c:852`) only routes text↔graphics
  and consumes no sizing register, so there is no overflow surface there
  to re-assert against; the BPP-4 write-handler divergence (bounded); and
  growing the VRAM window to QEMU's 16 MiB, which would move the
  guest-visible firmware contract and would not remove the need for the
  clamp anyway.
- **Docs**: `docs/plans/VIDEO_PLAN.md` — status line and table gain V4,
  the ledger gains **D6** with its repro receipts, and a new **§4**
  carries the phase (root cause against the fetched QEMU source, the
  vector's arithmetic table, the NC, the four-lane byte comparison, the
  gates, the deferred list); §"Standing constraints" renumbered to §5.
  ROADMAP (VIDEO row → V0–V4) + `docs/STATUS.md` sync.

### Added — patch 0049 (NET N3, end-to-end close-out: DNS + TCP)

- **`tools/tap_responder.py` 485 → 1024 lines** (stdlib only): the two
  self-tests that follow the ping in `kernel/kernel.c:428-430`
  (`net_dns_self_test()`, `tcp_x5_self_test()`) now have a peer, on the
  same ground-rule endpoint and with no emulator source change
  (`git status --porcelain src/` empty, so `src/e1000.c` stays
  byte-identical to N1's).
- **DNS over UDP/53** (`handle_dns`). Measured out of the guest, not
  assumed: the query leaves from the fixed local port **12345**
  (`dns.h:19`), so the answer has to go back there (`dns.c:172`);
  `flags 0x0100` (RD), `QDCOUNT=1`, lowercase length-prefixed QNAME,
  QTYPE=A, QCLASS=IN, `ip.ident = 2`, UDP checksum **0**; the id comes
  from `dns_next_id()` and is **not deterministic**, so it is echoed.
  The reply is 45 bytes — id · `0x8180` (QR+RD) · 1 question 1 answer ·
  `example.com` A/IN · compressed owner `0xC00C` · A/IN · TTL 3600 ·
  rdlen 4 · `93.184.216.34` — and it survives the guest's own strict
  parser (`dns_parse.c:132`: `class == 1`, `type == 1`, `rdlen == 4`,
  owner equal to the queried name, `0xC0` pointers with a bounded hop
  budget). TC (`0x0200`) is deliberately **never** set: a truncated
  answer would push the guest down the TCP retry path. Flags
  `--dns-ip` / `--dns-ttl`, and `dns` is a `--drop` value.
- **DNS over the RFC 1035 §4.2.2 TCP carriage** (`dns_over_tcp`).
  `dns.c:187 dns_wire_query_tcp` sends a *fresh* id, a two-byte length
  prefix and the message as **two separate `tcp_send_h` calls**, then
  accumulates the reply through non-blocking `tcp_recv_h` — so the
  responder buffers per connection and answers `[len][message]` in
  **one** segment that acknowledges the data itself. A separate bare ACK
  first was measured to be wrong twice over: it spends two of the
  guest's RX descriptors on one round trip, and the guest's reader would
  simply see the ACK before the answer. A prefix outside 12..4096 drops
  the stream instead of being trusted.
- **A real TCP connection table** (`handle_tcp`). `TCP_ISN = 0x5A5A0000`
  is a *constant* so the tape is deterministic — the guest never
  validates our ISN (`tcp.c:290` filters on the 4-tuple and the header
  length only, no seq/ack check). `TCP_MAX_CONNS = 64` rather than the
  guest's 16, because 16 is a *client* bound and a server must not
  refuse a 17th one. States `SYN_RCVD`/`ESTAB`/`LAST_ACK`; every
  behaviour below is pinned by a vector: SYN → SYN-ACK with MSS 1460 +
  SACK-permitted (`hdr=28`, matching the guest's own 8-byte SYN options
  at `tcp.c:178`); a **retransmitted SYN → a byte-identical SYN-ACK**
  (the guest retransmits after `TCP_RTO_TICKS`); the final bare ACK →
  **silence**; data → ACK, `ACK|PSH` with the answer when the buffer
  completes; `FIN|ACK` → `FIN|ACK`, which closes the guest's
  `tcp_close` in one RTT (`tcp.c:1343` needs both bits — ACK →
  FIN_WAIT_2, FIN → TIME_WAIT, in the same segment); `RST` → tuple
  dropped, no reply; an **unknown tuple → no reply** (an RST would be
  wrong: the guest's closer consumes whatever comes back on a live
  4-tuple); table full → `RST|ACK` acking `seq+1`. Inbound checksums are
  validated only when the field is non-zero, which is what the guest
  sends. `--no-tcp` is the phase's NC lever.
- **ARP for the whole on-link subnet.** `net_arp_resolve` (`net.c:356`)
  ARPs the *target* for an on-link address and only routes off-subnet
  traffic through the gateway, so N2's gateway-only answers made the DNS
  server unreachable. The responder now answers who-has for the gateway
  **or** any address in the guest's own `/24` (`on_subnet()`), never for
  the guest's own address, never off-subnet. The gateway log line is
  byte-identical to N2's; the rest get the suffix `(on-subnet)`, and the
  summary line gained `dns=` / `tcp=` counters.
- **`tests/test_tap_responder.py` 481 → 1070 lines: 9 → 16 vectors,
  135 `ok`, RC=0.** New: `test_dns_query_answer`,
  `test_dns_only_answers_what_was_asked`,
  `test_dns_is_deterministic_and_droppable`, `test_tcp_open_and_close`,
  `test_tcp_sixteen_concurrent`, `test_tcp_behaviours`,
  `test_dns_over_tcp`, `test_arp_for_the_whole_subnet`. Each builds its
  request with the guest's own builder and feeds the reply back through a
  transcription of the guest's own accept filter
  (`tcp_recv_segment_timeout`, `dns_parse_response`, the ARP reply
  check), so a reply passes only if the frozen guest would accept it.
- **`tests/nic_net.c` 552 → 1125 lines** (payload 15968 → 23096 B): it
  prints N2's receipts **verbatim** and the same four frames first on
  the wire, then runs ARP for `10.0.2.3` with a cache, a DNS query over
  UDP, the same query over the TCP carriage with the prefix framed as
  `dns_wire_query_tcp` frames it, and 16 concurrent connections to
  `10.0.2.3:53` opened in sequence, held, then closed. Marker moved
  `N2OK`/`N2FAIL` → **`N3OK`/`N3FAIL`**.
- **`tests/integration/n3_net_e2e.sh`** (new, 292 lines,
  `make test-net-e2e`, wired into `make test`). Lane A (×2) puts
  **76 frames** on the wire, byte-identical between runs —
  `replies arp=2 dhcp=2 icmp=1 dns=2 tcp=17 ignored=0`, counted off the
  tape as ARP 2 + DHCP 2 + ICMP 1 + DNS/UDP 1 + SYN 17 + bare ACK 34 +
  FIN|ACK 17 + ACK|PSH 2:

  ```
  [serial] [dns] PASS: 'example.com' -> 93.184.216.34
  [serial] [tcp] SYN options: 8 bytes, hdr=28, mss=1460 sack-perm
  [serial] [tcp] ESTABLISHED (peer_wnd=64240)
  [serial] [dns] PASS: 'example.com' -> 93.184.216.34 over TCP (45-byte message, prefix ok)
  [serial] [tcp-x5] probing 16 concurrent TCP connections...
  [serial] [tcp-x5] PASS: 16 concurrent connections held and closed cleanly
  [serial] [net] PASS: DNS over UDP + DNS over TCP + 16 concurrent TCP connections
  [serial] N3OK
  ```

  Lane B re-pins N2's opt-in rule on the longer payload (no `--tap=` →
  the guest's own `[e1000] TX timeout` → `N3FAIL`, and not one
  `[dns]`/`[tcp` line). Lane C asserts the frozen-guest DoD strings and
  SKIPs here. Lane D is the NC.
- **The negative control, measured**: `--no-tcp` (lane D, runs every
  time) → harness **RC=1**, **10 frames** on the wire (N2's 4 + the DNS
  ARP + the DNS query + 4 SYNs), `replies … dns=1 tcp=0`, and

  ```
  [serial] [dns] PASS: 'example.com' -> 93.184.216.34
  [serial] [tcp] RTO waiting for SYN-ACK, retransmitting SYN   (×3)
  [serial] [tcp] timeout waiting for SYN-ACK
  [serial] [net] FAIL: no DNS answer over the TCP carriage
  [serial] N3FAIL
  ```

  The load-bearing assert is the **count**: exactly 3 retransmits, i.e.
  `TCP_MAX_RETRIES` from `tcp.c:766` counted off the guest's own ladder
  rather than a constant the harness invented. `--drop=dns` is *not* a
  substitute for this lever — with it the handshakes still succeed and
  only the query hangs, so the connection table is never exercised.
- **Measured**: `make test` RC=0 (now including `test-net-e2e`, with
  `test-net-tx`, `test-tap-responder` and `test-net-rx` re-green in the
  same invocation, and the boot integration receipts untouched —
  `[usb-msc] READ(10) LBA=0 blocks=1`, `GUEST_OK`, `framebuffer marker
  OK: pixel[0]=0x00200000`); `make test-sanitize` RC=0 (ASan+UBSan);
  `make fuzz FUZZ_PROGRAMS=256` — `256 programs, 6144 instructions
  compared -- all states match`, `crash invariant: 64 garbage streams,
  33 clean faults, 2 halts, 0 host-level crashes`. Artifact shas are in
  `docs/plans/NET_PLAN.md` §N3.
- **The patch itself, verified**: `patches/0049-NET-N3-net-e2e.patch`
  `git am`s cleanly onto the parent `6c2ee3d` in a fresh
  clone, the resulting tree is **identical** to the committed one
  (`git diff cb7287b HEAD` empty), and `make test` in that clone returns
  **RC=0** with `N1`/`N2`/`N3` receipts all green. Two SKIPs there are
  environment, not code: the frozen-guest lanes (no `AuraLite-OS` build
  in this sandbox) and `test-net-tx` (`SKIP: nasm missing` — the
  `.gitignore`d `firmware/*.bin` and `disk/*.{bin,img}` are `nasm`
  products and `nasm` is not installed here, so a fresh clone cannot
  build the *firmware-boot* artifacts at all; the clone was seeded with
  them, which is what a machine with `nasm` does by itself). The NET
  lanes N3 adds ride `--kernel=`, not the firmware boot, so they are
  unaffected. `git am` also reports `50 lines add whitespace errors`:
  every one is a line inside the **nested `patches/0048`** this commit
  carries (house convention — a phase's commit ships the previous
  phase's patch); `grep -cP "[ \t]+$"` is `0` for all eleven files
  authored in N3. Seeding that clone took three `nasm` artifacts, not
  one, plus backdating their inputs below them — `git checkout`
  gives every source file the checkout time, so a copied-in
  `firmware.bin` otherwise looks *older* than `sample_firmware.nas`
  and make rebuilds it through the missing `nasm` anyway. Recipe and
  measurements in `docs/plans/NET_PLAN.md` §3.9; which artifact
  shas are workdir-independent, and why `unit.out` is not one of
  them, in §3.4.

### Fixed / measured — patch 0049

- **D8 — ARP was blind to every address but the gateway.** The first N3
  wire run stopped at `[net] ARP request sent for 10.0.2.3` /
  `[dns] FAIL: could not resolve the server's MAC`: N2 answered who-has
  for `10.0.2.2` only, but the guest ARPs an on-link *target* directly.
  Fixed with `on_subnet()`; locked by `test_arp_for_the_whole_subnet`
  (10.0.2.2/.3/.99 answered, the guest's own 10.0.2.15, `8.8.8.8` and
  `10.0.3.1` not).
- **D9 — TCP checksums were gated on `--udp-checksum`.**
  `tcp_segment` was handed `compute_csum=self.udp_checksum`, so with the
  default flags every FIN-ACK and RST went out with checksum **0**.
  That is legal for UDP and not for TCP — RFC 793 §3.1 makes the
  checksum mandatory over IPv4 — and `tcp.c:319` drops the segment.
  Receipts before the fix: `TCP: bad checksum ignored` on the responder's
  log next to `[dns] FAIL: TCP carriage returned 0 of 2 bytes` on the
  guest's. TCP checksums are now always computed and `--udp-checksum`
  documents itself as UDP-only.
- **D10 — the odd-length checksum tail, in the test-side code.** The
  payload's `checksum()` and the vectors' `g_checksum()` added a trailing
  byte as `p[len-1]` instead of `p[len-1] << 8`; RFC 1071 treats an odd
  tail as the high half of a word whose low half is zero, and the frozen
  guest gets it right (`net.c:181`). The bug was invisible while every
  checksummed length was even — IP header 20, ICMP echo 40 — and the
  first odd-length segment on the wire exposed it: a **49-byte TCP
  segment** carrying a 29-byte DNS query, sum off by `0xFF00`, correctly
  refused. A frame-by-frame dump of the tape localised it (every
  even-length segment `verify=0x0000`, that one `0xff00`), and the
  responder's own `csum()` — which pads with a zero byte — was already
  right. Both copies fixed; the rule is now pinned by
  `test_dns_over_tcp` and by the wire tape.
- **D11 — harness port contention.** The first `make test-net-e2e` run
  produced a lane A that saw **3 frames and answered none of them**
  (`replies arp=0 dhcp=0 icmp=0 dns=0 tcp=0`) while the very next lane
  was perfect. Cause: a responder left over from a hand run still held
  UDP 8080, and `SO_REUSEADDR` lets two UDP sockets bind the *same*
  port, so the datagrams were split between them. Both harnesses now
  check `port_free()` before starting a lane and print the remedy
  (`pkill -f tools/tap_responder.py`) instead of reporting a failure
  that is really the harness's own mess.
- **Docs**: `docs/plans/NET_PLAN.md` — **the plan is now COMPLETE
  (N0–N3 ✅)**, §N3 written up with the measured DNS/TCP/ARP contract
  from `kernel/net/dns.c`, `dns_parse.c`, `tcp.c` and `net.c`, the 76-
  and 10-frame compositions counted off the tapes, the artifact shas,
  and ledger entries **D8–D11**; ROADMAP (NET row 🔵 → ✅) +
  `docs/STATUS.md` + README sync. `tests/integration/n2_responder.sh`
  updated for the payload's new marker and tape length: it still asserts
  N2's receipts and N2's NC unchanged, and pins N2's four frames as
  `head -4` of the tape rather than the whole tape, leaving the total to
  N3's harness. **N2's recorded artifact shas no longer reproduce**
  (the payload grew), which is recorded in both plans rather than left
  to look like a regression.

### Added — patch 0048 (NET N2, host responder: DHCP + ARP + ICMP)

- **`tools/tap_responder.py`** (new, 485 lines, stdlib only): the host
  side of the `--tap=` socket. It binds the ground-rule endpoint
  `UDP 127.0.0.1:8080` and answers the three protocols the frozen guest
  speaks — DHCP DISCOVER→OFFER and REQUEST→ACK, ARP who-has for the
  gateway, ICMP echo → echo reply — with slirp's user-mode addressing
  taken from the guest's own source (`kernel/net/net.c:39-47`): guest
  `10.0.2.15`, gateway/DHCP server `10.0.2.2`, DNS `10.0.2.3`, lease
  3600 s. It is **stateless** (no lease table, no ARP cache, no
  sequence numbers), so two instances and a replayed frame answer
  identically — both pinned by vectors. Option order is slirp's
  (`53, 54, 51, 1, 3, 6, END`), the DHCP replies are broadcast at L2
  *and* L3, the ARP reply is broadcast, the echo reply is unicast and
  returns all 32 payload bytes unchanged. Flags: `--mode=slirp|capture`
  (`capture` = N1's record-only lane), `--gw-mac`, `--udp-checksum`
  (off by default — the guest sends 0 and validates nothing),
  `--drop=arp,icmp,dhcp` (the NC lever), `--out`, `--count`,
  `--timeout`, `--quiet`. Closes defect **D3**.
- **`tests/test_tap_responder.py`** (new, 481 lines, `make
  test-tap-responder`, RC=0): 9 vectors. The test builds its requests
  with a *second, independent* transcription of `net.c`'s builders and
  feeds the replies back through a transcription of `net.c`'s own
  accept filters, so a reply passes only if the guest would accept it —
  including the details that are easy to get right-looking and wrong:
  DISCOVER is **287** bytes (`14+20+8+236+3+5+1`), UDP checksum **0**,
  ports `68→67`, ARP padded to 60, ICMP 74 bytes with payload
  `0x41..0x60`, the ACK path checks `xid` while the ARP path checks
  only `opcode == 2` and copies `sender_mac` verbatim.
- **`tests/nic_net.c` + `tests/nic_net.ld`** (new, 552 + 33 lines): the
  wire payload, a freestanding x86_64 guest stand-in that rides the
  **K1 kernel-load transport** (`--kernel=`) instead of N1's
  firmware-boot one. The reason is measured: the firmware loader
  delivers a single 512-byte block from LBA 0 and its long-mode map
  covers only `0x100000..0x1FFFFF` (`PD[0] = 0x19B`, 2 MiB page), while
  DHCP + ARP + ICMP plus a 64-deep RX ring need more code and more
  address space; `--kernel=` gives identity 0..4 GiB + HHDM and entry
  in long mode with paging on. It reproduces the frozen guest's driver
  where it matters — rings programmed as `e1000_init` steps 6/7 do
  (`RCTL = 0x0400801a`, `MTA` all-ones, `RDT = N-1`, `RAL/RAH` + AV),
  `nic_send` = `e1000_send` (`cmd 0x0B`, TDT doorbell, the **100000**
  pause-spin budget from `drivers/e1000/e1000.c:565`), `nic_poll` =
  `e1000_hw_rx_drain` — and its frames are byte-identical to `net.c`'s
  (`xid 0x12345678`, ident `0x1234`/`0x1235`, `flags 0x8000`, ICMP
  ident `0x1234` seq `1`, `ip.ident 1`, `ttl 64`, 60-byte pad).
- **The wire lane** (`tests/integration/n2_responder.sh`, new, 222
  lines, `make test-net-rx`, wired into `make test`). Lane A (×2
  determinism pair) receipts, all four frames on the wire and nothing
  else:

  ```
  [serial] [dhcp] PASS: IP 10.0.2.15, gateway 10.0.2.2
  [serial] [net] ARP reply: gateway is 52:55:0a:00:02:02
  [serial] [net] ICMP payload verified (ident+seq+32 bytes)
  [serial] [net] PASS: lease + ARP + echo reply (payload verified)
  [serial] N2OK
  responder: 4 frame(s) in, replies arp=1 dhcp=2 icmp=1 ignored=0
  ```

  Lane B re-runs the same payload **without** `--tap=`: no PCI function
  ⇒ BAR0 reads the open bus ⇒ all-ones ⇒ `STATUS.LU` *reads set*, so
  the payload proceeds into a TX ring nobody services and reports the
  guest's own timeout instead of hanging —
  `[e1000] TX timeout: desc[0].status=0x00 TDH=4294967295
  TDT=4294967295`, `[dhcp] FAIL: DISCOVER transmit failed`, `N2FAIL`.
  Lane C (frozen guest, the DoD as the guest prints it) **SKIPs** here
  for the same reason as in N0/N1 — the AuraLite-OS toolchain
  (`clang --target=x86_64-elf`, `ld.lld`, `rustc`, `mtools`) is not
  installable in this tree — and prints the exact reproduction line.
- **NC (once per phase)** — measured on both sides:
  1. *host-side, lane D, runs every time*: `--drop=arp,icmp,dhcp` ⇒
     `[drop] DHCP message type 1 (negative control)`,
     `replies arp=0 dhcp=0 icmp=0 ignored=0`, guest
     `[dhcp] FAIL: no OFFER received` / `[net] FAIL: no DHCP lease` /
     `N2FAIL`, exactly **1** frame on the wire (the DISCOVER). The
     harness asserts every line and exits 1 if any is missing.
  2. *emulator-side, by hand* (needs a rebuilt emulator, so it is a
     documented step rather than a lane that quietly rebuilds the tree):
     removing `mem_write(m, desc + 8, 2, len);` — the RX descriptor
     **length** writeback, `src/e1000.c:372` — flips lane A to
     `[dhcp] FAIL: no OFFER received` / `[net] FAIL: no DHCP lease` /
     `N2FAIL` while the responder still logs a complete
     DISCOVER/OFFER/REQUEST/ACK/ARP/ICMP exchange. Restored from
     backup, gates re-green. **Not** a valid control, and this is the
     phase's most useful measurement: removing the *DD* writeback at
     `src/e1000.c:373` leaves the run **green** (`N2OK`), because
     `e1000_hw_rx_drain` gates on `pkt_len > 0` and never looks at DD.
     `tests/nic_net.c` now additionally requires DD (a strictly tighter
     gate than the guest's, so it cannot pass on an emulator that
     publishes a stale length with no completion), and the comment in
     `nic_poll` records why the control had to move.
- **A bug the wire found that no protocol vector could**: the responder
  first replied with `sendto(reply, (host, port))` — to the endpoint it
  had *bound*, which is the endpoint the emulator `connect()`ed **to**,
  so every reply went to the responder's own socket. Its log looked
  perfect (`DISCOVER → OFFER 10.0.2.15`) next to a guest printing
  `[dhcp] FAIL: no OFFER received`. Fixed to reply to the peer address
  `recvfrom` reports, and locked by
  `test_reply_goes_to_the_peer_not_to_the_endpoint`, which drives the
  real tool over a real connected socket; reintroducing the bug fails 3
  of its 4 checks (measured).
- **Measured**: `make test` RC=0 (now including `test-tap-responder` +
  `test-net-rx`; `test-net-tx` re-run green in the same invocation, and
  the boot integration receipts untouched — `[usb-msc] READ(10) LBA=0
  blocks=1`, `GUEST_OK`, `framebuffer marker OK:
  pixel[0]=0x00200000`); `make test-sanitize` RC=0 (ASan+UBSan);
  `make fuzz FUZZ_PROGRAMS=256` — `256 programs, 6144 instructions
  compared -- all states match`, `crash invariant: 64 garbage streams,
  33 clean faults, 2 halts, 0 host-level crashes`. **No emulator source
  changed in N2** — `git status --porcelain src/` is empty, so every
  historical lane is byte-stable by construction. Lane A/B/D artifact
  shas are in `docs/plans/NET_PLAN.md` N2.
- **New ledger entry D7** — the gateway's MAC is *not measurable* here:
  slirp's convention is `52:55:` + the gateway's four IP bytes
  (`52:55:0a:00:02:02` for `10.0.2.2`), but libslirp's `bootp.c` was
  unreachable from this sandbox (four mirrors tried — qemu master,
  v5.2.0, v4.2.0, gitlab.freedesktop.org — all 404 or access-blocked)
  and no QEMU binary is installed to trace. It gates nothing (the
  guest's ARP accept path checks only length, ethertype and
  `opcode == 2`), so the value is quarantined behind `--gw-mac`.
- **Docs**: `docs/plans/NET_PLAN.md` (N2 ✅ with the measured protocol
  contract from `kernel/net/net.c`, D3 closed, **D7** added, and a
  correction — the DoD receipt this plan used to quote,
  `[net] PASS: lease + ARP + echo reply (payload verified)`, is from
  `kernel/arch/i386/net32.c:319`, the **i386** port; the x86_64 guest
  never prints it. N2's real receipts are the two the guest's own
  integration cases assert: `[dhcp] PASS: IP 10.0.2.15, gateway
  10.0.2.2` (`test_e1000e.sh:48`, `test_vmxnet3.sh:49`) and `[net]
  PASS: ping 10.0.2.2 successful` (`net.c:1190`, `test_e1000e.sh:50`,
  `test_vmxnet3.sh:51`, `scripts/ci_test.sh:72`). The payload prints
  the i386 line too, as an *extra* receipt on top of the real ones),
  ROADMAP + `docs/STATUS.md` + README sync.

### Added — patch 0047 (NET N1, e1000 TX-ready)

- **The TX ring** (`src/e1000.c`): the doorbell walk `e1000_start_xmit`
  (QEMU `set_tctl`, which `macreg_writeops[]` maps **both** `[TCTL]` and
  `[TDT]` to — store, `TDT &= 0xffff`, walk), `e1000_tx_desc` (legacy
  descriptor: `split_size = lower.data & 0xffff`, clamped to the room
  left in `tx_data[0x10000]`, DMA-read at `buffer_addr`, frame out on
  `CMD.EOP`, `CMD` left intact) and `e1000_send_frame` (one TAP
  datagram per frame; `tap_fd < 0` or a failed `send` drops the frame
  but still completes the descriptors — QEMU's null-backend shape, and
  what lets the socketless unit lane assert DD instead of the wire).
  `TCTL.EN` gates the walk, `tx_busy` is QEMU's re-entrancy guard, TDH
  advances with `++TDH * 16 >= TDLEN -> 0`, and QEMU's two escapes for
  a bogus TDT/TDLEN (TDH wraparound, `tdh_start >= TDLEN/16`) are kept
  so a misprogrammed ring cannot spin. Causes end in ICR as
  `TXQE | TXDW*` (`ICR = 0x81` for the guest's single-descriptor
  send). **DD is written back inside the TDT store**, because
  `e1000_send()` spins on `status & 0x01` with a 100000-PAUSE budget —
  a `devices_tick`-deferred completion would still burn it. Closes
  defect **D2** (`[e1000] TX timeout: desc[N].status=0x00 TDH=0
  TDT=N+1`, `[dhcp] FAIL: DISCOVER transmit failed`).
- **QEMU write-op parity** (ledger **D6**): N0 stored every non-CTRL/IMC
  register verbatim, which measured wrong against `macreg_writeops[]` —
  `IMS` is `set_ims` (`|= val`, not assign), `ICR` is `set_icr`
  (`&= ~val`, not a no-op), `ICS` is `set_ics`, `TDH/RDH` are
  `set_16bit`, `TDLEN/RDLEN` are `set_dlen` (`val & 0xfff80`), and
  `TDT` shares TCTL's op. No N0 vector pinned the old behaviour.
- **`e1000_tx_stats`** (`src/devices.h`): the wire counters
  (`tx_frames`/`tx_dropped`). The device logs nothing on the RX/TX path
  (parity with N0), so a harness proves "the frame reached the wire" by
  reading these rather than grepping a log whose line count would
  depend on how many frames a responder answered with — which would
  cost the lane its byte-identical determinism pair. `e1000_reset_regs`
  clears them plus `tx_size`/`tx_busy`, so a half-assembled frame
  cannot survive `CTRL.RST` into the next ring.
- **`tests/test_net.c` 5 new vectors** (2 N0 + 5 N1, in `make
  test-unit`): `ok e1000 tx doorbell completion`, `ok e1000 tx TCTL.EN
  gate + TCTL-write doorbell + TDT 16-bit mask`, `ok e1000 tx
  multi-descriptor EOP, RS-gated writeback, TDH wrap, TDLEN mask`, `ok
  e1000 interrupt/register write-op parity`, `ok e1000 tx link gate +
  CTRL.RST resets the TX ring`. They pin DD + `CMD` intact + TDH +
  `ICR=0x81` + read-to-clear + drop counters; `TCTL.EN` clear ⇒ TDT
  stores but consumes nothing *and raises no cause*, then a TCTL write
  drains it; a no-EOP descriptor accumulates so two descriptors yield
  **one** frame while both get DD (writeback runs for every descriptor
  of the walk); `RS` clear ⇒ no DD but the slot is consumed; TDH 31→0;
  `TDLEN & 0xfff80`; `STATUS.LU` and `CTRL.RST`.
- **The wire lane** (`tests/nic_tx.asm`,
  `tests/integration/n1_tap_responder.py`,
  `tests/integration/n1_tx_tap.sh`, `make test-net-tx`, wired into
  `make test`): a 489-byte payload rides the **real firmware-boot
  transport** (throwaway `--disk=` image → `[usb-msc] READ(10) LBA=0
  blocks=1` → `conventional entry reached at 0x00100000` → long mode),
  programs the ring exactly like `e1000_init`/`e1000_send` and rings
  TDT; the responder holds the ground-rule endpoint `UDP
  127.0.0.1:8080`. Receipts, twice, byte-identical (`cmp` clean):
  guest `[serial] N1TXOK  status=01 tdh=01 icr=81`, wire
  `ffffffffffff52540012345608004e3154584f4b` (`ff:ff:ff:ff:ff:ff` /
  `52:54:00:12:34:56` / `0x0800` / `N1TXOK` — 20 bytes in, 20 bytes
  out, no FCS invention). Lane B re-runs the same payload **without**
  `--tap=` and gets `N1TXFAIL  status=00 tdh=ff icr=ff` (no PCI
  function ⇒ BAR0 reads the open bus) — the pin that keeps every
  historical no-NIC lane byte-stable. Lane C (the frozen guest, `[e1000]
  TX timeout` gone + `[dhcp]` on the wire) **SKIPs** here: the
  AuraLite-OS toolchain (`clang --target=x86_64-elf`, `ld.lld`, `rustc`,
  `mtools`) is not installable in this tree, so the harness prints the
  exact reproduction line instead of failing.
- **NC (once per phase)**: removing the descriptor writeback from
  `e1000_tx_desc` reddens the doorbell vector — `RC=134`,
  `tests/test_net.c:167: test_tx_doorbell_completion: Assertion
  'tx_desc_status(&f, ring, 0) == 0x01' failed` — and flips the wire
  receipt to `N1TXFAIL status=00 tdh=01 icr=80`, i.e. exactly the
  `status=0x00` half of the D2 guest receipt (head still advances,
  frame still leaves, only the completion is missing — which is why the
  guest's *spin* is what times out). Restored from backup, gates
  re-green.
- **Measured**: `make test` RC=0 (boot integration receipts untouched:
  `[usb-msc] READ(10) LBA=0 blocks=1`, `GUEST_OK`, `framebuffer marker
  OK: pixel[0]=0x00200000`); `make test-sanitize` RC=0 — and since that
  target does not rebuild `test-net`, the net vectors *and* the wire
  lane were additionally run by hand under
  `-fsanitize=address,undefined -fno-sanitize-recover=all`
  (`X86EMU=<asan build> tests/integration/n1_tx_tap.sh`), both clean,
  so the real `send()` path is covered too; `make fuzz
  FUZZ_PROGRAMS=64` — `64 programs, 1536 instructions compared -- all
  states match`, `0 host-level crashes`. Lane A artifact shas are in
  `docs/plans/NET_PLAN.md` N1.
- **Deliberately not in N1** (measured, recorded as ledger **D4** and
  in the plan's "Not in N1"): the INTx line — `e1000_add` seeds no PCI
  cfg `0x3C` and nothing emulates INTA#/ack, so the guest reads
  `IRQ line 0` and polls; raising a line with no ack path would leave
  the 8259A ISR bit set and never re-service the guest, which is worse
  than not raising it. Also deferred: TX statistics counters
  (`TPT`/`TOTL`/`GPTC`/`PTC*`), context/TSO descriptors, VLAN
  insertion, FCS insertion for `CMD.IFCS`.
- **Docs**: `docs/plans/NET_PLAN.md` (N1 ✅ with the measured TX
  contract from both sources, D2 closed, D4/D5/D6 added — D5 records
  the deliberate divergence from QEMU's `TXD_STAT_DD = 0x01000000`,
  which lands in descriptor byte 15 while the SDM and the frozen guest
  driver both read DD as bit 0 of the status byte at offset 12),
  ROADMAP + `docs/STATUS.md` + README sync.

### Added — patch 0046 (NET N0, e1000 RX-ready + TAP ingress)

- **The RX-ready e1000 model** (`src/e1000.c`): PCI 0:4.0 `8086:100E`
  (class `0x02/0x00`) with **BAR0 `0xFEBC0000`/128 KiB seeded into PCI
  config** (the house pattern, cf. EHCI/UHCI/AHCI — the first N0 lane
  measured the defect first: `BAR0 at phys 0x0`, `STATUS=0x00000000
  (LU=0)`, `using default QEMU MAC`, `link is down`), the QEMU
  `e1000_reset` behavioural core (RXCSUM 0, `RAL/RAH` preloaded with
  the QEMU default MAC `52:54:00:12:34:56` + `RAH.AV`, `STATUS.LU`,
  `CTRL.RST` self-clear), the RX ring ingress (frame DMA + `DD` +
  `RDH++ mod RDLEN/16` + `ICR.RXT0`, gates `RCTL.EN` and
  `RDH == RDT`), and opt-in TAP ingress: `--tap=host:port` (UDP
  datagrams = Ethernet frames at the fixed endpoint `127.0.0.1:8080`,
  the emulator connects; ≤8 datagrams/tick drained from
  `devices_tick`).  Without `--tap=` no PCI function exists at all --
  the historical firmware/K0–K8 and U5 receipts stay byte-stable and
  the guest-visible firmware contract does not move.
- **`tests/test_net.c` 2 vectors** (in `make test-unit`): `ok e1000
  pci/reset contract` (RXCSUM 0, RAL/RAH preload, STATUS.LU, CTRL.RST
  self-clear + preload survives, ICR read-to-clear, and the BAR0
  readback regression vector over `0xCF8/0xCFC` == `0xFEBC0000`) and
  `ok e1000 rx ingress` (RCTL.EN drop gate, DMA `0x6c6c6568`/"hell" LE,
  desc.length + DD, RDH++/RXT0/ICR read-to-clear, ring-full drop with
  `RDT = RDH = 1`).
- **NC (once per phase)**: removing the RX-DMA `mem_write` loop reddens
  the ingress vector -- `RC=134`, `tests/test_net.c:85: Assertion
  'mem_read(&f.m, buf, 4) == 0x6c6c6568' failed`; restored from backup,
  gates re-green.
- **Measured**: `make test` + `make test-sanitize` RC=0; the guest
  lane pair (`--tap=127.0.0.1:8080`, `--max-instr=4500000000`, the U5
  freeze `kernel.elf` sha256 `395e5ec02bf39cc5…` + `initrd.tar`
  sha256 `0581d3a5cb777af0…`) is byte-identical (both tapes sha256
  `82614cd6f896aa9a…`, `cmp` clean) with the N0 gate `[net] using NIC:
  e1000`, plus `BAR0 at phys 0xfebc0000`, `STATUS=0x00000002 (LU=1)`,
  `MAC from RAL/RAH` (the preload contract exercised end-to-end); the
  still-unmodelled TX path is the N1 ledger (`[e1000] TX timeout:
  desc[N].status=0x00 TDH=0 TDT=N+1`, `[dhcp] FAIL: DISCOVER transmit
  failed`, `[net] DHCP failed, using hardcoded IP 10.0.2.15`).
- **Docs**: `docs/plans/NET_PLAN.md` (ground rules, guest/QEMU
  contract, defect ledger D0–D3, N0–N3 phase table), ROADMAP +
  `docs/STATUS.md` sync; doc-sync repair -- `docs/plans/VIDEO_PLAN.md`'s
  status line was stale since 0042 ("in progress — V0 ✅, V1–V3
  planned") and now reads "complete — V0–V3 (plan closed)" to match
  its own phase table.

### Added — patch 0045 (VIDEO V3, mode switching + close-out)

- **Mode state as one machine**: text mode 3 ↔ VBE graphics driven by the
  dispi ENABLE edge, the GPU BAR0 / kload fb publishes, and the VGA graphics
  controller (`0x3CE/0x3CF`, QEMU `gr_mask`; `VGA_GFX_MISC` index 6).  Clearing
  `VGA_GR06_GRAPHICS_MODE` is the guest's mode-3-forcing hardware path --
  stage2's `INT 0x10` (AL=3) mode-set lands the display in text mode 3 there --
  and a VBE client's mode-set lands it in graphics; `ok vga gc graphics/mode-3
  path` pins both sides.
- **Mode-set memory per `NOCLEARMEM`** (QEMU `vbe_ioport_write_data` literal):
  every ENABLED store clears `YRES * vbe_line_offset` of VRAM unless
  `DISPI_NOCLEARMEM`; `ok vga vbe mode-set clear/keep` measures the clear
  boundary (320x200x32) and the keep case.
- **`--vbe=WxHxBPP|off`**: a harness-side VBE client's mode-set through the
  real `0x1CE/0x1CF` io path (the mode-switch lane's guest stand-in), plus
  `tests/integration/test_video_modes.sh` (`make test-video-modes`): the
  mode-switch lane and the V1 `--no-fb` guest lane, each twice -- all six
  artifact pairs (ppm/txt/log) byte-identical, with display assertions per
  mode (gfx: the Limine splash panel `0x10101c` + `0xdcdcdc` on the LFB
  surface; txt: the mode-3 console render).

### Fixed / measured — patch 0045

- NC V3: removing the NOCLEARMEM clear reddens the clear vector (RC=134,
  `test_video.c:258`).  `make test` + `make test-sanitize` green (video
  10/10); the firmware lane and the K0–K8 kernel receipts do not move.

### Added — patch 0044 (VIDEO V2, Bochs VBE dispi)
- Bochs VBE dispi register file on ports `0x1CE` (index select, stored
  verbatim — QEMU has no auto-increment) / `0x1CF` (word data): the
  QEMU `-vga std` contract of `hw/display/bochs-vbe.h` + `vga.c`
  `vbe_ioport_write_data`.  ID `0xB0C0..0xB0C5` accept-list (reset =
  `ID0`, QEMU `vga_reset`), XRES accept `<= 16000 && (val & 7) == 0`,
  YRES `<= 12000`, BPP `0 -> 8` with `{4, 8, 15, 16, 24, 32}`,
  GETCAPS (0x02) reads return the 16000/12000/32 maxima,
  `VIDEO_MEMORY_64K` = vram/64K = 29 (read-only), BANK masked by
  QEMU's `(vram >> 16) - 1` = 28 with `bank_offset = val << 16`,
  `VIRT_WIDTH`/`VIRT_HEIGHT`/offsets with the behavioural
  `vbe_fixup_regs` core (VIRT_WIDTH clamps to XRES; VIRT_HEIGHT
  derives from vram/linesize).
- ENABLE edge semantics: the 0->1 edge resets `VIRT_WIDTH`/offsets and
  switches the display to the LFB; the 1->0 edge resets the bank window
  and restores the machine's previous display (captured at the enable
  edge).  The enable-edge VRAM clear (`NOCLEARMEM`) is V3's scope — V2
  never mutates the LFB.  `tests/test_video.c` 4 -> 8 vectors.

### Fixed / measured — patch 0044
- The guest baseline never touches dispi (measured: no `0x1CE`/`0x1CF`
  users in AuraLite-OS@`0ed0d29` — its GPU path is PCI BAR0 +
  `boot_info.fb`); both U5-era guest lanes reproduce the historical
  tapes byte-for-byte with the era recipe (`--max-instr=4500000000`).
- NC: removing the ID accept-list turns `ok vga dispi id ladder` red
  (abort RC=134 at `io_read(0x1CF)==0xB0C5`).

### Added — patch 0043 (VIDEO V1, VGA text mode 3)
- VGA text mode 3 model (`src/devices.c` VGA section): the 80x25 cell
  buffer is plain guest RAM at `0xB8000` (the mem.c RAM path owns
  everything below `RAM_SIZE`), plus the CRTC index/data pair at
  `0x3D4/0x3D5` (`crtc[256]`, `crtc_idx`) — the guest's own
  `vga_move_cursor` recipe (idx 0x0E/0x0F) round-trips.
- Glyph rendering from the guest's own font: `src/vga_font.h` is
  byte-extracted from AuraLite-OS `psf2_default_font.inc` (256/256
  glyphs identical, PD Linux console VGA 8x16), 8x16 1:1, 80x25 console
  = 640x400 at the top-left of the 800x600 panel, VGA/EGA 16-colour
  attribute palette.  Graphics mode copies `m->fb` exactly — historical
  `framebuffer.ppm` bytes do not move.
- Display routing: reset = text mode 3; GPU BAR0 programmed (the
  firmware's `enable:` CFG write) or `boot_info.fb` published flips the
  display to the LFB; `--no-fb` locks the text console.
- CLI: `--no-fb` (no `boot_info.fb`, no FRAMEBUF e820 entry — the
  guest's `fb.c:215` fallback engages and prints its
  `VGA text 80x25 @ 0xB8000 (BIOS: no linear framebuffer)` receipt),
  `--dump-text=path` (mode-3 console as text), `--dump-fb` renders the
  display (console in text mode).  `tests/test_video.c` 4 vectors in the
  `test-unit` gate.

### Fixed / measured — patch 0043
- Guest `--no-fb --dump-text` lane: the kload receipt `fb none (VGA
  text)` (no FRAMEBUF e820 entry) and the guest's own fallback receipt
  `[fb] VGA text 80x25 @ 0xB8000 (BIOS: no linear framebuffer)`
  (fb.c:215); the dumped console shows the guest's cleared mode-3
  screen (on this frozen baseline the guest's `vga_text_putc` is dead
  code — fb.c:158, static, zero call sites — so console text stays
  serial-only in the fallback; guest-side characteristic, measured).
- Normal guest lanes byte-identical to the U5-era tapes — the full
  stream (log + register dump + last-log reprint) on both shapes with
  the era's recipe (`--max-instr=4500000000`): AURALUSB/normal
  (`uhci1.out`, sha256 e615c39ec0c8…) and AURALEHC/`--no-usb-uhci`
  (`ehci1.out`, sha256 eb778d65a001…) — byte-for-byte.
- NC: reverting the CRTC write handler turns `ok vga crtc cursor pair`
  red (abort RC=134 at `io_read(0x3D5)==0xF5`).

### Added — patch 0041 (USB U5, end-to-end + hardening close-out)
- `--no-usb-uhci` CLI (`machine_t.cfg_no_uhci`): omits the UHCI companion
  (PCI 0:1.2 + I/O window) so the guest's `test_usb_ehci.sh` QEMU lane
  (`-device usb-ehci`, no `-usb`) is reproducible as a machine shape;
  unit vector `ok uhci omitted` pins the open-bus absence and the intact
  EHCI register file. The guest's own probe still prints
  `[uhci] no UHCI controller found` — the historical receipt.
- Both sticks through the guest's MSC path with `/usb` info + a sector
  read typed over the KBC (`run /apps/filesize /usb/sector0.bin`): the
  UHCI companion lane (AURALUSB 8 MiB) and the EHCI high-speed lane
  (AURALEHC 8 MiB), each run twice sequentially with byte-identical
  serial logs. `tests/test_usb.c` 25 -> 26 vectors.

### Fixed / measured — patch 0041
- `test_ahci_matrix.sh` with `MATRIX_DETERMINISM=1` (S5 gate) stays
  green with USB present; every lane pair byte-identical, and the
  AHCI/filesystem receipt lines are byte-identical to the S5-era lane
  logs (the guest's vmdrv PCI-scan line grows the intended U4
  companion entry — recorded in `docs/plans/USB_PLAN.md`).
- Frozen-kernel reproducibility: `SOURCE_DATE_EPOCH=1791484814`
  reproduces the historical `build: Oct 8 2026 18:40:14` banner; the
  92160-byte trimmed initrd (bin/init + apps/filesize) is reconstructed
  from the `tools/mkinitrd.sh` recipe.
- Measured budget: an 8 MiB `/usb/disk.img` read over the UHCI 64-byte
  chunk path costs ~21G instructions (3880/16384 sectors per 8G) —
  media byte-correctness stays pinned by `ok uhci td walk`.

### Added — patch 0040 (USB U4, UHCI root host controller)
- A first-class UHCI companion: PCI 0:1.2 `8086:0x7020` (piix3 identity),
  I/O BAR4 `0xC040` register file in the guest's measured bit map
  (USBSTS.HCHALTED = bit 5), frame list + QH/TD walker on USBCMD.RUN
  (doorbell + cadence sampler), TD completion with the
  `ctrl[10:0] = bytes-1` actual-length contract (0x7FF = 0) and
  IOC->USBINT, control + bulk through a per-controller BOT state over
  the shared device model (`usb_serve()` refactor; the U3 qTD path is
  byte-identical). `tests/test_usb.c` 22 -> 25 vectors.

### Fixed / measured — patch 0040
- Real model bug: the disk-data path did not advance the media offset by
  the consumed bytes — 64-byte UHCI chunks re-read LBA 0 forever (the
  512-byte E32 chunks hid it). `disk_off = lba*512 + st->moved`;
  regression vector `ok uhci td walk` (byte-for-byte sector + CSW
  residue 0). Negative control: reverting the RUN-gating reddens
  `tests/test_usb.c:556` (`[uhci] halted: TD still Active`), abort 134.
- Guest parity run x2 byte-identical: `[uhci] controller at PCI 0:1.2`
  + `I/O base = 0xc040`, stick enumerated UHCI full-speed, MSC binds
  the UHCI instance, `test_usbfs.sh` receipts over 64-byte bulk.

### Added — patch 0039 (USB U3, EHCI register file honesty + control)
- The EHCI model answers a real driver: CAPLENGTH/HCIVERSION/HCSPARAMS/
  HCCPARAMS, USBCMD RUN/stop/HCRESET walk gate with doorbell on USBCMD
  writes only, USBSTS USBINT/IHS/SEmpty/HCHALTED, USBINTR, PORTSC
  power/reset/enable/owner with CSC latching, and per-qTD SETUP control
  transfers (device/config/string descriptors, SET_ADDRESS,
  SET_CONFIGURATION, GET_STATUS, GET_MAX_LUN). `tests/test_usb.c`
  14 -> 22 vectors.

### Fixed / measured — patch 0039
- Guest boot parity with QEMU `run_qemu_usb_msc.sh` receipts
  (`1 high-speed device(s) ready`, `[msc] mass storage candidate`)
  against `AuraLite-OS@0ed0d29`; negative control: an unconditional
  walk reddens `tests/test_usb.c:352` (abort 134).

### Added — patch 0038 (USB U2, SCSI enumeration set)
- The four SCSI commands the AuraLite-OS `msc.c` enumeration sends are
  answered from the attached image over the honest BOT channel: INQUIRY
  (36-byte direct-access/removable descriptor: vendor `AURALITE`,
  product `USB DISK`, revision `1.0 `), READ CAPACITY(10) (last LBA =
  `disk_len/512 − 1`, block length 512), TEST UNIT READY (OK with an
  image, FAILED + sense `2/3A` MEDIUM NOT PRESENT without), and
  REQUEST SENSE (18-byte fixed format `0x70`, consumed on read;
  ILLEGAL REQUEST `5/20` for unsupported opcodes).
- `tests/test_usb.c` grows 7 -> 14 machine vectors (TUR ready/failed,
  sense reported then consumed, INQUIRY bytes, capacity at 1- and
  N-block images, illegal-opcode sense).

### Fixed / measured — patch 0038
- Negative control measured: breaking the capacity math reddens exactly
  `ok read capacity n-block` (assert `r[3] == 7`).

### Added — patch 0037 (USB U1, BOT/CSW correctness)
- Honest Bulk-Only status channel: the CSW carries `dCSWTag` echoed from
  the CBW (the AuraLite-OS `msc.c` validates it), `dCSWDataResidue`
  computed as expected-minus-moved, and `bCSWStatus` distinguishes
  OK / FAILED (unsupported SCSI opcode) / PHASE (malformed CBW).
  Status-phase framing is BOT-shaped: the CSW is the 13-byte IN (the
  sample firmware's status qTD token measures `0x000D0180`), so a data
  phase cut short still settles residue honestly.
- `tests/test_usb.c` grows from the be16 smoke to 7 machine vectors
  (QH/qTD fixtures driven through the doorbell): tag echo, residue on
  short data and on an absent image, FAILED opcode, PHASE bad CBW, and
  the firmware-shaped negative control (tag=1 recipe, receipts stable).
- `devices_done()` frees the PCI device list (`pci_done()`) — LSan
  measured 11232 bytes / 36 allocations leaked per full-device fixture.

### Fixed / measured — patch 0037
- Negative control measured: reverting the tag echo reddens exactly
  `ok csw tag echo` (assert `r32r(c + 4) == tag`, RC=134).

### Added — patch 0035 (STORE S5, large transfers + hardening)
- `--sata-writethrough`: opt-in host write-through -- every guest WRITE
  DMA EXT byte also lands in the host image file at the same offset
  (`ahci_wt_attach`/`ahci_wt_close_all`).  The default policy stays
  copy-on-attach in-memory (S3's pristine-host receipt is asserted by
  both harnesses).  NEW `tests/integration/test_ahci_writethrough.sh`
  proves both policies with the same typed guest token.
- Honest INTx: the AHCI 1.3 interrupt condition ((PxIS & PxIE) on any
  port, or HBA IS nonzero with GHC.IE) drives board line AHCI_IRQ0+ctrl
  (14/15, legacy IDE pairing) through `pic_set_irq()` -- which fans the
  line out to the I/O APIC (CHIPSET H7 board wire).  HBA IS is now a
  live per-port PxIS aggregation.  The guest driver polls with PxIE=0
  and never sets GHC.IE (measured), so no historical receipt changes.
- NEW `tests/integration/test_ahci_large_read.sh`: the QEMU
  `test_ahci_large_read.sh` gate reproduced guest-side -- 16 MiB payload
  (the measured size a bounce leak cannot survive) on a pre-built FAT32
  at LBA 64 behind 64 zero sectors, typed `run /apps/filesize
  /fat/payload.bin`, asserting `FILESIZE /fat/payload.bin 16777216` and
  no reformat / leak-ceiling truncation / read error.
- `tests/integration/test_ahci_matrix.sh` grows `MATRIX_DETERMINISM=1`:
  lanes A, B, C and the unattached lane E re-run sequentially and must
  be byte-identical (the S5 full-matrix determinism gate).

### Fixed / measured — patch 0035
- 18 -> 22 unit vectors: 256-sector (128 KiB) READ and WRITE DMA EXT
  through two 64-KiB PRDTs with `prdbc == 131072` golden bytes (the
  guest's measured bounce-buffer ceiling had no vector), writethrough
  host-file bytes, and the INTx line lifecycle (quiet under the guest's
  polled PxIE=0 contract; assert on unmask; W1C drop; GHC.IE aggregate
  path; LTIM level-follow on deassert).

### Backfill — patches 0031–0033 (STORE S1–S3, recorded here for the
### ledger; shipped with the S0-S3 tree)
- 0031: AHCI HBA registers + disk presence -- ABAR MMIO @0xFEB10000,
  CAP/PI/VS/GHC, per-port register files, `--sata=`/`--sata-portN=` CLI,
  COMRESET path, stop/start handshake (8 vectors).
- 0032: command engine (CL/TBL/CFIS/PRDT walk), READ DMA EXT, IDENTIFY
  DEVICE, TFES error channel, `--keys-at=prompt` testability; 13 vectors.
- 0033: WRITE DMA EXT shares the engine (`ahci_dma_xfer(..., w)`),
  guest `/disk`+`/fat` token round-trips typed via KBC, sequential
  determinism pair byte-identical, `tests/integration/test_ahci_rw.sh`;
  16 vectors.

### Added — patch 0034 (STORE S4, breadth matrix)
- Second AHCI controller (`--ahci2`): PCI function 0:31:3 (8086:2922,
  class 01/06/01) with its own ABAR window at `0xFEB12000` (the guest
  maps 8 KiB per BAR5 -- measured) and its own six port files.  CLI
  `--sata2=` / `--sata2-portN=` attach images to it; an image on that row
  implies the controller.  Placement at 0:31:3 is deliberate: the guest
  class scan is bus-0 dev/func ascending (measured), so the onboard
  0:31:2 keeps the `controller 0` receipt.
- `machine_t` attach rows became `[AHCI_CTRLS][AHCI_MAX_ATTACH]`;
  `ahci_register_ctrl(m, ctrl)` registers per-controller instances
  (`ahci_register()` stays the controller-0 entry point; controller-1
  logs are tagged `[ahci2]`, controller-0 keeps the exact historical
  `[ahci]` spelling).
- `tests/integration/test_ahci_matrix.sh`: the QEMU `test_ahci_matrix.sh`
  lanes A/B/C reproduced guest-side through the emulator's `--keys-at`
  lane, plus an unattached negative-control lane and a sequential
  determinism pair (lane B always; `MATRIX_DETERMINISM=1` for the full
  matrix).

### Fixed / measured — patch 0034
- 15 -> 18 unit vectors: second-window independence (ABAR/ABAR2 do not
  alias, per-controller GHC), port placement through the command engine
  (TFES on a dark port, clean DMA on port 3), cross-controller image
  isolation (READ/WRITE never bleed between rows).

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
