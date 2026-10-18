/* tests/test_video.c -- VIDEO V1+V2 vectors: VGA text mode 3 (the BIOS box)
 * and the Bochs VBE dispi register file (ports 0x1CE/0x1CF, QEMU `-vga std`).
 *
 * The contract is measured from AuraLite-OS@0ed0d29 drivers/framebuffer/fb.c:
 * 80x25 uint16 cells at 0xB8000 (attribute in the high byte, VGA_ATTR 0x07),
 * the hardware cursor through the CRTC index/data pair (outb(0x3D4, 0x0F/0x0E),
 * outb(0x3D5, lo/hi)), and glyphs from the guest's own PD Linux VGA 8x16 font
 * so the emulator's text display looks exactly like the guest console.
 * The fixture pins: cell writes through the RAM window (layout + dump),
 * the CRTC cursor pair round-trip, glyph pixels on the display surface, and
 * display routing (text <-> LFB) including the --no-fb lock.
 */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "devices.h"
#include "harness.h"

#define VGA_TEXT  0xB8000u
#define GPU_BASE  0xD0000000u

typedef struct { machine_t m; } fx_t;

static void fx_init(fx_t *f) {
    setup_machine(&f->m);
    devices_init_common(&f->m);
}

static void fx_done(fx_t *f) { devices_done(&f->m); }

/* cells: char at 2*i, attribute at 2*i+1 (little-endian uint16 = attr<<8|ch) */
static void put_cell(machine_t *m, unsigned cell, uint8_t ch, uint8_t attr) {
    mem_write(m, VGA_TEXT + 2*cell, 1, ch);
    mem_write(m, VGA_TEXT + 2*cell + 1, 1, attr);
}

static void test_cells_layout(void) {
    fx_t f; fx_init(&f);
    /* the guest's own recipe (fb.c vga_cls/fb_puts): attr 0x07, 'A'@0, 'B'@79 */
    put_cell(&f.m, 0, 'A', 0x07);
    put_cell(&f.m, 79, 'B', 0x70);   /* reversed video */
    put_cell(&f.m, 80, 'C', 0x07);   /* row 1 col 0 */

    /* dump_text: 25 lines, trailing blanks cut, exact characters */
    vga_dump_text(&f.m, "/tmp/test_video_dump.txt");
    FILE *fp = fopen("/tmp/test_video_dump.txt", "r");
    assert(fp);
    char line[256];
    assert(fgets(line, sizeof line, fp));
    assert((int)strlen(line) == 80 + 1);         /* 80 cols + \n */
    assert(line[0] == 'A' && line[79] == 'B' && line[80] == '\n');
    for (int i = 1; i < 79; i++) assert(line[i] == ' ');
    assert(fgets(line, sizeof line, fp));
    assert(strcmp(line, "C\n") == 0);            /* row 1: 'C' + 79 spaces -> "C" */
    int rows = 2;
    while (fgets(line, sizeof line, fp)) rows++;
    assert(rows == 25);
    fclose(fp);
    /* raw cells round-trip through the window (most-specific MMIO) */
    assert(mem_read(&f.m, VGA_TEXT + 2*79, 1) == 'B');
    assert(mem_read(&f.m, VGA_TEXT + 2*79 + 1, 1) == 0x70);
    fx_done(&f);
    printf("ok vga cells / layout (80x25, attr hi byte, dump)\n");
}

static void test_crtc_cursor_pair(void) {
    fx_t f; fx_init(&f);
    /* guest recipe (fb.c vga_move_cursor): offset = row*80+col; hi then lo */
    unsigned off = 3*80 + 5;                    /* row 3, col 5 */
    io_write(&f.m, 0x3D4, 1, 0x0F);
    io_write(&f.m, 0x3D5, 1, off & 0xFF);
    io_write(&f.m, 0x3D4, 1, 0x0E);
    io_write(&f.m, 0x3D5, 1, (off >> 8) & 0xFF);
    /* read back through the same index/data pair */
    io_write(&f.m, 0x3D4, 1, 0x0F);
    assert(io_read(&f.m, 0x3D5, 1) == 0xF5);
    io_write(&f.m, 0x3D4, 1, 0x0E);
    assert(io_read(&f.m, 0x3D5, 1) == 0x00);
    assert(io_read(&f.m, 0x3D4, 1) == 0x0E);    /* index reads back */
    fx_done(&f);
    printf("ok vga crtc cursor pair (0x3D4/0x3D5, idx 0x0E/0x0F)\n");
}

