# COMPAT G3 — Stage-4 FS/GS and ACPI boundary diagnostics

**Date:** 2026-10-16 (diagnostics). **Status:** superseded by **acceptance** —
see [`acceptance/`](acceptance/README.md) (2026-10-17, patch `0063`).  This
directory keeps the diagnostic trail that root-caused the failure; the runs
below are mechanism tests and are not acceptance evidence.

> **G3 acceptance (2026-10-17):** the pinned AuraLite-OS source `518bef0`
> rebuilt with the release toolchain, with only the reviewed Stage-2 patch
> applied, boots under an unmodified Debian SeaBIOS 1.16.3-2 `bios.bin` to
> `[kernel] shell active; kmain idling.` in x86emu (`fault=0`) and QEMU, with
> a validated 7,776-byte `boot_info_t` at `0x10000`.  The pinned ISO fails at
> `[BL4] ELF parse FAILED` in the identical lane.  Receipts, hashes and the
> sector-diff proof: [`acceptance/`](acceptance/README.md).

## Authority / limits

The authoritative input remains `/home/user/AuraLite-OS-compat/release/auralite.iso`, SHA-256 `d2110f8ba93f739a0ffe1180540fe456a5b2396c3fad375cb7504cb11d8144e7`, with unchanged `kernel.elf` SHA-256 `a1c5264bcf6fbed66a75e0a4a54f3deb6d713842fa64a812cb1a0a05cb647bb3`. The unmodified Stage 2 in that ISO still reaches `ELF parse FAILED`. The historical G3 ROM (`3267c5e4…`) is unavailable; the recreated diagnostic ROM is `69f2819d…`, and the ACPI-ID probe ROM is `012f5d98…`. Neither is a same-ROM historical comparison.

At the time of these diagnostics no accepted pinned-image x86emu BIOS-to-kernel/shell result existed; the 2026-10-17 acceptance closes that gap.  The temporary ISO runs below—including runs under the unmodified stock SeaBIOS—remain mechanism tests only.  A bounded `boot_info_t` layout/field check was captured at kernel entry on the candidate image; the accepted capture is in `acceptance/`.  A limited independent QEMU reference comparison is recorded below, with firmware/transport caveats. No allocator-zone measurement, CF9-causality claim, or PERF P4 pass is established. PERF P4 remains **NO-GO**.

## Stage-4 diagnosis and candidate source fix

SeaBIOS `call32_post()` reloads FS/GS selectors on the AHCI INT 13h path. The BIOS returns with selector `0x0010`, hidden base `0x100`, and limit `0xffff`; Stage 2 had assumed its unreal-mode flat caches survived. `fat_load` then reads the scratch sector through `GS:[ESI]` and writes through `FS:[EDI]`. The baseline first-copy trace saw ELF magic at `0x11400`, zeroes at `0x11500`, and the first destination write at `0x200100` rather than `0x200000`. Thus the `[BL4] kernel.elf loaded` receipt did not mean the ELF image was copied correctly.

The pinned kernel itself has a parser-compatible ELF64 header: 3,073,904 bytes, entry `0xffffffff80191640`, `e_phoff=0x40`, `e_phentsize=56`, `e_phnum=3`; the three load headers fit Stage 2's eight-entry spool. The failure was the corrupted staging bytes, not a mismatch in ELF class or program-header count/size.

A candidate fix is now in the compatibility source tree:

- `disk_read_lba` calls `go_unreal` on both success and exhausted-retry exits, before restoring the saved GPR frame with `popad`.
- `go_unreal` now uses `pushad`/`popad` in 16-bit code, matching its documented full-register preservation contract.
- The Stage-4 comment now documents that INT 13h can reload the hidden FS/GS caches.

The exact source patch is `stage2-fs-gs-restore.patch`. NASM 2.16.03 assembled a 6,656-byte Stage 2, SHA-256 `51ecc150f6eefd84f570f2fbdc808c4f3edd7ac7bf342559c19f652638b85963`. The hash-guarded builder copied the pinned ISO to `/tmp`, replaced only the reserved Stage-2 slot at LBA 34–159, and produced diagnostic ISO SHA-256 `b9d650b93a3894be4c5ea0ddcf8f3cd1bbae9958a00fc0743ee5aef56d0bb905`. The original ISO and kernel hashes remain unchanged; the modified ISO is not stored.

