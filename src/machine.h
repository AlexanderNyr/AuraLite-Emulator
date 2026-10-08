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

/* STORE S1+: AHCI attach slots (kept in step with AHCI_HW_PORTS in ahci.h) */
#define AHCI_MAX_ATTACH 6

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

/* ---- KERNEL-BOOT K6 (real SMP): second vCPU by context multiplexing --
 * m->cpu and m->lapic always hold the CURRENTLY-RUNNING context; with
 * --smp=2 the other context waits its turn in vcpu[] between steps.
 * The main loop runs a strict 1:1 round-robin, so every stat stays
 * deterministic (C9).  n_vcpus==1 (all unit tests, all firmware lanes)
 * never touches this machinery: vcpu_load/store are no-ops and
 * vtime_instr advances exactly like the old cpu.instr_count time base,
 * which keeps every UP behavior bit-exact. */
#define EM_MAX_VCPU   2
#define VCPU_OFF      0               /* slot unused entirely         */
#define VCPU_WAIT_SIPI 1              /* parked after INIT, waits SIPI */
#define VCPU_RUN      2               /* executing in the round-robin  */
typedef struct vcpu_slot {
    cpu_t    c;
    lapic_t  l;
    int      state;
    uint64_t sipi_cs_base;            /* CS base of the accepted SIPI (trace) */
} vcpu_slot_t;

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

    /* STORE S1+: SATA/AHCI attachments -- up to AHCI_MAX_ATTACH raw host
     * images, one per HBA port; NULL = port dark (PxSSTS.DET = 0). */
    uint8_t *sata_img[AHCI_MAX_ATTACH];
    size_t   sata_len[AHCI_MAX_ATTACH];

    logring_t log;

    uint64_t max_instructions; /* safety cap, 0 = unlimited */
    int running;
    int stop_requested;
    int guest_entry_seen; /* conventional guest entry at physical 0x00100000 */
    /* KERNEL-BOOT K1 (--kernel): the direct kernel-load lane's run markers */
    uint64_t kmain_va;   /* `kmain` symbol from the loaded ELF, 0 = no symtab */
    int kmain_seen;      /* logged once when RIP first reaches kmain_va */
    int shell_prompt_seen; /* STORE/SMP receipts: guest echoed "auralite#" on COM1 */
    /* KERNEL-BOOT K5 (--cpus): how many boot_cpu_t entries the kloader
     * publishes; the emulator still has exactly ONE vCPU, so entries > 1
     * exist only to meter the kernel's SMP bring-up path.  0/1 = UP. */
    int cfg_cpus;

    /* KERNEL-BOOT K6 (--smp): real executing contexts.  0/1 = one vCPU
     * (all historical behavior).  The virtual master clock: +1 per
     * retired instruction from ANY vcpu, +0 for an hlt idle slot while
     * any peer vcpu has runnable work (K8: a halted sibling consumes no
     * wall time on real hardware), and the K3 +512 quantum only when
     * EVERY running vcpu is idle -- so it equals cpu.instr_count exactly
     * when n_vcpus==1. PIT/RTC/LAPIC-timer/RDTSC all driven off this. */
    int cfg_smp;
    int dbg_pit1;             /* --smp-probe: PIT ch2 load/OUT-flip trace */
    int n_vcpus;
    int cur_vcpu;          /* context currently loaded in m->cpu/m->lapic */
    uint64_t vtime_instr;
    uint64_t watch_phys;      /* --watch-phys=addr: DW store probe, 0=off */
    vcpu_slot_t vcpu[EM_MAX_VCPU];
};
typedef struct machine machine_t;

/* K6: swap a vcpu context between its slot and the live m->cpu/m->lapic
 * pair.  Both are complete no-ops when n_vcpus == 1, so every existing
 * code path (unit suites included) keeps its byte-exact behavior and
 * never pays for the multiplexing. */
static inline void vcpu_load(machine_t *m, int i) {
    if (m->n_vcpus > 1) {
        m->cpu = m->vcpu[i].c;
        m->lapic = m->vcpu[i].l;
        m->cur_vcpu = i;
    }
}
static inline void vcpu_store(machine_t *m, int i) {
    if (m->n_vcpus > 1) {
        m->vcpu[i].c = m->cpu;
        m->vcpu[i].l = m->lapic;
    }
}
/* LAPIC of context i.  With one vCPU the identity mapping keeps the
 * live m->lapic pair as context 0 at ALL times (nothing ever swaps);
 * otherwise the running context's LAPIC is the live pair and parked
 * contexts answer from their slots. */
static inline lapic_t *vcpu_lapic(machine_t *m, int i) {
    if (m->n_vcpus <= 1) return &m->lapic;
    return i == m->cur_vcpu ? &m->lapic : &m->vcpu[i].l;
}

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