static void test_glyph_pixels(void) {
    fx_t f; fx_init(&f);
    put_cell(&f.m, 0, 'A', 0x07);   /* fg = light grey 0xAAAAAA, bg = black */
    uint32_t px[800*600];
    memset(px, 0xCD, sizeof px);
    vga_render_display(&f.m, px, 800, 600);
    /* the guest's own psf2_default_font.inc 'A': row2 = 0x10 -> x=3 on;
     * row7 = 0xfe -> x=0 on, x=7 off (MSB = leftmost pixel). */
    assert(px[2*800 + 3] == 0xAAAAAA);
    assert(px[2*800 + 2] == 0x000000);
    assert(px[7*800 + 0] == 0xAAAAAA);
    assert(px[7*800 + 7] == 0x000000);
    /* glyph cell pitch 8x16: the panel around the 640x400 console is black */
    assert(px[0*800 + 640] == 0x000000);
    assert(px[400*800 + 0] == 0x000000);
    fx_done(&f);
    printf("ok vga glyph pixels (8x16 PD font, console 640x400 @ top-left)\n");
}

static void test_display_routing(void) {
    fx_t f; fx_init(&f);
    assert(vga_display_is_text(&f.m));          /* reset = text mode 3 */
    /* the firmware's enable: recipe programs BAR0 -> the LFB is the display */
    io_write(&f.m, 0xCF8, 4, 0x80001010u);      /* 0:2.0 cfg addr, BAR0 */
    io_write(&f.m, 0xCFC, 4, GPU_BASE);
    assert(!vga_display_is_text(&f.m));
    /* graphics: the display shows m->fb exactly */
    f.m.fb[7] = 0x00112233;
    uint32_t px[800*600];
    vga_render_display(&f.m, px, 800, 600);
    assert(px[7] == 0x00112233);
    /* kload-style flip back is allowed; then text again */
    vga_set_display_mode(&f.m, 1);
    assert(vga_display_is_text(&f.m));
    fx_done(&f);

    /* --no-fb locks the display to the text console */
    fx_t g; setup_machine(&g.m); g.m.cfg_no_fb = 1;
    devices_init_common(&g.m);
    assert(vga_display_is_text(&g.m));
    io_write(&g.m, 0xCF8, 4, 0x80001010u);
    io_write(&g.m, 0xCFC, 4, GPU_BASE);
    assert(vga_display_is_text(&g.m));          /* locked */
    vga_set_display_mode(&g.m, 0);
    assert(vga_display_is_text(&g.m));          /* locked */
    fx_done(&g);
    printf("ok vga display routing (reset text, BAR0/LFB, --no-fb lock)\n");
}

static void test_dispi_id_ladder(void) {
    fx_t f; fx_init(&f);
    /* reset value is ID0 (QEMU vga_reset) -- the "interface present" probe */
    io_write(&f.m, 0x1CE, 2, 0x00);
    assert(io_read(&f.m, 0x1CF, 2) == 0xB0C0);
    /* client ladder: write the wanted ID, read it back */
    io_write(&f.m, 0x1CF, 2, 0xB0C5);            /* ID5 */
    assert(io_read(&f.m, 0x1CF, 2) == 0xB0C5);
    io_write(&f.m, 0x1CF, 2, 0xB0C6);            /* unsupported -> rejected */
    assert(io_read(&f.m, 0x1CF, 2) == 0xB0C5);
    io_write(&f.m, 0x1CF, 2, 0xB0C2);            /* ID2 accepted verbatim */
    assert(io_read(&f.m, 0x1CF, 2) == 0xB0C2);
    assert(io_read(&f.m, 0x1CE, 2) == 0x00);     /* index port reads back */
    fx_done(&f);
    printf("ok vga dispi id ladder (ID0 reset, ID0..ID5 accept, others rejected)\n");
}

