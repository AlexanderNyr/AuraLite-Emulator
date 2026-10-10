# USB Plan — honest USB host models for the emulator

> House style (AuraLite-OS/docs/plans): dependency-ordered phases, each with
> a definition of done and a test gate, shipping as **one `.patch` per
> phase** under `patches/`. Everything below is measured against the trees,
> not assumed. Baseline measured 2026-10-09 on
> `AuraLite-Emulator@fbbc8bd` with `AuraLite-OS@0ed0d29` (guest kernel.elf
> + initrd.tar built from that tree) and the sample firmware
> (`firmware/sample_firmware.nas`).

**Status: complete — U0 ✅, U1 ✅, U2 ✅, U3 ✅, U4 ✅, U5 ✅ (plan closed; VIDEO / NET / COMPAT follow in their own plans)**

| Phase | Scope | Status | Deliverable |
|---|---|---|---|
| U0 | Measured baseline + this plan | ✅ done | `patches/0036-USB-U0-usb-plan.patch` |
| U1 | BOT/CSW correctness: tag echo, residue, honest status byte (QEMU `test_ahci_matrix.sh`-style negative controls) | ✅ done | `patches/0037-USB-U1-bot-csw.patch` |
| U2 | SCSI command set the guest's MSC actually sends: INQUIRY, READ CAPACITY(10), TEST UNIT READY, REQUEST SENSE | ✅ done | `patches/0038-USB-U2-scsi-opcodes.patch` |
| U3 | EHCI register file honesty + doorbell on USBCMD only + control transfers (SETUP) so enumeration can bind | ✅ done | `patches/0039-USB-U3-ehci-usbcmd.patch` |
| U4 | UHCI controller model (I/O BAR4, frame list, TD/QH walk, control+bulk) — AuraLite-OS QEMU parity | ✅ done | `patches/0040-USB-U4-uhci.patch` |
| U5 | Guest end-to-end receipts (usbfs/MSC parity lanes), determinism ×2, sanitize, docs close-out | ✅ done | `patches/0041-USB-U5-usb-e2e.patch` |

Note: the *guest* has its own `USB_PLAN.md` (phases U0–U9, AuraLite-OS
repo) that closes the **driver** gaps. This plan closes the **emulator's
host-controller model** gaps so those drivers have honest hardware to talk
to. The guest's `docs/usb.md` claims table is the acceptance contract.

## 1. Measured baseline

### 1.1 What the machine presents today (`src/devices.c`)

A behavioural EHCI block, written for the sample firmware's fixed boot
recipe, not for a real driver:

- PCI 0:3.0, `8086:1E26`, class `0x0C/0x03/0x20`, BAR0 `0xFEB00000`
  (4 KiB `rw2` window) — the window is **scratch RAM**: capability
  registers are not modelled and read back zero.
- One doorbell hook (`ehci_write2`) fires `ehci_doorbell()` on **every**
  register write, not on USBCMD writes.
- `ehci_process_qtd()` walks at most 32 qTDs from `ASYNCLISTADDR`
  (0x18). One special case is implemented: pid=OUT, total=31, signature
  `'USBC'` — a Bulk-Only CBW; `READ(10)`/`READ(12)` set a pending
  LBA/blocks, every other opcode is logged
  `SCSI opcode 0x%02x (ignored by behavioural model)`. pid=IN serves
  image bytes (clipped to `disk_len` / RAM end) or, with no pending
  command, fabricates a CSW: `'USBS'`, **dCSWTag=0**, **residue=0**,
  **status=0** always. pid=SETUP (control) is ignored.
- Static single-LUN state (`s_lba/s_blocks/s_have`) persists across
  doorbells. No USBSTS/PORTSC/USBCMD semantics, no reset, no device
  presence, no interrupt line to the PIC/IOAPIC, no UHCI/OHCI/xHCI.
- The sample firmware contract (measured `firmware/sample_firmware.nas`):
  CBW with **dCBWTag=1**, READ(10) LBA=0 blocks=1, 512 bytes to
  `0x00100000`, CSW to `0x00020200`, then `USBCMD |= 0x21` (Run +
  Doorbell) and poll qTD Active clear. The boot-integration gate greps
  exactly `READ(10) LBA=0 blocks=1`, `bulk-IN 512 bytes -> guest
  0x00100000`, `synthesized CSW (status=OK)`.

