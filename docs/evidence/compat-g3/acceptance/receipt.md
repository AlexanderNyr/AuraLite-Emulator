# COMPAT G3 — acceptance receipt

**Date:** 2026-10-17.  **Verdict:** G3 acceptance criterion met.

Criterion (`docs/plans/COMPAT_PLAN.md`, G3):
> the authoritative release ISO, unchanged except for a reviewed Stage-2 fix,
> reaches `shell active` under a stock BIOS, and the handoff block the kernel
> parses matches the documented `boot_info_t` on that image.

## 1. Image identity

Built from the pinned AuraLite-OS commit `518bef0` with `ESP_MB=80` and the
release `mkiso`.  The only guest-source change is
`../stage2-fs-gs-restore.patch` (3 files, +11/−4), applied in a separate
worktree and assembled into an otherwise identical ISO.  The two ISOs differ
in exactly 10 sectors, all inside the 13-sector Stage-2 staging area at LBA
34..46 (see `iso-layout.txt`); kernel, initrd, UEFI payload, boot sector, GPT
and FAT metadata are byte-identical.

| artefact | size | SHA-256 |
|---|---|---|
| pinned ISO | 84,934,656 | `3bf633dc127acfd445bc5ceb5178cec138a8c780ad07e3cc4f25c1af76cd25f3` |
| candidate ISO | 84,934,656 | `573818257ff30001fa3980ffdd23964ff02390c74c4bbaeebaea1f9c2a8e7fea` |
| Stage 2, pinned | 6,656 | `3018fcd50629ceccb45a256e199192ce140a45b5dbe5f8f806b5077ff91ecfac` |
| Stage 2, candidate | 6,656 | `51ecc150f6eefd84f570f2fbdc808c4f3edd7ac7bf342559c19f652638b85963` |
| stock SeaBIOS `bios.bin` | 131,072 | `4ee65f3d02c5e37c3c358618334de65c5b161fac6504aaacd39675eb4ecb80ca` |

The candidate Stage 2 reassembles to the same SHA-256 as the diagnostic blob
already in `../stage2-fs-gs-restore.bin`, i.e. the reviewed patch is
deterministic.

## 2. x86emu, stock SeaBIOS, identical devices

Command shape for both lanes (full lines in `commands.txt`):

```
./x86emu --rom=<stock bios.bin> --platform=haswell \
  --disk=docs/evidence/compat-g3/blank-usb-1m.img \
  --sata=<iso> --max-instr=... --stats
```

Serial receipt (`x86emu-stock-*.serial.log.gz`):

| marker | pinned | candidate |
|---|---|---|
| `[BL3] AuraLite stage2 alive` | yes | yes |
| `[BL3] disk read OK` | yes | yes |
| `[BL4] kernel.elf loaded to 0x00200000` | yes | yes |
| `[BL4] ELF PT_LOAD segments copied to phys` | **no** | yes |
| `[BL4] initrd.tar loaded to 0x01800000` | **no** | yes |
| `[BL4] entering long mode; jumping to kernel _start` | **no** | yes |
| `[kernel] shell active; kmain idling.` | **no** | yes |
| `[BL4] ELF parse FAILED` | **yes** | **no** |

End-of-run state (`x86emu-stock-*.stdout`): both runs `fault=0`; the pinned
lane halts in `real16` after `[BL3] real-mode services complete; halting`
(`executed_steps=155,247,564`, `cap=1,300,000,000`, `cap_reached=1`), the
candidate lane halts in `long64` with `executed_steps=830,338,903`,
`cap=2,000,000,000`, `cap_reached=1`.  The candidate's `idle_quantum` shows the
shell idling in `HLT`, not spinning.

Repeatability: the candidate lane was run a second time at the same cap with
the same O2 binary (SHA-256 `d8794740…`).  The replay produced a **byte-identical**
serial stream and stats block (`x86emu-stock-candidate-replay.*`); the gate
compares them against the primary capture.

## 3. Handoff block

At kernel entry, temporary breakpoint on `_start` `0xffffffff80191640`:

```
rip 0xffffffff80191640  rdi 0x0000000000010000
0000000000010000: 0x4155524142544c44      (BOOT_MAGIC)
```

Raw debugger excerpt: `x86emu-kernel-entry.log.gz`.  The 7,776-byte block was
then extracted from the end-of-run v2 snapshot (`--snapshot-save`) and is
stored as `bootinfo.bin`; `bootinfo-parse.txt` is the parser output.  Validated
fields, all against the pinned `boot/shared/boot_info.h` layout:

- size 7,776 B; generated `bootinfo-layout.inc` offsets/size MATCH;
- `magic=0x4155524142544c44`; `boot_from_uefi=0`; reserved padding zero;
- `mmap_count=7` sorted, non-overlapping E820 entries
  (`0x0+0x9fc00`, `0x9fc00+0x400`, `0xf0000+0x10000`, `0x100000+0x7efe000`,
  `0x7ffe000+0x2000`, `0xb0000000+0x10000000`, `0xfffc0000+0x40000`);
- `hhdm_offset=0xffff800000000000`;
- `initrd_phys=0x1800000`, `initrd_size=0xb22000` (11,673,600 B);
- `cpu_count=1`, `bsp_lapic_id=0`, `cpu[0].processor_id=0`, `lapic_id=0`;
- `rsdp_phys=0` (stock BIOS publishes no RSDP with the default LPC identity;
  ACPI identity is G4's opt-in `--lpc=ich9`).

## 4. QEMU cross-check

Same stock `bios.bin`, QEMU 10.0.13, `-machine q35,accel=tcg,sata=off,usb=off`,
`-cpu Haswell`, `-m 128M`, explicit `ich9-ahci` + `qemu-xhci` + blank USB:

| lane | result |
|---|---|
| pinned | `[BL4] ELF parse FAILED`, then `KERNEL.OLD missing or unloadable; halting` |
| candidate | `[BL9] ACPI RSDP found`, `[BL9] ACPI MADT parsed`, `ELF PT_LOAD segments copied`, `entering long mode`, `[kernel] shell active; kmain idling.` |

QEMU's SeaBIOS publishes ACPI (ICH9 LPC on q35), so this lane additionally
shows the Stage-2 fix is compatible with the RSDP/MADT path.  It is a
cross-check, not the x86emu primary receipt.

## 5. What this does not establish

- No claim about the published pinned ISO `d2110f8b…` itself: the accepted
  image is a hash-guarded rebuild from the pinned source plus the reviewed
  Stage-2 patch (see `inputs.txt` for why bit-identity is impossible).
- No SMP/IOAPIC, allocator-zone, CF9-causality, PERF P4 or USB-storage claim.
- No ACPI claim from the x86emu stock lane (`rsdp_phys=0`); ACPI identity is
  measured separately in `docs/evidence/compat-g4/`.
- The release ISO is not redistributed here (81 MiB); only hashes, the
  sector-diff receipt and the reproduction commands are stored.
