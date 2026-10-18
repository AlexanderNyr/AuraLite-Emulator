# Universal x86 Emulator

A from-scratch C emulator for raw x86 firmware, with a switchable Intel platform profile and a real firmware execution path.

## Current scope

- x86 interpreter starting in real mode (`0xFFFFFFF0` reset vector) and transitioning through 16/32-bit protected mode, compatibility mode and 64-bit long mode, with a 256-entry success-only translation TLB (PERF P2) and a 512-slot RAM-only prefix/opcode predecode cache (PERF P3).
- 16/32/64-bit general registers, ModRM/SIB addressing, segmentation, GDT/IDT/TSS (`LTR`, IST stack switching, Ring-3 ↔ Ring-0 transitions), CR0/CR2/CR3/CR4 (`SMEP`/`SMAP`), EFER/MSR, CPUID, RDTSC, paging (4-level, 4 KiB / 2 MiB / 1 GiB pages), `SYSCALL`/`SYSRET`, `SWAPGS`, SSE media subset (16×128-bit `XMM0..XMM15`), I/O instructions, and the full firmware/OS integer ISA per `docs/plans/CORE_PLAN.md`.
- Physical address space with 128 MiB RAM, 128 KiB ROM, ROM aliases at `0xE0000` and `0xFFFE0000`, Q35 PAM (`0x90..0x96`) shadow RAM decode for `0xC0000–0xFFFFF`, and A20 gate masking.
- PCI mechanism #1 (`0xCF8`/`0xCFC`) and 256 MiB ECAM/MMCONFIG (`0xE0000000`), Q35 host bridge (`8086:29c0`), switchable LPC identity (`8086:1e55` default or `--lpc=ich9` `8086:2918` for stock SeaBIOS ACPI table publication, COMPAT G4).
- Full PC/AT + APIC chipset (`docs/plans/CHIPSET_PLAN.md`, `docs/plans/COMPAT_PLAN.md`): dual 8259A PIC, 8254 PIT, 24-bit ACPI PM timer at `PMBASE+8` (3.579545 MHz = 3×PIT, COMPAT G5), MC146818A RTC/CMOS (including `0x34`/`0x35` high-RAM and `0x5F` SMP CPU count), 8042 KBC with Set-1 scancode injection (`--keys`, `--keys-at`), port `0x92`/`0xCF9` warm reset, xAPIC (`0xFEE00000`, timer, self/peer/broadcast IPI, INIT/SIPI) and 82093AA I/O APIC (`0xFEC00000`, 24 RTEs).
- Up to 8 vCPU execution contexts (`--smp=N`, `EM_MAX_VCPU = 8`) in deterministic 1:1 round-robin with shared virtual time (`vtime_instr`) and HLT-idle clock yield (`docs/plans/KERNEL_BOOT_PLAN.md` K6–K8, `docs/plans/COMPAT_PLAN.md` G6).
- Storage, USB, video, and networking:
  - **AHCI SATA** (`src/ahci.c`, `docs/plans/STORE_PLAN.md` S0–S5): command-list/FIS/PRDT engine, `IDENTIFY`, `READ/WRITE DMA EXT`, 128 KiB transfers, up to 2 controllers (`--sata=`, `--sata-portN=`, `--ahci2`, `--sata2=`, `--sata-writethrough`), INTx delivery.
  - **USB** (`src/devices.c`, `docs/plans/USB_PLAN.md` U0–U5): EHCI (`0:29.0`) + UHCI companion (`0:1.2`, `--no-usb-uhci`) with control/bulk schedules and BOT/SCSI mass storage (`INQUIRY`, `READ CAPACITY(10)`, `TEST UNIT READY`, `REQUEST SENSE`, `READ(10)`).
  - **Video** (`src/devices.c`, `docs/plans/VIDEO_PLAN.md` V0–V4): 800×600×32 linear framebuffer (`0xD0000000`), VGA text mode 3 (`0xB8000` + CRTC cursor + 8×16 PSF2 font, `--no-fb`, `--dump-text`), and Bochs VBE dispi (`0x1CE`/`0x1CF`, `--vbe=WxHxBPP|off`).
  - **Networking** (`src/e1000.c`, `tools/tap_responder.py`, `docs/plans/NET_PLAN.md` N0–N3): Intel 82540EM (`e1000`) with TAP-over-UDP (`--tap=host:port`) and a host responder speaking ARP, DHCP, ICMP, UDP/TCP DNS and 16-way concurrent TCP.
