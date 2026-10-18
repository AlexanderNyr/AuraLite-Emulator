# COMPAT G5 — ICH9 boot, the ACPI PM timer, and the E820 delta

Patch **0064**. The G5 blocker was recorded as "`--lpc=ich9` never reaches
Stage 2". This bundle contains the measurement that located the cause, the
receipt for the fix, and the two acceptance extensions G5 owed: RSDP/MADT on
the accepted image, and an explanation of the E820 7-vs-8 difference.

## What was broken

`--lpc=ich9` reports LPC `8086:2918`. Stock SeaBIOS then takes its full
Q35/ICH9 path: it builds the ACPI tables, publishes the RSDP, and tells its
own timer code to use PMBASE+8 (`TimerPort=0xb008`). Our chipset model had no
ACPI PM block at all, so that port read `0xFFFFFFFF` forever and SeaBIOS's
`timer_check()` never timed out.

The stall that follows is in the AHCI probe: port 0 answers `PxSSTS=0x123`
(DET=3, device present) and is accepted, while the empty port 1 answers
`0x1a8`. SeaBIOS waits for DET==3 on port 1 with a deadline measured on the
PM timer; with the timer frozen the wait never ends. The 718-instruction loop
`6ff4c94 → … → f0792 → … → f0068` (PxSSTS read) with `in eax,dx` at `f010b`
was the whole symptom, not the cause: the RIP `0xefa1f` reported earlier was
just where the debugger happened to stop inside a `call`.

**Root cause: the emulated ICH9 LPC has no ACPI PM block, so no timeout that
depends on the ACPI timer can expire.**

## The fix

`src/pm.{h,c}` — a 0x40-byte ACPI PM register block:

- base `PMBASE` (default `0xB000`, bit 7 forced per the ICH9 spec), moved by
  LPC config bits 15:7 of registers `0x40..0x43`;
- a 24-bit Timer at PMBASE+8 counting at 3.579545 MHz, i.e. **three counts per
  PIT tick** (the PIT model retires 384 instructions per tick, so one TMR count
  per 128 retired instructions, plus the remainder carried);
- every other byte of the block reads zero; all writes are swallowed; the TMR
  is read-only. Ports outside the window still float high, as before.

`devices_init_common` now calls `pm_init(m)` right after `pit_io_register`, and
the LPC device carries `cfg_write=lpc_cfg_write` with `ctx=m` so a PMBASE
programmed in config space moves the window.

## Result

`--lpc=ich9` + stock SeaBIOS 1.16.3-2 + the accepted candidate ISO:

    [serial] [BL9] ACPI RSDP found
    [serial] [BL9] ACPI MADT parsed
    [serial] [BL4] ELF PT_LOAD segments copied to phys
    [serial] [ioapic] base 0xfec00000 (MADT agree)
    [serial] [kernel] shell active; kmain idling.
    [stats] executed_steps=829920364 instr_count=599910693996 halted=0 fault=0

RC 0, 158 s wall, budget 2 G not the binding constraint. `shell-active-bisect.txt`
bounds the first `shell active` line between 435 M and 445 M instructions
(no marker at 435 M, marker at 445 M); the run then idles in HLT until the cap,
which is why `cap_reached=1` and why `executed_steps` keeps growing after the
marker.

Negative control (`nc-pm.txt`, `tests/integration/g5_pm_nc.sh`): the same tree
with `pm_timer_now()` restored to the pre-fix `0xFFFFFFFF` never prints `[BL3]`
in 300 M instructions and burns the entire budget; the unmodified tree prints
it at 297,585,739.

## Acceptance extension 1 — RSDP and MADT on the accepted image

`acpi-receipt.txt` is produced by `tests/g5_acpi_receipt.py` from a snapshot
taken after `shell active`, and is checked against the serial log:

    rsdp addr=0x000f6830 rev=0 checksum=ok rsdt=0x07ffe3a0
    rsdt sig=RSDT entries=4 checksum=ok
    table sig=FACP ... SSDT ... APIC ... MCFG ...   (all checksums ok)
    madt lapic_base=0xfee00000 cpus=1 ioapics=1 isos=4
    madt_ioapic id=0 addr=0xfec00000 gsi_base=0x0

