# COMPAT G4 — LPC identity pairing: stock SeaBIOS publishes ACPI

Captured 2026-10-17. **Scope:** one emulator-side compatibility boundary —
the LPC bridge identity — and its measured effect on ACPI publication by an
*unmodified* SeaBIOS. This closes the emulator half of the G3 ACPI finding
only; it is **not** a pinned-image BIOS-to-kernel boot, not shell acceptance,
and not an E820/`boot_info_t` agreement claim.

## The boundary

G3 measured that SeaBIOS `rel-1.16.3`'s `acpi_setup()` recognizes exactly two
LPC functions as ACPI/FADT-capable: PIIX4 PM `8086:7113` and ICH9 LPC
`8086:2918`. This machine already advertises the Q35 host bridge
(`8086:29c0`, `src/pci.h`) and implements its seven PAM registers
(`src/mem.c`), but the LPC function identified as Panther Point `8086:1e55`
— the ID the bundled sample firmware knows. Result: stock SeaBIOS completes
POST and publishes **no RSDP**, so the guest boots BSP-only with no ACPI
surface.

G4 makes the identity pairing switchable:

- `machine_t.cfg_lpc_ich9` (`src/machine.h`), set by `--lpc=ich9`
  (`src/main.c`); the default (`--lpc=pantherpoint`) leaves the historical
  `8086:1e55` untouched, so every prior lane keeps its receipt.
- `devices_init_common()` (`src/devices.c`) adds the LPC function with
  `0x2918` when the flag is set. Class `06/01`, the multifunction header bit
  and function 2 (AHCI `8086:1e03`) are unchanged.
- `tests/test_compat.c` pins 7 vectors: both identities, class/header,
  host bridge `8086:29c0`, AHCI function, and Q35 PAM under the ICH9 identity
  (`make test-compat`, also inside `make test` / `make test-sanitize`).

## Measured receipts

Inputs are the G3 bundle's own ROMs (hashes re-checked):

| Input | SHA-256 |
|---|---|
| `seabios-diagnostic-128k.bin` (ACPI match table **unmodified**) | `69f2819d10c6ea27506462e2d1ecafcd8d0a30e7e377d45f3f99a1996e8a544d` |
| `seabios-acpi-id-probe-128k.bin` (diagnostic ROM patch adds `1e55`) | `012f5d983b4768bbd463cafa4350933469b32ee822a3872f8acefe98775b97c5` |
| `blank-usb-1m.img` (1 MiB zeroes) | `30e14955ebf1352266dc2ff8067e68104607e750abb9d3b36582b8af909fcb58` |

Run shape: `--platform=haswell --max-instr=200000000`, blank USB only, exit 0.

1. **Negative control** — default identity, diagnostic ROM:
   end-of-run snapshot scan = `NO RSDP`.
2. **Scanner control** — probe ROM (the ROM-side patch G3 measured), default
   identity: `RSDP @0xf6210` rev 0, checksum ok, RSDT `0x7ffe4c0`, 3 entries.
3. **G4 lane** — `--lpc=ich9` with the **same unmodified** diagnostic ROM:

   ```
   ACPI DSDT=0x07ffe4b0
   Copying ACPI RSDP from 0x00006dfc to 0x000f6210
   RSDP  @0x000f6210 rev=0 checksum=ok -> RSDT @0x07ffe470
     RSDT @0x07ffe470 len=52 checksum=ok, 4 entries
     FACP @0x07ffff80 len=116 checksum=ok
     SSDT @0x07fff720 len=2073 checksum=ok
     APIC @0x07fff630 len=110 checksum=ok (local APIC base 0xfee00000)
     MCFG @0x07fff5f0 len=60 checksum=ok
   ```

   The ICH9 identity yields one **extra** table versus the ROM-patch control:
   `MCFG`, because SeaBIOS now follows its full Q35/PCIe path (the probe ROM
   only taught the old match table about `1e55`; it did not change PCIe
   capability detection).

4. **Regression** — `./x86emu --lpc=ich9 --stats` on the bundled sample
   firmware still ends `HALTED`, `executed=4837677`, `GUEST_OK`,
   `pixel[0]=0x00200000`; the default run is unchanged.

## Files

- `acpi-identity-receipt.txt` — the four measured lanes above, verbatim.
- `scan_acpi_snapshot.py` — walks RSDP → RSDT → tables in a v2 AURASNP1
  snapshot and prints per-table checksum verdicts.
- `commands.txt` — exact reproduction commands and caveats.
- `SHA256SUMS` — bundle hashes.

Large end-of-run snapshots (~19 MB, sparse) are intentionally not stored.

## What remains open

- **G3**: the authoritative release ISO still carries the original Stage 2;
  the candidate FS/GS restoration exists only in the compatibility source
  tree. G4 does not change that.
- **G5**: pinned-image acceptance under a stock BIOS requires the Stage-2
  change to land in the ISO, then E820/RSDP/`boot_info_t` field agreement
  measured on *that* image.
- No allocator-zone, CF9-causality or PERF P4 result is claimed here.
