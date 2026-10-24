# Upkeep Plan — the documented small gaps, measured one by one

> `ROADMAP.md` measured-gaps row 4 and next-block candidate 4.
> XHCI, MSI and PM are complete; this plan takes the remaining
> site-documented gaps in dependency/risk order.

**Status: U0–U7 done ✅ — plan COMPLETE (closed by U7)**

| Phase | Scope | Status | Deliverable |
|---|---|---|---|
| U0 | Measured baseline for all seven row-4 items, defect ledger, decisions and phase split | ✅ done | this file (`patches/0091`) |
| U1 | RTC: a settable MC146818 clock/calendar without changing the pinned power-on behaviour | ✅ done | `tests/test_rtc.c` vectors (`patches/0092`) |
| U2 | e1000: EEPROM reads through EERD and a PHY register file through MDIC | ✅ done | `tests/test_net.c` vectors (`patches/0093`) |
| U3 | gdbstub: publish and accept the real x87 register/control state in `g`/`G`/`p`/`P` | ✅ done | `tests/test_gdbstub.py` vectors (`patches/0094`) |
| U4 | LAPIC: architectural `IA32_APIC_BASE` relocation and global enable/disable | ✅ done | `tests/test_lapic.c` + `tests/test_snapshot.c` vectors (`patches/0095`) |
| U5 | Chipset: CPU INIT from CF9 `RST_CPU`, distinct from the existing full reset | ✅ done | `tests/test_chipset.c` vectors (`patches/0096`) |
| U6 | TAP responder TCP: sequenced receive, SACK generation and complete close-state handling | ✅ done | `tests/test_tap_responder.py` vectors (`patches/0097`) |
| U7 | GUI/monitor: capability authentication and browser-origin defences, then plan close-out | ✅ done | GUI/monitor vectors + close-out (`patches/0098`) |

Each implementation phase ships one `.patch` under `patches/` with
measured receipts, a regression vector, one once-per-phase negative
control, green gates and the docs update in the same commit (the
standing house rules). U0 changes documentation only.

## 0. Measurement method and current receipts

Baseline taken at emulator `dd536b1` after PM P3. The current code
and its test contracts were inspected directly; the current
AuraLite-OS source was inspected at `4450da7` for consumer use (it
is a consumer clue, not a replacement for this repository's frozen
guest receipts). QEMU's current `hw/net/e1000.c` was inspected for
the EEPROM/PHY contrast in §1.4. Targeted current-tree receipts
for U0:

- `make test-unit`: green (includes LAPIC, chipset, RTC and e1000
  register-file suites);
- `make test-tap-responder`: green;
- `make test-gui` and `make test-monitor`: green (the desktop
  construction sub-lane skips without a display, as designed);
- `make test-gdb`: green for the 30-check pure-RSP protocol lane;
  the real-GNU-gdb lanes SKIP on this host because `gdb` is not
  installed, which U3 must not mistake for a real-client receipt.

No unmeasured runtime claim below depends on the absent
AuraLite-OS KERNEL/INITRD artifacts.

## 1. Measured baseline (U0)

### 1.1 LAPIC `IA32_APIC_BASE` — stored, never decoded

- `cpu_reset()` seeds MSR `0x1B` to `LAPIC_MSR_DEFAULT`
  (`0xFEE00900`: base, global enable, BSP). Guest `WRMSR` uses the
  generic sparse MSR store and executes no LAPIC side effect; the
  header says relocation/disable is “stored only, documented.”
- The MMIO window is registered exactly once at compile-time
  `LAPIC_BASE = 0xFEE00000`, and both handlers subtract that
  constant. A guest that writes a new base and then follows it
  (AuraLite-OS reads MSR `0x1B` in every LAPIC helper) leaves the
  live window behind and accesses whatever the normal decoder
  finds at the new address, while the old window continues to work.
- Delivery checks only `SVR` software-enable. Clearing
  `IA32_APIC_BASE.EN` changes neither MMIO, timer progress, IRR
  latching nor CPU delivery. CPUID leaf 1 does **not** advertise
  x2APIC (ECX bit 21), so x2APIC-mode selection is not an implicit
  sub-feature waiting for a decoder; it must remain rejected
  unless a separate x2APIC plan changes the CPUID contract.
- The SMP context machinery multiplies the problem rather than
  hiding it: each slot has its own CPU/MSR and LAPIC state, but the
  bus has one fixed registration. `cpu_reset()` also has no context
  identity, so every AP slot is seeded with BSP=1 until some other
  code corrects it. Snapshot restore additionally requires the
  live MMIO list to match the saved base exactly, except for the
  special runtime regions it already knows how to rebind.
- The north-star guest reads but does not write MSR `0x1B`; the
  consumers are firmware/OS paths (notably Linux APIC setup and
  disable/re-enable flows) and architectural honesty.

### 1.2 CF9 CPU INIT — only the full-reset half exists

- `cf9_write()` stores every byte and requests the existing full
  warm reset only when `SYS_RST` rises while `RST_CPU` is set.
  `0x02` alone and `0x04` alone are explicitly pinned as no-reset
  by `test_reset_via_cf9_predicates`. Thus the missing transition
  is the `0x02` CPU-INIT write itself; the familiar
  `0x02 -> 0x06` sequence already reaches a full reset on the
  second write.
