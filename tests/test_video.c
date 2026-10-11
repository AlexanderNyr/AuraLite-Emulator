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
    /* VIDEO_MEMORY_64K = vram / 64K = 1920000/65536 = 29, read-only */
    io_write(&f.m, 0x1CE, 2, 0x0A);
    assert(io_read(&f.m, 0x1CF, 2) == 29);
    io_write(&f.m, 0x1CF, 2, 0xFFFF);            /* ignored */
    assert(io_read(&f.m, 0x1CF, 2) == 29);
    /* BANK: QEMU mask = (vram >> 16) - 1 = 28 */
    io_write(&f.m, 0x1CE, 2, 0x05);
    io_write(&f.m, 0x1CF, 2, 0xFFFF);
    assert(io_read(&f.m, 0x1CF, 2) == 28);
    io_write(&f.m, 0x1CF, 2, 5);                 /* 5 & 28 = 4 */
    assert(io_read(&f.m, 0x1CF, 2) == 4);
    /* out-of-range index reads 0 */
    io_write(&f.m, 0x1CE, 2, 0x0B);
    assert(io_read(&f.m, 0x1CF, 2) == 0);
    fx_done(&f);
    printf("ok vga dispi mem/bank (MEMORY_64K=29 RO, bank mask 28, oob=0)\n");
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
    assert(io_read(&f.m, 0x1CF, 2) == 600);      /* 1920000 / (800*4) */
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
    test_gc_mode3_path();
    printf("test-video: all tests passed\n");
    return 0;
}
