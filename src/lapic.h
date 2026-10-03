/* lapic.h -- local APIC (xAPIC flavor) for the single vCPU (CHIPSET H6).
 *
 * MMIO window at 0xFEE00000, 4KB, 32-bit registers on 16-byte spacing.
 * CPUID.1:EDX already advertised APIC before this phase (measured: the
 * window absorbed writes and floated reads then) -- H6 makes the claim
 * true. IA32_APIC_BASE (MSR 0x1B) resets to 0xFEE00900 (EN|BSP, seeded
 * by cpu_reset); MSR-based relocation/disable is not modeled (stored
 * only, documented).
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
#define LAPIC_MSR_DEFAULT     0xFEE00900ULL   /* base | EN(bit11) | BSP(bit8) */

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

/* Highest deliverable vector per the SVR+PPR arbitration, or -1. */
int  lapic_deliverable(struct machine *m);
/* Moves the deliverable vector IRR->ISR and returns it. */
int  lapic_intack(struct machine *m);
/* Latches a vector into IRR (H7: the IOAPIC's delivery endpoint). */
void lapic_set_irr(struct machine *m, int vector);
/* Timer: advance on virtual time; no-op until programmed. */
void lapic_tick(struct machine *m);

#endif