- All reset sources collapse into one Boolean `reset_pending`,
  consumed by `cpu_step()` at the next instruction boundary.
  `machine_reset()` resets the active CPU/LAPIC plus PIC, PIT,
  KBC, IOAPIC and chipset state. There is no pending-request kind
  and no CPU-only boundary operation to reuse unchanged.
- IPI INIT is related but not a drop-in substitute:
  `vcpu_take_init()` resets a non-running slot and parks it in
  wait-for-SIPI, while deliberately refusing INIT at the running
  context. A chipset INIT# request necessarily originates from
  the running context and has platform-wide target semantics.
  U5 must measure those semantics from the chipset/CPU documents
  instead of treating the IPI helper as the definition.
- AuraLite-OS reboots through the 8042 `0xFE` pulse and has no CF9
  consumer. The ACPI FADT shipped by PM P2 advertises reset value
  `0x0E`, so U5 must preserve that as a full reset exactly.

### 1.3 RTC — a deterministic clock that cannot be set

- The clock is a pure function of monotonic virtual ticks from a
  fixed 2026-01-01 00:00:00 anchor. `rtc_data_write()` discards
  writes to seconds/minutes/hours/day-of-week/day/month/year and
  the century byte. The existing test pins even that refusal.
- Register B is stored, but only UIE/PIE have effects. Reads are
  always emitted as BCD in 24-hour form regardless of Register
  B's data-mode and hour-mode bits, and the SET bit does not stop
  the update cycle or stage an initialization sequence.
- The civil conversion starts at 2026 and walks forward from an
  unsigned tick count, so simply “accept the fields and convert”
  first needs a representation that can name dates before the
  power-on anchor as well as after it. UIP/update-ended and the
  later periodic-IRQ logic derive phases from the same ticks and
  are canaries, not collateral.
- AuraLite-OS only reads the epoch and writes Registers A/B to
  run periodic IRQ8; firmware and general-purpose OSes are the
  consumers that set the clock. A hardware-setting utility or
  Linux `hwclock` operation currently appears to succeed at the
  port and then reads back the untouched 2026-derived time.

### 1.4 e1000 EERD/MDIC — two command registers are storage cells

- `e1000_mmio_write()` has special cases for control, interrupt
  and ring registers; EERD (`0x14`) and MDIC (`0x200`) fall into
  the generic register store. No EEPROM array, PHY array, DONE/
  READY completion, ERROR bit or MDAC cause exists in `e1000_t`.
- This is on the north star's probe path. Its driver issues an
  EERD read for EEPROM words 0, 1 and 2, polls DONE up to 100,000
  times per word, and only then falls back to RAL/RAH. The current
  stub therefore costs three timeouts during every probe and the
  RAL/RAH preload is what makes the receipt say
  `MAC from RAL/RAH`. If U2 does its job, the same MAC must arrive
  through the EEPROM and that source-line change is an expected,
  reviewed receipt change—not a reason to preserve the timeout.
- The guest does not use MDIC. General drivers do use it for PHY
  ID, reset, link and autonegotiation state. The contrast in
  QEMU is a 64-word EEPROM plus a 32-register PHY file with
  capability masks, synchronous EERD completion on read, MDIC
  READY/ERROR handling and an optional MDAC interrupt. U2 will
  pin the exact 82540EM values from primary/QEMU source at
  implementation time rather than copying constants from memory.
- The e1000 exists only with `--tap`; snapshot save/load refuses
  TAP runs because the endpoint is host state. New EEPROM/PHY
  state therefore does not need to invent a snapshot format in
  U2, but reset behaviour does need a vector.

### 1.5 TAP-responder TCP — SACK is negotiated, never spoken

- The responder advertises SACK-permitted in its SYN-ACK and logs
  whether the peer offered it, but does not retain that fact in
  the connection record, never emits a SACK option, and
  `tcp_segment()` always builds a 20-byte header.
- More importantly, receive is not sequenced. For any known
  tuple, `rcv_nxt += seglen` and payload bytes are appended to the
  DNS buffer without comparing the segment sequence number.
  A duplicate advances the acknowledgement twice; an
  out-of-order segment is delivered to the application as if it
  were contiguous; an out-of-order FIN consumes sequence space
  at the same time. The current in-order guest/TAP lanes do not
  expose this, but the state machine cannot generate an honest
  SACK until it stops doing it.
- AuraLite-OS's TCP stack can parse SACK blocks and use them to
  mark its retransmission queue. It is therefore a real potential
  consumer of responder-generated SACK, not merely an RFC reader.
- The close path has a narrower defect than the ROADMAP's short
  “no TIME_WAIT” wording suggests. In the only reachable normal
  flow the guest closes first, the responder answers FIN|ACK and
  enters LAST_ACK, and the guest—not the responder—is the side
  that enters TIME_WAIT. Freeing the tuple on the final ACK is
  architecturally right for that passive close. What is missing
  is complete duplicate/crossing-segment behaviour: a duplicate
  FIN while LAST_ACK currently frees the tuple, there is no
  CLOSING/active-close path, and no bounded retained state can
  answer late duplicates. U6 must implement the states that are
  actually reachable and must not fabricate a passive-side
  TIME_WAIT merely to match a checklist word.
