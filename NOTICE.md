# Notices

The sample firmware source in `firmware/sample_firmware.nas` and its associated
sample ROM are derived from the firmware project that supplied the original
sample image. The applicable MIT license is included at `firmware/LICENSE`
(Copyright (c) 2026 $yscall). That license covers the firmware directory only.

The emulator core and surrounding tooling in `src/`, `tests/`, `tools/`,
`web/`, `docs/`, the `Makefile` and `patches/` are licensed under the Apache
License, Version 2.0 — the repository-level license in `LICENSE`.

Third-party names and CPUID/PCI identifiers are used descriptively; no
affiliation with Intel, SeaBIOS, QEMU or the AuraLite OS project is implied.
The diagnostic ROMs under `docs/evidence/` are local builds of SeaBIOS
(`rel-1.16.3`, distributed upstream under LGPLv3), retained only as test
inputs for the compatibility evidence; they are not part of the emulator
binary or of the Apache-2.0 licensed sources.