### 1.2 What the guest expects (measured `AuraLite-OS@0ed0d29`)

- `drivers/usb/msc.c` (BOT): `dCBWTag = ++msc_tag` from `0x12345678`;
  the CSW is **validated** — signature, **`dCSWTag` (mismatch receipt:
  `CSW tag mismatch got 0x%x want 0x%x`)**, `dCSWDataResidue`,
  `bCSWStatus` (OK=0 / FAILED=1 / PHASE=2).
- SCSI commands the enumeration path actually sends (all five are in the
  request helpers): `TEST_UNIT_READY` (0x00), `REQUEST_SENSE` (0x03),
  `INQUIRY` (0x12), `READ_CAPACITY` (0x25), `READ_10` (0x28);
  `WRITE(10)` is exposed on the media path.
- `drivers/usb/ehci.c`: reads CAPLENGTH/HCIVERSION/HCSPARAMS/HCCPARAMS
  (receipt `HCI version %x.%02x, %d ports, PPC=%d, 64-bit=%d,
  companions=%d`), halt handshake via USBSTS.HCHALTED (`controller did
  not halt`), HCRESET with timeout, PERIODICLISTBASE/ASYNCLISTADDR/
  CTRLDSSEGMENT, USBCMD RS/HCRESET/PSE/ASE/ITC, PORTSC reset/speed/
  companion release (`%d high-speed device(s) ready`), async qTD with
  token timeout, control transfers (SETUP stage), periodic QH.
- `drivers/usb/uhci.c`: class probe (receipt `controller at PCI
  %u:%u.%u (0x%04x:0x%04x)`), **BAR4 must be I/O space**, frame list +
  TD chains (timeout/error receipts), USBSTS.HCHALTED start check.
- QEMU parity gates (what "AuraLite-OS parity" means): 
  `tools/run_qemu_usb_msc.sh` — `-usb` + `usb-storage` (a **UHCI**
  companion) with a 16 MiB stick (`'AURALUSB'` + MBR `55 AA`); 
  `tests/integration/cases/test_usbfs.sh` — `usb-storage` on
  `piix3-usb-uhci` with `qemu-xhci` present, asserting
  `[vfs] mounted '/usb'`, `[usbfs] device available at /usb`,
  `sector0.bin`, `disk.img`, `AuraLite usbfs`, `status: ready`,
  `sectors: 16384`, and an MSC hex read of the stick's magic.

### 1.3 Measured receipts on this machine (U0 baseline run)

Firmware lane (600k instr, `x86emu` default):

```
[usb-msc] READ(10) LBA=0 blocks=1
[usb-msc] bulk-IN 512 bytes -> guest 0x00100000 (from LBA 0)
[usb-msc] synthesized CSW (status=OK) -> guest 0x00020200
[guest] conventional entry reached at 0x00100000
```

Kernel lane (`--kernel=` + 12 MiB initrd, `AuraLite-OS@0ed0d29`):

```
[usbfs] ready: mount at /usb for hotplug USB mass storage
[uhci] no UHCI controller found
[uhci] self-test: no controller
[ohci] no OHCI controller found
[ohci] self-test: no controller
[ehci] controller at PCI 0:3.0
[ehci] HCI version 0.00, 0 ports, PPC=0, 64-bit=0, companions=0
[ehci] controller did not halt
[ehci] self-test: halted=0 async=0 periodic=0 — full support mode
[ehci] frame index: 0 -> 0 (delta=0)
[ehci] async qTD + periodic QH real; 0 interrupt endpoint(s) on the periodic schedule
[ehci] NOT IMPLEMENTED: iTD/siTD isochronous (USB_PLAN.md U9)
[ehci] PASS: 0 USB device(s) ready
[xhci] no xHCI controller found
[xhci] self-test: no controller
[usb] enumerating devices across all controllers...
[usb] 0 device(s) enumerated
[usb] SKIP: no USB devices attached
[msc] no enumerated USB mass storage device found
[msc] hint: attach usb-storage on UHCI/OHCI/EHCI/xHCI to test I/O
[msc] self-test: no ready mass storage device
```

### 1.4 Defect ledger (measured, drives U1–U5)