A debugger replay of the modified image at the LBA-128 AHCI return shows FS/GS `base=0x100`, `limit=0xffff` immediately after BIOS return and `base=0`, `limit=0xffffffff` after `go_unreal`. EAX is restored to the original LBA value `0x80` at the helper return. With the ACPI-ID probe ROM and that temporary ISO, the run emits RSDP/MADT, ELF PT_LOAD copy, initrd, page-table and long-mode receipts, followed by Ring-3 init-shell and `shell active` markers; the run ends at its 1.3-billion-step cap with `fault=0`. This is evidence that the source change repairs the diagnosed segment-cache/copy path in this diagnostic setup—not acceptance evidence or a full pinned-image BIOS-to-kernel boot. The authoritative ISO still contains the original Stage 2.

A separate run used the same candidate ISO with the base recreated diagnostic SeaBIOS ROM, without the ACPI-ID addition. It still reported `ACPI RSDP not found; BSP-only`, but loaded `kernel.elf`, copied its PT_LOAD segments, reached long mode and emitted the Ring-3 shell-active marker (`halted=1`, `fault=0`, `cap_reached=1`). This isolates the candidate FS/GS fix from the separate ACPI-ID ROM change. The ISO still contains modified Stage 2 and the ROM remains a recreated diagnostic build; this is not pinned-image acceptance, and this run alone does not capture the `boot_info_t` entry handoff. Its run receipt and captures are `stage2-fs-gs-restore-stock-acpi-key.txt` and `stage2-fs-gs-restore-stock-acpi.*`.

The final INT 13h failure exit was tested too. `make_fat_failure_iso.py` starts from the hash-checked temporary candidate image and changes only the KERNEL.ELF first-cluster FAT link in both FAT copies to bad cluster `0x0ffffff7`; it never writes the pinned ISO. The resulting invalid-LBA read reaches `disk_read_lba`'s exhausted-retry path. At the `stc` breakpoint, FS/GS are flat (`base=0`, `limit=0xffffffff`) and EAX is restored to `0x10000b17`, the invalid LBA. The full negative run reports `kernel.elf load FAILED`, the bounded KERNEL.OLD fallback, and its terminal missing/unloadable receipt, with `fault=0`. This is an intentional error-injection diagnostic, not a boot acceptance test.

An earlier diagnostic-only `stage2-unreal-restore.patch/.bin` placed the helper call after `popad`; it is retained as historical experiment data, not the final candidate. The final patch above keeps the saved register frame live through the helper.

## Candidate boot_info_t handoff check

A temporary debugger breakpoint at the kernel ELF entry (`0xffffffff80191640`) stops before the first kernel instruction. `RDI=0x10000`, matching `BOOT_INFO_PHYS`; the 7,776-byte block there begins with the expected `BOOT_MAGIC`. The remaining field dump and full run capture are preserved in `stage2-bootinfo-candidate.*`.

The Stage 2 header offsets were independently regenerated from the pinned `boot/shared/boot_info.h` by `tools/gen_boot_offsets.c`; `bootinfo-layout.inc` is that generated assembly layout and `bootinfo-layout.txt` contains a host-C `sizeof`/`offsetof` measurement. The measured block has a 32-byte framebuffer struct and 24-byte mmap/CPU entries, with `boot_info_t` size 7,776 and offsets matching those emitted by the OS tool. `inspect_bootinfo_snapshot.py` checks the block in a v2 sparse-RAM snapshot (raw snapshot intentionally omitted; 30,080,378 bytes, SHA-256 `784ca493b84daa8371722ebb86d87c13b5de717caf898717b45d6481bc509925`).

The candidate snapshot reports seven ordered, non-overlapping E820 entries, HHDM `0xffff800000000000`, initrd `0x01800000` / 11,673,600 bytes, one CPU with BSP/APIC ID 0, RSDP `0x000f6210` with a valid revision-0 checksum, and `boot_from_uefi=0`; magic and reserved padding are valid. The kernel subsequently logs the same handoff magic and BIOS path, parses 206 initrd files, and reaches the shell-active marker. This is a bounded structural/selected-field check for the **modified candidate ISO + ACPI-ID diagnostic ROM only**. It does not validate the authoritative x86emu handoff, allocator policy, SMP/IOAPIC behavior, or acceptance; the separate QEMU reference comparison below is limited and not a substitute.

