# Implementation status

## Working now

- The sample 128 KiB firmware image can be loaded at the x86 reset vector.
- Reset execution supports real mode, protected mode, compatibility mode and long mode.
- All five selectable Intel-style platform profiles are available.
- The EHCI model reaches virtual USB Mass Storage transfers when the loaded firmware
  requests them, and since USB U3 the register file is honest enough for the guest
  driver to enumerate: capability registers (CAPLENGTH/HCIVERSION 2.00/HCSPARAMS/
  HCCPARAMS with the measured firmware window quirks), USBSTS/USBCMD halt handshake
  with HCRESET, PORTSC power/reset/enable/owner with CSC latching, the async schedule
  walked on USBCMD writes plus a deterministic instruction-cadence sampler, and SETUP
  qTDs running real control transfers (device/config/string descriptors, SET_ADDRESS,
  SET_CONFIGURATION, GET_STATUS, GET_MAX_LUN) — measured `1 high-speed device(s)
  ready` and `[msc] mass storage candidate` receipts against `AuraLite-OS@0ed0d29`.
- The UHCI companion (USB U4) is a first-class root host: PCI 0:1.2 `8086:0x7020`,
  I/O BAR4 `0xC040`, the guest's USBSTS.HCHALTED map (bit 5), frame list + QH/TD
  walking on USBCMD.RUN (doorbell) and the cadence sampler, TD completion with the
  `ctrl[10:0] = bytes-1` actual-length contract and IOC→USBINT, control+bulk through
  the shared device model — the stick enumerates as a second, full-speed device and
  the MSC binds the UHCI instance (`[uhci] 1 device(s) ready`,
  `[usbfs] device available at /usb` over 64-byte bulk chunks).
- USB end-to-end close-out (U5): `--no-usb-uhci` reproduces the guest's
  `test_usb_ehci.sh` EHCI-only machine shape; both sticks (UHCI companion
  AURALUSB + EHCI high-speed AURALEHC) run the `/usb` info + typed sector-read
  tokens through the guest's MSC path with byte-identical x2 lane pairs; the AHCI
  matrix stays green with USB present (`MATRIX_DETERMINISM=1`, all pairs
  byte-identical) and its AHCI/fs receipt lines are byte-identical to the S5-era
  lane logs. The USB plan (U0–U5) is closed.
- The VIDEO plan (V0–V3, plus the post-closure fix V4) is closed.  VGA text
  mode 3 is a real display now
  (80x25 cells at `0xB8000`, the CRTC `0x3D4/0x3D5` cursor pair, 8x16 glyphs
  from the guest's own PSF2 font -- 256/256 byte-identical to
  `psf2_default_font.inc`, `--dump-text`/`--no-fb` measured on the guest's
  `fb.c` fallback receipts), the Bochs VBE dispi (`0x1CE/0x1CF`) follows the
  QEMU `vga.c` contract (ID `0xB0C0..0xB0C5` accept-list, XRES `%8`, BPP
  set, GETCAPS, `VIDEO_MEMORY_64K=29`, BANK mask 28, ENABLE edge display
  flip + `VIRT_*` fixup), and mode switching is one machine state (text 3 ↔
  VBE graphics: the dispi ENABLE edge, the BAR0/kload publishes, and the GC
  `VGA_GR06_GRAPHICS_MODE` bit at `0x3CE/0x3CF` -- stage2's INT 0x10 (AL=3)
  lands in mode 3 on the hardware side; mode-set clears `YRES * vbe_line_offset`
  unless `DISPI_NOCLEARMEM`).  `tests/test_video.c` runs 10 vectors; the
  V3 mode-switch + V1 guest lanes (`tests/integration/test_video_modes.sh`,
  `--vbe=WxHxBPP`) run twice each with six byte-identical artifact pairs and
  display assertions per mode; historical firmware/K0–K8 receipts do not
  move and `make test` + `make test-sanitize` stay green.
- **V4 fixed a host heap overflow in that mode-set** (defect **D6**, found
  by ASan after the plan was closed): `dispi_fixup` was a *partial*
  transcription of QEMU's `vbe_fixup_regs` — it raised `VIRT_WIDTH` to
  `XRES` and derived `VIRT_HEIGHT` but never clamped `YRES`, which is the
  one adjustment that bounds the clear-to-screen, so the `memset` ran on
  the guest's own height out of a window sized to the published 800×600
  framebuffer (1920000 B).  Measured on the built emulator with no guest
  involved: `--vbe=1024x768x32` → ASan RC=1, `heap-buffer-overflow WRITE
  of size 3145728 at dispi_data_write, src/devices.c:1055`, unsanitized
  RC=134 `free(): invalid pointer`; `--vbe=1600x1200x32` → WRITE of size
  7680000; `--vbe=16000x12000x32` → WRITE of size 768000000, RC=139;
  `--vbe=800x600x32` → RC=0.  Same class as QEMU's CVE-2016-3712.  The
  fix transcribes the whole QEMU function in QEMU's order (depth switch
  with the default arm assigning 8, the three width clamps, `maxy =
  vbe_size / linelength`, `YRES` → 1/MAX/`maxy`, the three-rung offset
  ladder, `VIRT_HEIGHT = maxy`), moves the ENABLE arm to QEMU's shape
  (fixup + `VGA_GFX_MISC` + display flip + clear all inside the 0→1 edge,
  `bank_offset = 0` in the else), and bounds the clear with an explicit
  `min(clear, vram)` as belt-and-braces.  Two divergences are recorded
  rather than hidden: no `vbe_start_addr` (this model renders from `m->fb`
  at offset 0, so the ladder's result has no consumer — the clamps still
  run, being observable through readbacks), and BPP 4 (QEMU's write
  handler rejects it so its `case 4:` is dead code; ours has accepted it
  since V2, and following QEMU's expression literally into a zero
  linelength would divide by zero in `maxy`).
  `test_dispi_mode_clamped_to_vram` is the regression vector (10 → **11
  vectors**, RC=0 plain and under ASan+UBSan), with every expected number
  computed from QEMU's arithmetic first: 1024×768×32 → YRES **468**,
  16000×12000×32 → **30** (clear 1920000, exactly the window), 800×600×32
  → **600** (also exactly the window — the documented mode is untouched),
  YRES 0 → 1, XRES 0 → 8 with VIRT_HEIGHT 60000, BPP 33 → 8, VIRT_WIDTH
  65535 → 16000, VIRT_WIDTH 9 → 800, plus each rung of the offset ladder
  in the mode where it is reachable (rung 2 needs `linelength ≥ 160`
  precisely because Y_OFFSET is clamped to 12000 first, so it uses
  40×1×32 and is *unreachable* at 8×1×8 or 8×1×32).  The NC is the pre-fix
  `src/devices.c` with that vector compiled against it: **RC=1**, same
  `WRITE of size 3145728`, aborting before the first readback.  A one-off
  sweep backs it up — **5236 mode combinations** and **146672 register
  pokes** under ASan+UBSan asserting `YRES * line_offset <= VRAM` with
  QEMU's own expression: 0 violations post-fix, RC=1 on the first
  combination pre-fix.  Historical video artifacts were proved stable
  rather than assumed: `test_video_modes.sh` SKIPs here (no frozen
  `AuraLite-OS` build), so pre-fix and post-fix binaries were run side by
  side on four lanes — firmware boot, `--vbe=800x600x32`, `--vbe=off`,
  `--no-fb` — and all `--log`/`--dump-fb` pairs are **byte-identical**,
  with the `--no-fb` framebuffer coming out at `c3b3d73f6bbb…`, the sha
  V3 recorded.  All ten pre-existing vectors pass unmodified.  `make test`
  (incl. `boot integration test: ok` and the NET N2/N3 receipts, now green
  against the changed `src/`), `make test-sanitize` and `make fuzz
  FUZZ_PROGRAMS=256` stay green.  Deferred: display panning /
  `vbe_start_addr`, QEMU's `vbe_update_vgaregs` re-assertion (this model's
  `VGA_GFX_MISC` path consumes no sizing register, so the CVE-2016-3712
  surface does not exist here), the BPP-4 divergence, and growing the VRAM
  window to QEMU's 16 MiB (it would move the guest-visible firmware
  contract and would not remove the need for the clamp).
