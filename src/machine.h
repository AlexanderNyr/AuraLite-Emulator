// machine.h -- the "chipset": memory map, I/O ports, PCI, devices, CPU glue
#ifndef MACHINE_H
#define MACHINE_H
#include <stdint.h>
#include <stddef.h>
#include "cpu.h"
#include "pic.h"
#include "pit.h"
#include "pm.h"
#include "rtc.h"
#include "kbc.h"
#include "chipset.h"
#include "lapic.h"
#include "ioapic.h"

#define RAM_SIZE   (128u*1024*1024)   /* matches bochsrc "megs:128" */
#define ROM_SIZE   (128u*1024)        /* 0x20000, matches firmware.bin size */

#define ROM_ALIAS_LOW_BASE   0x000E0000ULL   /* 128K firmware flash alias; PAM selects RAM shadow */
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
    int dbg_readable;           /* TOOLING T2: read() is a pure RAM-window read with no
                                 * side effect, so debuggers may call it */
    struct mmio_region *next;
} mmio_region_t;

/* STORE S1+: AHCI attach slots (kept in step with AHCI_HW_PORTS in ahci.h).
 * STORE S4: slots are per controller -- [ctrl][port], ctrl < AHCI_CTRLS
 * (the second controller appears only under --ahci2; ctrl 0 is the
 * onboard 0:31:2 function every historical lane uses). */
#define AHCI_MAX_ATTACH 6
#define AHCI_CTRLS      2

/* ---- I/O port device model (64K space) ---- */
typedef uint32_t (*io_read_fn)(void *ctx, uint16_t port, int size);
typedef void     (*io_write_fn)(void *ctx, uint16_t port, int size, uint32_t val);

typedef struct io_port {
    io_read_fn read;
    io_write_fn write;
    void *ctx;
    const char *name;
} io_port_t;

/* PERF P2: one shared (machine-wide) successful-translation cache.  The
 * 4-KiB key works for both 4-KiB pages and the current 2-MiB/1-GiB PS
 * leaves: each cached subpage gets its own physical base.  A shared table
 * avoids copying cache arrays at EVERY vCPU load/store (K6), and the key
 * includes CR3/permission context; RAM writes invalidate every context.
 * Negative caching is deliberately forbidden: the original walk must raise
 * EVERY fault, with the original CR2/error code/RIP and delivery ordering.
 */
#define PERF_TLB_SLOTS 256u

typedef struct perf_tlb_entry {
    uint64_t generation, linear_page, phys_page;
    uint64_t cr3, cr0, cr4, efer;
    uint8_t ac, cpl, desc_sv, mode, valid;
} perf_tlb_entry_t;

/* PERF P3: cache ONLY the prefix/opcode predecode of a RAM-backed
 * instruction, never ModRM's dynamic effective address or immediates.
 * Use P2's global RAM-write generation: one store to guest code or any
 * page table invalidates both translation and predecode, in O(1).  A cache
 * entry records the decoded prefixes, the opcode and the byte cursor
 * after it; it may only be installed when the P2 execute translation is
 * a successful plain-RAM walk and all prefix/opcode bytes stay in one
 * 4KiB page.  Faulting fetches, side-effecting MMIO/ROM and page-crossing
 * prefix streams remain on the original per-byte fetch path. */
#define PERF_DECODE_SLOTS 512u
typedef struct perf_decode_entry {
    uint64_t generation, rip, cs_base, next_pc;
    uint64_t cr0, cr3, cr4, efer;
    uint16_t cs_sel;
    uint8_t cs_l, cs_db, ac, desc_sv;
    uint8_t opcode, valid;
    int addrsize, opsize, has_rex, rex_w, rex_r, rex_x, rex_b;
    int seg_override, rep;
} perf_decode_entry_t;

