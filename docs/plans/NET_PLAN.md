# NET plan — e1000 networking (N0–N3)

> **House style** (adopted from
> [`AuraLite-OS/docs/plans`](https://github.com/AlexanderNyr/AuraLite-OS/tree/main/docs/plans)):
> each effort is a dependency-ordered plan in `docs/plans/` with a
> definition of done and a test gate per phase, and ships as **one
> `.patch` per phase** under `patches/`. Every claim in these docs is
> measured against the tree, not assumed.

The machine owes the guest its primary NIC: an **Intel 82540EM (e1000)**
behind the QEMU `hw/net/e1000.c` behavioural contract, plus a **host TAP
backend** that moves Ethernet frames between the guest and a host-side
endpoint. Measured from `AuraLite-OS@0ed0d29` (`drivers/e1000/e1000.c`,
`drivers/pci/pci.c`, `kernel/kernel.c:425-440`) and the frozen guest
kernel (`kernel.elf` sha256 `395e5ec02bf39cc5…`, 3051512 B).

## 0. Ground rules (fixed, N0 artifact)

1. **TAP socket**: UDP datagrams = Ethernet frames at the fixed endpoint
   `127.0.0.1:8080` (the emulator **connects**; the host-side responder
   **binds**). CLI `--tap=host:port` (a bare host keeps port 8080).
2. **MAC**: the QEMU default `52:54:00:12:34:56`, pre-loaded into
   RAL/RAH (`RAL=0x12005452`, `RAH=AV|0x5634`) like QEMU's
   `e1000_reset` — the guest prefers it over EEPROM (measured lane
   receipt `[e1000] MAC from RAL/RAH`).
3. **Opt-in**: without `--tap=` no PCI function exists at all, so every
   historical lane (K0–K8 firmware, the U5 tapes) keeps its
   byte-for-byte receipts. The firmware line and the guest-visible
   firmware contract do not move (GPU probe 0:2:0 `0x8086:0x0412`,
   BAR0 `0xD0000000`, framebuffer marker `0x00200000`).
4. **QEMU compat reset**: zeroed register file, `STATUS.LU` link up,
   `RXCSUM` reads 0, `CTRL.RST` (bit 26) self-clears (the guest polls
   on it).

**Status: in progress — N0 ✅, N1–N3 planned 📋**

| Phase | Scope | Status | Deliverable |
|---|---|---|---|
| N0 | Measured baseline + this plan; the RX-ready e1000 (PCI `8086:100E` at 0:4:0 + BAR0 `0xFEBC0000`/128 KiB + reset contract + RX ring DMA) + TAP ingress (`--tap=`) | ✅ done | `patches/0046-NET-N0-e1000-rx-ready.patch` |
| N1 | TX-ready: the TDT doorbell → TDH walk → TAP datagrams (`TCTL.EN`), TX descriptor completion (the guest's `[e1000] TX timeout` receipts gone) | 📋 | `patches/0047-NET-N1-e1000-tx-ready.patch` |
| N2 | Host responder on the TAP endpoint (DHCP + ARP + ICMP, the QEMU `-netdev user` receipt shape) so the guest's online self-tests pass | 📋 | `patches/0048-NET-N2-tap-responder.patch` |
| N3 | End-to-end close-out: determinism ×2, sanitize, docs close-out (plan closed) | 📋 | `patches/0049-NET-N3-net-e2e.patch` |

Note: the *guest* runs its own online self-tests (`net_self_test`:
DHCP → ARP → ICMP echo; then DNS/TCP checks when the lease path is
live). This plan closes the **emulator's NIC + TAP** gaps those tests
assume; `[net] PASS: lease + ARP + echo reply (payload verified)` is the
N2 acceptance receipt.

## 1. Measured baseline (N0 entry state)

### 1.1 What the machine presented before N0

No e1000 model at all. The guest probe chain falls through to the
HISTORICAL receipts (measured `krun/u5/uhci1.out` sha256
`e615c39ec0c84cd7…` lines 154–159):

```
[serial] [e1000] NIC not found on PCI bus (supported: 82540EM/82545EM/82543GC)
[serial] [e1000e] NIC not found on PCI bus (supported: 82540EM/82545EM/82543GC)
[serial] [r8169] no Realtek gigabit NIC on PCI (supported: 8169/8168/8111)
[serial] [rtl8139] no Realtek NIC on PCI (supported: 8139/8138/8100/8130)
[serial] [net] no NIC available
[serial] [net] network unavailable; continuing boot without online self-tests
```

### 1.2 What the guest expects (measured, `AuraLite-OS@0ed0d29`)

- **Probe** (`netdev` chain): e1000 → e1000e → r8169 → rtl8139; on
  success `[net] using NIC: %s` (the N0 lane receipt gate).
- **`drivers/e1000/e1000.c`**: PCI `8086:100E/100F/1004` ("82540EM /
  PRO/1000 MT Desktop (0x100e) at PCI 0:4.0"); BAR0 = 128 KiB MMIO,
  mapped through `pci_get_bar()` + `paging_map` of the HHDM window;
  `CTRL.RST` poll 1e6; `CTRL |= FD|SLU`; `STATUS.LU` wait 1e5
  (printed: `STATUS=0x%08x (LU=%u)`); MAC via EEPROM (`EERD`) →
  RAL/RAH → hardcoded QEMU default (`[e1000] using default QEMU MAC`
  vs `[e1000] MAC from RAL/RAH`); TX 32 / RX 64 descriptors, 2048-byte
  buffers; `RCTL=0x0400801a` on init (EN|UPE|MPE|BAM|SECRC);
  `e1000_link_up()` = `STATUS.LU`; TX expects TDH advancement or
  prints `[e1000] TX timeout: desc[N].status=0x%02x TDH=%u TDT=%u`.
- **`kernel/kernel.c:425-440`**: `net_init() == 0` → online self-tests
  (`net_self_test` DHCP→ARP→ICMP, DNS, TCP x5, TCP6); `> 0` →
  `[net] fallback IP active; skipping online self-tests to keep boot
  fast`; `-1` → `[net] network unavailable; continuing boot without
  online self-tests`.
- **Self-test receipts** (acceptance surface): PASS
  `[net] PASS: lease + ARP + echo reply (payload verified)`; FAILs
  `[net] FAIL: no DHCP lease` / `gateway ARP unanswered` /
  `[net] FAIL: no ICMP echo reply (is QEMU -netdev user configured?)`.
  N2's responder is what makes the PASS reachable.

### 1.3 The QEMU contract to implement (`hw/net/e1000.c` `e1000_reset`)

PCI `8086:100E`, class `0x02/0x00` at 0:4:0; BAR0 `0xFEBC0000` size
`0x20000`. Register file (`E1000_REG_BYTES = 0x5800`):

| Reg | Off | Reset / behaviour |
|---|---|---|
| CTRL | `0x0000` | `FD\|SLU` set by the guest; `RST` (1<<26) self-clears |
| STATUS | `0x0008` | `LU` (1<<1) on attach |
| EERD | `0x0014` | EEPROM read stub (guest falls through to RAL/RAH) |
| ICR | `0x00C0` | read-to-clear; `RXT0` (1<<7) on RX |
| IMS / IMC | `0x00D0` / `0x00D8` | IMC clears the mask |
| RCTL | `0x00100` | `EN`(1<<1) gates RX; `BAM`(1<<15), `SECRC`(1<<26) |
| MDIC | `0x00200` | passthrough register (PHY stub) |
| TCTL | `0x00400` | `EN`(1<<1), `PSP`(1<<3) (TX is N1) |
| RDBAL/RDBAH/RDLEN/RDH/RDT | `0x2800..0x2818` | RX ring (16-byte descriptors) |
| TDBAL/TDBAH/TDLEN/TDH/TDT | `0x3800..0x3818` | TX ring (doorbell = TDT write; N1) |
| RXCSUM | `0x05000` | reads 0 after reset |
| MTA | `0x05200` | multicast table (stored) |
| RAL0/RAH0 | `0x5400` / `0x5404` | preload `0x12005452` / `AV\|0x5634` |

RX descriptor (16 B): `addr_lo, addr_hi, length, csum, status
(DD=0x01), errors, special`; on ingress: DMA frame into the head
buffer, set DD, `RDH = (RDH+1) % (RDLEN/16)`, `ICR |= RXT0`. Drop
gates: `RCTL.EN` clear, `RDH == RDT`, frame > 2048 clipped to one
buffer.

### 1.4 Defect ledger (measured; drives N0–N3)

- **D0 — no e1000** (1.1 receipts). Fixed by N0.
- **D1 — BAR0 not seeded into PCI config** (measured by the first N0
  lane, fixed inside N0): the guest mapped **phys 0** and every
  register read hit RAM — lane receipts
  `[e1000] BAR0 at phys 0x0 -> mmio 0xffff800000000000`,
  `[e1000] STATUS=0x00000000 (LU=0)`, `[e1000] using default QEMU MAC`,
  `[net] link is down; skipping DHCP and network self-tests`. Fix: seed
  `cfg[0x10..0x13] = 0xFEBC0000` in `e1000_add` (the house pattern,
  cf. EHCI/UHCI/AHCI) + the regression vector in
  `ok e1000 pci/reset contract` (BAR0 readback over `0xCF8/0xCFC`).
  Post-fix receipts: `BAR0 at phys 0xfebc0000`,
  `STATUS=0x00000002 (LU=1)`, `MAC from RAL/RAH`.
- **D2 — TX not modelled** (measured, deferred to N1): the TDT
  doorbell stores but TDH never advances — receipts
  `[e1000] TX timeout: desc[N].status=0x00 TDH=0 TDT=N+1`,
  `[net6] RS TX failed`, `[dhcp] FAIL: DISCOVER transmit failed (link
  down or TX timeout)`, then `[net] DHCP failed, using hardcoded IP
  10.0.2.15` / `[net] our IP: 10.0.2.15, gateway: 10.0.2.2` /
  `[net] fallback IP active; …`.
- **D3 — no host responder** (deferred to N2): DHCP/ARP/ICMP go
  unanswered, so the online self-tests cannot pass.

## 2. Phases

### N0 — RX-ready e1000 + TAP ingress — ✅ done (`patches/0046-NET-N0-e1000-rx-ready.patch`)

**Implementation** (`src/e1000.c`, `src/devices.c`, `src/devices.h`,
`src/main.c`, `Makefile`): the device model registers PCI 0:4:0
`8086:100E` (class `0x02/0x00`) with seeded BAR0 and the 128 KiB MMIO
window (`mem_register_mmio`, non-owned ctx); `e1000_reset_regs` (the
QEMU behavioural core); `e1000_mmio_read/write` (ICR read-to-clear,
CTRL.RST self-clear, IMC→IMS); `e1000_rx_frame` (ring DMA + gates);
`e1000_test_add` (socketless unit entry); `e1000_attach` (`--tap=`,
UDP connect + nonblocking); `e1000_tick` (≤8 TAP datagrams/tick, from
`devices_tick`). Without `--tap=` no PCI function is added (ground
rule 3).

**Unit vectors** (`tests/test_net.c`, in `make test-unit`):

```
ok e1000 pci/reset contract (RXCSUM 0, RAL/RAH preload, RST self-clear)
ok e1000 rx ingress (DMA + DD + RDH, EN/full gating)
test-net: all tests passed
```

(`pci/reset contract`: RXCSUM 0, RAL/RAH preload, STATUS.LU, CTRL.RST
self-clear + preload survives, ICR read-to-clear, **BAR0 readback over
0xCF8/0xCFC == 0xFEBC0000** — the D1 regression vector. `rx ingress`:
RCTL.EN drop gate, frame DMA `0x6c6c6568`/"hell" LE + `o`, desc.length,
DD, RDH++/RXT0/ICR read-to-clear, ring-full drop with RDT = RDH = 1.)

**NC (once per phase)**: removing the RX-DMA `mem_write` loop from
`e1000_rx_frame` reddens the ingress vector —
`RC=134`, `tests/test_net.c:85: test_rx_ingress: Assertion
'mem_read(&f.m, buf, 4) == 0x6c6c6568' failed`; restored from backup,
gates re-green.

**Lanes ×2** (byte-identical determinism pair, `--max-instr=4500000000`):

```
./x86emu --platform=haswell \
    --kernel=/home/user/AuraLite-OS/build/kernel.elf \
    --initrd=/home/user/AuraLite-OS/build/initrd.tar \
    --tap=127.0.0.1:8080 \
    --log=tests/integration/n0-guest-1.log   # and n0-guest-2.log
```

`kernel.elf` sha256 `395e5ec02bf39cc5…` (3051512 B, the U5 freeze
binary), `initrd.tar` sha256 `0581d3a5cb777af0…` (11653120 B, the
U5-era full initrd — `[kload] initrd=… (11653120 bytes)` receipts
match the U5 tapes). Guest receipts (`tests/integration/n0-guest-1.log`
lines 155–183):

```
[serial] [e1000] found 82540EM / PRO/1000 MT Desktop (0x100e) at PCI 0:4.0
[serial] [e1000] BAR0 at phys 0xfebc0000 -> mmio 0xffff8000febc0000
[serial] [e1000] STATUS=0x00000002 (LU=1)
[serial] [e1000] MAC from RAL/RAH
[serial] [e1000] MAC 52:54:00:12:34:56
[serial] [e1000] RX ring phys=0x3c89000, desc0_addr=0x3c8a000 desc0_status=0
[serial] [e1000] IRQ line 0 enabled (IMS=0x000000d5)
[serial] [e1000] TX/RX rings initialised, RCTL=0x0400801a
[serial] [netdev] active NIC: e1000
[serial] [net] using NIC: e1000                  <- the N0 lane gate
[serial] [net6] link-local fe80::5054:ff:fe12:3456 (from MAC 52:54:00:12:34:56)
[serial] [e1000] TX timeout: desc[0].status=0x00 TDH=0 TDT=1   (D2, N1)
[serial] [dhcp] starting DHCP discovery...
[serial] [dhcp] FAIL: DISCOVER transmit failed (link down or TX timeout) (D2/D3)
[serial] [net] DHCP failed, using hardcoded IP 10.0.2.15
[serial] [net] our IP: 10.0.2.15, gateway: 10.0.2.2
[serial] [net] fallback IP active; skipping online self-tests to keep boot fast
```

Determinism: the two `--log` tapes are byte-identical (`cmp` clean):

| Artifact | sha256 (lane 1 = lane 2) |
|---|---|
| `tests/integration/n0-guest-1.log` | `82614cd6f896aa9a2ac2e4b75ca1be86ba9e7807de04d76c847e6ca6c9be580b` |
| `tests/integration/n0-guest-2.log` | `82614cd6f896aa9a2ac2e4b75ca1be86ba9e7807de04d76c847e6ca6c9be580b` |

(The `*.log` tapes stay gitignored per house rule; the shas are the
receipts. The `[kload]`/`[boot]` prefix is byte-equal to the U5-era
tapes' prefix up to the USB/disk divergence — same frozen kernel +
initrd + paths.)

**Gates**: `make test` RC=0 (unit + table + kload + ring3 + boot
integration, firmware contract `pixel[0]=0x00200000` intact);
`make test-sanitize` RC=0 (ASan+UBSan).

### N1 — TX-ready — 📋 planned

The TX ring: TDT doorbell walks TDH, descriptors are consumed
(`status.DD`), frames leave through the TAP socket (`TCTL.EN` gate),
`IMS.TXD` interrupt when requested. DoD: unit vectors per the QEMU TX
contract (doorbell, completion, gate) + the guest's `[e1000] TX
timeout` receipts disappear and `[dhcp]` reaches the wire; the N0 lane
pair re-run byte-stable modulo the TX receipts; NC + sanitize.

### N2 — Host responder (DHCP + ARP + ICMP) — 📋 planned

A host-side responder bound to `127.0.0.1:8080` answering the guest's
DHCP discovery with a lease, ARP for the gateway, and ICMP echo (the
QEMU `-netdev user` receipt shape the guest already names). DoD: the
guest's online self-tests print `[net] PASS: lease + ARP + echo reply
(payload verified)` end-to-end through the TAP socket; determinism ×2;
NC + sanitize.

### N3 — End-to-end close-out — 📋 planned

DNS/TCP online receipts where the guest runs them (skips allowed when
the peer is absent), full-lane determinism ×2, sanitize, docs
close-out (plan closed).

## 3. Standing constraints

- The e1000 exists **only** under `--tap=`; historical lanes (K0–K8
  firmware boot, U5 tapes) stay byte-identical (ground rule 3).
- The firmware line and the guest-visible firmware contract do not move
  (GPU 0:2:0 `0x8086:0x0412`, BAR0 `0xD0000000`, marker `0x00200000`).
- One `.patch` per phase; the fix and its regression vector ship in the
  same patch; one NC per phase; the docs of a phase ship in the phase's
  own patch.