static void test_dispi_mode_regs(void) {
    fx_t f; fx_init(&f);
    /* XRES: QEMU rejects > 16000 and values not 8-pixel aligned */
    io_write(&f.m, 0x1CE, 2, 0x01);
    io_write(&f.m, 0x1CF, 2, 800);
    assert(io_read(&f.m, 0x1CF, 2) == 800);
    io_write(&f.m, 0x1CF, 2, 801);               /* not divisible by 8 */
    assert(io_read(&f.m, 0x1CF, 2) == 800);
    io_write(&f.m, 0x1CF, 2, 16008);             /* over MAX, aligned */
    assert(io_read(&f.m, 0x1CF, 2) == 800);
    /* YRES: <= 12000 */
    io_write(&f.m, 0x1CE, 2, 0x02);
    io_write(&f.m, 0x1CF, 2, 600);
    assert(io_read(&f.m, 0x1CF, 2) == 600);
    io_write(&f.m, 0x1CF, 2, 12001);
    assert(io_read(&f.m, 0x1CF, 2) == 600);
    /* BPP: 0 -> 8; accept 4/8/15/16/24/32; reject others */
    io_write(&f.m, 0x1CE, 2, 0x03);
    io_write(&f.m, 0x1CF, 2, 0);
    assert(io_read(&f.m, 0x1CF, 2) == 8);
    io_write(&f.m, 0x1CF, 2, 15);
    assert(io_read(&f.m, 0x1CF, 2) == 15);
    io_write(&f.m, 0x1CF, 2, 7);
    assert(io_read(&f.m, 0x1CF, 2) == 15);
    /* GETCAPS: XRES/YRES/BPP reads return the maxima while armed */
    io_write(&f.m, 0x1CE, 2, 0x04);
    io_write(&f.m, 0x1CF, 2, 0x02);              /* GETCAPS only */
    io_write(&f.m, 0x1CE, 2, 0x01);
    assert(io_read(&f.m, 0x1CF, 2) == 16000);
    io_write(&f.m, 0x1CE, 2, 0x02);
    assert(io_read(&f.m, 0x1CF, 2) == 12000);
    io_write(&f.m, 0x1CE, 2, 0x03);
    assert(io_read(&f.m, 0x1CF, 2) == 32);
    io_write(&f.m, 0x1CE, 2, 0x04);
    io_write(&f.m, 0x1CF, 2, 0x00);              /* disarm */
    io_write(&f.m, 0x1CE, 2, 0x03);
    assert(io_read(&f.m, 0x1CF, 2) == 15);       /* back to the stored BPP */
    fx_done(&f);
    printf("ok vga dispi mode regs (XRES x8, YRES/BPP rejects, GETCAPS maxima)\n");
}

static void test_dispi_mem_bank(void) {
    fx_t f; fx_init(&f);
    /* VIDEO_MEMORY_64K = vram / 64K = 16777216/65536 = 256 (V5: the full
     * 16 MiB std-VGA aperture), read-only */
    io_write(&f.m, 0x1CE, 2, 0x0A);
    assert(io_read(&f.m, 0x1CF, 2) == 256);
    io_write(&f.m, 0x1CF, 2, 0xFFFF);            /* ignored */
    assert(io_read(&f.m, 0x1CF, 2) == 256);
    /* BANK: QEMU mask = (vram >> 16) - 1 = 255 */
    io_write(&f.m, 0x1CE, 2, 0x05);
    io_write(&f.m, 0x1CF, 2, 0xFFFF);
    assert(io_read(&f.m, 0x1CF, 2) == 255);
    io_write(&f.m, 0x1CF, 2, 5);                 /* 5 & 255 = 5 */
    assert(io_read(&f.m, 0x1CF, 2) == 5);
    /* out-of-range index reads 0 */
    io_write(&f.m, 0x1CE, 2, 0x0B);
    assert(io_read(&f.m, 0x1CF, 2) == 0);
    fx_done(&f);
    printf("ok vga dispi mem/bank (MEMORY_64K=256 RO, bank mask 255, oob=0)\n");
}

