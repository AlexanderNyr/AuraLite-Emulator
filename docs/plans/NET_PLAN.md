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

**Status: COMPLETE — N0–N3 ✅ (plan closed by N3)**

| Phase | Scope | Status | Deliverable |
|---|---|---|---|
| N0 | Measured baseline + this plan; the RX-ready e1000 (PCI `8086:100E` at 0:4:0 + BAR0 `0xFEBC0000`/128 KiB + reset contract + RX ring DMA) + TAP ingress (`--tap=`) | ✅ done | `patches/0046-NET-N0-e1000-rx-ready.patch` |
| N1 | TX-ready: the TDT doorbell → TDH walk → TAP datagrams (`TCTL.EN`), TX descriptor completion (the guest's `[e1000] TX timeout` receipts gone) | ✅ done | `patches/0047-NET-N1-e1000-tx-ready.patch` |
| N2 | Host responder on the TAP endpoint (DHCP + ARP + ICMP, the QEMU `-netdev user` receipt shape) so the guest's online self-tests pass | ✅ done | `patches/0048-NET-N2-tap-responder.patch` |
| N3 | End-to-end close-out: DNS (UDP/53 + the RFC 1035 TCP carriage) and TCP (16 concurrent connections) on the responder, on-link ARP, determinism ×2, sanitize, docs close-out (plan closed) | ✅ done | `patches/0049-NET-N3-net-e2e.patch` |

Note: the *guest* runs its own online self-tests (`net_self_test`:
DHCP → ARP → ICMP echo; then DNS/TCP checks when the lease path is
live). This plan closes the **emulator's NIC + TAP** gaps those tests
assume. N2's acceptance receipts are the two the frozen guest's own
integration cases assert — `[dhcp] PASS: IP 10.0.2.15, gateway
10.0.2.2` (`AuraLite-OS/tests/integration/cases/test_e1000e.sh:48`,
`test_vmxnet3.sh:49`) and `[net] PASS: ping 10.0.2.2 successful (ICMP
echo reply received)` (printed at `kernel/net/net.c:1190`, asserted at
`test_e1000e.sh:50`, `test_vmxnet3.sh:51`, and `scripts/ci_test.sh:72`).

*Correction, measured in N2:* earlier revisions of this note quoted
`[net] PASS: lease + ARP + echo reply (payload verified)` as the N2
receipt. That line exists, but in `kernel/arch/i386/net32.c:319` — the
**i386** port, not the x86_64 kernel this emulator boots. The x86_64
guest never prints it, so a harness that grepped for it would have
passed against nothing. `tests/nic_net.c` prints it anyway, because that
payload reproduces the i386 port's *stronger* check (the reply's ident,
seq and all 32 payload bytes) as an extra receipt on top of the two the
x86_64 guest actually emits.

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
- **D2 — TX not modelled** (fixed by N1): the TDT doorbell stored but
  TDH never advanced and no descriptor was ever completed — receipts
  `[e1000] TX timeout: desc[N].status=0x00 TDH=0 TDT=N+1`,
  `[net6] RS TX failed`, `[dhcp] FAIL: DISCOVER transmit failed (link
  down or TX timeout)`, then `[net] DHCP failed, using hardcoded IP
  10.0.2.15` / `[net] our IP: 10.0.2.15, gateway: 10.0.2.2` /
  `[net] fallback IP active; …`. N1 re-measured the same shape from the
  machine side with the wire payload (lane B of `n1_tx_tap.sh` still
  shows it whenever the device is absent:
  `N1TXFAIL status=00 tdh=ff icr=ff`), and the post-fix receipt is
  `N1TXOK status=01 tdh=01 icr=81` plus the frame on the socket.
- **D3 — no host responder** (fixed by N2): DHCP/ARP/ICMP went
  unanswered, so the online self-tests could not pass. N2 adds
  `tools/tap_responder.py` on the ground-rule endpoint and measures the
  receipts in §N2 below; `--drop=arp,icmp,dhcp` reproduces the old
  behaviour on demand (that is lane D, the phase's NC).
- **D4 — the INTx line is not wired** (measured in N1, deliberately
  *not* fixed there): `e1000_add` does not seed PCI config `0x3C`
  (interrupt line) and nothing emulates INTA#/ack, so the guest reads
  `IRQ line 0` and stays on its polled path (N0 receipt
  `[e1000] IRQ line 0 enabled (IMS=0x000000d5)`). N1 therefore does
  ICR/ICS **bookkeeping only** — `set_ics` semantics without asserting
  a line. Raising one now would be *worse* than not raising it: with no
  ack path the 8259A ISR bit would stay set and the polling guest would
  never be re-serviced. Full INTx (cfg `0x3C`, `pci_set_irq`, the
  `(ICR & IMS) != 0` condition, EOI) is N3/N4 scope.
- **D8 — ARP only answered for the gateway** (measured in N3, fixed
  there): `net_arp_resolve` (`net.c:356`) ARPs the *target* for an
  on-link address and only routes off-subnet traffic through the
  gateway, so N2's responder — which answered who-has for `10.0.2.2`
  alone — made the DNS server unreachable. Receipt before the fix:
  `[net] ARP request sent for 10.0.2.3` then
  `[dns] FAIL: could not resolve the server's MAC`. Fix: answer for the
  gateway **or** any address in the guest's own `/24` (never the guest's
  own, never off-subnet), with the log suffix `(on-subnet)`; pinned by
  `test_arp_for_the_whole_subnet`.
- **D9 — TCP checksums were optional** (measured in N3, fixed there):
  `tcp_segment` was passed `compute_csum=self.udp_checksum`, so with the
  default flags every FIN-ACK and RST left with checksum 0. That is
  legal for UDP and not for TCP (RFC 793 §3.1 makes it mandatory over
  IPv4), and `tcp.c:319` drops such a segment. Receipt before the fix:
  `TCP: bad checksum ignored` on the wire log and
  `[dns] FAIL: TCP carriage returned 0 of 2 bytes`. TCP checksums are
  now always computed; `--udp-checksum` governs UDP only.
- **D10 — the odd-length checksum tail** (measured in N3, fixed there,
  in the *test-side* code, not the emulator): the payload's
  `checksum()` and the vectors' `g_checksum()` added a trailing byte as
  `p[len-1]` instead of `p[len-1] << 8` (RFC 1071; the frozen guest
  does it right at `net.c:181`). Invisible while every length was even
  (IP header 20, ICMP echo 40); it appeared on the first odd-length
  segment, a 49-byte TCP segment with a 29-byte DNS query, off by
  `0xFF00`. Both copies fixed; the rule is pinned by
  `test_dns_over_tcp` and by the wire tape.
- **D11 — harness port contention** (measured in N3, fixed there):
  `SO_REUSEADDR` lets two UDP sockets bind the same port, so a responder
  left over from a hand run silently split lane A's datagrams — the lane
  saw `3 frame(s) in, replies arp=0 dhcp=0 icmp=0 dns=0 tcp=0` while the
  next lane was perfect. Both harnesses now check `port_free()` before
  starting a lane and print the remedy (`pkill -f
  tools/tap_responder.py`) rather than reporting a bogus failure.
- **D5 — QEMU's `TXD_STAT_*` bits sit in the wrong descriptor byte**
  (measured in N1, diverged from QEMU on purpose): QEMU's
  `hw/net/e1000.c` defines `E1000_TXD_STAT_DD` as `0x01000000`, i.e.
  bit 24 of the dword read at descriptor offset 12 — which lands in
  descriptor **byte 15** (the VLAN ID byte), not the status byte. The
  Intel 8254x SDM puts `DD/EC/LC/TU` in bits 0..3 of the **status byte
  at offset 12**, and the frozen guest driver tests
  `tx_ring[old_tail].status & 0x01` where `status` is the `u8` at
  struct offset 12 (`drivers/e1000/e1000.c:93,571`). This file's own RX
  half already writes DD that way (`RXD_STAT_DD 0x01` at offset 12),
  which is what the N0 guest receipt showed. N1 follows the
  datasheet + guest, byte-granular.