- The NET plan's N0 (RX-ready e1000 + TAP ingress) is measured.  The
  NIC is opt-in (`--tap=` only — without it no PCI function exists and
  the historical no-NIC receipts stay byte-stable): PCI 0:4.0
  `8086:100E` with BAR0 `0xFEBC0000`/128 KiB seeded into config (the
  N0 lane measured the pre-fix defect `BAR0 at phys 0x0` /
  `STATUS=0x00000000 (LU=0)` / `using default QEMU MAC` first), the
  QEMU `e1000_reset` core (RXCSUM 0, `RAL/RAH` preload
  `52:54:00:12:34:56`, `STATUS.LU`, `CTRL.RST` self-clear), and RX
  ring DMA with `RCTL.EN`/`RDH==RDT` drop gates; the TAP socket (UDP
  datagrams = Ethernet frames at `127.0.0.1:8080`, the emulator
  connects) drains up to 8 datagrams/tick into the guest RX ring.
  `tests/test_net.c` runs 2 vectors (incl. the BAR0 readback
  regression vector over `0xCF8/0xCFC`); the guest lane pair
  (`--max-instr=4500000000`) is byte-identical (both tapes sha256
  `82614cd6f896aa9a…`) with the `[net] using NIC: e1000` gate,
  `BAR0 at phys 0xfebc0000`, `STATUS=0x00000002 (LU=1)` and
  `MAC from RAL/RAH`.  NC reddens the ingress vector (`test_net.c:85`,
  RC=134); `make test` + `make test-sanitize` stay green.
- The NET plan's N1 (TX-ready) is measured.  The TDT/TCTL doorbell walks
  TDH over the legacy TX ring, the frame leaves as one TAP datagram
  (`TCTL.EN` gated) and the descriptors are completed **inside the
  doorbell store** — because the guest's `e1000_send()` spins on
  `status & 0x01` with a 100000-PAUSE budget, a tick-deferred completion
  would still burn it.  Causes land in ICR as `TXQE|TXDW` (`0x81` for a
  single-descriptor send); the register-write fallback gained QEMU
  `macreg_writeops[]` parity (`IMS |=`, `ICR &= ~val`, `ICS`, 16-bit
  heads, `TDLEN/RDLEN & 0xfff80`).  `tests/test_net.c` runs 7 vectors
  (2 N0 + 5 N1).  The wire lane (`make test-net-tx` →
  `tests/integration/n1_tx_tap.sh`) puts a 489-byte payload
  (`tests/nic_tx.asm`) on the **real firmware-boot transport** off a
  throwaway `--disk=` image and holds the ground-rule endpoint with
  `n1_tap_responder.py`: twice, byte-identical, the guest prints
  `N1TXOK  status=01 tdh=01 icr=81` and the wire carries
  `ffffffffffff52540012345608004e3154584f4b` (20 bytes in, 20 bytes
  out).  The same payload without `--tap=` reports
  `N1TXFAIL  status=00 tdh=ff icr=ff` (open-bus BAR0) — the opt-in pin.
  The frozen-guest lane SKIPs here (the AuraLite-OS toolchain is not
  installable in this tree) and prints its reproduction line instead.
  NC removes the descriptor writeback: `test_net.c:167`, RC=134, and the
  wire receipt flips to `N1TXFAIL status=00 tdh=01 icr=80`.  `make test`
  (now incl. `test-net-tx`), `make test-sanitize` and the hand-run
  sanitized wire lane stay green.  Deliberately not modelled yet: the
  INTx line (no PCI cfg `0x3C`, no ack path — a raised line would strand
  the 8259A ISR bit under a polling guest), TX statistics counters,
  context/TSO descriptors, VLAN and `CMD.IFCS` FCS insertion.