- Direct ELF64 kernel loader (`--kernel=`, `--initrd=`, `--cpus=`) and full BIOS boot route (`SeaBIOS → Stage 1 → Stage 2 → kernel.elf → init` on 1 to 4 vCPUs).
- Non-perturbing tooling (`docs/plans/TOOLING_PLAN.md` T0–T5): standalone disassembler (`x86dis`), instruction trace with CPU-vs-disassembler self-check (`--trace-file=`), interactive debugger REPL (`--debug`, `--debug-script=`, `--symbols=`), GDB RSP server (`--gdb=`), whole-machine `AURASNP1` v2 snapshots (`--snapshot-save=`, `--snapshot-load=`), and live HTTP/WebSocket browser monitor (`--monitor=`).

This is intentionally a firmware-focused, multi-purpose emulator, not yet a drop-in replacement for QEMU: the CPU and devices are implemented locally rather than delegated to an existing virtualization library.

## Build

Dependencies: GCC/Clang, NASM, Python 3.

```sh
make
make firmware/firmware.bin disk/disk.img
```

### Windows 10 / 11 (`build.bat`)

On Windows 10/11 you can build native `x86emu.exe` and `x86dis.exe` binaries with **MinGW-w64 GCC** (MSYS2 / WinLibs) using the POSIX/Winsock2/ELF compatibility layer in `win32/`, or delegate to **WSL2** (see [`docs/WINDOWS.md`](docs/WINDOWS.md) for the full Windows guide):

```bat
build.bat           REM Build x86emu.exe, x86dis.exe, and (if nasm is in PATH) firmware/disk images
build.bat demo      REM Build and run the Haswell sample firmware demo
build.bat monitor   REM Start the live browser monitor at http://127.0.0.1:8080/
build.bat test      REM Run the native Windows C unit tests + Python G3/G5/G6 acceptance gates
build.bat wsl test  REM Delegate full `make test` to WSL2
```

## Run

The emulator returns exit code `0` for a normal stop or instruction-limit stop and exit code `2` when the CPU enters a fault state. Input-file errors return exit code `1`.

The unmodified firmware is useful for observing its actual behaviour and faults:

```sh
./x86emu --platform=haswell --max-instr=200000 --trace
```

`--trace` logs mode/RIP/opcode into the log ring. For a full disassembled
trace (one line per instruction, with a CPU-vs-disassembler self-check and a
summary on stderr) use `--trace-file=trace.txt`, optionally windowed with
`--trace-from=VTIME --trace-count=N`. `make x86dis` builds an
ndisasm-style disassembler for raw binaries (`./x86dis -b64 file.bin`).

A built-in debugger stops before the first instruction with `--debug`
(commands on stdin; `help` lists them; Ctrl-C pauses a running guest):

```sh
./x86emu --platform=haswell --debug
(x86dbg) break 0x100000
(x86dbg) continue
(x86dbg) x/8i $rip
(x86dbg) watch w 0xd0000000 4
```

`--debug-script=file` runs the same commands from a file, and `--symbols=elf`
(default: the `--kernel` ELF) enables `break kmain`, symbolized `x/i` and `bt`.

For gdb, start the emulator with `--gdb=1234` and connect:

