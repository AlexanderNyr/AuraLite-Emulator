# MSI Plan — message-signalled interrupts (MSI / MSI-X)

> Audit 2026-10-19 candidate #2 (after XHCI, now complete). Companion to
> `ROADMAP.md` §"Next-block candidates" and to `docs/plans/XHCI_PLAN.md`
> (the xHCI INTx delivery this plan generalises).

**Status: M0–M4 done ✅ COMPLETE**

| Phase | Scope | Status | Deliverable |
|---|---|---|---|
| M0 | Measured baseline: who consumes MSI, the QEMU capability shapes, our PCI/LAPIC mechanics, the defect ledger | ✅ done | this file (`patches/0082`) |
| M1 | PCI MSI-X capability on the xHCI (QEMU-parity shape) + capability-list plumbing (Status.Cap, cfg 0x34) + table/PBA MMIO in BAR0 | ✅ done | `tests/test_xhci.c` `v_msix_cap` (23 groups) (`patches/0083`) |
| M2 | LAPIC MSI message delivery: address/data decode -> `lapic_set_irr_dest`, edge semantics, delivery-mode honesty | ✅ done | `tests/test_msi.c` (4 groups, new `test-msi` lane) (`patches/0084`) |
| M3 | Per-device routing: xHCI picks MSI-X vs INTx by Enable/mask state; AHCI gains an MSI capability + routing; INTx fallback byte-identical | ✅ done | `v_msix_route` (test-xhci) + `test_msi_line` (test_ahci) (`patches/0085`) |
| M4 | Close-out: snapshots, determinism, docs, ledger | ✅ done | `v_msix_snapshot` + `v_determinism` (`patches/0086`) |

Each phase ships one `.patch` under `patches/` with measured receipts, a
regression vector, one once-per-phase negative control, green gates and
the docs update in the same commit (the `ROADMAP.md` standing rules).

## 1. Measured baseline (M0)

### 1a. The consumer side

- **AuraLite-OS (the north star) does not use MSI at all** (measured
  2026-10-20): its PCI API is 119 lines (`drivers/pci/pci.c`) offering
  class scan, BAR read and the INTx line only; `msi` appears nowhere in
  `drivers/pci/pci.{c,h}`, `kernel/arch/x86_64/irq.c` or
  `kernel/arch/x86_64/ioapic.c`. Its interrupt world is legacy INTx
  routed through the IOAPIC redirection table built from MADT source
  overrides (bus source -> GSI). So the north star needs nothing here
  today; the audience is every *other* guest (Linux/Windows on the
  SeaBIOS lanes), for whom xHCI/AHCI without message interrupts means
  shared-INTx degradation -- and QEMU parity, which XHCI_PLAN adopted
  as a north-star-adjacent rule.