- The NET plan's N2 (host responder) is measured — and it needed **no
  emulator source change at all** (`git status --porcelain src/` empty),
  so every historical lane is byte-stable by construction.
  `tools/tap_responder.py` holds the ground-rule endpoint
  `UDP 127.0.0.1:8080` and answers DHCP (DISCOVER→OFFER,
  REQUEST→ACK), ARP who-has for the gateway and ICMP echo, with slirp's
  user-mode addressing read out of the guest's own source
  (`kernel/net/net.c:39-47`: guest `10.0.2.15`, gateway `10.0.2.2`, DNS
  `10.0.2.3`).  It is stateless — no lease table, no ARP cache, no
  sequence numbers — so two instances and a replayed frame answer
  identically.  `tests/test_tap_responder.py` runs 9 vectors
  (`make test-tap-responder`) that build requests with a second,
  independent transcription of `net.c`'s builders and feed the replies
  back through `net.c`'s own accept filters.  The wire lane
  (`make test-net-rx` → `tests/integration/n2_responder.sh`) runs
  `tests/nic_net.c` — a freestanding x86_64 guest stand-in on the **K1
  kernel-load transport**, chosen over N1's firmware-boot one because
  that delivers a single 512-byte block and maps only
  `0x100000..0x1FFFFF` (`PD[0] = 0x19B`), too little for DHCP+ARP+ICMP
  plus a 64-deep RX ring.  Twice, byte-identical (serial tape
  `d5ac2cb1ccdb76af…`, the four wire frames `3b6de9b8de3ab3c4…`,
  responder log `2c0075bf2856b66d…`): `[dhcp] PASS: IP 10.0.2.15,
  gateway 10.0.2.2`, `[net] ARP reply: gateway is 52:55:0a:00:02:02`,
  `[net] ICMP payload verified (ident+seq+32 bytes)`, `[net] PASS:
  lease + ARP + echo reply (payload verified)`, `N2OK`, and
  `responder: 4 frame(s) in, replies arp=1 dhcp=2 icmp=1 ignored=0`.
  The same payload without `--tap=` reports the guest's own timeout
  rather than hanging (`[e1000] TX timeout: desc[0].status=0x00
  TDH=4294967295 TDT=4294967295` → `N2FAIL`), because an open-bus BAR0
  reads all-ones and `STATUS.LU` therefore *reads set*.  NC is measured
  on both sides: `--drop=arp,icmp,dhcp` (lane D, runs every time) gives
  `replies arp=0 dhcp=0 icmp=0`, exactly one wire frame and
  `[dhcp] FAIL: no OFFER received` / `N2FAIL`; removing the RX
  descriptor **length** writeback (`src/e1000.c:372`) by hand gives the
  same failure while the responder still logs a full exchange.  The
  phase's most useful measurement is what is *not* a control: removing
  the **DD** writeback (`src/e1000.c:373`) leaves the run green, because
  `e1000_hw_rx_drain` gates on `pkt_len > 0` and never inspects DD — the
  payload now requires DD anyway (strictly tighter than the guest) and
  says so in a comment.  New ledger entry **D7**: the gateway MAC
  follows slirp's `52:55:` + IP-bytes convention but could not be
  measured (libslirp unreachable from this sandbox, four mirrors tried),
  gates nothing, and is pinned behind `--gw-mac`.  `make test` (now
  incl. `test-tap-responder` + `test-net-rx`, with `test-net-tx`
  re-green), `make test-sanitize` and `make fuzz FUZZ_PROGRAMS=256`
  (`6144 instructions compared -- all states match`, `0 host-level
  crashes`) stay green.  Still not modelled: the INTx line (**D4**),
  DNS/TCP receipts (N3), DHCP lease state and renewal, ICMP other than
  echo, IPv6.