| # | Defect | Evidence | Phase |
|---|---|---|---|
| 1 | CSW `dCSWTag` always 0 — the guest validates it | `msc.c` tag check + our `wr32(buf+4, 0)` | U1 |
| 2 | CSW `dCSWDataResidue` always 0 (clip/short/absent transfers disagree) | `wr32(buf+8, 0)` vs `msc.c` residue field | U1 |
| 3 | `bCSWStatus` always OK — no FAILED/PHASE path | `wr32(buf+8, 0); /* status = good */` | U1 |
| 4 | INQUIRY / READ CAPACITY / TEST UNIT READY / REQUEST SENSE produce no data ("ignored") | `opcode == 0x28 \|\| 0xA8` only | U2 |
| 5 | Doorbell fires on every register write | `ehci_write2` wraps all of `rw2_write` | U3 |
| 6 | Capability registers read zero (`HCI version 0.00, 0 ports`) | baseline receipt | U3 |
| 7 | USBSTS.HCHALTED unmodelled (`controller did not halt`) | baseline receipt | U3 |
| 8 | No PORTSC / device presence — nothing to enumerate (`0 device(s)`) | baseline receipt | U3 |
| 9 | pid=SETUP ignored — control transfers impossible | `ehci_process_qtd` has no SETUP case | U3 |
| 10 | No UHCI model (the QEMU parity bus) | `[uhci] no UHCI controller found` | U4 |
| 11 | No guest-visible USB receipts end-to-end (usbfs/MSC lanes) | `[msc] SKIP` baseline | U5 |

## 2. Phases

### U0 — Baseline + this plan — ✅ done

**DoD:** the §1 receipts captured verbatim from frozen trees; defect
ledger measured against `src/devices.c` and the guest sources; phase
table with named deliverables.

### U1 — BOT/CSW correctness — ✅ done (patches/0037-USB-U1-bot-csw.patch)

Tag echo (CSW.dCSWTag = CBW.dCBWTag), residue = dCBWDataTransferLength −
bytes actually moved (0 for no-data commands), honest status byte: OK /
FAILED (command not supported) / PHASE (bad CBW signature), and a
malformed CBW cannot desynchronise the next one.  Status-phase framing:
the CSW is the 13-byte IN (measured: the sample firmware's status qTD
token is `0x000D0180`; BOT fixes the CSW at 13 bytes), so a data phase
cut short still settles residue honestly.

**Measured (all in `tests/test_usb.c`, 2 -> 7 vectors):**
- `ok csw tag echo` — READ(10) golden bytes + CSW carries the CBW tag
  (`0xA5A5A5A5`), residue 0, status OK.
- `ok csw residue short data` — expected 1024 with one 512-byte data
  qTD -> residue 512.
- `ok csw residue no image` — `disk_len = 0` -> 0 bytes moved, residue
  512.
- `ok csw failed opcode` — REPORT LUNS (0xA0) -> data phase moves
  nothing, CSW status FAILED, residue 36 (receipt: `SCSI opcode 0xa0
  failed (CSW status=FAILED)`).
- `ok csw phase bad cbw` — signature `'XXXX'` -> CSW status PHASE.
- `ok firmware shaped cbw` — negative control: the sample firmware's
  exact recipe (tag=1, READ(10) LBA 0, data `0x00100000`, CSW
  `0x00020200`) round-trips with status OK; the boot-integration gate
  strings cannot move (verified `make test` green).

Once-per-phase negative control (measured): reverting the tag echo
(`wr32(buf+4, 0)`) reddens `ok csw tag echo` exactly
(`Assertion 'r32r(c + 4) == tag' failed`, RC=134).

Teardown fix riding the same patch: `devices_done()` now calls
`pci_done()` (the PCI device list leaked 11232 bytes / 36 allocations
under LSan as soon as a test built a full device set — measured with
`make test-sanitize`).

**DoD (measured):** the six vectors above green; the firmware lane
receipts byte-stable; `make test` + `make test-sanitize` green (LSan
clean).

**Gate:** `make test` + `make test-sanitize` green.

### U2 — SCSI: INQUIRY / READ CAPACITY / TEST UNIT READY / REQUEST SENSE — ✅ done (patches/0038-USB-U2-scsi-opcodes.patch)

The four commands the guest's enumeration sends (measured shape from
`AuraLite-OS@0ed0d29` `drivers/usb/msc.c`: TUR x5 with REQUEST SENSE
retries, INQUIRY allocation 36 with vendor at +8 / product at +16,
READ CAPACITY(10) parsed as be32 last-LBA + be32 block length and
**rejected unless 512**) are answered from the attached image:

- INQUIRY: 36-byte direct-access (`0x00`), removable (`RMB 0x80`),
  SPC-3/RDF-2, vendor `AURALITE`, product `USB DISK`, revision `1.0 `
  (fixed model strings; the guest prints
  `[msc] INQUIRY: vendor '...' product '...'`).
- READ CAPACITY(10): last LBA = `disk_len/512 − 1`, block length 512
  (measured at both 1-block and N-block images).
- TEST UNIT READY: OK while an image is attached; FAILED + sense
  `2/3A` (NOT READY / MEDIUM NOT PRESENT) otherwise — the guest's
  retry loop then reads it back via REQUEST SENSE.
- REQUEST SENSE: 18-byte fixed-format (`0x70`, key/ASC/ASCQ, additional
  length `0x0A`), consumed on read; ILLEGAL REQUEST `5/20` for
  unsupported opcodes (the U1 FAILED path now saves sense).

**Measured (7 new vectors in `tests/test_usb.c`, 7 -> 14):**
`ok tur ready`, `ok tur no image failed` (negative control: FAILED,
not OK), `ok request sense not ready` (2/3A returned then consumed),
`ok inquiry response` (byte-compared 36-byte descriptor),
`ok read capacity n-block` (8 sectors -> last LBA 7), `ok read capacity
1-block` (last LBA 0), `ok illegal opcode sense` (5/20).

Once-per-phase negative control (measured): breaking the capacity math
(`disk_len/512` without the `− 1`) reddens exactly `ok read
capacity n-block` (assert `r[3] == 7`).

READ(10)/READ(12) keep the U1 channel; firmware receipts byte-stable
(`make test` boot gate green).

**DoD (measured):** the seven vectors green; capacity math pinned at
1- and N-block images; no-image TUR carries FAILED with sense; firmware
receipts unchanged; `make test` + `make test-sanitize` green.

**Gate:** `make test` + `make test-sanitize` green.

### U3 — EHCI register file + USBCMD-only doorbell + control transfers — ✅ done (patches/0039-USB-U3-ehci-usbcmd.patch)

Capability registers (CAPLENGTH, HCIVERSION 2.00, HCSPARAMS with N_PORTS
and companion routing, HCCPARAMS), USBSTS (HCHALTED/HSE/…), USBCMD
(RS/HCRESET/PSE/ASE/ITC) with the halt handshake the guest times out on
today, FRINDEX, CTRLDSSEGMENT, PERIODICLISTBASE, ASYNCLISTADDR, PORTSC
(connect status for the attached image, port power, reset sequence,
speed, enable, companion release) — the doorbell runs **on USBCMD
writes only**. SETUP qTDs perform real control transfers against a
minimal device model (device/config/string descriptors, SET_ADDRESS,
SET_CONFIGURATION, GET_MAX_LUN) so the guest can enumerate a stick.

Three window quirks are deliberate and measured against the sample
firmware's hand-tested dance (`firmware/sample_firmware.nas`) and the
guest driver's access shapes (`drivers/usb/ehci.c`, 1/2/4-byte only):

- **CAPLENGTH=0 shared BAR map** — the firmware's `usb_ff` re-derives
  opbase from `movzx rbx,byte [rax]` (the byte it just wrote), so the
  capability and operational maps must coincide at BAR0.
- **The 8-byte capability-root slurp answers 0 until the first USBCMD
  write.** The firmware slurps `mov rbx,[r15]` (QWORD at BAR0) and uses
  the low dword as a scratch pointer (`mov [rax],r15`); a HCIVERSION
  living at +0x02 would make that store target `0x02000000` and triple-
  fault (measured). After the first USBCMD write the same offset answers
  USBCMD for the firmware's QWORD read-modify-write dance.
- **+0x04 is HCSPARAMS until the first USBCMD write, USBSTS after.** The
  driver reads the caps once at init and only then starts the halt
  handshake (measured). PORTSC.CCS is gated `attached && PP && !OWNER`
  (an unpowered port has no device — keeps the firmware's
  `test eax,0x05` poll from spinning) and CSC latches on CCS change.