`boot_info_t` now carries `rsdp_phys=0xf6830` — the same pointer the walk
found — instead of the `0` the default identity reported.

**One divergence, measured and attributed:** the guest prints

    [ioapic] overrides: none in the MADT; PC-standard defaults programmed

although this MADT has four Interrupt Source Overrides (ISA IRQ 5/9/10/11 →
same GSI, flags `0x000d` = level, active-low). The cause is guest-side:
`kernel/arch/x86_64/ioapic.c` accepts a type-2 entry only when
`elen >= 12`, while the ACPI spec defines the ISO structure as 10 bytes. On a
10-byte entry the walk stops and `madt_iso_count` stays 0. QEMU with the same
BIOS prints the identical line (`docs/evidence/compat-g3/acceptance/qemu-stock-candidate.serial.log.gz`),
so this is not an emulator divergence and not a boot-behavior change here:
none of the four overrides touches IRQ0, and the PC-default route it falls back
to (IRQ0→GSI2) is what the FADT/ISO set describes anyway. The emulator-side
MADT is spec-conformant; the kernel's guard is recorded, not fixed, because
AuraLite-OS is not ours to change in this phase.

## Acceptance extension 2 — E820: 7 vs 8

`bootinfo-parse.txt` compares the block captured under `--lpc=ich9` with the
QEMU block from G3 (same 128-KiB `bios.bin`, candidate ISO, AHCI + blank USB):

| # | x86emu `--lpc=ich9` | QEMU q35 |
|---|---|---|
| 0 | `0x0 + 0x9fc00` t1 | same |
| 1 | `0x9fc00 + 0x400` t2 | same |
| 2 | `0xf0000 + 0x10000` t2 | same |
| 3 | `0x100000 + 0x7efc000` t1 | `0x100000 + 0x7edf000` t1 |
| 4 | `0x7ffc000 + 0x4000` t2 | `0x7fdf000 + 0x21000` t2 |
| 5 | `0xb0000000 + 0x10000000` t2 | same |
| 6 | `0xfed1c000 + 0x4000` t2 | same |
| 7 | `0xfffc0000 + 0x40000` t2 | same |

The default `8086:1e55` lane reports **7** entries because it never reaches
the ICH9 path: no RSDP and no RCRB reservation. Under `--lpc=ich9` the eighth
entry `0xfed1c000+0x4000` (the ICH9 Root Complex Register Block) appears,
exactly as in QEMU. The E820 count therefore tracked the same identity fix as
the timer, and the only remaining numeric difference is the size of the
firmware's own ACPI/NVS slab at the top of low RAM (16 KiB here, 132 KiB in
QEMU), with both slabs ending at `0x08000000`. No allocator-zone conclusion is
drawn from this.

## Files

- `x86emu-ich9.receipt` — RC and wall time of the acceptance run.
- `x86emu-ich9.stdout.txt` — final CPU state and the PERF P0 stats block.
- `x86emu-ich9.serial.log.gz` — the full serial log (compressed with `gzip -n`).
- `bootinfo-ich9.bin` / `bootinfo-parse.txt` — the 7,776-byte handoff block and
  its decoded map.
- `acpi-receipt.txt` — RSDP → RSDT → tables, MADT decoded, from the snapshot.
- `nc-pm.txt` — the negative control's two lines of proof.
- `shell-active-bisect.txt` — instruction-bound bisect for the `shell active` marker.
- `commands.txt`, `inputs.txt` — exact reproduction and input identities.
- `SHA256SUMS` — bundle hashes.

The 30 MB end-of-run snapshot and the 1.7 MB raw serial log are not stored.

## What remains open

- The AuraLite-OS ISO type-2 length guard above is a guest-side finding, not
  fixed here.
- No HPET, SMM, or PCIe hotplug claim is made. `MCFG` is published but the
  emulator still serves ECAM itself, as in G4.
- The default `8086:1e55` identity keeps its 7-entry E820 and absent RSDP;
  nothing in this patch changes that lane.