- The NET plan's N3 (end-to-end close-out) is measured — and, like N2,
  it needed **no emulator source change at all**
  (`git status --porcelain src/` empty), so `src/e1000.c` is
  byte-identical to N1's and the plan is now **closed: N0–N3 ✅**.
  `tools/tap_responder.py` (485 → 1024 lines) answers the two
  self-tests that follow the ping in `kernel/kernel.c:428-430`:
  `net_dns_self_test()` and `tcp_x5_self_test()`.  DNS arrives on
  **UDP/53** from the guest's fixed local port 12345 (`dns.h:19`) and
  is answered with a 45-byte A record for `93.184.216.34`, TTL 3600,
  the id echoed (the guest's `dns_next_id()` is not deterministic) and
  TC clear — TC set would push the guest down the TCP retry path.  The
  same answer rides the **RFC 1035 §4.2.2 TCP carriage** with the
  two-byte length prefix, framed the way `dns.c:187` frames it: prefix
  and message as two separate sends, one segment back, and that segment
  acknowledges the data instead of being preceded by a bare ACK.  TCP is
  a real connection table (`TCP_ISN = 0x5A5A0000`, a constant so the
  tape is deterministic; `TCP_MAX_CONNS = 64`, because 16 is the
  *guest's* bound and a server must not refuse a 17th client):
  SYN → SYN-ACK with MSS 1460 + SACK-permitted (`hdr=28`), a
  retransmitted SYN → a byte-identical SYN-ACK, the final bare ACK →
  silence, `FIN|ACK` → `FIN|ACK`, `RST` → tuple dropped, an unknown
  tuple → **no reply** (an RST would be wrong — `tcp.c:290` accepts any
  segment on a live 4-tuple), table full → `RST|ACK` acking `seq+1`.
  ARP now answers who-has for **any on-link address**, not just the
  gateway, because `net_arp_resolve` (`net.c:356`) ARPs the target for a
  local address and only routes off-subnet traffic through the gateway —
  with the gateway log line byte-identical to N2's and the suffix
  `(on-subnet)` on the rest.  `tests/test_tap_responder.py` grows 9 →
  **16 vectors, 135 `ok`, RC=0**, each reply fed back through a
  transcription of the guest's own accept filter
  (`tcp_recv_segment_timeout`, `dns_parse_response`, the ARP reply
  check).  `tests/nic_net.c` grows 552 → 1125 lines and now runs the
  whole online path on the wire, printing N2's receipts verbatim first;
  its marker moved `N2OK`/`N2FAIL` → `N3OK`/`N3FAIL`, which is why N3
  also touches `n2_responder.sh` (that harness now pins N2's four frames
  as `head -4` of the tape and leaves the total to N3's).
  `tests/integration/n3_net_e2e.sh` (new, 292 lines,
  `make test-net-e2e`, wired into `make test`) runs four lanes:
  **A** ×2 — **76 frames** on the wire, byte-identical between runs
  (frames `c76a94ff9a0f1744…`, responder log `44193467f66ed7f9…`,
  payload `f5176d456c219828…` — all three reproduced byte for byte in a
  *second* harness run in a different workdir, inside the verification
  clone; the serial tapes are byte-identical within a run but embed the
  payload path, so their shas are workdir-dependent and are listed in
  `docs/plans/NET_PLAN.md` §3.4), receipts `[dns] PASS:
  'example.com' -> 93.184.216.34`, `… over TCP (45-byte message, prefix
  ok)`, `[tcp-x5] PASS: 16 concurrent connections held and closed
  cleanly`, `[net] PASS: DNS over UDP + DNS over TCP + 16 concurrent TCP
  connections`, `N3OK`, and
  `replies arp=2 dhcp=2 icmp=1 dns=2 tcp=17 ignored=0` — the 76 count as
  ARP 2 + DHCP 2 + ICMP 1 + DNS/UDP 1 + SYN 17 + bare ACK 34 + FIN|ACK
  17 + ACK|PSH 2; **B** — no `--tap=`, still the guest's own
  `[e1000] TX timeout` → `N3FAIL` and not one `[dns]`/`[tcp` line;
  **C** — the frozen-guest DoD strings, SKIP here (`AuraLite-OS` does
  not build in this sandbox); **D**, the phase's NC — `--no-tcp`,
  harness RC=1, **10 frames**, `tcp=0`, and exactly **3** `[tcp] RTO
  waiting for SYN-ACK, retransmitting SYN` lines, which is
  `TCP_MAX_RETRIES` from `tcp.c:766` counted off the guest's own ladder
  rather than a harness constant.  `--drop=dns` is explicitly *not* a
  substitute for that lever: it leaves the handshakes succeeding and
  hangs the query instead of exercising the table.  Four new ledger
  entries, all measured: **D8** (ARP blind to non-gateway addresses —
  receipt `[dns] FAIL: could not resolve the server's MAC`), **D9**
  (TCP checksums were gated on `--udp-checksum`, so FIN-ACK/RST went out
  with checksum 0, which RFC 793 forbids and `tcp.c:319` drops),
  **D10** (the odd-length checksum tail: the payload's `checksum()` and
  the vectors' `g_checksum()` added `p[len-1]` instead of
  `p[len-1] << 8`; invisible while every length was even, and it showed
  up as a sum off by `0xFF00` on the first odd segment — 49 bytes
  carrying a 29-byte DNS query — which the responder correctly refused),
  and **D11** (`SO_REUSEADDR` lets two UDP sockets bind the same port,
  so a leftover responder silently split a lane's datagrams:
  `3 frame(s) in, replies … tcp=0` while the next lane was perfect; both
  harnesses now refuse to start a lane on a busy port).  `make test`,
  `make test-sanitize` and `make fuzz FUZZ_PROGRAMS=256` stay green.
  Still not modelled: the INTx line (**D4**), the measured gateway MAC
  (**D7**), TCP retransmission timers of our own / window scaling /
  SACK sending / `TIME_WAIT`, DHCP lease state and renewal, DNS beyond
  A/IN for one name (no AAAA, no NXDOMAIN, no cache, no EDNS0), IPv6.
- The physical address decoder handles overlapping ECAM and fixed BAR regions by choosing the most-specific region.
- A framebuffer is available as `framebuffer.ppm`.
- The CPU, memory, PCI and device models do not depend on a particular firmware name or binary layout.
- A PC-compatible dual 8259A PIC (ports 0x20/0x21/0xA0/0xA1) delivers maskable hardware
  interrupts at instruction boundaries: ICW1-4 init, OCW1 mask, OCW2 EOI, OCW3 IRR/ISR
  readback, slave cascade through master IRQ2, STI shadow and HLT wake semantics (H0),
  plus fixed/rotating priorities with all OCW2 rotation commands, fully-nested
  in-service gating, special mask mode, spurious IRQ7/IRQ15 vectors, the poll command,
  level-triggered lines with re-assertion, and AEOI (H1). Device-side line APIs:
  `pic_raise_irq()` (edge strobe) and `pic_set_irq()` (level hold).
- An 8254 PIT (ports 0x40-0x43, port B 0x61) runs on virtual time derived from
  `instr_count` (1193182 Hz nominal at ~99.4 virtual MIPS, so all cadence is
  deterministic): counter 0 modes 0/2/3 raise IRQ0 to the PIC, counter 2 is gated
  by 0x61 bit0 and its square-wave output is readable at 0x61 bit5; latch and
  LSB/MSB access-phase semantics included (CHIPSET H2).
- An MC146818A RTC/CMOS (ports 0x70/0x71 -- previously floating sinks,
  measured) keeps BCD clock registers on the same deterministic virtual
  timebase (epoch pinned), models the update-in-progress/commit phases
  exactly, fires the update-ended IRQ8 through the slave PIC once per
  virtual second when UIE=1, and serves the CMOS bytes BIOS-era firmware
  reads: equipment 0x14, base/ext memory KB at 0x15-0x18 and 0x30/0x31
  (16-bit capped), BCD century at 0x32, checksum area pinned zero
  (CHIPSET H3).
- An 8042 keyboard controller (ports 0x60/0x64) models the status
  register, command byte and the 0x64 command subset (read/write cmd
  byte, self-test 0xAA -> 0x55, interface test, 0xAD/0xAE inhibit,
  output port 0xD0/0xD1 -- bit1 = A20 gate, bit0 = SRST# --, 0xFE
  CPU-reset pulses). Scancode set-1 injection -- API or CLI
  `--keys=1E,9E,...` -- drives IRQ1 as a level of (OBF and cmd bit0),
  exactly like the 8042's line (CHIPSET H4).
- The A20 gate is a real bus effect (CHIPSET H5): with the gate closed,
  physical address bit 20 is forced to zero for reads, writes AND
  fetches, so every odd megabyte aliases onto the even one below (the
  1MB wrap). Effective line = (8042 output-port bit1) OR (port 0x92
  bit1); measured firmware behavior pinned the power-on state to OPEN
  (like Bochs/QEMU). System reset is a full warm reset consumed at the
  next instruction boundary, from four sources: port 0x92 bit0 edge,
  port 0xCF9 (SYS_RST rising with RST_CPU set, i.e. the canonical
  0x06/0x0E or Linux 0x02-then-0x06 dance), KBC 0xFE pulses, and the
  KBC output-port bit0 falling edge. CPU back to the reset vector,
  PIC/PIT/KBC/A20 re-initialized, RAM and the battery-backed RTC ride
  through (CHIPSET H5).
- A local APIC (xAPIC, MMIO at 0xFEE00000) serves the single vCPU
  (CHIPSET H6): ID/VERSION/TPR/computed-PPR/LDR/DFR/ESR/EOI, SVR
  software-enable gating delivery (latching continues while disabled),
  full 256-bit IRR/ISR with class arbitration, ICR fixed self-IPI
  (shorthand matrix on one vCPU), and the LAPIC timer -- one-shot and
  periodic, DCR divides, clocked by the deterministic virtual TSC,
  edge-lost-when-masked. IA32_APIC_BASE resets to 0xFEE00900. The 8259
  path stays the fallback whenever the LAPIC has nothing deliverable
  (plan D7). CPU side gained 64-bit IDT-gate IST stack switching
  through the TSS plus LTR/STR (the H6 long-mode vector measured why:
  delivery onto a register-window RSP corrupts it).
- An 82093AA I/O APIC sits at 0xFEC00000 (CHIPSET H7): IOREGSEL/IOWIN
  indirection, 24-entry 64-bit redirection table (vector, delivery/dest
  modes stored, polarity/trigger/mask live). Every ISA line reaches the
  same-numbered INTIN pin through the pic_* fan-out (the board wire),
  so the 8254's IRQ0 strobe and the 8042's held IRQ1 appear as edges
  and levels per entry programming. Edge entries latch masked edges
  until unmask; level entries carry remote_IRR with LAPIC-EOI feedback,
  redelivering held lines after each EOI. Fixed delivery targets the
  single vCPU's LAPIC; the PAIR-TEST vectors the full
  8254 -> IOAPIC -> LAPIC -> CPU chain with the 8259 pair masked out.
- The `--kernel=path` lane boots an AuraLite-OS `kernel.elf` directly,
  no firmware in between (KERNEL-BOOT K1): ELF64 segments placed at
  physical (p_paddr - kernel VMA), a seven-page paging hierarchy the
  kernel's boot contract demands (identity + HHDM of the low 4 GiB as
  shared 1 GiB pages, kernel VMA as 4 KiB pages), a fabricated
  `boot_info_t` handoff (magic-pinned ABI mirror, framebuffer window,
  E820-style memmap, HHDM offset, one BSP), and the CPU parked at
  `_start` in long mode per the measured contract. The run log marks
  `kmain` reached (18 instructions in, measured on AuraLite-OS @
  0ed0d29); the measured frontier and the next phases live in
  `docs/plans/KERNEL_BOOT_PLAN.md`.

- The K4 lane brings ring-3 userspace up: `--initrd=` publishes a USTAR
  rootfs through `boot_info_t`, the kernel mounts it, /hello runs to
  exit(0), the execve argv/envp self-test passes, and the interactive
  shell banner prints on COM1. The path forced four measured CPU-core
  fixes: descriptor-table reads (GDT/IDT inside SYSCALL/exception
  delivery) are implicit supervisor accesses; the IDT gate IST byte is
  fetched through the linear IDTR base; ISTn slots live at
  TSS+0x24+(n-1)*8; WRMSR IA32_FS_BASE keeps the FS shadow in sync so
  userspace TLS (`%fs:0` errno) works. New test_ring3 suite pins every
  one of them (verified red without each fix).

- COM1 is a 16550 subset register file (CHIPSET K4): LSR reports
  TX-idle/RX-empty, RBR is silent, IER/LCR/MCR/SCR store, IIR says
  "no pending", DLAB detaches THR/RBR onto the divisor latch. The
  K4-measured consequence of the old one-port model was an infinite
  space storm into the guest shell's stdin. Scancode injection can be
  scheduled by instruction count: `--keys-at=N` (the PS/2 driver's
  boot-time drain consumes anything queued before its init).

- The K5 lane meters the kernel's SMP bring-up against the emulator's
  single vCPU: `--cpus=N` publishes N CPUs in `boot_info_t` and the
  fabricated MADT (`acpi_build_table_set_smp`), so smp_init() runs its
  real path — LAPIC-timer calibration against the PIT (measured
  1317458400 Hz, 99.982% of the virtual-time ideal), then the classic
  INIT-SIPI-SIPI ICR sequence per would-be AP, which the LAPIC model now
  accepts and traces (delivery modes INIT/SIPI are legal sends, not
  ESR-illegal). With one vCPU no AP answers; the kernel's bounded wait
  falls back to BSP-only and the boot reaches the shell unimpaired.
  Measured verdicts: the loader `goto_address` wake mechanism is unused
  by the kernel (zero source references — it self-serves via the ICR);
  the BSP's scheduler tick stays on the PIT by kernel design (periodic
  LAPIC ticks are armed per-AP only), so scheduler preemption is
  observable through the kernel's tick-paced `[sched]` self-test, and
  true per-AP LAPIC scheduling needs a second vCPU (future phase).

- The K6 lane wires that second vCPU: `--smp=2` parks a second
  `cpu_state_t` context in wait-for-SIPI (LAPIC id from the fabricated
  MADT/boot_cpu_t), runs a strict 1:1 round-robin where one step = one
  instruction of the current context, and keeps the m->cpu/m->lapic
  pair as "the current context" by swapping in/out around each step —
  every device and exception path keeps its existing shape.  INIT resets
  and parks the target context (SDM 8.4), SIPI starts it in real mode at
  vector<<12; a single virtual master clock (+1 per retired instruction
  from any vCPU, +512 per HLT idle quantum — identity when n_vcpus==1)
  drives PIT/RTC/LAPIC timers and RDTSC for both contexts.  Measured
  with kernel 0.0.1: the INIT-SIPI walk wakes the real AP, which reaches
  `[smp] AP #0 online` in ~20 ms of virtual time; the AP's periodic
  LAPIC timer interrupt (vector 32) is delivered into its hlt-ed idle
  loop and EOI'd back every ~143k vtime (100 Hz) — per-CPU preemption
  ticks on the AP are real; `PASS: multi-core system detected`; a full
  boot to the shell.  Debugging lanes kept: `--smp-probe` (AP wake-window
  vital signs, PIT ch2/udelay cadence, LAPIC intack/EOI trace) and
  `--watch-phys=addr` (single-address store probe).  The MC146818A
  periodic interrupt chain (register-A RS rate, PF+PIE) is modeled now
  so the kernel's AP-wake receipt test (SYS_IRQ_AP_WAKE, an RTC IRQ8
  storm routed to the AP through the I/O APIC) can run.

