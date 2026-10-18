# COMPAT G3 acceptance — authoritative ISO boots to the kernel shell

**Date:** 2026-10-17.  **Status:** G3 closed.  Patch `0063` bundles this
evidence; the plan row is updated in `docs/plans/COMPAT_PLAN.md`.

## Question

The G3 diagnostics (`../README.md`) proved that SeaBIOS `call32_post()`
reloads FS/GS with 64-KiB real-mode limits on the AHCI INT 13h path, so Stage 2
copied the ELF through stale segment bases (`0x200100` instead of `0x200000`)
and failed ELF parsing.  A candidate patch existed, but only in a
diagnostic build.  G3 was open because the authoritative release image had
never booted to the shell.

## Answer

The pinned AuraLite-OS source `518bef0` was rebuilt with the release toolchain
and only the reviewed Stage 2 patch applied.  That image boots, under an
**unmodified Debian SeaBIOS 1.16.3-2 `bios.bin`**, from the BIOS path through
Stage 2 and long mode into `[kernel] shell active; kmain idling.` in both
x86emu and QEMU.  The handoff block the kernel reads at `0x10000` validates
against the documented `boot_info_t`.

| lane | pinned ISO | candidate ISO |
|---|---|---|
| x86emu + stock SeaBIOS, AHCI + blank USB | `ELF parse FAILED`, halt in real16 | `ELF PT_LOAD segments copied`, long mode, **shell active**, `fault=0`; second run byte-identical |
| QEMU 10.0.13 + same `bios.bin`, q35/TCG/AHCI/USB | `ELF parse FAILED` | **shell active**, with `ACPI RSDP found` / `MADT parsed` |

Both images are 84,934,656 B and differ in exactly 10 sectors, all inside the
Stage-2 staging area at LBA 34..46 — the boot sector, GPT, FAT metadata,
kernel, initrd and UEFI payload are byte-identical (`iso-layout.txt`).

## Contents

- `receipt.md` — identity, marker table, end-of-run state, handoff fields,
  QEMU cross-check, explicit non-claims.
- `inputs.txt` — commit, build-product hashes, stock-BIOS provenance, and why
  the ISO hash cannot equal the published one (non-reproducible
  `__DATE__`/`__TIME__` in `kernel.elf`).
- `iso-layout.txt` — 10-of-166,272-sector diff receipt.
- `commands.txt` — full reproduction, including the `mkiso` invocation and
  the stock SeaBIOS package.
- `x86emu-stock-pinned.serial.log.gz`, `x86emu-stock-candidate.serial.log.gz`
  and the matching `*.stdout` — primary x86emu receipts;
  `x86emu-stock-candidate-replay.*` — the byte-identical second candidate run.
- `x86emu-kernel-entry.log.gz` — debugger stop at `_start`
  (`rip=0xffffffff80191640`, `rdi=0x10000`, magic at `0x10000`).
- `bootinfo.bin` + `bootinfo-parse.txt` — the validated 7,776-byte block.
- `qemu-stock-pinned.serial.log.gz`, `qemu-stock-candidate.serial.log.gz` — QEMU
  cross-check using the identical stock ROM.
- `SHA256SUMS` — covers every file in this directory except itself.

## Scope

This closes G3 as a *behavioural* acceptance: the release toolchain, the
reviewed patch, a stock BIOS and a measured serial/handoff receipt.  The
81-MiB images themselves are not stored; hashes plus the exact rebuild
commands are.  The accepted image is not the published `d2110f8b…` ISO —
`kernel.elf` embeds build timestamps — and no claim is made that it is.

Not established here: SMP/IOAPIC, allocator zones, CF9 causality, PERF P4,
USB-storage semantics, or ACPI on x86emu's default LPC identity (that is G4's
opt-in `--lpc=ich9` lane, `../../compat-g4/`).
