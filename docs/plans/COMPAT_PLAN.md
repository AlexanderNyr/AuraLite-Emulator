# COMPAT plan — the BIOS route to the north star

> **House style** (`ROADMAP.md`, `CORE_PLAN.md` §2/§5): dependency-ordered
> phases, each with a definition of done and a test gate, shipping as **one
> `.patch` per phase** under `patches/`. Everything below is measured against
> the trees and the pinned inputs, and every phase names what it does **not**
> establish. `COMPAT` is the only plan whose acceptance artifacts live partly
> outside this repository (the AuraLite-OS release ISO and its Stage 2), so
> absence of an artifact is recorded as a SKIP, never as a pass.

## Why this plan exists

`KERNEL_BOOT_PLAN` (K0–K8) proved the *direct* route: `--kernel=` loads an
ELF64 kernel and reaches the guest shell without any firmware in between. The
north star in `ROADMAP.md`, however, is written against the **BIOS route**:

```
  SeaBIOS → AuraLite Stage 1 (boot sector) → Stage 2 (real-mode loader)
          → flat 64-bit kernel load → AuraLite OS shell
```

That route exposes a different class of defects — real-mode instruction
encodings, IVT/BIOS-call ABI, unreal-mode segment caches, ACPI/E820 surfaces,
and the handoff block the kernel actually parses. COMPAT is the plan that
walks it.

## Plan ledger

| Phase | Status | Scope | Evidence |
|---|---|---|---|
| G0 | ✅ **measured** | Baseline and harness: pinned ISO/kernel/ROM hashes, deterministic run shape, v2 snapshot reader, PAM-aware debugger physical peek | `docs/evidence/compat-g2/`, `docs/evidence/compat-g3/` |
| G1 | ✅ **measured** | Stage-1 boot-sector dispatch; the original Stage-2 relocation panic closes | `docs/evidence/compat-g2/commands.txt` (predecessor boundary) |
| G2 | ✅ **measured** | Stage-2 entry gate: real-mode `SGDT/SIDT` store width fixed, `PUSHA/POPA` added, BIOS read receipts repeatable | `docs/evidence/compat-g2/README.md` |
| G3 | ✅ **delivered** (patch `0063`) | authoritative ISO (pinned `518bef0` + reviewed Stage-2 FS/GS fix, 10-of-166,272-sector diff) boots to `[kernel] shell active` under a **stock** SeaBIOS in x86emu and QEMU; `boot_info_t` at `0x10000` validates | `docs/evidence/compat-g3/acceptance/` |
| G4 | ✅ **delivered** (patch `0062`) | ACPI identity pairing on the **emulator** side: `--lpc=ich9` makes the LPC function `8086:2918`, so stock SeaBIOS publishes RSDP/RSDT/FACP/SSDT/MADT(**MCFG**) | `docs/evidence/compat-g4/README.md` |
| G5 | ✅ **delivered** (patch `0064`) | ICH9 boot completed: the missing ACPI PM block (PMBASE+8, 24-bit TMR at 3×PIT) was the real blocker, not the `0xefa1f` RIP; `--lpc=ich9` now boots the accepted image to `[kernel] shell active` with `fault=0`, `rsdp_phys=0xf6830`, 8-entry E820 including the ICH9 RCRB, and an RSDP→RSDT→MADT receipt | `docs/evidence/compat-g5/` |
| G6 | ✅ **delivered** (patch `0065`) | SMP as the final rung: `EM_MAX_VCPU = 8`, `CMOS[0x5F]`, broadcast INIT/SIPI (`shorthand=2/3`), `CPUID(1).EBX[31:24]` APIC ID, AP work-counter preservation across second-stage INIT; `--lpc=ich9 --smp=4` boots the accepted image through SeaBIOS, wakes all 3 APs (`4 CPU(s) online`), logs R5, and passes `/tests/irqapwake` + `/tests/smpstress` | `docs/evidence/compat-g6/` |