The async schedule advances two ways (real EHCI walks it while ASE=1):
the USBCMD doorbell walks immediately, and `devices_tick` (hooked into
the cpu instruction boundary every 256 instructions) samples the
schedule so the guest's RAM-polling `ehci_run_async` (no MMIO at all,
measured) makes progress. A qTD only executes when Active (0x80) is
set, so re-walks of a chain that already ran are idempotent.

**Measured (8 new vectors in `tests/test_usb.c`, 14 -> 22):**
`ok ehci caps` (CAPLENGTH 0, HCIVERSION 0x0200, HCSPARAMS `N_PORTS=1|
PPC`, HCCPARAMS 0), `ok ehci halt handshake` (HCHALTED at rest, cleared
by Run|ASE with ASS set, back after stop, HCRESET self-clears),
`ok doorbell usbcmd only`, `ok ehci portsc` (PP gating, CSC latch + W1C,
PR->PED+PEC, OWNER release), `ok control get device descriptor` (18
bytes, bcdUSB 2.00, maxpkt0 64, VID 0x0525 / PID 0xa4a5),
`ok control get config descriptor` (32 bytes, MSC/SCSI/BOT, bulk
0x81/0x02 at 512), `ok control string0` (`04 03 09 04`),
`ok control set address + max lun` (SET_ADDRESS + GET_MAX_LUN -> 0).

Once-per-phase negative control (measured): restoring the U2 defect
(doorbell on **every** register write) reddens exactly `ok doorbell
usbcmd only` (assert `token & 0x80` "still Active", RC=134).

**Guest receipts (measured, `AuraLite-OS@0ed0d29` kernel.elf +
initrd.tar, QEMU-parity 8 MiB stick, sector 0 `AURALUSB\x55\xAA`
(`il_make_disk` shape) = 16384 sectors):**

```
[ehci] HCI version 2.00, 1 ports, PPC=1, 64-bit=0, companions=0
[ehci] controller running, async+periodic schedules active
[ehci] port 0: high-speed device
[ehci] 1 high-speed device(s) ready
[ehci] PASS: 1 USB device(s) ready
[usb] device at addr 1: VID=0x0525 PID=0xa4a5 class=0x00 (Generic) maxpkt0=64 speed=high (480 Mbps)
[usb] device enumeration complete: addr=1 class=Mass Storage
[msc] mass storage candidate: addr=1 VID=0x0525 PID=0xa4a5 bulk_in=0x81 bulk_out=0x02 maxpkt=512
[msc] INQUIRY: vendor 'AURALITE' product 'USB DISK'
[msc] capacity: 16384 sectors, 512 bytes/sector (8192 KiB)
[msc] PASS: USB mass storage ready
[msc] sector 0 first bytes: 41 55 52 41 4c 55 53 42 ...
[msc] PASS: USB mass storage READ(10) works
```

**DoD (measured):** the eight vectors above green; guest receipts moved
from `HCI version 0.00, 0 ports` / `controller did not halt` to the
modelled caps and a passed halt handshake; `1 high-speed device(s)
ready` with the image attached; enumeration reaches (and passes) the
MSC candidate receipt with the `'AURALUSB'` stick; determinism pairs
byte-identical (firmware lane stdout+stderr; kernel boot log through
`[kernel] shell active`).

**Gate:** `make test` + `make test-sanitize` green.

### U4 — UHCI controller model (AuraLite-OS parity) — ✅ done (patches/0040-USB-U4-uhci.patch)

A real UHCI companion: PCI function with class `0x0C/0x03/0x00` (the
guest's probe prints vendor:device), I/O BAR4 register file (USBCMD/
USBSTS/USBINTR/SOFMOD/PORTSC1-2, frame number), frame list + TD/QH
walker with the guest's timeout/error semantics, control + bulk
transfers, interrupt transfer support (TD completion → USBSTS), and
the same device model / BOT channel as U3 so `usb-storage` on UHCI
reaches the guest's MSC exactly as in `run_qemu_usb_msc.sh`.

**DoD (measured):** `[uhci] controller at PCI …` + `I/O base = …` receipts;
the QEMU-parity stick enumerated over UHCI; `test_usbfs.sh`-parity
receipts (`[usbfs] device available at /usb`, `sector0.bin`, `disk.img`,
`AuraLite usbfs`, `status: ready`, `sectors: 16384`) reproduced guest-side
in a new harness lane; unit vectors for the register file + TD walk +
negative control (halted controller runs no frames).

