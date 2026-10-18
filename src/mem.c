// mem.c -- physical address space: RAM, ROM (with its two real aliasing windows), MMIO
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stdarg.h>
#include "machine.h"
#include "pci.h"

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
    /* The low decode is ROM until PAM enables reads from this RAM-backed
     * shadow. Seed it from the right-aligned flash image so enabling PAM
     * does not expose uninitialized data before SeaBIOS copies its writable
     * code/data sections. The flash buffer itself remains immutable.
     *
     * WARNING (the one spatial surprise on this machine): the alias window
     * 0x000E0000..0x00100000 lives INSIDE `m->ram`, not in `m->rom`.  Every
     * raw RAM read therefore sees the shadow copy -- flash contents until a
     * guest stores over it -- and a raw RAM write mutates it while `m->rom`
     * keeps the pristine image (which is exactly what PAM's RAM-write
     * select wants).  Anything that treats "in-range" as "plain RAM" must
     * know this: perf_tlb_plain_table() excludes the window for exactly
     * that reason, and a future RAM-only cache MUST keep doing so. */
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

#define PAM_WINDOW_BASE 0x000C0000ULL
#define PAM_WINDOW_END  0x00100000ULL

/* The advertised host bridge is Q35. Its seven PAM bytes are also the
 * source of truth for the memory decoder; there is no shadow-enable state
 * that can get out of sync with PCI config space or snapshots. */
static pci_dev_t *q35_host_bridge(machine_t *m) {
    for (pci_dev_t *d = m->pci_devices; d; d = d->next) {
        uint16_t vendor = (uint16_t)d->cfg[0] | ((uint16_t)d->cfg[1] << 8);
        uint16_t device = (uint16_t)d->cfg[2] | ((uint16_t)d->cfg[3] << 8);
        if (d->bus == 0 && d->dev == 0 && d->func == 0 &&
            vendor == 0x8086 && device == PCI_DEVICE_ID_INTEL_Q35_MCH)
            return d;
    }
    return NULL;
}

/* Return the PAM field value (bit 0: read RAM, bit 1: write RAM) for a
 * physical byte in C0000-FFFFF. PAM0's bits 5:4 select the entire 64-KiB
 * F0000-FFFFF BIOS window; PAM1..6 each select two 16-KiB halves. */
static uint8_t q35_pam_mode(const pci_dev_t *hb, uint64_t phys) {
    unsigned reg, shift;
    if (phys >= 0x000F0000ULL) {
        reg = PCI_Q35_PAM0;
        shift = 4;
    } else {
        uint64_t off = phys - PAM_WINDOW_BASE;
        reg = PCI_Q35_PAM0 + 1 + (unsigned)(off >> 15);
        shift = (off & 0x7FFFu) >= 0x4000u ? 4 : 0;
    }
    return (uint8_t)((hb->cfg[reg] >> shift) & 0x03);
}

static uint8_t q35_pam_read_byte(machine_t *m, const pci_dev_t *hb,
                                 uint64_t phys) {
    if (q35_pam_mode(hb, phys) & 0x01)
        return m->ram[phys];
    if (phys >= ROM_ALIAS_LOW_BASE)
        return m->rom[(phys - ROM_ALIAS_LOW_BASE) % ROM_SIZE];
    /* C0000-DFFFF is an option-ROM window; no expansion-ROM image is
     * currently installed, so reads with PAM closed are open bus. */
    return 0xFF;
}

/* Debugger/trace reads must see the same side-effect-free byte as a CPU
 * physical read, including Q35 PAM's RAM-shadow selection. Do not call a
 * device MMIO handler merely to inspect memory. */
int mem_debug_peek_byte(machine_t *m, uint64_t phys, uint8_t *out) {
    if (!out) return 0;
    if (!m->chipset.a20) phys &= ~0x100000ULL;
    if (phys >= PAM_WINDOW_BASE && phys < PAM_WINDOW_END) {
        pci_dev_t *hb = q35_host_bridge(m);
        if (hb) {
            *out = q35_pam_read_byte(m, hb, phys);
            return 1;
        }
    }
    if (phys >= ROM_ALIAS_LOW_BASE && phys < ROM_ALIAS_LOW_BASE + ROM_SIZE) {
        *out = m->rom[(phys - ROM_ALIAS_LOW_BASE) % ROM_SIZE];
        return 1;
    }
    if (phys >= ROM_TOPOF4G_BASE && phys <= 0xFFFFFFFFULL) {
        *out = m->rom[(phys - ROM_TOPOF4G_BASE) % ROM_SIZE];
        return 1;
    }
    if (phys < RAM_SIZE) {
        *out = m->ram[phys];
        return 1;
    }
    mmio_region_t *r = find_mmio(m, phys);
    if (r) {
        if (!r->dbg_readable || !r->read) return 0;
        *out = (uint8_t)r->read(r->ctx, phys, 1);
        return 1;
    }
    *out = 0xFF;                   /* unmapped open bus */
    return 1;
}