Ordering rationale: the CPU/bios boundary defects (G1–G2) had to close before
the loader's own copy path could even be observed; the copy path (G3) had to
be understood before ACPI (G4) could be attributed; ACPI had to be published
before any handoff-field comparison on a stock BIOS is meaningful (G5); SMP
(G6) multiplies every one of those surfaces and therefore comes last.

## G0 — baseline and harness ✅

**Done when:** every input is hash-pinned, the run shape is deterministic, and
the inspection tools read the machine without changing it.

Measured and recorded:

- Pinned inputs: ISO 84,934,656 B SHA-256 `d2110f8b…`; `kernel.elf`
  3,073,904 B SHA-256 `a1c5264b…`, entry `0xffffffff80191640`, `e_phnum=3`;
  compatibility checkout `518bef07…`; SeaBIOS `rel-1.16.3` `a6ed6b70…`.
- Two diagnostic ROMs (`69f2819d…` diagnostic, `012f5d98…` ACPI-ID probe) and
  a 1 MiB all-zero USB image are bundled in `docs/evidence/compat-g3/`.
- Snapshot format **v2**: PCI config and the AHCI BAR are restored before MMIO
  validation; v1 files are refused instead of misread (this came out of a G3
  test run — a tooling defect found by the gate, not by inspection).
- `mem_debug_peek_byte()` follows Q35 PAM, so the debugger reads the guest's
  RAM shadow instead of immutable flash.

**Gate:** `make test` + `make test-sanitize` green; the 12-lane split-snapshot
gate green; the bundled ROMs run to POST under `x86emu` with exit 0.

## G1 — Stage-1 dispatch ✅

**Done when:** the pinned ISO's boot sector is read and entered, and the
Stage-2 relocation panic no longer terminates the loader.

The G2 bundle records this as the predecessor boundary (the dispatch receipt
`G2 basic status=0 count=1`, then Stage-2 relocation). No new claim is made
here beyond what that bundle captured.

## G2 — Stage-2 entry ✅

**Done when:** the BIOS read path completes and `[BL3] AuraLite stage2 alive`
is repeatable, with the CPU defect that blocked it fixed in `src/` and pinned
by a unit vector.

Measured root cause and fix:

- `SGDT`/`SIDT` wrote an extra zero word at `addr+6` outside 64-bit mode.
  SeaBIOS `call32_prep()` executes `SGDT [ESP+2]`; the eighth byte overwrote
  the low half of saved `EBX` (`000eff88` → `000e0000`), so dispatch jumped to
  the wrong address (`op=000e0000`).
- `src/cpu.c` now writes the architectural six bytes in non-long mode and ten
  in 64-bit mode; `tests/test_cpu.c` pins the exact real-mode encoding
  `67 66 0F 01 44 24 02` with a stack sentinel.
- `PUSHA/PUSHAD`/`POPA/POPAD` were added for the Stage-2 instruction path;
  the post-entry `#UD opcode 0x60` boundary in the first replays is closed.

**Receipts:** boot-sector read (`cmd=2 count=1 lba=0 buf=00007c00`), extended
read (`count=126 lba=34 buf=00008000`), `[BL3] AuraLite stage2 alive`, twice
repeatable; negative media lane (`--sata` omitted) prints
`Boot failed: could not read the boot disk` and no Stage-2 marker.

**Gate:** `make test-unit` (CPU + PAM + RTC vectors) green; the two positive
replays byte-identical; no always-writable-ROM bypass in the lane.

## G3 — Stage-4 FS/GS, and the ACPI gate ✅ (patch `0063`)

**Done when (acceptance):** the authoritative release ISO, unchanged except
for a reviewed Stage-2 fix, reaches `shell active` under a stock BIOS, and the
handoff block the kernel parses matches the documented `boot_info_t` on that
image.

Diagnostics completed (measured, diagnostic-only):

1. **FS/GS root cause.** SeaBIOS `call32_post()` reloads FS/GS selectors on
   the AHCI INT 13h path: selector `0x0010`, hidden base `0x100`, limit
   `0xffff`. Stage 2 had assumed its unreal-mode flat caches survived, so
   `fat_load` read through `GS:[ESI]` and wrote through `FS:[EDI]` with the
   wrong base — the ELF was copied to `0x200100` instead of `0x200000`.
