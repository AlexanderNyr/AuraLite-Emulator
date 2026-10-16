// pci.c -- mechanism #1 (0xCF8/0xCFC) + ECAM-style MMCONFIG window.
#include <stdlib.h>
#include <string.h>
#include "pci.h"
#include "snapshot.h"   /* TOOLING T4 */

static uint32_t cfg_addr_io_read(void *ctx, uint16_t port, int size) {
    machine_t *m = ctx; (void)port;
    uint32_t v = m->pci_config_address;
    return size==4 ? v : (uint16_t)v;
}
static void cfg_addr_io_write(void *ctx, uint16_t port, int size, uint32_t val) {
    machine_t *m = ctx; (void)port;
    if (size == 4) m->pci_config_address = val;
    else m->pci_config_address = (m->pci_config_address & 0xFFFF0000) | (val & 0xFFFF);
}

static uint64_t default_cfg_read(pci_dev_t *d, int reg, int size) {
    uint64_t v = 0;
    for (int i = 0; i < size && reg+i < 256; i++) v |= (uint64_t)d->cfg[reg+i] << (8*i);
    return v;
}
static void default_cfg_write(pci_dev_t *d, int reg, int size, uint64_t val) {
    for (int i = 0; i < size && reg+i < 256; i++) {
        int off = reg+i;
        /* loose "writable unless it's a hard-wired ID/class field" model --
         * adequate for a behavioural emulation rather than a bit-exact one.
         * Vendor/Device ID (0-3), class/subclass/progif/revision (8-11) and
         * header type (0x0E) are read-only; everything else (command,
         * status, BARs, capability pointers, ...) is writable. */
        if ((off <= 0x03) || (off >= 0x08 && off <= 0x0B) || off == 0x0E) continue;
        d->cfg[off] = (uint8_t)(val >> (8*i));
    }
}

/* VIDEO V1: exported so device-specific cfg_write hooks (the GPU's
 * BAR0-enable) can keep the same store semantics as the default path. */
void pci_default_cfg_write(pci_dev_t *d, int reg, int size, uint64_t val) {
    default_cfg_write(d, reg, size, val);
}

pci_dev_t *pci_find(machine_t *m, int bus, int dev, int func) {
    for (pci_dev_t *d = m->pci_devices; d; d = d->next)
        if (d->bus==bus && d->dev==dev && d->func==func) return d;
    return NULL;
}

static uint32_t cfg_data_io_read(void *ctx, uint16_t port, int size) {
    machine_t *m = ctx;
    uint32_t a = m->pci_config_address;
    if (!(a & 0x80000000)) return size==4?0xFFFFFFFF:(size==2?0xFFFF:0xFF);
    int bus=(a>>16)&0xFF, dev=(a>>11)&0x1F, func=(a>>8)&0x7, reg=(a&0xFC) + (port-0xCFC);
    pci_dev_t *d = pci_find(m, bus, dev, func);
    if (!d) return size==4?0xFFFFFFFF:(size==2?0xFFFF:0xFF);
    uint32_t value = (uint32_t)(d->cfg_read ? d->cfg_read(d, reg, size) : default_cfg_read(d, reg, size));
    static unsigned probe_reads;
    if (bus == 0 && dev == 31 && probe_reads++ < 128)
        mlog(&m->log, "[pci-probe] io-read fn=%d reg=0x%02x size=%d val=0x%08x rip=%llx vt=%llu",
             func, reg, size, value, (unsigned long long)m->cpu.rip,
             (unsigned long long)m->vtime_instr);
    return value;
}
static void cfg_data_io_write(void *ctx, uint16_t port, int size, uint32_t val) {
    machine_t *m = ctx;
    uint32_t a = m->pci_config_address;
    if (!(a & 0x80000000)) return;
    int bus=(a>>16)&0xFF, dev=(a>>11)&0x1F, func=(a>>8)&0x7, reg=(a&0xFC) + (port-0xCFC);
    pci_dev_t *d = pci_find(m, bus, dev, func);
    if (!d) return;
    static unsigned probe_writes;
    if (bus == 0 && dev == 31 && probe_writes++ < 128)
        mlog(&m->log, "[pci-probe] io-write fn=%d reg=0x%02x size=%d val=0x%08x rip=%llx vt=%llu",
             func, reg, size, val, (unsigned long long)m->cpu.rip,
             (unsigned long long)m->vtime_instr);
    if (d->cfg_write) d->cfg_write(d, reg, size, val); else default_cfg_write(d, reg, size, val);
}

/* ---- ECAM / MMCONFIG window: standard encoding base|(bus<<20)|(dev<<15)|(func<<12)|reg ---- */
static uint64_t mmconfig_read(void *ctx, uint64_t addr, int size) {
    machine_t *m = ctx;
    uint64_t off = addr - m->mmconfig_base;
    int bus=(int)((off>>20)&0xFF), dev=(int)((off>>15)&0x1F), func=(int)((off>>12)&0x7), reg=(int)(off&0xFFF);
    pci_dev_t *d = pci_find(m, bus, dev, func);
    if (!d) return size==8?~0ULL:(size==4?0xFFFFFFFF:(size==2?0xFFFF:0xFF));
    uint64_t value = reg < 256
        ? (d->cfg_read ? d->cfg_read(d, reg, size) : default_cfg_read(d, reg, size))
        : 0;
    static unsigned probe_reads;
    if (bus == 0 && dev == 31 && probe_reads++ < 128)
        mlog(&m->log, "[pci-probe] ecam-read fn=%d reg=0x%03x size=%d val=0x%08llx rip=%llx vt=%llu",
             func, reg, size, (unsigned long long)value, (unsigned long long)m->cpu.rip,
             (unsigned long long)m->vtime_instr);
    return value;
}
static void mmconfig_write(void *ctx, uint64_t addr, int size, uint64_t val) {
    machine_t *m = ctx;
    uint64_t off = addr - m->mmconfig_base;
    int bus=(int)((off>>20)&0xFF), dev=(int)((off>>15)&0x1F), func=(int)((off>>12)&0x7), reg=(int)(off&0xFFF);
    pci_dev_t *d = pci_find(m, bus, dev, func);
    if (!d || reg >= 256) return;
    static unsigned probe_writes;
    if (bus == 0 && dev == 31 && probe_writes++ < 128)
        mlog(&m->log, "[pci-probe] ecam-write fn=%d reg=0x%03x size=%d val=0x%08llx rip=%llx vt=%llu",
             func, reg, size, (unsigned long long)val, (unsigned long long)m->cpu.rip,
             (unsigned long long)m->vtime_instr);
    if (d->cfg_write) d->cfg_write(d, reg, size, val); else default_cfg_write(d, reg, size, val);
}

