# PM Plan — a real ACPI power-management surface

> Audit 2026-10-19 candidate #3 (after XHCI and MSI, both complete).
> Companion to `ROADMAP.md` §"Next-block candidates", to COMPAT G5
> (`src/pm.{h,c}` — the Timer block this plan completes) and
> KERNEL-BOOT K3 (`src/acpi.c` — RSDP/MADT only today).

**Status: ✅ COMPLETE (P0–P3, patches 0087–0090)**

| Phase | Scope | Status | Deliverable |
|---|---|---|---|
| P0 | Measured baseline: the current PM/ACPI surface, who consumes PM, the defect ledger, decisions | ✅ done | this file (`patches/0087`) |
| P1 | PM1 register block: PM1_STS/EN (W1C, TMR_STS overflow), PM1_CNT (SCI_EN store, sleep decode), SCI line, S5 power-off | ✅ done | `tests/test_pm.c` vectors (`patches/0088`) |
| P2 | Tables + GPE: FADT + minimal DSDT (\_S5) on the kernel lane, GPE0 STS/EN registers, SCI aggregate; SMI_CMD stays an honest no-SMI | ✅ done | table + GPE vectors (`patches/0089`) |
| P3 | S1 + close-out: a deterministic, snapshot-compatible suspend state; wake sources; docs | ✅ done | zero-duration S1 + close-out (`patches/0090`) |

Each phase ships one `.patch` under `patches/` with measured receipts, a
regression vector, one once-per-phase negative control, green gates and
the docs update in the same commit (the `ROADMAP.md` standing rules).

## 1. Measured baseline (P0)

### 1a. Our machine today

