// pci.c -- mechanism #1 (0xCF8/0xCFC) + ECAM-style MMCONFIG window.
#include <stdlib.h>
#include <string.h>
#include "pci.h"

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
    return (uint32_t)(d->cfg_read ? d->cfg_read(d, reg, size) : default_cfg_read(d, reg, size));
}
static void cfg_data_io_write(void *ctx, uint16_t port, int size, uint32_t val) {
    machine_t *m = ctx;
    uint32_t a = m->pci_config_address;
    if (!(a & 0x80000000)) return;
    int bus=(a>>16)&0xFF, dev=(a>>11)&0x1F, func=(a>>8)&0x7, reg=(a&0xFC) + (port-0xCFC);
    pci_dev_t *d = pci_find(m, bus, dev, func);
    if (!d) return;
    if (d->cfg_write) d->cfg_write(d, reg, size, val); else default_cfg_write(d, reg, size, val);
}

/* ---- ECAM / MMCONFIG window: standard encoding base|(bus<<20)|(dev<<15)|(func<<12)|reg ---- */
static uint64_t mmconfig_read(void *ctx, uint64_t addr, int size) {
    machine_t *m = ctx;
    uint64_t off = addr - m->mmconfig_base;
    int bus=(int)((off>>20)&0xFF), dev=(int)((off>>15)&0x1F), func=(int)((off>>12)&0x7), reg=(int)(off&0xFFF);
    pci_dev_t *d = pci_find(m, bus, dev, func);
    if (!d) return size==8?~0ULL:(size==4?0xFFFFFFFF:(size==2?0xFFFF:0xFF));
    if (reg < 256) return d->cfg_read ? d->cfg_read(d, reg, size) : default_cfg_read(d, reg, size);
    return 0;
}
static void mmconfig_write(void *ctx, uint64_t addr, int size, uint64_t val) {
    machine_t *m = ctx;
    uint64_t off = addr - m->mmconfig_base;
    int bus=(int)((off>>20)&0xFF), dev=(int)((off>>15)&0x1F), func=(int)((off>>12)&0x7), reg=(int)(off&0xFFF);
    pci_dev_t *d = pci_find(m, bus, dev, func);
    if (!d || reg >= 256) return;
    if (d->cfg_write) d->cfg_write(d, reg, size, val); else default_cfg_write(d, reg, size, val);
}

/* host bridge dev0/fn0 reg 0x60: the firmware's "PCIEXBAR"-style enable register */
static void hostbridge_write(pci_dev_t *d, int reg, int size, uint64_t val) {
    machine_t *m = d->ctx;
    default_cfg_write(d, reg, size, val);
    if (reg <= 0x60 && reg+size > 0x60) {
        uint32_t v = (uint32_t)(d->cfg[0x60]|(d->cfg[0x61]<<8)|(d->cfg[0x62]<<16)|(d->cfg[0x63]<<24));
        if (v & 1) {
            m->mmconfig_base = v & 0xF0000000ULL;
            mlog(&m->log, "[pci] host bridge: MMCONFIG (ECAM) window enabled at 0x%08llx", (unsigned long long)m->mmconfig_base);
            mem_register_mmio(m, m->mmconfig_base, 0x10000000ULL, mmconfig_read, mmconfig_write, m, "MMCONFIG/ECAM");
        }
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