2. **Candidate guest fix.** `disk_read_lba` calls `go_unreal` on both the
   success and the exhausted-retry exits, before restoring the saved GPR
   frame; `go_unreal` now preserves all registers with `pushad/popad`
   (`stage2-fs-gs-restore.patch`, Stage 2 = 6,656 B `51ecc150…`).
3. **A/B under the same stock 128-KiB SeaBIOS:** pinned ISO → `ELF parse
   FAILED`; candidate ISO → PT_LOAD copy, long mode, `[kernel] shell active`,
   `fault=0`. QEMU reproduces the same split with both `if=ide` and explicit
   AHCI + blank USB, so the defect is in the guest's assumption, not in
   x86emu's segment model.
4. **ACPI gate.** With an RSDP-publishing probe ROM and the *unchanged* ISO,
   Stage 2 reports RSDP found and MADT parsed, then still fails ELF parsing:
   the two defects are independent.

**Acceptance (2026-10-17, patch `0063`).** The pinned source `518bef0` was
rebuilt with the release toolchain in a clean worktree; a second worktree
applied only `stage2-fs-gs-restore.patch` and reassembled the same 6,656-byte
Stage 2 (`51ecc150…`, byte-identical to the diagnostic blob). Both 81-MiB ISOs
came from the release `mkiso`, with `ESP_MB=80`; they differ in exactly 10 of
166,272 sectors, all inside the Stage-2 staging area at LBA 34..46 — boot
sector, GPT, FAT metadata, kernel, initrd and UEFI payload are identical.

Under the **unmodified** Debian SeaBIOS 1.16.3-2 `bios.bin` (`4ee65f3d…`),
x86emu boots the candidate to `[kernel] shell active; kmain idling.` with
`fault=0` and `cap_reached=1`; the pinned ISO stops at `[BL4] ELF parse
FAILED` in the identical lane (negative control). A temporary breakpoint at
`_start` (`0xffffffff80191640`) records `rdi=0x10000` and `BOOT_MAGIC` at
`0x10000`; the 7,776-byte block validates against the generated
`boot_info_t` offsets — 7 sorted E820 entries, HHDM `0xffff800000000000`,
initrd `0x1800000/0xb22000`, `cpu_count=1`, `bsp_lapic_id=0`, `rsdp=0`. QEMU
10.0.13 with the same ROM reproduces the split and additionally boots through
`ACPI RSDP found` / `ACPI MADT parsed`.

`make test-g3` re-checks the bundle (54 conditions: manifest digests, ordered
serial receipts, handoff layout, the deterministic Stage-2 blob and a second
byte-identical candidate replay) and `make test-g3-nc` requires five tampered
copies to be rejected.  The candidate lane was run twice with `cap=2,000,000,000`
and produced byte-identical serial streams and stats.

`kernel.elf` is not bit-reproducible (`__DATE__`/`__TIME__`), so the accepted
image carries its own hash (`57381825…`) instead of the published `d2110f8b…`.
Acceptance is behavioural; the rebuild recipe, hashes and both serial and
handoff captures are in `docs/evidence/compat-g3/acceptance/`.

## G4 — ACPI identity pairing ✅ (patch `0062`)

**Done when:** a stock, unpatched SeaBIOS publishes a complete and
checksum-valid ACPI table set on this machine, with an opt-in switch that
leaves every historical lane byte-identical.

Measured boundary and fix:

- SeaBIOS `rel-1.16.3` `acpi_setup()` matches only PIIX4 PM `8086:7113` or
  ICH9 LPC `8086:2918`; this machine advertised Panther Point `8086:1e55`
  (the ID the sample firmware knows) while already advertising the Q35 host
  bridge `8086:29c0` and implementing its PAM registers. That inconsistency
  suppressed ACPI entirely.
