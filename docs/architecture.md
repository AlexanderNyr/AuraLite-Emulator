# AuraLite Emulator Architecture

This document explains how the emulator is put together: the layers, the machine
contract, the CPU core, the chipset and device models, the three boot paths, the
timebase, the tooling, and the invariants that must not move when the tree
changes. It replaces the older `PROJECT_ANALYSIS.md` audit note (removed in
patch `0112`), which described a pre-`f0fc4e7` tree and carried hand-copied
counts that promptly drifted.

Scope and status live elsewhere and are not duplicated here:

| Question | Document |
|---|---|
| What is implemented, with receipts | [`STATUS.md`](STATUS.md) |
| What is planned, phase by phase | [`../ROADMAP.md`](../ROADMAP.md), [`plans/`](plans/) |
| What is deliberately absent, and why | s[Known limits and non-claims](#known-limits-and-non-claims) below, [`../README.md`](../README.md) |
| Address layout (physical, I/O, PCI, kernel lane) | [`memory_map.md`](memory_map.md) |
| Firmware observations | [`BUGS.md`](BUGS.md) |
| Windows host build | [`WINDOWS.md`](WINDOWS.md) |
| Chronological changes | [`../CHANGELOG.md`](../CHANGELOG.md) |

## What this is

A from-scratch x86 PC emulator in portable C11: no QEMU, no KVM, no third-party
libraries (the whole program links against `-lm` and nothing else). It executes
raw x86 firmware from the reset vector and models the Intel platform around it
closely enough that stock SeaBIOS, the bundled sample firmware and the
companion AuraLite OS kernel all boot on it.

The north star (`../ROADMAP.md`) is:

```
  AuraLite-Emulator boots the AuraLite OS kernel to its shell.
```

Two design rules follow from it and explain most of the code:

1. **The core is firmware-agnostic.** No instruction, address or behaviour in
   `src/cpu.c` may depend on a particular ROM or guest. Platform differences are
   `--platform=` profiles and PCI identity switches; a guest that needs a quirk
   gets it in its own image or its own explicit option.
2. **Measured before claimed.** Every behaviour that the tree asserts is backed
   by a harness vector, and every fix ships with a regression test and one
   negative control (revert the fix, watch the test redden). See
   [Evidence discipline](#evidence-discipline).

## Layers

```
┌───────────────────────────────────────────────────────────────────────────┐
│ front ends                                                                │
│   CLI (src/main.c)                  GUI back end (tools/launcher.py)      │
│   x86dis (tools/x86dis.c)           desktop GUI (tools/gui_app.py, tk)    │
│   host NIC responder (tools/tap_responder.py)                             │
│   browser monitor (src/monitor.c + web/monitor.html)                      │
│   GDB remote (src/gdbstub.c)        REPL (src/dbg.c)                      │
└───────────────┬───────────────────────────────────────────────────────────┘
                │ argv schema / HTTP+WS / RSP / stdin
┌───────────────▼───────────────────────────────────────────────────────────┐
│ machine_t  (src/machine.h)                                                │
│   one CPU context per vCPU, RAM/ROM pointers, MMIO region list, I/O port   │
│   table, PCI bus, every device struct, the log ring, the P2/P3 caches,     │
│   virtual time (vtime_instr), run/stop state                               │
└───────┬─────────────────────┬───────────────────────┬─────────────────────┘
        │                     │                       │
┌───────▼───────┐   ┌─────────▼─────────┐   ┌─────────▼──────────────────┐
│ CPU core      │   │ address space     │   │ devices / chipset          │
│ src/cpu.c     │   │ src/mem.c src/io.c│   │ pic pit pm rtc kbc serial  │
│ decode, paging│   │ PAM, A20, MMIO    │   │ lapic ioapic ahci xhci     │
│ exceptions,   │   │ precedence, ROM   │   │ e1000 virtio* devices.c    │
│ x87, SSE, MSRs│   │ aliases           │   │ pci.c platform.c chipset.c │
└───────────────┘   └───────────────────┘   └────────────────────────────┘
        │                     │                       │
        └─────────────────────┴───────────────────────┘
                              │ libc + lm only
                        host OS (Linux / Windows via win32/)
```

There is exactly one run loop. `main.c` owns it for the CLI, the monitor's
`continue` command drives the same loop, and the GUI spawns the CLI binary
rather than linking the core — so a GUI run and a command-line run are the same
machine by construction (`tools/launcher.py` builds argv strictly from its
`SCHEMA`).

## The machine contract

`machine_t` is the entire machine. Everything a snapshot must restore lives in
it or in a struct it owns; everything a run needs to be reproducible lives in it
too:

| Field group | Meaning |
|---|---|
| `cpu`, `vcpu[EM_MAX_VCPU]` | execution contexts; `m->cpu` is always the *current* one |
| `ram`, `rom`, `rom_len` | 128 MiB RAM, 128 KiB ROM window |
| `mmio_list` | MMIO regions, a list where **the smallest matching region wins** |
| `io[]` | 64 Ki I/O ports, function-pointer table |
| `pci_devs`, `pci_ecam` | config mechanism #1 and the ECAM window |
| `pit`, `rtc`, `pm`, `pic`, `lapic[]`, `ioapic`, `kbc`, `serial` | chipset state |
| `disk`, `sata_img/len/kind`, `usb/*`, `xhci`, `ahci`, `e1000`, `virtio*` | storage and I/O backends |
| `fb`, `fb_w`, `fb_h`, display mode, VGA text state | display |
| `vtime_instr`, `n_vcpus`, `cur_vcpu`, `vcpu[].state` | SMP and the timebase |
| `perf_tlb[]`, `perf_tlb_generation`, `perf_decode[]` | the two caches (PERF P2/P3) |
| `log` | 4096-line ring, the log the monitor and `--log=` publish |
| `dbg`, `run_steps`, `stop_requested` | tooling state (never guest-visible) |

Two consequences worth stating explicitly:

- **One vCPU is the historical machine.** With `n_vcpus == 1` the SMP machinery
  is identity: `vcpu_load/store` are no-ops, `m->cpu` never moves, and every
  stat and artifact is bit-identical to the pre-SMP tree. This is why the
  firmware lanes and all unit harnesses can ignore SMP entirely.
- **Time is counted, not sampled.** There is no `clock_gettime` on the guest
  path; `vtime_instr` advances one per executed instruction (plus the HLT
  quantum). Determinism is a feature, not a side effect — see
  [Timebase](#timebase).

## CPU core

`src/cpu.c` is a switch-threaded interpreter: `cpu_step()` executes exactly one
instruction and returns, which is what makes breakpoints, watchpoints, snapshots
and the trace self-check possible at instruction granularity.

### Modes

The mode is derived, never stored: `CR0.PE` selects real vs protected,
`CR0.PG` enables paging, `EFER.LME/LMA` plus `CS.L` give long mode, and `CS.D`
selects 16 vs 32-bit defaults.

```
real16 ──CR0.PE──▶ prot16 / prot32 ──EFER.LME+LMA+CS.L──▶ long64
                        │
                        └── compat32 (long mode, CS.L=0: legacy 32-bit code in a
                                     64-bit world; the guest's ring-3 lane uses it)
```

### The step pipeline

```
cpu_step()
  ├── device cadence every 256 instructions (EHCI/UHCI/xHCI schedule walk)
  ├── pending reset / INIT applied on the boundary (never mid-instruction)
  ├── HLT: wake on PIC level or LAPIC delivery; else charge the idle quantum
  ├── interrupt arbitration (see Interrupts)
  ├── tooling boundary (breakpoints, watchpoints, step)         [cold path]
  ├── prefix loop: 66 67 F0 F2 F3 26 2E 36 3E 64 65 40..4F
  ├── P3 predecode cache hit?  ── yes ──▶ opcode already known
  ├── opcode dispatch (fast switch → group1 → really_slow switch)
  └── retire: instr_count++, executed_steps++ (faults count as attempts)
```

Prefixes are re-fetched on the slow path by re-walking from `start_pc`, which is
deliberate: the P3 cache only stores prefix/opcode bytes that were proven to be
plain RAM inside one page, and ModRM/SIB/displacement/immediates are **never**
reused.

### ISA coverage

Implemented: the full firmware/OS integer set (ALU incl. ADC/SBB with carry,
MOV family, Jcc/CMOVcc/SETcc, shifts/rotates with count masking, MUL/IMUL/
DIV/IDIV with `#DE`, BT/BTS/BTR/BTC, BSF/BSR/BSWAP, XADD, CMPXCHG, LOOP/JCXZ,
MOVS/CMPS/SCAS/LODS/STOS + INS/OUTS with REP/REPE/REPNE, IN/OUT, LAHF/SAHF,
XLATB, ENTER/LEAVE, CMPXCHG8B/CMPXCHG16B), the system instructions (MOV CRn/DRn,
LGDT/LIDT/SGDT/SIDT/LLDT/LTR/SLDT/STR, LAR/LSL/VERR/VERW, LMSW/SMSW, CLTS,
INVLPG/INVPCID, WBINVD/INVD/CLFLUSH, HLT/WAIT, STAC/CLAC), the segment-register
pushes/pops (06/07/0E/16/17/1E/1F and `0F A0/A1/A8/A9` for FS/GS), CPUID, RDTSC/
RDTSCP, RDMSR/WRMSR, SYSCALL/SYSRET, SWAPGS, POPCNT.

The D1 lane closed the last decode holes that kept compiled guest code from
running: `LAHF`/`SAHF` (with `CPUID.80000001H.ECX[0]` now advertised) and
`ENTER`/`LEAVE`, plus `XLATB`, the full `0F 00` group (`SLDT`/`STR` with a real
task-register selector, `LLDT` caching `LDTR`, `LTR`, `VERR`/`VERW`), `LAR`/
`LSL`, `INVD`, the FS/GS segment pushes and pops, and `CMPXCHG8B`/`CMPXCHG16B`
(with `CPUID.1H.ECX[13]` advertised). Each of those has a host-measured
expectation behind it: the D1 differential ran the same instruction on the host
CPU and on the core and compared register/flag/memory state byte for byte.

Deliberately partial, and honest about it:

| Area | What exists | What raises `#UD` |
|---|---|---|
| x87 | full 80-bit register file (`long double`, bit-exact), FCW/FSW/FTW/FOP/FIP/FDP, FLD/FST/FSTP/FILD/FIST, arithmetic, compare, the eight transcendentals, FBLD/FBSTP, FXSAVE/FXRSTOR/FNSAVE | nothing in the arith/transfer core |
| SSE | 16×128-bit `XMM0..15`, MOVAPS/UPS/DQA/DQU, scalar moves, PXOR/SIMD bitwise, and what the ring-3 lane needs | SIMD arithmetic, AVX |
| Other | — | SMM, VMX, SVM, NMI, x87 data beyond the above, MMX arithmetic |

Anything unimplemented **faults**, it does not silently misbehave — that rule is
what makes a `#UD` in a log a usable signal rather than noise.

### Paging and protection

`translate()` walks PML4 → PDPT → PD → PT with 4 KiB / 2 MiB / 1 GiB leaves and
enforces U/S, R/W, NX, `CR4.SMEP`, `CR4.SMAP` (with `EFLAGS.AC`), `CR0.WP`, and
PCID legality. Faults carry architectural error codes (P/W/U/RSVD/I-D) and set
`CR2`. One subtlety is modelled explicitly: **implicit descriptor-table reads
are supervisor accesses even at CPL3** (`c->desc_sv`), because a `SYSCALL`
entry that fetches GDT[1] from a supervisor page must not `#PF`.

Two caches sit in front of the walk:

| Cache | Slots | Key | Invariant |
|---|---|---|---|
| PERF P2 translation TLB | 256 | full security context: CR3, CR0, CR4, EFER, AC, CPL, `desc_sv`, access mode | **success only**; negative caching is forbidden so every fault takes the original path with the original CR2/error/RIP |
| PERF P3 predecode | 512 | RIP, CS base/selector/L/D, same security context, generation | only prefix+opcode bytes, only when the execute translation proved plain RAM and the bytes stay in one page |

Both are invalidated by one global RAM-write generation, so a guest store to
code or to a page table is O(1). Neither is copied on a vCPU switch: they are
machine-wide and keyed on the context.

### Exceptions and interrupt delivery

```
raise_exception(vector, has_err, err)
  ├── real mode            → IVT[vector] (CS:IP from 4*vector)
  └── protected/long mode  → IDT gate (interrupt/trap), IST or TSS.RSP0
        ├── hardware frame order: [SS,RSP,] RFLAGS, CS, RIP, err (err last)
        ├── ring change: switch stack FIRST, then push (translations see CPL0)
        ├── far-load CS, clear IF for gates, clear EFLAGS.AC (CF1)
        └── #DF guard: exception-during-exception escalates, never loops
```

Arbitration happens once per instruction boundary, in this order: **LAPIC
first, then the 8259**, and only when `IF=1` and the STI/MOV-SS shadow has
expired. `HLT` wakes on a line level even with `IF=0` (as silicon does), and
`HLT` with `IF=0` and nothing pending is a real stop.

### x87, MSRs and the feature contract

The x87 register file is the ISA's own 80-bit format, stored as host
`long double` (bit-exact on every host this builds for); FSW.TOP indexes the
physical file, FTW keeps the full two-bit tags and is abridged only at the
FXSAVE boundary. MSRs are a sparse table (`MAX_MSR` entries) with per-index
semantics for the ones that matter: EFER, STAR/LSTAR/SFMASK, FS/GS/KERNEL_GS
base, IA32_APIC_BASE, IA32_PAT (with its SDM reset value), MTRRs (permsissive,
because the firmware probes them before it knows what it wants).

CPUID must advertise **exactly** what the core does (CPU_FEATURES CF0–CF5).
When a feature bit and its implementation disagreed, the audit treated that as a
defect, not as a documentation problem: SMEP/SMAP guards were once inverted
while CPUID advertised neither, so a kernel setting `CR4.SMEP` would `#PF` on
its own text.

## Address space

Physical memory is a 64-bit address space with three layers of decode, in this
order of precedence:

```
guest physical address
  ├── MMIO region list         smallest matching region wins   (PCI BARs, LAPIC,
  │                                                            IOAPIC, VRAM, ECAM,
  │                                                            CAR, DDR, AHCI…)
  ├── ROM decode               reset window at the top of 4 GiB, aliases at
  │                            0xE0000 and 0xFFFE0000, PAM-shadowed at 0xC0000
  └── RAM                      128 MiB, unless A20 masks bit 20
```

The Q35 PAM bytes (`0x90..0x96`) are the single source of truth for the
`0xC0000..0xFFFFF` decoder: a write goes to the RAM shadow, an unshadowed read
falls back to the immutable flash buffer or open bus. The alias window
`0xE0000..0x100000` lives **inside `m->ram`**, which is why the P2 cache refuses
to cache that window — a trap that is documented at the site in `mem.c` and in
`machine.h`.

`mem_debug_peek_byte()` is the side-effect-free inspection path tools use: it
resolves PAM and MMIO without touching device state, so a debugger read cannot
perturb the machine.

Layout tables (physical, I/O ports, PCI, and the `--kernel` lane's linear map)
are in [`memory_map.md`](memory_map.md).

## PCI and platform identity

Two configuration mechanisms are live at once, as on real Q35 hardware:

| Mechanism | Where | Notes |
|---|---|---|
| #1 (port I/O) | `0xCF8` address / `0xCFC` data | 32-bit, the firmware's primary path |
| ECAM (MMCONFIG) | `0xE0000000`, 256 MiB | enabled by the host bridge's own register; runtime-registered MMIO so snapshots can replay it |

The device tree is assembled in `devices_init_common()`; each function owns its
config-space hooks (read/write callbacks), its BARs and its interrupt route.
`platform.c` supplies five CPUID profiles (Sandy Bridge, Ivy Bridge, Haswell,
Broadwell, Bay Trail) with Intel's real family/model/stepping signatures, so a
firmware written to branch on the CPU lands on a plausible path — and
`--platform=` is the only thing that changes.

Identity switches are explicit, never silent: `--lpc=ich9` moves the LPC bridge
from `8086:1e55` to `8086:2918` so that stock SeaBIOS recognises a Q35+ICH9 pair
and publishes its ACPI tables (COMPAT G4). MSI/MSI-X capability presence follows
the same rule: a device offers it only when the model implements it (xHCI 16
entries, both virtio functions), and every device keeps an INTx fallback whose
behaviour is byte-identical to the pre-MSI lanes.

## Interrupts

Three controllers coexist, and a guest may use any of them:

```
                      ┌────────────── bus lines (level) ─────────────┐
 devices ─────────────┤ AHCI 0/1, xHCI, e1000, virtio, PIT, RTC, KBC  │
                      └───────┬─────────────────────────┬─────────────┘
                              │ legacy                   │ APIC
                     ┌────────▼────────┐        ┌────────▼─────────┐
                     │ 8259A PIC pair  │        │ I/O APIC (24 RTE)│
                     │ IRQ0..15        │        │ GSI → vector+dest│
                     └────────┬────────┘        └────────┬─────────┘
                              │                          │
                     ┌────────▼──────────────────────────▼─────────┐
                     │ CPU (per vCPU): LAPIC first, then 8259 —     │
                     │ 8259 INTR is wired to the BSP only           │
                     │ MSI writes bypass both: lapic_set_irr_dest() │
                     └──────────────────────────────────────────────┘
```

- The **8259A** pair is modelled fully: ICW/OCW, priorities, spurious IRQ7/15,
  level lines, AEOI, and a virtual time base.
- The **xAPIC** has 256-bit IRR/ISR, TPR/PPR arbitration, EOI, the timer, and
  IPIs (self/peer/broadcast, INIT/SIPI with the AP parking protocol for up to 8
  contexts). `IA32_APIC_BASE` relocation and disable are modelled, including the
  snapshot consequences.
- The **I/O APIC** has 24 redirection entries, fixed deliver mode plus
  lowest-priority/logical support, mask, trigger/polarity, and remote-IRR
  write-back.
- **MSI/MSI-X** deliver through a LAPIC message core (`msi_deliver`) with the
  QEMU-compatible edge semantics: an unmask/Function-Mask-clear/Enable edge (or
  a snapshot load) releases held messages, and PBA projects pending flags
  through the assigned vectors.

Three rules keep this deterministic and testable: delivery happens only at
instruction boundaries; the pending state is level-and-edge explicit per source;
and EOI ordering (before the handler for the timer path) is documented where it
matters for preemption.

## Timebase

```
instr_count / vtime_instr  ──┬──▶ PIT  (divides by instr_per_tick, PIT_DEFAULT_IPT)
                             ├──▶ RTC  (seconds boundary, update-ended IRQ)
                             ├──▶ LAPIC timer (per-context delta accumulator)
                             └──▶ RDTSC / TSC-deadline
```

There is no wall clock on the guest path. `vtime_instr` is the master clock and
advances one per executed instruction; the HLT path charges a quantum (512) only
when **all** contexts are idle, because a halted vCPU on real hardware consumes
zero issue slots and must not drag a busy peer's clock. Getting that wrong is
measurable: the K8 note records a 2-vCPU boot inflated to ~1031 virtual seconds
before the fix.

`--stats` reports the throughput numerator as `executed_steps` (instruction
attempts actually retired by the step loop, faults included), not `instr_count`,
because the idle quantum and guest-visible time share that counter.

## Storage

| Backend | Model | Options |
|---|---|---|
| AHCI SATA | command list → H2D FIS → PRDT engine, `IDENTIFY`, `READ/WRITE DMA EXT`, up to 128 KiB per command, honest INTx (`IS != 0 && GHC.IE` or `PxIS & PxIE`) | `--sata=`, `--sata-portN=`, `--ahci2`, `--sata2=`, `--sata-writethrough` |
| AHCI ATAPI | PACKET device, `IDENTIFY PACKET`, 2048-byte LBAs, El Torito boot | `--cdrom=` |
| virtio-blk | modern (1.x) split-ring transport, RAM image, `FEATURES_OK` gate, MSI-X or INTx | `--virtio-blk=` |
| USB mass storage | one BOT/SCSI device model driven by all three host controllers (UHCI companion, EHCI high-speed, xHCI), `INQUIRY`, `READ CAPACITY(10)`, `TEST UNIT READY`, `REQUEST SENSE`, `READ(10)`, tag echo, residue, CSW status | `--disk=`, `--no-usb-uhci` |

Writes are copy-on-attach by default; `--sata-writethrough` sends them to the
host image as well, and snapshots refuse that mode because the host file is not
machine state.

## Video

Two display sources, one arbitration point:

```
        VGA text mode 3                        Bochs VBE graphics
  0xB8000 cells + CRTC cursor            16 MiB aperture at 0xD0000000
  8×16 PSF2 font (the guest's own)      dispi at 0x1CE/0x1CF
        │                                      │
        └─────────── vga_render_display() ─────┘
                            │
                800×600×32 published display (boot_info.fb, --dump-fb, monitor)
```

`vga_set_display_mode()` picks the source; `--no-fb` locks the display to the
text console, which is what the guest's fallback path needs. The dispi is a full
transcription of QEMU's `vbe_fixup_regs` (VIDEO V4): mode-set clamps width,
height and offset against the aperture (`YRES` → `1..maxy`), so an oversized
request cannot write outside the window — the original partial transcription was
a host heap overflow of the same class as CVE-2016-3712, and the regression
vector now computes every expected number from QEMU's arithmetic first.
`vbe_start_addr` panning is modelled; BPP 4 is accepted on purpose (a documented
divergence, because a literal reading divides by zero).

## Networking

```
guest NIC ── PCI function ── MMIO registers ── descriptor rings ──┐
   e1000 82540EM (0:04.0)                                          │  UDP datagrams
   virtio-net 1af4:1041 (0:06.0)                                   │  to host:port
        │                                                          ▼
        └── INTx (IRQ 11) or MSI-X                        tools/tap_responder.py
                                                            DHCP, ARP, ICMP echo,
                                                            DNS (UDP + TCP), 16
                                                            concurrent TCP conns
```

The TAP endpoint is a UDP socket standing in for a tap device: the emulator
sends and receives whole Ethernet frames, and the host responder speaks
slirp's user-mode addressing (guest `10.0.2.15`, gateway `10.0.2.2`, DNS
`10.0.2.3`). `--tap=` and `--virtio-net=` are mutually exclusive, because one
endpoint has one owner (VIRTIO V4 D6).

Scope is explicit: IPv4 only, stateless DHCP, DNS A/IN only, TCP without window
scaling/SACK/TIME_WAIT handling. The wire lane pins the frame count
(76 frames, byte-identical across runs) so a regression shows up as a number,
not as a hang.

## ACPI and power management

On the `--kernel` lane the emulator builds the table set itself
(RSDP/RSDT/XSDT/MADT/FADT/DSDT at `0x9F000`); on the BIOS lane it publishes the
PM block so the *firmware* builds them, which is what `--lpc=ich9` unlocks.

| Surface | Behaviour |
|---|---|
| PM1_STS / PM1_EN / PM1_CNT | modelled; level SCI on IRQ9 |
| ACPI PM timer | 24-bit at `PMBASE+8`, 3.579545 MHz (3×PIT) |
| GPE0 | modelled |
| `\_S5` / `\_S1` | real AML in the DSDT; S5 stops the run deterministically, S1 is a spec-legal zero-duration suspend |
| SMI | **honestly absent** — `SMI_CMD=0`, no SMM |

## Boot paths

Three ways in, all ending in the same machine:

**1. Firmware (`--rom=`, the default).** The image is right-aligned in the
128 KiB window so its last byte lands on `0xFFFFFFF0`.

```
reset vector 0xFFFFFFF0 (CS=f000:fff0)  →  ROM alias 0xE000:0
   → real mode setup, GDT, CR0.PE
   → protected mode, CPUID dispatch (five platform profiles)
   → ECAM/MMCONFIG enable, PAM shadow, DDR training, GPU BAR
   → page tables, EFER.LME, long mode
   → USB mass-storage READ(10) of the kernel sector
   → jump to 0x00100000
```

**2. Direct kernel (`--kernel=`).** No firmware at all: the loader parses the
higher-half ELF64, places `PT_LOAD` segments at `p_paddr - KERNEL_VMA`, builds
seven page-table pages (identity + HHDM + kernel VMA), fabricates the
`boot_info_t` handoff (7760-byte ABI, magic checked against the companion OS
header), publishes the ACPI set, and parks the CPU at `_start` in long mode with
paging on.

**3. BIOS compatibility (`--lpc=ich9` + a stock `bios.bin`).** SeaBIOS → its own
stage1/stage2 → the guest, with ACPI tables published by the firmware, AHCI/ISO
or USB media, and 1–8 vCPUs woken by INIT/SIPI.

All three are gated: `make test` covers (1) and (2) locally, and the SeaBIOS
lanes (ISO as MBR disk and as ATAPI CD, virtio-blk boot) skip cleanly when no
`bios.bin` is present.

## SMP

`--smp=N` (max 8, `EM_MAX_VCPU`) creates N CPU contexts in **one process and one
thread**: `m->cpu` and its LAPIC always point at the running context, and the
run loop rotates them strictly 1:1.

```
for each slot in round-robin:
    vcpu_load(slot)  → m->cpu := vcpu[slot].c ; lapic sync
    cpu_step()       → one instruction (or HLT handling)
    vcpu_store(slot)
```

Reasons, in order: determinism (identical artifacts across runs and machines),
snapshot simplicity (one context array), and the ability to keep the UP path
byte-identical. The cost is real parallelism, which this emulator does not
claim: it reports vCPUs as *threads to GDB* (thread ids) and as contexts to
everything else. A vCPU that hard-stops (HLT+CLI or a fault) is parked off the
rotation; the run ends when every active context has stopped.

## Tooling

Every tool is opt-in and **non-perturbing**: with a tool attached, stdout,
`--stats`, the PPM dump and the log ring are byte-identical to a plain run, and
each tool has a negative control that fails if it ever perturbs.

| Tool | Surface | Hook |
|---|---|---|
| trace | `--trace-file=`, `--trace-from`, `--trace-count` | instruction boundary; CPU-vs-disassembler successor self-check |
| disassembler | `x86dis -b16/-b32/-b64` | standalone, also the trace's oracle |
| REPL debugger | `--debug`, `--debug-script=`, `--symbols=` | break/tbreak, watch r/w/a, `x/Ni`, `info`, `bt`, `xlate`, `set`, `poke`, `savevm/loadvm` |
| GDB stub | `--gdb=[host:]port` | RSP, `target.xml`, vCPU threads, hardware watchpoints, x87 via `info float` |
| snapshots | `--snapshot-save=`, `--snapshot-load=` | whole machine, `AURASNP1` v2 |
| live monitor | `--monitor=[IPv4:]port`, `--monitor-token=` | HTTP + RFC 6455 WebSocket, framebuffer deltas, run control, key injection |
| GUIs | `make gui` (tkinter), `make gui-web` | same back end, argv built from a schema |

The debug/trace hooks are called through `__attribute__((noinline, cold))`
wrappers behind one pointer test, so the untooled run loop carries no cost and
no inlining change — a 3% regression found that way is documented at the site.

Access control (UPKEEP U7) is part of the monitor and launcher contract: the
`Host` header must name this machine (DNS rebinding), a browser `Origin` must be
same-origin, and a capability token (`--monitor-token=`, `X86EMU_MONITOR_TOKEN`
in the environment so it stays out of the process list) gates the WebSocket
upgrade and every launcher API route. A non-loopback bind without a token is
refused at startup, and the transport is plaintext HTTP — hence loopback by
default.

## Snapshots

`AURASNP1` version 2 is the whole machine at a run-loop boundary:

```
header: magic, version, sizeof(machine_t), sizeof(cpu_t)
records: { tag[16], length, bytes } in a fixed visitor order
   RAM (sparse) · machine_t minus host pointers · cpu + every vcpu slot
   every device struct · PCI config spaces · MMIO list incl. runtime ECAM
   RAM windows (VRAM, CAR, DDR) · USB stick and SATA images · log ring
   P2/P3 caches (so --stats stays identical) · device cadence phase
end record
```

Loading verifies the build fingerprint (record tags and sizes) and the machine
shape (platform, ROM, kernel entry, vCPU count, disk and SATA sizes) and refuses
a mismatch **with the reason** rather than misreading it. Harness actions
(`--keys`, `--keys-at`, `--vbe`) are not machine state and are never stored;
`--sata-writethrough` is refused because the host file is outside the model.
The P2/P3 caches are stored deliberately: restoring without them changes
`decode_hits` by one and breaks the "snapshot split equals the uninterrupted
run" receipt.

## Building

```
make                     # x86emu              (-std=c11 -O2 -Wall -Wextra -Werror)
make firmware/firmware.bin disk/disk.img x86dis
make DEBUG_HOOKS=1       # + guest bring-up diagnostics (see below)
make test                # the full gate
make test-unit           # C unit suites only
make test-cli            # CLI input contract + shipped-binary invariant
make test-sanitize       # ASan + UBSan, leaks and UB are hard errors
make fuzz                # differential vs the host CPU through ptrace
make bench / bench-nc    # honest benchmark and its negative control
```

**Guest diagnostics are a compile-time option** (audit A6, patch `0112`). The
K4-era probes (`EMU_DBG_U=1`: per-instruction RIP watches, a probe file sink,
`AURALITE_IVT_LOG`) live behind `-DAURA_DEBUG_HOOKS=1` because their constants —
three literal kernel RIPs, a `/tmp` write from inside the interpreter — have no
business in a shipped, firmware-agnostic core. With the default
`DEBUG_HOOKS=0` those strings are absent from the binary (pinned by
`make test-cli`), and `EMU_DBG_U_RIPS` / `EMU_DBG_U_FILE` name the watch list and
the sink when the option is on.

CI runs three lanes (`.github/workflows/build.yml`): build + `make test`,
`make test-sanitize`, and `make fuzz`.

## Testing

Five layers, each with a different failure mode in mind:

| Layer | Files | Catches |
|---|---|---|
| table-driven ISA + device vectors | `tests/test_*.c` (39 suites) | instruction semantics, register/flags, device registers |
| differential fuzz | `tests/diff_fuzz.c` | flag/register divergence against the **real** CPU via `ptrace`; no-crash invariant on random streams |
| integration shells | `tests/integration/*.sh` (27) | multi-step lanes: AHCI matrix, video modes, NET tx/rx/e2e, trace, debug, gdb, snapshots, monitor, perf compare/history |
| python gates | `tests/test_*.py` (14) | acceptance receipt checking (G3/G5/G6), ISO boot, virtio+SeaBIOS, monitor protocol, GUI/launcher, CLI inputs |
| in-tree evidence | `docs/evidence/` | recorded stdout/stderr bundles with `SHA256SUMS`, re-checked by the acceptance gates |

The acceptance gates read the *recorded* evidence and compare it with a fresh
run, which is why the project can claim "the receipt still matches" instead of
"the test still passes".

### Evidence discipline

From `ROADMAP.md` and `CORE_PLAN.md` s2/s5, and enforced in every phase:

1. **Measured before claimed** — a defect is filed with a harness reproduction.
2. **Fix + regression vector in the same patch**, plus one negative control per
   phase (revert the fix, watch the test redden).
3. **`make test` green at every phase boundary.**
4. **No guest-visible contract of the sample firmware changes** as a side effect
   of a core repair; the firmware is the integration test, not the spec.
5. **Docs sync is part of the definition of done** — phase row, `STATUS.md`
   evidence, and the changelog entry ship together.

## Invariants that must not move

These are the properties a change is most likely to break silently. Each has a
test or a receipt behind it.

| Invariant | Why it exists | Pinned by |
|---|---|---|
| UP behaviour is bit-exact | every recorded receipt runs on one vCPU | `n_vcpus == 1` identity path; all lanes |
| A tool must not perturb | traces/monitors are only useful if the run is unchanged | byte-identical stdout/log/PPM comparisons in the monitor and trace lanes |
| Only successful translations are cached | a fault must raise with the original CR2/error/RIP | PERF P2 contract; `test_tlb` + negative control |
| One RAM-write generation invalidates both caches | O(1) coherency, no stale code | PERF P3 contract |
| Snapshot split equals uninterrupted run | the split lanes must be byte-identical | `test-snapshot` (13 lanes) |
| The shipped interpreter carries no guest constants | firmware-agnostic core | `make test-cli` (greps the binary) |
| The CLI input contract is exit 0/1/2 as documented | scripted lanes must not silently boot another machine | `tests/test_cli_inputs.py` (64 checks) |
| INTx fallback stays byte-identical when MSI is added | historical tapes keep their receipts | MSI M3 lane |
| Time comes from instruction counts | determinism, reproducible artifacts | `--stats` receipts, perf history |
| Device cadence is machine state | EHCI timing after a snapshot load | snapshot visitor (TOOLING T4) |

## Known limits and non-claims

Stated once, here, so the rest of the tree does not have to hedge:

- **Not a QEMU replacement.** The CPU and devices are local; the ISA grows from
  traces and tests. No hardware acceleration, no live migration of a foreign
  format, no device passthrough.
- **SSE is a subset; AVX/SMM/VMX/SVM/NMI are absent.** x87 arithmetic is
  complete; `#UD` is the honest answer everywhere else.
- **Segment limit and CPL checks are modelled where a guest needed them**, not
  as a complete protection unit.
- **SMP is context multiplexing**, not parallel threads (see [SMP](#smp)).
- **Networking is IPv4-only over TAP-as-UDP**, with the protocol subset listed
  above.
- **The monitor is plaintext HTTP**; loopback by default, token required
  elsewhere.
- **xHCI is a QEMU-compatible model** (`1b36:000d`), not a complete USB 3 stack.
- **Snapshots require the same build and machine shape** — by design.
- **The guest lanes need frozen AuraLite-OS artifacts** and skip without them;
  a skip is reported as a skip, never as a pass.

## Extending the emulator

The file you touch first depends on what you are adding:

| Change | Start here | Gate to extend |
|---|---|---|
| An instruction or encoding | `src/cpu.c` (`switch (op)`, `slow_decode`, `really_slow`) | `tests/test_table.c` (table-driven vectors), `tests/diff_fuzz.c` for flags |
| A page-table or protection rule | `src/cpu.c` `translate()` / `page_fault()` | `tests/test_tlb.c`, `tests/test_pam.c` |
| A chipset register | the module (`pic.c`, `pit.c`, `pm.c`, `rtc.c`, `kbc.c`, `lapic.c`, `ioapic.c`) | that module's `tests/test_*.c` |
| A device | its own `src/<name>.c` if it has an engine (`ahci.c`, `xhci.c`, `e1000.c`, `virtio*.c`), else `devices.c`; add the PCI function in `devices_init_common()` | `tests/test_<name>.c` +, if it boots, an integration lane |
| A PCI identity or platform profile | `src/platform.c`, `devices.c` (`--lpc=` pattern) | `tests/test_compat.c` |
| A CLI option | `src/main.c` option loop **and** `tools/launcher.py` `SCHEMA` (both, or the GUI cannot reach it) | `tests/test_cli_inputs.py` (rejection + control) |
| A monitor command or state field | `src/monitor.c` (+ `web/monitor.html`) | `tests/test_monitor.py` (Python-side protocol vectors) |
| A snapshot field | the owning module's `*_snap()` visitor | `tests/test_snapshot.c`, the 13 split lanes |
| A plan phase | a new `.patch` in `patches/`, a row in its plan, a receipt in `STATUS.md` | the phase's own gate + one negative control |

Two habits keep the tree honest: add the failing vector **before** the fix, and
when a behaviour is deliberately partial, write the `#UD`/`refuse` path rather
than a plausible-looking approximation.

## Source map

Counts are from the tree at `f0fc4e7` + patch `0112` (`src/`: 30 `.c` + 28 `.h`,
22,177 lines).

| File | Lines | Role |
|---|---|---|
| `src/cpu.c` | 4,731 | the interpreter: decode, paging, exceptions, x87, SSE, caches |
| `src/cpu.h` | 202 | `cpu_t` layout (a frozen ABI for gdb/snapshots), flags, MSR ids |
| `src/main.c` | 1,013 | CLI parsing and validation, media loading, the run loop, dumps |
| `src/machine.h` | 325 | `machine_t`, MMIO/IO models, vCPU slots, caches, log ring |
| `src/mem.c` / `io.c` | 296 / 28 | physical address space (PAM, A20, MMIO precedence) / port table |
| `src/devices.c` | 1,540 | display + VBE, EHCI/UHCI, USB BOT/SCSI, DDR, CAR, PCI tree |
| `src/ahci.c` / `xhci.c` | 743 / 1,051 | SATA engine (+ATAPI) / USB 3 controller |
| `src/e1000.c` / `virtio*.c` | 684 / 1,169 | NIC + TAP socket / virtio transport, blk, net |
| `src/pic.c` `pit.c` `pm.c` `rtc.c` `kbc.c` `serial.c` | 1,282 | chipset: 8259A, 8254, ACPI PM, MC146818A, 8042, 16550 |
| `src/lapic.c` / `ioapic.c` | 515 / 149 | APIC pair (IRR/ISR, timer, IPIs, MSI sink) / 82093AA |
| `src/pci.c` / `platform.c` / `chipset.c` | 489 | config #1 + ECAM / five CPUID profiles / A20 and reset |
| `src/acpi.c` / `kloader.c` | 181 / 493 | table set builder / ELF64 kernel lane + `boot_info_t` |
| `src/dbg.c` / `gdbstub.c` | 1,438 / 696 | run control, symbols, REPL / GDB RSP |
| `src/disasm.c` / `ws.c` / `monitor.c` | 1,362 / 160 / 1,404 | disassembler / SHA-1 + RFC 6455 / HTTP + live monitor |
| `src/snapshot.c` | 335 | `AURASNP1` v2 save/load and shape refusal |
| `firmware/sample_firmware.nas` | 849 | MIT sample ROM: real mode → long mode → `0x100000` |
| `tools/` | 4,209 | `x86dis`, `launcher.py`, `gui_app.py`, `tap_responder.py`, `mkiso.py` |
| `tests/` | 25,523 | 39 C suites, 14 Python gates, 27 integration shells |

## Documentation map

| Document | Purpose |
|---|---|
| [`../README.md`](../README.md) | overview, quickstart, option reference |
| [`../ROADMAP.md`](../ROADMAP.md) | plan ledger, north star, measured gaps |
| [`plans/`](plans/) | 17 dependency-ordered plans, one `.patch` per phase |
| [`STATUS.md`](STATUS.md) | what works now, with measured receipts |
| [`../CHANGELOG.md`](../CHANGELOG.md) | chronological changes, defect by defect |
| [`memory_map.md`](memory_map.md) | physical, I/O, PCI and kernel-lane layout |
| [`BUGS.md`](BUGS.md) | firmware observations and their evidence |
| [`WINDOWS.md`](WINDOWS.md) | MinGW-w64 host build |
| [`evidence/`](evidence/) | recorded stdout/stderr bundles + `SHA256SUMS` |
| [`research/`](research/) | measurement notes (e.g. the rejected PERF P4 dispatch) |
