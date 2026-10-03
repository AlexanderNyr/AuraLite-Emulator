// machine.h -- the "chipset": memory map, I/O ports, PCI, devices, CPU glue
#ifndef MACHINE_H
#define MACHINE_H
#include <stdint.h>
#include <stddef.h>
#include "cpu.h"
#include "pic.h"
#include "pit.h"
#include "rtc.h"
#include "kbc.h"
#include "chipset.h"
#include "lapic.h"
#include "ioapic.h"

#define RAM_SIZE   (128u*1024*1024)   /* matches bochsrc "megs:128" */
#define ROM_SIZE   (128u*1024)        /* 0x20000, matches firmware.bin size */

#define ROM_ALIAS_LOW_BASE   0x000E0000ULL   /* legacy 128K BIOS shadow window */
#define ROM_TOPOF4G_BASE     0xFFFE0000ULL   /* top-of-4GB reset window */

/* ---- MMIO callback device model ---- */
typedef uint64_t (*mmio_read_fn)(void *ctx, uint64_t addr, int size);
typedef void     (*mmio_write_fn)(void *ctx, uint64_t addr, int size, uint64_t val);

typedef struct mmio_region {
    uint64_t base, size;
    mmio_read_fn read;
    mmio_write_fn write;
    void *ctx;
    const char *name;
    int enabled;
    int owned_ctx;              /* C10: devices_done() frees ctx (rw2_t) only when set */
    struct mmio_region *next;
} mmio_region_t;

/* ---- I/O port device model (64K space) ---- */
typedef uint32_t (*io_read_fn)(void *ctx, uint16_t port, int size);
typedef void     (*io_write_fn)(void *ctx, uint16_t port, int size, uint32_t val);

typedef struct io_port {
    io_read_fn read;
    io_write_fn write;
    void *ctx;
    const char *name;
} io_port_t;

/* ---- Event log for the GUI / debugging ---- */
#define LOG_RING 4096
typedef struct {
    char lines[LOG_RING][160];
    int  head, count;
} logring_t;
void mlog(logring_t *lr, const char *fmt, ...);

struct machine {
    cpu_t cpu;
    uint8_t *ram;
    uint8_t *rom;      /* 128KB firmware image, read-only to the guest */
    size_t   rom_len;

    mmio_region_t *mmio_list;
    io_port_t io[65536];

    /* CHIPSET H0: 8259A master+slave pair (see pic.h) */
    pic_t pic;
    pit_t pit;
    rtc_t rtc;
    kbc_t kbc;
    chipset_t chipset;   /* CHIPSET H5: A20 gate + reset plumbing */
    lapic_t lapic;       /* CHIPSET H6: local APIC for the single vCPU */
    ioapic_t ioapic;     /* CHIPSET H7: 82093AA I/O APIC, ISA lines fanned out */

    /* PCI mechanism #1 state */
    uint32_t pci_config_address;
    uint64_t mmconfig_base;
    struct pci_dev *pci_devices; /* linked list, see pci.h */

    /* platform (chipset generation) profile, see platform.h */
    struct platform *plat;

    /* framebuffer (HDMI/GPU) */
    uint32_t *fb;
    int fb_w, fb_h;

    /* disk image backing the emulated USB mass storage stick */
    uint8_t *disk;
    size_t   disk_len;

    logring_t log;

    uint64_t max_instructions; /* safety cap, 0 = unlimited */
    int running;
    int stop_requested;
    int guest_entry_seen; /* conventional guest entry at physical 0x00100000 */
};
typedef struct machine machine_t;

/* memory.c */
void      mem_init(machine_t *m, const uint8_t *rom_image, size_t rom_len);
void      mem_done(machine_t *m);                            /* C10: frees ram/rom */
uint64_t  mem_read(machine_t *m, uint64_t phys, int size);
void      mem_write(machine_t *m, uint64_t phys, int size, uint64_t val);
void      mem_register_mmio(machine_t *m, uint64_t base, uint64_t size,
                             mmio_read_fn r, mmio_write_fn w, void *ctx, const char *name);
void      mem_register_mmio_owned(machine_t *m, uint64_t base, uint64_t size,
                        mmio_read_fn r, mmio_write_fn w, void *ctx, const char *name); /* C10 */

/* io.c */
void     io_init(machine_t *m);
void     io_register(machine_t *m, uint16_t port, int count, io_read_fn r, io_write_fn w, void *ctx, const char *name);
uint32_t io_read(machine_t *m, uint16_t port, int size);
void     io_write(machine_t *m, uint16_t port, int size, uint32_t val);

#endif