**Gate:** `make test` + `make test-sanitize` green.

**U4 — UHCI root host controller (measured, complete)**

U4 is the legacy-companion root host: the same guest UHCI driver
(`AuraLite-OS/drivers/usb/uhci.c`) drives frames against the model, and the
stick appears as a second, full-speed device — same identity, 64-byte bulk
chunks — while U3's EHCI keeps its high-speed instance. Reusing the device
model made the transport the test subject; one register file is shared by
the U3 walker and the U4 UHCI walker (`usb_serve()`; the U3 qTD path is
byte-identical — its 22 vectors stayed green).

Model (measured from `drivers/usb/uhci.c` @0ed0d29):

- PCI 0:1.2, `8086:0x7020` (QEMU piix3 identity; the guest prints
  `[uhci] controller at PCI 0:1.2 (0x8086:0x7020)` + `I/O base = 0xc040`),
  I/O BAR4 at `0xC040`, 32 ports, class `0x0C/0x03/0x00` (matches the
  guest's class probe before its identity fallback).
- Register file per the guest's `uhci.h` bit map — 16-bit r/w + FLBASEADD
  dword: USBCMD (RUN0/HCRESET 1/GRESET 2/CF 6/MAXP 7), USBSTS (HCHALTED =
  bit 5 — the guest's halt-map), USBINTR, FRNUM (10-bit), FLBASEADD (phys
  of the 1024-entry frame list), SOFMOD, PORTSC1/2 (CCS/CSC/PR/PED/ECSC
  W1C/LS/LSDA). The stick is attached on port 0 (the QEMU recipe attaches
  one device; this model also exposes it on port 1), port 1 empty-wire
  model at 0x13 on reset.
- Frame/queue walker in the device cadence: USBCMD.RUN walks one frame per
  cadence tick (frame list + QH + TDs; terminate = QH-element 0x1 form;
  the guest's TD-wait timeouts are seconds of PIT time — the walker is far
  inside them). Completion contract: Active cleared, `ctrl[10:0] = bytes-1`
  (0x7FF = 0 transferred — the guest's interrupt-path length read),
  IOC + USBINTR.0 → USBSTS.USBINT; STALLED/CRC/NAK/BABBLE error bits
  modelled. SETUP/IN/OUT complete at whole-packet granularity; bulk/ctrl
  run in 64-byte UHCI chunks through a per-controller BOT state over the
  shared device model. USBCMD.RUN doorbell triggers an immediate frame;
  !RUN or HCRESET stops the walker (the QEMU self-test's failure mode is
  "controller did not halt" / frames over a halted controller).
- **Model bug found and fixed**: the disk-data path of `usb_serve()` did
  not advance the media offset by the consumed bytes — 64-byte UHCI chunks
  re-read LBA 0 forever (the 512-byte E32 chunks of U3 hid it).
  `disk_off = lba*512 + st->moved` — regression vector `ok uhci td walk`
  asserts the full 512-byte sector byte-for-byte across 8×64-byte IN
  chunks + CSW residue 0.

Measured receipts (guest boot lane, run 1) — QEMU
`tools/run_qemu_usb_msc.sh` parity:

```
[uhci] controller at PCI 0:1.2 (0x8086:0x7020)
[uhci] I/O base = 0xc040
[uhci] controller running, frame list at phys 0x3c68000
[uhci] port 0: device attached (full-speed)
[uhci] 1 device(s) ready
[uhci] self-test: frame counter 772 -> 677 (delta=65441)
[uhci] frame list active (controller running)
[uhci] port 0: CCS=1 PED=1 LSDA=0
[uhci] self-test: full support mode — CONTROL, BULK, INTR, ISOC — 1 dev ready — PASS
[uhci] PASS: 1 USB device(s) ready
[usb] device at addr 1: VID=0x0525 PID=0xa4a5 class=0x00 (Generic) maxpkt0=64 speed=full (12 Mbps)
[usb] device at addr 2: VID=0x0525 PID=0xa4a5 class=0x00 (Generic) maxpkt0=64 speed=high (480 Mbps)
[usb] addr 1: UHCI port 0, full (12 Mbps), class=Mass Storage VID=0x0525 PID=0xa4a5
[msc] mass storage candidate: addr=1 VID=0x0525 PID=0xa4a5 bulk_in=0x81 bulk_out=0x02 maxpkt=512
[msc] INQUIRY: vendor 'AURALITE' product 'USB DISK'
[msc] capacity: 16384 sectors, 512 bytes/sector (8192 KiB)
[msc] PASS: USB mass storage ready
[usbfs] device available at /usb (info, sector0.bin, disk.img)
[msc] sector 0 first bytes: 41 55 52 41 4c 55 53 42 00 00 00 00 00 00 00 00
[msc] PASS: USB mass storage READ(10) works
```

(`addr 1` is the UHCI instance and the MSC binds it; `addr 2` is the EHCI
instance of the same stick. The self-test frame delta prints as a uint16
wrap: the model's frame clock paces one frame per 256-instruction cadence
tick, so a 100k-nop window spans ~900 frames and the 10-bit counter wraps
— the semantics (nonzero = frames moving) hold; QEMU's real-time pacing
prints a small delta.)

Determinism: two full runs of the boot + harness lanes are byte-identical
(`cmp` clean; sha256 prefix `6f061aaebc26cc5e`, `krun/u4/kernel{1,2}.out`).

The harness lane (`--keys`/`--keys-at=prompt`, the same keystrokes
`test_usbfs.sh` types) reproduces the QEMU usbfs receipts through the UHCI
bulk path:

```
[vfs] mounted '/usb'
[usbfs] device available at /usb (info, sector0.bin, disk.img)
ls /usb
  info  (256 bytes)
  sector0.bin  (512 bytes)
  disk.img  (8388608 bytes)
cat /usb/info
AuraLite usbfs
status: ready
sectors: 16384
files: info sector0.bin disk.img
```

Unit vectors (`tests/test_usb.c`, 22 → 25): `ok uhci regs` (register
handshake — HCHALTED map, RUN/stop/HCRESET, PORTSC1 CCS/PR→PED|CSC|ECSC
W1C), `ok uhci td walk` (control SETUP→IN18→IN0 + BOT READ(10) CBW 31 →
8×64-byte IN chunks → CSW 13, byte-for-byte sector + residue 0),
`ok uhci halted runs no frames` (CF without RUN keeps TDs Active).
Negative control: reverting the RUN-gating reddens `tests/test_usb.c:556`
(`[uhci] halted: TD still Active`), abort RC=134. `make test` green
(25 usb vectors; full boot gate).

**Gate:** `make test` + `make test-sanitize` green.

### U5 — Guest end-to-end + hardening close-out

Both sticks (EHCI high-speed and UHCI companion) through the guest's
MSC path: `/usb` info + sector reads typed over the KBC, the
`test_ahci_matrix.sh` lanes kept green (USB is additive), determinism
×2 across the new lanes, sanitize lanes, `MATRIX_DETERMINISM`-style
byte compares, docs refresh (`ROADMAP.md` ledger row, `docs/STATUS.md`
evidence, CHANGELOG entries for U1–U5).

**DoD:** the U4 parity receipts + an EHCI lane with the same token
round-trip through `/usb`; two identical runs of each lane; no
historical receipt moves (firmware lane + AHCI lanes byte-stable).

**Gate:** all above + `make test` + `make test-sanitize` green.

**U5 — end-to-end + hardening close-out (measured, complete)**

- **`--no-usb-uhci`** (`machine_t.cfg_no_uhci`): omits the UHCI companion
  entirely (PCI 0:1.2 + its I/O window) — the machine shape of the
  guest's `test_usb_ehci.sh` QEMU lane (`-device usb-ehci`, no `-usb`).
  Unit vector `ok uhci omitted` pins the absence (open-bus I/O per the
  `io.c` float-high rule, no halt handshake after a USBCMD write, EHCI
  register file intact). The guest still probes: `[uhci] no UHCI
  controller found` / `self-test: no controller` — the same receipts the
  historical lanes carry.
- **Both sticks through the guest's MSC path with `/usb` info + sector
  reads typed over the KBC** (`run /apps/filesize /usb/sector0.bin` —
  the `test_ahci_large_read.sh` analog over BOT), each lane run twice
  sequentially with byte-identical serial logs:
  - **UHCI companion lane** (AURALUSB 8 MiB): `[usb] addr 1: UHCI port 0,
    full (12 Mbps)` (same stick also at `addr 2: EHCI port 0, high`),
    MSC binds the UHCI instance (`mass storage candidate: addr=1 …`),
    sector 0 `41 55 52 41 4c 55 53 42` (`AURALUSB`).
  - **EHCI high-speed lane** (AURALEHC 8 MiB, `test_usb_ehci.sh`
    parity): `[ehci] controller at PCI 0:3.0` + `HCI version 2.00, 1
    ports` + `port 0: high-speed device` + `1 high-speed device(s)
    ready`, `[usb] addr 1: EHCI port 0, high (480 Mbps)`, MSC binds the
    EHCI instance, sector 0 `41 55 52 41 4c 45 48 43` (`AURALEHC`), zero
    `Page Fault|kernel panic|[ehci].*FAIL|[msc].*FAIL`.
  - Both lanes type the same tokens over the KBC and reproduce the
    `test_usbfs.sh` receipts over their transport: `ls /usb` → `info /
    sector0.bin / disk.img`, `cat /usb/info` → `AuraLite usbfs`,
    `status: ready`, `sectors: 16384`, `FILESIZE /usb/sector0.bin 512`,
    `[usbfs] device available at /usb`, `[vfs] mounted '/usb'`.
- **`test_ahci_matrix.sh` kept green (USB is additive)** with
  `MATRIX_DETERMINISM=1` (S5 gate, 3.2G-instr lanes): lanes A/B/C/E all
  green and all four sequential pairs byte-identical
  (57400/57400/56363/29623 B) — `S4/S5 matrix + full determinism
  receipts: ALL PASS`.
- **No historical receipt moves (firmware lane + AHCI lanes):** the
  AHCI/filesystem receipt lines (`[ahci]`, `[fat32]`, `[diskfs]`,
  `[ext2]`, the typed token round-trips) of the fresh lane A are
  **byte-identical to the S5-era lane log**, line for line with matching
  multiplicities. Two measured caveats of any full-log cross-era compare
  (recorded, not assumed): (1) the guest's vmdrv PCI scan now prints the
  companion — `PCI 0:1.2 8086:7020 Intel PIIX3 UHCI driver=uhci
  status=active` + `summary: 7 PCI devices, 1 active/bootfb` vs the
  historical `6 PCI devices, 0 active/bootfb` — the intended U4
  PCI-topology receipt; (2) the S5-era logs carry the pre-U3 EHCI stub
  receipts (`HCI version 0.00, 0 ports, …` + `controller did not halt`),
  which U3's honest register file replaced by design, so full-log
  equality across the USB phase boundary is not the freeze line — the
  firmware-lane greps (`make test`) and the AHCI receipts are.
- **Frozen-kernel reproducibility (measured):** the guest banner embeds
  `__DATE__/__TIME__` (`build: Oct 8 2026 18:40:14` in the historical
  lanes); the U5 matrix lanes rebuilt that exact kernel with
  `SOURCE_DATE_EPOCH=1791484814` (banner + 3051512-byte size
  reproduced) and reconstructed the historical 92160-byte trimmed
  initrd (`bin/init` + `apps/filesize`, 2 files / 4 dirs,
  `tools/mkinitrd.sh` USTAR recipe) the S3–S5 lanes used.
- **Measured budget fact:** a full 8 MiB `/usb/disk.img` read through
  the UHCI 64-byte chunk discipline costs ~21G instructions (measured:
  3880/16384 sectors per 8G-instr budget) — outside a sane lane; media
  byte-correctness stays pinned by `ok uhci td walk` (512 bytes
  byte-for-byte across 8×64-byte chunks + CSW residue 0) and the typed
  `sector0.bin` read.

**Gate:** all above + `make test` + `make test-sanitize` green (26 usb
vectors).

## 3. Standing constraints

- The sample firmware's guest-visible contract is fixed: its boot recipe
  (CBW tag=1, READ(10) LBA 0, CSW at `0x00020200`, `USBCMD |= 0x21`)
  and its receipt strings never move — U1–U5 treat
  `tests/test_ahci_matrix.sh` / boot-integration greps as frozen.
- Guest-visible changes are additive: receipts that say "no UHCI
  controller found" today may gain a controller (U4) — the *string*
  stays valid when the option is off; the default run grows the new
  receipts, documented in the same patch.
- K0–K8 KERNEL-BOOT receipts are not touched.
- One `.patch` per phase under `patches/`, fix + regression vector in
  the same patch with a once-per-phase negative control.