## QEMU BIOS reference comparison (limited)

A Debian `qemu-system-x86` 10.0.13 package was extracted to `/tmp` without system installation (package SHA-256 `5e841d0723a657fd8bc2e9e1a085d24f47fcb3e4a0f94762e6c38b429f22fbb1`). Runs used `q35` + TCG, `-cpu Haswell`, 128 MiB, IDE disk, and Debian SeaBIOS `bios-256k.bin` SHA-256 `fc6d2ea888862e6b26d53fb877ab712324726eb8f44eb6baf076c64a31f1c3fb`; `-snapshot` protected the disk images. Both the unchanged authoritative ISO (SHA-256 `d2110f8b…`) and the temporary candidate ISO (SHA-256 `b9d650b9…`) reached Stage 2, ACPI RSDP/MADT parsing, ELF PT_LOAD copy, kernel/initrd startup, and the shell-active marker in QEMU. The two QEMU runs produced byte-identical 7,776-byte `boot_info_t` blocks at physical `0x10000` (SHA-256 `9119c44956fae7044f125c52c42aaea7d76e8347333516fe74747ee447cbdd42`), so the candidate Stage-2 change made no observed QEMU handoff-field change in this setup.

Comparing that QEMU block with the candidate x86emu block, the selected stable fields match: magic, zero framebuffer, HHDM, initrd address/size, single-CPU/BSP fields, and BIOS path. The firmware-derived fields differ: QEMU reports eight E820 entries and RSDP `0xf52c0`; x86emu reports seven E820 entries and RSDP `0xf6210`. The reserved E820 regions also differ; the complete tuples are in `qemu-bootinfo-comparison.txt`. This is a limited reference comparison, not a cause or allocator-zone conclusion: QEMU used stock Debian SeaBIOS/Q35/IDE, while the x86emu handoff block used the recreated ACPI-ID diagnostic SeaBIOS with AHCI plus a blank USB. The QEMU shell logs, raw small handoff blocks, capture hashes, and exact reproduction commands are included. No equivalence of firmware or acceptance result is claimed.

## Same-stock-ROM xemu A/B (diagnostic)

The current O2 x86emu build ran both the pinned ISO and temporary candidate ISO under the same unmodified Debian SeaBIOS 1.16.3-2 `bios.bin` (128 KiB, SHA-256 `4ee65f3d02c5e37c3c358618334de65c5b161fac6504aaacd39675eb4ecb80ca`), Haswell profile, blank USB + SATA attachments, and instruction cap. Both report no ACPI RSDP/BSP-only. The pinned ISO reaches `[BL4] ELF parse FAILED`; the candidate copies ELF PT_LOAD segments, enters long mode and reaches `[kernel] shell active`. Both terminate without emulator fault (`fault=0`). This controlled same-ROM/device A/B supports the FS/GS repair, but the passing image is modified and is not acceptance evidence.

A separate candidate-only kernel-entry snapshot under the same stock ROM records `RDI=0x10000` and validates the 7,776-byte block's layout, HHDM, initrd, one-CPU fields and seven-entry E820 map; `rsdp_phys=0`. This is not pinned-image handoff validation. Details and captures: `stock-bios-ab.txt`, `stock-bios-candidate-entry.txt`, and `stock-bios-candidate-*`. The 128 KiB `bios.bin` differs from the 256 KiB QEMU BIOS file in the initial reference run.

## QEMU A/B with the same 128-KiB SeaBIOS image

A second QEMU lane uses the exact 128 KiB `bios.bin` above, q35/TCG, Haswell and 128 MiB. Under both explicit AHCI + blank USB and QEMU `if=ide` configurations, the pinned ISO reaches `ELF parse FAILED`, while the temporary Stage-2 candidate reaches shell-active. QEMU finds RSDP/MADT in both images. Within each mode the declared E820 entries and RSDP match between pinned/candidate; the failed pinned run has no initrd (`0/0`), while the candidate has `0x1800000/0xb22000`. Raw `boot_info_t` blocks are not byte-identical: each pair differs in 362 bytes, mostly beyond the declared E820 count. The full field comparison and caveats are in `qemu-samebios-ab.txt`.

