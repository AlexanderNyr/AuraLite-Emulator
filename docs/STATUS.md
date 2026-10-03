# Implementation status

## Working now

- The sample 128 KiB firmware image can be loaded at the x86 reset vector.
- Reset execution supports real mode, protected mode, compatibility mode and long mode.
- All five selectable Intel-style platform profiles are available.
- The EHCI model reaches virtual USB Mass Storage transfers when the loaded firmware requests them.
- The physical address decoder handles overlapping ECAM and fixed BAR regions by choosing the most-specific region.
- A framebuffer is available as `framebuffer.ppm`.
- The CPU, memory, PCI and device models do not depend on a particular firmware name or binary layout.
- A PC-compatible dual 8259A PIC (ports 0x20/0x21/0xA0/0xA1) delivers maskable hardware
  interrupts at instruction boundaries: ICW1-4 init, OCW1 mask, OCW2 EOI, OCW3 IRR/ISR
  readback, fixed priority, slave cascade through master IRQ2, plus STI shadow and HLT
  wake semantics (CHIPSET H0; `pic_raise_irq()` is the device-side line API).

## Current limits

This is a general firmware-focused x86 emulator, not yet a drop-in replacement for QEMU. The CPU ISA is expanded from execution traces and tests. Some chipset blocks are behavioural models rather than cycle-accurate implementations. The descriptor/CBW/CSW path and live web monitor are still being expanded.
