# AuraLite Emulator Address Map

The machine's physical address space, its I/O ports, its PCI topology and the
linear map the direct-kernel lane hands to the guest. Everything here is
measured from the tree (`src/machine.h`, `src/mem.c`, `src/devices.c`,
`src/*.h`), not paraphrased; the register-level contracts live in each module's
header.

For how these pieces fit together, see [`architecture.md`](architecture.md).

## Physical memory

| Region | Address | Size | Notes |
|---|---|---|---|
| RAM | `0x00000000` – `0x07FFFFFF` | 128 MiB | `RAM_SIZE`; `calloc`-zeroed at reset |
| Conventional low RAM | `0x00000000` – `0x0009FFFF` | 640 KiB | IVT, BDA, loader block, real-mode stacks |
| PAM decode window | `0x000C0000` – `0x000FFFFF` | 256 KiB | Q35 PAM bytes `0x90..0x96` select RAM shadow per 16 KiB block |
| ROM alias (low) | `0x000E0000` – `0x000FFFFF` | 128 KiB | **inside `m->ram`**: seeded from flash, mutated by guest stores; the pristine image stays in `m->rom` |
| VGA text buffer | `0x000B8000` | 4 KiB | mode-3 cells, 80×25 × (char, attr) |
| GPU VRAM / framebuffer | `0xD0000000` | 16 MiB | `GPU_VRAM_BYTES`; the published display is 800×600×32 at offset 0 |
| PCI ECAM / MMCONFIG | `0xE0000000` | 256 MiB | enabled by the host bridge; runtime-registered MMIO |
| EHCI BAR0 | `0xFEB00000` | 4 KiB | Intel `8086:1E26` at `0:03.0` |
| AHCI ABAR (ctrl 0) | `0xFEB10000` | 4 KiB | `8086:1E03` at `0:1F.2`, 6 ports |
| AHCI ABAR (ctrl 1, `--ahci2`) | `0xFEB12000` | 4 KiB | `8086:2922` at `0:1F.3` |
| xHCI BAR0 | `0xFEB20000` | 64 KiB | `1b36:000D` at `0:14.0`, 4 ports, MSI-X table at `+0x3000` |
| e1000 MMIO | `0xFEBC0000` | 128 KiB | `8086:100E` at `0:04.0`, only with `--tap=` |
| I/O APIC | `0xFEC00000` | 4 KiB | 82093AA, 24 redirection entries |
| Local APIC | `0xFEE00000` | 4 KiB | xAPIC per vCPU (relocatable via `IA32_APIC_BASE`) |
| DDR3/DDR4 register window | `0xFED10000` | 64 KiB | the firmware's memory-training probe target |
| CAR scratch window | `0xFEF00000` | 4 MiB | cache-as-RAM |
| ROM decode (top of 4 GiB) | `0xFFE00000` – `0xFFFFFFFF` | 2 MiB | the 128 KiB image is tiled; the reset vector is the last 16 bytes at `0xFFFFFFF0` |

Decode precedence is MMIO (smallest matching region wins) → ROM → RAM, and the
A20 gate masks bit 20 for reads, writes and fetches. The two ROM windows are the
architectural aliases: `0xE0000` (legacy) and top-of-4-GiB (reset).

### Why the 2 MiB top window

The flash is 128 KiB but the chipset decodes 2 MiB at the top of 4 GiB so the
A20-masked image of the reset window still lands in flash: with bit 20 forced
low, `0xFFFFFFF0` becomes `0xFFEFFFF0`, which must remain ROM for the reset
fetch to work (CF5, recorded in `CHANGELOG.md`).

## The direct-kernel lane (guest-visible low memory)

`--kernel=` skips firmware entirely, so the emulator itself publishes the block
that a real loader would have published. Constants are in `src/kloader.h`;
offsets of `boot_info_t` are pinned by `_Static_assert` against the companion
OS's own `boot/shared/boot_info.h`.

| Region | Physical | Size | Notes |
|---|---|---|---|
| Page tables | `0x00010000` – `0x0001FFFF` | 64 KiB | PML4 + PDPT + PD + PTs, bump-allocated; hard limit `KLOAD_PT_LIMIT` |
| `boot_info_t` | `0x00020000` | 7,776 B | `KLOAD_INFO_PHYS`; passed in `RDI` |
| Loader memmap region | `0x00020000` – `0x00020FFF` | 4 KiB | published as `BOOT_MEM_BOOTLOADER` |
| Temporary stack top | `0x00070000` | — | `RSP` for the guest's `boot.asm` |
| ACPI table set | `0x0009F000` | 16 KiB | `KLOAD_ACPI_BASE`: RSDP/RSDT/XSDT/MADT/FADT/DSDT |
| Lowest legal kernel segment | `0x00100000` | — | `KLOAD_KERNEL_MIN` |
| Kernel segments | `p_paddr − KERNEL_VMA` | — | must fit guest RAM, `.bss` tail zero-filled |

| Guest virtual constant | Value | Source of truth |
|---|---|---|
| `KERNEL_VMA` | `0xFFFFFFFF80000000` | the guest's `kernel.ld` |
| HHDM offset | `0xFFFF800000000000` | the `boot_info` ABI |
| Identity map | `0..4 GiB` as four 1 GiB PS entries | shared PDPT with the HHDM view |
| Kernel map | higher-half range, 4 KiB PTEs | 7 table pages for the reference kernel |