- The responder has no timers and its determinism tests require
  replayed frames to produce identical replies. U6 therefore
  cannot quietly introduce wall-clock ageing as guest-visible
  behaviour; any retained tuple/TTL mechanism must be explicit,
  bounded and replay-deterministic.

### 1.6 gdbstub x87 — target description promises registers it zeroes

- The target XML and packet layout already reserve registers
  24–31 for `st0..st7` (10 bytes each) and 32–39 for x87
  control/status. `reg_hex()` nevertheless emits zero for every
  ST register, emits FCW but zero for FSW and pointer/opcode
  fields, and hard-wires FTAG to all-empty. `reg_set()` silently
  ignores all of them. The stale comment says “no x87 data
  model”, but FPU F1–F4 has since given the CPU a full physical
  register file, tags, TOP, status/control and last-instruction
  pointers.
- The hard part is not finding state; it is mapping it the way
  GDB expects. CPU storage is physical `fst[8]` plus `ftop`,
  host `long double` slots with normalized padding, and helper
  converters private to `cpu.c`. U3 must measure GDB/QEMU's
  logical-vs-physical register convention and control-field
  widths before exposing bytes, then prove a write with an FPU
  instruction after resume.
- Current regression coverage parses the fixed-size `g` packet
  but asserts no x87 value. Real GDB is unavailable on the U0
  host, so the phase gate needs either a real client receipt or
  an explicitly pinned protocol fixture measured from GDB's own
  target conventions—not only self-consistent stub bytes.

### 1.7 GUI/monitor — loopback is a default, not an authentication model

- The desktop GUI is a local process, but it starts each machine
  with an unauthenticated monitor on a free loopback port. The
  web launcher defaults to loopback and its HTTP API can start/
  stop machines, browse server-side files, download artifacts,
  manage presets/snapshots and splice a browser WebSocket onto
  the live monitor. The emulator monitor likewise accepts its
  page/WebSocket upgrade with no credential, Origin or Host
  authorization check.
- Loopback stops ordinary remote hosts; it does not stop another
  local process or a malicious web page attempting cross-origin
  requests/DNS-rebinding against a predictable local service.
  The launcher and monitor also allow an operator to bind
  non-loopback addresses, at which point the same unauthenticated
  control surface is network-facing. Existing README/STATUS text
  correctly calls this an operator choice, but the product has
  no safer authenticated choice to offer.
- User experience is a constraint, not an excuse: the standing
  requirement is that **no command line be needed**. U7's normal
  desktop/web flow must remain one-click, generate and carry its
  capability internally, and expose any remote-access secret
  through the GUI—not require terminal or environment setup.

### 1.8 Defect ledger