static void test_dispi_enable_display(void) {
    fx_t f; fx_init(&f);
    assert(vga_display_is_text(&f.m));           /* reset = text mode 3 */
    io_write(&f.m, 0x1CE, 2, 0x01); io_write(&f.m, 0x1CF, 2, 800);
    io_write(&f.m, 0x1CE, 2, 0x02); io_write(&f.m, 0x1CF, 2, 600);
    io_write(&f.m, 0x1CE, 2, 0x03); io_write(&f.m, 0x1CF, 2, 32);
    /* enable: display -> LFB, QEMU 0->1 edge resets VIRT_WIDTH/offsets
     * and vbe_fixup derives VIRT_WIDTH=XRES, VIRT_HEIGHT=vram/linesize */
    io_write(&f.m, 0x1CE, 2, 0x04);
    io_write(&f.m, 0x1CF, 2, 0x41);              /* ENABLED | LFB_ENABLED */
    assert(!vga_display_is_text(&f.m));
    io_write(&f.m, 0x1CE, 2, 0x06);
    assert(io_read(&f.m, 0x1CF, 2) == 800);      /* VIRT_WIDTH clamped up */
    io_write(&f.m, 0x1CE, 2, 0x07);
    assert(io_read(&f.m, 0x1CF, 2) == 5242);     /* V5: 16777216 / (800*4) */
    io_write(&f.m, 0x1CE, 2, 0x08);
    assert(io_read(&f.m, 0x1CF, 2) == 0);        /* X_OFFSET reset */
    /* disable: back to the previous display (text) */
    io_write(&f.m, 0x1CE, 2, 0x04);
    io_write(&f.m, 0x1CF, 2, 0x00);
    assert(vga_display_is_text(&f.m));
    fx_done(&f);

    /* previous-display capture: from graphics, enable/disable returns to
     * graphics (the machine's previous mode), not text */
    fx_t g; fx_init(&g);
    vga_set_display_mode(&g.m, 0);               /* BAR0-style flip */
    io_write(&g.m, 0x1CE, 2, 0x04);
    io_write(&g.m, 0x1CF, 2, 0x01);              /* ENABLED only */
    assert(!vga_display_is_text(&g.m));
    io_write(&g.m, 0x1CE, 2, 0x04);
    io_write(&g.m, 0x1CF, 2, 0x00);
    assert(!vga_display_is_text(&g.m));          /* restored to graphics */
    fx_done(&g);
    printf("ok vga dispi enable/display (LFB on enable, previous on disable)\n");
}

static void test_vbe_mode_set_clear(void) {
    fx_t f; fx_init(&f);
    /* QEMU vbe_ioport_write_data ENABLED store: memset(vram, 0, YRES *
     * vbe_line_offset) unless DISPI_NOCLEARMEM -- measured via the patch
     * history ("clear the screen").  VRAM is our LFB buffer. */
    for (unsigned i = 0; i < 320u*200u*4u + 16; i++)
        mem_write(&f.m, GPU_BASE + i, 1, 0xA5);
    /* harness-side client's mode-set: 320x200x32 -> clear 200*(320*4) */
    vbe_mode_set(&f.m, 320, 200, 32);
    assert(mem_read(&f.m, GPU_BASE, 4) == 0x00000000);
    assert(mem_read(&f.m, GPU_BASE + 320u*200u*4u - 4, 4) == 0x00000000);
    assert(mem_read(&f.m, GPU_BASE + 320u*200u*4u, 4) == 0xA5A5A5A5);   /* past the clear */
    /* NOCLEARMEM keeps memory: reprogram a mode through the ports with the
     * flag set -- the pattern must survive the enable store */
    for (unsigned i = 0; i < 16; i++)
        mem_write(&f.m, GPU_BASE + i, 1, 0x5A);
    io_write(&f.m, 0x1CE, 2, 0x01); io_write(&f.m, 0x1CF, 2, 640);
    io_write(&f.m, 0x1CE, 2, 0x02); io_write(&f.m, 0x1CF, 2, 480);
    io_write(&f.m, 0x1CE, 2, 0x03); io_write(&f.m, 0x1CF, 2, 32);
    io_write(&f.m, 0x1CE, 2, 0x04);
    io_write(&f.m, 0x1CF, 2, 0xC1);              /* ENABLED | LFB | NOCLEARMEM */
    assert(mem_read(&f.m, GPU_BASE, 4) == 0x5A5A5A5A);
    fx_done(&f);
    printf("ok vga vbe mode-set clear/keep (NOCLEARMEM)\n");
}