- `--lpc=ich9` (`machine_t.cfg_lpc_ich9`, `src/devices.c`) makes the LPC
  function `8086:2918`; the default stays `8086:1e55`.
- `tests/test_compat.c` pins 7 vectors: both identities, class/header,
  host bridge, AHCI function, and PAM under the ICH9 identity.

**Receipts** (`docs/evidence/compat-g4/`):

| Lane | Result |
|---|---|
| default identity + diagnostic ROM | `NO RSDP` |
| probe ROM (ROM-side patch), default identity | RSDP `0xf6210`, RSDT `0x7ffe4c0`, 3 entries |
| `--lpc=ich9` + **unmodified** diagnostic ROM | RSDP `0xf6210`, RSDT `0x7ffe470`, **4** entries: FACP, SSDT, APIC (MADT, LAPIC base `0xfee00000`), **MCFG** — all checksums 0 |
| bundled sample firmware with the flag | unchanged: `HALTED`, `executed=4837677`, `GUEST_OK`, framebuffer marker |

The extra `MCFG` versus the ROM-patch lane is itself a receipt: SeaBIOS now
follows its full Q35/PCIe path, which the old match-table patch alone did not
enable.

**Gate:** `make test-compat` 7/7 in `make test` and `make test-sanitize`;
historical lanes unchanged (flag is opt-in).

**Not established by G4:** any pinned-image boot, any E820/`boot_info_t`
agreement, any SMP/IOAPIC claim. G4 removes the emulator-side ACPI blocker
only.

## G5 — post-acceptance ACPI/E820 agreement ✅ (patch `0064`)

G3 delivered the shell-active acceptance on the **default** LPC identity.  G5
had to extend it to `--lpc=ich9`, where G4 publishes the ACPI tables.  The
phase turned into a real boot fix, because that lane never reached Stage 2.

**The blocker and its root cause.** `--lpc=ich9` makes the LPC function
`8086:2918`, so stock SeaBIOS takes its full Q35/ICH9 path: it builds ACPI,
publishes the RSDP and points its own timer at `PMBASE+8` (`TimerPort=0xb008`).
The emulated chipset had no ACPI PM block, so that port read `0xFFFFFFFF`
forever and no `timer_check()` deadline could expire.  The visible stall is
the AHCI probe waiting for `DET==3` on an empty port 1 (`PxSSTS=0x1a8`) while
port 0 is accepted (`0x123`); the 718-instruction loop around `f0068`
(PxSSTS) and `in eax,dx` at `f010b` is the symptom.  The RIP `0xefa1f`
recorded earlier was only a debugger stop inside a `call` in
`ich9_lpc_setup` — **the boot was never reset-looping; it was waiting on a
timer that did not exist.**

**Fix (patch `0064`).** `src/pm.{h,c}` model the 0x40-byte ACPI PM block: a
default base of `0xB000` moved by LPC config bits 15:7 of `0x40..0x43`, a
24-bit read-only TMR at PMBASE+8 counting **three counts per PIT tick**
(3.579545 MHz vs 1.193182 MHz, i.e. one count per 128 retired instructions
with the remainder carried), every other byte reading zero and every write
swallowed.  `pm_init()` runs in `devices_init_common`, and the LPC device's
`cfg_write` hook applies PMBASE changes.  Ports outside the window still
float high.