| # | Defect (measured) | Consequence | Phase |
|---|-------------------|-------------|-------|
| D1 | `IA32_APIC_BASE` writes are sparse-MSR storage; decode/delivery ignore base/EN; AP reset also seeds BSP=1 | relocated/disabled LAPIC software follows an address or state the machine does not implement | ~~U4~~ **resolved in U4** (live validated WRMSR: per-context window mirror, BSP strap seeded by role + preserved on writes, EN gates MMIO/timer/IPI/IOAPIC/MSI, snapshot rebinds the window) |
| D2 | CF9 `RST_CPU` alone has no boundary effect; only full reset exists | CPU-only INIT reboot paths continue executing as if the write were inert | ~~U5~~ **resolved in U5** (the 0x04 soft write now broadcasts INIT# at the next boundary to every vCPU; the pending request is a kind with full-reset coalescing; device state rides through; the register's bit roles were also corrected to the PCH names — SYS_RST=type 0x02, RST_CPU=trigger 0x04) |
| D3 | RTC calendar/century writes are dropped and Register-B SET/DM/hour mode do not govern the clock | OS time-setting silently fails; readback always derives from the 2026 anchor | ~~U1~~ **resolved in U1** (SET-staged writes commit to `base_sec`; DM/24H govern both directions) |
| D4 | EERD/MDIC are generic storage with no EEPROM/PHY state or completion bits | the north star takes three EEPROM timeouts; PHY-probing drivers see a fictional command echo | ~~U2~~ **resolved in U2** (64-word EEPROM + synchronous DONE; 32-register PHY with cap table, READY/ERROR, MDAC; MDIC offset corrected 0x200 → 0x20) |
| D5 | Responder TCP appends/acks payload without sequence validation, sends no SACK and has incomplete duplicate-close states | loss/reordering corrupts the DNS stream and acknowledgement state instead of eliciting retransmission | ~~U6~~ **resolved in U6** (modulo-2^32 validation vs RCV.NXT+window, trim/carve, bounded OOO queue with contiguous-only delivery, ≤4 RFC 2018 SACK blocks when negotiated, in-order FIN, dup FIN in LAST_ACK re-sends our FIN|ACK, bounded tombstones for late duplicates) |
| D6 | gdbstub advertises x87 registers but reports zeros/all-empty and ignores writes | FPU debugging displays a machine that is not the one executing | ~~U3~~ **resolved in U3** (logical STs, full physical FTAG, FCW/FSW/FIP/FDP/FOP live; P/G writes rotate + re-tag; real-gdb lane F) |
| D7 | Launcher API, launcher WebSocket proxy and direct monitor are unauthenticated and do not authorize Host/Origin | local cross-origin/process access and any opted-in remote bind get full machine control | U7 |

## 2. Architecture decisions (U0)

- **This is one plan, not one mega-patch.** The ROADMAP shorthand
  says “one upkeep patch”, but the measured items span CPU reset
  architecture, two devices, a Python protocol peer, debugger
  serialization and web security. One commit would make the
  required negative controls and canary receipts meaningless.
  Each subsystem gets its own phase/patch; U7 closes the plan.
- **Defaults are canaries.** Every phase must keep the current
  no-feature-used path byte-identical where a receipt exists:
  RTC power-on/UIP timing, e1000 MAC and data rings, register
  packet size/order outside x87 values, LAPIC at the reset base,
  full-reset sources, in-order TCP frames and local GUI startup.
  Expected receipt changes (initially the e1000 MAC source line)
  are called out in the phase commit, not discovered afterward.
- **Encodings come from primary sources at implementation time.**
  Intel SDM/ICH and MC146818 documents for U1/U4/U5, Intel
  82540EM + QEMU behaviour for U2, GDB's i386 target conventions
  and a real/protocol client for U3, RFC 9293/2018 plus the
  guest parser for U6. U0 fixes scope and acceptance; it does not
  bless register constants remembered by the plan author.
- **Snapshot safety is explicit.** The snapshot header already
  rejects different `machine_t`/`cpu_t` layouts. Phases should
  first try to derive new behaviour from state already carried
  (MSR store, RTC ticks, existing pending kind encodings). Any
  layout growth is a stated snapshot-compatibility break with a
  refusal vector, never a silent reinterpretation. LAPIC U4 in
  particular must teach snapshot restore to rebind or refuse a
  relocated window deliberately.
- **No wall clocks enter deterministic models.** RTC remains on
  `vtime_instr`; the TAP responder remains replay-deterministic.
  Host time may exist in process startup/security-token lifetime
  at U7, but it must not alter guest execution or wire receipts.
- **No fake completeness.** x2APIC, RTC alarm delivery, EEPROM
  programming/bit-bang access, TCP congestion/retransmission
  timers/window scaling/IPv6 and TLS/account management are not
  silently folded into these phases. If a phase's primary-source
  measurement proves one is inseparable from the named defect,
  the phase plan/receipts say so before code moves.

## 3. Phase acceptance

### U1 — RTC

Prove the canonical set sequence (SET, field/century writes,
clear SET) readback and continued advance; leap-day and year
rollover; BCD vs binary and 12- vs 24-hour read/write conversion;
no update/UIP while SET; invalid fields cannot hang or create
out-of-range civil state; a warm reset preserves the set clock;
snapshot split-run restores it; untouched power-on vectors and
periodic IRQ8 remain exact.

**U1 receipts.** Shipped with QEMU's `mc146818rtc` SET semantics
(measured from that source): the rising SET edge seeds the
register bytes from the live clock in the current format and
clears UIE; writes stage in the CMOS bytes; the falling edge
parses under the format that was live during SET and moves the
new `base_sec` (Unix seconds under the unchanged tick count),
keeping the sub-second phase. Range-invalid fields log and
change nothing. While SET: reads return staged bytes, UIP
reads 0, no UF is raised; the periodic chain keeps running.
`machine_t` grew by the 8-byte `base_sec`, so pre-U1 snapshots
are refused by the existing header layout check (never
reinterpreted). Four new groups in `tests/test_rtc.c`
(leap-day set + warm reset, freeze/invalid, binary + 12-hour
round-trips, SET-clears-UIE); all pre-U1 vectors pass
unmodified. NC: rtc.c/rtc.h at U0 — behavioural abort at the
first staged-readback assert.

### U2 — e1000 EEPROM + PHY

Prove the guest's exact EERD transaction for words 0–2 returns
the same MAC without DONE timeout; pin checksum/identity words
and out-of-range behaviour; prove MDIC PHY-1 read/write,
READY/ERROR, PHY ID/status/reset and MDAC-to-INTx if the source
contract includes it; prove device reset restores the template
while MAC, RAL/RAH and rings remain consistent. Record the
expected guest MAC-source receipt change.

**U2 receipts.** Shipped with QEMU's semantics and values
(measured from `hw/net/e1000.c`, `e1000x_common.c`,
`e1000x_regs.h`): the 64-word `e1000_eeprom_template` prepared
`e1000x_core_prepare_eeprom`-style (MAC in words 0–2, device
id `0x100E` in words 11/13, word `0x3F` closing the image to
a `0xBABA` sum), prepared once at device add; EERD writes are
plain stores and the read mirrors `flash_eerd_read` (synchronous
DONE, unmasked address shift, out-of-range past `0x3F` completes
with the stored data field). MDIC mirrors `set_mdic` branch for
branch at PHY address 1 over QEMU's `phy_regcap` table: READY
on every store, ERROR on read-only/unimplemented accesses and
for a wrong PHY address (with QEMU's previous-value echo),
MDAC (`ICR` bit 9) when INT_EN survives — driven onto the N4
INTx wire. BMCR stores through `set_phy_ctrl`'s mask (RESET /
ANRESTART / reserved self-clear; the file is not reset, as in
QEMU). The PHY is seeded at QEMU's post-autonegotiation steady
state (`phy_reg_init` + BMSR.AN_COMP + ANLPAR.ACK,
PHYID2=`0x0C20`): QEMU reaches it through a 500 ms virtual
timer because its link can be down; this model's link is
permanently up, so ANRESTART completes synchronously and no
link flap is fabricated. Found and fixed in passing: the N0
MDIC define was `0x200`; Intel's offset is `0x20` (unobservable
until now — the register was a storage cell and the guest
never writes it). The EECD Microwire bit-bang stays a storage
stub, out of this phase's measured scope (the frozen guest
never touches EECD). Snapshot strategy is empty by construction:
new state lives in `e1000_t` (not `machine_t`) and `--tap` runs
already refuse snapshot save/load, so the layout does not move.
Expected guest receipt change, recorded in advance: the frozen
driver's probe now gets `52:54:00:12:34:56` from the EEPROM and
its serial line names the EEPROM as the MAC source instead of
RAL/RAH; the value is bit-identical. Two new groups in
`tests/test_net.c` (EEPROM probe with first-poll DONE +
checksum + out-of-range + reset survival; MDIC ID/status/RW,
OP=0/OP=3 branch order, ERROR paths, BMCR mask, MDAC wire,
reset restores the template); all pre-U2 groups pass
unmodified, and `make test`'s N1/N2/N3 network lanes are
unchanged. NC: e1000.c at U1 — behavioural abort at the
EERD DONE assert after the guest's full 100,000-poll timeout.

