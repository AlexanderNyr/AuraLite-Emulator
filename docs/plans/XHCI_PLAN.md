# XHCI Plan — a real USB 3.0 host controller

> Companion to `ROADMAP.md` (audit 2026-10-19 candidate #1) and to the
> closed `docs/plans/USB_PLAN.md` (EHCI U3 + UHCI U4). The north star
> names xHCI: AuraLite OS speaks UHCI/OHCI/EHCI/xHCI and drives all four
> from `drivers/usb/xhci.c` (90 KB -- real command/event/transfer rings,
> slot addressing, endpoint contexts). The machine has UHCI + EHCI only;
> a class 0x0C/0x03/prog-if 0x30 scan of bus 0 finds nothing today.

**Status: X0–X5 done ✅ COMPLETE**

| Phase | Scope | Status | Deliverable |
|---|---|---|---|
| X0 | Measured baseline: the guest contract, the QEMU identity, our topology, the defect ledger | ✅ done | this file (`patches/0076`) |
| X1 | PCI + register file: identity/BAR/MMIO map, capability + operational + runtime + doorbell + port registers, HCRST/CNR handshake, port power + RW1C discipline | ✅ done | `src/xhci.c` + `tests/test_xhci.c` (9 groups) (`patches/0077`) |
| X2 | Command ring engine: CRCR/Link/cycle discipline, No Op round-trip + ring wrap, Enable/Disable Slot, Command Completion events | ✅ done | `tests/test_xhci.c` X2 groups (13 total) (`patches/0078`) |
| X3 | Contexts + Address Device + EP0 control transfers: DCBAA/slot/EP contexts (CSZ=0), SETUP/DATA/STATUS stages, Transfer Events, Evaluate/Configure/Reset/Stop + Set TR Dequeue | ✅ done | `tests/test_xhci.c` X3 groups (17 total) (`patches/0079`) |
| X4 | Bulk + interrupt transfers through the shared device model (the U1–U5 MSC/HID sticks) | ✅ done | `tests/test_xhci.c` X4 groups (19 total) (`patches/0080`) |
| X5 | Interrupter + ports + close-out: IMAN/IMOD/ERST/ERDP, EINT/PCD, INTx delivery, hotplug (CSC / Port Status Change events), determinism x2, guest receipts where artifacts exist | ✅ done | `tests/test_xhci.c` X5 groups (22 total) (`patches/0081`) |

Each phase ships one `.patch` under `patches/` with measured receipts, a
regression vector, one once-per-phase negative control, green gates and
the docs update in the same commit (the `ROADMAP.md` standing rules).

## 1. Measured baseline (X0)

### 1a. The guest contract (AuraLite-OS@main `drivers/usb/xhci.c` + `xhci.h`)

Everything the machine must model, measured from the driver that must
run on it. Line numbers refer to that file (90,246 bytes, fetched
2026-10-19).

**Probe** (xhci.c:443–468): bus-0 scan for class 0x0C / subclass 0x03 /
prog-if 0x30 -- any vendor/device; `pci_enable_bus_master`; BAR0 mapped
64 KiB (`paging_map` per 4 KiB page), 64-bit BARs accepted (BAR type
0x4 -> BAR1 is the upper dword). No vendor/device filtering at all, so
the PCI identity is free -- we take QEMU's (see 1b). Interrupt: PCI cfg
`0x3C` line registered through `irq_register_handler` (xhci.c:688–698)
-- **legacy INTx, not MSI**; a line >= 16 degrades to a 500 ms hotplug
poll.

**Capability registers** (xhci.c:43–68, 492–508) -- offsets from BAR0:

| off | reg | fields the guest consumes |
|-----|-----|---------------------------|
| 0x00 | CAPLENGTH / HCIVERSION | one dword read (`cap_rd32(0)`); CAPLENGTH in bits 7:0 -> op base; HCIVERSION in bits 31:16, printed `%x.%02x` |
| 0x04 | HCSPARAMS1 | MaxSlots 7:0, MaxIntrs 15:8, MaxPorts 31:24 ("QEMU's xHCI uses bits 24-31 for MaxPorts") |
| 0x08 | HCSPARAMS2 | scratchpad lo 19:15 + hi 25:21 (10-bit count) |
| 0x0C | HCSPARAMS3 | read, unused |
| 0x10 | HCCPARAMS1 | AC64 bit0, CSZ bit2 (0 = 32-byte contexts), PPC bit3 |
| 0x14 | DBOFF | doorbell array base |
| 0x18 | RTSOFF | runtime base |
| 0x1C | HCCPARAMS2 | read, unused |

**Operational registers** (xhci.c:78–109, at BAR0+CAPLENGTH): USBCMD
0x00 (RUN bit0, HCRST bit1, INTE bit2), USBSTS 0x04 (HCH bit0, EINT
bit3 RW1C, PCD bit4, CNR bit11), PAGESIZE 0x08, DNCTRL 0x14, CRCR 0x18
(RCS bit0, CS bit1, CA bit2, CRR bit3; pointer bits 63:3), DCBAAP 0x30
(64-bit), CONFIG 0x38 (MaxSlotsEn 7:0).

**Ports** (xhci.c:125–149, 324–329): PORTSC at op+0x400, stride 0x10.
Bits consumed: CCS0, PED1, OCA3, PR4, PLS 8:5, PP9, SPEED 13:10
(1=FS 2=LS 3=HS 4=SS), LWS16, CSC17, PEC18, WRC19, OCC20, PRC21, PLC22,
CEC23, WCE25, WDE26, WOE27, DR30, WPR31. The driver is strict about
RW1C: change bits (CSC/PEC/WRC/OCC/PRC/PLC/CEC) must clear on write-1
and must never be acknowledged accidentally (xhci.c:712–727).

**Runtime / interrupter 0** (xhci.c:113–120, 645–650): IMAN 0x20 (IP
bit0 RW1C, IE bit1), IMOD 0x24, ERSTSZ 0x28, ERSTBA 0x30 (64-bit),
ERDP 0x38 (64-bit). Interrupter *n* at 0x20 + 0x20*n*.

**Init sequence** (xhci.c:517–747) -- the exact bring-up the machine
must survive:

1. USBCMD=0, wait USBSTS.HCH;
2. USBCMD=HCRST, wait HCRST clear (1e6 spin budget);
3. wait USBSTS.CNR clear after reset;
4. CONFIG=MaxSlotsEn;
5. DCBAA = (MaxSlots+1)*8 bytes (entry 0 = scratchpad array);
6. scratchpads if HCSPARAMS2 says so;
7. DCBAAP lo/hi written as two dwords (DMA below 4 GiB);
8. command ring: 1 page (256 TRBs), Link TRB at [255] carrying the
   *current* cycle + TC (the guest's own U1 fix); CRCR = ring_phys|RCS(1);
9. event ring: 1 page (256 TRBs) + ERST[0] {addr_lo, addr_hi, size=256};
   interrupter 0: ERSTSZ=1, ERSTBA lo/hi, ERDP lo/hi, IMAN=IE, IMOD=0;
10. port power: write PP to every port regardless of PPC (RW1C-safe
    single-bit write), 20 ms settle;
11. USBCMD=INTE|RUN; wake enables WCE|WDE on every port (RW1C-safe);
    register the INTx line; wait HCH clear ("controller running");
    100 ms settle; PORTSC scan (CCS/SPEED) acknowledging change bits
    deliberately.

**Rings and TRBs** (xhci.c:172–206, 842–1010): 16-byte TRBs, type in
bits 15:10 of the spec Control dword, cycle bit 0. The guest's struct
names (`param,status,control,flags`) sit at spec offsets 0/4/8/12 --
its `flags` field IS the spec Control dword. Commands used: No Op (23),
Enable Slot (9), Disable Slot (10), Address Device (11), Configure
Endpoint (12), Evaluate Context (13), Reset Endpoint (15), Set TR
Dequeue (16); events: Command Completion (33, slot id in the Control
dword's high byte), Transfer (32), Port Status Change (34); transfer:
Normal (1), Setup/Data/Status Stage (2/3/4), Isoch (5), Link (6, TC
bit1). Submission writes the TRB with the current cycle, rings doorbell
0 (`db_wr(0,0)`) and **polls the event ring** (xhci_poll_event_type) --
so deterministic synchronous completion at doorbell time is legal and
the IRQ is only the hotplug/take path (U8). Completion codes: 1 =
Success; the driver distinguishes "never answered" (its own timeout
diagnostic) from "answered with cc != 1".

**Receipts** (kprintf strings X5 pins): `[xhci] controller at PCI
%u:%u.%u`, `[xhci] HCI version %x.%02x, %d ports, %d slots, %d
scratchpads, 64-bit=%d, context=%d bytes, PPC=%d`, `[xhci] IRQ line %u
registered (INTE=1, IMAN.IE=1)`, `[xhci] controller running`,
`[xhci] starting port scan: %d ports`, `[xhci] port %d: device attached
(%s) (PORTSC=0x%08x)`, `[xhci] %d device(s) ready`, `[xhci] command
ring: No Op -> Success (cc=1)`, `[xhci] command ring: PASS — 256/256
No Ops across a ring wrap`, `[xhci] slot %u addressed (root port %u,
route=0x%05x, tier=%d, ...)`, `[xhci] configured ep 0x%02x slot=%u
ep_id=%d maxpkt=%u`. The guest's own gates: `test_xhci_control`,
`test_xhci_bulk` (1 MiB verified pattern), `test_xhci_interrupt`,
`test_xhci_address`, `test_usb_msc` over xHCI bulk.

### 1b. QEMU identity

`qemu-xhci` is PCI **1b36:000d** ("PCI xhci usb host adapter"; QEMU
`docs/specs/pci-ids`, `lspci`: "Red Hat, Inc. QEMU XHCI Host Controller
[1b36:000d] (rev 01) (prog-if 30 [XHCI])"). The guest's QEMU lanes
(`-device qemu-xhci`) are the X5 parity oracle. The guest never checks
the identity (class match only), so identity parity costs nothing and
keeps full-receipt comparisons possible.

### 1c. Our machine today (measured)

PCI topology (`src/devices.c:1287–1421`): 0:0.0 host-bridge 8086:29c0;
0:1.0 pci-bridge-misc 8086:0101; 0:1.2 UHCI 8086:7020 (0C/03/00);
0:2.0 igpu 8086:0412; 0:3.0 EHCI 8086:1E26 (0C/03/20); 0:31.0 LPC;
0:31.2 AHCI 8086:1E03; 0:31.3 AHCI2 8086:2922 (with `--ahci2`). There
is no xHCI model in `src/` (grep), no class 0x0C/0x03/0x30 function on
bus 0, and the guest on this machine ends `xhci_init` with
`[xhci] no xHCI controller found` (xhci.c:464) -- the X0 baseline
receipt.

### 1d. Defect ledger

| # | Defect (measured absence) | Consequence | Phase |
|---|---------------------------|-------------|-------|
| D1 | no PCI function with class 0x0C/0x03/0x30 | probe returns -1, no controller | X1 |
| D2 | no capability/operational/runtime/doorbell/port register file | nothing to program | X1 |
| D3 | no HCRST/CNR handshake | init stalls at step 2/3 | X1 |
| D4 | no command ring engine (CRCR/Link/cycle, doorbell 0) | no command completes | X2 |
| D5 | no DCBAA / slot / endpoint contexts | no devices addressable | X3 |
| D6 | no transfer rings / transfer events | no enumeration, no data | X3/X4 |
| D7 | no interrupter / INTx / Port Status Change events | hotplug dead, U8 receipts impossible | X5 |

## 1e. X1 measured receipts (PCI + register file)

- **Identity/topology pins** (`tests/test_xhci.c`, 9 groups, all through
  the real 0xCF8/0xCFC mechanism + mem_read/mem_write): the guest's
  class scan finds **1b36:000d rev 01 at 0:20.0** (subsystem 1af4:1100,
  cfg 0x3C=10 / 0x3D=1); BAR0 seeded 64-bit at **0xFEB20000** (measured
  free: EHCI 0xFEB00000, AHCI 0xFEB10000/0xFEB12000, e1000 0xFEBC0000);
  the sizing probe reads 0xFFFF0004 (mask + 64-bit type), BAR1 probe 0,
  and a firmware relocation rebinds the decode window (the AHCI BAR5
  pattern, probe flag included).
- **Register file**: caps dword `0x01000040`; HCSPARAMS1 = 32 slots /
  1 interrupter / 4 ports; HCSPARAMS2=0 (the guest skips its scratchpad
  step); HCCPARAMS1 = AC64 (CSZ=0, PPC=0); DBOFF 0x1000 / RTSOFF 0x800.
  HCRST wipes the programmed state and self-clears with CNR=0
  immediately (the guest's 1e6 spin budgets never burn); CONFIG/DCBAAP/
  CRCR/DNCTRL store as 32-bit halves, PAGESIZE reads 1; empty PORTSC
  reads PP=1 only, wake bits store, an all-ones write fabricates no
  CCS/PED/change bits; interrupter-0 ERSTSZ/ERSTBA/ERDP/IMOD/IMAN store
  with IP quiet; doorbells are accepted and read 0 (engine is X2).
- **Guest bring-up replay**: the 11 measured init steps (section 1a)
  complete against the register file; the port scan reports 0 devices
  (the sticks bind in X4).
- **Presence is unconditional** (the UHCI-default pattern): measured
  against the full lane kit, `make test` rc=0 with every historical
  receipt byte-identical, so no omit flag exists. **IRQ 10** measured
  free (in use: 0 PIT, 1 KBC, 8 RTC, 11 e1000, 14/15 AHCI; EHCI/UHCI
  drive no PIC line).
- **NC**: `src/devices.c` wiring reverted → `test-xhci` red with 58
  failures (the class scan finds nothing). The RW1C *clear* path gets
  its vector in X5, when hardware can latch CSC (in X1 writes cannot
  set change bits, exactly as on silicon with empty ports).

## 1f. X2 measured receipts (command ring)

- **Engine discipline** pinned against the guest's consumer
  (xhci.c:824–1010): completion code at spec Status[31:24] (the guest's
  `control` field), TRB type at Control[15:10], slot id at
  Control[31:24]; ownership by the Cycle bit only. The guest writes
  ERDP|EHB back after every event ("QEMU tolerates omitting this; real
  silicon stops delivering events"), so an ERDP write advances the
  consumer mirror and its EHB bit clears IMAN.IP; ERDP reads back EHB
  while an event is unacknowledged and USBSTS.EINT mirrors IP. A full
  event ring drops (never overwrites) -- unreachable in the guest's
  submit->poll flow.
- **No Op round-trip**: the completion's Parameter echoes the command
  TRB's physical address with cc=1; **256/256 No Ops across the Link
  wrap** with exact params and producer cycles -- the guest's own U1
  gate made ours (`[xhci] command ring: PASS — 256/256 No Ops across a
  ring wrap` is its receipt string).
- **Slots**: Enable allocates the first free id, Disable frees, reuse
  takes the first free. Refusals use the spec codes (the guest branches
  only on cc==1): double-Disable = 11 (Slot Not Enabled), slot 0/33 =
  5 (TRB Error), CONFIG.MaxSlotsEn exhausted = 9 (No Slots Available).
- **Honest X2 edges**: X3 commands (Address Device) complete with cc=5
  and the ring keeps working; a TRB whose cycle the consumer does not
  own is left untouched until handed over; a doorbell while RUN=0
  moves nothing and the queued command lands on the next doorbell
  once running. The INTx *pin* is driven in X5; IMAN.IP/EINT already
  behave.
- NC: `src/xhci.c` reverted to X1 → 24 failures, all in the X2 groups.

## 1g. X3 measured receipts (contexts + EP0 control)

- **Address Device** (guest xhci.c:1124–1243): the guest programs
  DCBAA[slot] *before* the command and then gates on the OUTPUT Slot
  Context -- Slot State (dword3[31:27]) must read **Addressed (2)**,
  with the hardware address in dword3[7:0]. Input shapes pinned: ICC
  Add = A0|A1, Slot = route|speed<<20|entries<<27 + rootport<<16, EP0
  = CErr 3 | Type 4 | MPS<<16 | dequeue|DCS. Hardware address = slot
  id; BSR honoured (state Default). Address on an unenabled or
  disabled slot = cc 11.
- **One event per TD**: the guest takes the *first* Transfer Event
  matching slot+EP and parks later ones (xhci.c:1396–1420), so the
  engine completes the whole Setup->Data->Status TD and posts exactly
  one event (TRB pointer = Status TRB). Residue goes in the event
  Status dword[23:0] and is believed only on cc=13, so a short IN
  (every variable-length descriptor read) completes **cc 13 with
  residue = untransferred bytes**; exact transfers complete cc 1 --
  measured 256-ask on the 32-byte config descriptor. EP0 of the shared
  MSC stick (0525:a4a5, same descriptors as UHCI/EHCI); SET/GET
  CONFIGURATION, GET_STATUS, GET_MAX_LUN round-trip.
- **Stall discipline**: a refused request (unknown descriptor index)
  completes cc 6 **and halts EP0** -- queued TDs do not move -- until
  the guest's own recovery runs: Reset Endpoint (Halted->Running),
  then Set TR Dequeue to the producer enqueue point. Refusals: Reset
  of an unconfigured EP = cc 14 (Endpoint Not Enabled).
- **Evaluate does not move the consumer**: the guest's input EP
  context carries ring-base|producer-cycle in its dequeue field
  (xhci_update_max_packet0); only MaxPacket/EP Type are taken. The
  vector proves it by moving the consumer first, evaluating MPS
  64->32, and answering the next TD. Configure Endpoint copies
  Add-flagged contexts, drops Drop-flagged ones, and moves the output
  slot state to Configured (3).
- X3 boundary, stated: transfers run on EP0 only; a doorbell for
  bulk/interrupt endpoints is still inert (X4), and any addressed slot
  reaches the built-in device -- port presence (CCS) joins in X5.
- NC: `src/xhci.c` reverted to X2 → 39 failures.

## 1h. X4 measured receipts (bulk + interrupt)

- **Bulk shape** (guest xhci.c:1636–1727): a transfer is a Normal-TRB
  chain -- <= 64 KiB chunks, no 64 KiB boundary crossing, **CH on all
  but the last, IOC on the last only** -> exactly one Transfer Event
  per chain. Direction is the endpoint context type (2 OUT / 6 IN);
  Normal TRBs have no direction bit. Residue discipline identical to
  control (cc13-believed only). BOT runs as three such transfers:
  CBW (OUT 31), data phase, CSW (IN 13).
- **Shared stick**: `devices.c`'s BOT engine (`usb_serve`, U1–U5) got a
  public front door `usb_stick_serve(m, ctl, pid, total, buf)` with a
  third BOT state instance (EHCI/UHCI keep theirs); it is snapshot-
  tagged `dev.usb.bot.xhci`. Vectors drive the real thing: TUR
  without medium -> CSW FAILED, with medium -> OK; INQUIRY vendor
  "AURALITE"; READ CAPACITY 7+512; READ(10) of 2 sectors through a
  2-TRB CH chain (one event asserted), pattern exact; short READ at
  the image end -> cc13 residue 512 *and* CSW residue 512; malformed
  CBW signature -> CSW PHASE. The stick's limits are inherited as-is
  by design (no WRITE opcodes -- it answers FAILED like everywhere).
- **Interrupt**: the guest arms one Normal TRB and polls non-blockingly
  (xhci.c:1731–1868) -- no event means "nothing yet", and a zero-filled
  completion is exactly the forgery U2 deleted from the guest.  This
  machine has no HID device model on *any* bus (the keyboard is PS/2),
  so an interrupt TRB stays armed forever: no event, not consumed,
  other endpoints unaffected.  Vector pins that contract rather than
  inventing reports.
- NC: `src/xhci.c` reverted to X3 → 32 failures.

## 1i. X5 measured receipts (interrupter pin + ports) + close-out

- **INTx delivery**: the pin (IRQ 10, INTA#) is asserted while
  IMAN.IP & IMAN.IE & USBCMD.INTE hold, level-driven on change
  (pic_set_irq). The guest's handler acks IP through IMAN (RW1C)
  and relies on the pin dropping ("or the system will livelock on
  this IRQ") -- vectors pin both ack paths (IMAN RW1C and ERDP-EHB)
  plus both masks (IE=0 and INTE=0 leave IP latched but the pin
  down; re-enabling re-asserts). HCRST deasserts.
- **Ports**: the stick sits on root port 1 (the uhci_port_attached
  rule -- a medium present). PORTSC: CCS, SPEED=HS, PP (PPC=0);
  at the RUN 0->1 edge silicon-style CSC latches and a **Port Status
  Change event (34, Param[31:24] = port id)** is posted -- the
  guest's handler peeks exactly the ring head for it (its notes say
  QEMU never raises PCD, so the event is the real signal; we raise
  PCD too). Port Reset completes synchronously: PR never reads set,
  PED+PRC come up, a second PSC follows. The RW1C-clear vector
  deferred since X1 now latches and clears for real.
- **Determinism**: a full doorbell script replayed after HCRST
  produces a byte-identical event stream (vector).
- NC: `src/xhci.c` reverted to X4 → 21 failures.

### Close-out

X0–X5 complete (`patches/0076`–`0081`): the machine answers a class
0x0C/03/30 scan with a QEMU-parity controller (1b36:000d) whose full
register file, command ring, slot/endpoint contexts, control + bulk
+ interrupt rings, ports and INTx pin all execute synchronously and
deterministically, against the same MSC stick the UHCI/EHCI lanes
serve. 22 vector groups, six once-per-phase negative controls
(58/24/39/32/21 failures across X1–X5), every phase gated on
`make test` + `make test-sanitize` + both SeaBIOS ISO lanes. Guest
receipts (the guest's own kprintf gates) remain gated on AuraLite-OS
artifacts, stated since X0. Stated limits, inherited or chosen: no
MSI (the guest is INTx), one device on port 1 (no hubs), no HID
source on any bus (armed interrupt TRBs NAK, X4), isochronous TRBs
unconsumed, the stick's own limits (no WRITE opcodes).

## 2. Architecture decisions (X1)

- **PCI identity**: 1b36:000d, rev 01, class 0x0C/0x03/0x30, subsystem
  1af4:1100 (QEMU receipt parity).
- **Slot**: 0:14.0 -- the modern Intel xHCI slot; nothing lives there
  today, and dev/func scan order (used by `pci_find_class_after`) keeps
  every historical receipt intact.
- **BAR0**: 64-bit MMIO, 64 KiB window (the guest maps 64 KiB
  unconditionally). X1 pins a base that cannot collide with AHCI ABAR
  `0xFEB10000`, AHCI2 `0xFEB12000`, ECAM `0xB0000000`, LAPIC
  `0xFEE00000` (the kload/ACPI windows publish these).
- **Capability values** (ours; the guest reads them all dynamically):
  HCIVERSION 0x0100, CAPLENGTH 0x40; HCSPARAMS1 = MaxSlots 32 |
  MaxIntrs 1 << 8 | MaxPorts 4 << 24 (guest API bounds are 32/32; one
  interrupter is all the guest programs); HCSPARAMS2 = 0 (no
  scratchpads -- the guest skips step 6); HCCPARAMS1 = AC64 (0x1) --
  CSZ=0 (32-byte contexts), PPC=0 like QEMU (the guest powers ports
  unconditionally). DBOFF/RTSOFF chosen inside the window and pinned:
  ops at CAPLENGTH 0x40, ports at op+0x400 (stride 0x10), runtime at
  0x800 (interrupter 0 only), doorbells at 0x1000 (33 dwords).
- **Work model**: process rings **at doorbell write** (the AHCI
  "doorbell" precedent; the guest polls the event ring after ringing,
  so completion is deterministic at instruction level). The INTx line
  (cfg 0x3C/0x3D, one dedicated PIC line -- X1 measures the free one)
  asserts per IMAN.IP & IE & USBCMD.INTE, the e1000 N4 / AHCI S5
  pattern.
- **Ports**: 4 root ports in two protocol groups (2x USB2: FS/LS/HS,
  2x USB3: SS) with the PORTSC.SPEED field per group; the U1–U5 sticks
  (AURALUSB FS, AURALEHC HS) bind to the USB2 group in the X5 machine
  shape; SS ports stay empty until a SuperSpeed device model exists
  (honest emptiness, not a stub).
- **Determinism**: all xHCI state (slots, ring indices, port regs,
  IMAN/ERDP) is plain machine state and must ride snapshots byte-stably
  (the F4 x87-file lesson: no run-dependent bytes anywhere in the raw
  dump).

## 3. Definition of done (every phase)

1. Measured first: host probe or a guest-contract line ref in the
   receipts table above; nothing is claimed from assumption.
2. Fix + regression vector in the same patch; one once-per-phase
   negative control (revert the phase's core change, watch its vectors
   redden).
3. `make test` and `make test-sanitize` green; historical lanes
   byte-identical.
4. No guest-visible contract changes outside this plan's rows.
5. Docs (this file's status row + `docs/STATUS.md` + `CHANGELOG.md`) in
   the same commit; one `.patch` per phase under `patches/`.

## 4. Test gate

- `tests/test_xhci.c` (in `make test-unit` and `make test-sanitize`):
  X1 register-file vectors (identity, caps, HCRST/CNR, RW1C discipline,
  port power), X2 command-ring vectors (No Op cc=1, 256/256 wrap,
  Enable/Disable Slot), X3 context/control vectors, X4 bulk/interrupt
  vectors.
- Integration: a `tests/integration/` lane (X5) running the xHCI
  machine shape end to end with determinism x2 and the guest receipts
  above; SKIP-gated on the frozen AuraLite-OS KERNEL/INITRD artifacts
  like every other guest lane.
- NC per phase (once): `git revert`-style removal of the phase's core
  change reddens that phase's vectors.
