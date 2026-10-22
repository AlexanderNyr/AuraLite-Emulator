/* lapic.h -- local APIC (xAPIC flavor) for the single vCPU (CHIPSET H6).
 *
 * MMIO window at 0xFEE00000, 4KB, 32-bit registers on 16-byte spacing.
 * CPUID.1:EDX already advertised APIC before this phase (measured: the
 * window absorbed writes and floated reads then) -- H6 makes the claim
 * true. IA32_APIC_BASE (MSR 0x1B) resets to base|EN|BSP on the BSP and
 * base|EN on APs (UPKEEP U4, seeded per-role by cpu_reset).
 *
 * UPKEEP U4 -- IA32_APIC_BASE is live (Intel SDM Vol.3 ss.10.4.3-10.4.5,
 * measured against QEMU master hw/intc/apic.c + apic_common.c):
 *  - the single "LAPIC" MMIO region mirrors the CURRENT context's
 *    base/EN (SDM 10.4.5 permits per-processor window addresses, and
 *    every MMIO access here executes in the accessor's context, so one
 *    region re-synced on WRMSR / reset / context switch / snapshot
 *    load reproduces per-vCPU windows exactly);
 *  - WRMSR 0x1B is validated BEFORE the store: any bit outside
 *    BASE[39:12]|EN(bit11)|BSP(bit8) -- reserved lo bits, bit9, the
 *    x2APIC EN bit (this model has no x2APIC in CPUID), or bits beyond
 *    MAXPHYADDR=40 -- rejects the whole write with state untouched
 *    (logged; no #GP, the house MSR lane is permissive elsewhere too);
 *  - the BSP bit is a machine-assigned strap: writes preserve it
 *    (QEMU apic_set_base does the same), cpu_reset seeds it by role
 *    (SDM 10.4.4: "set to 1 for the processor selected as the BSP and
 *    set to 0 for the remaining processors");
 *  - EN=0 means "functionally equivalent to a processor without an
 *    on-chip APIC" (SDM 10.4.3): the window unmaps (region disabled,
 *    so accesses fall to open bus), CPUID.1:EDX[9] reads 0, fixed
 *    interrupts from IOAPIC/MSI/IPI are dropped at the latch, the
 *    timer freezes in place, and delivery stops. Register state is
 *    KEPT across the disable (SDM only says it "may be lost"; QEMU
 *    also keeps it) except the SVR software-enable bit, which the
 *    disabling write clears (QEMU apic_set_base). INIT analogously
 *    preserves IA32_APIC_BASE (QEMU apic_init_reset does not touch
 *    apicbase; SDM 10.4.7.3's "same as reset" covers the APIC
 *    register state). Deliberately NOT copied from QEMU: its loose
 *    EN=0 behaviour (MMIO writes still apply, reads float, IRR still
 *    latches) contradicts the SDM sentence above.
 *
 * Modeled:
 *  - ID (RW), VERSION (RO, 6 LVT entries), TPR (RW), PPR (RO, computed
 *    max of TPR-class and in-service class), LDR/DFR (RW), SVR (RW:
 *    bit8 is THE software enable for delivery, reset value 0x00FF),
 *    ESR (RO sticky bits the model raises), EOI (WO);
 *  - IRR/ISR as full 256-bit maps (TMR reports 0 -- all edge);
 *  - arbitration = vector classes: deliverable iff SVR.enabled and the
 *    highest IRR vector's class (vec>>4) strictly beats PPR's class;
 *    intack moves IRR->ISR, EOI drops the highest ISR bit;
 *  - ICR: fixed-mode (DM=0) self-IPI -- destination shorthand "self",
 *    "all", or an explicit dest matching our ID / 0xFF (single vCPU:
 *    every addressed target is us; "all-excluding-self" delivers to
 *    nobody, NMI/SIPI/INIT styles are stored, not delivered);
 *  - LAPIC timer: initial/current count, DCR divide {1,2,4,8,16,32,64,
 *    128}, one-shot and periodic (LVT bit17), clocked by the
 *    deterministic virtual TSC (same instr_count x tsc_per_instr time
 *    source RDTSC reads, D6). Reaching zero while the LVT entry is
 *    unmasked sets the vector's IRR bit; masked at fire time = the edge
 *    is LOST (edge semantics, documented). The timer counts and latches
 *    IRR state regardless of SVR; only DELIVERY needs SVR.enabled --
 *    which is exactly why an interrupt that fires while the LAPIC is
 *    soft-disabled lands on the CPU the moment software re-enables it.
 *
 * PIC path stays untouched (plan D7): cpu_step consults the LAPIC
 * first and falls back to the 8259 INTR line whenever the LAPIC has
 * nothing deliverable (soft-disabled included).
 */