### U3 — gdbstub x87

Run a known FPU payload to a breakpoint; prove `g` and `p`
bytes for ST/control/tag/status against the measured GDB
convention (including TOP rotation), then use `P` or `G` to
change state and resume into an FPU store that proves the CPU
consumed exactly those bytes. Packet lengths and every non-x87
register remain unchanged. A non-80-bit-`long double` host must
fail honestly rather than fabricate bytes.

**U3 receipts.** The convention was measured at both ends before
any byte was exposed. GDB side (`gdb/i387-tdep.c`,
`i387_print_float_info`): GDB iterates PHYSICAL registers,
takes `tag = (ftag >> 2*physical) & 3` (0 valid / 1 zero /
2 special / 3 empty) and fetches the value from regcache slot
`(physical + 8 - TOP) % 8` — so the stub's `st_i` must be the
LOGICAL register, physical `(TOP+i)&7`, and `ftag` the full
16-bit tag word keyed by physical register. QEMU side
(`target/i386/gdbstub.c`): reads rotate exactly that way and
re-derive FSTAT's TOP bits from `fpstt`; both are mirrored.
Two QEMU facts were measured and deliberately NOT copied:
its write side indexes `fpregs[]` physically without rotation
(and never updates tags), so `set $st0` there does not name
the register `p $st0` read; and it returns 0 for
ftag/fiseg/fioff/foseg/fooff/fop. Here an ST write lands in
the logical register's slot through `x87_w` (padding stays
normalized, so snapshots stay byte-stable) with the tag
re-classified by `x87_class` — the FRSTOR-admissible state,
never valid-bytes-tagged-empty — an FSW write moves `ftop`
with TOP (the `x87_load_env` invariant), and the pointer/
opcode fields publish the CPU's real records (32-bit XML slots
truncate FIP/FDP as for every 32-bit consumer). The converters
stayed private-shaped: `cpu.c` grew four bridge functions
(`cpu_x87_host_supported`, `cpu_x87_st_to80`,
`cpu_x87_st_from80`, `cpu_x87_set_fsw`); on a non-80-bit host
the stub answers 'x' (RSP unavailable) for the STs and the
write bridge is a no-op — the FPU already refuses to execute
there, and no bytes are fabricated. Packet layout is
untouched: the `g` canary still measures 592 bytes/64
registers, and lanes A–C pass unmodified.

Vectors: lane D of `tests/test_gdbstub.py` (+27 checks, 57
total) runs a ROM payload (fninit; fld1; fldpi; fldl2e; fldz;
four `fstp m80`) to a breakpoint and pins the rotated stack
byte-for-byte, ftag `0x01FF`, fstat `0x2000`, FIP/FCS/FOP
naming the FLDZ; P-writes −2.5 → st0, 0.5 → st3, the smallest
denormal → empty st7 (tags re-classify to `0x00BF`) and a new
FCW; resumes and reads guest memory proving the FSTP stores
consumed exactly the written bytes (a physical-order write
would store 1.0 fourth instead of 0.5); an FSW write then
moves TOP under a live stop. Real-client half: GNU gdb 16.3
was vendored onto the host for this phase (apt download +
extract, as with NASM), so lane F of `test-gdb` now runs for
real alongside the previously-skipped lane G: `info float`
renders R4.Zero/TOP:4/tag `0x01ff`/FIP `f000:ff08`/opcode
`0xd9ee`, `set $st0 = -2.5` / `set $st3 = 0.5` round-trip, and
`x/10xb` shows the store results. Found on the wire while
building lane F (recorded, out of U3 scope): the stub's
breakpoints are LINEAR (CS.base+RIP) while gdb reconciles a
software-breakpoint hit against raw `$pc`, so in segmented
real mode gdb auto-continues a ROM breakpoint as a spurious
trap; flat modes are unaffected (lane G), and lane F drives
by `stepi`. NC: the U3 test on a `b6d048b` tree — 16 lane-D
checks fail on the old zeros/`0xFFFF`/ignored writes while
lanes A–C and the packet-shape checks stay green.

