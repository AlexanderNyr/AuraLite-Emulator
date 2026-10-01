# Implementation status

## Working now

- The sample 128 KiB firmware image can be loaded at the x86 reset vector.
- Reset execution supports real mode, protected mode, compatibility mode and long mode.
- All five selectable Intel-style platform profiles are available.
- The EHCI model reaches virtual USB Mass Storage transfers when the loaded firmware requests them.
- The physical address decoder handles overlapping ECAM and fixed BAR regions by choosing the most-specific region.
- A framebuffer is available as `framebuffer.ppm`.
- The CPU, memory, PCI and device models do not depend on a particular firmware name or binary layout.

## Current limits

This is a general firmware-focused x86 emulator, not yet a drop-in replacement for QEMU. The CPU ISA is expanded from execution traces and tests. Some chipset blocks are behavioural models rather than cycle-accurate implementations. The descriptor/CBW/CSW path and live web monitor are still being expanded.