## I/O port space

| Ports | Device | Model notes |
|---|---|---|
| `0x0020`–`0x0021`, `0x00A0`–`0x00A1` | 8259A PIC master / slave | full ICW/OCW, spurious IRQ7/15, AEOI, level lines |
| `0x0040`–`0x0043`, `0x0061` | 8254 PIT | three counters, mode 0/2/3 behaviour, gate on ch2; `0x61` also exposes ch2 OUT |
| `0x0060`, `0x0064` | 8042 KBC | set-1 scancode injection (`--keys`, `--keys-at`) |
| `0x0070`–`0x0071` | MC146818A RTC / CMOS | BCD or binary, 12/24 h, settable epoch, `0x5F` = extra CPUs |
| `0x0092` | System Control Port A | A20 gate, fast reset |
| `0x01CE`, `0x01CF` | Bochs VBE dispi | index/data; QEMU-compatible `vbe_fixup_regs` clamps |
| `0x03CE`–`0x03CF` | VGA graphics controller | `VGA_GR06` graphics-mode bit (mode 3 ↔ graphics) |
| `0x03D4`–`0x03D5` | VGA CRTC | cursor position (`0x0E`/`0x0F`) |
| `0x03F8`–`0x03FF` | 16550 COM1 | the firmware's `GUEST_OK` receipt channel |
| `0x0402` | SeaBIOS debugcon | G2-era probe output |
| `0x0B000`–`0x0B03F` | ACPI PM block (`PMBASE`) | PM1_STS/EN/CNT, 24-bit TMR at `PMBASE+8`, GPE0; PMBASE is programmable, `0xB000` is the Q35/SeaBIOS default |
| `0x0C040`–`0x0C05F` | UHCI BAR4 | `8086:7020` at `0:01.2` |
| `0x0CF8`–`0x0CFF` | PCI config mechanism #1 | address/data pair |

## PCI topology

PCI is walked by bus/device/function through both mechanism #1 and ECAM. The
default tree (before `--tap=`, `--virtio-*`, `--ahci2` add functions):

| Bus:Dev.Fn | Function | IDs | Class | Notes |
|---|---|---|---|---|
| `00:00.0` | Q35 host bridge (MCH) | `8086:29C0` | `0600` | owns ECAM enable, PAM bytes |
| `00:01.0` | PCI bridge (misc) | `8086:0101` | `0604` | |
| `00:01.2` | UHCI companion | `8086:7020` | `0C03` | BAR4 `0xC040`, removable with `--no-usb-uhci` |
| `00:02.0` | integrated GPU | `8086:0412` | `0300` | BAR0 = `0xD0000000`; programming it flips the display |
| `00:03.0` | EHCI | `8086:1E26` | `0C03` | high-speed USB 2.0, port routing |
| `00:04.0` | e1000 NIC | `8086:100E` | `0200` | **opt-in** with `--tap=` |
| `00:05.0` | virtio-blk | `1AF4:1042` | `0100` | **opt-in** with `--virtio-blk=`; modern 1.x, MSI-X |
| `00:06.0` | virtio-net | `1AF4:1041` | `0200` | **opt-in** with `--virtio-net=`; MSI-X |
| `00:14.0` | xHCI | `1B36:000D` | `0C03` | QEMU-compatible, 4 ports, MSI-X cap @ `0x90` |
| `00:1F.0` | LPC bridge | `8086:1E55` / `8086:2918` | `0601` | `--lpc=ich9` switches to the Q35 pair identity |
| `00:1F.2` | AHCI SATA | `8086:1E03` | `0106` | 6 ports, INTx |
| `00:1F.3` | AHCI SATA #2 | `8086:2922` | `0106` | **opt-in** with `--ahci2` (or implied by `--sata2=`) |

Identity is a contract, not decoration: `--lpc=ich9` exists because stock
SeaBIOS only publishes `RSDP/RSDT/FACP/SSDT/MADT/MCFG` when it sees the ICH9
LPC function paired with the Q35 host bridge (COMPAT G4).

### Interrupt routing

| Source | Legacy route | APIC route | MSI |
|---|---|---|---|
| PIT counter 0 | IRQ0 (8259) | GSI2 (MADT override) | — |
| RTC | IRQ8 (slave) | GSI8 | — |
| KBC | IRQ1 (8042) | GSI1 | — |
| COM1 | IRQ4 | GSI4 | — |
| AHCI ctrl 0 / 1 | IRQ 14+ctrl (`0x3C`/`0x3D` seeded in config space) | per IOAPIC RTE | — |
| xHCI | INTA# | per RTE | MSI-X, 16 entries |
| e1000 | IRQ11 / INTA#, `(ICR & IMS)` level | per RTE | — |
| virtio-blk / net | INTA# | per RTE | MSI-X, `queues + 1` entries |

Delivery order at an instruction boundary is **LAPIC first, then 8259**, and
the 8259 INTR line is wired to the BSP only — an AP wakes exclusively on its own
LAPIC.

## Snapshot shape

A snapshot is only loadable into a machine of the same shape. The fingerprint
compares platform, ROM length, kernel entry, vCPU count, and the length of every
attached disk image (`disk`, `sata_img/len` per controller and port) — see
`src/snapshot.c` and [`architecture.md` sSnapshots](architecture.md#snapshots).