### U4 — LAPIC MSR ✅

Prove guest WRMSR relocation and return, old-window closure,
global-disable/re-enable effects across MMIO, timer, IPI,
IOAPIC and MSI sinks, invalid/reserved/x2APIC writes leaving
state unchanged, and BSP-bit correctness in UP and SMP.
Snapshot save/load must either restore a relocated LAPIC and
continue equivalently or refuse with the measured reason the
phase intentionally chooses; silently comparing against the
fixed base is not acceptance.

**Receipts (U4):** primary sources measured first — Intel SDM
Vol.3A §§10.4.3–10.4.5, 10.4.7 (EN=0 ⇒ "functionally without an
on-chip APIC" + CPUID flag 0; base bits 12..MAXPHYADDR−1 with
0–7/9/10 reserved; per-processor window addresses are legal)
and QEMU master `hw/intc/apic.c` + `apic_common.c`
(`apic_set_base` preserves BSP and drops the SVR enable on a
disabling write; state persists across disable; `apic_init_reset`
never touches `apicbase`; QEMU's looser EN=0 latching/MMIO-write
behaviour is deliberately not copied). Model: one `"LAPIC"`
MMIO region mirroring the *current context's* MSR decode,
re-synced on accepted WRMSR, live-pair reset, `vcpu_load` and
snapshot load — per-vCPU windows with a single registration;
WRMSR 0x1B validates before storing (reserved/x2APIC/beyond-40
rejects leave all state untouched, logged, no #GP — the house
permissive MSR lane); EN=0 unmaps the window, drops fixed
IOAPIC/MSI/IPI at the latch, freezes the timer without losing
its count, and clears CPUID.1:EDX[9], while INIT/SIPI stay
reachable; `cpu_reset` seeds BSP by slot identity and INIT
preserves the MSR. Snapshot takes the stronger branch: the
load path rebinds the window from the restored MSR before the
MMIO-map check, so relocated and disabled LAPICs restore
equivalently (no format change). Vectors: 4 new groups in
`tests/test_lapic.c` (real guest WRMSR relocation + return,
7 rejected-write shapes, full disable lifecycle incl. timer
freeze/resume and end-to-end delivery of a pre-disable latch,
SMP identity/window-follows-context/INIT-preservation) and
both snapshot cases in `tests/test_snapshot.c`. NC: the new
tests on `25faae1` — `test_lapic` aborts at the first U4
assertion; `test-snap` reports exactly the 9 U4 FAILs.
Adjacent finding (recorded, not fixed — outside D1): a spent
one-shot LAPIC timer re-latches its vector on every tick once
at zero (pre-existing H6 behaviour; masked-LVT guests are
unaffected).

### U5 — CF9 CPU INIT ✅

Prove `0x02` is consumed only at an instruction boundary;
sentinels distinguish CPU INIT from full reset across RAM,
RTC, PIC/PIT/KBC/IOAPIC/PCI and device state; pin edge/repeat
behaviour, `0x02 -> 0x06` coalescing, FADT `0x0E`, and all-vCPU
state under SMP. The implementation must not be an
active-context-only accident.

**Receipts (U5):** primary-source measurement corrected the
register's bit roles before any code moved: the Intel PCH
register databases (two generations) define SYS_RST(bit1,
0x02) as the reset *type* and RST_CPU(bit2, 0x04) as the
trigger whose 0->1 edge forces INIT# (SYS_RST=0) or a hard
reset (SYS_RST=1). The plan above and the pre-U5 code carried
the two names swapped (with identical hard-reset outcomes);
the missing INIT# transition is therefore the 0x04-from-clear
write, not a literal 0x02 — recorded here per the U0 rule that
the plan does not bless remembered constants. INIT# state is
SDM Vol.3A Table 9-1's INIT column, corroborated field-for-
field by QEMU `do_cpu_init`'s init-save region: x87 register
patterns ride through with a FINIT-shaped control file, XMM/
MXCSR/KERNEL_GS_BASE and the software MSRs stay (EFER/FS_BASE/
GS_BASE take their reset 0), LAPIC file resets except its ID
(10.4.7.3), APs park in wait-for-SIPI. The LAPIC INIT-IPI path
was unified onto the same `cpu_init_reset`, so both INIT
sources now agree archivally. The pending boundary request
became a kind in the existing field (1=full, 2=INIT#; an INIT
upgraded by a full source coalesces, an INIT behind a pending
full is absorbed); no `machine_t` movement, so snapshots are
untouched. Vectors: four new groups in `tests/test_chipset.c`
(guest OUT ritual with tombstone byte + PIC/PIT/KBC/RTC/RAM/
chipset-port sentinels, INIT CPU-state preservation, edge/
repeat/coalesce/absorb, SMP with BSP-writer and AP-writer),
plus the CF9 predicates vector rewritten to the datasheet
truth (0x06, 0x0E and 0x02->0x06 hard outcomes unchanged).
NC: the new test on `1acaf46` aborts at the first CPU_INIT
assertion (134). Adjacent finding, recorded not fixed:
fetching the reset vector with the A20 gate closed does not
follow the bus alias (only reachable by INIT# with the gate
deliberately shut; every reset source on this board opens it,
and guests are unaffected). — *Resolved later by
CPU_FEATURES CF5 (`0104`): reproduced in harness; the fix
widened the top-of-4G firmware decode to the top 2 MiB so
the A20-masked image of the reset window lands in flash;
see `docs/plans/CPU_FEATURES_PLAN.md` §CF5.*