- The STORE plan opened (S0 done): baseline measured on machine
  `b00c4d9` — the AHCI PCI stub at 0:31.2 has no BAR5, so the guest
  prints `[ahci] controller 0: BAR5 empty, skipping` and every storage
  mount is offline (`/disk /fat /ext2 /exfat /ext4`); the guest AHCI
  contract was extracted from `AuraLite-OS/drivers/ahci/ahci.c` (731
  lines: ABAR regs, port files, polled `PxCI` completion, READ/WRITE
  DMA EXT over a 128-KiB per-port bounce buffer, self-test LBA0/LBA1
  semantics); phases S1–S5 mirror the OS repo's QEMU gates
  (`test_ahci_rw.sh`, `test_ahci_matrix.sh`, `test_ahci_large_read.sh`).
  Plan: `docs/plans/STORE_PLAN.md`.

- STORE S3 done (patches/0033): WRITE DMA EXT (0x35) shares the S2
  engine -- one `ahci_dma_xfer(..., w)` PRDT walk moves guest-RAM bytes
  into the attached image (direction still governed by the command
  header `W` bit; overflow-guarded like the read leg), so the S2 TFES
  stub for writes is retired.  Unit ladder 13 -> 16 vectors all green
  (`make test`, `make test-sanitize`): write+readback, multi-PRDT write,
  OOB write -> TFES with tails intact.  Guest receipts against the
  frozen kernel/initrd: marked-image boot prints `PASS: SATA read/write
  DMA works` (guest's own scratch write/verify/restore), auto-format of
  AUFS (LBA2) + FAT32 (LBA64) is clean, and a typed script injected at
  the guest's own prompt round-trips both tokens (`/disk/ci.txt` ->
  `s3disk1ok`, `/fat/ci.txt` -> `s3fat22ok`) with zero FAIL/unsupported
  lines; the KBC injection queue grew 64 -> 512 to fit the ~180-byte
  script (test-lane depth, guest-visible KBC unchanged).  Blank-image
  lane run twice sequentially: byte-identical 97,406-byte serial logs.
  Persistence stays copy-on-attach in-memory (host image byte-pristine
  after the guest formatted it) -- writethrough policy is S5 scope.
  Harness: `tests/integration/test_ahci_rw.sh`.