```sh
./x86emu --kernel=kernel.elf --initrd=initrd.tar --gdb=1234 &
gdb kernel.elf -ex 'target remote :1234' -ex 'break kmain' -ex continue
```

vCPUs appear as threads, hardware watchpoints work, and `monitor help` lists the
built-in debugger commands. Addresses are linear (in real mode, `$pc` is IP).

### Live monitor

The live monitor serves the browser UI and a WebSocket connection from the
emulator process. It starts the guest stopped at the reset vector:

```sh
./x86emu --platform=haswell --monitor=8080
# Open http://127.0.0.1:8080/ and press Continue
```

`--monitor=[IPv4:]port` binds to `127.0.0.1` by default. The page shows CPU and
vCPU state, registers, disassembly, new log lines and the live framebuffer; its
controls provide pause/continue/step, breakpoints, debugger inspection, keyboard
scancodes, ASCII typing and quit. Offline PPM/log loading is still available.
When GDB is attached, monitor run control is read-only and GDB remains the owner.

There is **no authentication**. Keep the default loopback binding unless remote
access is intentional; an explicit bind such as `--monitor=0.0.0.0:8080` exposes
the emulator controls to anyone who can reach that port. Use appropriate network
isolation/firewall rules. The monitor currently accepts IPv4 bind addresses.

Snapshots save the whole machine when the run loop ends and resume it later in
a fresh process; `--max-instr` stays the total budget:

```sh
./x86emu --kernel=kernel.elf --initrd=initrd.tar --max-instr=900000000 --snapshot-save=boot.snap
./x86emu --kernel=kernel.elf --initrd=initrd.tar --max-instr=1200000000 --snapshot-load=boot.snap
```

The second run must use the same machine shape (platform, ROM, kernel, vCPUs,
disks); the debugger adds `savevm FILE` / `loadvm FILE`.

Run any compatible ROM directly; the emulator does not rewrite the guest image. For the bundled sample:

```sh
./x86emu --rom=firmware/firmware.bin --platform=haswell \
  --max-instr=200000 --dump-fb=framebuffer.ppm --log=boot.log
```

Select other profiles with `--platform=sandybridge`, `ivybridge`, `broadwell`, or `baytrail`. `--disk=...` supplies the virtual USB storage image. `--rom=...` supplies another firmware image.

For a full stock-SeaBIOS boot with ACPI tables (`--lpc=ich9`), AHCI ISO attachment, and 4-vCPU SMP (`--smp=4`):

```sh
./x86emu --rom=bios.bin --lpc=ich9 --smp=4 \
  --disk=docs/evidence/compat-g3/blank-usb-1m.img \
  --sata=auralite.iso --max-instr=1000000000 --stats
```

For direct ELF64 kernel boot without firmware (`--kernel=`):

```sh
./x86emu --kernel=kernel.elf --initrd=initrd.tar --cpus=4 --smp=4 --max-instr=500000000 --stats
```

## Testing

Run the CPU unit tests and the platform smoke tests with:

```sh
make test
```

The focused CPU tests can be run independently:

```sh
make test-unit
```

`make test` also runs the NET wire lane (`make test-net-tx`): a small
payload (`tests/nic_tx.asm`) boots through the real firmware transport
off a throwaway `--disk=` image, transmits a frame through the e1000 TX
ring and a host-side responder records what arrived on the TAP endpoint.
It needs `nasm` and `python3`, and the fixed endpoint `UDP
127.0.0.1:8080` free; it SKIPs (exit 0) when a dependency is missing.
The frozen-guest lane inside it SKIPs unless `KERNEL`/`INITRD` point at
an AuraLite-OS build.