**Measured on the G3 accepted image** (`docs/evidence/compat-g5/`):
`--lpc=ich9` + unmodified Debian SeaBIOS 1.16.3-2 reaches `[kernel] shell
active; kmain idling.` with `executed_steps=829920364`, `fault=0`, RC 0 in
158 s; the marker first appears between 435 M and 445 M instructions.
`boot_info_t` now carries `rsdp_phys=0xf6830`, and a snapshot walk after the
marker finds RSDP `0xf6830` (rev 0, checksum ok) → RSDT `0x7ffe3a0` with four
checksum-valid tables (FACP, SSDT, APIC/MADT, MCFG) and a MADT whose I/O APIC
entry (`0xfec00000`, GSI base 0) matches the guest's `[ioapic] ... (MADT
agree)` line.  Negative control: restoring the frozen `0xFFFFFFFF` read never
prints `[BL3]` in 300 M instructions, while the fixed tree prints it at
297,585,739.

**E820 7 vs 8 — explained.** The default `8086:1e55` identity reports seven
entries and no RSDP.  Under `--lpc=ich9` the eighth entry is
`0xfed1c000+0x4000` — the ICH9 Root Complex Register Block that SeaBIOS
reserves once it takes the ICH9 path.  QEMU's q35 map with the same 128-KiB
ROM has the identical eight-entry shape; the only numeric difference left is
the size of the firmware's own ACPI/NVS slab at the top of low RAM (16 KiB vs
132 KiB), with both ending at `0x08000000`.  The E820 count tracked the same
identity fix as the timer, not any memory-sizing divergence.

**One divergence recorded, not fixed (guest side).** The kernel prints
`[ioapic] overrides: none in the MADT` although the MADT carries four
Interrupt Source Overrides (ISA IRQ 5/9/10/11, flags `0x000d`).  AuraLite-OS's
`madt_walk()` accepts a type-2 entry only when `elen >= 12`, while the ACPI
spec defines that structure as 10 bytes.  QEMU with the same BIOS prints the
same line, so this is a guest bug, not an emulator divergence; the fallback
routing it produces is correct for this machine.

**Gates:** `make test-g5` (in `make test`) re-checks the bundle — manifest,
ordered boot markers, `fault=0`/long-mode/budget in the stats block, the
8-entry E820 with the RCRB reservation, `boot_info_t.rsdp` equal to the RSDP
the memory walk found, the ACPI receipt against the serial log, and the
negative-control numbers.  `make test-g5-nc` tampers four bundle copies and
requires each to be rejected; `make test-g5-live` replays the frozen-timer
stall (`g5_pm_nc.sh`, ~55 s).  `tests/test_pm.c` pins the block itself in
`make test-unit`: 3 counts per PIT tick, 24-bit wrap, byte/word/dword reads,
zero elsewhere, swallowed writes, PMBASE moves, and the LPC config hook.

## G6 — SMP as the final rung ✅ (patch `0065`)

**Done when:** the BIOS route boots a kernel with **more than two** executing
contexts, real APs answer INIT-SIPI-SIPI through the IOAPIC/LAPIC path, and
per-AP timers tick on the shared virtual clock without changing UP receipts.

**Decision and root causes.** Extending the round-robin context array from
`EM_MAX_VCPU = 2` to `8` preserves the K6/K8 work-clock identity
(`vtime_instr == instr_count` when one context runs, and
`sum(instr_count) == vtime_instr` across `N` contexts) without introducing
scheduler state.  Disassembling SeaBIOS 1.16.3-2's `smp_setup()` (`0xf1538`),
`qemu_get_present_cpus_count()` (`0xe8040`), `handle_smp()` (`0xef526`), and
`build_madt()` (`0xe9a3f`) exposed four firmware/hardware handoff requirements
beyond K6's `--kernel` path, plus one two-stage bring-up accounting invariant:

1. **CMOS `0x5F` (`src/rtc.c`, `src/main.c`):** SeaBIOS's `smp_probe()` reads
   CMOS `0x5F` (`CMOS_BIOS_SMP_COUNT`) and sets `MaxCountCPUs = CMOS[0x5F] + 1`.
   `rtc_init()` and `--smp=N` wiring now write `nv - 1` into `CMOS[0x5F]` when
   `nv > 1` (`0` in UP).
2. **Broadcast INIT and SIPI (`src/lapic.c`):** SeaBIOS wakes all APs during
   POST with ICR destination shorthand `3` ("all excluding self":
   `0x000C4500` broadcast INIT, then `0x000C4610` broadcast SIPI at vector
   `0x10`).  `lapic_icr_write()` now handles `shorthand == 2` and `3` on the
   `dm == 5` (INIT) and `dm == 6` (SIPI) path.
3. **Initial APIC ID in `CPUID(1).EBX[31:24]` (`src/cpu.c`):** Each woken AP
   executes `cpuid` leaf 1 inside `smp_write_msr` (`0xef526`) and shifts `EBX`
   right by 24 to set bit `(1 << apic_id)` in `FoundAPICIDs` (`0xf5a00`), which
   `build_madt()` tests to set each Type-0 Local APIC entry's `Flags.Enabled`
   bit.  `do_cpuid()` now reports `m->lapic.id & 0xFF000000u` in `EBX[31:24]`
   (`0` on BSP/UP, `i << 24` on `vcpu_i`).
4. **AP counter preservation across second-stage `INIT` (`src/lapic.c`):** On
   the BIOS route each AP executes 736–2,038 instructions during SeaBIOS POST
   before parking in `cli; hlt` (`VCPU_OFF`), and is later reset and woken a
   second time by the kernel's targeted physical `INIT` + `SIPI` at `0x8000`.
   `vcpu_take_init()` now saves and restores `executed_steps`, `instr_count`,
   `idle_quantum`, and `last_vtime` across `cpu_reset(&s->c)`, keeping
   `sum(executed_steps) == executed` and `sum(instr_count) == vtime`.

**Measured on the accepted image** (`docs/evidence/compat-g6/`):
- Diagnostic SeaBIOS POST (`--lpc=ich9 --smp=4 --max-instr=10000000`) prints
  `handle_smp: apic_id=0x1..0x3` and `Found 4 cpu(s) max supported 4 cpu(s)`.
- Full BIOS boot (`--rom=bios.bin --lpc=ich9 --smp=4 --max-instr=4000000000`,
  RC 0) publishes a 134-byte MADT (`cpus=4 ioapics=1 isos=4`) and a
  `boot_info_t` with `cpu_count=4` (`processor_id = lapic_id = 0..3`), wakes
  all three APs (`[smp] AP #0..#2 online`, `[smp] 4 CPU(s) online (3 AP(s)
  woken)`), prints `[sched] R5 receipt: user thread pid=6 on AP cpu=3`,
  reaches `[kernel] shell active; kmain idling.`, and passes typed
  `/tests/irqapwake` (`[smpwake] PASS: 14 RTC(GSI8) device IRQs delivered to
  cpu1 (apic id 1); hlt looper woken 5 times -- RES-16 receipt`,
  `IRQAPWAKE PASS: kernel receipt returned 0`) and `/tests/smpstress`
  (`O_APPEND 6 x 200 x 32 B intact`, `100 fork/wait cycles precise`,
  `8 x 10 signal deliveries counted`, `SMPSTRESS PASS`) with `fault=0` on all
  4 vCPUs and exact work-counter identities (`executed=1187467939`,
  `idle_quantum=180026141696`, `vtime=181213609635`).
- All 9 historical `p5_history.sh` lanes plus 40 M `--kernel` UP and
  `--cpus=2 --smp=2` runs are byte-identical against the G5 binary.

**Gates:** `make test-g6` (73 bundle checks, inside `make test`),
`make test-g6-nc` (five tampered bundles rejected), `make test-g6-live`
(`tests/integration/g6_smp_nc.sh`, broadcast INIT/SIPI negative control on the
in-tree diagnostic SeaBIOS ROM), and unit vectors in `tests/test_rtc.c` and
`tests/test_lapic.c`.

## Non-claims

- No claim that the published pinned ISO `d2110f8b…` itself boots: bit
  identity is impossible because `kernel.elf` embeds build timestamps.  G3's
  accepted image is a hash-recorded rebuild from the pinned source whose only
  guest change is the reviewed Stage-2 patch.
- The 81-MiB images are not stored in the tree; the bundle keeps hashes, the
  sector-diff receipt, serial captures and the handoff block.
- No allocator-zone measurement, CF9-causality claim, or PERF P4 pass is
  established by any G phase.
- G4's ROM-side control (`seabios-acpi-id-probe-128k.bin`) exists only to
  validate the scanner and the root cause; the shipped fix is the emulator
  identity switch, not a BIOS patch.
