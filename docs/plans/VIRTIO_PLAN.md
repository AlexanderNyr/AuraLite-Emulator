# VIRTIO_PLAN — modern (1.x) virtio-pci block, then net

> Started from CPU_FEATURES_PLAN §5 rank 1. **V0 (this phase, docs-only):**
> a consumer census and a measured baseline; code starts at V1.

## §0 Status

| phase | contents | status |
|---|---|---|
| V0 | baseline: consumers, infrastructure, decisions D1–D7 | ✅ this patch (0105) |
| V1 | transport core `src/virtio.c` + unit harness | ✅ patch 0106 (§8) |
| V2 | virtio-blk: image, CLI/GUI, snapshot, vectors | ✅ patch 0107 (§9) |
| V3 | blk integration: SeaBIOS lane, determinism | ✅ patch 0108 (§10) |
| V4 | virtio-net: RX/TX, TAP backend, GUI, vectors | ✅ patch 0109 (§11) |
| V5 | MSI-X + blk FLUSH + close-out audit | ✅ patch 0110 (§12) — plan closed |

## §1 Rules (inherited)

1. **Advertise exactly what is implemented** (the CPU_FEATURES contract, now
   applied to virtio feature bits): a bit appears in `device_feature` in the same
   phase where its semantics are pinned by a vector. Initial offer sets:
   transport = `VERSION_1` only (no INDIRECT 28, EVENT_IDX 29,
   PACKED 34); blk = nothing; net = `MAC`. Not offering is spec-legal.
2. **Opt-in devices**: with no flags there are no PCI functions — all historical
   lanes (K, firmware, U5, SeaBIOS) keep byte-exact receipts (precedent:
   the e1000 without `--tap`).
3. **Synchronous completion on notify** — no background DMA threads;
   determinism and snapshots never see "the middle of a request" (AHCI precedent).
4. **GUI parity**: every CLI flag gains a GUI field in the same phase.
5. The snapshot layout never changes silently; new state goes through
   `*_snap` sections and `pci_snap` tags (config identity).

## §2 Baseline: consumers (measured on AuraLite-OS@da89be4)

### 2.1 virtio-blk — `drivers/virtio_blk/virtio_blk.c` (the kernel calls
`virtio_blk_init()` from `kernel.c:462`, after AHCI)

The contract the device must hold:

- probe: `1AF4:1042`, else `1AF4:1001`; the BAR is memory, only the low
  dword is read, base < 4 GiB; cap chain from status.bit4/`0x34`;
  cap types 1 (common) and 2 (notify + multiplier at +16) are mandatory.
- **Feature negotiation is NOT performed**: the driver never reads `device_feature`,
  never writes `driver_feature`, sets `FEATURES_OK|DRIVER_OK` blindly and
  never re-reads status. **Finding N1** (see §6).
- queue 0: `queue_size` is read, clamped to 256, written back;
  64-bit MMIO writes of `queue_desc/driver/device`; `queue_enable=1`.
- **capacity is read from PCI config `0x10/0x14` (BAR0/BAR1!)** —
  **finding N2**; besides printing there are no consumers (`grep`: 0 external
  calls of `virtio_blk_read/write/sector_count` — the x86 block stack does not go
  through virtio-blk yet; the visible proof is the init output lines).
- request: a 3-descriptor chain (hdr 16 B `{type,rsv,sector}`,
  data ≤ 8 sectors, status 1 B, `S_OK=0`); completion by polling
  `used->idx` (no interrupts awaited); notify writes the queue number (0).

### 2.2 virtio-net — `drivers/virtio_net/virtio_net.c` (net.c:1129,
fallback when the e1000 is absent and the stack comes up on it)

- probe `1AF4:1041`|`1AF4:1000`; caps 1/2/3/4 are all parsed.
- **Full negotiation**: requires `VERSION_1`, writes
  `VERSION_1|(MAC?)`, sets `FEATURES_OK` and **re-reads** —
  the device must keep the bit for a legal set.