Comparing QEMU's AHCI + blank-USB candidate block with the xemu candidate block captured under the same BIOS file and high-level storage setup, selected stable fields match; 19 raw bytes differ. QEMU reports RSDP `0xf67b0` and eight E820 entries; xemu reports no RSDP and seven. SeaBIOS recognizes QEMU q35 LPC `8086:2918` but not xemu's `8086:1e55`; the separate ROM-only ACPI probe confirms the RSDP gate. The remaining E820 differences are not attributed or treated as allocator-zone evidence. This is still not pinned-image acceptance, handoff validation for the authoritative ISO, or a PERF P4 result.

## ACPI table evidence

SeaBIOS `rel-1.16.3` has `CONFIG_ACPI=y`, but `acpi_setup()` only matches PIIX4 PM `8086:7113` or ICH9 LPC `8086:2918`; x86emu advertises LPC `8086:1e55`. A ROM-only diagnostic patch adds `8086:1e55` to the ACPI/FADT match tables. With that probe ROM and the **unchanged pinned ISO / unmodified Stage 2**, SeaBIOS copies an RSDP to `0xf6210`; Stage 2 reports RSDP found and MADT parsed, then still reports `ELF parse FAILED`.

The MADT-body breakpoint reached `acpi_parse_madt_body` with EAX=`0x07fff630` and the `APIC` signature. Snapshot-RAM inspection using the current v2 snapshot reader found: RSDP at `0xf6210`, revision 0, checksum valid; RSDT at `0x07ffe4c0`, length 48, 3 entries, checksum valid; MADT at `0x07fff630`, length 110, checksum valid, Local APIC base `0xfee00000`, flags 1. It contains one enabled Local APIC entry (processor/APIC ID 0), one type-1, four type-2, and one type-4 entries. The raw 19,148,154-byte v2 snapshot is intentionally omitted; SHA-256 `c802a8236633280f8cae3469b730df475bb55ea7d9015994dd8419bbcc70ee3c`. This verifies table structure/checksums only—not FADT/PM behavior, IOAPIC/SMP, or `boot_info_t` ABI.

The debugger originally read immutable flash at `0xf6210` because its physical peek bypassed Q35 PAM. The side-effect-free PAM-aware `mem_debug_peek_byte()` now follows the CPU's selected RAM shadow; `test_pam` covers ROM-vs-shadow inspection. This is tooling-only and does not change guest memory execution.

The no-SATA control with the original diagnostic ROM and blank USB produces no Stage-2 marker and reaches its cap without a halt or fault; it is a media control only. Reset/CF9 observations do not establish CF9 causality.

## Emulator test outcome

After the G3 diagnostic work, the full emulator `make test` and `make test-sanitize` were run and passed (RC 0). `test-snapshot` passes 12 split lanes. Optional frozen-guest lanes skip because the historical `KERNEL/INITRD` fixture is absent; GDB lanes skip because `gdb` is not installed. NASM 2.16.03 was explicitly added to `PATH` for `make test`. QEMU was not installed system-wide at test time; a Debian QEMU 10.0.13 package was later extracted under `/tmp` and used for the limited reference runs below.

The full test first exposed an unrelated snapshot-v1 restore-order defect: load compared the MMIO map before restoring the relocated PCI/AHCI BAR state, refusing the AHCI ABAR at region 5. Snapshot format v2 now restores PCI/AHCI state before validating the MMIO map; v1 files are rejected rather than misread. The 12-lane integration gate, save/load round trip, and sanitizer snapshot suite pass. This tooling correction is separate from the G3 guest-side issue.

## Files

