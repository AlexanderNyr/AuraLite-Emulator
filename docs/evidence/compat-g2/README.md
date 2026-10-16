# COMPAT G2 — Stage 2 entry and boot-disk dispatch

Captured 2026-10-15. The G2 Stage-2-entry gate is met in the pinned-image x86emu
lane: the BIOS read receipts succeed and `[BL3] AuraLite stage2 alive` is
repeatable. This is **not** a complete BIOS-to-kernel or shell boot. The positive
run faults later on an unimplemented real-mode opcode; that is a separate
post-entry boundary.

## Pinned inputs and artifact identity

| Item | Identity |
|---|---|
| Emulator source HEAD at capture | `475a0dc48e9b560753d0e759fcf01e379ad0e625` (worktree has prior G2 changes plus this SGDT fix) |
| Built x86emu executable | SHA-256 `8d9017805de57da9a054ed9c430328f88fd45fa7d68fccdb7c6f67717b7910e7` |
| AuraLite-OS compatibility checkout | `518bef075f0eed2b24c5cac8ce3f8a741dfd7cfe` |
| Full ISO | 84,934,656 bytes; SHA-256 `d2110f8ba93f739a0ffe1180540fe456a5b2396c3fad375cb7504cb11d8144e7` |
| Empty USB image | 1,048,576 zero bytes; SHA-256 `30e14955ebf1352266dc2ff8067e68104607e750abb9d3b36582b8af909fcb58` |
| SeaBIOS base | upstream `rel-1.16.3`, commit `a6ed6b701f0a57db0569ab98b0661c12a6ec3ff8` |
| Instrumented SeaBIOS ROM | `/tmp/seabios-src/out/bios.bin`, 131,072 bytes; SHA-256 `6762769de5a1902f56490e31be3496f7c97e9360a55a9c43797797a37f895c4b` |

The ROM is a local diagnostic build of the SeaBIOS base, with debug logging in
`block.c`/`disk.c` and the experimental `movl %ebx,%eax` in `stacks.c`; the
changes are preserved as `seabios-diagnostic.patch`. It is not a pristine
upstream ROM. The x86emu run used the real Q35 PAM/shadow implementation and
did **not** use the diagnostic always-writable-ROM bypass.

## Root cause and fix

The observed `op=000e0000` was caused by x86emu's SGDT/SIDT handler writing an
extra zero word at `addr+6` outside 64-bit mode. SeaBIOS's `call32_prep()`
executes `SGDT [ESP+2]`; in this stack layout the descriptor occupies EFF32
through EFF37 and saved EBX begins at EFF38. The extra zero word overwrote the
low half of the saved EBX value `000eff88`, so `pop ebx` at `F000:AA1B` restored
`000e0000`. The old watchpoint evidence and exact disassembly are in
`dispatch-diagnosis.txt`.

`src/cpu.c` now writes only the architectural six bytes in non-long mode and
10 bytes in 64-bit mode. `tests/test_cpu.c` adds a sentinel regression using
the same `67 66 0F 01 44 24 02` real-mode form. `make test-unit` passed after
the change; this includes the CPU, Q35 PAM and CMOS/RTC vectors.

## Positive pinned-image / SATA-only lane

`commands.txt` records the exact invocation. The ISO is attached with
`--sata`; `--disk` points to a separate blank USB image, so this is an explicit
SATA-only media diagnostic. No always-writable-ROM option or patch was used.

The run logs show:

- `G2 p32 op=000eff88 ... cmd=2 count=1 lba=0 buf=00007c00`;
- `G2 basic status=0 count=1` (boot-sector read succeeds);
- an extended read with `count=126 lba=34 buf=00008000`; and
- `[BL3] AuraLite stage2 alive`.

Two post-fix positive replays reached the same receipts. The targeted watch
capture is retained as `x86emu-sata-positive-traced.*`; at `F000:AA1D` it
shows EBX restored as `000eff88` and no SGDT zero-word store into that slot.
The clean replay then exits with status 2 on `#UD unsupported opcode 0x60` at
`CS=0800:IP=0A0B` (linear `0x8A0B`); final state is `FAULT`. In 16-bit mode
opcode `0x60` is PUSHA. This occurs after the Stage 2 alive receipt and is the
next observed CPU/Stage-2 boundary; E820/ACPI/kernel handoff and a complete OS
boot are not yet established. Keep it separate from G1's original relocation
panic and from the earlier diagnostic-only SHRD fault.

## Negative media lane

The negative run used the same blank USB image and BIOS, but omitted `--sata`
so no ISO was attached. It emitted `Boot failed: could not read the boot disk`
and `No bootable device. Retrying in 60 seconds.`, reached `HALTED`, and did
not print the Stage 2 alive marker. This is a negative media control, not proof
of the origin of any CF9 writes.

## BIOS-shadow and media tests

`make test-unit` passed. In particular, `tests/test_pam.c` reports
`q35_pam_shadow_and_protection` and covers positive writable-shadow reads,
read-only behavior, flash fallback, disabled-shadow behavior, subregion
routing, and reserved bits. The RTC tests cover the CMOS memory-size surface.
The two firmware runs add positive/negative media coverage while exercising
the real PAM path. No allocator-zone measurement is claimed.

## Contents

- `dispatch-diagnosis.txt` — disassembly and saved-stack-slot proof for the
  SGDT over-store and the post-fix dispatch receipt.
- `x86emu-sata-positive.stdout` / `.stderr` — complete clean positive run outputs.
- `x86emu-sata-positive-traced.stdout` / `.stderr` — earlier post-fix replay
  with the targeted CPU trace and physical-stack watchpoint enabled.
- `x86emu-empty-media.stdout` / `.stderr` — complete negative media outputs.
- `emulator-make-test-unit.log` — complete unit-test run (exit 0).
- `seabios-diagnostic.patch` — local diagnostic SeaBIOS edits; no ROM-shadow
  bypass is included.
- `commands.txt` — reproducible commands and caveats.
- `SHA256SUMS` — hashes of the evidence artifacts (not the manifest itself).