- queues 0 (RX) / 1 (TX); `queue_size` is clamped to 16 and written
  back; `queue_enable` is re-read and verified.
- frame: a 12-byte `virtio_net_hdr` (with `num_buffers`, VERSION_1 form);
  RX — single 2048 B WRITE descriptors, prefilled after `DRIVER_OK`;
  `used.len` = hdr+frame; TX — one descriptor, polled completion.
- INTx: the line comes from cfg `0x3C`, the handler reads the ISR cap (ack), wakes
  the waiters; the MAC comes from device-cfg when `F_MAC` is offered.

### 2.3 Second consumer: SeaBIOS

`strings bios.bin` contains `init_virtio_blk`, `using modern (1.0)`
virtio mode, vendor-cap parsing — the lane firmware has the modern path;
it gives an independent e2e proof for blk without building the OS.

### 2.4 Out of scope: virtio-gpu (the guest has a driver, `kernel.c:640`) —
scanout semantics needed, a separate plan; virtio-mmio/vpci are other
architectures, not x86.

## §3 Infrastructure audit (AuraLite-Emulator@2e586c0)

- **PCI** (`src/pci.c`, 185 lines): `cfg[256]` + per-device `cfg_read/write`
  hooks; no caps machinery exists or is needed — the device places the
  vendor caps in its own `cfg[]` (precedent: the XHCI MSI-X cap at `0x90`).
  ECAM exists; `pci_snap` saves configs by BDF tags (refusal on
  a device-list mismatch — snapshot identity for free).
- **BARs**: pre-assigned in `devices.c`, guest relocation follows the
  `ahci_set_abar` + "probe latch" pattern (an all-ones write → size mask,
  `ahci.c:670/689`). Free BDF slots: taken are 00.0, 01.0, 01.2,
  02.0, 03.0, 20.0, 31.0/2/3 → virtio: `04.0` (blk), `05.0` (net).
- **MMIO/DMA**: `mem.c:228/287` — accesses reach handlers at their full
  size (8-byte `queue_desc` writes are OK); the device's reads of guest rings
  go through `mem_read/write` as in AHCI/XHCI.
- **Interrupts**: `pic_set_irq(m, line, level)` (fan-out into the I/O APIC
  inside, CHIPSET H7); lines taken: AHCI 14/15 (`AHCI_IRQ0=14`),
  XHCI 10; a free line to be measured in V1 (candidates 11/5/7).
  MSI/MSI-X infrastructure exists (`msi_deliver`, the XHCI table) — V5.
- **Disks**: `machine_t.sata_img[ctrl][port]` — the whole file in RAM
  (`main.c:24-26` loader), snapshots serialize it (`ahci_snap`);
  writethrough ⇒ snapshot refusal (`snap_cfg_t.sata_wt`).
- **Network**: the e1000 is opt-in via `--tap` → UDP datagrams = frames, endpoint
  `127.0.0.1:8080` (responder `tools/tap_responder.py`); RX polling —
  `e1000_tick()` from `devices.c:579`; injection C-API `e1000_rx_frame`,
  counters `e1000_tx_stats` — vnet mirrors both; TAP ⇒ snapshot refusal
  (`snap_cfg_t.tap`).
- **Tests**: device-model unit lanes `tests/test_xhci.c`,
  `tests/test_net.c`, `tests/test_ahci.c` — the pattern for `test_virtio.c`.

## §4 Decisions

**D1 — layout.** Modern-only functions `1AF4:1042`/`1AF4:1041`, revision
1. One 32-bit memory BAR0 (16 KiB): common `+0x0000`, ISR `+0x1000`,
device-cfg `+0x2000`, notify `+0x3000`, multiplier 4 — offsets as in
QEMU (`hw/virtio/virtio-pci.c:2227-2242`, measured at HEAD);
deviation from QEMU only in the BAR index (QEMU uses 64-bit BAR4): the caps are
self-describing, both consumers read the index from the cap (verified
in the guest code and SeaBIOS strings). BAR pre-assignment + relocation (AHCI).