- **Tables** (`src/acpi.c`, 117 lines): RSDP/RSDT/XSDT/MADT only, built
  for the kernel lane. **There is no FADT and no DSDT anywhere** — a
  guest that wants PM from ACPI on that lane finds nothing to parse.
  On the SeaBIOS lane the firmware publishes its own FADT, so guests
  there do find PM registers (at SeaBIOS' PMBASE, 0xB000).
- **PM block** (`src/pm.c`, 100 lines, COMPAT G5): a 0x40-byte I/O block
  at PMBASE (default 0xB000, relocated by the LPC cfg hook like on
  Q35). Exactly one register lives: the 24-bit PM Timer at +0x08,
  deterministic from `vtime_instr` at 3×PIT (3.579545 MHz). Everything
  else reads 0 and swallows writes — the file says so itself: "PM1_STS
  would be W1C and PM1_CNT a sleep transition, neither of which this
  behavioural model implements."
- **Interrupt**: nobody drives SCI (the ACPI IRQ, conventionally IRQ 9;
  the line is free — in use today: 0,1,8,10,11,14,15).
- **State transitions**: reset exists (port 0xCF9 → `machine_reset`);
  *power-off does not* — `m->stop_requested` is set only by the
  debugger. A guest writing SLP_TYP(S5)+SLP_EN to PM1_CNT today is
  writing into the void.
- Measurements of what the block must contain are the ACPI spec's
  fixed register map against the one anchor we already match: ICH9
  PMBASE layout (PM1_STS +0x00/2, PM1_EN +0x02/2, PM1_CNT +0x04/2,
  PM_TMR +0x08/4, GPE0_STS +0x20/4, GPE0_EN +0x24/4 on ICH9) — the
  block is already 0x40 bytes, so all of it fits where it sits.

### 1b. The consumer side

- **SeaBIOS** uses the PM Timer (G5's whole reason to exist) and
  publishes the FADT guests then trust: any ACPI OS on that lane
  drives PM1_CNT for power-off and PM1_STS/EN + SCI for events. Today
  a Linux poweroff hangs after "Power down" — the SLP write is
  swallowed (D1 below).
- **AuraLite-OS** (the north star) consumes no PM today either
  (measured: its ACPI use is RSDP→MADT enumeration for SMP in
  `boot/bios/stage2/acpi.inc`; no FADT/PM/sleep code in the tree). As
  with MSI, the audience is every other guest plus surface honesty.
- `tests/test_pm.c` exists (in `test-unit`) pinning the Timer — the
  P1 vectors extend it.

### 1c. Defect ledger

| # | Defect (measured) | Consequence | Phase |
|---|-------------------|-------------|-------|
| D1 | PM1_CNT writes swallowed; no sleep decode | ACPI power-off never stops the machine | ~~P1~~ **resolved in P1** (SLP_TYP=7+SLP_EN → `stop_requested`) |
| D2 | PM1_STS/PM1_EN dead: TMR_STS never sets on 24-bit overflow, no W1C; no SCI source | timer-overflow events and event enables are fiction-free zeros — guests polling STS spin | ~~P1~~ **resolved in P1** (W1C STS, stored EN, TMR_STS on wrap, level SCI on IRQ9) |
| D3 | GPE0 block reads zero; no SCI aggregate | general-purpose events cannot exist | ~~P2~~ **resolved in P2** (GPE0 STS/EN live at +0x20/+0x24, joined to SCI; no sources yet) |
| D4 | no FADT/DSDT on the kernel lane | PM is undiscoverable except via SeaBIOS' tables | ~~P2~~ **resolved in P2** (FACP rev 4 + DSDT \\_S5 in the kernel-lane set, listed in RSDT/XSDT) |
| D5 | no S-state machine at all (S1 included) | suspend is unrepresentable | ~~P3~~ **resolved in P3** (zero-duration S1: WAK_STS latched; §2c fork) |

## 2. Architecture decisions (P0)

- **S5 is `stop_requested`**: SLP_EN with the S5 type stops the run
  deterministically at the next loop boundary (the debugger's stop
  path, plus a `[pm] S5 power-off` log line and a distinct reason a
  test can assert). No new shutdown machinery.
- **Sleep types are table-driven, not guessed**: the encoding an OS
  writes comes from _S5 (SeaBIOS' DSDT on that lane; ours on the
  kernel lane from P2). The register layer accepts the Q35/PIIX4
  conventional encoding (SLP_TYP 7 = S5, 0/1 = S0/S1 family) and P2's
  DSDT publishes exactly what P1 decodes — the two phases pin each
  other. Unknown types log and do nothing.
- **SCI is a level like the rest of the fleet**: raised while
  (PM1_STS & PM1_EN) or (GPE0_STS & GPE0_EN) is nonzero and SCI_EN is
  set, on IRQ 9 through `pic_set_irq` (the AHCI/XHCI convention),
  recomputed at every write that can change the expression.
- **TMR_STS on the real overflow**: set when the 24-bit counter wraps
  in `pm_advance` (it already knows the delta), W1C clear — guests get
  the ~4.7 s periodic event real hardware gives them.
- **SMI stays absent, honestly**: FADT.SMI_CMD = 0 and no SMI
  machinery is fabricated (there is no SMM in this machine; the audit
  allowed "an honest no-op or a measured vector" — the no-op is the
  spec-legal answer when SMI_CMD is 0).
- **S1 (P3) has an open design fork, recorded now**: real parking
  (vCPUs halted, devices ticking, wake on RTC/keyboard/PM event) vs
  zero-duration S1 (WAK_STS latched, run continues). QEMU implements
  the former via VM suspension; our deterministic single-thread loop
  makes the latter trivially snapshot-safe and the former a scheduler
  change. P2 measures what guests actually require to resume sanely;
  P3 takes the fork with that receipt in hand.
- **Snapshot**: PM state is small and lives in `pm_t` inside
  `machine_t`; new fields ride the existing machine snapshot
  composition (checked at each phase like MSI M4 did).

## 2b. P1 receipts

Implemented in `src/pm.c` (state `sts/en/cnt/sci_level` in `pm_t`):
reads assemble the W1C status, stored enables and the stored CNT bits
(SLP_EN reads 0) through the existing byte stream; writes are
byte-composed — STS clears through the RW1C mask `0x8501`, EN stores,
CNT low stores SCI_EN/BM_RLD, CNT high decodes SLP_TYP/SLP_EN. The
24-bit wrap inside `pm_advance` latches TMR_STS; `pm_tick` (joined to
`devices_tick`) advances at the device cadence, so the event latches
even when nobody reads the Timer. SCI is a level on IRQ9 through
`pic_set_irq`, recomputed on every state write and advance. S5
(SLP_TYP=7) logs and sets `stop_requested`; other types log and do
nothing until P3. Timer behaviour is byte-identical (the pre-P1
"only TMR has state" vector still passes unmodified — its CNT write
is 0, which is a no-op store). NC: `pm.c/pm.h` reverted to the P0
version — the three P1 groups abort at the first EN-store assert
while the Timer groups stay green (the vectors deliberately use
only pre-P1 symbols, so the revert compiles and fails
behaviourally, not at link time). Gates: `make test` + sanitize
green, ISO lane green.

## 2c. P2 receipts

`src/acpi.c` now builds the kernel-lane set as RSDP → RSDT/XSDT
{FACP, APIC} → FADT (rev 4, 244 bytes at set+0x900, after the
largest 255-CPU MADT) → DSDT (set+0xA00). The FADT fields were
taken from the ACPICA structure definition (Linux `actbl.h`), not
memory: SCI_INT=9, SMI_CMD=0, PM1a_EVT/CNT/TMR and GPE0 pointing at
`PM_DEFAULT_BASE` + offsets with the 64-bit GAS mirrors in
agreement, block lengths 4/2/4/8, flags = WBINVD|C1|RESET_REG
(24-bit timer), RESET_REG = the 0xCF9 port the chipset decodes,
RESET_VALUE 0x0E (SYS_RST|RST_CPU), boot flags legacy-devices +
8042. The DSDT is 12 bytes of hand-encoded AML: `\_S5 =
Package(7, 0)` — the same type P1 decodes, pinned byte-for-byte by
the vector. GPE0 (pm.c): STS W1C + EN stored at +0x20/+0x24, joined
into the SCI expression; nothing latches GPE bits yet (the vector
pokes the latch directly to prove the wire). NC: pm.c+acpi.c
reverted to their P1 versions with the P2 header kept — both
suites abort behaviourally (GPE0_EN store; RSDT length). The
SeaBIOS lanes were run with the stock `bios.bin` present (`make
test` alone skips them when it is absent — P1's ISO gate had run
in skip mode; both lanes green for real in P2).

**The S1 fork, measured (for P3).** No consumer exists today that
requires real parking: the north-star guest has no ACPI sleep at
all, and no other suite drives PM1_CNT. A zero-duration S1 — the
sleep write latches WAK_STS and the run continues — is spec-legal
(a wake event may be pending when the sleep is entered), trivially
deterministic and snapshot-safe, and is what a guest's
resume path exercises anyway (it must tolerate an immediate-wake
system). Real parking would additionally require a vCPU/device
scheduler pause that no consumer exercises. **Recommendation for
P3: zero-duration S1 with the limit stated plainly**, publishing
`\_S1` only with the type P3 decodes; real parking becomes a
separate, consumer-driven plan if one ever appears.

## 2d. P3 receipts (close-out)

P3 takes the §2c fork: `pm_sleep` decodes SLP_TYP=1 as a
**zero-duration S1** — WAK_STS latches, the event flows through
the normal SCI expression if enabled, the run continues, and a
guest's resume path does what it must on any fast-waking machine.
The DSDT grows the matching `\_S1 = Package(1, 0)` (60 bytes
total), pinned to the decoder by the vector alongside `\_S5`.
Unknown types still log and do nothing. NC: pm.c+acpi.c at P2 —
both suites abort (WAK_STS; DSDT length). Gates green, both
SeaBIOS lanes green with the stock BIOS present.

**Plan limits, stated plainly.** GPE0 has registers but no
sources (they arrive with their consumers). Only S1 and S5 exist;
S1 never actually parks the machine (real parking needs a
scheduler pause no consumer exercises — a separate plan if one
appears). SMI/SMM do not exist and the FADT says so (SMI_CMD=0).
The PM block still sits at PM_DEFAULT_BASE in the FADT even
though the LPC hook can relocate the real block (as it can on
silicon without the FADT changing). RTC wake/BM and the PM2
block are unmodelled.

## 3. Gates (standing rules)

Per phase: `tests/test_pm.c` (extends the existing Timer vectors),
`tests/test_acpi.c` for P2's tables, one NC, `make test` +
`make test-sanitize` + both SeaBIOS ISO lanes (the G5 ich9 boot lane
is the canary — the PM block must never re-break SeaBIOS' pmtimer).