### U6 — responder TCP ✅

Prove sequence arithmetic at and across 2^32 wrap, duplicate and
partial-overlap trimming, bounded out-of-order queueing,
contiguous-only application delivery, duplicate ACKs with up to
four correctly ordered SACK blocks only when negotiated, and
in-order FIN sequencing. Pin duplicate FIN in LAST_ACK, the
final-ACK passive close, and any reachable CLOSING/TIME_WAIT
transition with a deterministic retained-state bound. Existing
in-order handshake/DNS replies stay byte-identical.

**Receipts (U6):** the consumer contract was measured before
code moved. RFC 9293 §3.10.7.4: Table 6 acceptability, "trim
off any portions that lie outside the window (including SYN and
FIN)… Segments with higher beginning sequence numbers SHOULD be
held", unacceptable → `<SEQ=SND.NXT><ACK=RCV.NXT><CTL=ACK>`,
LAST-ACK deletes the TCB only on an ACK of our FIN, TIME-WAIT's
sole arrival is a remote-FIN retransmission: "Acknowledge it".
RFC 2018 §3–4: `8n+2` option bytes, at most four blocks, the
first block MUST hold the segment that triggered the ACK, no
SACK at all without the peer's SACK-permitted. Frozen guest
`0ed0d29`: `tcp_m6d.h`'s decoder takes at most
`TCPM6D_MAX_BLOCKS=4` blocks, rejects `(int32)(e-s)<=0`, is
consulted only when the peer negotiated, and `tcp.c` marks its
retransmission queue from the blocks while counting duplicate
ACKs per RFC 5681 §2 (`data_len > 0` or a changed window
un-counts a duplicate) — so dup ACKs here are data-less at the
constant window 64240, whose honesty the queue bound (≤32 runs,
bytes ≤ window, overflow discarded whole and never SACKed)
preserves. Implementation (`tools/tap_responder.py`):
modulo-2^32 sequencing against RCV.NXT and the window with
duplicate/left/right-overlap trimming; contiguous-only delivery
to the DNS stream (queued OOO runs are carved against earlier
arrivals — first copy wins — and drained in SEQ order); the
SYN's SACK-permitted is retained per connection (pre-U6 parsed
and dropped) and up to four merged blocks ride every ACK while
data is held, answers included; a FIN is sequence space and is
consumed only in order (an OOO FIN waits; queued post-FIN junk
is purged at consumption). Close: duplicate FIN in LAST_ACK
re-sends our FIN|ACK — the retransmission timer's job keyed
deterministically to the peer's retransmission, since this
responder has no clock by design — instead of freeing the
tuple; the final ACK still frees it (passive close unchanged),
now leaving a bounded FIFO tombstone (16 tuples,
count-evicted) that answers a late duplicate FIN with the
final ACK. CLOSING and our-side TIME_WAIT stay genuinely
unreachable (no active close exists here), so no states were
fabricated for the checklist word. Vectors: 71 checks in
`test_tcp_sequencing` (OOO + gap-fill delivery-once, partial
overlap, two-gap first-block and drain order, four-block cap,
no-SACK-without-negotiation, mid-query 2^32 wrap, OOO FIN,
dup FIN in LAST_ACK twice + tombstone, beyond-window discard,
right-edge trim, run-33 refusal, FIN-with-data, two-instance
byte replay). NC: the same file on `6d199cb` fails exactly 35
checks, all inside `test_tcp_sequencing` (e.g. the bound
vector's ack reads `0x90047` there vs `0x90007` — the old
blind sum). In-order identity: the unmodified suite is green
on the new code, and an A/B harness (pre-U6 vs new Responder
over a 17-frame handshake/two-queries/close/RST/stray script)
compares 16/16 pinned inputs byte-identical; the one
deliberate delta is junk data sent after the peer's FIN at a
bogus sequence, which pre-U6 advanced RCV.NXT into phantom
bytes and now earns a challenge ACK in LAST_ACK. Gates:
`make test` green with both live SeaBIOS ISO lanes, the
N1/N2/N3 wire lanes' A/B/D legs passing over the real socket
(frozen-guest leg C skips — no AuraLite-OS build in this
sandbox, as in prior phases), `make test-sanitize` green
(Python-only change). Adjacent finding, recorded not fixed
(outside D5): a bare SYN to a live tuple still restarts its
handshake state (a pre-U6 permissiveness the 16-concurrent
vector uses as a probe); RFC 9293 would challenge-ACK it, but
the measured guest never emits one outside SYN_RCVD
retransmission.

### U7 — GUI/monitor authentication

Define and test the threat model (malicious page, local
process, explicit LAN bind). Prove missing/wrong capability,
cross-origin mutation/WebSocket and DNS-rebinding Host requests
are refused; the generated capability is carried internally by
desktop and web GUI flows with no command line or manual token
handling in the default path; a non-loopback bind requires an
explicit GUI-managed credential and a plaintext warning.
Direct-monitor behaviour must have an equally deliberate local
bootstrap, not a second unauthenticated back door.