static void test_gc_mode3_path(void) {
    fx_t f; fx_init(&f);
    assert(vga_display_is_text(&f.m));           /* reset = text mode 3 */
    /* GC VGA_GFX_MISC (index 6): setting VGA_GR06_GRAPHICS_MODE is the
     * graphics side; clearing it is the guest's mode-3-forcing path --
     * stage2's INT 0x10 (AL=3) mode-set lands here on the hardware side */
    io_write(&f.m, 0x3CE, 2, 6);
    io_write(&f.m, 0x3CF, 2, 0xFF);              /* gr_mask[6] = 0x0f */
    assert(io_read(&f.m, 0x3CF, 2) == 0x0F);
    assert(!vga_display_is_text(&f.m));          /* graphics side */
    /* mode-3 forcing after a VBE mode-set: both land in text mode 3 */
    vbe_mode_set(&f.m, 800, 600, 32);
    assert(!vga_display_is_text(&f.m));
    io_write(&f.m, 0x3CE, 2, 6);
    io_write(&f.m, 0x3CF, 2, 0x0E);              /* bit0 clear = mode 3 */
    assert(vga_display_is_text(&f.m));
    fx_done(&f);
    printf("ok vga gc graphics/mode-3 path (stage2 INT 0x10 contract)\n");
}

/* VIDEO V4 regression vector for the heap overflow the mode clamp used to
 * miss.  Before the fix, enabling any mode whose YRES * line_offset exceeded
 * the 800x600x4 = 1920000-byte VRAM window made QEMU's "clear the screen"
 * memset run past the end of it:
 *
 *   ./x86emu --platform=haswell --vbe=1024x768x32 --max-instr=1000
 *     unsanitized: RC=134, `free(): invalid pointer`
 *     ASan+UBSan:  RC=1,   heap-buffer-overflow WRITE of size 3145728
 *                          at dispi_data_write, src/devices.c:1055
 *                          "0 bytes after 1920000-byte region"
 *   ./x86emu --platform=haswell --vbe=1600x1200x32   -> WRITE of size 7680000,   RC=1
 *   ./x86emu --platform=haswell --vbe=16000x12000x32 -> WRITE of size 768000000, RC=139
 *
 * The expected numbers below are QEMU's `vbe_fixup_regs` arithmetic on that
 * window, not this model's opinion of a sensible mode: linelength =
 * VIRT_WIDTH * bits / 8, maxy = vbe_size / linelength, YRES pulled down to
 * maxy -- so the clear is YRES * linelength <= vbe_size by construction.
 *
 * One structural detail shapes every case: the fixup runs from the register
 * write itself (QEMU groups XRES/YRES/BPP/VIRT_WIDTH/X_OFFSET/Y_OFFSET into
 * one `store, then vbe_fixup_regs` arm) AND from the 0->1 enable edge, but the
 * edge first RESETS VIRT_WIDTH and both offsets to 0.  So a value written
 * before enabling is erased before it can be clamped; the clamps on those
 * three registers are only observable by writing them while already enabled. */
