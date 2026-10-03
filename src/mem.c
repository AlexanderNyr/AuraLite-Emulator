// mem.c -- physical address space: RAM, ROM (with its two real aliasing windows), MMIO
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stdarg.h>
#include "machine.h"

void mlog(logring_t *lr, const char *fmt, ...) {
    char buf[160];
    va_list ap; va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    int idx = (lr->head) % LOG_RING;
    snprintf(lr->lines[idx], sizeof lr->lines[idx], "%s", buf);
    lr->head = (lr->head + 1) % LOG_RING;
    if (lr->count < LOG_RING) lr->count++;
    fprintf(stderr, "%s\n", buf);
}

void mem_init(machine_t *m, const uint8_t *rom_image, size_t rom_len) {
    m->ram = calloc(1, RAM_SIZE);
    m->rom_len = ROM_SIZE;
    m->rom = calloc(1, ROM_SIZE);
    if (rom_image && rom_len) {
        size_t n = rom_len < ROM_SIZE ? rom_len : ROM_SIZE;
        /* Real firmware images are linked so the LAST byte of the image sits at
         * 0xFFFFFFFF (the reset vector). If the supplied image is smaller than
         * our alias window, right-align it, matching how real flash parts are
         * decoded (reset vector always at top). */
        memcpy(m->rom + (ROM_SIZE - n), rom_image, n);
    }
    m->mmio_list = NULL;
}

static mmio_region_t *find_mmio(machine_t *m, uint64_t addr) {
    /* On real chipsets, fixed-function BARs (EHCI, MCHBAR/DDR regs, CAR, GPU
     * VRAM, ...) take decode priority over the generic 256MB MMCONFIG/ECAM
     * window even though that window's address range nominally covers them
     * (0xE0000000-0xF0000000 spans all of those fixed BARs on Haswell-class
     * chipsets). The uncore's address decoder special-cases those smaller,
     * more specific windows ahead of the generic ECAM catch-all. We emulate
     * that priority by picking the SMALLEST matching region rather than
     * simply the most-recently-registered one, so a huge generic window
     * registered after a small specific one never shadows it. */
    mmio_region_t *best = NULL;
    for (mmio_region_t *r = m->mmio_list; r; r = r->next) {
        if (!r->enabled || addr < r->base || addr >= r->base + r->size) continue;
        if (!best || r->size < best->size) best = r;
    }
    return best;
}

void mem_register_mmio(machine_t *m, uint64_t base, uint64_t size,
                        mmio_read_fn r, mmio_write_fn w, void *ctx, const char *name) {
    mmio_region_t *reg = calloc(1, sizeof *reg);
    reg->base = base; reg->size = size; reg->read = r; reg->write = w; reg->ctx = ctx;
    reg->name = name; reg->enabled = 1; reg->owned_ctx = 0;
    reg->next = m->mmio_list;
    m->mmio_list = reg;
}

void mem_register_mmio_owned(machine_t *m, uint64_t base, uint64_t size,
                        mmio_read_fn r, mmio_write_fn w, void *ctx, const char *name) {
    mem_register_mmio(m, base, size, r, w, ctx, name);
    m->mmio_list->owned_ctx = 1;
}

uint64_t mem_read(machine_t *m, uint64_t phys, int size) {
    /* CHIPSET H5: with the A20 gate closed the bus forces address bit 20
     * to zero -- odd megabytes alias onto the even ones below (the real
     * 1MB wrap). Applies to fetches too; the CPU path funnels here. */
    if (!m->chipset.a20) phys &= ~0x100000ULL;
    /* ROM aliasing: legacy shadow window and top-of-4GB reset window both map
     * to the same 128KB firmware image (this mirrors real PCH/ICH firmware hub
     * decoding and is how the firmware's own reset vector -> 0xE000:0000 trick works). */
    if (phys >= ROM_ALIAS_LOW_BASE && phys < ROM_ALIAS_LOW_BASE + ROM_SIZE) {
        uint64_t off = phys - ROM_ALIAS_LOW_BASE;
        uint64_t v = 0;
        for (int i = 0; i < size; i++) v |= ((uint64_t)m->rom[(off + i) % ROM_SIZE]) << (8*i);
        return v;
    }
    if (phys >= ROM_TOPOF4G_BASE && phys <= 0xFFFFFFFFULL) {
        uint64_t off = phys - ROM_TOPOF4G_BASE;
        uint64_t v = 0;
        for (int i = 0; i < size; i++) v |= ((uint64_t)m->rom[(off + i) % ROM_SIZE]) << (8*i);
        return v;
    }
    if (phys < RAM_SIZE) {
        uint64_t v = 0;
        for (int i = 0; i < size; i++) {
            uint64_t a = phys + i;
            v |= (a < RAM_SIZE ? (uint64_t)m->ram[a] : 0xFFu) << (8*i);
        }
        return v;
    }
    mmio_region_t *r = find_mmio(m, phys);
    if (r) return r->read(r->ctx, phys, size);
    /* unmapped: open bus */
    return size >= 8 ? 0xFFFFFFFFFFFFFFFFULL : (uint64_t)((1ULL << (size*8)) - 1);
}

void mem_write(machine_t *m, uint64_t phys, int size, uint64_t val) {
    /* CHIPSET H5: A20 mask, same bus rule as mem_read. */
    if (!m->chipset.a20) phys &= ~0x100000ULL;
    if (phys >= ROM_ALIAS_LOW_BASE && phys < ROM_ALIAS_LOW_BASE + ROM_SIZE) return; /* ROM: read-only */
    if (phys >= ROM_TOPOF4G_BASE && phys <= 0xFFFFFFFFULL) return;                   /* ROM: read-only */
    if (phys < RAM_SIZE) {
        for (int i = 0; i < size; i++) {
            uint64_t a = phys + i;
            if (a < RAM_SIZE) m->ram[a] = (uint8_t)(val >> (8*i));
        }
        return;
    }
    mmio_region_t *r = find_mmio(m, phys);
    if (r) { r->write(r->ctx, phys, size, val); return; }
    /* unmapped: ignored (open bus) */
}

/* C10: counterpart of mem_init; the CLI and the test harness call this at
 * shutdown so the sanitizer lanes stay leak-clean. */
void mem_done(machine_t *m) {
    free(m->ram);  m->ram = NULL;
    free(m->rom);  m->rom = NULL;
}