- `pinned-inputs.txt`, `source-cleanup.txt`, `stage2-bytes.txt`, `seabios-call32-segments.txt`, `seabios-acpi-id-mismatch.txt`, and `stage4-snapshot-analysis.txt` — hashes and compact analysis; `blank-usb-1m.img` — exact 1 MiB all-zero USB input.
- `stage2-fs-gs-restore.patch/.bin`, its build log and diagnostic-ISO hash receipt — candidate guest-source change and built Stage 2; `stage2-fs-gs-restore-segments.dbg` and its captures — BIOS-return and post-helper FS/GS registers.
- `stage2-fs-gs-restore-acpi.*` — modified-Stage-2 + ACPI-ID ROM diagnostic run. `stage2-fs-gs-restore-stock-acpi-key.txt` and `stage2-fs-gs-restore-stock-acpi.*` — final candidate with the base diagnostic ROM, no ACPI-ID patch; the temporary ISO itself is not stored.
- `make_fat_failure_iso.py`, `stage2-fs-gs-failure-iso.txt`, and `stage2-fs-gs-failure*` — isolated bad-FAT-link/error-return diagnostic; its two modified FAT entries and resulting media image are outside the bundle.
- `stage2-unreal-restore.patch/.bin` — earlier diagnostic experiment, superseded by the final candidate patch.
- `seabios-diagnostic-128k.bin` and `seabios-acpi-id-probe-128k.bin` with their patches/config/build logs — the distinct diagnostic ROMs.
- `make_diagnostic_iso.py` — hash-guarded temporary ISO builder.
- `acpi-table-verification.txt` — earlier v1 snapshot/table capture. `stage2-acpi-table-verification.txt`, `stage2-acpi-table-parser-v2.stdout`, `inspect_acpi_snapshot.py`, `stage2-acpi-madt-body.dbg`, and `stage2-acpi-table-inspection-v2.*` — current v2 table/checksum/parser-path captures. The raw RAM snapshots are intentionally not stored.
- `stage2-bootinfo-candidate-entry.dbg`, `stage2-bootinfo-candidate-key.txt`, `stage2-bootinfo-candidate-parser.txt`, and its compressed captures — earlier ACPI-ID-ROM candidate entry/boot_info data. `inspect_bootinfo_snapshot.py` parses the omitted snapshots and can optionally write the validated 7,776-byte block. `bootinfo-layout-probe.c`, `bootinfo-layout.txt`, and `bootinfo-layout.inc` — independent header layout measurement and generated offsets.
- `qemu-pinned.serial.gz` / `qemu-candidate.serial.gz`, their stderr captures, the initial 256-KiB-BIOS boot_info blocks, and `qemu-bootinfo-comparison.txt` — limited QEMU reference.
- `qemu-samebios-{ahci,ide}-{pinned,candidate}.*` plus `qemu-samebios-{ahci,ide}-bootinfo-comparison.txt` — controlled 128-KiB stock-BIOS QEMU A/Bs; `qemu-samebios-ab.txt` records scope and interpretation.
- `stock-bios-ab.txt`, `stock-bios-{pinned,candidate}.*`, `stock-bios-candidate-entry.*`, `stock-bios-candidate-bootinfo.bin`, `stock-bios-candidate-handoff-parser.txt`, and `stock-bios-bootinfo-comparison.txt` — same unmodified stock SeaBIOS xemu A/B, candidate-only entry snapshot, and selected-field comparison.
- `qemu-reference-g3.txt`, `qemu-samebios-ab.txt`, `qemu-package-sha256.txt`, `run_qemu_g3_reference.sh`, `run_qemu_g3_samebios.sh`, `capture_qemu_bootinfo.py`, `compare_bootinfo_runs.py`, and `compare_bootinfo_pair.py` — QEMU inputs, reproduction helpers and comparison procedures. No QEMU package/binary or large snapshot is bundled.
- `stage2-acpi-table-inspection.dbg` — PAM-aware debugger read of the RSDP RAM shadow.
- `emulator-make-test-v2.log` and `emulator-make-test-sanitize-v2.log` — passing full emulator gates; `emulator-make-test-unit.log` — unit run after the debugger correction.
- `*-key.txt`, compressed stderr/log captures, and stdout files — selected receipts and compact diagnostic streams; `commands.txt` — reproduction commands and caveats.

`SHA256SUMS` covers all bundle files except itself. Large baseline RAM snapshots, the temporary modified ISO, and full emulator binaries are not duplicated in this compact bundle.