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
| `docs/plans/VIDEO_PLAN.md` | ✅ **CLOSED** (V0–V3 done ✅, plus V4 ✅ — a post-closure heap-overflow fix: the dispi mode registers were not clamped to VRAM, so `--vbe=1024x768x32` wrote 3145728 bytes out of a 1920000-byte window, ASan RC=1 / unsanitized RC=134; QEMU's `vbe_fixup_regs` transcribed in full, historical video artifacts proved byte-identical pre/post) | VGA text mode 3 (`0xB8000` + CRTC cursor), Bochs VBE (dispi 0x1CE/0x1CF), mode switching | V0–V4 |
| `docs/plans/NET_PLAN.md` | ✅ **CLOSED** (N0–N3 done ✅; the guest's whole online boot path is measured on the real wire — DHCP lease, ARP, ICMP echo, DNS over UDP **and** over the RFC 1035 TCP carriage, and 16 concurrent TCP connections held and closed: 76 frames, byte-identical twice, `N3OK`) | e1000 model (the OS's primary NIC) + host TAP backend + host responder | N0–N3 |
| [`docs/plans/PERF_PLAN.md`](docs/plans/PERF_PLAN.md) | 🚧 P0 ✅ (0051), P1 ✅ (0052), P2 ✅ (0053), P3 ✅ (0054), P4 NO-GO (measured), P5 ✅ (0055) | honest bench, profile-guided RAM read, success-only translation TLB and RAM-only prefix/opcode predecode; P4 dispatch rejected by measurement; P5 paired CI / work-clock close-out | P0–P5 |
| [`docs/plans/TOOLING_PLAN.md`](docs/plans/TOOLING_PLAN.md) | ✅ **CLOSED** (T0–T5 delivered, 0056–0061) | measured baseline (trace = log ring, receipts 5 → 0; static monitor mock; no stop/inspect/persist/remote facility), then trace disassembly, run-control REPL, **gdbstub**, snapshots, live WebSocket monitor for `web/monitor.html` — all opt-in and non-perturbing | T0–T5 |
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

- ~~`0056` — TOOLING T0: measured baseline + `docs/plans/TOOLING_PLAN.md`
  (`--trace` floods the 4096-line ring: 4,095 of 4,837,677 lines kept and
  the five boot receipts 5 → 0; trace costs ≈8–24× and switches off P3
  predecode; no breakpoint/gdbstub/snapshot; `web/monitor.html` is a
  static mock with fabricated values; run state lives in `main()` locals)~~
  shipped.
- ~~`0057` — TOOLING T1: disassembler + `--trace-file=` with CPU-vs-
  disassembler self-check (0 mismatches / 0 undecodable over five firmware
  boots and 40M guest steps UP+SMP), ndisasm differential (226,884
  encodings, 0 unexplained), `tools/x86dis`~~ shipped.
- ~~`0058` — TOOLING T2: run-control core + REPL (`--debug`,
  `--debug-script=`, `--symbols=`); exact stops and byte-identical stdout
  versus the plain run on five platforms and the guest with `--smp=2`~~
  shipped.
- ~~`0059` — TOOLING T3: gdbstub (`--gdb=`); real gdb debugs the frozen
  guest at source level (`kmain` at kernel/kernel.c:196, two vCPU
  threads)~~ shipped.
- ~~`0060` — TOOLING T4: snapshots (`--snapshot-save=`/`--snapshot-load=`,
  `savevm`/`loadvm`); fourteen split lanes incl. the frozen guest SMP are
  byte-identical to straight runs~~ shipped.
- ~~`0061` — TOOLING T5: live WebSocket monitor (`--monitor=`) + close-out;
  `make test`/`make test-sanitize` green, 54 HTTP/WebSocket/control checks,
  framebuffer marker and byte-identical stdout/log/PPM; wrong-GUID NC~~ shipped.

- ~~`0051` — PERF P0: honest instruction-attempt counter and opt-in `--stats`,
  seven-tape O2 benchmark on a separate binary, cap-based MIPS negative
  control, deterministic work-count gates, six unit vectors, all historical
  receipts unchanged~~ shipped.  `make bench BENCH_REPS=5` reports local
  machine-relative wall time, not a cross-host performance assertion.
- ~~`0052` — PERF P1: five-run O2 gprof baseline showed `mem_read` at
  45.66% of sampled time (313,666,250 calls; 235,535,030 page-walk reads).
  Optimize only in-range, A20-open 8-byte RAM reads with unaligned-safe
  memcpy; six bus vectors, LE-swap NC, full gates, seven-tape alternating
  pre/post with identical work counts, historical tapes byte-identical~~
  shipped.  Local ratios 1.18–1.83× across two paired 3-rep series have
  visible host variance; no fixed cross-runner wall-time gate.
- ~~`0053` — PERF P2: P1-followup O2 gprof measured `translate` 22.10%
  of samples / 79,798,615 calls over five boots; success-only shared TLB
  with 4KiB subpage keys including CR3/CR0/CR4/EFER/AC/CPL/desc_sv/mode,
  conservative O(1) global generation flush on EVERY guest/DMA RAM store,
  MOV CR0/CR3/CR4/LMSW/WRMSR EFER/INVLPG/reset; dynamic MMIO/ROM tables
  and A20-off walks uncached. Real #PF err/RIP/CR2 vectors, PTE-rewrite NC,
  seven-tape paired measurement, byte-identical historical receipts~~ shipped.
- ~~`0054` — PERF P3: O2 profile showed `decode_modrm` 8%, `fetch8` 6%,
  with 970,913 redundant `group1` re-fetches per boot. Cache ONLY single-page
  ordinary-RAM prefix/opcode on successful P2 EXEC, not live ModRM/EA/imm;
  shared RAM-write epoch, SMC/PTE/NX/MMIO/mode vectors and one deliberate
  stale-opcode NC, seven paired P0 tapes, historical byte identity~~ shipped.
  SMP2 ratio ~1.02×; no universal speed claim.
- `P4` — measured NO-GO, not a patch: the switch cost was not isolated in
  O2 gprof and a scratch direct-group1 route had mixed seven-tape paired
  results. See `docs/research/PERF_P4_GATE.md`; no dispatch table or NC
  was invented. Reopen only with a genuinely hot dispatch profile.
- ~~`0055` — PERF P5: paired nine-rep seven-tape O2 comparison with seeded
  bootstrap local 95% intervals, exact guest work/K6 clock equality and
  a deliberate mismatched-clock NC, defect D0–D8 vector ledger, nine
  byte-identical historical lanes, full gates and fresh-clone git am~~
  shipped. SMP interval includes 1; no claimed P5 runtime speedup.



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
- ~~`0042` — VIDEO V0: measured baseline + `docs/plans/VIDEO_PLAN.md` (defect ledger: `0xB8000` is plain RAM with no display path, no CRTC `0x3D4/0x3D5` cursor pair, no Bochs VBE dispi `0x1CE/0x1CF`, no mode state; guest contract measured from `fb.c` VGA-text fallback + `stage2_start.asm` mode-3 force + QEMU `-vga std`)~~ shipped
- ~~`0043` — VIDEO V1: VGA text mode 3 (`0xB8000` cells as guest RAM + CRTC `0x3D4/0x3D5` cursor pair, 8x16 glyph rendering from the guest's own PSF2 font 256/256 byte-identical to `psf2_default_font.inc`, display routing text↔LFB with `--no-fb` lock), CLI `--no-fb`/`--dump-text`, `tests/test_video.c` 4 vectors (cells/layout, CRTC pair, glyph pixels, display routing); guest `--no-fb` lane hits the `fb.c:215` fallback receipt; normal guest lanes byte-identical to U5-era tapes; NC = CRTC-handler revert reddens the cursor-pair vector (RC=134)~~ shipped
- ~~`0044` — VIDEO V2: Bochs VBE dispi (ports `0x1CE/0x1CF`) per the QEMU `vga.c` contract (ID `0xB0C0..0xB0C5` accept-list + ID0 reset, XRES `<=16000 && %8`, YRES `<=12000`, BPP `0->8` + `{4,8,15,16,24,32}`, GETCAPS maxima, `VIDEO_MEMORY_64K=29` read-only, BANK mask 28 + `bank_offset`, ENABLE edge: LFB display on enable / previous display on disable + `VIRT_*` fixup); `tests/test_video.c` 8 vectors; guest baseline never touches dispi (measured) and both U5-era guest lanes stay byte-identical; NC = ID accept-list removal reddens the id-ladder vector (RC=134)~~ shipped
- ~~`0045` — VIDEO V3: mode switching + close-out -- the text-3 ↔ VBE-graphics machine state (dispi ENABLE edge, GPU BAR0/kload publish, GC `VGA_GR06_GRAPHICS_MODE` at `0x3CE/0x3CF` so stage2's INT 0x10 (AL=3) lands in mode 3 on the hardware side), QEMU-literal mode-set memory (`memset(YRES * vbe_line_offset)` unless `DISPI_NOCLEARMEM`), `--vbe=WxHxBPP|off` harness-side VBE client through the real dispi ports, `tests/integration/test_video_modes.sh` (mode-switch + V1 guest lanes x2: six byte-identical artifact pairs, display assertions per mode), `tests/test_video.c` 10 vectors; NC = NOCLEARMEM-clear removal reddens the clear vector (RC=134)~~ shipped