/* ---- Event log for the GUI / debugging ---- */
struct dbg_state;   /* TOOLING: src/dbg.c, opaque to everything else */

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
#define EM_MAX_VCPU   8
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
    uint8_t *rom;      /* immutable 128KB flash image; PAM shadow is backed by RAM */
    size_t   rom_len;

    mmio_region_t *mmio_list;
    /* P2: internal-only TLB.  No guest CPU field and no K6 clock change.
     * Generation invalidation is O(1) on each guest/DMA RAM write; on the
     * impossible wrap to 0, explicitly clear validity before reuse. */
    perf_tlb_entry_t perf_tlb[PERF_TLB_SLOTS];
    uint64_t perf_tlb_generation;
    uint64_t perf_tlb_hits, perf_tlb_misses, perf_tlb_flushes;
    perf_decode_entry_t perf_decode[PERF_DECODE_SLOTS];
    uint64_t perf_decode_hits, perf_decode_misses;
    io_port_t io[65536];

    /* CHIPSET H0: 8259A master+slave pair (see pic.h) */
    pic_t pic;
    pit_t pit;
    pm_t pm;             /* COMPAT G5: ACPI PM block (TMR at PMBASE+8) */
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
     * images per controller, one per HBA port; NULL = port dark
     * (PxSSTS.DET = 0).  STORE S4: row 0 = onboard controller (0:31:2),
     * row 1 = the --ahci2 controller (0:31:3). */
    uint8_t *sata_img[AHCI_CTRLS][AHCI_MAX_ATTACH];
    size_t   sata_len[AHCI_CTRLS][AHCI_MAX_ATTACH];
    /* STORE S6: attach kind 0 = ATA disk, 1 = ATAPI CD (--cdrom=). */
    uint8_t  sata_kind[AHCI_CTRLS][AHCI_MAX_ATTACH];

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
    int cfg_no_fb;        /* VIDEO V1: --no-fb -- publish no linear fb (VGA text box) */
    /* COMPAT G4: --lpc=ich9 identifies the LPC bridge as 8086:2918 (the
     * ICH9 LPC function that belongs to the Q35 host bridge this machine
     * already advertises).  SeaBIOS rel-1.16.3's acpi_setup() matches only
     * PIIX4 8086:7113 or ICH9 8086:2918, so the default Panther Point
     * identity (8086:1e55, the ID the sample firmware knows) makes stock
     * SeaBIOS skip RSDP publication entirely.  0 = historical identity. */
    int cfg_lpc_ich9;

    /* USB U5 (--no-usb-uhci): omit the UHCI companion entirely so the
     * EHCI-only machine shape of the guest's test_usb_ehci.sh QEMU lane
     * (-device usb-ehci, no -usb) is reproducible.  0 = UHCI present
     * (all historical behavior). */
    int cfg_no_uhci;

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
    /* TOOLING: the attached tooling core (trace, debugger, gdbstub,
     * monitor). NULL unless a T flag asked for it; never guest state. */
    struct dbg_state *dbg;
    /* TOOLING T2: the run loop's step counter (what --max-instr budgets),
     * formerly a main() local; one loop iteration per vCPU step. */
    uint64_t run_steps;
};
typedef struct machine machine_t;

/* PERF P2: flush on any RAM write and on every paging-control operation.
 * This is deliberately global rather than CR3/linear scoped: device DMA
 * writes may update a page table through mem_write, and the current bus has
 * no tracked page-table ownership.  Conservatism is correct; the benchmark
 * determines whether it is fast enough to keep. */
static inline void perf_tlb_flush(machine_t *m) {
    ++m->perf_tlb_flushes;
    if (++m->perf_tlb_generation == 0) {
        for (size_t i = 0; i < PERF_TLB_SLOTS; ++i) m->perf_tlb[i].valid = 0;
        /* The shared epoch also guards P3 predecode; clear both tables
         * before the mathematically impossible uint64_t wrap is reused. */
        for (size_t i = 0; i < PERF_DECODE_SLOTS; ++i) m->perf_decode[i].valid = 0;
        m->perf_tlb_generation = 1;
    }
}

/* K6: swap a vcpu context between its slot and the live m->cpu/m->lapic
 * pair.  Both are complete no-ops when n_vcpus == 1, so every existing
 * code path (unit suites included) keeps its byte-exact behavior and
 * never pays for the multiplexing. */
/* PERF P0: instruction attempts actually executed across every execution context.
 *
 * This is the throughput numerator, and it is deliberately NOT any of the
 * two clocks that already exist:
 *   - cpu.instr_count is the K6 clock.  It also carries the K3 idle quantum
 *     (+512 per all-idle HLT slot, cpu.c) and it is guest-visible through
 *     PIT/RTC/LAPIC/RDTSC, so its semantics are frozen by contract.
 *   - vtime_instr is the machine-wide master clock; it equals the BSP's
 *     instr_count when there is one context (the K6 identity) and is the
 *     number --max-instr budgets against, idle quantum included.
 *   - neither distinguishes "attempted an instruction" from "sat in HLT"
 *     while virtual time flowed.  That distinction is a throughput measure.
 * cpu.executed_steps increments only in cpu_step's done block, including
 * faulting attempts.  An idle-only run reports 0 here even as its
 * instr_count climbs by 512 per slot.
 *
 * Slot 0 lives in m->cpu (the K6 exit path re-loads it, and with a single
 * context the slot is never written); the rest are authoritative in their
 * slots.  n_vcpus is 0 for the historical single-context machine -- only the
 * --smp>=2 wiring sets it -- so the effective context count is max(1, n).
 * Shared by main.c's --stats block and tests/test_perf.c so the test asserts
 * the shipped formula rather than a copy of it. */
static inline uint64_t machine_executed_total(const machine_t *m) {
    int n = m->n_vcpus > 1 ? m->n_vcpus : 1;
    uint64_t sum = m->cpu.executed_steps;
    for (int i = 1; i < n && i < EM_MAX_VCPU; i++) sum += m->vcpu[i].c.executed_steps;
    return sum;
}

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
/* Side-effect-free one-byte physical peek that follows RAM/ROM/PAM decoding
 * and refuses device MMIO callbacks (used by debugger inspection). */
int       mem_debug_peek_byte(machine_t *m, uint64_t phys, uint8_t *out);
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
