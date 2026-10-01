# Firmware execution notes

The emulator is firmware-agnostic. This document records optional compatibility observations for the sample ROM shipped in `firmware/`; they are not assumptions required by the CPU core.

## Sample ROM launch mode

The sample ROM contains a byte that selects its post-boot launch mode. The emulator does not patch it. A different ROM can be supplied with `--rom=path`.

## Sample ROM page-table and jump assumptions

The sample ROM's long-mode transition uses hand-maintained addresses and a fixed page-table layout. If a ROM uses different addresses, the generic emulator simply executes those values and reports the resulting fault or halt.

## Device models

The device layer exposes standard-style PCI, ECAM, fixed BAR, DDR-controller, framebuffer and EHCI/USB Mass Storage models. Firmware-specific compatibility workarounds belong in a ROM image or in a separately selected platform profile, not in the generic CPU core.