- **D6 — the N0 register-write fallback was too permissive** (fixed
  inside N1): N0 stored every non-CTRL/IMC register verbatim.
  Measured against QEMU `macreg_writeops[]` that is wrong for
  `IMS` (`set_ims` is `IMS |= val`, not an assign), `ICR` (`set_icr` is
  `ICR &= ~val`, not a no-op), `ICS` (`set_ics`), `TDH/RDH`
  (`set_16bit`), `TDLEN/RDLEN` (`set_dlen`, `val & 0xfff80`) and
  `TDT` (shares TCTL's op: store, mask TDT to 16 bits, `start_xmit`).
  No N0 vector pinned the old behaviour, so the change is safe;
  `ok e1000 interrupt/register write-op parity` now pins the new one.
- **D7 — the gateway's MAC address is not measurable** (found in N2,
  *not* fixed, pinned behind a flag): the responder has to put some
  source MAC on the DHCP OFFER/ACK and on the ARP reply, and slirp's
  convention is `52:55:` followed by the gateway's four IP bytes —
  `52:55:0a:00:02:02` for `10.0.2.2`. That is what QEMU/libslirp are
  widely documented to emit, but it could not be **measured** here:
  libslirp's `bootp.c` was unreachable from this sandbox (four mirrors
  tried — `raw.githubusercontent.com/qemu/qemu` at master, v5.2.0 and
  v4.2.0, and `gitlab.freedesktop.org/slirp/libslirp` — all 404 or
  access-blocked), and no QEMU binary is installed to trace. It does not
  gate anything: the guest's ARP accept path (`net.c:441-465`) checks
  only `len >= 42`, ethertype `0x0806` and `opcode == 2`, then copies
  `sender_mac` verbatim — it never compares the target IP either. So the
  value is a wire-detail guess, quarantined behind `--gw-mac` (default
  `52:55:0a:00:02:02`) so that whoever can measure it against a real
  `qemu -netdev user` fixes one flag and no code. Recorded here rather
  than asserted in prose.

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

### N1 — TX-ready — ✅ done (`patches/0047-NET-N1-e1000-tx-ready.patch`)

**TX contract measured first, from two sources.** QEMU
`hw/net/e1000.c`: `macreg_writeops[]` maps **both** `[TCTL]` and
`[TDT]` to `set_tctl` (store, `TDT &= 0xffff`, `start_xmit`), so the
doorbell is a TDT *or* a TCTL write; `start_xmit` returns unless
`TCTL.EN`, guards re-entrancy with `tx.busy`, walks `while (TDH !=
TDT)` over `tx_desc_base + 16*TDH` (`TDBAH<<32 | TDBAL & ~0xf`),
advances with `if (++TDH * 16 >= TDLEN) TDH = 0`, breaks on the TDH
wraparound and on `tdh_start >= TDLEN/16` (its two escapes for a bogus
TDT/TDLEN), and ends with `set_ics(s, 0, cause)` where `cause` starts
at `E1000_ICS_TXQE` and ORs in `E1000_ICR_TXDW` per RS writeback.
`process_tx_desc` takes `split_size = lower.data & 0xffff`, clamps it
to the room left in `tx.data[0x10000]`, DMA-reads at `buffer_addr`,
and only on `CMD.EOP` calls `xmit_seg` (the send) and resets the
assembly. `txdesc_writeback` returns 0 unless `CMD.RS|RPS`, else
`upper = (upper | DD) & ~(EC|LC|TU)` and returns `ICR_TXDW`. The guest
side (`AuraLite-OS@0ed0d29 drivers/e1000/e1000.c`, identical at HEAD):
`e1000_init` programs `TDLEN = 32*16`, `TDH = TDT = 0`,
`TCTL = EN | (0x10<<4) | (0x40<<12)`, `IMS = 0xD5`; `e1000_send`
refuses without `STATUS.LU`, clamps to 2048, fills
`length/cso=0/cmd=0x0B (EOP|IFCS|RS)/status=0`, bumps
`tail = (tail+1) % 32`, **rings `TDT = tail`**, then spins
`while ((tx_ring[old_tail].status & 0x01) == 0 && timeout-- > 0) pause`
with `timeout = 100000` and prints `[e1000] TX timeout:
desc[%u].status=0x%02x TDH=%u TDT=%u` on exhaustion. **Consequence for
the model: DD must be written back *inside* the TDT store, not on a
later tick** — a `devices_tick`-deferred completion would still burn
the guest's PAUSE budget and, worse, make the receipt depend on the
256-instruction cadence.

**Implementation** (`src/e1000.c`, `src/devices.h`, `Makefile`):
`e1000_send_frame` (TAP datagram out, `tap_fd < 0` or a failed `send`
drops the frame but still completes the descriptors — QEMU's
null-backend shape, and what makes the socketless unit lane assert DD
rather than the wire), `e1000_tx_desc` (one descriptor of the walk),
`e1000_start_xmit` (the `set_tctl` walk), `e1000_tx_stats` (the wire
counters — the device logs nothing on the RX/TX path, parity with N0,
so a harness proves "the frame reached the wire" by reading these
instead of grepping a log whose line count would otherwise depend on
how many frames a responder answered with), and TX state in `e1000_t`
(`tx_data[0x10000]`, `tx_size`, `tx_busy`, `tx_frames`, `tx_dropped`)
which `e1000_reset_regs` clears — a half-assembled frame must not
survive a `CTRL.RST` into the next ring. `e1000_mmio_write` gained the
QEMU write-op parity of ledger **D6**.

**Unit vectors** (`tests/test_net.c`, in `make test-unit`) — 2 N0 + 5 N1:

```
ok e1000 pci/reset contract (RXCSUM 0, RAL/RAH preload, RST self-clear)
ok e1000 rx ingress (DMA + DD + RDH, EN/full gating)
ok e1000 tx doorbell completion (DD + CMD intact + TDH + TXDW/TXQE)
ok e1000 tx TCTL.EN gate + TCTL-write doorbell + TDT 16-bit mask
ok e1000 tx multi-descriptor EOP, RS-gated writeback, TDH wrap, TDLEN mask
ok e1000 interrupt/register write-op parity (IMS|=, ICR&=~val, 16-bit heads)
ok e1000 tx link gate + CTRL.RST resets the TX ring
test-net: all tests passed
```

(What they pin: DD + `CMD` left intact + TDH advance + `ICR = 0x81`
(TXDW|TXQE) + read-to-clear + the socketless drop counters; `TCTL.EN`
clear ⇒ TDT stores but consumes nothing *and raises no cause*, then a
TCTL write drains the pending descriptor, `TDT` masked to 16 bits;
a no-EOP descriptor accumulates so two descriptors yield **one** frame
while both get DD (writeback runs for every descriptor of the walk,
EOP or not), `RS` clear ⇒ no DD but the slot is still consumed, TDH
31 → 0 wrap, `TDLEN & 0xfff80`; `IMS |=`, `IMC &= ~`, `ICR &= ~val`,
`ICS` ORs causes in, 16-bit heads; `STATUS.LU` readable, and
`CTRL.RST` clearing TDLEN/TDH while the MAC preload comes back.)