- STORE S2 done (patches/0032): the AHCI command engine is online for
  the read path -- PxCI walks the guest-built command list/table (H2D
  Register FIS, CFL/PRDTL honoured), READ DMA EXT moves image bytes into
  guest RAM through the PRDT (multi-entry walk, prdbc write-back), and
  IDENTIFY DEVICE serves a machine-honest 512-byte block; the error
  channel is TFES (PxIS bit30) + sticky PxTFD.ERR with the guest's own
  W1C recovery.  Receipts: UP `--sata=` boot reaches the guest self-test
  line `blank LBA0 read successfully; write verification skipped`; the
  same 2-vCPU boot prints R5 + typed `/tests/smpstress` -> `SMPSTRESS
  PASS` with all three storm assertions (10-minute wall time this
  run-shape).  New testability lane `--keys-at=prompt` (fires on the
  guest's own `auralite#` echo) because prompt arrival is measured
  budget-noisy across machine shapes.  13-vector test_ahci remains
  LeakSanitizer-clean; determinism: two identical UP boots byte-equal.
  The S1 2-vCPU caveat is retired with the timeout path gone.

- STORE S1 done (patches/0031): the SATA/AHCI function at 0:31.2 now
  has a real ABAR MMIO window (0xFEB10000) behind BAR5 with the register
  model the guest driver consumes -- CAP/PI/VS/GHC, 6 port files with
  SSTS DET/SIG, CLB/FB bases, CMD ST/FRE mirrored into CR/FR, W1C
  IS/SERR, COMRESET via SCTL.  CLI: `--sata=` (port 0) and
  `--sata-portN=`.  Measured receipts: unattached boot -> `1
  controller(s), 0 SATA device(s) ready` + all mount-skips unchanged;
  attached blank disk -> `hw port 0: SATA disk (sig=0x00000101)` + the
  guest's documented timeout receipt (PxCI latch, TFD=0x40) -> shell on
  both, ~5 s guest wall time.  Tests: 8-vector test_ahci.c; whole
  `make test` + `make test-sanitize` green.  Caveat carried into S2: on
  the 2-vCPU lane the guest's ahci_exec timeout does not retire inside
  the fixed window (it would also need ~3,5% more budget than UP on
  QEMU -- instruction interleave, not a machine defect); S2's real
  engine makes the timeout path unreachable anyway.

- The K8 lane made the 2-vCPU machine pass the guest's own userspace
  SMP gates (`/tests/smpstress` and `/tests/irqapwake` typed into the
  shell over the KBC lane; the same assertions the OS repo's QEMU
  -smp 2 integration case makes).  Two machine-side pacing defects
  were found red-first and fixed: an hlt-idle vcpu no longer charges
  its K3 512-instruction quantum when a peer has runnable work (busy
  instruction streams no longer pay for a halted sibling's idle
  fast-forward; guest-measured 2-CPU boot-to-shell 1031s -> ~5s), and
  the PIT/RTC/LAPIC time-settle calls now run on every idle slot so a
  halted vcpu's calibrated LAPIC watchdog still expires at true wall
  rate while its peer works (was: frozen mid-tick, stranding a fork
  child on the AP runqueue — a wake-lost deadlock).

- The K7 lane closed the wake-window boundary K6 recorded: fine-grained
  `--smp-probe` RIP histograms showed the AP spending its entire window
  in `fb_putchar`/`fb_scroll` (~5.5M retired instructions for one line's
  full-screen scroll — NOT the UART ring, which drains synchronously on
  our insta-ready LSR; K6's ring-latency hypothesis is superseded).
  The D6 pin (12 vtime/PIT tick) models a ~14-MIPS CPU, making that
  scroll ~370 ms of PIT wall time against the kernel's 100 ms SIPI
  wait.  K7 lifts the pin to 384 (~373 virtual MIPS), keeps the RTC on
  the same divisor, and sets `tsc_per_instr` = 1 on all profiles (the
  in-order single-issue model the interpreter is) — kernel receipts:
  `LAPIC bus frequency: 458163200 Hz (458 MHz)` and
  `2 CPU(s) online (1 AP(s) woken)`, deterministic (two runs
  byte-identical), `PASS: multi-core system detected`.

- STORE S4 done (patches/0034): the breadth matrix the guest's own QEMU
  gate (`test_ahci_matrix.sh`) exercises is reproduced guest-side on the
  emulator.  Machine half first (measured): `machine_t` attach rows are
  per controller now (`[ctrl][port]`, `AHCI_CTRLS=2`), `src/ahci.c` runs
  one register instance per controller (ctrl 0 = onboard 0:31:2 keeps
  every historical `[ahci]` receipt; ctrl 1 = `--ahci2` at 0:31:3 logs as
  `[ahci2]`), second ABAR window at 0xFEB12000 (the guest maps 8 KiB per
  BAR5 -- measured), CLI `--ahci2` / `--sata2=` / `--sata2-portN=`
  (images on row 1 imply the controller).  Placement at 0:31:3 is not
  arbitrary: the guest's class scan is bus-0 dev/func ASCENDING (measured
  `pci_find_class_after`), so 31:3 binds second and "controller 0 at PCI
  0:31.2" stays the S1-S3 receipt.  Unit suite 15 -> 18 vectors (second
  window does not alias the first, port placement honoured by the engine
  incl. TFES on a dark port, cross-controller images never bleed).
  Guest receipts (`tests/integration/test_ahci_matrix.sh`, frozen
  kernel@0ed0d29 + initrd): lane A port-0 token round-trip `laneaok` +
  `PASS: SATA read/write DMA works`; lane B `hw port 1: SATA disk` with
  the empty port 0 skipped + `lanebok` + byte-identical determinism pair;
  lane C `controller 1 at PCI 0:31.3` + `2 controller(s), 2 SATA
  device(s) ready` + `[ext2] blkdev 1 mounted at /ext2` (2-disk boot) +
  `lanecok`; lane E (no attachment) `1 controller(s), 0 SATA device(s)
  ready` + `self-test: no devices`.  Prompt arrival is deterministic and
  measured (2680097588 / 2680082155 / 2748891330 instructions).

