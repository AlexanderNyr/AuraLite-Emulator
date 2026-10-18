# VIDEO plan — display hardware (VGA text + Bochs VBE)

> **House style** (adopted from
> [`AuraLite-OS/docs/plans`](https://github.com/AlexanderNyr/AuraLite-OS/tree/main/docs/plans)):
> each effort is a dependency-ordered plan in `docs/plans/` with a
> definition of done and a test gate per phase, and ships as **one
> `.patch` per phase** under `patches/`. Every claim in these docs is
> measured against the tree, not assumed.

The display stack the machine owes the guest, measured from
`AuraLite-OS@0ed0d29` and the QEMU recipes that gate it (`-vga std` in
`tools/run_qemu.sh` / `tools/run_qemu_usb_msc.sh`): a **VGA text mode 3**
console at `0xB8000` (the kernel's BIOS-path fallback writes cells and
the CRTC cursor there) and the **Bochs VBE dispi** interface (ports
`0x1CE`/`0x1CF`) behind QEMU's std-VGA identity, with mode switching
between the two. The linear framebuffer at `0xD0000000` (GPU VRAM,
published through `boot_info.fb`) is live since the KERNEL-BOOT phases
and stays the graphics path; this plan makes the *legacy + VBE* surfaces
honest so the BIOS/stage2 path and any VBE client have real hardware.

**Status: complete — V0 ✅, V1 ✅, V2 ✅, V3 ✅ (plan closed), plus V4 ✅ — a
post-closure heap-overflow fix in the dispi mode clamp, found by ASan after the
plan was closed and shipped as its own phase**

| Phase | Scope | Status | Deliverable |
|---|---|---|---|
| V0 | Measured baseline + this plan (defect ledger: no VGA text rendering, no CRTC, no Bochs VBE dispi, no mode state) | ✅ done | `patches/0042-VIDEO-V0-video-plan.patch` |
| V1 | VGA text mode 3: `0xB8000` text window + CRTC cursor pair (0x3D4/0x3D5), glyph rendering into the display output, `--no-fb` lane so the guest's `fb.c` fallback engages | ✅ done | `patches/0043-VIDEO-V1-vga-text.patch` |
| V2 | Bochs VBE dispi (0x1CE/0x1CF): the `VBE_DISPI` register file, LFB window + video-memory sizing (the QEMU `-vga std` contract) | ✅ done | `patches/0044-VIDEO-V2-vbe-dispi.patch` |
| V3 | Mode switching (text 3 ↔ VBE graphics), determinism ×2, sanitize, docs close-out | ✅ done | `patches/0045-VIDEO-V3-video-mode-switch.patch` |
| V4 | **Post-closure fix**: `dispi_fixup` was a partial transcription of QEMU's `vbe_fixup_regs` — the YRES-to-VRAM clamp was missing, so enabling any mode bigger than 800×600 overflowed the host heap (defect **D6**) | ✅ done | `patches/0050-VIDEO-V4-vbe-mode-clamp.patch` |

Note: the *guest* keeps its own roadmap (`AuraLite-OS/docs/BOOTLOADER_ROADMAP.md`
BL1–BL8) and its display driver consumes `boot_info.fb` (Limine/GOP or
the emulator's direct-load lane). This plan closes the **emulator's
video hardware** gaps those paths assume. The guest's `fb.c` fallback
and `stage2_start.asm` mode-3 contract are the acceptance surface.

## 1. Measured baseline

### 1.1 What the machine presents today (`src/devices.c`, `src/kloader.c`)

- **GPU PCI 0:2:0** — `0x8086:0x0412`, class `0x03/0x00/0x00` (the
  comment records the firmware convention: "GPU: dev2/func0 (real Intel
  iGPU convention)"; class mirrored at the offset the firmware reads).
- **GPU VRAM / framebuffer** — a RAM window at `0xD0000000`
  (`make_ramwindow`, `fb_w*fb_h*4` bytes = 800×600×4); `m->fb` points
  into it; the kloader publishes it in `boot_info.fb` (`fb 800x600 @
  0xD0000000` in every kernel-lane `[kload]` receipt) and in the e820
  map as `BOOT_MEM_FRAMEBUF`. The sample firmware's `enable:` code finds
  the same window (it writes the `0x00200000` pixel marker the boot
  integration gate pins: `framebuffer marker OK: pixel[0]=0x00200000`).
  `--dump-fb` writes the LFB out as `framebuffer.ppm`.
- **VGA text: nothing.** `0xB8000` sits inside the flat 128 MiB RAM
  map, so guest cell writes survive as ordinary RAM — but no VGA
  display path exists: nothing renders the buffer, and the kloader's
  e820 marks `0xA0000–0x100000` reserved ("legacy VGA hole + our
  ROM-alias shadow window"). No VGA register file: `0x3C0–0x3DF` are
  unregistered I/O and float high (`io.c` open-bus rule).
- **Bochs VBE: nothing.** `0x1CE`/`0x1CF` float high; there is no mode
  state anywhere in the machine.

### 1.2 What the guest expects (measured, `AuraLite-OS@0ed0d29`)

- **VGA text mode 3 console** (`drivers/framebuffer/fb.c:24–34`):
  "BIOS Stage 2 never programs VBE: boot_info.fb is zeros and the
  monitor is still in VGA text mode 3" — the fallback writes 80×25
  `uint16` cells at **`0xB8000`** with attribute `0x07`
  (`VGA_TEXT_PHYS`, `VGA_COLS 80`, `VGA_ROWS 25`, `VGA_ATTR 0x07`),
  scrolls by memmove between cells, and moves the hardware cursor
  through the CRTC index/data pair (`fb.c:129–132`): `outb(0x3D4,
  0x0F); outb(0x3D5, lo); outb(0x3D4, 0x0E); outb(0x3D5, hi)`.
  `fb.h:20`: after `paging_init()` the kernel punches a 4 KiB UC map of
  `0xB8000` "so the BIOS [text console works]".
- **Stage 2 forces mode 3** (`boot/bios/stage2/stage2_start.asm:78`):
  "Force VGA text mode 3. SeaBIOS starts there; a leftover graphics
  mode (or a firmware that never programmed the CRTC) would leave the
  monitor dark for the entire BIOS path — there is no VBE linear
  framebuffer in this loader." (`mov ax, 0x0003; int 0x10` — the INT
  0x10 mode-set is a *firmware* service; the hardware contract is the
  mode-3 state this plan's V1/V3 provide.)
- **QEMU `-vga std`** (`tools/run_qemu.sh:51`,
  `tools/run_qemu_usb_msc.sh:30`) — the Bochs VBE (dispi) machine the
  guest names in its own tables: `drivers/vm/virtual_drivers.c:57` maps
  PCI `1234:1111` to `"QEMU/Bochs std VGA"` / `"limine-framebuffer"`, and
  `drivers/framebuffer/render3d.c:93` identifies the same id before
  rendering through the LFB.
- **GUI/3D lanes** (`tests/integration/cases/test_3d_render.sh`,
  `test_gui_usb.sh`, `test_doom.sh`) render through the linear
  framebuffer in software (`[3d] demo complete` receipts) — those run
  against `boot_info.fb` and already work on the direct-load lane; they
  are non-regression lanes for V1–V3.

### 1.3 The dispi contract to implement (QEMU hw/display/vbe.h + vga.c)

Ports `0x1CE` (index select — QEMU stores the index verbatim, no
auto-increment: `vbe_ioport_write_index`) / `0x1CF` (data, word-sized);
registers `VBE_DISPI_INDEX_ID=0x00` (accept `ID0..ID5` = `0xB0C0..0xB0C5`
verbatim and read back the accepted value — the client-side ladder is
write-then-read; anything else is rejected and the previous value
survives), `XRES=0x01` (accept `<= 16000` and `val & 7 == 0`, QEMU
`vbe_ioport_write_data`), `YRES=0x02` (`<= 12000`), `BPP=0x03`
(`0 -> 8`, accept `4/8/15/16/24/32`), `ENABLE=0x04` (`ENABLED=0x01`,
`GETCAPS=0x02`, `8BIT_DAC=0x20`, `LFB_ENABLED=0x40`, `NOCLEARMEM=0x80`),
`BANK=0x05` (`val &= (vram_size >> 16) - 1`, `bank_offset = val << 16`),
`VIRT_WIDTH=0x06`, `VIRT_HEIGHT=0x07`, `X_OFFSET=0x08`, `Y_OFFSET=0x09`,
`VIDEO_MEMORY_64K=0x0A` (read-only, `vram_size / 64K`).  With `GETCAPS`
set in ENABLE, reads of XRES/YRES/BPP return the maxima (16000/12000/32)
— QEMU's capability probe.  Enabling resets `VIRT_WIDTH`/`X_OFFSET`/
`Y_OFFSET` to 0 (QEMU's 0->1 transition); disabling restores the
machine's previous display.  The LFB is the device's video memory —
here the existing `0xD0000000` VRAM window.

## 2. Defect ledger (drove V1–V3; **D6** was found after the close-out and drove V4)

1. **No VGA text display path** — `0xB8000` is plain RAM; the guest's
   BIOS-path console is invisible (`--dump-fb` shows only the LFB).
2. **No CRTC register file** — the guest's `vga_move_cursor` I/O
   (`0x3D4/0x3D5`) hits open bus; the cursor never exists.
3. **No Bochs VBE dispi** — `0x1CE/0x1CF` float high; any VBE client
   (stage2-with-VBE, Limine-style loaders, mode-setting tools) sees
   nothing to talk to.
4. **No mode state** — text mode 3 vs VBE graphics does not exist as
   machine state, so nothing can switch between them.
5. **No text-mode display output** — even with (1), the display surface
   must show the text console when the machine is in mode 3 and the LFB
   when VBE is enabled (the visible-display contract `--dump-fb`
   renders).
6. **D6 — the dispi mode registers were not clamped to VRAM, so enabling
   a mode bigger than the window overflowed the host heap** (found after
   V3 closed, fixed by V4). `dispi_fixup` was a partial transcription of
   QEMU's `vbe_fixup_regs`: it raised `VIRT_WIDTH` to `XRES` and derived
   `VIRT_HEIGHT`, but it never clamped `YRES` — the one adjustment that
   bounds QEMU's clear-to-screen. Repro, measured on the built emulator
   with no guest involved:

   ```
   ./x86emu --platform=haswell --vbe=1024x768x32  --max-instr=1000
     unsanitized: RC=134, `free(): invalid pointer`
     ASan+UBSan:  RC=1,   heap-buffer-overflow WRITE of size 3145728
                          at dispi_data_write, src/devices.c:1055
                          "0 bytes after 1920000-byte region"
   ./x86emu --platform=haswell --vbe=1600x1200x32   -> WRITE of size 7680000,   RC=1
   ./x86emu --platform=haswell --vbe=16000x12000x32 -> WRITE of size 768000000, RC=139 (segfault)
   ./x86emu --platform=haswell --vbe=800x600x32     -> RC=0 (the documented mode)
   ```

   The overflow is ~400× the window in the extreme case, i.e. any guest
   or CLI user that asks for a common resolution like 1024×768 corrupts
   host memory. This is the same class QEMU fixed as CVE-2016-3712 — its
   commit note says the point of `vbe_fixup_regs` is that "the sanity
   checks … are effective", and without them "which is good for one or
   another buffer overflow".

## 3. Phases

### V0 — Measured baseline + this plan — ✅ done (patches/0042-VIDEO-V0-video-plan.patch)

This document: baseline (§1) + defect ledger (§2) + the phase ladder.

**Gate:** `make test` green (no code changes).

### V1 — VGA text mode 3 ✅ shipped

**Result (measured):** the BIOS box now has a real text display.  The
guest's own fallback path (`fb.c:215` `VGA_TEXT_PHYS=0xB8000`) drives an
emulated mode-3 console rendered with the guest's own PSF2 glyphs
(256/256 byte-identical to `psf2_default_font.inc` — the PD Linux
console VGA 8x16 font, extracted to `src/vga_font.h`).

**Model (src/devices.c VGA section):**
- cells: plain guest RAM at `m->ram+0xB8000` (mem.c owns everything below
  `RAM_SIZE`, so the flat RAM path *is* the cell buffer — reads/writes
  are the guest's own, no MMIO window involved);
- CRTC index/data pair at `0x3D4`/`0x3D5` (`crtc[256]`, `crtc_idx`);
  the guest's `vga_move_cursor` recipe (idx 0x0E/0x0F) round-trips;
- display routing: reset = text mode 3 ("SeaBIOS starts there"); GPU
  BAR0 programmed (the firmware's own `enable:` CFG write) or
  `boot_info.fb` published (kloader) flips the display to the LFB;
  `--no-fb` (`machine_t.cfg_no_fb`) locks it to the text console.
  V2/V3 replace both flip hooks with the real dispi mode state;
- render: 8x16 glyphs 1:1, 80x25 console = 640x400 at the panel
  top-left (800x600 canvas, black around), VGA/EGA 16-colour palette;
  graphics mode copies `m->fb` exactly (historical `framebuffer.ppm`
  bytes do not move).

**CLI:** `--no-fb` (no `boot_info.fb`, no FRAMEBUF e820 entry — the
guest's own fb fallback engages and prints
`[fb] VGA text 80x25 @ 0xB8000 (BIOS: no linear framebuffer)`),
`--dump-text=path` (mode-3 console as text: non-printables -> blanks,
trailing blanks cut), `--dump-fb` renders the *display* (console in
text mode, LFB in graphics mode).  The `[kload] boot_info` receipt
prints the `fb none (VGA text)` variant only in `--no-fb` lanes.

**DoD — all measured:**
1. unit vectors `tests/test_video.c` (4): `ok vga cells / layout (80x25,
   attr hi byte, dump)`, `ok vga crtc cursor pair (0x3D4/0x3D5, idx
   0x0E/0x0F)`, `ok vga glyph pixels (8x16 PD font, console 640x400 @
   top-left)`, `ok vga display routing (reset text, BAR0/LFB, --no-fb
   lock)` — wired into the `test-unit` gate;
2. guest lane `--no-fb --dump-text`: the kload receipt prints the
   `fb none (VGA text)` variant (no FRAMEBUF e820 entry, 8 memmap
   entries) and the guest's own fallback engages — measured serial
   receipt `[fb] VGA text 80x25 @ 0xB8000 (BIOS: no linear framebuffer)`
   (fb.c:215); `--dump-text` shows the guest's own mode-3 console state
   (the fallback's cleared 80x25 screen — on this frozen baseline the
   guest's `vga_text_putc` is dead code (fb.c:158, static, zero call
   sites), so console text stays serial-only in the fallback and the
   cells hold the fallback's own cls pattern; guest-side characteristic,
   measured);
3. normal guest lanes byte-identical to the U5-era tapes — the *full*
   stream (log + register dump + last-log reprint), both shapes, with
   the era's own recipe (`--max-instr=4500000000`, the AURALUSB stick on
   the normal machine / the AURALEHC stick on `--no-usb-uhci`):
   `uhci1.out` sha256 `e615c39ec0c8…`, `ehci1.out` sha256
   `eb778d65a001…` — byte-for-byte, including the final register dump;
   `make test` + `make test-sanitize` green;
4. NC: reverting the CRTC write handler turns `ok vga crtc cursor pair`
   red (measured: abort RC=134 at `io_read(0x3D5)==0xF5`).

**Gate:** `make test` (incl. `test-video`) + `make test-sanitize` green;
guest `--no-fb` lane receipts above; NC measured.

### V2 — Bochs VBE dispi (0x1CE/0x1CF) ✅ shipped

**Result (measured):** the QEMU `-vga std` dispi register file is
implemented per §1.3 (the exact QEMU `vga.c` write-accept rules:
XRES `<= 16000 && (val & 7) == 0`, YRES `<= 12000`, BPP `0 -> 8` with
`4/8/15/16/24/32`, ID `0xB0C0..0xB0C5` verbatim, reset ID = `ID0`).
`VIDEO_MEMORY_64K` derives from the real VRAM (1920000 / 64K = 29,
read-only), BANK masks by QEMU's `(vram >> 16) - 1` = 28 and shifts
`bank_offset = val << 16`, GETCAPS returns the 16000/12000/32 maxima on
XRES/YRES/BPP reads.  The 0->1 ENABLE edge resets `VIRT_WIDTH`/offsets
and switches the display to the LFB; the 1->0 edge resets the bank
window and restores the machine's **previous** display (captured at the
enable edge).  `vbe_fixup_regs` behavioural core: `VIRT_WIDTH` clamps up
to XRES, `VIRT_HEIGHT` derives from `vram / linesize` (e.g. 800x600x32 →
VIRT_WIDTH 800, VIRT_HEIGHT 600).  Model notes: the enable-edge VRAM
clear (`NOCLEARMEM`) is V3's scope — V2 never mutates the LFB; the
banked `0xA0000` aperture is out of scope (the flat RAM model serves it
as plain RAM; `bank_offset` is real state).

The guest baseline never touches the interface (measured: no
`0x1CE`/`0x1CF` users in AuraLite-OS@`0ed0d29` — its GPU path is PCI
BAR0 + `boot_info.fb`), so no new guest lane is required; the freeze is
the historical lanes staying byte-identical.

**DoD — all measured:** unit vectors `ok vga dispi id ladder (ID0 reset,
ID0..ID5 accept, others rejected)`, `ok vga dispi mode regs (XRES x8,
YRES/BPP rejects, GETCAPS maxima)`, `ok vga dispi mem/bank
(MEMORY_64K=29 RO, bank mask 28, oob=0)`, `ok vga dispi enable/display
(LFB on enable, previous on disable)` (8 vectors total in
`tests/test_video.c`); the LFB stays byte-identical (KERNEL-BOOT
receipts do not move; both U5-era guest lanes reproduce byte-for-byte);
NC: removing the ID accept-list turns `ok vga dispi id ladder` red
(measured: abort RC=134 at `io_read(0x1CF)==0xB0C5`).

**Gate:** `make test` + `make test-sanitize` green.

### V3 — Mode switching + close-out ✅ shipped (0045)

Text mode 3 ↔ VBE graphics as machine state: ENABLE drives which
surface the display output shows; mode-set clears/keeps memory per
`NOCLEARMEM`; the guest's mode-3-forcing path (stage2's INT 0x10
contract, hardware side) and a VBE client's mode-set both land in the
documented state. Determinism ×2 across the new lanes (byte-identical
pairs), sanitize lanes, docs refresh (`ROADMAP.md` ledger row,
`docs/STATUS.md` evidence, CHANGELOG entries for V1–V3).

**DoD (measured):** the V1 guest lane + a mode-switch lane through the
dispi interface, each twice, byte-identical; display output assertions
per mode; no historical receipt moves (firmware lane + the K0–K8
kernel-lane receipts byte-stable).

**Gate:** all above + `make test` + `make test-sanitize` green.

**Measured notes (V3):** the mode state is one machine now -- text mode
3 ↔ VBE graphics driven by the dispi ENABLE edge, the GPU BAR0/kload
publishes, and the graphics controller's `VGA_GR06_GRAPHICS_MODE` bit
(ports `0x3CE/0x3CF`, QEMU `gr_mask`): stage2's INT 0x10 (AL=3) mode-set
lands on the hardware side with that bit clear → text mode 3; a VBE
client's mode-set lands in graphics; both transitions are pinned by
`ok vga gc graphics/mode-3 path`. Mode-set memory follows QEMU's
`vbe_ioport_write_data` literally: every ENABLED store clears
`YRES * vbe_line_offset` of VRAM unless `DISPI_NOCLEARMEM`
(`ok vga vbe mode-set clear/keep` measures the clear boundary and the
keep case). The mode-switch lane is a harness-side VBE client's mode-set
through the real `0x1CE/0x1CF` ports (`--vbe=WxHxBPP|off`, V3 CLI):
`tests/integration/test_video_modes.sh` runs it and the V1 `--no-fb`
guest lane twice each -- all six artifact pairs (ppm/txt/log) are
byte-identical (shas `d471c24697fa`/`6a33fe9ef156`/`658e6fcd3ee3` gfx,
`c3b3d73f6bbb`/`6a33fe9ef156`/`ccafdf3272c7` txt) with display
assertions per mode (gfx = the Limine splash panel `0x10101c` +
`0xdcdcdc` on the LFB surface; txt = the mode-3 console render). NC V3:
removing the NOCLEARMEM clear reddens the clear vector (RC=134,
`test_video.c:258`). `make test` + `make test-sanitize` green; the
firmware lane and the K0–K8 kernel receipts do not move.

## 4. V4 — post-closure fix: the dispi mode clamp (heap overflow, D6)

✅ done — `patches/0050-VIDEO-V4-vbe-mode-clamp.patch`. This is a section
of its own rather than a `### V4` under §3 because §3 documents the four
phases the plan was written for and closed; V4 was found afterwards, by
ASan, and changes `src/` — which V3's close-out had declared stable.

V3 closed this plan, and then ASan found D6: a **host heap overflow
reachable from the CLI and from any guest** that programs a VBE mode
bigger than the 800×600 framebuffer window. It is filed as its own phase
because it changes `src/`, which V1–V3's close-out had declared stable.

#### 4.1 Root cause, measured against the primary source

QEMU's `hw/display/vga.c` `vbe_fixup_regs` is the dispi interface's
whole error-reporting mechanism — the comment above it says so:

```c
/* Sanity check vbe register writes.
 *
 * As we don't have a way to signal errors to the guest in the bochs
 * dispi interface we'll go adjust the registers to the closest valid
 * value.
 */
```

and `vbe_ioport_write_data` then clears the screen with

```c
memset(s->vram_ptr, 0,
       s->vbe_regs[VBE_DISPI_INDEX_YRES] * s->vbe_line_offset);
```

That memset is bounded **only** because `vbe_fixup_regs` clamps YRES to
the number of lines that fit:

```c
linelength = r[VBE_DISPI_INDEX_VIRT_WIDTH] * bits / 8;
maxy = s->vbe_size / linelength;
if (r[VBE_DISPI_INDEX_YRES] == 0)                r[YRES] = 1;
if (r[VBE_DISPI_INDEX_YRES] > VBE_DISPI_MAX_YRES) r[YRES] = VBE_DISPI_MAX_YRES;
if (r[VBE_DISPI_INDEX_YRES] > maxy)               r[YRES] = maxy;   /* <-- the bound */
```

This file's `dispi_fixup` carried the VIRT_WIDTH raise and the
VIRT_HEIGHT derivation and **none of the clamps** — no `YRES → maxy`, no
`YRES == 0 → 1`, no `XRES &= ~7 / == 0 → 8 / > MAX → MAX`, no
`VIRT_WIDTH &= ~7 / > MAX`, no offset ladder, and `VIRT_HEIGHT` was
conditional where QEMU assigns it outright. The memset therefore ran on
the guest's own unclamped height: `--vbe=1024x768x32` clears 3145728
bytes out of a 1920000-byte `calloc`, and `--vbe=16000x12000x32` clears
768000000 — ~400× the window (receipts in D6 above).

The full QEMU function was fetched for this phase
(`gitlab.com/qemu-project/qemu/-/raw/master/hw/display/vga.c`) rather
than reconstructed, and `dispi_fixup` is now a line-for-line
transcription of it in QEMU's order: depth switch (with the default arm
*assigning* 8), width checks, height checks, the three-rung offset
ladder, then `VIRT_HEIGHT = maxy` and `vbe_line_offset = linelength`.

Two places legitimately differ, both marked in the source:

- **No `vbe_start_addr`.** QEMU folds the offset ladder's result into
  `s->vbe_start_addr = offset / 4` for its display code to pan inside
  VRAM. This model renders the visible display from `m->fb` at offset 0,
  so there is no consumer — but the clamps are still performed, because
  they are observable through the register readbacks a guest client
  polls (and the vector below asserts all three rungs).
- **BPP 4.** QEMU's *write handler* accepts only 8/15/16/24/32, so its
  fixup `case 4:` is dead code; this model's handler also accepts 4 (a
  pre-existing V2 divergence, not touched here), which means `bits = 4`
  and `linelength = VIRT_WIDTH * 4 / 8`. That is bounded and safe.
  Following QEMU's expression *literally* into a zero linelength would
  divide by zero in `maxy`, so the transcription keeps the division
  guarded — the divergence is recorded rather than silently "fixed" into
  a crash.

The `DISPI_ENABLE` arm was also brought to QEMU's shape: fixup, the
`VGA_GFX_MISC` update, the display flip and the clear all happen
**inside the 0→1 edge** (QEMU's `else` arm is just `s->bank_offset = 0`,
which covers both 1→0 and a 1→1 flag rewrite). Previously fixup ran on
*every* ENABLE write. That was not what made the overflow reachable —
the clear only runs on the edge — but the comment above the memset
claimed a bound nothing enforced, and the ordering is exactly the
property D6 turned on, so it is now QEMU's ordering with the reason
written down.

Finally the memset itself is bounded explicitly rather than trusted:

```c
size_t clear = (size_t)regs[DISPI_YRES] * g_dispi.line_offset;
size_t vram  = (size_t)m->fb_w * m->fb_h * 4u;
if (clear > vram) clear = vram;      /* belt and braces */
memset(m->fb, 0, clear);
```

The clamp in `dispi_fixup` already guarantees `clear <= vram`
(`YRES <= maxy = vram / linelength`), and the vector asserts that
arithmetic for the boundary modes; the extra min() is there so that a
future edit to the fixup cannot silently turn into host heap corruption
again. It is not the fix and is not a substitute for it — removing the
fixup clamp still reddens the vector, because the *readbacks* are what
it asserts.

#### 4.2 Regression vector (ships in the same patch)

`tests/test_video.c` → `test_dispi_mode_clamped_to_vram` (10 → **11
vectors**, `test-video: all tests passed`, RC=0 both plain and under
ASan+UBSan). Every expected number is QEMU's arithmetic on the
1920000-byte window, computed independently before being asserted:

| Mode / write | linelength | maxy | YRES | clear | |
|---|---|---|---|---|---|
| 1024×768×32 (the crashing mode) | 4096 | 468 | **468** | 1916928 | ≤ VRAM |
| 16000×12000×32 (largest accepted) | 64000 | 30 | **30** | 1920000 | **== VRAM** |
| 800×600×32 (the documented mode) | 3200 | 600 | **600** | 1920000 | **== VRAM** |
| 800×**0**×32 | 3200 | 600 | **1** | 3200 | YRES 0 → 1 |
| **0**×600×32 | 32 | 60000 | 600 | 19200 | XRES 0 → 8, VIRT_HEIGHT 60000 |
| 800×600×**33** | 800 | 2400 | 600 | 480000 | BPP → 8 by the default arm |
| VIRT_WIDTH 65535 while enabled | 64000 | 30 | 30 | 1920000 | 65528 → MAX 16000 |
| VIRT_WIDTH 9 while enabled | 3200 | 600 | 600 | 1920000 | 9 & ~7 = 8 → raised to XRES |

plus all three rungs of the offset ladder, each needing its own mode
because the rungs are arithmetic, not flags:

- **rung 1** (clamp to MAX, nothing zeroed) needs the clamped maxima to
  still fit: 8×1×8, `16000/4 + 12000*8 + 1*8 = 100008 ≤ 1920000`, so
  X_OFFSET reads back **16000** and Y_OFFSET **12000**;
- **rung 2** (Y_OFFSET zeroed, X_OFFSET survives) needs
  `12000 * linelength + YRES * linelength > VRAM`, and since Y_OFFSET is
  clamped to 12000 first that means `linelength ≥ 160` → 40×1×32:
  `12000*160 + 160 = 1920160 > 1920000`, retest `25 + 0 + 160 = 185`
  fits, so X_OFFSET stays **100** and Y_OFFSET becomes **0**. At 8×1×8 or
  8×1×32 this rung is *unreachable by construction* (12000×8 = 96000 and
  12000×32 = 384000 bytes cannot exceed the window) — which is why the
  vector uses a third mode rather than reusing the first;
- **rung 3** (both zeroed): 800×600×32 with both at their maxima,
  `16000 + 38400000 + 1920000 > VRAM` and `16000 + 1920000 > VRAM`, so
  both read back **0**.

One structural detail shaped every case and is recorded in the vector's
comment: the fixup runs from the register write itself (QEMU groups
XRES/YRES/BPP/VIRT_WIDTH/X_OFFSET/Y_OFFSET into one `store, then
vbe_fixup_regs` arm) *and* from the 0→1 enable edge — but the edge first
**resets** VIRT_WIDTH and both offsets to 0. A value written before
enabling is therefore erased before it can be clamped, so those three
registers' clamps are only observable by writing them while already
enabled. Getting this wrong produced two false assertions during
development, both caught by the vector rather than by reasoning.

#### 4.3 The phase's negative control, measured

The NC is the pre-fix `src/devices.c` (`git show HEAD:src/devices.c`,
i.e. the V3 state) with the new vector compiled against it:

```
cc -fsanitize=address,undefined -fno-sanitize-recover=all … tests/test_video.c <pre-fix src>
  -> RC=1
     ERROR: AddressSanitizer: heap-buffer-overflow
     WRITE of size 3145728 … 0 bytes after 1920000-byte region
     ABORTING (before the vector's first readback is even reached)
```

and the same binary with the fix in place: `RC=0`, `ok vga dispi mode
clamped to VRAM (YRES/XRES/offsets, the V4 overflow)`, all 11 vectors
green. So the vector does not merely pass after the fix — it is the
thing that fails before it.

Beyond the vector, a one-off exhaustive sweep was run under
ASan+UBSan (not a `make` target; the permanent gate is the vector):
**5236 mode combinations** (17 hostile XRES × 14 YRES × 11 BPP values,
each enabled with and without `DISPI_NOCLEARMEM`) plus **146672 register
pokes** (every index 0x00–0x09 written as 65535, 1 and 0 while enabled,
which re-runs the fixup each time, plus a 64-value BANK sweep),
asserting after each step that `YRES * line_offset <= VRAM` using QEMU's
own `VIRT_WIDTH * bits / 8` expression:

```
post-fix:  0 violations, RC=0, no ASan/UBSan report
pre-fix:   RC=1 on the FIRST combination (WRITE of size 2621440, same site)
```

#### 4.4 Historical receipts did not move

`tests/integration/test_video_modes.sh` SKIPs in this sandbox (it needs
the frozen `AuraLite-OS` build), so the V3 artifact shas could not be
re-measured directly. They were proved stable a stronger way instead:
the pre-fix and post-fix emulators were run side by side on four lanes
and every `--log` and `--dump-fb` pair compared byte for byte —

| Lane | pre-fix vs post-fix | `--dump-fb` sha256 |
|---|---|---|
| firmware boot, 600000 instr | **IDENTICAL** (rc 0/0) | `15c96625cfec…` |
| `--vbe=800x600x32` (V3's mode-gfx recipe) | **IDENTICAL** (rc 0/0) | `15c96625cfec…` |
| `--vbe=off` (V3's disable path) | **IDENTICAL** (rc 0/0) | `15c96625cfec…` |
| `--no-fb` (V1's guest lane) | **IDENTICAL** (rc 0/0) | `c3b3d73f6bbb…` |

The last row is the load-bearing one: `c3b3d73f6bbb` is the sha V3
recorded for its txt lane in §V3 above, reproduced here from a different
binary — so the V1/V3 artifacts are unchanged by V4, not merely
unchanged by V4's own run. All ten pre-existing `test_video.c` vectors
also still pass unmodified, including `ok vga dispi mode regs (XRES x8,
YRES/BPP rejects, GETCAPS maxima)` and `ok vga dispi enable/display` —
the write-handler gates and the GETCAPS maxima (16000/12000/32, which
QEMU also returns unconditionally) are untouched; only the fixup grew.

#### 4.5 Gates

`make test` **RC=0** (`boot integration test: ok`, so the firmware GPU
probe 0:2:0 `0x8086:0x0412` / BAR0 `0xD0000000` / `pixel[0]=0x00200000`
contract did not move; `test-tap-responder: all tests passed`; `N2
responder receipts: ALL PASS`; `N3 end-to-end receipts: ALL PASS`, so
the NET lanes just closed are green against the changed `src/` too);
`make test-sanitize` **RC=0** (ASan+UBSan); `make fuzz
FUZZ_PROGRAMS=256` — `256 programs, 6144 instructions compared -- all
states match`, `crash invariant: 64 garbage streams, 33 clean faults, 2
halts, 0 host-level crashes`.

Changed by V4: `src/devices.c` (the fixup, the ENABLE arm, the bounded
clear) and `tests/test_video.c` (the vector). Nothing else — no CLI
flag, no register map, no documented mode.

#### 4.6 The delivered patch, verified

`patches/0050-VIDEO-V4-vbe-mode-clamp.patch`, `git format-patch -1` of this
phase's commit, on top of the parent `07b2f15 "N3 update"`. As in
`NET_PLAN.md` §3.9, its size, line count and commit sha are deliberately
not quoted here: all three are properties of the artifact that *contains*
this section, so any number written down goes stale the moment the
section is amended. The check is a procedure:

```sh
git clone …/AuraLite-Emulator $CLONE && cd $CLONE
git checkout 07b2f15                              # the parent
git am  …/patches/0050-VIDEO-V4-vbe-mode-clamp.patch
git diff --quiet $REPO_HEAD HEAD                  # empty: the tree is the commit's
make test                                         # RC=0
```

Run against the delivered patch that produced: `git am` clean, **tree
identical** to the phase commit, and `make test` **RC=0** with
`test-video: all tests passed` including the new
`ok vga dispi mode clamped to VRAM (YRES/XRES/offsets, the V4 overflow)`,
`boot integration test: ok`, `test-tap-responder: all tests passed`,
`N2 responder receipts: ALL PASS`, `N3 end-to-end receipts: ALL PASS`,
and the same two environment SKIPs as §3.9 of the NET plan (`nasm`
missing → `test-net-tx`; no frozen `AuraLite-OS` build → the guest
lanes). Seeding that clone uses the recipe recorded there — the three
`nasm` artifacts with their inputs backdated below them, because
`.DELETE_ON_ERROR:` turns a failed rebuild into a *deleted*
`firmware/firmware.bin`.

`git am` reports `106 lines add whitespace errors` for this patch. Every
one is inside the nested `patches/0049-NET-N3-net-e2e.patch` that this
commit carries (house convention: a phase's commit ships the previous
phase's patch, and 0049 in turn carries 0048 — so the warnings nest).
Of the six files V4 authors, five have **zero** trailing-whitespace
lines; the sixth, `src/devices.c`, has exactly one, on the `line_offset`
comment inside `dispi_t`, and it is **pre-existing** — the parent commit
has the same single line, and V4 did not touch it.

**Not in V4** (measured, deferred): `vbe_start_addr` / display panning
(no consumer in this model — the offsets are clamped and readable but
the visible display still renders from offset 0); QEMU's
`vbe_update_vgaregs` and the CVE-2016-3712 re-assertion of VGA registers
on every GR/CR write (this model's `VGA_GFX_MISC` write path only routes
text↔graphics display and consumes no sizing register, so there is no
overflow surface there to re-assert against — measured: `vga_io_write`
at `src/devices.c:852` touches nothing but the display flag); the BPP-4
write-handler divergence from QEMU's accepted set (bounded, recorded
above); and growing the VRAM window itself (QEMU's std-VGA carries 16
MiB, this model's window is sized to the published 800×600 framebuffer
that `boot_info.fb` and the `0x00200000` marker contract fix — growing
it would move the guest-visible firmware contract, which §5 forbids, and
it would not remove the need for the clamp anyway, only reduce how often
it fires).

## 5. V5 — post-closure: the 16 MiB aperture + `vbe_start_addr` panning

The V4 close-out left two measured gaps ("Not in V4"): the VRAM window was
the 800x600x4 = 1,920,000-byte display buffer rather than a real VGA
aperture, and the offsets were "clamped and readable but the visible
display still renders from offset 0" -- no panning consumer.  Both are
closed here, together, because they share one code path (`dispi_fixup`'s
offset fold).

**What changed (`src/devices.c`):**

- **The aperture is the full 16 MiB Bochs/QEMU std-VGA window.**  The
  `make_ramwindow` at `0xD0000000` now allocates `GPU_VRAM_BYTES` (16 MiB)
  instead of `fb_w*fb_h*4`; `m->fb_w/fb_h` remain the published 800x600x32
  DISPLAY (boot_info.fb, the 0x00200000 marker, the firmware probe -- all
  unmoved).  Everything that used to measure "VRAM = fb_w*fb_h*4" --
  the YRES clamp (`maxy`), the offset ladder, the enable-edge clear,
  `VIDEO_MEMORY_64K`, the BANK mask -- now measures the aperture, exactly
  as QEMU's `vram_size` does.  Consequences a guest can see: 1024x768x32
  (the V4 ASan repro) **fits now**, `VIDEO_MEMORY_64K` reads 256 (was 29),
  the BANK mask is 255 (was 28), and the clamp still fires one line short
  of the window for oversized modes (16000x12000x32 -> YRES 262).
- **`vbe_start_addr` is kept and consumed.**  `dispi_fixup` folds the
  offset exactly as QEMU (`offset = X_OFFSET*bits/8 + Y_OFFSET*linelength,
  vbe_start_addr = offset >> 2`, DWORDS), and `vga_render_display` now
  renders the visible surface from `m->fb + vbe_start_addr` at the VBE
  line offset (VIRT_WIDTH may exceed XRES -> the pitch may exceed a row of
  output pixels).  Reads past the aperture render black instead of running
  off the window -- the offset clamp only guards `YRES` rows while the
  renderer draws `fb_h`, a case the unit vector pins explicitly.

**Contract preservation (measured):**

- The firmware lane is byte-stable: `GUEST_OK`, `framebuffer marker OK:
  pixel[0]=0x00200000`, `framebuffer.ppm (800x600)` -- the firmware never
  touches the dispi (measured: no `0x1CE`/`0x1CF` in
  `firmware/sample_firmware.nas`) and never reads BAR size.
- `kloader`'s FRAMEBUF memmap entry stays `fb_w*fb_h*4` -- the published
  framebuffer, not the aperture.
- With no dispi mode ever set (the plain `--rom` path), `vbe_start_addr`
  is 0 and the render falls back to the historical tightly-packed copy.

**Receipts (measured, this phase):**

- `tests/test_video.c`: `test_dispi_mem_bank` (256/255), the reworked
  `test_dispi_mode_clamped_to_vram` (1024x768x32 fits unclamped;
  16000x12000x32 clamps at 262 lines with `262*64000 <= VRAM <
  263*64000`; the offset ladder re-derived against 16 MiB: rung 1
  8x1x8, rung 2 400x1x32, rung 3 1024x4096x32), and the new
  `test_vbe_start_addr_panning` (Y-offset fold, X-offset pixel shift,
  VIRT_WIDTH pitch, bounded tail render).
- Negative control: revert `src/devices.c`, the suite aborts at the first
  V5 assertion (`MEMORY_64K == 256`).  RC measured non-zero.
- The 800x600x32 PPM path is byte-identical (pitch == w, offset 0).

**Still not modelled** (unchanged): the BPP-4 write-handler divergence
recorded in V4 (bounded), QEMU's `vbe_update_vgaregs` full VGA-register
re-assertion (no sizing surface exists in this model's `vga_io_write`),
and text-mode panning (the CRTC start address is not implemented; VBE
panning only).

## 6. Standing constraints

- The guest-visible firmware contract (`firmware/sample_firmware.nas`)
  does not change; its GPU probe (`0:2:0`, `0x8086:0x0412`, BAR0
  `0xD0000000`) and the `0x00200000` pixel marker stay put.
- Historical receipts do not move: K0–K8 kernel-lane receipts, the
  STORE/USB matrix lanes, and the firmware lane stay byte-stable
  across V1–V3.
- Every phase: `fix + regression vector in the same patch`, one
  negative control per phase (revert → red, RC measured),
  `make test` + `make test-sanitize` green at the boundary, receipts
  measured against the tree (`AuraLite-OS@0ed0d29`), docs in the same
  patch as the code.
