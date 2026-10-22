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
  - **Video** (`src/devices.c`, `docs/plans/VIDEO_PLAN.md` V0–V5): 16 MiB VGA aperture at `0xD0000000` with the published 800×600×32 display (boot_info.fb), VGA text mode 3 (`0xB8000` + CRTC cursor + 8×16 PSF2 font, `--no-fb`, `--dump-text`), and Bochs VBE dispi (`0x1CE`/`0x1CF`, `--vbe=WxHxBPP|off`) with `vbe_start_addr` display panning.
  - **Networking** (`src/e1000.c`, `tools/tap_responder.py`, `docs/plans/NET_PLAN.md` N0–N4): Intel 82540EM (`e1000`) with TAP-over-UDP (`--tap=host:port`) and a host responder speaking ARP, DHCP, ICMP, UDP/TCP DNS and 16-way concurrent TCP; honest INTx (IRQ 11 / INTA#, `(ICR & IMS)` level).
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
build.bat gui       REM Start the desktop GUI application (same as gui.bat)
build.bat gui-web   REM Open the web GUI at http://127.0.0.1:8080/
build.bat monitor   REM Start the live browser monitor at http://127.0.0.1:8080/
build.bat test      REM Run the native Windows C unit tests + Python G3/G5/G6 acceptance gates
build.bat wsl test  REM Delegate full `make test` to WSL2
```

## GUI — no command line needed

`make gui` (or `python3 tools/gui_app.py`; on Windows `build.bat gui` or
double-click `gui.bat`) starts the **AuraLite Emulator desktop application** —
a native window (tkinter, Python standard library only — no browser and no
third-party packages) with two switchable modes:

- **Обычный (Normal)** — VirtualBox/VMware style: a machine list, big
  start/pause/stop buttons, the live screen, and a three-step *new machine*
  wizard.  Pick an **ISO** and press Start — that is the whole procedure
  (stock SeaBIOS `bios.bin` + the ISO on a SATA port, the documented
  `docs/WINDOWS.md` recipe, wired automatically).
- **Экспертный (Expert)** — the full option surface: every CLI option,
  the monitor console (`regs`, `info break`, `exec`/`savevm`/`loadvm`),
  artifacts, snapshots, the TAP responder and run logs.

The web GUI remains available as `make gui-web` (or `python3
tools/launcher.py`, `build.bat gui-web`) at **http://127.0.0.1:8080/** and
exposes the same capabilities:


- **Machine setup**: boot path (firmware ROM / direct kernel ELF / disk),
  platform profile, SMP, chipset (LPC identity, USB), SATA disks (both AHCI
  controllers), VBE mode, TAP networking with a managed host responder
  (`tools/tap_responder.py`), run limits, artifacts (log / PPM / text /
  trace), debug options (gdb port, watch-phys, symbols, script, keys).
- **Run control**: start / restart / stop, pause / continue / step, run to
  the end, breakpoints, keyboard scancodes and ASCII typing.
- **Live view**: framebuffer (PNG export), registers, vCPUs, mode / stop
  state, the machine log, and the x86dbg REPL (`x/8i`, `info break`,
  `xlate`, `savevm`/`loadvm` behind the snapshot buttons).
- **Results**: per-run artifact download, named snapshots, named config
  presets, and the process output (stdout receipts + stderr log ring).

Both front ends share one core (`tools/launcher.py`): the emulator's argv is
built strictly from the option schema (unknown/injected values are refused),
each machine runs exactly like the CLI would (`--monitor=127.0.0.1:<free
port>` on loopback), and the state (`~/.x86emu-gui` by default: presets,
per-run artifacts, snapshots, the remembered `bios.bin` location) lives
outside the repository.  For plain ISO boot the app looks for a stock
SeaBIOS (`bios.bin`, e.g. from the Debian/Ubuntu `seabios` package or
seabios.org) in the state dir, the repo, or `$AURALITE_BIOS`, and remembers
a user-picked copy. At the post-mortem
"end" stop the page keeps the final state visible; **Остановить** sends the
monitor's `quit` so the emulator prints its stats and writes the artifacts
before exiting. Access is authenticated (UPKEEP U7): on loopback the page
carries its token automatically; on any other bind the visitor must enter
the access token shown by the desktop GUI (expert mode, «Сеть» tab), and
the transport is plaintext HTTP — still keep the launcher on loopback
unless remote access is intentional.

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

Requests are authorized (UPKEEP U7): the `Host` must name this machine,
a browser `Origin` must be same-origin, and with a token configured
(`--monitor-token=STR`, or `X86EMU_MONITOR_TOKEN` in the environment —
the form the GUI uses so the secret stays out of process lists) the
WebSocket upgrade must present it (`?token=`, `X-X86emu-Token` or
`Authorization: Bearer`). A non-loopback bind without a token is refused
at startup; with one, the transport is still plaintext HTTP, so keep the
default loopback binding unless remote access is intentional and use
appropriate network isolation/firewall rules. The monitor currently
accepts IPv4 bind addresses.

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

`make test` includes `make test-gui` (both GUI layers: the web launcher
vectors in `tests/test_gui_launcher.py` and the desktop app vectors in
`tests/test_gui_app.py`) and `make test-iso` (`tests/test_iso_boot.py`).
The ISO lane builds a 1 MiB El Torito test image in memory
(`tools/mkiso.py` + `tests/fixtures/bootstub.nas`) and must see the boot
stub's `AURA_ISO_BOOT_OK` marker after stock SeaBIOS reads its MBR off
the SATA-attached image -- the exact path the GUI's «Загрузка с ISO»
takes.  It SKIPs (exit 0) without `nasm` or a stock SeaBIOS `bios.bin`
(found via `--bios`, `$AURALITE_BIOS`, `~/.x86emu-gui`, or the repo);
the CD lane boots the same ISO through `--cdrom=` (ATAPI + El Torito).  Note
the image must be at least 1008 sectors (512 B): smaller images round
the BIOS CHS translation to 0 cylinders and firmware refuses the read
(measured on stock SeaBIOS 1.16.3).

`make test` also runs the NET wire lane (`make test-net-tx`): a small
payload (`tests/nic_tx.asm`) boots through the real firmware transport
off a throwaway `--disk=` image, transmits a frame through the e1000 TX
ring and a host-side responder records what arrived on the TAP endpoint.
It needs `nasm` and `python3`, and the TAP endpoint (default `UDP
127.0.0.1:8080`; `AURALITE_TAP_PORT` moves the whole 8080/8081/8082
family) free; it SKIPs (exit 0) when a dependency is missing.
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

It needs `python3` and the TAP endpoints free (default `UDP
127.0.0.1:8080`/`:8081`/`:8082`; set `AURALITE_TAP_PORT` to move the
family, e.g. for two parallel test runs) —
all three net harnesses now *check* the port before starting a lane, because
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