**D2 — the ring.** Split virtqueue, one shared core: chain walking with hard
bounds (address+length inside RAM, chain length ≤ `queue_size`),
broken chains — an error in the device's protocol (blk `S_IOERR`, net —
a drop) + logging of the first N. Completion is synchronous in the notify handler;
an interrupt fires when `used.idx` grew and `avail.flags.NO_INTERRUPT`
is clear (without EVENT_IDX that is the whole rule).

**D3 — status/features.** `driver_feature` writes are masked to the offer
(QEMU precedent). `FEATURES_OK` is kept ⇔ `VERSION_1` was acked
(otherwise the bit is cleared in the readback — the guest's net driver
checks this). The queue engine works under
`DRIVER_OK && queue_enable` **regardless** of FEATURES_OK's fate —
otherwise the guest's blind blk driver (§2.1, finding N1) would not work;
QEMU has the same leniency in practice (`virtio_validate_features` rejects
only the IOMMU_PLATFORM mismatch and device-specific combos,
measured at `hw/virtio/virtio.c:2292+`). The deviation from a strict
reading of the spec is deliberate and pinned by vectors on both sides.
Reset (`status=0`) is synchronous and full; `config_generation` does not
change after attach (there are no config-change events yet).

**D4 — interrupts.** INTx only until V5: `msix_config`/
`queue_msix_vector` = `0xFFFF`, no MSI-X cap. ISR byte:
bit 0 = queue, bit 1 = config; a read clears it; the INTx line mirrors
`isr != 0` (level, `pic_set_irq` on change).