Closed. Threat model (§1.7, now implemented): (T1) a hostile
web page — cross-origin fetch/form/WebSocket and DNS
rebinding against a loopback service; (T2) another local
process, stopped by a per-run/per-process capability it cannot
read unless it can already read the GUI state directory or
the child's environment (the honest, documented boundary, in
the `monitor.c` and `launcher.py` headers); (T3) any network
peer once bound beyond loopback — the transport stays
plaintext HTTP, the token is the only credential, and both
servers say so on startup. Wire basis RFC 6455 §4.2.2/§10.1/
§10.2 (Origin is unforgeable *by browsers* and arbitrary for
everything else — hence both checks, neither alone).
Launcher: every `/api/*` route and the `/ws` splice require
the token (`X-X86emu-Token`, `Authorization: Bearer`,
`?token=`, or the `x86emu_token` HttpOnly SameSite=Strict
cookie), the Host must name this machine (403), and mutations
plus `/ws` require a same-origin Origin when one is sent
(403); token failures are 401. On a loopback bind the token
is per-process random and is injected into the served page
(+ cookie, `Cache-Control: no-store`), so the one-click flow
is unchanged; beyond loopback the credential is the
persistent state-dir file `access-token` (0600,
`load_or_create_access_token`), the page embeds nothing
until it is presented (login form), and the desktop GUI
displays and rotates it (expert → "Network"), no terminal
involved. Each machine run gets its own monitor capability
delivered to the emulator in `X86EMU_MONITOR_TOKEN` — never
in argv, so it stays out of process lists and `argv.json` —
and the proxy presents it to the backend monitor. Direct
monitor (`src/monitor.c`): `--monitor-token=` or the same
env var; a Host allowlist (loopback names + the bound
literal; `0.0.0.0` also admits IP-literal Hosts); Origin
must be same-origin with the request Host on the upgrade;
the token gates the upgrade only (the static page is inert
and stays fetchable); a non-loopback bind with no token is
refused before any socket exists. Tokenless loopback keeps
its pre-U7 shape apart from the Host/Origin checks — the
deliberate local bootstrap, with §1.7's residual risk
explicit: a tokenless direct monitor relies on Origin/Host
alone against local non-browser processes, which is why the
GUI's spawns are always tokened. `web/monitor.html` passes a
`?token=` from the page URL into its WebSocket URL.
Vectors: `tests/test_monitor.py` lane `auth` (16 checks;
the 55 pre-U7 tokenless checks untouched and green),
`tests/test_gui_launcher.py` lane E (13 loopback + 9 remote
checks; happy paths now scrape the page token exactly like
a browser), `tests/test_gui_app.py` lane C (tokenless
client refused at the upgrade; token absent from
`argv.json`). NC against `41a850e`: test_monitor 10/16 FAIL
(all refusals/warnings; the positive carriers pass there
because the old monitor upgrades *anyone*), launcher 22
FAILs all U7-labelled, gui_app exactly the one refusal
check. Gates: `make test` and `make test-sanitize` green
with both live SeaBIOS ISO lanes, `make test-gui`,
`test-monitor`, `test-monitor-codec` green; guest-visible
state untouched (no snapshot/RTC/device change — the token
lives outside `machine_t`, and host time is consulted by no
token code path; tokens do not expire, so the deterministic
no-wall-clock rule is kept literally). Adjacent fix, in
scope because it is the same GUI state surface:
`tools/gui_app.py` gained the missing `import subprocess`
(a latent NameError in `open_state_dir` on non-Windows).
Recorded, not changed: the gdbstub remains
unauthenticated-by-protocol; its default loopback bind and
the docs' operator warning are its boundary (U7/D7 covered
the GUI and monitor surfaces).

## 4. Gates

Per phase: its named suite, one behavioural negative control,
`make test`, `make test-sanitize`, and both SeaBIOS ISO lanes
with the stock BIOS actually present. Snapshot lanes are
mandatory for U1/U4/U5, network end-to-end lanes for U2/U6,
real-client or pinned-fixture GDB lanes for U3, and GUI +
monitor lanes for U7. U0 is docs-only; its receipts are the
targeted suites in §0 plus clean patch checks.

## 5. Close-out (U7)

The plan is complete: D1–D7 are all closed (U1 RTC, U2
e1000, U3 gdbstub x87, U4 LAPIC APIC_BASE, U5 CF9 INIT, U6
responder TCP, U7 GUI/monitor authentication), each with its
own patch (`0092`–`0098`), regression vectors and one
behavioural negative control, and the row-4 "small honest
gaps" entry in `ROADMAP.md` is closed with them. The only
snapshot-layout statement in the whole plan remains U1's
(pre-U1 snapshots are rejected; U2–U7 changed no layout).
Open, by deliberate record only: the reset-vector fetch
under a closed A20 gate observed at U5 (since resolved by
CPU_FEATURES CF5, `0104`), the responder's
bare-SYN tuple restart noted at U6, and the gdbstub's
protocol-inherent lack of authentication (loopback by
default, operator's choice) noted at U7. None of the three
is inside D1–D7; each is named here so a future plan can
start from the measurement, not from rediscovery.
