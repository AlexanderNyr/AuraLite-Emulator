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

`src/cpu.c` is the instruction interpreter. `src/mem.c` and `src/io.c` implement the physical and port address spaces. `src/pci.c` implements PCI config mechanisms. `src/devices.c` contains the platform models and EHCI/USB storage behaviour. `src/main.c` is the CLI harness.

The next GUI layer can consume the machine event ring and framebuffer without changing the emulation core; the framebuffer export is currently PPM so it works without external graphics dependencies.

## License

The sample firmware carries its applicable license in `firmware/LICENSE`.
The emulator core does not currently declare a repository-level license; add
one before publishing if you want to grant redistribution and reuse rights.