**NC (once per phase)**: removing the descriptor writeback from
`e1000_tx_desc` (DD + `ICR_TXDW`) reddens the doorbell vector —
`RC=134`, `tests/test_net.c:167: test_tx_doorbell_completion:
Assertion 'tx_desc_status(&f, ring, 0) == 0x01' failed` — and the wire
payload's receipt flips to `N1TXFAIL status=00 tdh=01 icr=80`, i.e.
exactly the `status=0x00` half of the D2 guest receipt (the head still
advances, the frame still leaves, only the completion is missing —
which is why the guest's *spin* is what times out). Restored from
backup, gates re-green.

**Lanes** (`make test-net-tx` → `tests/integration/n1_tx_tap.sh`;
needs `nasm` + `python3` and the endpoint `UDP 127.0.0.1:8080` free —
it SKIPs, exit 0, when a dependency is missing):

- **Lane A (wire, ×2 determinism pair).** `tests/nic_tx.asm` (489 code
  bytes of the 512 the loader reads) rides the **real firmware-boot
  transport**: a throwaway `--disk=` image, `[usb-msc] READ(10) LBA=0
  blocks=1` → `bulk-IN 512 bytes -> guest 0x00100000` → `conventional
  entry reached`, long mode, then the payload programs the ring exactly
  like `e1000_init`/`e1000_send` and rings TDT.
  `tests/integration/n1_tap_responder.py` holds the ground-rule
  endpoint and records what arrived:

  ```
  [serial] N1TXOK  status=01 tdh=01 icr=81
  ffffffffffff52540012345608004e3154584f4b
  ```

  The frame is `ff:ff:ff:ff:ff:ff` / `52:54:00:12:34:56` / `0x0800` /
  `N1TXOK` — the buffer the guest DMA'd, byte for byte, 20 bytes in and
  20 bytes out (no FCS invention). `status=01` is DD in the status
  *byte*, `tdh=01` the head advance, `icr=81` TXDW|TXQE.

  *Measured constraint worth keeping:* the firmware's long-mode map is
  **not** all of RAM. `PML4[0] → PDPT[0] (0x12003, a table) → PD`, and
  `PD[0] = 0x19B` is a **2 MiB** page (PS in a PDE), so a payload may
  only touch conventional RAM at `0x100000..0x1FFFFF`; a write to
  `0x200000` raises `#PF v=0x200000 err=0x2` (`PD[1] = 0`). MMIO
  reaches it via `PDPT[3]` (`0xC0000000..0xFFFFFFFF`) — which is how
  the framebuffer at `0xD0000000` and BAR0 at `0xFEBC0000` work. The
  ring and the DMA buffer therefore live at `0x100800`/`0x100C00`.

- **Lane B (opt-in ground rule).** The same payload **without**
  `--tap=`: no PCI function exists, BAR0 reads hit the open bus and
  return all-ones, so the receipt is
  `N1TXFAIL  status=00 tdh=ff icr=ff` and no frame can exist. This is
  the pin that keeps every historical no-NIC lane byte-stable.

- **Lane C (frozen-guest receipts) — SKIP in this tree.** The N0 lane
  re-run; asserts `[net] using NIC: e1000`, `[dhcp] starting DHCP
  discovery`, the **absence** of `[e1000] TX timeout`, and ≥1 frame on
  the wire. The AuraLite-OS toolchain (`clang --target=x86_64-elf`,
  `ld.lld`, `rustc`, `mtools`) is not installable here, so the frozen
  `kernel.elf`/`initrd.tar` cannot be produced; the harness prints the
  exact reproduction line instead of failing:

  ```
  KERNEL=/home/user/AuraLite-OS/build/kernel.elf \
  INITRD=/home/user/AuraLite-OS/build/initrd.tar \
      tests/integration/n1_tx_tap.sh
  ```

  (Note for whoever runs it: `AuraLite-OS/SHA256SUMS` pins
  `kernel.elf` `be3258d7…`, not the U5-freeze `395e5ec0…` this plan
  cites, so a fresh build is a *different* binary — record its sha
  alongside the receipts.)

Determinism: lane A run twice, both tapes and both frame logs
byte-identical (`cmp` clean; the harness defaults to the fixed workdir
`/tmp/n1-tx-tap` because the tape embeds the `--disk=` path, which is
what makes these shas reproducible):

| Artifact | sha256 |
|---|---|
| `a1.out` = `a2.out` (lane A tape) | `f080146e99daf0086bf7a9c6f0c724586543e4e54204a7b904c1f1dfbd14e3be` |
| `a1-frames.hex` = `a2-frames.hex` (wire) | `407c20de647cfb1459ebcd72d24e62167db60f7ec4cdee300a85c6cbb904c8bf` |
| `b1.out` (lane B tape) | `270c237cfd4ff5ffbed1ec6c484ce323fe2f199dfd793a1d3bcbbdcadd652fc8` |
| `nic_tx.bin` (payload, 512 B) | `29a20dd600b1944800a3e0fea79b8e4651b0e97d5a1bf83f4d1bddcee13f40f3` |

**Gates**: `make test` RC=0 (now including `test-net-tx`; the boot
integration receipts are untouched — `[usb-msc] READ(10) LBA=0
blocks=1`, `GUEST_OK`, `framebuffer marker OK: pixel[0]=0x00200000`);
`make test-sanitize` RC=0 (ASan+UBSan), and because that target does
not rebuild `test-net`, the net vectors *and* the wire lane were also
run by hand under `-fsanitize=address,undefined
-fno-sanitize-recover=all` (`X86EMU=<asan build> tests/integration/
n1_tx_tap.sh`) — both clean, so the real `send()` path is covered too;
`make fuzz FUZZ_PROGRAMS=64` — `64 programs, 1536 instructions
compared -- all states match`, `0 host-level crashes`.

**Not in N1** (measured, deferred): the INTx line (**D4**), TX
statistics counters (`TPT`/`TOTL`/`GPTC`/`PTC*`, QEMU's `xmit_seg` +
`e1000x_increase_size_stats`), context/TSO descriptors, VLAN insertion
(`VLE`), and FCS insertion for `CMD.IFCS` — the guest asks for IFCS
but the wire frame is what the buffer held, which is also what QEMU's
`e1000_send_packet` hands its backend.

### N2 — Host responder (DHCP + ARP + ICMP) — ✅ done (`patches/0048-NET-N2-tap-responder.patch`)

**Implementation** (`tools/tap_responder.py`, 485 lines): a
single-file, stdlib-only host responder bound to the ground-rule
endpoint `UDP 127.0.0.1:8080`. It is deliberately **stateless** — it
keeps no lease table, no ARP cache and no sequence numbers, so two
instances answer identically and replaying a frame answers identically
(both are pinned below). It answers exactly the three protocols the
frozen guest speaks and nothing else:

| protocol | accepted when | answered with |
|---|---|---|
| DHCP | `udp.sport == 68 and dport == 67`, option 53 ∈ {1, 3}, `xid` echoed | OFFER (53=2) / ACK (53=5), `yiaddr 10.0.2.15`, options `53, 54, 51, 1, 3, 6, END` in slirp's order, broadcast at L2 **and** L3 |
| ARP | ethertype `0x0806`, `opcode == 1`, `target_ip == 10.0.2.2` | reply `opcode 2`, `sender_mac = --gw-mac`, broadcast (slirp's shape) |
| ICMP | ethertype `0x0800`, `ip.protocol == 1`, `icmp.type == 8` | `type 0` echo reply, ident/seq/`ip.ident` preserved, 32 payload bytes returned unchanged, unicast |

Addressing is slirp's user-mode scheme, taken from the guest's own
source rather than assumed (`kernel/net/net.c:39-47`): guest
`10.0.2.15`, gateway/DHCP server `10.0.2.2`, DNS `10.0.2.3`, lease
3600 s. Flags: `--mode=slirp|capture` (`capture` is N1's record-only
lane), `--gw-mac` (ledger **D7**), `--udp-checksum` (off by default —
the guest sends 0 and validates nothing), `--drop=arp,icmp,dhcp` (the
NC lever), `--out`, `--count`, `--timeout`, `--quiet`.

*One bug the wire found that no protocol vector could:* the first
version replied with `sendto(reply, (host, port))` — i.e. to the
endpoint it had **bound**, which is the endpoint the emulator
`connect()`ed *to*. The reply went to the responder's own socket. The
host log looked perfect (`DISCOVER → OFFER 10.0.2.15`) while the guest
printed `[dhcp] FAIL: no OFFER received`. Fixed to reply to the peer
address `recvfrom` reports, and locked by
`test_reply_goes_to_the_peer_not_to_the_endpoint`, which drives the real
tool over a real connected socket and fails 3 of its 4 checks when the
bug is reintroduced (measured).

**Protocol vectors** (`tests/test_tap_responder.py`, 481 lines,
`make test-tap-responder`, RC=0): the test builds its requests with a
*second, independent* implementation of `net.c`'s builders, then feeds
the replies back through a transcription of `net.c`'s own accept
filters — so a reply only passes if the guest would accept it. 9
vectors:

```
test_frames_match_the_guest_builder      DISCOVER is 287 B, IP checksum 0,
                                         UDP checksum 0, 68→67; ARP padded to
                                         60; ICMP 74 B with 0x41..0x60
test_arp_reply                           guest filter passes; sender_mac is
                                         the gw MAC; an ARP *reply* from the
                                         guest is not answered (no loop)
test_dhcp_discover_offer                 op=2, chaddr/xid echoed, yiaddr
                                         10.0.2.15, options 53/54/1/3/6/51 +
                                         END, broadcast, ip.total_length 298
                                         == bytes behind the eth header
test_dhcp_request_ack                    ACK 53=5, option 3 → gateway_ip; a
                                         different xid is rejected
test_icmp_echo_reply                     type 0, ident 0x1234 + seq 1 + all
                                         32 bytes unchanged, checksums 0,
                                         src/dst swapped
test_reply_goes_to_the_peer_not_to_the_endpoint
                                         (see above; socket-level)
test_determinism_and_the_drop_lever      two instances identical, replay
                                         identical, --drop per-protocol
test_junk_does_not_answer                runt / wrong-ethertype / wrong-port
                                         / non-echo frames get no reply
test-tap-responder: all tests passed
```

**The wire payload** (`tests/nic_net.c`, 552 lines +
`tests/nic_net.ld`): a freestanding x86_64 guest stand-in that rides the
**K1 kernel-load transport** (`--kernel=`, `src/kloader.c`) rather than
N1's firmware-boot one. The reason is measured, not stylistic: the
firmware loader delivers a single 512-byte block from LBA 0 and its
long-mode map covers only `0x100000..0x1FFFFF` (`PD[0] = 0x19B`, a 2 MiB
page), while DHCP + ARP + ICMP plus a 64-deep RX ring need both more
code and more address space. `--kernel=` gives identity-mapped 0..4 GiB
plus HHDM, entry in long mode with paging on.

The payload reproduces the frozen guest's protocol code line for line
where it matters — `nic_init` programs the rings exactly as
`e1000_init` steps 6/7 do (`RDLEN = N*16`, `RDH = 0`, `RDT = N-1`,
`MTA[0..127] = 0xFFFFFFFF`, `RCTL = EN|SECRC|BAM|MPE|UPE = 0x0400801a`,
`RAL/RAH` with AV), `nic_send` is `e1000_send` including `cmd 0x0B`
(EOP|IFCS|RS), the TDT doorbell and the **100000** pause-spin budget
(`drivers/e1000/e1000.c:565`), and `nic_poll` is `e1000_hw_rx_drain`
(RDH delta, `length` from the descriptor, `status = 0`, `RDT` re-armed).
Rings and buffers sit at fixed physical addresses (`0x300000`/
`0x310000`/`0x340000`/`0x350000`). The DHCP, ARP and ICMP frames it
emits are byte-identical to what `net.c` builds, including `xid
0x12345678`, ident `0x1234`/`0x1235`, `flags 0x8000`, ICMP ident
`0x1234` seq `1`, `ip.ident 1`, `ttl 64` and the 60-byte pad.

*One fidelity gap this closed, and it changes what the NC can be:* the
first `nic_poll` consumed a descriptor on the RDH delta alone, exactly
like the guest does. That made N2's planned emulator-side negative
control — removing `e1000_rx_frame`'s DD writeback
(`src/e1000.c:373`) — **inert**: the run still printed `N2OK`. Reading
`e1000_hw_rx_drain` again shows why — the guest gates on `pkt_len > 0`,
not on DD, so a missing DD genuinely does not break it. Two consequences,
both now in the tree: the stand-in additionally requires DD (a strictly
tighter gate than the guest's, so it cannot pass on an emulator that
publishes a stale length with no completion), and the phase's
emulator-side control removes the **length** writeback
(`src/e1000.c:372`) instead, which does fail.

**NC (once per phase)** — two measurements, one host-side (a lane) and
one emulator-side (documented, because it needs a rebuilt emulator):

1. *Host-side, lane D of the harness.* `--drop=arp,icmp,dhcp`: the
   responder logs `[drop] DHCP message type 1 (negative control)` and
   `replies arp=0 dhcp=0 icmp=0 ignored=0`, the guest degrades to its
   first timeout and stops — `[dhcp] FAIL: no OFFER received`,
   `[net] FAIL: no DHCP lease`, `N2FAIL`, exactly one frame on the wire
   (the DISCOVER). The harness asserts all of it and exits 1 if any of
   it is missing.
2. *Emulator-side, by hand.* Removing
   `mem_write(m, desc + 8, 2, len);` from `e1000_rx_frame`
   (`src/e1000.c:372`) — the RX descriptor length writeback, which is
   the field the guest actually reads — flips lane A to
   `[dhcp] FAIL: no OFFER received` / `[net] FAIL: no DHCP lease` /
   `N2FAIL`, while the responder still logs a complete DISCOVER/OFFER,
   REQUEST/ACK, ARP and ICMP exchange (the frames leave the guest fine;
   only the completion is lost). Restored from backup, gates re-green.
   *Not* a valid control: removing the DD writeback at `:373` — see
   above, measured `N2OK` either way.

**Lanes** (`make test-net-rx` → `tests/integration/n2_responder.sh`,
222 lines; needs `python3` + a C compiler and the endpoints
`UDP 127.0.0.1:8080`/`:8081` free — SKIPs, exit 0, when python3 is
missing):

- **Lane A (wire, ×2 determinism pair).**
  `tests/nic_net.elf` + `tools/tap_responder.py`:

  ```
  [serial] [dhcp] starting DHCP discovery...
  [serial] [dhcp] DISCOVER sent (xid=0x12345678)
  [serial] [dhcp] OFFER: IP 10.0.2.15, server 10.0.2.2
  [serial] [dhcp] REQUEST sent (requesting 10.0.2.15)
  [serial] [dhcp] subnet mask: 255.255.255.0
  [serial] [dhcp] DNS server: 10.0.2.3 (1 total)
  [serial] [dhcp] PASS: IP 10.0.2.15, gateway 10.0.2.2
  [serial] [net] ARP request sent for 10.0.2.2
  [serial] [net] ARP reply: gateway is 52:55:0a:00:02:02
  [serial] [net] ICMP echo request sent to 10.0.2.2
  [serial] [net] ICMP echo reply received from 10.0.2.2 (seq 1)
  [serial] [net] ICMP payload verified (ident+seq+32 bytes)
  [serial] [net] PASS: lease + ARP + echo reply (payload verified)
  [serial] N2OK
  ```

  and the responder's own log:

  ```
  responder: mode=slirp bound 127.0.0.1:8080 gw-ip=10.0.2.2 gw-mac=52:55:0a:00:02:02
    DHCP DISCOVER xid=0x12345678 chaddr=52:54:00:12:34:56 -> OFFER 10.0.2.15
    DHCP REQUEST xid=0x12345678 for 10.0.2.15 -> ACK 10.0.2.15
    ARP  who-has 10.0.2.2 tell 10.0.2.15 -> reply 10.0.2.2 is at 52:55:0a:00:02:02
    ICMP echo request from 10.0.2.15 (id 0x1234 seq 1, 32 payload bytes) -> echo reply
  responder: 4 frame(s) in, replies arp=1 dhcp=2 icmp=1 ignored=0
  ```

  Exactly **four** frames reach the wire, and they are the guest's,
  byte for byte — the harness asserts the count and the two-run `cmp`:

  | # | frame (hex) | what it is |
  |---|---|---|
  | 1 | `ffffffffffff5254001234560800450001111234…638253633501013703010306ff` (287 B) | DISCOVER: `ip.ident 0x1234`, `udp 68→67 len 0x00fd csum 0`, `xid 12345678`, `flags 0x8000`, `chaddr 52:54:00:12:34:56`, cookie + `53=1`, `55=[1,3,6]`, END |
  | 2 | `ffffffffffff5254001234560800450001181235…6382536335010332040a00020f36040a000202ff` (294 B) | REQUEST: `ip.ident 0x1235`, `udp len 0x0104`, `53=3`, `50=10.0.2.15`, `54=10.0.2.2`, END |
  | 3 | `ffffffffffff525400123456080600010800060400015254001234560a00020f00ff000000000a000202…` (60 B) | ARP who-has `10.0.2.2` tell `10.0.2.15`, padded |
  | 4 | `52550a00020252540012345608004500003c00010000400162b00a00020f0a0002020800e0b512340001 4142…5f60` (74 B) | ICMP echo request, ident `0x1234` seq `1`, payload `0x41..0x60` |

- **Lane B (opt-in ground rule).** The same payload **without**
  `--tap=`: no PCI function exists, BAR0 reads hit the open bus and
  return all-ones — which means `STATUS.LU` *reads set*, so the payload
  proceeds into a TX ring nobody services and reports the guest's own
  timeout rather than hanging:

  ```
  [serial] [e1000] TX timeout: desc[0].status=0x00 TDH=4294967295 TDT=4294967295
  [serial] [dhcp] FAIL: DISCOVER transmit failed (link down or TX timeout)
  [serial] [net] FAIL: no DHCP lease
  [serial] N2FAIL
  ```

  (`4294967295` is `0xFFFFFFFF`, the open-bus read, printed with the
  guest's own `%u` formatting.) This is the pin that keeps every
  historical no-NIC lane byte-stable.

- **Lane C (frozen-guest receipts) — SKIP in this tree.** The DoD as the
  real guest prints it: `[net] using NIC: e1000`, `[dhcp] PASS: IP
  10.0.2.15, gateway 10.0.2.2`, `[net] PASS: ping 10.0.2.2 successful
  (ICMP echo reply received)`, and the absence of `[e1000] TX timeout`.
  As in N0/N1, the AuraLite-OS toolchain (`clang --target=x86_64-elf`,
  `ld.lld`, `rustc`, `mtools`) is not installable here, so the harness
  prints the reproduction line instead of failing:

  ```
  KERNEL=/home/user/AuraLite-OS/build/kernel.elf \
  INITRD=/home/user/AuraLite-OS/build/initrd.tar \
      tests/integration/n2_responder.sh
  ```

- **Lane D (negative control).** See NC 1 above; it runs every time.

Determinism: lane A run twice, serial tape + wire frames + responder log
all byte-identical (`cmp` clean; harness workdir `/tmp/n2-responder`).
The serial tape embeds the `--kernel=` path, so the shas below are for
the harness-built payload:

| Artifact | sha256 |
|---|---|
| `a1.log` = `a2.log` (lane A serial tape) | `d5ac2cb1ccdb76afa317675cdd45f94dbe2ae894349d7d5366af8a73a4e6e0fd` |
| `a1-frames.hex` = `a2-frames.hex` (the 4 wire frames) | `3b6de9b8de3ab3c424e337c722bf7450946594ff4b4fe98363b854c08255ed73` |
| `a1-resp.out` = `a2-resp.out` (responder log) | `2c0075bf2856b66dfa094eb5e44cb51a80b31524fe030e52c0fc30c355737a28` |
| `b1.log` (lane B serial tape) | `6f85467f92d284897acc93859a34fcd9a359577758d3bc9b7b18b602af4f2ca3` |
| `d1.log` (lane D / NC serial tape) | `d952b38fa0446abfb789231a1be432f49cc5bd4754453dbaabdcbe432a5ee09b` |
| `tests/nic_net.elf` (payload, 15968 B) | `f96d60069b94771fa7047f9001892f82ed20db3e60dae3dd796afd33bb47b01d` |

The wire-frame sha is stable across payload rebuilds and was already
`3b6de9b8…` before the `nic_poll` DD gate went in — the frames on the
wire did not change, only what the payload is willing to accept.

**Gates**: `make test` RC=0 (now including `test-tap-responder` and
`test-net-rx`; `test-net-tx` re-run green in the same invocation, and
the boot integration receipts untouched — `[usb-msc] READ(10) LBA=0
blocks=1`, `GUEST_OK`, `framebuffer marker OK: pixel[0]=0x00200000`);
`make test-sanitize` RC=0 (ASan+UBSan); `make fuzz FUZZ_PROGRAMS=256` —
`256 programs, 6144 instructions compared -- all states match`,
`crash invariant: 64 garbage streams, 33 clean faults, 2 halts, 0
host-level crashes`. No emulator source changed in N2, so the
`src/` tree is byte-identical to N1's (`git status --porcelain src/`
empty).

**Not in N2** (measured, deferred): the INTx line (**D4**), DNS and TCP
receipts (N3 — the responder answers no UDP/53 and no SYN), DHCP lease
state / renewal / `DECLINE` handling (the responder is stateless by
design), the gateway MAC measurement (**D7**), UDP checksums on DHCP
replies (off by default; `--udp-checksum` exists), ICMP other than echo
(no destination-unreachable, no TTL-exceeded), and any IPv6.

### N3 — End-to-end close-out — ✅ done (`patches/0049-NET-N3-net-e2e.patch`)

N2 stopped at the lease: the responder answered ARP, DHCP and ICMP and
nothing else, so `net_self_test()` passed and the two self-tests that
follow it in `kernel/kernel.c:428-430` had no peer:

```c
if (net_dhcp_ok) {
    net_self_test();            /* ping the gateway  -- N2 closed this */
    net_dns_self_test();        /* resolve 'example.com'               */
    tcp_x5_self_test();         /* 16 concurrent TCP connections       */
}
```

N3 closes both, on the same endpoint and with the same payload, so the
whole online path now runs on the real wire: **76 frames, byte-identical
across two runs**.

#### 3.1 What the guest actually asks for (measured, not assumed)

**DNS** (`kernel/net/dns.c`, `dns_parse.c`):

- the query leaves from **UDP port 12345** (`dns.h:19
  DNS_LOCAL_PORT`) to `10.0.2.3:53`, so the answer has to come back to
  *that* port — `dns.c:172` rejects anything else;
- `flags 0x0100` (RD set), `QDCOUNT=1`, QNAME lowercase and
  length-prefixed with a NUL, QTYPE=A, QCLASS=IN, `ip.ident = 2`, UDP
  checksum **0**;
- the id is `dns_next_id()` — **not deterministic**, so the responder
  echoes whatever id arrived rather than inventing one;
- the reply must set QR (`0x8000`), echo the id (`dns.c:177`), and must
  **not** set TC (`0x0200`) — a truncated answer sends the guest down
  the TCP retry path instead;
- `dns_parse_response` (`dns_parse.c:132`) is strict: `class == 1`,
  `type == 1`, `rdlen == 4`, owner name equal to the queried name (or a
  CNAME chain, taking the minimum TTL), pointer `0xC0` labels with a
  bounded hop budget.

The answer built from that is 45 bytes and goes through the guest's own
parser:

```
a5f1 8180 0001 0001 0000 0000 | 076578616d706c65 03636f6d 00 0001 0001 |
c00c 0001 0001 00000e10 0004 5db8d822
```

id echo · `0x8180` = QR+RD · 1 question 1 answer · QNAME `example.com`
· A/IN · compressed owner · A · IN · TTL 3600 (`0x0e10`) · rdlen 4 ·
`93.184.216.34` — the same address `dns.c:441` prints as
`[dns] PASS: example.com -> 93.184.216.34 (cache verified)`.

**DNS over TCP** (`dns.c:187 dns_wire_query_tcp`): a *fresh* id, the
RFC 1035 §4.2.2 two-byte length prefix, and the prefix and the message
sent as **two separate `tcp_send_h` calls**; the reply is accumulated
through non-blocking `tcp_recv_h` calls, so it may span segments and may
not arrive in the same segment as the ACK.

**TCP** (`kernel/net/tcp.c`, 1534 lines):

| Contract point | Value | Where |
|---|---|---|
| window we must advertise | `TCP_WINDOW = 64240` | tcp.c:44 |
| MSS the guest offers | `NETL3_V4_MSS = 1460` | netl3.h:35 |
| SYN options | MSS + SACK-permitted, 8 bytes → `hdr=28` | tcp.c:178 |
| handshake retries | `TCP_MAX_RETRIES = 3` × `TCP_RTO_TICKS = 20` | tcp.c:766 |
| RST during handshake | `-ECONNREFUSED`, printed as `[tcp] connection refused` | tcp.c:790 |
| receive filter | **4-tuple and header length only** — no seq/ack validation | tcp.c:290 |
| payload length | from `ip.total_length`, not the frame (the NIC pads to 60) | tcp.c:326 |
| `tcp_send` with data | `ACK|PSH` immediately, does not wait for an ACK | tcp.c:954 |
| close from ESTABLISHED | one `FIN|ACK` out; a single `FIN|ACK` back closes it in one RTT | tcp.c:1343 |
| concurrent connections | `TCP_MAX_CONNS = 16`, 17th is `-EMFILE` | tcp.h:53 |
| the X5 gate | 16 × `tcp_open(10.0.2.3, 53)` in sequence, **all held open**, then all closed | tcp.c:1496 |

`tcp_x5_self_test` is the one that constrains a responder: it opens all
16, keeps them open, and only then closes them — so the table has to
hold 16 live tuples at once, each with its own sequence numbers, and the
guest's ephemeral ports come from its 40000..64999 wheel with ISN
`0x1000 + h*0x100`. It also deliberately probes the 17th connection and
prints `[tcp-x5] PASS: 16 concurrent connections held; table full is
diagnosed, not fatal` either way.

**ARP** (`net.c:356 net_arp_resolve`): an **on-link** address is
resolved by ARPing *that address*; only an off-subnet destination goes
through the gateway. N2 answered who-has for the gateway alone, so the
first N3 wire run stopped at

```
[net] ARP request sent for 10.0.2.3
[dns] FAIL: could not resolve the server's MAC
```

That is defect **D8** in §1.4. The check the guest applies to a reply is
only length + ethertype + `opcode == 2` (it never compares the protocol
address), so one MAC for the whole subnet is what slirp does and what
this responder does.

#### 3.2 What N3 adds to `tools/tap_responder.py`

`tools/tap_responder.py` grew 485 → **1024 lines** (stdlib only, still
RC=0 under `make test-tap-responder`):

- **`handle_dns`** — UDP/53: parse the question, refuse anything that
  is not an A/IN query for a length-prefixed name, echo the id, answer
  from `--dns-ip` (default `93.184.216.34`) with `--dns-ttl` (default
  3600). The reply is unicast `gw-mac → guest-mac`, src `10.0.2.3:53` →
  dst the guest's own source port, `ip.ident` echoed, UDP checksum 0
  unless `--udp-checksum`.
- **`handle_tcp` + a connection table** — `TCP_ISN = 0x5A5A0000`
  (a *constant*, so the tape is deterministic; the guest never validates
  our ISN), `TCP_MAX_CONNS = 64` (16 is the guest's own bound, and the
  responder is a server — it must not refuse a 17th *client*), states
  `SYN_RCVD`/`ESTAB`/`LAST_ACK`. Behaviour, each pinned by a vector:
  SYN → SYN-ACK with MSS+SACK-permitted (`hdr=28`); a retransmitted SYN
  → a **byte-identical** SYN-ACK (the guest retransmits after an RTO and
  would otherwise see two different answers); the final bare ACK →
  **silence**; data → ACK, and `ACK|PSH` with the answer when the buffer
  completes; `FIN|ACK` → `FIN|ACK`; `RST` → tuple dropped, no reply; a
  segment for an unknown tuple → **no reply** (an RST would be wrong:
  `tcp_recv_segment_timeout` accepts any segment on a live 4-tuple, and
  the closer consumes whatever comes back); table full → `RST|ACK`
  acking `seq+1`.
  Inbound checksums are validated **only when the field is non-zero**,
  which is what the guest sends (`ip.ident = 2`, UDP/TCP checksums 0 on
  the query path) and what RFC 793 permits a receiver to skip.
- **`dns_over_tcp`** — the RFC 1035 carriage: accumulate into the
  connection's buffer, and when a complete `[len][message]` is present,
  answer with `[len][message]` in **one** segment. The answer
  acknowledges the data itself rather than being preceded by a bare ACK:
  two segments would spend two of the guest's RX descriptors on one
  round trip and the guest's non-blocking reader would simply see the
  ACK first. A length prefix outside 12..4096 drops the stream instead
  of trusting it.
- **on-link ARP** — answer who-has for the gateway **or** for any
  address in the guest's own `/24` (`on_subnet()`), never for the
  guest's own address, never off-subnet. The log line for a non-gateway
  probe gets the suffix `(on-subnet)`; the gateway line is
  byte-identical to N2's, so nothing N2 asserted moved.
- **`--no-tcp`** — the N3 NC lever, and the reason `--drop=dns` cannot
  substitute: with `--drop=dns` the handshakes still succeed and only
  the query hangs, so the connection table is never exercised.
  `--no-tcp` ignores every TCP segment and leaves ARP/DHCP/ICMP/DNS-UDP
  untouched.
- **TCP checksums are always computed.** `--udp-checksum` means UDP.
  That was defect **D9** in §1.4: the first implementation passed
  `compute_csum=self.udp_checksum` into `tcp_segment`, so with the
  default flags every FIN-ACK and RST went out with checksum 0 — legal
  for UDP, not for TCP (RFC 793 §3.1 makes it mandatory in IPv4), and
  `tcp.c:319` rejects the segment outright.

#### 3.3 The payload: `tests/nic_net.c` 552 → 1125 lines

The wire payload keeps N2's receipts **verbatim** — same DHCP xid, same
ARP, same ICMP payload check, same four frames first on the wire — and
then runs the two legs N2 could not: ARP for `10.0.2.3` with an ARP
cache, a DNS query over UDP checked against `dns_parse_response`, the
same query over the TCP carriage with the two-byte prefix framed exactly
as `dns_wire_query_tcp` frames it, and finally 16 concurrent connections
to `10.0.2.3:53` opened in sequence, held, and closed. Its summary
marker moved `N2OK`/`N2FAIL` → **`N3OK`/`N3FAIL`**; both harnesses were
updated in this patch (that is why N3 touches `n2_responder.sh`).

Two bugs were found by running it, and both are the *same* bug:

- **D10 — the odd-length checksum tail.** `checksum()` in the payload
  (and its transcription `g_checksum()` in `test_tap_responder.py`)
  added a trailing byte as `p[len-1]` instead of `p[len-1] << 8`. RFC
  1071 treats an odd tail as the high half of a word whose low half is
  zero, and the frozen guest gets this right (`net.c:181`). The bug was
  invisible while every checksummed length was even — IP header 20,
  ICMP echo 40 — and appeared the first time an odd-length segment
  crossed the wire: a **49-byte TCP segment** carrying a 29-byte DNS
  query, off by `0xFF00`, correctly refused by the responder as
  `TCP: bad checksum ignored`. Both copies are fixed and the fixed rule
  is now pinned twice: by the vector that builds the odd-length segment
  and by the wire tape that carries it.
- The responder's own `csum()` (`data += b"\x00"`) was already correct;
  the frame dump proved it — `verify=0x0000` on every even-length
  segment and `0xff00` on exactly the odd one.

#### 3.4 Receipts (measured)

`tests/integration/n3_net_e2e.sh` (new, **292** lines,
`make test-net-e2e`, wired into `make test`), workdir `/tmp/n3-net-e2e`.

**Lane A** (×2 determinism), all 76 frames and the responder summary
`replies arp=2 dhcp=2 icmp=1 dns=2 tcp=17 ignored=0`:

```
[serial] [dhcp] PASS: IP 10.0.2.15, gateway 10.0.2.2
[serial] [net] ARP reply: gateway is 52:55:0a:00:02:02
[serial] [net] ICMP payload verified (ident+seq+32 bytes)
[serial] [net] PASS: lease + ARP + echo reply (payload verified)
[serial] [net] ARP request sent for 10.0.2.3
[serial] [net] ARP reply: 10.0.2.3 is at 52:55:0a:00:02:02
[serial] [dns] cache MISS 'example.com' — querying
[serial] [dns] PASS: 'example.com' -> 93.184.216.34
[serial] [tcp] SYN options: 8 bytes, hdr=28, mss=1460 sack-perm
[serial] [tcp] ESTABLISHED (peer_wnd=64240)
[serial] [dns] PASS: 'example.com' -> 93.184.216.34 over TCP (45-byte message, prefix ok)
[serial] [tcp-x5] probing 16 concurrent TCP connections...
[serial] [tcp-x5] PASS: 16 concurrent connections held and closed cleanly
[serial] [net] PASS: DNS over UDP + DNS over TCP + 16 concurrent TCP connections
[serial] N3OK
```

Composition of the 76, counted off the tape: ARP 2 (gateway, DNS
server) + DHCP 2 + ICMP 1 + DNS/UDP 1 + **SYN 17** (1 for the carriage,
16 for X5) + **bare ACK 34** (two per connection: handshake and close) +
**FIN|ACK 17** (one per connection) + **ACK|PSH 2** (the TCP length
prefix and its answer).

**Lane B** (no `--tap=`) re-measures N2's opt-in pin on the longer
payload: `[e1000] TX timeout: desc[0].status=0x00 TDH=4294967295
TDT=4294967295`, `[dhcp] FAIL: DISCOVER transmit failed`, `N3FAIL`, and
no `[dns]`/`[tcp` line at all — the payload stops at the lease.

**Lane C** (frozen guest) SKIPs here because `AuraLite-OS` cannot be
built in this sandbox; it asserts the DoD strings the guest's own cases
assert — `[dns] PASS: example.com -> ` (dns.c:441),
`[tcp-x5] PASS: 16 concurrent connections held`, plus
`UDPTEST PASS: example.com A .* from 10\.0\.2\.3:53`
(`AuraLite-OS/tests/integration/cases/test_udp_sockets.sh:25`) at the
shell prompt. Reproduce with the KERNEL/INITRD env pair.

**Lane D (the phase's NC)** — `--no-tcp`, RC=1 from the harness, 10
frames (`N2's 4 + the DNS ARP + the DNS query + 4 SYNs`), responder
summary `tcp=0`:

```
[serial] [dns] PASS: 'example.com' -> 93.184.216.34
[serial] [tcp] SYN options: 8 bytes, hdr=28, mss=1460 sack-perm
[serial] [tcp] RTO waiting for SYN-ACK, retransmitting SYN   (×3)
[serial] [tcp] timeout waiting for SYN-ACK
[serial] [net] FAIL: no DNS answer over the TCP carriage
[serial] N3FAIL
```

The assert that matters is the **count**: exactly 3 retransmits, which
is `TCP_MAX_RETRIES` from `tcp.c:766` — the guest's own retry ladder,
not a harness constant.

Determinism: lane A run twice inside one harness invocation; serial
tape, wire frames and responder log all byte-identical (`cmp` clean).

**Which shas are workdir-independent, measured.** Every serial tape
embeds the payload path twice (`[kload] kernel=<workdir>/nic_net.elf`
and the `--kernel=` echo), so `a1.log`/`b1.log`/`d1.log` are functions
of the workdir and their shas below are for the run named in the table.
The **wire artifacts are not**: `a1-frames.hex`, `a1-resp.out`,
`d1-frames.hex`, `d1-resp.out` and `nic_net.elf` reproduced byte for
byte in two independent harness runs in *different* workdirs
(`/tmp/n3-run3` and `/tmp/n3-net-e2e`, the latter inside the §3.9
verification clone) — which is the stronger statement, since it means
the tape does not depend on where the harness ran. `unit.out` is
deliberately **not** listed: it differs between two invocations in the
*same* workdir, because Python's per-process hash randomisation orders
the vector labels; the harness asserts its content (`all tests passed`
plus one line per vector), never its sha.

| Artifact | sha256 (workdir `/tmp/n3-net-e2e`) |
|---|---|
| `a1.log` = `a2.log` (lane A serial tape) | `a7c5e18dc1bf882a150c320b8eea8cfc54567fbcfa8f1e3ad622bf8aa3b0adf1` |
| `a1-frames.hex` = `a2-frames.hex` (the 76 wire frames) | `c76a94ff9a0f17448ba33e25d1b24be4e88d7fd13e54458b7059af74b24480ef` |
| `a1-resp.out` = `a2-resp.out` (responder log) | `44193467f66ed7f95e0e03dff7e9e7802a79f4b839793093edd86e6e9e61526c` |
| `b1.log` (lane B serial tape) | `3f59c16c8dac5683ca0158cadc0f7c261a462e7ab1c4d7952f8c9dca79d36008` |
| `d1.log` (lane D / NC serial tape) | `7fc3aae85851ecac0f004e160850a22e178001ecfbcba0b792f5a24374341d00` |
| `d1-frames.hex` (the 10 NC frames) | `dad7156350e5b0d72a81b23a21db2ee60137e876a5f1366331ad328663ead968` |
| `d1-resp.out` (NC responder log) | `97bfcfc16cd5062b4789da10f743285a1b8d2fabcacb92c49d36383db3c8071b` |
| `tests/nic_net.elf` (payload, 23096 B) | `f5176d456c219828ee367f575447da0cc034ff9253fdfac526f4ee2366edb94a` |

The payload grew 15968 → 23096 B, so **N2's recorded shas no longer
reproduce**; N2's four frames are still the first four on the wire, byte
for byte, and `n2_responder.sh` now pins exactly that (it compares
`head -4` of the two tapes and leaves the total to this harness). Its
own receipts and its NC are unchanged.

#### 3.5 Protocol vectors: `tests/test_tap_responder.py` 481 → 1070 lines

9 → **16 vectors, 135 `ok` lines, RC=0**. The new ones, all built with
the guest's own builders and fed back through transcriptions of the
guest's own accept filters (`tcp_recv_segment_timeout`,
`dns_parse_response`, `net_arp_resolve`'s reply check):

| Vector | What it pins |
|---|---|
| `test_dns_query_answer` | the 45-byte answer survives `dns_parse_response` and lands on port 12345 with the echoed id |
| `test_dns_only_answers_what_was_asked` | no answer for AAAA/CNAME/other names/other ports; a malformed question is ignored, not guessed at |
| `test_dns_is_deterministic_and_droppable` | the same query twice → identical frames; `--drop=dns` → silence |
| `test_tcp_open_and_close` | `tcp_open_addr`'s handshake reaches ESTABLISHED, the final ACK is met with silence, `tcp_close` completes in one RTT, the tuple is freed |
| `test_tcp_sixteen_concurrent` | 16 tuples alive at once, each with its own ISN, all closed, table empty afterwards |
| `test_tcp_behaviours` | retransmitted SYN → identical SYN-ACK; unknown tuple → silence; RST → tuple dropped; table full → `RST|ACK` acking `seq+1`; `--no-tcp` → no SYN-ACK but ARP still answered |
| `test_dns_over_tcp` | prefix and query as two separate sends; exactly **one** segment back; prefix length == body length; same A record as UDP; the connection still closes after carrying data |
| `test_arp_for_the_whole_subnet` | who-has 10.0.2.2/.3/.99 answered, the guest's own address and off-subnet targets not |

Plus the existing `test_reply_goes_to_the_peer_not_to_the_endpoint`,
which is the vector that keeps the responder from ever answering a
broadcast DNS query back to the broadcast address.

#### 3.6 Harness defect: port contention (**D11**)

The first `make test-net-e2e` run produced a lane A that received **3
frames and answered none of them** (`replies arp=0 dhcp=0 icmp=0 dns=0
tcp=0`) while the very next lane was perfect. Cause, measured: a
responder left over from a hand run still held UDP 8080, and
`SO_REUSEADDR` lets two UDP sockets bind the *same* port — so the
datagrams were split between them and half the lane's replies went to a
process that was not recording. Both harnesses now refuse to start a
lane on a busy port (`port_free()` in `run_lane`) and print the remedy
instead of reporting a failure that is really the harness's own mess.

#### 3.7 Gates

`make test` **RC=0** — including the new `test-net-e2e` and a re-run of
`test-net-tx` and `test-net-rx` in the same invocation; `N1 TX wire
receipts: ALL PASS`, `N2 responder receipts: ALL PASS`, `N3 end-to-end
receipts: ALL PASS`, and the historical boot receipts untouched
(`[usb-msc] READ(10) LBA=0 blocks=1`, `GUEST_OK`, `framebuffer marker
OK: pixel[0]=0x00200000`). `make test-sanitize` **RC=0**
(ASan+UBSan). `make fuzz FUZZ_PROGRAMS=256` — `256 programs, 6144
instructions compared -- all states match`, `crash invariant: 64 garbage
streams, 33 clean faults, 2 halts, 0 host-level crashes`.

**No emulator source changed in N3**: `git status --porcelain src/` is
empty, so `src/e1000.c` is byte-identical to N1's and every historical
lane (K0–K8 firmware boot, the U5 tapes, the firmware contract GPU
0:2:0 `0x8086:0x0412` / BAR0 `0xD0000000` / marker `pixel[0]=0x00200000`)
is untouched by construction. N3 is host-side tooling, the wire payload
and its harnesses.

#### 3.8 Not in N3 (measured, deferred)

- **D4 — the INTx line.** Still bookkeeping-only. The guest stays on
  its polled path and every receipt above was produced by polling, so
  nothing in N3 needed it; wiring INTA# (cfg `0x3C`, `pci_set_irq`, the
  `(ICR & IMS) != 0` condition, EOI) remains its own phase because it
  touches `src/`.
- **D7 — the gateway MAC** is still taken from slirp's published
  `52:55:<ip-bytes>` scheme rather than measured against a real
  `bootp.c` (every mirror 404'd). `--gw-mac` pins it if that ever
  matters; no guest assert depends on the value beyond consistency.
- TCP beyond what the guest exercises: no retransmission timers of our
  own, no window scaling, no SACK *sending* (we only acknowledge the
  guest's SACK-permitted option), no out-of-order handling, no
  `TIME_WAIT` timer (the tuple is freed on the closing ACK), no IPv6,
  no `tcp6_self_test` peer (the guest skips it without one).
- DHCP lease state, renewal and `DECLINE` — the responder stays
  stateless by design, which is what makes two instances and a replayed
  frame answer identically.
- DNS beyond A/IN for a single name: no AAAA, no CNAME chains in our
  answers, no negative answers (NXDOMAIN), no cache, no EDNS0. The
  guest asks for A/IN only (`dns.c:96`) and treats anything else as a
  non-answer.

#### 3.9 The delivered patch, verified

`patches/0049-NET-N3-net-e2e.patch`, `git format-patch -1` of this
phase's commit, on top of the parent `6c2ee3d "N2 update"`. Its size,
line count and commit sha are deliberately **not** quoted here: all
three are properties of the artifact that *contains* this section, so
any number written down would be stale the moment the section is
amended -- and amending it is exactly what recording the number
provokes. `ls -l patches/0049-*.patch` and `git log -1` are the current
answer, and the same reasoning is why the check below is written as a
*procedure* rather than as "commit `abcdef0` verified":

```sh
git clone …/AuraLite-Emulator $CLONE && cd $CLONE
git checkout 6c2ee3d                              # the parent
git am  …/patches/0049-NET-N3-net-e2e.patch       # applies clean
git diff --quiet $REPO_HEAD HEAD                  # empty: the tree is the commit's
make test                                         # RC=0
```

Run against the delivered patch, that produced: `git am` clean,
**tree identical** to the phase commit, and `make test` **RC=0** with
`boot integration test: ok`, `test-tap-responder: all tests passed`,
`N2 responder receipts: ALL PASS`, `N3 end-to-end receipts: ALL PASS`
and exactly two SKIPs (below). It was run twice on two successive
amendments of this section, and the wire artifacts came out byte
identical both times — `a1-frames.hex`, `a1-resp.out`, `d1-frames.hex`,
`d1-resp.out` and `nic_net.elf` have now reproduced in **three**
independent harness runs across three different workdirs, which is the
evidence behind the workdir-independence claim in §3.4.

`make test` in that clone reported `boot integration test: ok`,
`test-tap-responder: all tests passed`, `N2 responder receipts: ALL
PASS`, `N3 end-to-end receipts: ALL PASS`, and SKIPped exactly two
things: the frozen-guest lanes (no `AuraLite-OS` build in this sandbox)
and `test-net-tx` with `SKIP: nasm missing`.

That last SKIP is a **sandbox limitation, not a patch property**, and it
is worth recording because it shapes how any clone is verified here:
`firmware/*.bin` and `disk/*.{bin,img}` are `.gitignore`d build products
of `nasm` (`Makefile:20-27`), and `nasm` is not installed in this
sandbox — so a fresh clone cannot build the firmware-boot artifacts and
`make test` dies on `Makefile:21` before any NET code runs. Seeding the
clone is therefore part of the recipe, and it took three artifacts, not
one (each omission was measured as its own failure):

```sh
cp $REPO/firmware/firmware.bin  $CLONE/firmware/   # 131072 B, sha256 00a32837c2709431…
cp $REPO/disk/test_kernel.bin   $CLONE/disk/       #   4096 B -- Makefile:24 wants it
cp $REPO/disk/disk.img          $CLONE/disk/       # 1048576 B
touch -d 2019-01-01 $CLONE/firmware/sample_firmware.nas \
                    $CLONE/disk/test_kernel.asm $CLONE/Makefile
```

The `touch` is not cosmetic, and getting it wrong is *destructive*
rather than merely noisy. Two measurements:

- `git checkout` gives every source file the checkout time, so a
  copied-in binary looks **older** than `sample_firmware.nas` and make
  rebuilds it through the missing `nasm`.
- That failed rebuild then **deletes the artifact**: the Makefile sets
  `.DELETE_ON_ERROR:` (line 9), which is correct make hygiene and
  normally invisible, but here it means a `nasm` failure removes the
  only copy of `firmware/firmware.bin` in the tree. Measured in the
  working repository itself, not just in a clone: one `touch` of
  `firmware/sample_firmware.nas` followed by any `make` target that
  depends on the firmware cost us `firmware.bin`, and `make clean` was
  nowhere near it (`clean:` removes `x86emu`, the `test-*` binaries,
  `framebuffer.ppm` and `tests/kload_fixture.elf` — **not** the
  firmware or the disk image).

So the ordering has to be *sources older than artifacts*, and it is
worth avoiding a future-dated artifact too: dating the binaries into the
future buys the same `up to date` verdict but make then prints
`Clock skew detected. Your build may be incomplete.` on every
invocation. The arrangement used for the measurements in this section is
sources at 2019-01-01, artifacts at 2020-01-01, `Makefile` at
2021-01-01 — all in the past, correctly ordered, no warning. On a
machine that has `nasm` none of this is needed: make builds all three
itself.

The NET lanes N3 adds do not need the firmware at all: they ride the
`--kernel=` load path, not the firmware-boot one, which is why
`test-tap-responder`, `test-net-rx` and `test-net-e2e` all went green in
that clone on their own merits.

`git am` also reports `50 lines add whitespace errors` for this patch.
Every one of them is a line **inside the nested
`patches/0048-NET-N2-tap-responder.patch`** that this commit adds (the
house convention is that a phase's commit carries the previous phase's
patch, and 0048 itself contains markdown with trailing spaces). No file
authored in N3 has a trailing-whitespace line:
`grep -cP "[ \t]+$"` is `0` for `tools/tap_responder.py`,
`tests/test_tap_responder.py`, `tests/nic_net.c`,
`tests/integration/n2_responder.sh`,
`tests/integration/n3_net_e2e.sh`, `Makefile`, `CHANGELOG.md`,
`README.md`, `ROADMAP.md`, `docs/STATUS.md` and this plan.

## 3. Standing constraints

- The e1000 exists **only** under `--tap=`; historical lanes (K0–K8
  firmware boot, U5 tapes) stay byte-identical (ground rule 3).
- The firmware line and the guest-visible firmware contract do not move
  (GPU 0:2:0 `0x8086:0x0412`, BAR0 `0xD0000000`, marker `0x00200000`).
- One `.patch` per phase; the fix and its regression vector ship in the
  same patch; one NC per phase; the docs of a phase ship in the phase's
  own patch.
