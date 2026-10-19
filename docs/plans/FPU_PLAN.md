# FPU Plan — a real x87 for the emulator

> House style (AuraLite-OS/docs/plans): dependency-ordered phases, each with
> a definition of done and a test gate, shipping as **one `.patch` per
> phase** under `patches/`. Everything below is measured against the tree,
> not assumed. Baseline measured 2026-10-18 on
> `AuraLite-Emulator@30d1c5d` (post G2/S6, x86-64 Linux reference host).

**Status: F0–F4 done ✅**

| Phase | Scope | Status | Deliverable |
|---|---|---|---|
| F0 | Baseline measurement + this plan (see §1 receipts) | ✅ done | rides with F1 |
| F1 | x87 state and data movement: the physical register file (80-bit IEEE 854) + TOP + FSW/FTW/FCW, FLD/FST/FSTP (m32/m64/m80 + ST(i)), FILD/FIST/FISTP (m16/m32/m64), FLD constants, FXCH/FFREE/FINCSTP/FDECSTP/FNOP, FNSTSW(AX/m16)/FNSTCW/FLDCW/FNINIT/FNCLEX, FNSTENV/FLDENV/FNSAVE/FRSTOR, live x87 in FXSAVE/FXRSTOR, CR0.EM/TS traps (#UD/#NM), rounding per FCW.RC with #MF on unmasked precision/underflow/overflow | ✅ done | `patches/0072-FPU-F1-x87-state-and-data.patch` |
| F2 | Arithmetic and compare: FADD/FSUB/FSUBR/FMUL/FDIV/FDIVR (+P/+I/+R forms), FSQRT/FRNDINT/FPREM/FPREM1/FSCALE/FXTRACT, FCOM/FUCOM/FICOM families + FCOMI/FCOMIP/FUCOMI/FUCOMPP (FCHS/FABS/FTST/FXAM/FCMOV* landed early in F1 -- they are data/status, not arithmetic) | ✅ done | `patches/0073-FPU-F2-arithmetic-and-compare.patch` |
| F3 | Transcendentals + BCD: FSIN/FCOS/FSINCOS/FPATAN/FPTAN/F2XM1/FYL2X/FYL2XP1, FBLD/FBSTP | ✅ done | §1d receipts (`/tmp/transprobe{,2..7}`, `/tmp/bcdprobe`); 11 new vector groups; NC red with 114 failures |
| F4 | Integration: a `--kernel=` freestanding payload receipts pair (determinism x2), snapshot byte-stability of the x87 file, STATUS close-out | ✅ done | `tests/fpu_payload.c` + `tests/test_fpu_gate.c` + `t4_snapshot_lanes.sh` f4 lanes; `x87_w` padding fix (`patches/0075`) |

## 1. Measured baseline (F0)

The machine **advertises** an FPU today: CPUID.1.EDX bits 0 (FPU) and 24
(FXSR) are set (`src/platform.c`), `CR0` powers on with `ET=1`, and
`cpu_t.fcw` resets to the architected `0x037F`. What the decoder actually
executes (probe: `tests/test_fpu.c` style flat machine, program at 0):

| bytes | instruction | measured today |
|---|---|---|
| `D9 E8` | FLD1 | **no fault**; decoded as `FLDCW /5` in *register* form -- FCW loaded with AX (`fcw=0000`) and nothing pushed (defect **D1**) |
| `D9 EE` | FLDZ | same misdecode as FLD1 (D1) |
| `DB E3` | FNINIT | accepted, but a no-op -- the register file does not reset (defect **D2**) |
| `DB E2` | FNCLEX | accepted, but a no-op (D2) |
| `DF E0` | FNSTSW AX | `#UD unsupported opcode 0xdf` (defect **D3**) |
| `D8 C1` | FADD ST(0),ST(1) | `#UD unsupported opcode 0xd8` (defect **D3**) |
| `DB /3` | FSTP m80 | `#UD x87 data op out of scope (K3)` (the honest K3 boundary) |

FXSAVE (0F AE /0) writes a **powered-on** x87 block (FCW 0x037F, zeros
elsewhere) regardless of live state -- fine while no x87 data exists,
wrong the moment it does (defect **D4**: K4's own contract says context
images must round-trip what the guest computed).

Probing the HOST CPU for the real FXSAVE/FNSTENV images (the project
standard: measure, don't trust the manual) surfaced one more baseline
defect: the FXSAVE writer put every x87-prefix field two bytes off and
stored MXCSR into the MXCSR_MASK slot (`addr+28`) with the MASK in the
ST0 area (`addr+32`) -- only invisible because FXRSTOR read the same
wrong slots back (defect **D5**, closed by F1 against the host-measured
layout: FCW@0 FSW@2 FTW@4(1B) FOP@6 FIP@8 FCS@12 FDP@16 FDS@20 MXCSR@24
MASK@28 ST(i)@32+16i logical XMM@160, pinned byte-exact by
`test_fpu.c::v_fxsave_live_layout`).

Defects D1–D5 are what F1 closes; the negative control reverts the F1
escape dispatch wholesale (measured: 104 vector failures, `test-fpu-nc`).

## 1b. F1 measured receipts (host probes + vectors)

- **80-bit constants** (`/tmp/const_probe` = host `fld1`..`fldz` dump):
  FLD1 `3FFF 8000 0000 0000 0000`, FLDL2T `4000 D49A 784B CD1B 8AFE`,
  FLDL2E `3FFF B8AA 3B29 5C17 F0BC`, FLDPI `4000 C90F DAA2 2168 C235`,
  FLDLG2 `3FFD 9A20 9A84 FBCF F799`, FLDLN2 `3FFE B172 17F7 D1CF 79AC`,
  FLDZ zero. The engine loads exactly these patterns.
- **FNSTENV images** (`/tmp/envprobe2`): 14-byte form is
  CW@0 SW@2 TW@4 FIP16@6 FCS@8 FDP16@10 FDS@12; the 28-byte (32-bit
  opsize) form interleaves `FFFF` words: CW@0 rsv@2 SW@4 rsv@6 TW@8
  rsv@10 FIP32@12 FCS@16 FOP@18 FDP32@20 FDS@24 rsv@26. FNSAVE appends
  ST(0)..ST(7) in **logical** order, 10 bytes each (94/108 bytes total).
- **FFREE ST(i) is stack-relative** (`/tmp/ffree`: FFREE ST(1) with
  TOP=6 frees phys7, the stack slot -- not physical register 1).
- **Stack faults** are off-by-one sensitive: overflow fires when the
  *destination* slot of a push is occupied (the 8th legal push onto an
  empty file is silent), underflow when popping an *already* empty ST(0)
  (a pop that merely empties the file is legal). Both were wrong in the
  first cut and are pinned by `v_stack_faults`.
- **FINCSTP/FDECSTP** move TOP without touching tags and never fault.
- **#MF gating** must consider only the six FCW-maskable bits (PE..IE);
  the sticky SF bit is not an exception and must not force #MF.

## 1c. F2 measured receipts (raw-byte probes)

- **Operand directions** (`/tmp/rawdir`, raw `db` bytes -- GAS swaps the
  `fsubp`/`fsubrp` mnemonics, so mnemonics are not an oracle):
  D8 reg = `OP ST(0),ST(i)` (dest ST0), DC reg = `OP ST(i),ST(0)`
  (dest ST(i)), DE reg = same as DC then pop; FSUBR/FDIVR swap the
  values (D8 E8: st0 = sti-st0; DC E0: sti = st0-sti). Memory forms on
  D8/DC/DA/DE all share the NON-reversed /4-/7 map (DC /4 = FSUB m64,
  /5 = FSUBR -- matches ndisasm).
- **FSW/FCW exception-bit order (defect D6)**: the F1 map was reversed.
  Host-measured (`/tmp/flagbits`, isolated FNCLEX per op):
  **IE=0x0001 DE=0x0002 ZE=0x0004 OE=0x0008 UE=0x0010 PE=0x0020**
  (denormal operand -> DE alone; overflow -> OE+PE; underflow ->
  UE+PE). The FCW mask bits mirror the same order (IM=bit0..PM=bit5);
  the F1 vectors that pinned the wrong bits were corrected with F2.
- **Compare matrix** (`/tmp/cmpmat`): FCOM/FCOMI/FCOMP raise IE on ANY
  NaN; FUCOM/FUCOMI raise IE only for SNaN (QNaN stays quiet); all
  unordered results give C0=C2=C3=1 (C-flag form) or CF=PF=ZF=1
  (EFLAGS form); a denormal source sets DE but compares normally.
- **FPREM/FPREM1 quotient flags**: C0=Q2 C3=Q1 C1=Q0, C2=1 while
  partial (17 mod 3: FPREM -> 2.0 with 5=101b -> C0=1 C1=1 C3=0;
  FPREM1 -> -1.0 with 6=110b -> C0=1 C3=1 C1=0).
- **FXTRACT**: after the op ST0 = significand (original sign) and
  ST1 = the unbiased exponent as a real (the push lands the significand
  on top); measured with raw `D9 F4`.
- **Converting loads vs m80**: FLD m32/m64 of an SNaN quiets it and
  raises IE (the conversion is a real x87 load); FLD m80 is a pure move
  -- SNaN stays signaling, no exception (host receipt) -- which is why
  the FUCOM-SNaN rule is testable through m80 loads.
- **FRNDINT/FSQRT/FSCALE honour FCW.RC** through the host control word
  (the F2 core sets `fesetround` before every numeric op).

## 1d. F3 measured receipts (transcendentals + BCD)

Host oracles `/tmp/transprobe{,2..7}` + `/tmp/bcdprobe` (raw encodings,
FNINIT-isolated per case -- the first probe run taught that FNCLEX does
NOT clear C1, so unpinned flags were polluted; every number below is
from an isolated run).

- **Semantics**: FPATAN = `atan2(y=ST1, x=ST0)` with the full signed-zero
  quadrant table (`atan2(+0,-0)=+pi`, `atan2(-0,+0)=-0`, `atan2(±0,+inf)=±0`);
  FYL2X = `ST1*log2(ST0)`; FYL2XP1 = `ST1*log2(1+ST0)`.
- **Range-reduction boundary (trig four)**: `|x| >= 2^63` (exact
  `2^63-eps` still computes) -> source UNCHANGED, C2=1, C1=0, quiet, and
  FPTAN/FSINCOS do **not** push at all.
- **Push-op shape**: FPTAN -> `[1.0, tan(x)]`, FSINCOS -> `[cos, sin]`
  (ST0 on top). Inf input -> QNaN-indefinite in BOTH slots + IE; QNaN ->
  the source NaN twice; SNaN -> its quieted form twice + IE. On a full
  stack the op still pushes (ftop moves) but BOTH the old ST0 and the
  push slot become indefinite with IE|SF|C1=1 (measured on distinct
  canaries).
- **NaN folding (two NaN inputs)**: QNaN beats SNaN in either position;
  two QNaNs or two SNaNs -> the larger payload, quieted. Single SNaN ->
  that NaN quieted + IE. (Undefined by SDM; pinned as host receipts.)
- **F2XM1 domain quirk**: finite `|x| > 1` returns the SOURCE with PE
  (1.5 -> 1.5, -2 -> -2); `+inf -> +inf`, `-inf -> -1.0` (the identity
  `2^x-1` survives at infinities).
- **FYL2X corners**: `x=0, y finite nonzero` -> `-sign(y)*inf` + ZE;
  `x=0, y=±inf` -> `-+inf` WITHOUT the ZE (host quirk!); `x=0, y=±0` or
  `y=0, x=+inf` -> indefinite + IE (0*inf); `x<0` -> indefinite + IE;
  `x=+inf` -> `y*inf` folding. FYL2XP1 `x <= -1` -> the host returns
  ST0 itself for finite nonzero ST1 (with PE), `ST0*ST1` for
  ST1 in {0, ±inf}; `x=-inf` -> indefinite + IE.
- **C-flag policy (probes 6/7)**: all eight ops WRITE C1 (the round-up
  indicator: FSIN(0.5) C1=1, F2XM1(0.5) C1=0 under RC=nearest); the trig
  four also WRITE C2; C0/C3 -- and C2 on F2XM1/FYL2X/FYL2XP1/FPATAN --
  are preserved on silicon (preset survives), so the engine preserves
  them too and never copies unmeasured host bits.
- **FBLD**: a pure weighted nibble sum -- nibble values A-F count as
  10-15 with NO invalid-digit exception; sign = bit 7 of byte 9 (the
  low nibble ignored); always exact, no flags, C flags untouched.
  (The all-FF byte pattern is NOT an "indefinite" for FBLD: it loads
  -1666666666666666665.)
- **FBSTP**: rounds to an integer per FCW.RC (ties-to-even under
  RC=nearest), PE iff the value changed, C1 iff the magnitude rounded
  UP (1.5 down -> 1 C1=0; -1.5 down -> -2 C1=1; 2.5 near -> 2 C1=0),
  source sign kept even for a zero result (FBSTP(-0.9, up) -> -0);
  NaN/Inf/rounded `|v| >= 1e18` -> IE + the QNaN-indefinite byte pattern
  (mem `00 x7 C0 FF FF`); denormal sources do NOT raise DE (measured);
  C0/C2/C3 untouched (measured: preset survives).

## 1e. F4 measured receipts (integration gates)

- **In-guest receipt pair** (`tests/fpu_payload.c`, booted through the
  `--kernel=` loader core twice on fresh machines; COM1 lines pinned by
  `tests/test_fpu_gate.c` and by the t4 CLI lane). Each block is
  FNINIT-anchored; bytes are 80-bit stores in memory order:

  | line | content | measured bytes |
  |------|---------|----------------|
  | `F4R0=3800` | FNSTSW after FNINIT+FLD1 (TOP=7, clean) | — |
  | `F4R1=` | FLD1 -> FSTP m80 | `0000000000000080ff3f` |
  | `F4R2=` | FLDPI -> FSTP m80 | `35c26821a2da0fc90040` |
  | `F4R3=` | FSIN(0.5) -> FSTP m80 (host-silicon payload) | `447f2f58a24377f5fd3f` |
  | `F4R4=3a20` | FNSTSW after FSIN(0.5) (TOP=7 \\| C1 \\| PE) | — |
  | `F4R5=` | FBSTP(1234.0) (packed BCD, LSD-first) | `34120000000000000000` |
  | `F4R6=` | 1.0+1.0 = 2.0 through FADD | `00000000000000800040` |
  | `F4R7=` | FBLD(1234 BCD) -> FSTP = 1234.0 | `000000000000409a0940` |

  (R5/R7 also pin the Intel packed-BCD byte order for the file: LSD
  digit-pair in byte 0, sign byte at the end -- the m80 byte order of
  1234.0 is `00 x6 40 9A 09 40`, NOT `.. 9A 09 40` with 9A in byte 7.)

- **x87 file byte-stability -- the measured defect.** The fst slots are
  `long double` (10 data bytes + 6 padding) and `machine.lo/machine.hi`
  dump `cpu_t` RAW. FNSAVE and FXSAVE emit explicit bytes (10, and
  10+zeroed reserved) so the guest formats never leaked, but a COMPUTED
  long double assigned to a slot (measured at FBLD's accumulator) copied
  its temporary's uninitialized -- run/ENV-dependent -- stack bytes into
  the padding. Consequence: two processes saving the same state produced
  different snapshot files (flaky standalone; red under `make`'s
  environment; 2 bytes inside `fst[7]`'s padding). The fix is `x87_w` in
  `src/cpu.c`: every one of the 24 fst write sites zeroes the slot's 6
  padding bytes, keeping the raw dump byte-stable.

- **CLI contract notes (measured while wiring the lanes).** (1) The
  load+save fixed point is "nothing executes": `--max-instr` compares
  against the RESTORED `run_steps`, so use the saved budget or
  `--max-instr=0`; a resumed halted run advances `s_devtick` once (the
  wake check is device time -- by design, not a snapshot bug). (2) The
  snapshot shape pins `rom_hash` and `kmain_va`, so a resave run must
  pass the same `--kernel=` or the load refuses ("a different ROM
  image").

- **Vectors / NC.** `tests/test_fpu_gate.c` (lane 1 receipts pair x2 +
  pinned content; lane 2 fresh-run byte-identity + save->load->save) and
  `t4_snapshot_lanes.sh` (f4-payload split 20/2000000, receipts pinned in
  the machine log, two-process file identity, `--max-instr=0` fixed
  point). NC (engine reverted to F2): the payload dies `#UD x87
  transcendental out of scope (F3)` -> gate red. NC (run-dependent bytes
  injected at the fst dump): both lanes red at the first padding byte.

## 2. Architecture decisions (F1)

- **Format.** The x87 register file holds IEEE 854 *double-extended*
  (80-bit) values in physical order (`fst[0..7]` + `ftop` == FSW.TOP), the
  exact layout FXSAVE/FNSAVE serialize. On hosts where C `long double` IS
  that format (`LDBL_MANT_DIG == 64`, x86-64 gcc/clang and MinGW-w64), the
  numeric core runs on the host unit with `fesetround()`/`fetestexcept()`
  mapping FCW.RC and the exception flags; conversion ops (m32/m64/ints)
  then round exactly once, as the hardware does. On any other host the
  data opcodes keep today's `#UD` boundary -- an honest compile-time
  boundary instead of silently wrong precision.
- **Traps.** ESC with `CR0.EM=1` raises `#UD`; ESC/WAIT with `CR0.TS=1`
  raises `#NM` (vector 7); arithmetic/conversion exceptions accumulate in
  FSW and raise `#MF` (vector 16) when unmasked. F1 delivers #MF for the
  conversion ops it owns; F2 owns the arithmetic surface.
- **Tags.** FTW is kept full (2 bits per physical register) and abridged
  to the FXSAVE 8-bit form on save. FNINIT resets the whole file (F1's
  fix of defect D2). FIP/FOP/FDP track the last executed data op (the
  env/status/control group leaves them alone; FNINIT clears them) --
  matching where the host puts FIP after a data sequence.
- **Rounding & flags.** FCW.RC drives `fesetround` per op; host
  `fetestexcept` bits fold into FSW (PE UE OE ZE IE). Integer stores
  round through `rintl` (the raw C cast would truncate regardless of RC),
  FISTTP always truncates. Default FCW (all six masked) keeps stack and
  conversion faults sticky-but-silent, as the hardware does.
- **gdbstub is not touched.** The GDB `g`/`g`-packet layout advertised by
  the T3 XML stays GPRs+XMMs; x87 registers are not part of that contract.

## 3. Definition of done

- F1: `tests/test_fpu.c` vectors green under `make test-unit` and
  `make test-sanitize` (identical bytes saved/restored through
  FNSAVE/FRSTOR and FXSAVE/FXRSTOR, rounding receipts vs known 80-bit
  constants and host-conversion results, D1 NC red on revert), guest
  lanes byte-identical (the sample firmware and the frozen K-lanes never
  execute x87 data; FNINIT stays a *state* reset now, receipts unchanged),
  docs synced (this plan, `docs/STATUS.md`, CHANGELOG).
- F2–F4: rows above, same rules.

## 4. Test gate

`make test-unit` runs `test-fpu` beside `test-cpu`; `make test` and
`make test-sanitize` both carry it. Frozen receipt invariant for every
F-phase: the 5-platform smoke, USB, NET and BOOT lanes stay byte-stable.