- `tests/` fixtures and lanes drive devices through PIC/IOAPIC paths
  today, so all historical lanes must stay byte-identical behind the
  INTx fallback (M3's core invariant).

### 1b. The QEMU parity shapes

- **qemu-xhci `1b36:000d`** (the identity our XHCI wears) exposes
  **MSI-X only, no MSI** (Red Hat bug 1451189: "nec-usb-xhci supports
  both MSI and MSI-X, qemu-xhci only the (newer) MSI-X interrupts").
  `lspci` shape (bug 1373184): `Capabilities: [90] MSI-X: Enable+
  Count=16 Masked-`, `Vector table: BAR=0 offset=00003000`,
  `PBA: BAR=0 offset=00003800`. Our BAR0 is 64 KiB and both offsets are
  free (doorbells end at 0x1080), so the parity shape drops in as-is.
- **ich9-ahci** (our AHCI's QEMU counterpart): MSI-capable in QEMU
  (`msi` property); the exact cap shape is an M3 measurement.
- **e1000 82540EM**: no MSI on the hardware or in QEMU -- parity says
  leave it alone (recorded so nobody "fixes" it later).

### 1c. Our machine today (measured)

- **Zero PCI capabilities anywhere**: `cfg[0x34]` is 0 and Status bit 4
  (Capabilities List) is clear on every function (`grep` over
  `src/pci.c`, `src/devices.c`). The config machinery itself is ready:
  `cfg[256]` per device with per-device `cfg_read`/`cfg_write` hooks
  (AHCI, XHCI already use them for BAR probe semantics).
- **No MSI mechanism**: `grep MSI|0xFEE` finds only the LAPIC window
  itself (`LAPIC_BASE 0xFEE00000`, MADT). A would-be MSI write decodes
  as an ordinary LAPIC register access today.
- **Vector delivery already exists**: the IOAPIC lands interrupts via
  `lapic_set_irr_dest(m, dest, vec)` (`src/ioapic.c`), so M2 is a decode
  layer in front of a proven sink, not new plumbing. Devices drive
  level INTx through `pic_set_irq` on change (AHCI S5 / XHCI X5
  convention) -- the per-device place where M3 chooses the path.

### 1d. Defect ledger

| # | Defect (measured absence) | Consequence | Phase |
|---|---------------------------|-------------|-------|
| D1 | no MSI/MSI-X capability on any function | modern-guest xHCI/AHCI drivers fall back to shared INTx or polling | M1 (+M3 for AHCI) |
| D2 | no MSI message decode/delivery | an enabled capability would signal into the void | M2 |
| D3 | devices cannot select MSI vs INTx | enabling order (OS programs cap, expects pin to go quiet) unhonoured | M3 |

## 1e. M1 measured receipts (MSI-X capability on the xHCI)

- **Shape** (cfg space of 0:20.0): Status bit 4 (Capabilities List) set,
  cfg 0x34 = 0x90; at 0x90: ID 0x11, Next 0x00, Message Control Table
  Size = 15 (16 entries), Table Offset/BIR = 0x3000/0, PBA
  Offset/BIR = 0x3800/0 -- the QEMU offsets verbatim.
- **RO discipline**: the default cfg write path is fully writable, so
  `xhci_pci_cfg_write` re-asserts every RO field after each write; a
  vector blasts 0xFFFFFFFF across the capability and only Message
  Control's Enable (bit 15) and Function Mask (bit 14) move. Those two
  bits fold into `xhci_t` (`msix_enable`/`msix_fmask`) at the same
  place, ready for M3's router.
- **Table/PBA MMIO**: entries at BAR0+0x3000 (16 x {addr_lo, addr_hi,
  data, ctrl}), power-on state all masked (Vector Control = 1);
  storage round-trips through real MMIO; past-the-table reads 0; PBA
  at +0x3800 reads 0 and ignores writes (the §2 no-accumulation rule).
- **Ownership line**: MSI-X state is PCI-function state, not HC state --
  `xhci_hw_reset` preserves the table and both control bits across
  HCRST (vector-asserted), matching silicon and the snapshot model
  (the table rides `xhci_t`).
- NC: `src/xhci.c` + `src/devices.c` reverted to M0 → 13 failures.

## 1f. M2 measured receipts (delivery core)

- **`msi_deliver(m, addr, data)`** (`src/lapic.c`, next to its sink):
  decodes the x86 message and lands on `lapic_set_irr_dest` -- the
  same endpoint the IOAPIC proves every boot. The new `test-msi` lane
  observes landings through the real arbitration
  (deliverable -> intack -> EOI over MMIO), not through internals.
- **Measured behaviours**: physical Fixed arrives and acks; an
  unroutable physical dest falls back to the BSP (the IOAPIC's own
  documented rule); a non-APIC address (0xFEC window) is ignored;
  Lowest Priority degenerates to Fixed at the named destination (no
  arbitration fabric exists to consult); flat-logical destinations
  match LDR[31:24] with the same BSP fallback; a vector < 16 latches
  the ESR receive-illegal-vector bit and pends nothing; the bare
  (0xFEE00000, vector) pair a driver writes into an MSI-X entry lands
  verbatim, with level/trigger bits advisory (one edge, no phantom).
- **Refused, by decision**: delivery modes SMI/NMI/INIT/ExtINT and
  reserved encodings are logged (`[msi] delivery mode N dropped`) and
  inject nothing; the vectors assert the ring stays empty for each.
- NC: `msi_deliver` body stubbed out → 9 failures across the 4 groups.

## 1g. M3 measured receipts (per-device routing)

- **One router per device**: both controllers already had exactly one
  place computing the interrupt level (`xhci_intx_update`, AHCI's
  `intx_update`); M3 turns each into the router. The level (xHCI:
  IP&IE&INTE; AHCI: (PxIS&PxIE)|(IS&GHC.IE)) is untouched -- only its
  *transport* is chosen: MSI(-X) enabled -> pin forced silent, one
  message on the level's rising edge; disabled -> the level pin,
  byte-identical to before. The guest's ack lowers the level and
  re-arms the edge; a second completion under a held level sends
  nothing (both vectors assert exactly-once).
- **Mask release**: xHCI per-vector mask and Function Mask behave the
  same -- pending rises silently, the unmask write itself releases
  the message (router call sites added to table-MMIO writes and to
  the cfg-write sync). No PBA accumulation anywhere (the §2 rule).
- **AHCI capability shape** measured from two independent ICH `lspci`
  dumps: **MSI at cfg 0x80, Count=1/1, Maskable-, 64bit-** -- one
  message, 32-bit address, no per-vector masking; seeded by
  `ahci_pci_seed` on both controllers (a PM cap, present on silicon
  at 0x70, is PM_PLAN's, not invented here). RO fields re-asserted
  after every cfg write, MME accepted but one message exists.
- **Fallback proof**: both vectors end by disabling the capability
  and watching the *same* completion drive the pin again; every
  historical lane stayed green in `make test` (nothing enables MSI
  anywhere in the suite).
- NC: xHCI engine at M2 → 5 failures (the route group; the group's
  fallback expectations still pass there, by design); AHCI router
  branch stubbed → the msi_line group asserts on the raised pin.

## 1h. M4 measured receipts (snapshots, determinism) + close-out

- **Snapshots**: no new code was needed and that is worth recording --
  both state copies already ride whole-struct snapshot tags
  (`xhci_t` in `dev.xhci`, `ahci_t` in `dev.ahciN`), and the PCI cfg
  side (Message Control bits) rides the per-function 256-byte
  `pci.BB.DD.F` records. The M4 vector proves the composition instead
  of assuming it: programme entry 0 + Enable, save, vandalise
  (disable, scribble the entry, HCRST), load — cfg enable bit, table
  contents and *live delivery of the next message* all restored.
- **Determinism**: a fixed message script (physical, fallback,
  illegal, logical) replayed against `msi_deliver` yields identical
  intack streams twice.
- NC: MSI-X state dropped on snapshot load → 3 failures.

### Close-out

M0–M4 complete (`patches/0082`–`0086`): from zero PCI capabilities to
a QEMU-parity MSI-X on the xHCI (16 entries, entry 0 in use), a
shared x86 message-delivery core on the IOAPIC's LAPIC sink, an
ICH-parity MSI on both AHCIs, per-device edge routing with
byte-identical INTx fallback, and snapshot/determinism receipts.
Ledger D1–D3 closed by M1/M2/M3. Stated limits: one MSI-X vector in
use (one interrupter exists), no PBA accumulation (level-source
re-evaluation), Lowest Priority degenerates to Fixed (no arbitration
fabric), SMI/NMI/INIT/ExtINT messages refused, no interrupt
remapping, e1000 stays INTx-only by silicon parity, and the
north-star guest still prefers INTx -- which now has a modern
sibling waiting for whoever asks.

## 2. Architecture decisions (M0)

- **XHCI gets MSI-X in the QEMU shape**: capability at cfg 0x90 (id 0x11,
  next=0), Table Size 16 (Message Control[10:0] = 15), table at
  BAR0+0x3000, PBA at BAR0+0x3800. We use entry 0 (interrupter 0 is the
  only source, `HCSPARAMS1.MaxIntrs=1`). The other 15 entries exist for
  shape parity; a guest that arms them sees correct table storage and
  no sources, which is what a 1-interrupter controller is.
- **Delivery is synchronous and edge-shaped**, consistent with the whole
  device fleet: when the interrupter condition (IP level) rises with
  MSI-X enabled and unmasked, one message is delivered (the X5
  `xhci_intx_update` single call site becomes the router). While masked
  (Function Mask or per-vector Mask), the pending bit does **not**
  accumulate in v1: IP itself stays latched and unmasking re-evaluates
  the level, which is equivalent for a level source -- stated here so
  M1 vectors assert the actual contract, not the spec's PBA subtlety.
- **`msi_deliver(m, addr, data)` lives in `lapic.c`** next to its sink:
  x86 message decode (addr: dest id at [19:12], RH/DM; data: vector
  [7:0], delivery mode [10:8]). Fixed delivery lands as-is; other modes
  are logged and delivered as Fixed in v1 (single-vCPU machine: lowest
  priority degenerates to the one CPU; SMI/NMI/INIT messages are
  dropped with a log line, never fabricated).
- **INTx fallback is the default and byte-identical**: nothing changes
  until a guest sets MSI-X Enable (or MSI Enable on AHCI in M3), so
  every historical receipt and the guest's INTx path are preserved by
  construction; the X5 vectors keep guarding the pin.
- **Cap plumbing is per-device**: the capability list pointer/Status
  bit get set in `devices.c` device construction, and the capability
  registers ride each controller's existing cfg hooks (RO fields
  written by nobody, Enable/mask/table storage in controller state,
  covered by the existing snapshot tags -- `xhci_t` for the table).

## 3. Gates (standing rules)

Per phase: profile vectors (`test-xhci`, `test-ahci`, a new `test-msi`
lane for M2's delivery core) + one NC + `make test` +
`make test-sanitize` + both SeaBIOS ISO lanes, docs in the same commit,
one patch per phase, cumulative patch refreshed from the current
github `main`.