**D5 — blk.** Offer: `VERSION_1` only; device-cfg = `capacity`
(512-byte sectors) per the uapi `virtio_blk.h`. Types IN=0/OUT=1 (general
gather/scatter over segments), out of range → `S_IOERR(1)`,
unknown type → `S_UNSUPP(2)`. Image: a RAM copy of the file (like
`sata_img`), `--virtio-blk=path` + GUI; snapshot — full support
(an image section like AHCI's); writethrough — not in V2.

**D6 — net.** Offer: `VERSION_1|MAC`; device-cfg = `mac[6]`; hdr
12 B, `num_buffers=1` on RX; a frame longer than the buffer chain — a drop +
counter (no multi-buffer promised without MRG_RXBUF). Backend: the same TAP
UDP endpoint; `--virtio-net[=host:port]` (bare = `127.0.0.1:8080`);
a simultaneous `--tap` (e1000) is refused at startup — one owner of
the endpoint; `virtio_net_tick()` next to `e1000_tick`; snapshot refusal
through `snap_cfg_t.tap`.

**D7 — determinism.** Opt-in ⇒ zero receipt drift; synchronicity ⇒
a snapshot between notifies cannot land "inside" a request; core state
(common-cfg registers, queue addresses/sizes, `last_avail_idx`,
ISR, the blk image) goes into snapshots from V2; the rings live in guest RAM.

## §5 Phases (each: code + vectors + NC + gates + docs + patch)

- **V1 core.** `src/virtio.h/c`: transport state, filling the cfg space with caps,
  common-cfg/ISR/notify MMIO, feature banks,
  the D3 FEATURES_OK gate, reset, the ring engine, INTx. `tests/test_virtio.c`
  with a fake sink device (`test_xhci.c` shape). Measure
  a free IRQ line. The new-module NC caveat (X1 precedent):
  the vectors live with the API; the parent is "no symbols".
- **V2 blk.** The device on the core: IN/OUT/UNSUPPORTED statuses, the RAM
  image, `--virtio-blk=`, a GUI field, `virtio_snap` (transport+image),
  guest-shaped vectors (including the blind init without negotiation).
- **V3 blk integration.** A SeaBIOS lane (the firmware probes modern virtio-blk
  — the check is in the lane's boot logo), a mid-run snapshot lane,
  docs (may merge into V2 if the volume is small).
- **V4 net.** The device: RX/TX on the core, MAC, the TAP backend per D6,
  `virtio_net_rx_frame`+statistics for lanes, GUI, guest-shaped vectors
  (prefill, hdr-12, recycle, IRQ-wake via ISR-read).
- **V5 MSI-X + close-out.** An MSI-X cap + table in the BAR page for
  both functions (`msix_config`/`queue_msix_vector` live), blk
  `FLUSH(9)` with pinned semantics, a final inventory of the offer
  against the vectors, plan closure.

Deliberately deferred: indirect/EVENT_IDX/PACKED (no consumer),
MQ/ctrl queue, virtio-gpu, >1 blk disks, writethrough for vblk.

## §6 Findings register (guest side — not ours to fix, but record them)

- **N1**: the guest's `virtio_blk_init` performs no feature negotiation and never
  checks FEATURES_OK — against a strictly-spec device it would go
  live in a "rejected" state. Our device stays compatible
  via D3; the guest should fix this one day (as in its net driver).
- **N2**: blk capacity is read from PCI `0x10/0x14` (the BAR0/BAR1 values)
  instead of the device-cfg cap — the printout will show garbage on any correct
  device, ours included. We implement device-cfg capacity honestly.

## §7 Gates for every phase

`make test` (full, with `AURALITE_BIOS`) + `make test-sanitize` +
a negative control (the new vectors are red on the parent) +
a parent-check of the patch in `patches/` + a sync of
STATUS/ROADMAP/CHANGELOG in the same commit. Zero receipt drift in the
historical lanes is part of acceptance (D7/rule 2).

## §8 V1 addendum (patch 0106): the transport core

`src/virtio.h`/`src/virtio.c` (core without devices; blk is V2) and
`tests/test_virtio.c` with a fake echo device at the exact
coordinates of the future blk. Implemented per D1–D4:

- **PCI**: a modern-only function, revision 1, subsystem = device id;
  vendor caps in `cfg[0x40..]` (common@`0x40`, notify@`0x50` with
  multiplier 4, isr@`0x64`, device@`0x74` — only when a config exists),
  re-asserted after every cfg write (XHCI pattern);
  32-bit 16 KiB BAR0: an all-ones probe → `0xFFFFC000`, relocation
  rebinds the window (the `ahci_set_abar` pattern); BAR1–5/ROM = 0.
- **Common-cfg** — an image-based `0x40` B register file: any
  access size composes, semantics apply only to the touched
  fields (selects → features → queues → status). Feature banks,
  the offer mask (D3), `queue_size`: reading before any write = max,
  a write >max or 0 → max, a smaller one sticks; `msix_config`/
  `queue_msix_vector` = `0xFFFF`; an invalid `queue_select` reads
  zeros and harms no queue.
- **The D3 FEATURES_OK gate** is pinned discriminatingly: the vector
  "device bit acked (validator happy), VERSION_1 not" goes red
  when exactly the VERSION_1 check is removed (mutation NC);
  the inverse vector pins the validate branch. The guest's blind init (§2.1)
  passes through a FEATURES_OK rejected in the readback — the status check
  after the blind write sequence = `ACK|DRIVER|DRIVER_OK`,
  and the transfer still completes.
- **Rings**: pop/complete with hard D2 bounds (rings and
  buffers strictly inside RAM; the DMA helpers refuse a target outside RAM
  wholesale, including the device's own MMIO). Broken chains:
  a head outside the table → drop with no used; loop/spill/indirect →
  completion with len 0 + a log line (budget 8 lines); the queue keeps
  working after garbage. Interrupt POLICY: ISR bit + INTx on
  completion if `avail.flags.NO_INTERRUPT` is clear; an ISR read
  clears the byte and the line.
- **Reset** is synchronous: status reads 0 immediately, queues/features/
  addresses wiped, `queue_size` back to max, device-reset callback.

Phase measurements that correct §3:

- **BDF**: `0:04.0` is taken conditionally — the e1000 grabs it under `--tap`
  (`e1000.c` pci 0,4,0), §3 missed that. Result: **blk 0:05.0,
  net 0:06.0**; BAR bases `0xFEB30000`/`0xFEB34000` (free behind
  XHCI `0xFEB20000`+64 KiB).
- **IRQ**: census of `pic_set_irq` consumers — taken are 0 (PIT),
  1 (KBC), 8 (RTC), 9 (SCI), 10 (XHCI), 11 (e1000), 14/15 (AHCI);
  serial/mouse have no lines. Result: **blk IRQ5, net IRQ7**
  (`cfg[0x3C]`, INTA# pin).
- `pci_dev.ctx` is taken by `machine_t*` (host bridge), so the core
  binds the PCI function to `virtio_dev` via a pointer registry.

NC: on the parent (tree 0105) the test does not build (no `virtio.h`)
— the new-module form per §5; the mutation NC above sits on top.
Gates: full `make test` (ISO lanes genuinely run with NASM in
PATH) and `make test-sanitize` — rc 0, 0 findings, test-virtio
green under ASan/UBSan; no historical drift (the device is connected
to no lane: there are no functions in the machine yet).

## §9 V2 addendum (patch 0107): virtio-blk

`src/virtio_blk.c/h` on the V1 core + `virtio_snap` in the core (transport
state) + vectors in `tests/test_virtio_blk.c` (7 vectors).

- **Device (D5):** `1AF4:1042` class 01/80 at `0:05.0`,
  queue max 128, device-cfg = exactly `capacity`
  (`img_len/512`, the file tail ignored). The image is a RAM copy
  (`--virtio-blk=path`, attach from main after `devices_init`,
  an unreadable file = fatal exit 1 as with `--tap`). IN/OUT requests with
  general gather/scatter over the streams (per-sector chunks through
  the core's stream helpers); the status byte = **the last byte of
  the in-stream** (the guest's and SeaBIOS's shapes converge in one place);
  `used_len` = what was actually written to in (IN: data+1, OUT/errors: 1).
  Range/length not a multiple of 512 → `S_IOERR`; FLUSH/GET_ID/
  anything off the offer → `S_UNSUPP` (FLUSH only in V5, once its
  semantics are pinned, no fake OK). A truncated header
  (<16 B in out) completes with `S_IOERR` if there is at least one in byte.
- **GUI parity:** the `virtio_blk` path key in the `tools/launcher.py`
  SCHEMA + the "VirtIO disk (image, 0:05.0)" row in the expert
  storage tab of `gui_app.py`; `test-gui` green.
- **Snapshot:** `virtio_blk_snap` is called from `snapshot.c::visit`
  next to `ahci_snap` (before the MMIO-map comparison: BAR rebind +
  INTx re-assertion from the restored ISR inside
  `virtio_snap`). The section is *present-only*: without the flag the record stream
  is byte-identical to history; blk↔no-blk crossings are refused by the
  `pci_snap` identity; the image — `snap_sparse` "disk.vblk"
  (the record length refuses a differently sized file at load).
  The round-trip is pinned by a vector: after saving, the image is trampled
  through the device; after loading, the saved state is what reads back
  and requests continue over the restored rings.
- **Linking:** `snapshot.c` pulls in `virtio_blk_snap`, so
  `virtio.c`+`virtio_blk.c` were added to the 4 link lines with
  snapshot.c (fpu-gate/xhci/virtio/snap) — the `ahci_snap` precedent.

NC: the parent has no `virtio_blk_test_add` (new-module form);
the `S_UNSUPP→S_OK` mutation burns exactly the FLUSH vector. Gates:
full `make test` + sanitize rc 0, 0 findings; the smoke
`x86emu --virtio-blk=disk/disk.img` raises the function and does not
break the firmware boot (the device's SeaBIOS lane is V3, as in §5).

## §10 V3 addendum (patch 0108): SeaBIOS lane + a snapshot defect

`tests/test_virtio_seabios.py` (+ `make test-virtio-seabios`, in
`make test`): stock SeaBIOS — a second, independent consumer of the
transport — boots an MBR stub from an image attached **only**
via `--virtio-blk` (the USB stick gets a zero non-bootable image:
the marker can only arrive through the queue). From the very first run:
the firmware parses the caps itself, relocates the BAR in its own
pciinit, negotiates and reads LBA0 → `AURA_ISO_BOOT_OK`.

The lane caught a **pre-existing snapshot defect, not a virtio one**:
loading any snapshot taken after SeaBIOS had
relocated the XHCI BAR0 was refused with "MMIO map differs ...
(XHCI BAR0)" — reproduced on the plain `--sata` recipe.
Cause: the snapshot driver compares the MMIO map before
`devices_snap`, while the XHCI rebind lives inside `devices_snap`
(`xhci_snap`); AHCI never had the problem precisely because its
rebind sits before the comparison. The fix (file format untouched,
old snapshots load — measured on a "pre-fix" snapshot):
`xhci_rebind()` replays the base from the cfg bytes
restored by `pci_snap`, next to `lapic_sync_mmio`;
`xhci_snap` later applies the same base from the serialized
controller.

Lane legs: V boot; S save-mid-SeaBIOS
(60M budget — the marker not yet printed, verified) → load
→ marker + **the final `[stats]` byte-identical to the uninterrupted
run** (`executed_steps=120334760, instr_count=1288199080`
in both: machine determinism survives save/load with blk);
C control: the same pair on `--sata` (pins the rebind fix itself).
NC on the 0107 tree: V green, S/S-determinism/C red —
discrimination exactly on the fix. The lane is not in sanitize
(SeaBIOS-under-ASan is covered by test-iso; blk's snapshot serialization —
by a unit vector under ASan). Gates: `make test` + sanitize rc 0,
0 findings.

## §11 V4 shipped (patch 0109)

`src/virtio_net.c/.h` — modern virtio-net on the shared core:
`1af4:1041` @0:06.0, BAR0 `0xFEB34000`, IRQ7 (V1 placement);
the offer is only `VIRTIO_NET_F_MAC` (the guest acks
`VERSION_1|MAC` and demands FEATURES_OK — a contrast with N1),
device-config is exactly 6 bytes `52:54:00:12:34:56` (the guest reads it
as a u64 through the function — extra bytes are forbidden with the
STATUS bit off), queues RX=0/TX=1 with offered max 128 (the guest
programs 16, the DMA pointers round-trip byte-exactly through the core).
Backend — the same TAP/UDP as the e1000's (D6): CLI
`--virtio-net[=host:port]`, default `127.0.0.1:8080`;
`--tap`+`--virtio-net` are mutually exclusive (one endpoint).
RX consumes buffers only on ingress (the doorbell only
advances last_avail: the guest's posted prefill is never "completed" by
kicks), the 12-byte header is all zeros except
`num_buffers=1`, `used.len = 12+payload`, no-queue/no-buffer —
honest drop counter, oversize is truncated per the e1000
precedent. TX gathers the chain, strips the 12-byte header,
used len is always 0; without a socket (unit mode) — the drop counter;
`virtio_net_stats()` — rx/tx/dropped for the lanes. Snapshots with
a TAP-attached NIC are refused, as with `--tap` (the message
was generalised in main/snapshot — `snap_cfg.tap` covers both
forms; without flags the stream is byte-for-byte historical). Vectors in
`tests/test_virtio_net.c` — guest shape: identity/MAC,
by-the-book negotiation (FEATURES_OK kept —
MAC+VERSION_1 accepted, a broken attempt drops it), prefill and
recycle, RX frame+IRQ7/ISR-ack, starve→drop, truncate with
canaries past in-len, TX single/split gather, a short
chain bounded, ingress before DRIVER_OK — drop. NC: the parent
without `src/virtio_net.h` — compile-fail (the new-module
caveat); the `num_buffers=1→0` mutation burns exactly the
hdr assertion. GUI: the `virtio_net` schema + the
"VirtIO net TAP" row. Gates: `make test` rc 0 (SeaBIOS/ISO
lanes not skipped), sanitize rc 0, 0 findings. Remaining is V5:
MSI-X + blk FLUSH + close-out.

## §12 V5 shipped (patch 0110) — plan closed

**MSI-X in the core** (`src/virtio.c`), MSI M1/M3 shape: the capability
`0x11` in cfg at `0x84` terminates the chain after the vendor caps (RO
defended by the same re-seed on every write), the table in the tail of the
notify page of BAR0 (`+0x3800`, 16 B entries), the PBA at `+0x3C00`.
Table size is `queues+1` (blk 2, net 3); the notify BAR was not
grown, all window addresses unchanged, the SeaBIOS lane green without
edits. `msix_config`/`queue_msix_vector` are live: a write past
the table reads back NO_VECTOR (the QEMU rule
Linux leans on when falling back). The route is decided at raise
time: Enable=0 — INTx off the ISR byte (the byte
latches always, QEMU shape); Enable=1 — the pin is silent (PCI),
the source (0=config, 1+i=queue i) raises its own
pending flag — the MSI_PLAN §2 "the source keeps its pending state" rule
generalised from the XHCI's single level to per-source flags, because
virtio completions are edges; the PBA shows these flags
through the assigned vectors, unmasking (entry unmask /
function-mask clear / enable edge / snapshot load)
releases the message through `msi_deliver`. A device reset
re-arms the assignments; the table and Enable are PCI state and
survive a reset; the table starts masked at power-on. NO_VECTOR under
Enable=1: there is no signal — honestly, and the guest has nothing to grab the
pin with. The whole plane rides in the serialized `virtio_dev_t` (the
xhci_snap precedent: the struct grew, so pre-V5 `--virtio-blk`
snapshots are refused with the honest "different build" line —
the flagless stream is byte-for-byte as before); the blk snapshot vector
proves: after a load the completion arrives on the
restored vector.

**blk FLUSH:** the `VIRTIO_BLK_F_FLUSH` offer (bit 9), request
type 4 (verified against the Linux uapi; the plan's "FLUSH(9)" in §5 was
the bit) completes with `S_OK`, used 1, the sector ignored.
Honesty comes from the synchronous machine: the write's bytes are already in the
RAM image before its completion is visible, so the barrier does not
promise more than the medium holds. GET_ID/DISCARD/
WRITE_ZEROES stay off the offer and keep getting S_UNSUPP.

NC: the `0109` parent + the new suites are red exactly on the V5 sur-
face (cap chain, FLUSH, deliveries, snapshot vector);
mutations: dropping the INTx gate burns exactly the 2 retired asserts,
dropping the unmask scan — a cascade of pending asserts, withdrawing the
FLUSH bit — identity. Final inventory of the offer
against the vectors: transport `VERSION_1` (V1 vectors), blk
`FLUSH` (v_flush + identity), net `MAC` (v_identity) — every
advertised bit has a vector, no unadvertised type ever
completes successfully. The MSI-X plane is pinned in three
suites: core fake (delivery/masks/PBA/config vector/
NO_VECTOR/INTx hand-over), blk (snapshot), net (RX/TX vectors,
IRQ7 retired). Gates: `make test` rc 0 (SeaBIOS/ISO not
skipped), sanitize rc 0, 0 findings.

**Plan total (V0–V5, `0105`–`0110`):** the two measured guests and
SeaBIOS ride modern virtio-pci (D1–D7 untouched by a single edit);
deferred out of scope (§5): indirect/EVENT_IDX/PACKED, MQ/
ctrl queue, virtio-gpu, >1 blk disks, writethrough vblk,
`queue_notif_config_data`/queue_reset (there are no feature bits
behind them). Guest findings N1/N2 (§6) remain open on the guest's side
— the device stays compatible with them via D3.
