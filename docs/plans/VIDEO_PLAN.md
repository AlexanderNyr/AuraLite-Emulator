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

**Status: complete — V0 ✅, V1 ✅, V2 ✅, V3 ✅ (plan closed; NET / COMPAT follow in their own plans)**

| Phase | Scope | Status | Deliverable |
|---|---|---|---|
| V0 | Measured baseline + this plan (defect ledger: no VGA text rendering, no CRTC, no Bochs VBE dispi, no mode state) | ✅ done | `patches/0042-VIDEO-V0-video-plan.patch` |
| V1 | VGA text mode 3: `0xB8000` text window + CRTC cursor pair (0x3D4/0x3D5), glyph rendering into the display output, `--no-fb` lane so the guest's `fb.c` fallback engages | ✅ done | `patches/0043-VIDEO-V1-vga-text.patch` |
| V2 | Bochs VBE dispi (0x1CE/0x1CF): the `VBE_DISPI` register file, LFB window + video-memory sizing (the QEMU `-vga std` contract) | ✅ done | `patches/0044-VIDEO-V2-vbe-dispi.patch` |
| V3 | Mode switching (text 3 ↔ VBE graphics), determinism ×2, sanitize, docs close-out | ✅ done | `patches/0045-VIDEO-V3-video-mode-switch.patch` |

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

## 2. Defect ledger (drives V1–V3)

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

## 4. Standing constraints

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