`make test` also runs the NET RX lane, the responder's protocol vectors
and the NET end-to-end lane (`make test-net-rx`,
`make test-tap-responder`, `make test-net-e2e`).
`tools/tap_responder.py` is the host side of the TAP socket: it answers
DHCP, ARP, ICMP echo, **DNS** and **TCP** with slirp's user-mode
addressing (guest `10.0.2.15`, gateway `10.0.2.2`, DNS `10.0.2.3`) so a
guest can get a lease, ping its gateway, resolve a name over UDP *and*
over the RFC 1035 TCP carriage, and hold 16 TCP connections open at
once. `tests/nic_net.c` is a freestanding x86_64 payload that reproduces
the guest's own protocol code and rides the `--kernel=` load lane; twice,
byte-identical, it puts **76 frames** on the wire and prints

```
[dhcp] PASS: IP 10.0.2.15, gateway 10.0.2.2
[net] PASS: lease + ARP + echo reply (payload verified)
[dns] PASS: 'example.com' -> 93.184.216.34
[dns] PASS: 'example.com' -> 93.184.216.34 over TCP (45-byte message, prefix ok)
[tcp-x5] PASS: 16 concurrent connections held and closed cleanly
N3OK
```

Run the responder by hand with:

```sh
python3 tools/tap_responder.py --timeout 30          # DHCP/ARP/ICMP/DNS/TCP
python3 tools/tap_responder.py --mode=capture --out frames.hex   # record only
python3 tools/tap_responder.py --drop=arp,icmp,dhcp,dns          # withhold L2/L3 replies
python3 tools/tap_responder.py --no-tcp                          # withhold TCP (the N3 control)
```

It needs `python3` and the endpoints `UDP 127.0.0.1:8080`/`:8081` free —
both harnesses now *check* that before starting a lane, because
`SO_REUSEADDR` lets two sockets bind the same port and a leftover
responder will silently split the datagrams (ledger **D11**). `--gw-mac`
overrides the gateway MAC, `--dns-ip` / `--dns-ttl` the A record
(see ledger **D7** in `docs/plans/NET_PLAN.md`: the MAC follows slirp's
published convention but could not be measured here).

## Architecture

- `src/cpu.c` / `src/cpu.h` — x86/x86-64 interpreter, paging, exceptions/interrupts, P2 translation TLB and P3 prefix/opcode predecode cache.
- `src/mem.c` / `src/io.c` — physical RAM/ROM/MMIO address space (with Q35 PAM and A20) and 64K I/O port space.
- `src/pci.c` / `src/platform.c` / `src/chipset.c` — PCI config #1 + ECAM, five Intel CPUID profiles, A20 and warm-reset control.
- `src/pic.c` / `src/pit.c` / `src/pm.c` / `src/rtc.c` / `src/kbc.c` / `src/serial.c` — 8259A PIC, 8254 PIT, ACPI PM timer (`PMBASE+8`), MC146818A RTC/CMOS, 8042 KBC, 16550 UART.
- `src/lapic.c` / `src/ioapic.c` — Local APIC (xAPIC, timer, IPIs, INIT/SIPI) and 82093AA I/O APIC.
- `src/devices.c` / `src/ahci.c` / `src/e1000.c` — GPU framebuffer, VGA text mode 3, Bochs VBE dispi, EHCI + UHCI USB mass storage, dual-controller AHCI SATA, Intel 82540EM NIC.
- `src/kloader.c` / `src/acpi.c` — direct ELF64 loader, `boot_info_t` builder, and RSDP/RSDT/XSDT/MADT generator.
- `src/disasm.c` / `src/dbg.c` / `src/gdbstub.c` / `src/snapshot.c` / `src/ws.c` / `src/monitor.c` — disassembler, debugger REPL, GDB RSP stub, `AURASNP1` v2 snapshots, RFC 6455 WebSocket + live browser monitor.
- `src/machine.h` / `src/main.c` — machine state (`machine_t`, up to 8 `vcpu_slot_t` contexts) and CLI harness.

## License

The emulator core is licensed under the Apache License, Version 2.0 (see `LICENSE` and `NOTICE.md`). The bundled sample firmware under `firmware/` carries its own MIT license in `firmware/LICENSE`.
