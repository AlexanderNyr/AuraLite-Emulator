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
| [`docs/plans/CORE_PLAN.md`](docs/plans/CORE_PLAN.md) | 🔵 **IN PROGRESS** (C0–C6 done ✅, C7–C11 planned) | CPU correctness: measured defect ledger, ISA completeness, test rig | C0–C11 |
| `docs/plans/CHIPSET_PLAN.md` | 📋 planned | IRQ delivery at instruction boundaries, PIC 8259, PIT 8254, RTC/CMOS, KBC 8042, **A20 gate**, port 0x92/0xCF9 reset; LAPIC+IOAPIC (SMP stretch) | H0–H7 |
| `docs/plans/STORE_PLAN.md` | 📋 planned | real AHCI HBA (command lists, FIS, IDENTIFY, READ/WRITE DMA EXT); disk geometry; second-disk support | S0–S5 |
| `docs/plans/USB_PLAN.md` | 📋 planned | finish BOT/CSW correctness (tag echo, residue, status byte), INQUIRY/READ CAPACITY/TEST UNIT READY, doorbell on USBCMD only, UHCI controller model for AuraLite OS parity | U0–U5 |
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
- `0013` — CHIPSET H0: IRQ delivery scaffold + PIC 8259 (sketch)  ← next
