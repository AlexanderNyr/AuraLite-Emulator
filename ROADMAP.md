# ROADMAP — AuraLite Emulator

> **House style** (adopted from
> [`AuraLite-OS/docs/plans`](https://github.com/AlexanderNyr/AuraLite-OS/tree/main/docs/plans)):
> each effort is a dependency-ordered plan in `docs/plans/` with a definition
> of done and a test gate per phase, and ships as **one `.patch` per phase**
> under `patches/`. Every claim in these docs is measured against the tree,
> not assumed.

## North star

```
  AuraLite-Emulator boots the AuraLite OS kernel to its shell.
```

AuraLite OS is the companion from-scratch x86-64 OS. Its kernel requires from
the machine: long mode + higher-half paging, GDT/IDT/**TSS**, **PIC** IRQ
dispatch (later LAPIC for SMP), **SYSCALL/SYSRET** (Ring 3!), UART, a linear
framebuffer, **AHCI** storage, **e1000** networking, and USB MSC
(UHCI/EHCI/xHCI — it speaks all three; the emulator has EHCI). Each of those
maps to exactly one plan below.

## Plan ledger

| Plan | Status | Scope | Phases |
|---|---|---|---|
| [`docs/plans/CORE_PLAN.md`](docs/plans/CORE_PLAN.md) | ✅ **CLOSED** (C0–C11 done ✅) | CPU correctness: measured defect ledger, ISA completeness, test rig | C0–C11 |
| [`docs/plans/CHIPSET_PLAN.md`](docs/plans/CHIPSET_PLAN.md) | ✅ **CLOSED** (H0–H7 done ✅) | IRQ delivery at instruction boundaries, PIC 8259, PIT 8254, RTC/CMOS, KBC 8042, **A20 gate**, port 0x92/0xCF9 reset; LAPIC+IOAPIC (SMP stretch) | H0–H7 |
| [`docs/plans/KERNEL_BOOT_PLAN.md`](docs/plans/KERNEL_BOOT_PLAN.md) | ✅ **CLOSED** (K0–K8 done ✅; north star met) | `--kernel=` lane boots AuraLite-OS `kernel.elf` to its shell on 1 and 2 vCPUs; guest receipts measured end-to-end: `(1 AP(s) woken)`, R5 cross-CPU scheduling receipt, `SMPSTRESS PASS` + `IRQAPWAKE PASS` (parity with the OS repo's QEMU -smp 2 gate) | K0–K8 |
| [`docs/plans/STORE_PLAN.md`](docs/plans/STORE_PLAN.md) | ✅ **CLOSED** (S0–S5 done ✅; measured receipts in the plan + `docs/STATUS.md`) | real AHCI HBA: ABAR/ports/presence, command-list+FIS engine, IDENTIFY, READ/WRITE DMA EXT, up to 4 disks, second controller, 128-KiB transfers, writethrough, honest INTx | S0–S5 |
| [`docs/plans/USB_PLAN.md`](docs/plans/USB_PLAN.md) | ✅ **CLOSED** (U0–U5 done ✅; both sticks through the guest's MSC path with byte-identical lane pairs) | BOT/CSW correctness (tag echo, residue, status byte), INQUIRY/READ CAPACITY/TEST UNIT READY, doorbell on USBCMD only, UHCI controller model for AuraLite OS parity | U0–U5 |
| `docs/plans/VIDEO_PLAN.md` | 📋 planned | Bochs VBE (dispi 0x1CE/0x1CF), VGA text mode 0xB8000, mode switching | V0–V3 |
| `docs/plans/NET_PLAN.md` | 📋 planned | e1000 model (the OS's primary NIC) + host TAP backend | N0–N3 |
| `docs/plans/PERF_PLAN.md` | 📋 planned | measurement rig first; RAM fast path (per-byte loops today), translation TLB, decode cache, threaded dispatch | P0–P5 |
| `docs/plans/TOOLING_PLAN.md` | 📋 planned | built-in debugger REPL, **gdbstub**, live WebSocket monitor for `web/monitor.html`, snapshots, trace disassembly | T0–T5 |
| `docs/plans/COMPAT_PLAN.md` | 📋 planned | the north-star integration: flat kernel load → AuraLite OS stage2 → shell; ACPI/e820 surfaces; SMP (multi-vCPU) as the final rung | G0–G6 |

Ordering is dependency-driven: CORE → (CHIPSET, STORE, USB) → VIDEO/NET →
COMPAT; PERF and TOOLING run in parallel once CORE's differential rig exists.

## Standing rules (per `CORE_PLAN.md` §2/§5)

1. **Measured before claimed.** A defect is filed with a harness repro or it
   isn't filed.
2. **Fix + regression vector in the same patch**, with a once-per-phase
   negative control (revert the fix, watch the test redden).
3. **`make test` green at every phase boundary** — unit, USB, boot-integration,
   5-platform smoke.
4. **No guest-visible contract of the sample firmware is changed** by core
   repairs; the firmware is the integration test, not the spec.
5. Docs sync is part of each phase's definition of done: phase row in its
   plan + `docs/STATUS.md` evidence, same patch.

## Immediate queue (next patches)

- ~~`0008` — CORE C7: string ops + REP semantics~~ shipped
- ~~`0009` — CORE C8: XCHG/CMOVcc/SETcc/MOVSX/CBW family/LOOP/JCXZ/BSWAP/XADD/CMPXCHG~~ shipped
- ~~`0010` — CORE C9: CPUID leaves, RDTSC, PAUSE/fences~~ shipped
- ~~`0011` — CORE C10: test rig, ASan/UBSan CI lanes, Makefile hygiene, CHANGELOG~~ shipped
- ~~`0012` — CORE C11: differential fuzzer vs host CPU, crash-invariant~~ shipped
- ~~`0013` — CHIPSET H0: IRQ delivery scaffold + PIC 8259 (sketch)~~ shipped
- ~~`0014` — CHIPSET H1: full PIC (priorities/rotation, special mask, spurious IRQ, poll, level lines)~~ shipped
- ~~`0015` — CHIPSET H2: PIT 8254 (counter 0 periodic/one-shot, counter 2 + port 0x61, virtual-time tick)~~ shipped
- ~~`0016` — CHIPSET H3: RTC/CMOS (clock registers, update-ended IRQ8, equipment/memory CMOS bytes)~~ shipped
- ~~`0017` — CHIPSET H4: KBC 8042 (status/command/data ports + IRQ1, scancode set-1 injection, A20 command consumed by H5)~~ shipped
- ~~`0018` — CHIPSET H5: A20 gate + warm reset (A20 = 8042-outport-bit1 OR port-0x92-bit1, masked on the bus; 0x92 bit0 + 0xCF9 + KBC 0xFE/outport-bit0 → boundary-consumed full warm reset)~~ shipped
- ~~`0019` — CHIPSET H6: LAPIC (MMIO 0xFEE00000, SVR, TPR/PPR, self-IPI, timer; +IST/LTR CPU support)~~ shipped
- ~~`0020` — CHIPSET H7: IOAPIC (IOREGSEL/IOWIN, 24 RTEs, edge-latch + level remote-IRR, LAPIC delivery; PIC-line fan-out; PAIR-TEST 8254→IOAPIC→LAPIC→CPU)~~ shipped — CHIPSET plan complete H0–H7
- ~~`0021` — KERNEL-BOOT K1: direct kernel-load lane (`--kernel=` ELF64 placement, loader-built paging w/ identity+HHDM, fabricated boot_info_t, measured to `kmain` @ instr 18)~~ shipped
- ~~`0022` — KERNEL-BOOT K2: ISA-gap closure to the kernel banner (measured ISA/device deltas)~~ shipped
- ~~`0023` — KERNEL-BOOT K3: probe-path hardware (MADT/hypervisor/fw_cfg receipts, sched PASS)~~ shipped
- ~~`0024`/`0025` — KERNEL-BOOT K4: Ring-3 userspace + serial-keyboard input lane (initrd → `auralite#`, commands typed over the KBC)~~ shipped
- ~~`0026` — KERNEL-BOOT K5: SMP metering (deterministic instruction/vtime accounting before multi-vCPU)~~ shipped
- ~~`0027` — KERNEL-BOOT K6: second vCPU, INIT/SIPI/SIPI wake, strict 1:1 round-robin contexts, AP landing receipt~~ shipped
- ~~`0028` — KERNEL-BOOT K7: virtual CPU speed honesty (~14 → ~373 virtual MIPS; PIT/RTC pin 12→384, coherent TSC/LAPIC) → guest receipt `(1 AP(s) woken)`~~ shipped
- ~~`0029` — KERNEL-BOOT K8: SMP clock pacing (hlt-idle clock yield + per-vCPU LAPIC timer settle on idle slots) → guest userspace gates `SMPSTRESS PASS` + `IRQAPWAKE PASS`~~ shipped — KERNEL_BOOT plan closed for the north star (2-vCPU shell + guest SMP gates)
- ~~`0030` — STORE S0: measured baseline + `docs/plans/STORE_PLAN.md`~~ shipped
- ~~`0031` — STORE S1: AHCI HBA registers + presence (ABAR 0xFEB10000, `--sata=` CLI, receipt: guest binds controller, attached disk reported)~~ shipped
- ~~`0032` — STORE S2: command engine + READ DMA EXT (guest self-test blank-LBA0 receipt)~~ shipped
- ~~`0033` — STORE S3: WRITE DMA EXT shares the engine; guest `/disk`+`/fat` token round-trips typed via KBC, determinism pair byte-identical~~ shipped
- ~~`0034` — STORE S4: breadth matrix -- `--sata-portN=` placement, `--ahci2` second controller (0:31:3, ABAR2 0xFEB12000), QEMU `test_ahci_matrix.sh` lanes A/B/C reproduced guest-side + unattached negative control~~ shipped
- ~~`0035` — STORE S5: large transfers + hardening -- 128-KiB PRDT vectors + QEMU `test_ahci_large_read.sh` guest parity (16 MiB payload through the 128-KiB bounce), `--sata-writethrough` dirty-image semantics, honest INTx (PxIS→PIC/IOAPIC), full-matrix determinism ×2~~ shipped — STORE plan complete S0–S5
- ~~`0036` — USB U0: measured baseline + `docs/plans/USB_PLAN.md` (defect ledger: CSW tag/residue/status, ignored SCSI opcodes, doorbell on every write, zeroed caps, no control/UHCI)~~ shipped
- ~~`0037` — USB U1: BOT/CSW correctness -- `dCSWTag` echo (guest-validated), `dCSWDataResidue` expected-minus-moved, OK/FAILED/PHASE status byte, 13-byte-IN status framing, 7 machine vectors + firmware negative control~~ shipped
- ~~`0038` — USB U2: SCSI enumeration set -- INQUIRY (36-byte descriptor), READ CAPACITY(10) (`disk_len/512-1`), TEST UNIT READY (FAILED + sense 2/3A without image), REQUEST SENSE (18-byte, consumed), ILLEGAL REQUEST 5/20; 14 machine vectors~~ shipped
- ~~`0039` — USB U3: EHCI register-file honesty + control -- CAPLENGTH/HCIVERSION/HCSPARAMS/HCCPARAMS, USBCMD RUN/stop/HCRESET walk gate + doorbell on USBCMD only, USBSTS USBINT/IHS/SEmpty/HCHALTED map, USBINTR, per-qTD SETUP with Data/Status stages; 22 vectors + QEMU `run_qemu_usb_msc.sh` boot-parity receipts + negative control (unconditional walk reddens `test_usb.c:352`)~~ shipped
- ~~`0040` — USB U4: UHCI root host controller -- PCI 0:1.2 `8086:0x7020`, I/O BAR4 `0xC040` register file (USBSTS.HCHALTED = bit 5), frame list + QH/TD walk (ctrl[10:0] actual-length contract, IOC→USBINT), control+bulk through the shared device model (fix: disk path advances media offset by `st->moved`), QEMU `run_qemu_usb_msc.sh` parity (`[uhci] controller at PCI 0:1.2` + `I/O base = 0xc040` + usbfs receipts over UHCI bulk), 25 vectors + negative control (halted controller runs no frames)~~ shipped
- ~~`0041` — USB U5: end-to-end + hardening close-out -- `--no-usb-uhci` (the `test_usb_ehci.sh` EHCI-only machine shape, `ok uhci omitted`), both sticks through the guest's MSC path (`/usb` info + `run /apps/filesize /usb/sector0.bin` typed over the KBC) with byte-identical x2 lane pairs (UHCI/AURALUSB + EHCI/AURALEHC), `MATRIX_DETERMINISM=1` matrix green with pairs byte-identical and AHCI/fs receipts byte-identical to the S5-era lanes, frozen-kernel reproducibility (`SOURCE_DATE_EPOCH` banner + 92160-byte trimmed initrd reconstruction), CHANGELOG U1–U5, 26 vectors~~ shipped — USB plan closed for both-stick MSC parity (U0–U5)