- STORE S5 done (patches/0035): the STORE plan's hardening phase.  128-KiB
  transfers first: the engine always walked PRDTs of any size, but the
  guest's own per-port DMA bounce ceiling (128 KiB, measured in
  AuraLite-OS drivers/ahci/ahci.c) had no vector -- unit suite 18 -> 22
  pins the full 256-sector READ/WRITE DMA EXT shape (two 64-KiB PRDTs,
  `prdbc == 131072`, golden bytes) and
  `tests/integration/test_ahci_large_read.sh` mirrors the QEMU
  large-read gate guest-side: a pre-built FAT32 at LBA 64 (64 leading
  zero sectors so the kernel cannot mistake the disk for blank) carrying
  a 16 MiB payload -- the QEMU-side MEASURED size a bounce leak cannot
  survive -- read to EOF via typed `run /apps/filesize
  /fat/payload.bin`, asserting `FILESIZE /fat/payload.bin 16777216`,
  no reformat, no leak-ceiling truncation, byte-identical on re-run.
  Writethrough: `--sata-writethrough` moves every guest WRITE DMA EXT
  byte into the host image file at the same offset (unit vector `ok
  writethrough` + `tests/integration/test_ahci_writethrough.sh` -- the
  same typed token is byte-pristine on the host under the default
  copy-on-attach policy and visible on the host with the flag).  Honest
  INTx: the AHCI 1.3 interrupt condition ((PxIS & PxIE) | (HBA IS &
  GHC.IE)) drives board line AHCI_IRQ0+ctrl (14/15) through
  pic_set_irq() into the PIC and the I/O APIC fan-out; HBA IS is a live
  PxIS aggregation now; the guest polls with PxIE=0 and never sets
  GHC.IE (measured), so receipts cannot move and `ok intx line` pins the
  line behavior (edge latch on assert, LTIM level follow on deassert).
  Hardening receipts: full-matrix determinism (`MATRIX_DETERMINISM=1`
  re-runs lanes A/B/C/E sequentially, byte-identical), the unattached
  negative-control lane is byte-identical on re-run and prints the
  unattached receipt verbatim, `make test` + `make test-sanitize` green.