static void test_dispi_mode_clamped_to_vram(void) {
    fx_t f; fx_init(&f);
    const uint32_t VRAM = 16u * 1024u * 1024u;  /* V5: the std-VGA aperture */

    /* 1024x768x32: linelength 4096 -> maxy = 16777216/4096 = 4096.  V5:
     * the mode that overflowed the old 1920000-byte window now FITS -- the
     * clamps no longer fire and the clear is 768*4096 = 3145728 <= VRAM. */
    vbe_mode_set(&f.m, 1024, 768, 32);
    io_write(&f.m, 0x1CE, 2, 0x02);
    assert(io_read(&f.m, 0x1CF, 2) == 768);        /* YRES unclamped now */
    io_write(&f.m, 0x1CE, 2, 0x07);
    assert(io_read(&f.m, 0x1CF, 2) == 4096);       /* VIRT_HEIGHT = maxy */
    assert(768u * 4096u <= VRAM);

    /* 16000x12000x32, the largest pair the write handler accepts at all:
     * linelength 64000 -> maxy = 262, and the clear is 262*64000 =
     * 16768000 <= VRAM with exactly one more line overflowing -- the V4
     * overflow property, pinned on the 16 MiB window. */
    fx_done(&f); fx_init(&f);
    vbe_mode_set(&f.m, 16000, 12000, 32);
    io_write(&f.m, 0x1CE, 2, 0x02);
    assert(io_read(&f.m, 0x1CF, 2) == 262);
    io_write(&f.m, 0x1CE, 2, 0x07);
    assert(io_read(&f.m, 0x1CF, 2) == 262);
    assert(262u * 64000u <= VRAM && 263u * 64000u > VRAM);

    /* YRES 0 -> 1 (QEMU: "if YRES == 0, YRES = 1") -- not 0, not maxy. */
    fx_done(&f); fx_init(&f);
    vbe_mode_set(&f.m, 800, 0, 32);
    io_write(&f.m, 0x1CE, 2, 0x02);
    assert(io_read(&f.m, 0x1CF, 2) == 1);

    /* XRES 0 -> 8, which is what keeps linelength off zero and maxy off a
     * division by zero.  The write handler stores 0 verbatim (it only rejects
     * >MAX and unaligned values), so this clamp belongs to the fixup alone. */
    fx_done(&f); fx_init(&f);
    vbe_mode_set(&f.m, 0, 600, 32);
    io_write(&f.m, 0x1CE, 2, 0x01);
    assert(io_read(&f.m, 0x1CF, 2) == 8);
    io_write(&f.m, 0x1CE, 2, 0x06);
    assert(io_read(&f.m, 0x1CF, 2) == 8);          /* VIRT_WIDTH raised to XRES */
    io_write(&f.m, 0x1CE, 2, 0x07);
    /* 16777216 / (8*4) = 524288, truncated to the 16-bit register as 0
     * (QEMU's vbe_regs are u16 too -- the same fold, not a divergence). */
    assert(io_read(&f.m, 0x1CF, 2) == (uint16_t)(VRAM / 32u));

    /* BPP the write handler accepts but QEMU's depth switch does not list is
     * normalised to 8 by the fixup's default arm (33 is stored verbatim by the
     * handler -- only 0/4/8/15/16/24/32 are gated there). */
    fx_done(&f); fx_init(&f);
    vbe_mode_set(&f.m, 800, 600, 33);
    io_write(&f.m, 0x1CE, 2, 0x03);
    assert(io_read(&f.m, 0x1CF, 2) == 8);

    /* VIRT_WIDTH, written while enabled: 65535 & ~7 = 65528, over MAX, so
     * 16000 -- and maxy follows at 16777216/(16000*4) = 262. */
    fx_done(&f); fx_init(&f);
    vbe_mode_set(&f.m, 800, 600, 32);
    io_write(&f.m, 0x1CE, 2, 0x06); io_write(&f.m, 0x1CF, 2, 65535);
    assert(io_read(&f.m, 0x1CF, 2) == 16000);
    io_write(&f.m, 0x1CE, 2, 0x07);
    assert(io_read(&f.m, 0x1CF, 2) == 262);
    /* and the ~7 rounding on its own: 9 -> 8, then raised back to XRES */
    io_write(&f.m, 0x1CE, 2, 0x06); io_write(&f.m, 0x1CF, 2, 9);
    assert(io_read(&f.m, 0x1CF, 2) == 800);

    /* The offset ladder, rung by rung.  Rung 1 (clamp to MAX, nothing zeroed)
     * needs a mode small enough that the clamped maxima still FIT: at 8x1x8bpp
     * linelength is 8, so 16000/1 + 12000*8 + 1*8 = 112008 <= VRAM. */
    fx_done(&f); fx_init(&f);
    vbe_mode_set(&f.m, 8, 1, 8);
    io_write(&f.m, 0x1CE, 2, 0x08); io_write(&f.m, 0x1CF, 2, 65535);
    assert(io_read(&f.m, 0x1CF, 2) == 16000);      /* X_OFFSET -> MAX_XRES */
    io_write(&f.m, 0x1CE, 2, 0x09); io_write(&f.m, 0x1CF, 2, 65535);
    assert(io_read(&f.m, 0x1CF, 2) == 12000);      /* Y_OFFSET -> MAX_YRES */

    /* Rung 2: Y_OFFSET alone is zeroed, X_OFFSET survives.  It needs
     * 12000 * linelength + YRES * linelength > VRAM, and since Y_OFFSET is
     * clamped to MAX_YRES = 12000 first, that means linelength >= 1400 -- so
     * 400x1x32 (linelength 1600): 12000*1600 + 1*1600 = 19201600 > VRAM,
     * while the retest with Y_OFFSET = 0 leaves 400 + 1600 = 2000, which
     * fits. */
    fx_done(&f); fx_init(&f);
    vbe_mode_set(&f.m, 400, 1, 32);
    io_write(&f.m, 0x1CE, 2, 0x08); io_write(&f.m, 0x1CF, 2, 100);
    assert(io_read(&f.m, 0x1CF, 2) == 100);        /* 400 bytes, fits as-is */
    io_write(&f.m, 0x1CE, 2, 0x09); io_write(&f.m, 0x1CF, 2, 65535);
    assert(io_read(&f.m, 0x1CF, 2) == 0);          /* clamped to 12000, then zeroed */
    io_write(&f.m, 0x1CE, 2, 0x08);
    assert(io_read(&f.m, 0x1CF, 2) == 100);        /* X_OFFSET untouched */

    /* Rung 3: neither fits, so both are zeroed.  At 1024x4096x32 the screen
     * alone is exactly VRAM (4096*4096 = 16777216), so the clamped maxima
     * give 64000 + 49152000 + 16777216 > VRAM -- and dropping Y_OFFSET still
     * leaves 64000 + 16777216 > VRAM. */
    fx_done(&f); fx_init(&f);
    vbe_mode_set(&f.m, 1024, 4096, 32);
    io_write(&f.m, 0x1CE, 2, 0x08); io_write(&f.m, 0x1CF, 2, 65535);
    io_write(&f.m, 0x1CE, 2, 0x09); io_write(&f.m, 0x1CF, 2, 65535);
    io_write(&f.m, 0x1CE, 2, 0x08);
    assert(io_read(&f.m, 0x1CF, 2) == 0);
    io_write(&f.m, 0x1CE, 2, 0x09);
    assert(io_read(&f.m, 0x1CF, 2) == 0);

    /* The documented display mode is untouched by every clamp: 800x600x32
     * gives linelength 3200, and its 1920000-byte clear is well inside the
     * window (VIRT_HEIGHT still reports the full aperture / linesize). */
    fx_done(&f); fx_init(&f);
    vbe_mode_set(&f.m, 800, 600, 32);
    io_write(&f.m, 0x1CE, 2, 0x02);
    assert(io_read(&f.m, 0x1CF, 2) == 600);
    io_write(&f.m, 0x1CE, 2, 0x07);
    assert(io_read(&f.m, 0x1CF, 2) == 5242);
    assert(600u * 3200u == 800u * 600u * 4u && 600u * 3200u < VRAM);
    fx_done(&f);
    printf("ok vga dispi mode clamped to VRAM (YRES/XRES/offsets, the V4 overflow, 16 MiB)\n");
}