/* host bridge dev0/fn0 reg 0x60: the firmware's "PCIEXBAR"-style enable register */
static void hostbridge_write(pci_dev_t *d, int reg, int size, uint64_t val) {
    machine_t *m = d->ctx;
    uint8_t old_pam[PCI_Q35_PAM_COUNT];
    memcpy(old_pam, d->cfg + PCI_Q35_PAM0, sizeof old_pam);
    default_cfg_write(d, reg, size, val);
    /* PAM0 defines only bits 5:4 for the whole F0000-FFFFF BIOS window;
     * PAM1..6 use bits 1:0 and 5:4 for two 16-KiB windows each. In every
     * field, bit 0 enables RAM reads and bit 1 enables RAM writes. */
    for (int i = 0; i < size; i++) {
        int off = reg + i;
        if (off == PCI_Q35_PAM0)
            d->cfg[off] &= 0x30;
        else if (off > PCI_Q35_PAM0 && off < PCI_Q35_PAM0 + PCI_Q35_PAM_COUNT)
            d->cfg[off] &= 0x33;
    }
    if (memcmp(old_pam, d->cfg + PCI_Q35_PAM0, sizeof old_pam) != 0)
        perf_tlb_flush(m);  /* invalidate any decode of the old ROM/RAM view */
    if (reg <= 0x60 && reg+size > 0x60) {
        /* byte<<24 must be done in uint32_t: shifting 224<<24 in int is UB
         * (measured by the C10 UBSan lane). */
        uint32_t v = (uint32_t)d->cfg[0x60] | ((uint32_t)d->cfg[0x61] << 8)
                   | ((uint32_t)d->cfg[0x62] << 16) | ((uint32_t)d->cfg[0x63] << 24);
        if (v & 1) {
            m->mmconfig_base = v & 0xF0000000ULL;
            mlog(&m->log, "[pci] host bridge: MMCONFIG (ECAM) window enabled at 0x%08llx", (unsigned long long)m->mmconfig_base);
            mem_register_mmio(m, m->mmconfig_base, 0x10000000ULL, mmconfig_read, mmconfig_write, m, "MMCONFIG/ECAM");
        }
    }
}

/* TOOLING T4: re-create a run-time ECAM window while loading a snapshot
 * (the same registration hostbridge_write performs, without the log). */
void pci_ecam_map(machine_t *m, uint64_t base) {
    mem_register_mmio(m, base, 0x10000000ULL, mmconfig_read, mmconfig_write, m, "MMCONFIG/ECAM");
}

/* TOOLING T4: every function's 256-byte config space, in list order; the
 * record tag carries bus/dev/func so a different device list is refused. */
void pci_snap(machine_t *m, snap_t *s) {
    for (pci_dev_t *d = m->pci_devices; d; d = d->next) {
        char tag[16];
        snprintf(tag, sizeof tag, "pci.%02x.%02x.%x", d->bus & 0xFF, d->dev & 0xFF, d->func & 7);
        uint8_t cfg[256];
        memcpy(cfg, d->cfg, sizeof cfg);
        snap_bytes(s, tag, cfg, sizeof cfg);
        if (snap_loading(s)) memcpy(d->cfg, cfg, sizeof cfg);
    }
}

void pci_init(machine_t *m) {
    m->pci_devices = NULL;
    m->mmconfig_base = 0;
    io_register(m, 0xCF8, 4, cfg_addr_io_read, cfg_addr_io_write, m, "PCI CONFIG_ADDRESS");
    io_register(m, 0xCFC, 4, cfg_data_io_read, cfg_data_io_write, m, "PCI CONFIG_DATA");
}

pci_dev_t *pci_add_device(machine_t *m, int bus, int dev, int func, const char *name,
                           uint16_t vendor, uint16_t device, uint8_t class_, uint8_t subclass, uint8_t progif) {
    pci_dev_t *d = calloc(1, sizeof *d);
    d->bus=bus; d->dev=dev; d->func=func; d->name=name; d->ctx = m;
    d->cfg[0]=(uint8_t)vendor; d->cfg[1]=(uint8_t)(vendor>>8);
    d->cfg[2]=(uint8_t)device; d->cfg[3]=(uint8_t)(device>>8);
    d->cfg[0xA]=subclass; d->cfg[0xB]=class_; d->cfg[0x9]=progif;
    d->cfg[0xE]=0x00;
    if (bus==0 && dev==0 && func==0) d->cfg_write = hostbridge_write;
    d->next = m->pci_devices;
    m->pci_devices = d;
    return d;
}

/* C10: teardown keeps the ASan CI lane leak-clean. */
void pci_done(machine_t *m) {
    pci_dev_t *d = m->pci_devices;
    while (d) { pci_dev_t *next = d->next; free(d); d = next; }
    m->pci_devices = NULL;
}
