# CPU Features Plan — advertise exactly what the machine does

> Post-UPKEEP audit (2026-10-22). All twelve prior plans are
> closed and upstream `main` (`957c12c`, "M, P, U update") is
> tree-identical to this branch's HEAD (`4db0e4b4…`), so the
> next block was chosen the way the 2026-10-19 audit chose
> XHCI/MSI/PM/UPKEEP: by measuring, in this order, the tree,
> its test contracts, and its consumers (§0). The winner is
> the CPU feature contract — the set of things CPUID and the
> MSR file *say* about the processor versus what the core
> actually *does* — because the measurement found, in one
> small surface, two implemented-but-inverted security
> features, four implemented-but-unadvertised capabilities,
> and one architectural facility (PCID) whose only consumer
> currently has **no emulator lane on earth that can execute
> it** (§1.2). Device-shaped candidates measured in the same
> pass (virtio, AC97, extra NICs, USB HID) are ranked in
> §5 for the next choice.

**Status: CF0–CF5 done ✅ — PLAN CLOSED**

| Phase | Scope | Status | Deliverable |
|---|---|---|---|
| CF0 | Measured baseline: advertised-vs-implemented matrix, MSR census, defect ledger, decisions, phase split | ✅ done | this file (`patches/0099`) |
| CF1 | CR4.SMEP/SMAP: fix the inverted walk guards, gate STAC/CLAC, vector, advertise | ✅ done | `tests/test_tlb.c` vectors (`patches/0100`) |
| CF2 | Advertised-truth bundle: NX, SYSCALL, 1GiB pages, CLFLUSH bits; IA32_PAT reset default + CPUID.PAT | ✅ done | CPUID/MSR vectors (`patches/0101`) |
| CF3 | PCID + INVPCID: CPUID bits, CR4.PCIDE/CR3 legality, INVPCID decode onto the flush-always TLB | ✅ done | `tests/test_tlb.c` vectors (`patches/0102`) |
| CF4 | Validation hardening, measure-gated: CR0/CR4/EFER reserved bits, unknown-MSR policy, MSR-table overflow honesty; POPCNT/RDTSCP inventory resolution | ✅ done | vectors + the MSR NO-GO record (`patches/0103`) |
| CF5 | Close-out: the U5 A20-closed reset-fetch record reproduced or refuted; docs sync | ✅ done | close-out (`patches/0104`) |

Each implementation phase ships one `.patch` under
`patches/` with measured receipts, a regression vector, one
once-per-phase negative control, green gates and the docs
update in the same commit (the standing house rules). CF0
changes documentation only.

## 0. Measurement method and current receipts

Baseline taken at emulator `5f6df06` after UPKEEP U7
(upstream `957c12c` is the same tree). The current code and
its test contracts were inspected directly; the current
AuraLite-OS source was inspected at `74c2b9b` for consumer
use (it is a consumer clue, not a replacement for this
repository's frozen lanes — the frozen guest build is absent
from this sandbox, so its lanes skip here, as in prior
phases). Two ad-hoc instruments were built for this audit
and are **not** part of the tree:

* an SMEP/SMAP harness (the `tests/test_tlb.c` fixture
  pattern: hand-built 4-level tables, CPL 0, one user page
  and one supervisor page) — §1.3's result table;
* an MSR census build (`cpu_get_msr`/`cpu_set_msr` log the
  first touch of each index to stderr) run over the sample
  firmware (3 platform profiles) and both live SeaBIOS ISO
  lanes — §1.2's footprint sets.

Receipts at baseline: `make test` and `make test-sanitize`
green with both live SeaBIOS ISO lanes (U7 close-out, tree
unchanged since), and `make test-unit test-tlb test-ring3`
re-run green for this audit. No production code changed in
CF0; the two stale `docs/STATUS.md` "Current limits"
statements found (§1.5) are corrected in this commit, as is
`ROADMAP.md` ledger drift.

## 1. Measured baseline (CF0)

### 1.1 What CPUID says today

`platform_cpuid` (`src/platform.c:43`) is the whole surface;
`do_cpuid` (`src/cpu.c:846`) patches leaf-1 EBX[31:24] with
the vCPU's initial APIC ID (G6) and masks EDX.9 when the
LAPIC is globally disabled (U4). Per leaf:

* **Leaf 0**: max basic leaf `0x16`, vendor `GenuineIntel`.
* **Leaf 1**: EAX = the five platform signatures
  (0x206A7…0x30678, the firmware's family/model contract);
  EBX = `0x00040800` (CLFLUSH line size 8×8 B in [15:8],
  APIC ID patched per vCPU); ECX = **SSE3 + hypervisor
  only**; EDX = **FPU, TSC, MSR, CX8, APIC, CMOV, MMX,
  FXSR, SSE, SSE2**. Absent among others: PSE (3), PAE (6),
  MTRR (12), PGE (13), **PAT (16)**, CLFLUSH (19); ECX
  PCID (17), x2APIC (21), POPCNT (23), TSC-deadline (24).
* **Leaf 7.0**: EBX = **ERMS (9) only** ("we do fast REP
  MOVSB/STOSB"). SMEP (7), INVPCID (10), SMAP (20) all 0.
* **Leaf 0x80000001**: EDX = **long mode (29) only** —
  SYSCALL (11), NX (20), 1GiB pages (26) all 0.
* Leaves 2/4 (cache), brand string, 0x80000006/08
  (40-bit phys / 48-bit virt), hypervisor leaf
  `0x40000000` ("AuraLite HV"); every other leaf reads 0,
  including 0x15/0x16 (TSC frequency = unknown, legal).

### 1.2 What the consumers actually probe

**AuraLite-OS `74c2b9b`** (the north star) makes exactly
six CPUID feature decisions, all runtime-gated with a
boot-log receipt (`kernel/arch/x86_64/diagnostics.c` prints
`[cpu] features: pat=_ pcid=_ invpcid=_ erms=_` and the raw
IA32_PAT every boot):

* **PAT** — `paging.c` rewrites MSR `0x277` PA4=WC on every
  CPU *iff* CPUID.1:EDX.16, and prints the readback;
* **PCID** — sets CR4.PCIDE *iff* CPUID.1:ECX.17, with a
  "hash-slot alloc, CR3-reload+generation fallback, no
  invpcid" runtime; `tlb_shootdown.c` holds the INVPCID
  path behind CPUID.7.0:EBX.10;
* **SMEP/SMAP** — sets CR4 bits and `cpu_smap_is_active`
  *iff* CPUID.7.0:EBX.7/.20 (`paging.c:33,120`);
* **ERMS** — calibrates `string_fast.c` on EBX.9.

Its own HW_PLAN measured the same four bits on every lane
available to it: qemu64 says `pat=1 pcid=0 invpcid=0
erms=0`; `-cpu max` (TCG) says `erms=1` and "STILL pcid=0
invpcid=0 (`+pcid` is refused by TCG with a warning)"; the
maintainer's WHPX machine says `pcid=1 invpcid=0`. HW H4
was therefore re-scoped into a *deferral protocol* — its
words: "no QEMU configuration here can execute PCID at
all". On this emulator today the guest reads `pat=0 pcid=0
invpcid=0 erms=1`, enables neither SMEP nor SMAP, and never
touches MSR 0x277. The guest's complete MSR footprint
(source census) is EFER, STAR/LSTAR/FMASK, FS/GS/KGS bases
and the gated PAT write — no MTRRs, no TSC-deadline.

**The sample firmware** branches on leaf-1 family/model
(the five `platform_t` signatures are its measured
contract, untouched by this plan) and — MSR census,
firmware boot, three profiles — *programs MTRRs*:
{ RD 0x1B, RD 0x2FF (MTRRdefType), RD EFER, WR 0x200,
WR 0x201 } (Bay Trail skips the MTRR writes). Those writes
land in the sparse MSR table (§1.3) and the reads get the
stored value back: the forgiving MSR lane is load-bearing
for the firmware lanes.

**Stock SeaBIOS** (both ISO lanes, census build) touches
exactly one MSR index per boot (0x1B, our own reset
bookkeeping included): its MTRR programming is CPUID-gated
off **by our own leaf-1 EDX.12 = 0**. Advertising MTRR
would inherit a programming contract we have not measured;
this plan does not propose it (§2).

### 1.3 What the core actually does

* **SMEP/SMAP enforcement exists and is inverted.** The
  walk's `check:` block (`src/cpu.c:210`, upstream
  `15166693`, untouched by every plan since) places both
  guards inside `if (!eff_us)` — the supervisor-page
  branch. SDM faults supervisor access to a **user** page
  (`eff_us == 1`). Harness repro (CPL 0; U/S=0 identity
  2 MiB + one U/S=1 page; this audit's instrument):

  | case | SDM | today |
  |---|---|---|
  | SMEP: exec supervisor page, read supervisor data | OK | **#PF** (fetch, `err=0x11`) |
  | SMEP: exec user page | #PF | **OK** |
  | SMAP, AC=0: read user data | #PF | **OK** |
  | SMAP, after STAC: read user data | OK | OK |
  | SMAP, AC=0: read supervisor data | OK | **#PF** |

  Nothing noticed for two compounding reasons: CPUID says
  0, so no measured consumer ever sets the CR4 bits; and
  no test in `tests/` mentions SMEP or SMAP (the K4-era
  STAC/CLAC decode at `src/cpu.c:3740` executes at any CPL
  with no CR4.SMAP gate — also unvectored). A kernel that
  *did* trust CR4 would #PF on its own text in the first
  instruction after enabling either feature.
* **NX** is enforced by the walk with EFER.NXE
  (`src/cpu.c:218` + `page_fault` I/D err bit) — works, is
  used by every long-mode lane, and is not advertised.
* **1 GiB pages** are walked (`pdpte & PS`,
  `src/cpu.c:193`); SYSCALL/SYSRET are the guest's ring-3
  backbone since K4; **CLFLUSH** decodes to a correct
  no-op on a cache-free machine (`src/cpu.c:3588`) while
  leaf-1 EBX already advertises its line size — an
  inconsistent pair. None of the three bits is set.
* **PAT**: the MSR file is a 64-entry sparse table in
  `cpu_t` (`src/cpu.h:34`, `src/cpu.c:64`); any WRMSR
  stores, any RDMSR returns the stored value or 0 — so
  MSR 0x277 already "works" as storage, but its reset
  value is 0 where SDM Table 9-1 (and the guest's H0
  receipt) require `0x0007040600070406`. On a machine
  with no cache model, memory types are performance
  attributes only — the ERMS precedent: storage + reset
  value + legality is the complete honest functional
  model.
* **PCID**: the walk masks CR3's low 12 bits
  (`src/cpu.c:176`) and the P2 TLB is success-only with a
  generation flush on *every* guest/DMA RAM store, so
  PCID-tagged translations are semantically inert here by
  construction. What's missing is purely the contract:
  CPUID bits, CR4.PCIDE legality, CR3[63] and PCID-field
  rules, and an INVPCID decode (today 0F 38 82 has no
  case). INVLPG would need its "current PCID" scope
  narrowed in text only — the flushes already cover it.
* **Control-register writes are unvalidated** (MOV CRn,
  `src/cpu.c:3705`): CR0/CR3/CR4 store any value verbatim;
  reserved bits neither #GP nor mask. EFER via WRMSR is
  likewise stored raw (`c->efer = val`). No measured
  consumer writes garbage — the guest CPUID-gates every
  bit it sets — but "CR4 accepts anything" is why the
  inverted SMEP could never produce a canary failure
  either.
* **The unknown-MSR lane is permissive by recorded
  decision** (U4: RDMSR unknown → 0, WRMSR → sparse store,
  rejections logged not faulted). Its one sharp edge is
  silent: the 65th distinct MSR index is *dropped without
  a word* (`cpu_set_msr` count guard), after which reads
  return 0 — a quiet correctness cliff no lane trips
  today (census footprints above are ≤ 5 indices).

### 1.4 The matrix

| Feature | CPUID says | Core does | Verdict |
|---|---|---|---|
| ERMS (7.0:EBX.9) | 1 | REP string ops correct (speed is a hint) | honest |
| SMEP (7.0:EBX.7) | 0 | guard present, **inverted**, unvectored | **D1 → CF1** |
| SMAP (7.0:EBX.20) | 0 | guard present, **inverted**; STAC/CLAC ungated | **D1 → CF1** |
| PAT (1:EDX.16) + MSR 277 | 0 | storage yes; **reset value 0 ≠ SDM** | **D2 → CF2** |
| NX (8·1:EDX.20) | 0 | enforced | **D3 → CF2** |
| SYSCALL (8·1:EDX.11) | 0 | implemented (K4) | **D3 → CF2** |
| 1 GiB pages (8·1:EDX.26) | 0 | walked | **D3 → CF2** |
| CLFLUSH (1:EDX.19) | 0 (but EBX line size = 64 B) | correct NOP | **D3 → CF2** |
| PCID (1:ECX.17) | 0 | plumbing inert-ready; legality absent | **D4 → CF3** |
| INVPCID (7.0:EBX.10) | 0 | not decoded | **D4 → CF3** |
| CR0/CR4/EFER reserved bits | — | stored verbatim, no #GP | **D5 → CF4** |
| unknown MSR | — | RD 0 / sparse WR; **silent 64-slot drop** | **D5 → CF4** |
| POPCNT (1:ECX.23) | 0 | no execution case found (disasm knows it) | D6 → CF4 inventory |
| RDTSCP (8·1:EDX.27) | 0 | unresolved by grep audit | D6 → CF4 inventory |
| x2APIC, TSC-deadline, MTRR | 0 | absent (LAPIC header says so for x2APIC) | honest; §5 |

### 1.5 Documentation debt found in the same pass

`docs/STATUS.md` "Current limits" still claims the PM
block "models only the Timer register… no power-management
transition is claimed" (false since PM P0–P3, `0087–0090`)
and that "x87 registers are not part of the gdbstub
`g`-packet contract" (false since UPKEEP U3, `0094`).
`ROADMAP.md`'s plan ledger lacks PM_PLAN and UPKEEP_PLAN
rows, its audit inventory still names upstream `1e7ff03`
and "the emulator has EHCI" for USB (all three controllers
since XHCI X5). All corrected in this commit.

## 2. Architecture decisions (CF0)

1. **Advertise nothing until it is pinned.** Each CPUID
   bit flips in the same phase that vectors the semantics
   behind it. Flipping a bit is a guest-visible contract
   change: the phase must name every pinned lane whose
   receipts read CPUID and record any receipt delta
   explicitly (expected: the guest's `[cpu] features:` line
   and feature-enable logs *gain* content; the sample
   firmware branches only on family/model, which this plan
   never touches; SeaBIOS gates MTRR setup on EDX.12,
   which stays 0 — §1.2's census is the evidence the flip
   set in CF2/CF3 keeps SeaBIOS on its current path, and
   both ISO lanes remain in the gate).
2. **Memory types are storage here.** PAT (and the MTRR
   sparse store the firmware already uses) have no
   observable semantics on a cache-free in-order
   interpreter beyond readback values and write legality.
   Modeling exactly that, reset values included, is
   honest in the same way ERMS-as-a-bit already is. We
   still do **not** advertise MTRR: that bit changes
   *SeaBIOS's* behavior (§1.2), a contract nobody has
   measured a need for.
3. **PCID collapses onto flush-always.** The P2 policy
   (success-only TLB, generation bump on every RAM store)
   makes per-PCID retention unobservable; CF3 implements
   the *legal* surface (enabling rules, CR3[63] #GP,
   INVPCID descriptor checks) and lets every invalidation
   type flush — the no-flush load hint honored as a hint.
   What the guest gains is real: its PCID allocator and
   generation shootdown become executable somewhere other
   than one WHPX machine.
4. **Tighten nothing blind.** CF4's validation work is
   gated on the census: the firmware's MTRR writes and the
   U4 recorded permissive decision mean "#GP on unknown
   MSR" ships only if firmware + SeaBIOS + guest source
   footprints all survive it in a scratch experiment
   first; an evidenced NO-GO (PERF P4 precedent) is an
   acceptable CF4 outcome. The silent MSR-table drop,
   however, gets at minimum a log line — silence is the
   one indefensible part.
5. **No snapshot-layout change is planned.** PAT seeds a
   slot in the existing sparse MSR table; no `cpu_t` or
   `machine_t` field is needed by any phase as scoped.
   If a phase discovers otherwise it takes the U1
   treatment (explicit incompatible-snapshot note) or
   stops. Snapshot lanes are in every phase's gate
   regardless — the CPUID words are not serialized, but
   MSR contents are (`msr[]` rides the cpu dump), so the
   CF2 reset seeding must be byte-stable across
   save/load.

## 3. Phase acceptance

### CF1 — SMEP/SMAP made true

Fix the walk guards to SDM (supervisor fetch of a U/S=1
page faults under CR4.SMEP; supervisor data access to a
U/S=1 page faults under CR4.SMAP with EFLAGS.AC=0), with
the architected error codes; pin down the surrounding
rules from SDM before coding (STAC/CLAC #UD conditions
and CPL behavior, AC handling across interrupt/exception
delivery and IRET, supervisor implicit accesses —
descriptor/table reads under `desc_sv`, page-walk accesses
themselves, which are supervisor-mode accesses to paging
structures and not SMAP subjects). Vectors in
`tests/test_tlb.c` covering this audit's five repro cases
plus CPL3-unchanged and implicit-access cases; only then
set EBX.7/EBX.20. NC: restore the inverted block — the
repro cases redden. Guest receipt change to record:
`SMEP enabled` / `SMAP enabled` lines and
`cpu_smap_is_active` copy paths light up (frozen lane
runs where available).

Closed. The guards moved to `eff_us && !user_cpl`
(supervisor accessing a user page), where `user_cpl`
already folds in K4's desc_sv rule — implicit descriptor
accesses take the same SMAP gate as explicit kernel ones
(the QEMU kernel-MMU-index shape; page-walk reads are
physical and never reach the check). The P2 TLB needed no
change: its key already contains whole CR4, CPL, AC,
desc_sv and mode, so no warm success can outrank the new
rules — vector 5 proves it across a STAC→CLAC toggle.
Delivery now clears EFLAGS.AC alongside IF (the pushed
frame preserves the old bit for IRET), and `page_fault`
sets the I/D error bit for SMEP regardless of NXE (the
only two nx_hit call sites are the NX-exec and SMEP
branches, so nothing else moved). Reading the STAC site
surfaced a second, worse latent defect that advertising
SMAP would have armed: STAC/CLAC had no `break` and fell
through into decode_modrm + the ext==1 SIDT store at
linear address 0 (the register form decodes addr=0) —
every STAC ever executed would have overwritten the
guest's first bytes with the IDT pseudo-descriptor,
invisible for the same reason as the inversion. Both
sites now end the case; CPL>0 takes the delivered #UD
(SDM; the WBINVD #GP(0) site is the local delivery
idiom); the leaf-7 word reads SMEP|SMAP|ERMS, pinned in
`test_cpuid_leaves`. Nine vectors in
`tests/test_tlb.c::smep_smap` (own supervisor-mapped
fixture) cover the CF0 repro table in house form plus
the linear-0 fallthrough canary, the CPL3 #UD and AC
across delivery. NC against `86539e4`: the 11 existing
P2 vectors pass, then the first CF1 case aborts in
`smstep` — the inverted walk delivers its bogus #PF on a
supervisor fetch; test-unit's NC aborts on the leaf-7
assertion. Gates: `make test` and `make test-sanitize`
green with both live SeaBIOS ISO lanes; test-unit,
test-tlb, test-ring3 green; no firmware receipt delta
(family/model words untouched; SeaBIOS still reads
EDX.12=0 and skips MTRR setup). Guest-side, when its
lanes next run: the `SMEP enabled`/`SMAP enabled`
receipts appear and the STAC/CLAC copy paths go live —
the first consumers these guards have ever had.

### CF2 — the advertised-truth bundle

Set, each with its pinning vector or an existing-lane
citation re-measured in-phase: NX (80000001h EDX.20),
SYSCALL (EDX.11), 1 GiB pages (EDX.26), CLFLUSH
(1:EDX.19; the EBX line size stops being a contradiction).
IA32_PAT: seed the SDM reset default `0x0007040600070406`
per vCPU at INIT/reset seeding (U5's
`cpu_init_reset` lane), set CPUID.1:EDX.16, and vector the
guest's exact sequence (read, RMW PA4=WC, readback) plus
snapshot byte-stability of the seeded slot. NC: unseed /
unset → vector red. Family/model EAX untouched.

**Shipped (`0101`).** As built: leaf-1 EDX = `0x07898331`
(+PAT+CLFLUSH), 80000001h EDX = `0x24100800` (LM|1GiB|NX|
SYSCALL), both pinned in `test_cpuid_leaves`. One measured
correction to the plan above: the seed lives in `cpu_reset`
(RESET semantics, deterministic slot 1 after APICBASE), not
`cpu_init_reset` — the INIT lane already preserves the sparse
table across INIT wholesale, which matches silicon (PAT is a
RESET-reset MSR; INIT# leaves MSRs alone), so no INIT-side
logic was needed and the vector pins INIT-preserves /
RESET-re-seeds. `test_pat_msr` runs the guest's exact
RDMSR→RMW→WRMSR→readback; `cf2_pat_cases` pins default and
guest values across the snapshot fixed point — no format
change (the seed enters the v1 cpu section as plain sparse
state). NC on the 0100 tree (+ header constants so the tests
compile, no logic): test-unit aborts on the leaf-1 EDX
assertion; test-snap U4 cases stay green and exactly the two
default-value checks fail — the guest-reprogrammed case passed
on the parent too, because storage already worked; only the
reset default was the defect. Gates: `make test` + sanitize
green incl. both SeaBIOS ISO lanes; firmware/SeaBIOS MSR
census unchanged (EDX.12 still 0, the MTRR path stays closed).

### CF3 — PCID + INVPCID

CPUID.1:ECX.17 and 7.0:EBX.10 after the legality surface
exists: setting CR4.PCIDE only in IA-32e mode with
CR3[11:0]==0 (else #GP(0)); MOV CR3 with bit 63 set #GPs
while PCIDE=0 and is the no-invalidate hint while
PCIDE=1; the PCID field is stored/echoed by MOV-from-CR3;
INVLPG semantics under PCID restated (global flush still
covers); INVPCID decoded with its four types, descriptor
reserved/PCID-range checks raising #GP, each type
flushing (types 1–3 global entries included per SDM).
Vectors for every #GP case and for a two-PCID
address-space switch driven the guest's way (CR3 reload
with PCID tags). NC: PCIDE acceptance removed → vectors
red; bit flips alone (without legality) must also redden
a vector, which is the advertise-nothing-until-pinned
rule made executable.

**Shipped (`0102`).** As built: leaf-1 ECX = `0x80020001`,
leaf-7 EBX gains INVPCID (deliberate pin change); MOV-CR
gains exactly the two PCIDE rules above plus the bit-63
hint/reserved split; `66 0F 38 82` decodes with the checks
in KVM's order (66 → memory-form → CPL → type → descriptor
→ reserved/PCID/canonical), every type landing on the one
conservative flush. Three measured corrections to the CF0-era
text above. (1) "types 1–3 global entries included" was
imprecise: only type 2 *must* invalidate globals; types 0/1/3
exclude them but carry explicit "may invalidate more"
clauses — the flush-always landing rides on those clauses,
recorded at the site. (2) PCIDE=0 does **not** #UD INVPCID:
the SDM narrows it (types 0/1 must name PCID 000H, types 2/3
run) — implemented and vectored as narrowed. (3) The
reference implementation is absent, not lax: QEMU refuses
`+pcid` under TCG and lists INVPCID as missing in its
decoder, so the SDM text was the sole authority (verified by
source clone in-phase). LOCK-prefixed INVPCID inherits the
decoder-wide LOCK swallow (the STAC precedent). Deferred to
CF4's inventory, all MOV-CR family and measure-gated there:
the CR3[62:MAXPHYADDR] band, compat-mode clear-CR0.PG-while-
PCIDE, and the PCIDE 1→0-in-IA-32e edge (absent from the
instruction's exception tables — neither permitted nor
forbidden there, so permissive stands until measured).
13 vectors in `tests/test_tlb.c::pcid`; two-form NC on the
0101 tree: tests-only aborts at CF3 case 1 after 20 prior
oks (disasm oracle red alone), and tests+bits-only aborts
identically — advertise-without-legality cannot pass. Gates
green incl. both SeaBIOS ISO lanes; no snapshot change
(PCID rides CR3).

### CF4 — validation, measure-gated

CR4 (and CR0) reserved-bit masks derived from what the
machine now advertises, #GP(0) on violation; EFER
reserved bits likewise. The unknown-MSR question is
decided by experiment, not taste: a scratch strict-mode
run over firmware (5 profiles), both SeaBIOS ISO lanes
and the unit suite decides between (a) keep permissive +
document at the site, (b) whitelist the censused
architectural set + #GP outside it. The 64-slot silent
drop gains at least a one-line log; a table-growth
decision needs snapshot-layout care per §2.5. POPCNT and
RDTSCP inventory is resolved one way or the other
(implement + advertise, or leave #UD/0 with the grep
verdict replaced by a vector).

**Shipped (`0103`).** The experiment ran as a scratch
log-only-instrumented build (reverted afterwards) counting
would-fire events per candidate: full `make test` for the
mask set, plus a second sweep (snapshot 5-profile + ring3)
for the PG-clear/CPL clauses. Raw results:

| Candidate policy | Census (violations in all lanes) | Verdict |
|---|---|---|
| CR4 mask 0x3207FF (base 0–10 + PCIDE/SMEP/SMAP) → #GP | 0 writes outside | **GO** |
| CR0[63:32] / PG&!PE / NW&!CD → #GP | 0 | **GO** |
| CR0 PG-clear in IA-32e (64-bit, or compat+PCIDE) → #GP | 0 | **GO** (closes the CF3 deferral) |
| CR3[62:MAXPHYADDR=40] → #GP | 0 | **GO** |
| EFER outside {SCE,LME,LMA,NXE} → #GP | writes: 0x100, 0xD00 only | **GO**, LMA write-preserved so RMW can neither fault nor desync |
| MOV CRn / RDMSR / WRMSR at CPL≠0 → #GP | 0 — and the checks **did not exist at all** (census find) | **GO** (in-phase find; ring 3 could rewrite CR4/MSRs before) |
| unknown-MSR whitelist → #GP | RDMSR miss: exactly {0x2FF}; WRMSR set {1B, 200, 201, 277, 2FF, EFER, FS_BASE} | **NO-GO** (a) keep permissive + document: firmware's reset-stage CAR code RDMSRs 0x2FF unconditionally before any IDT and programs variable MTRRs through sparse storage; a #GP there bricks the primary workload, and every other consumer probes via CPUID. Pinned permissive by vectors in `test_cf4_units` |
| grow the 64-slot MSR table | drops: 0 in every lane | **NO-GO**, log only: the 65th distinct write is now `mlog`-ged by name; table stays 64, snapshot layout untouched |
| PCIDE 1→0 in IA-32e | absent from the MOV-CR exception tables; 0 lane usage | permissive stands (not forbidden by the architected lists; no measurement argues otherwise) |
| CR0 low reserved bits store-masking | silicon "ignores" them; nothing here consumes them | verbatim stands = functionally ignored |

POPCNT (F3 0F B8; bare 0F B8 keeps the house #UD, JMPE is
not x86) and RDTSCP (0F 01 F9 + IA32_TSC_AUX sparse storage;
pre-CF4 it executed silently as an INVLPG flush — noted as
the second "worse than absent" find of the phase) are
implemented and advertised (leaf-1 ECX.23, 80000001h EDX.27;
both pins deliberately moved). Neither new bit claims more
than built: CR4.TSD gating of RDTSC/RDTSCP remains
unimplemented on both, identically. 11 vectors in
`cf4_validate` + `test_cf4_units`; NC on the 0102 tree: 33
prior walk vectors pass, abort at CF4 case 1; unit suite
aborts on the RDTSCP pin; full `make test` + sanitize green
with enforcement live (the census prediction held — no lane
trips a new #GP). One side observation for the backlog:
compat-mode exceptions deliver **32-bit frames** in this
core while silicon frames IA-32e exceptions 64-bit always —
invisible to every measured lane (all ring-3 consumers run
L=1), recorded here for whoever hardens the compat lane.

### CF5 — close-out + the A20 record

UPKEEP §5 left one CPU-shaped open record: at U5 a
reset-vector fetch with the A20 gate closed did not
follow the expected bus alias (`#UD group5 ext=7` in
debug). CF5 reproduces it in a harness first; if real,
it is fixed here with its vector (fetch-path A20 masking
is this plan's home turf), if not reproducible the record
is refuted with the attempt documented. Then the usual
close-out: STATUS/ROADMAP sync, plan §close.

**Shipped (`0104`) — reproduced, real, fixed.** The harness
repro is exact: close the gate at port 92h, read
`0xFFFFFFF0` — the H5 bus rule forces bit20 low
(`0xFFEFFFF0`), and the top-of-4G firmware decode covered
only `0xFFFE0000..0xFFFFFFFF`, so the masked fetch fell
one megabyte short of flash and read open bus (`0xFF` →
`FF /7` → the recorded `#UD group5 ext=7`). The mask
itself is not the defect: it is the pinned H5 contract
(every odd megabyte aliases, the wired-AND behaviour, and
its lanes assert it at 3 MiB too), so the fix landed on
the decode side — `ROM_TOPOF4G_BASE` now starts the
firmware range at the top **2 MiB** with the 128 KiB
image tiled by the existing modulo indexing (the
ICH-style boot-block decode span; both bases are
128 KiB-aligned, so every previously-decoded byte keeps
its slot). The window's A20 image now lands in flash,
INIT# under a shut gate runs the ROM stub, and the
write-side read-only guard widened with the same macro.
One fixture line in `test_chipset.c` (the old base-value
ternary) was flattened to the base-invariant `0x1FFF0`.
NC on the 0103 tree: the three H5 A20 vectors pass, the
new one aborts on its fetch assertion. The UPKEEP §5
record is closed at its source; the UPKEEP plan's other
two deliberate records (bare-SYN restart, gdbstub
authentication) stay open for their own future plans, as
does this plan's §5 backlog (virtio first) and the CF4
compat-frame observation.

**§close.** The contract the plan set out to write —
advertise exactly what the machine does — now holds in
both directions across the measured surface: the
implemented-but-silent (CF2, CF4/D6), the
inverted-then-made-true (CF1), the previously-absent
(CF3), and the validation boundary (CF4, with the one
measured NO-GO documented at its site instead of
enforced into a bricked primary workload). The two
recurring shapes of latent defect found on the way are
worth the next plan's attention as a class: code that
was unreachable while unadvertised (SMEP/SMAP inversion,
the STAC fallthrough, the INVLPG-aliased RDTSCP, the
missing CPL gates), and decode/state edges visible only
from a deliberately-awkward state (closed-A20 INIT#).
Still honestly zero and unbuilt, left for §5 or later
plans: x2APIC, TSC-deadline, MTRRs (storage rides the
permissive MSR lane; the CPUID bit stays 0 because no
caching semantics exist), XSAVE/AVX state, and the rest
of the ranked device backlog.

## 4. Gates

Per phase: its named suite (`test-tlb`, `test-unit`,
`test-ring3`, `test-snapshot` lanes as applicable), one
behavioural negative control, `make test`,
`make test-sanitize`, and both SeaBIOS ISO lanes with the
stock BIOS actually present. CPUID-touching phases
additionally re-run the five-platform firmware smoke
(inside `make test`) with receipt deltas recorded, never
assumed away. CF0 is docs-only; its receipts are the
baseline suites in §0 plus clean patch checks.

## 5. Measured backlog beyond this plan (ranked)

For the next "what's next", in the order this audit's
evidence supports:

1. **virtio-pci (modern) block**, then net: guest drivers
   exist and are modern-1.0 (vendor caps, common config —
   `drivers/virtio_blk/virtio_blk.c`, `drivers/virtio_net/`,
   `drivers/virtio/`), probed *after* AHCI
   (`kernel/kernel.c:449` vs `:462`), so AHCI stays
   primary — a STORE/NET-shaped plan with a shared
   virtqueue core.
2. **Responder bare-SYN tuple restart** (UPKEEP §5):
   RFC 9293 wants a challenge ACK; one `tools/` file plus
   the U6 vectors' home.
3. **AC97 audio** (`drivers/audio/ac97.c` + `pcspkr.c`):
   the PC-speaker register plumbing (PIT ch2 + port 61)
   exists since H2; AC97 needs a host-output design
   decision (this project's GUI-first rule applies) before
   any device model.
4. **Alternate NICs** — guest ships `rtl8139`, `r8169`,
   `vmxnet3`, `e1000e` drivers; our 82540EM e1000 is the
   guest's measured primary (NET plan), so these are
   interop breadth, not function.
5. **USB HID** (keyboard/mouse): STATUS records "the
   machine has no HID model on any bus"; guest input is
   PS/2-first, so demand is unmeasured.
6. **gdbstub breakpoints are linear** (STATUS U3 side
   finding) — a tooling-truth item, small.
7. **x2APIC**: consistently absent from both CPUID and
   the LAPIC; no consumer pressure measured (guest uses
   xAPIC MMIO throughout).