uint64_t mem_read(machine_t *m, uint64_t phys, int size) {
    /* CHIPSET H5: with the A20 gate closed the bus forces address bit 20
     * to zero -- odd megabytes alias onto the even ones below (the real
     * 1MB wrap). Applies to fetches too; the CPU path funnels here. */
    if (!m->chipset.a20) phys &= ~0x100000ULL;
    /* Q35 PAM controls reads from RAM throughout C0000-FFFFF. The E0000-
     * FFFFF half additionally falls back to the immutable 128-KiB firmware
     * image; absent C0000-DFFFF option ROMs read as open bus. */
    if (phys < PAM_WINDOW_END && phys + (uint64_t)size > PAM_WINDOW_BASE) {
        pci_dev_t *hb = q35_host_bridge(m);
        if (hb) {
            uint64_t v = 0;
            for (int i = 0; i < size; i++) {
                uint64_t a = phys + (uint64_t)i;
                uint8_t b;
                if (a >= PAM_WINDOW_BASE && a < PAM_WINDOW_END)
                    b = q35_pam_read_byte(m, hb, a);
                else if (a >= ROM_ALIAS_LOW_BASE && a < ROM_ALIAS_LOW_BASE + ROM_SIZE)
                    b = m->rom[(a - ROM_ALIAS_LOW_BASE) % ROM_SIZE];
                else if (a < RAM_SIZE)
                    b = m->ram[a];
                else
                    b = 0xFF;
                v |= (uint64_t)b << (8*i);
            }
            return v;
        }
    }
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
    /* PERF P1: profile-guided 8-byte RAM read.  Five firmware runs under
     * gprof found 235535030 mem_read calls from translate() -- its PTE/PDE/
     * PDPTE/PML4E walks all use size=8.  A single unaligned-safe memcpy
     * replaces eight byte-at-a-time checks only when the ENTIRE read stays
     * in RAM, ROM aliasing above has already been handled, and the A20
     * gate is open.  Keep the old byte loop for every boundary, disabled
     * A20, ROM window, and non-8-byte read.  memcpy avoids type-punning/
     * alignment UB; host big-endian builds use the original LE byte loop. */
#if defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
    if (m->chipset.a20 && size == 8 && phys <= (uint64_t)RAM_SIZE - 8) {
        uint64_t v;
        memcpy(&v, m->ram + phys, sizeof v);
        return v;  /* PERF P1: fast path, exactly 8 in-range RAM bytes */
    }
#endif
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
    /* --watch-phys=addr: one-address DW store probe.  Runs BEFORE the A20
     * mask on purpose (the probe address is a raw guest physical in the
     * pre-A20 namespace, matching what the store instruction targeted). */
    if (m->watch_phys && phys <= m->watch_phys &&
        phys + (uint64_t)size > m->watch_phys) {
        mlog(&m->log, "[watch] STORE phys=0x%llx size=%d val=0x%llx "
             "vcpu%d rip=0x%llx vtime=%llu",
             (unsigned long long)phys, size,
             (unsigned long long)(val & ((size >= 8) ? ~0ULL
                                          : ((1ULL << (size*8)) - 1))),
             m->cur_vcpu, (unsigned long long)m->cpu.rip,
             (unsigned long long)m->vtime_instr);
    }
    /* CHIPSET H5: A20 mask, same bus rule as mem_read. */
    if (!m->chipset.a20) phys &= ~0x100000ULL;
    /* PAM writes are routed per byte: a single store may straddle PAM
     * subregions or the 1-MiB boundary. Write-disabled bytes are discarded;
     * enabled writes update the RAM shadow, never the flash buffer. */
    if (phys < PAM_WINDOW_END && phys + (uint64_t)size > PAM_WINDOW_BASE) {
        pci_dev_t *hb = q35_host_bridge(m);
        if (hb) {
            int changed_ram = 0;
            for (int i = 0; i < size; i++) {
                uint64_t a = phys + (uint64_t)i;
                if (a >= PAM_WINDOW_BASE && a < PAM_WINDOW_END) {
                    if (q35_pam_mode(hb, a) & 0x02) {
                        m->ram[a] = (uint8_t)(val >> (8*i));
                        changed_ram = 1;
                    }
                } else if (a < RAM_SIZE) {
                    m->ram[a] = (uint8_t)(val >> (8*i));
                    changed_ram = 1;
                }
            }
            if (changed_ram) perf_tlb_flush(m);
            return;
        }
    }
    if (phys >= ROM_ALIAS_LOW_BASE && phys < ROM_ALIAS_LOW_BASE + ROM_SIZE) return; /* ROM: read-only */
    if (phys >= ROM_TOPOF4G_BASE && phys <= 0xFFFFFFFFULL) return;                   /* ROM: read-only */
    if (phys < RAM_SIZE) {
        for (int i = 0; i < size; i++) {
            uint64_t a = phys + i;
            if (a < RAM_SIZE) m->ram[a] = (uint8_t)(val >> (8*i));
        }
        /* PERF P2: a guest OR DMA RAM store may have modified any paging
         * level.  Even if this write was ordinary data, flush conservatively
         * rather than reuse a translation that might now have stale rights,
         * physical mapping or fault ordering. */
        perf_tlb_flush(m);
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