- PERF P0 delivered (`patches/0051-PERF-P0-honest-benchmark.patch`): a
  separate `cpu.executed_steps` counter increments only on the cpu_step
  `done:` path, never on the K3 +512 all-idle clock quantum; the K6
  guest-visible `instr_count`/`vtime_instr` semantics stay unchanged.
  `--stats` opts into per-vCPU attempts, machine-wide executed total,
  idle delta, virtual clock and cap flag; without it, the four firmware/
  video tapes are byte-identical pre/post in stdout, log and framebuffer.
  `tests/test_perf.c` pins STI;HLT + two idle slots as 2 executed steps
  versus `instr_count=vtime=1026`, the K6 identity, faulting attempt,
  single/multi-context summation, and cap-independent completed work.
  `make bench` uses a separately rebuilt O2 binary (the ASan lane replaces
  `x86emu` with O1 ASan), seven firmware tapes ×5 median host wall-time,
  stable work-count gates and no timing threshold. On this runner boot-
  haswell did 4,837,677 attempts in 678 ms (7.14 Msteps/s) and halted
  well below the 50M cap; the NC divides that SAME wall time by the
  cap instead, prints 10.34× inflation, and fails by design: script
  RC=1, `make bench-nc` RC=2 (`make` reports the recipe's Error 1).
  `make test` RC=0, `make test-sanitize` RC=0, `make fuzz
  FUZZ_PROGRAMS=256` RC=0 (6,144 instructions compared, no host crash).
  In a fresh clone at `a3f76b5`, `git am` applied cleanly and yielded the
  same tree; after seeding prebuilt nasm artifacts, `make test` and `make
  test-sanitize` passed RC=0, `make bench BENCH_REPS=3` RC=0, and the
  NC `make bench-nc BENCH_REPS=3` failed RC=2 as designed. Frozen
  AuraLite-OS guest lanes SKIP without its missing build tools. P1–P5
  are planned, not delivered.

- PERF P1 delivered (`patches/0052-PERF-P1-profiled-ram-read.patch`):
  after five O2 gprof firmware runs measured 313,666,250 `mem_read`
  calls (45.66% of samples; 235,535,030 from 8-byte page-table walks),
  `mem_read` has a narrowly guarded 8-byte RAM `memcpy` path. It runs
  only on little-endian hosts with A20 open, entirely inside RAM and
  *after* both ROM alias checks; no write path, MMIO, disabled A20,
  partial boundary or non-8 width changes. Five post-change gprof runs:
  the same 313,666,250 calls, 14.37% of samples. Six deterministic
  bus vectors pass, including the low BIOS ROM alias at `0xE0000` (which
  lives inside RAM), RAM-end FF fills, A20, MMIO and unaligned LE loads.
  The phase NC byte-swaps only a scratch fast-path return, causing the
  first LE assertion to fail (inner RC=134, wrapper RC=1, make RC=2).
  Seven tapes × two paired pre/post triples kept *identical* executed
  work counts and no capped run, with local wall-time ratios 1.18–1.83×
  and obvious host-frequency variation: not a CI wall-time threshold.
  Four historical firmware/video stdout+log+PPM triples remain
  byte-identical pre/post. `make test`, `make test-sanitize` and
  `make fuzz FUZZ_PROGRAMS=256` all RC=0. In a fresh clone at the
  P0 parent, `git am` produced an identical tree; after seeding the
  ignored nasm binaries, `make test`, `make test-sanitize`, `make fuzz
  FUZZ_PROGRAMS=256`, `make bench BENCH_REPS=3` all passed RC=0;
  `make test-perf-mem-nc` failed RC=2 by design. Frozen AuraLite-OS
  lanes still SKIP without its guest build toolchain. P2–P5 remain planned.

- PERF P2 delivered (`patches/0053-PERF-P2-coherent-success-tlb.patch`):
  after P1, five O2 gprof firmware boots measured `translate` 79,798,615
  calls, 22.10% of samples. A 256-entry shared success-only TLB caches
  4KiB subpage translations after the existing permission checks; full
  CR3/CR0/CR4/EFER, AC, CPL, descriptor override and access mode key
  the result. Only ordinary RAM-backed page-table reads with A20 open
  may be cached. Global generation flushes on every guest/DMA RAM store,
  CR0/CR3/CR4/LMSW/EFER/INVLPG/reset and K1 loader boot; faulting,
  ROM/MMIO-backed and A20-closed walks remain uncached. Fresh `--stats`
  on the boot tape: 15,863,692 hits, 4,668 misses, 4,669 flushes,
  4,837,677 actual instruction attempts; `mem_read` in five profiled
  boots fell 313,666,250→78,188,375 calls. P2 vectors include live
  PTE remap, all four #PF CR2/error/RIP verdicts, large PS pages,
  dynamic MMIO root and SMP keying; the single NC removes the RAM-write
  flush in a scratch copy and reddens `assert(c->gpr[RAX] == NEW_VALUE)`
  (binary RC=134, wrapper RC=1, make RC=2). Seven alternating pre/post
  tapes kept work counts identical; local paired time ratios 1.15–1.35×
  with no absolute timing gate. Four historical video/firmware stdout,
  log and PPM receipts byte-identical. `make test`, `make test-sanitize`,
  `make fuzz FUZZ_PROGRAMS=256`, `make bench BENCH_REPS=3` all RC=0;
  in a fresh clone from `f7c3bd7`, `git am` gave the identical tree;
  after nasm-artifact seeding, `make test`, `make test-sanitize`, `make
  fuzz FUZZ_PROGRAMS=256`, `make bench BENCH_REPS=3` each passed RC=0;
  P2 NC failed RC=2 by design. Frozen AuraLite-OS lanes SKIP without
  the missing guest toolchain. P3–P5 remain planned.

- PERF P3 (`patches/0054-PERF-P3-ram-prefix-predecode.patch`): after P2,
  `decode_modrm` sampled 8% and `fetch8` 6%; `group1` redundantly
  re-fetched 970,913 times per boot. A shared 512-slot cache stores only
  RAM-backed, one-page prefixes/opcode after successful P2 EXEC; dynamic
  ModRM/SIB/imm always re-read, the P2 RAM-write epoch coheres SMC/PTE/DMA,
  and MMIO/ROM/NX, A20-off, cross-page and debug stay live. New vectors
  cover SMC/PTE remap, mode/trace, NX #PF err/CR2/RIP, SIB/imm and MMIO;
  the one epoch-check NC fails deliberately (inner RC=134, make RC=2).
  Two alternating seven-tape series show UP 1.17–1.35×, SMP ~1.02×,
  identical instruction work; four historical stdout/stderr/log/PPM lanes
  byte-identical. The ring3 harness now flushes after post-start raw RAM
  instruction injection. Full gates/fresh-clone receipt in PERF_PLAN §6.4.
  Frozen AuraLite-OS toolchain absent: guest lanes SKIP. P4–P5 planned.

- PERF P4 measured NO-GO: gprof on P3 did not isolate opcode-switch
  cost, and a scratch-only direct-group1 dispatch experiment gave mixed
  paired results (including two Haswell/Broadwell slowdowns). No CPU
  dispatcher or bogus phase patch was shipped. Reproduction:
  `docs/research/PERF_P4_GATE.md`.
- PERF P5 (`patches/0055-PERF-P5-paired-confidence-closeout.patch`): the
  seven-tape comparator now measures alternating before/after O2 pairs,
  rejects unequal guest steps/idle/BSP clock/vtime/vCPU count and capped or
  nondeterministic runs, then reports seeded 95% *local* bootstrap intervals
  without timing CI thresholds. Nine-pair P2→P3 UP estimates 1.237–1.279×;
  SMP 1.002× [0.977,1.026] has no speed claim. P3→P5 binaries are
  bit-identical; nine available historical stdout/stderr/log/PPM lanes are
  byte-identical; N2/N3 wire gates remain x2. Five P5 comparator vectors
  pass, and its single NC removes the work-equality guard and fails
  `AssertionError: ValueError not raised` (inner 1, make 2). D0–D8 map
  to concrete vectors in PERF_PLAN. Full gates/fresh-clone verdict in §8.4.
  Frozen guest lanes SKIP without its absent toolchain.

## Current limits

This is a general firmware-focused x86 emulator, not yet a drop-in replacement for QEMU. The CPU ISA is expanded from execution traces and tests. Some chipset blocks are behavioural models rather than cycle-accurate implementations. The descriptor/CBW/CSW path and live web monitor are still being expanded.
