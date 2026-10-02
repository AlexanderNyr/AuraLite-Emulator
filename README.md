# Universal x86 Emulator

A from-scratch C emulator for raw x86 firmware, with a switchable Intel platform profile and a real firmware execution path.

## Current scope

- x86 interpreter starting in real mode and transitioning through protected mode, compatibility mode and long mode.
- 16/32/64-bit general registers, ModRM/SIB addressing, segmentation, GDT/IDT loading, CR0/CR3/CR4, EFER/MSR, CPUID, paging (4-level, 2 MiB/1 GiB pages), I/O instructions, PCI config-space access and firmware-oriented integer instructions.
- Integer ISA per `docs/plans/CORE_PLAN.md`: all conditional branches (short and near, all 16 conditions), the full shift/rotate group with correct count masks and CF/OF, MUL/IMUL/DIV/IDIV with `#DE`, SMSW/LMSW, SGDT/SIDT, PUSHF/POPF, software interrupts `INT n`/`INT3`/`INTO` delivered through the real-mode IVT or the protected/long-mode IDT, and `IRET`/`IRETQ` returning through the hardware-ordered exception frame.
- Physical address space with 128 MiB RAM, 128 KiB ROM and ROM aliases at `0xE0000` and `0xFFFE0000`.
- PCI mechanism #1 and ECAM/MMCONFIG.
- Platform profiles for Sandy Bridge, Ivy Bridge, Haswell, Broadwell and Bay Trail.
- Behavioural DDR3/DDR3L/DDR4 training register models, CAR scratch, GPU framebuffer, SATA/LPC stubs, and an EHCI/USB mass-storage model that walks guest queue descriptors and reads a disk image.
- A 64-bit test kernel and a PPM framebuffer dump path.
- Circular event log and optional instruction trace.

This is intentionally a firmware-focused, multi-purpose emulator, not yet a drop-in replacement for QEMU: the CPU and devices are implemented locally rather than delegated to an existing virtualization library.

## Build

Dependencies: GCC/Clang, NASM, Python 3.

```sh
make
make firmware/firmware.bin disk/disk.img
```

## Run

The emulator returns exit code `0` for a normal stop or instruction-limit stop and exit code `2` when the CPU enters a fault state. Input-file errors return exit code `1`.

The unmodified firmware is useful for observing its actual behaviour and faults:

```sh
./x86emu --platform=haswell --max-instr=200000 --trace
```

Run any compatible ROM directly; the emulator does not rewrite the guest image. For the bundled sample:

```sh
./x86emu --rom=firmware/firmware.bin --platform=haswell \
  --max-instr=200000 --dump-fb=framebuffer.ppm --log=boot.log
```

Select other profiles with `--platform=sandybridge`, `ivybridge`, `broadwell`, or `baytrail`. `--disk=...` supplies the virtual USB storage image. `--rom=...` supplies another firmware image.

## Testing

Run the CPU unit tests and the platform smoke tests with:

```sh
make test
```

The focused CPU tests can be run independently:

```sh
make test-unit
```

## Architecture

`src/cpu.c` is the instruction interpreter. `src/mem.c` and `src/io.c` implement the physical and port address spaces. `src/pci.c` implements PCI config mechanisms. `src/devices.c` contains the platform models and EHCI/USB storage behaviour. `src/main.c` is the CLI harness.

The next GUI layer can consume the machine event ring and framebuffer without changing the emulation core; the framebuffer export is currently PPM so it works without external graphics dependencies.

## License

The sample firmware carries its applicable license in `firmware/LICENSE`.
The emulator core does not currently declare a repository-level license; add
one before publishing if you want to grant redistribution and reuse rights.