#ifndef LAPIC_H
#define LAPIC_H
#include <stdint.h>

struct machine;

#define LAPIC_BASE            0xFEE00000ULL
#define LAPIC_MSR_APICBASE    0x1B
#define LAPIC_MSR_BSP         (1ULL << 8)
#define LAPIC_MSR_EXTD        (1ULL << 10)  /* x2APIC enable: unsupported, rejected */
#define LAPIC_MSR_EN          (1ULL << 11)
#define LAPIC_MSR_BASE_MASK   0xFFFFFF000ULL /* bits 39:12 (CPUID.80000008 phys=40) */
#define LAPIC_MSR_DEFAULT     0xFEE00900ULL   /* power-on value on the BSP:
                                               * base | EN(bit11) | BSP(bit8);
                                               * APs reset to the same minus BSP */

/* register offsets within the 4KB window */
#define LAPIC_ID       0x020
#define LAPIC_VER      0x030
#define LAPIC_TPR      0x080
#define LAPIC_APR      0x090
#define LAPIC_PPR      0x0A0
#define LAPIC_EOI      0x0B0
#define LAPIC_LDR      0x0D0
#define LAPIC_DFR      0x0E0
#define LAPIC_SVR      0x0F0
#define LAPIC_ISR_BASE 0x100   /* ..0x170: IRR; 0x180..0x1F0: ISR; 0x200..0x270: TMR */
#define LAPIC_IRR_BASE 0x100
#define LAPIC_ESR      0x280
#define LAPIC_ICR_LO   0x300
#define LAPIC_ICR_HI   0x310
#define LAPIC_LVT_TMR  0x320
#define LAPIC_LVT_THM  0x330
#define LAPIC_LVT_PRF  0x340
#define LAPIC_LVT_LI0  0x350
#define LAPIC_LVT_LI1  0x360
#define LAPIC_LVT_ERR  0x370
#define LAPIC_TMICT    0x380
#define LAPIC_TMCCUR   0x390
#define LAPIC_DCR      0x3E0

#define LVT_MASKED     0x10000u
#define LVT_TMR_PERIODIC 0x20000u

typedef struct lapic {
    uint32_t id, tpr, ldr, dfr, svr, esr;
    uint32_t lvt[6];          /* timer, thermal, perf, lint0, lint1, error */
    uint32_t icr_hi;
    uint8_t  irr[32], isr[32];/* 256 bits each, bit v = vector v */
    uint32_t tmict, tmccur, dcr;
    uint64_t accum;           /* fractional TSC units toward a decrement */
    uint64_t last_tsu;        /* TSC-units snapshot at last lapic_tick */
} lapic_t;

void lapic_init(struct machine *m);
void lapic_mmio_register(struct machine *m);

/* UPKEEP U5: the platform INIT# (chipset CF9) delivered to every
 * context -- BSP runnable at the reset vector, APs parked in
 * wait-for-SIPI, LAPIC ID and IA32_APIC_BASE preserved; devices
 * are not involved. See lapic.c. */
void lapic_broadcast_init(struct machine *m);

/* UPKEEP U4: point the single "LAPIC" MMIO region at the CURRENT
 * context's IA32_APIC_BASE decode (base + EN-driven enable). Called on
 * an accepted WRMSR, on reset of the live cpu pair, on every vCPU
 * context switch and on snapshot load; a no-op when no window is
 * registered (bare unit harnesses). */
void lapic_sync_mmio(struct machine *m);
/* UPKEEP U4: WRMSR to IA32_APIC_BASE (see lapic.c). Validates first;
 * on rejection nothing changes. Returns 1 when the write landed. */
struct cpu;
int  lapic_apicbase_write(struct machine *m, struct cpu *c, uint64_t val);

/* Highest deliverable vector per the SVR+PPR arbitration, or -1. */
int  lapic_deliverable(struct machine *m);
/* Moves the deliverable vector IRR->ISR and returns it. */
int  lapic_intack(struct machine *m);
/* Latches a vector into IRR (H7: the IOAPIC's delivery endpoint). */
void lapic_set_irr(struct machine *m, int vector);
/* K6: same, targeted at the context owning LAPIC-id `dest` (I/O APIC
 * rte.hi, physical destination mode; unroutable dest falls back to BSP). */
void lapic_set_irr_dest(struct machine *m, uint32_t dest, int vector);
/* MSI M2: deliver one PCI MSI/MSI-X message (addr/data as programmed
 * into the capability or vector-table entry).  See lapic.c for the
 * decode and the delivery-mode honesty rules. */
void msi_deliver(struct machine *m, uint64_t addr, uint32_t data);
/* Timer: advance on virtual time; no-op until programmed. */
void lapic_tick(struct machine *m);

#endif