/* V5 panning: QEMU folds X/Y_OFFSET into `vbe_start_addr` (dwords) and its
 * display code renders from `vram + vbe_start_addr*4` at the VBE line
 * offset.  This model now does the same -- the visible display PANS when a
 * guest programs the offsets, and a VIRT_WIDTH wider than XRES advances
 * whole lines at the pitch. */
static void test_vbe_start_addr_panning(void) {
    fx_t f; fx_init(&f);
    const uint64_t L = 3200;                      /* 800x32bpp line bytes */
    vbe_mode_set(&f.m, 800, 600, 32);
    /* plant distinct pixels: row 100 col 0, and row 0 col 1 */
    mem_write(&f.m, GPU_BASE + 100 * L + 0, 4, 0x00AABBCC);
    mem_write(&f.m, GPU_BASE + 0, 4, 0x00112233);
    uint32_t px[800 * 600];

    /* no offsets: the historical tightly-packed render (pitch == w) */
    vga_render_display(&f.m, px, 800, 600);
    assert(px[0] == 0x00112233);
    assert(px[100 * 800] == 0x00AABBCC);          /* row 100 col 0 */

    /* Y_OFFSET=100: the visible row 0 IS vram row 100 */
    io_write(&f.m, 0x1CE, 2, 0x09); io_write(&f.m, 0x1CF, 2, 100);
    mem_write(&f.m, GPU_BASE + 100 * L + 4, 4, 0x00445566);
    vga_render_display(&f.m, px, 800, 600);
    assert(px[0] == 0x00AABBCC);                  /* panned */
    assert(px[1] == 0x00445566);                  /* vram row 100 col 1 */

    /* X_OFFSET=1 shifts one PIXEL: offset bytes = X_OFFSET*bits/8, so the
     * visible col 0 is vram col 1 of the panned row */
    io_write(&f.m, 0x1CE, 2, 0x08); io_write(&f.m, 0x1CF, 2, 1);
    vga_render_display(&f.m, px, 800, 600);
    assert(px[0] == 0x00445566);                  /* one pixel right */

    /* VIRT_WIDTH=1600 doubles the pitch: with start 0, output row 1 col 0
     * (== px[800]) reads vram word 1600, not 800. */
    fx_done(&f); fx_init(&f);
    vbe_mode_set(&f.m, 800, 600, 32);
    io_write(&f.m, 0x1CE, 2, 0x06); io_write(&f.m, 0x1CF, 2, 1600);
    assert(io_read(&f.m, 0x1CF, 2) == 1600);
    mem_write(&f.m, GPU_BASE + 800 * 4, 4, 0x00000001);
    mem_write(&f.m, GPU_BASE + 1600 * 4, 4, 0x00000002);
    vga_render_display(&f.m, px, 800, 600);
    assert(px[800] == 0x00000002);                /* pitch = 1600 words */
    assert(px[0] == 0x00000000);

    /* reads past the aperture render black rather than running off it.
     * The offset clamp only guards YRES rows -- a one-line mode can legally
     * pan to the end of VRAM while the renderer still draws fb_h rows. */
    fx_done(&f); fx_init(&f);
    vbe_mode_set(&f.m, 800, 1, 32);
    io_write(&f.m, 0x1CE, 2, 0x09); io_write(&f.m, 0x1CF, 2, 4643); /* near end */
    assert(io_read(&f.m, 0x1CF, 2) == 4643);      /* survives the clamp */
    vga_render_display(&f.m, px, 800, 600);
    assert(px[800 * 600 - 1] == 0);               /* black past the window */
    fx_done(&f);
    printf("ok vbe_start_addr panning (Y/X offset fold, pitch, bounded render)\n");
}

int main(void) {
    test_cells_layout();
    test_crtc_cursor_pair();
    test_glyph_pixels();
    test_display_routing();
    test_dispi_id_ladder();
    test_dispi_mode_regs();
    test_dispi_mem_bank();
    test_dispi_enable_display();
    test_vbe_mode_set_clear();
    test_dispi_mode_clamped_to_vram();
    test_vbe_start_addr_panning();
    test_gc_mode3_path();
    printf("test-video: all tests passed\n");
    return 0;
}
