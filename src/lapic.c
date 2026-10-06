/* lapic.c -- local APIC (xAPIC) for the single vCPU (CHIPSET H6).
 * Contract in lapic.h. Bus-facing bits are an MMIO window; the CPU
 * interface is deliverable -> intack(IRR->ISR) -> EOI(ISR-). */
#include <stdio.h>
#include <string.h>
#include "machine.h"
#include "platform.h"
#include "lapic.h"
#include "ioapic.h"

/* ------------------------------------------------ vector-map helpers */

static int map_highest(const uint8_t *map) {
    for (int i = 31; i >= 2; i--)        /* vectors 0-15 are exceptions, */
        if (map[i]) {                    /* never set by this model      */
            int b = 7;
            while (!((map[i] >> b) & 1)) b--;
            return i * 8 + b;
        }
    return -1;
}
static void map_set(uint8_t *map, int v)   { map[v >> 3] |= (uint8_t)(1u << (v & 7)); }
static void map_clear(uint8_t *map, int v) { map[v >> 3] &= (uint8_t)~(1u << (v & 7)); }

/* --------------------------------------------------- arbitration */

static uint32_t lapic_ppr(const lapic_t *l) {
    int isrv = map_highest(l->isr);
    if (isrv >= 0 && (isrv >> 4) > (int)(l->tpr >> 4))
        return (uint32_t)isrv & 0xF0u;   /* in-service class wins, sub = 0 */
    return l->tpr;
}

int lapic_deliverable(machine_t *m) {
    lapic_t *l = &m->lapic;
    if (!(l->svr & 0x100)) return -1;                /* software-disabled */
    int v = map_highest(l->irr);
    if (v < 0) return -1;
    if ((v >> 4) <= (int)(lapic_ppr(l) >> 4)) return -1;  /* class blocked */
    return v;
}

int lapic_intack(machine_t *m) {
    int v = lapic_deliverable(m);
    if (v < 0) return -1;
    map_clear(m->lapic.irr, v);
    map_set(m->lapic.isr, v);
    if (m->dbg_pit1 && v == 32)
        mlog(&m->log, "[lapic-dbg] intack vec32 vcpu%d rip=0x%llx vtime=%llu",
             m->cur_vcpu, (unsigned long long)m->cpu.rip,
             (unsigned long long)m->vtime_instr);
    return v;
}

static void lapic_eoi(machine_t *m) {
    int v = map_highest(m->lapic.isr);
    if (v >= 0) {
        if (m->dbg_pit1 && v == 32)
            mlog(&m->log, "[lapic-dbg] EOI vec32 vcpu%d vtime=%llu",
                 m->cur_vcpu, (unsigned long long)m->vtime_instr);
        map_clear(m->lapic.isr, v);
        /* H7: level-triggered IOAPIC entries on this vector un-latch
         * their remote_IRR and may redeliver a still-asserted line. */
        ioapic_eoi_notify(m, v);
    }
}

void lapic_set_irr(machine_t *m, int vector) {
    if (vector < 16) { m->lapic.esr |= (1u << 6); return; }
    map_set(m->lapic.irr, vector);
}

/* --------------------------------------------------------- timer */

static uint32_t dcr_div(uint32_t v) {
    static const uint8_t t[8] = { 2, 4, 8, 16, 32, 64, 128, 1 };
    return t[(v & 3) | ((v >> 1) & 4)];
}

void lapic_tick(machine_t *m) {
    lapic_t *l = &m->lapic;
    /* D6 virtual time: identical to what RDTSC reads (deterministic).
     * K6: the master clock is vtime_instr; with n_vcpus==1 it equals
     * instr_count bit-for-bit, so UP behavior is unchanged.  Because the
     * delta accumulator is per-context, a suspended vcpu's timer simply
     * catches up with the whole elapsed window at its next turn. */
    uint64_t now = m->vtime_instr * (uint64_t)m->plat->tsc_per_instr;
    uint64_t delta = now - l->last_tsu;
    l->last_tsu = now;
    if (!l->tmict || !delta) return;

    uint32_t div = dcr_div(l->dcr);
    l->accum += delta;
    if (l->accum < div) return;
    uint64_t step = l->accum / div;         /* whole decrements due */
    l->accum %= div;

    while (step) {
        uint64_t take = step < l->tmccur ? step : l->tmccur;
        l->tmccur -= (uint32_t)take;
        step -= take;
        if (l->tmccur) continue;
        /* hit zero: unmasked LVT latches the vector, masked loses the edge */
        if (!(l->lvt[0] & LVT_MASKED) && !(l->lvt[0] & (7u << 8))) {
            int v = (int)(l->lvt[0] & 0xFF);
            if (v >= 16) map_set(l->irr, v);
            else l->esr |= (1u << 6);      /* receive-illegal-vector */
        }
        if (l->lvt[0] & LVT_TMR_PERIODIC) l->tmccur = l->tmict;  /* reload */
        else break;                        /* one-shot: stays at zero */
    }
}

/* ------------------------------------------------------ ICR / IPI */

/* K6: locate the running slot that owns a LAPIC-id (-1 = nobody). */
static int lapic_index_by_id(machine_t *m, uint32_t apic_id) {
    for (int i = 0; i < m->n_vcpus && i < EM_MAX_VCPU; i++) {
        lapic_t *li = vcpu_lapic(m, i);
        if (((li->id >> 24) & 0xFF) == apic_id) return i;
    }
    return -1;
}

/* K6: latch a vector into the context owning LAPIC-id `dest` (the I/O
 * APIC's rte.hi endpoint); an unroutable destination falls back to the
 * BSP context, documented. */
void lapic_set_irr_dest(machine_t *m, uint32_t dest, int vector) {
    if (vector < 16) { m->lapic.esr |= (1u << 6); return; }
    int idx = lapic_index_by_id(m, dest);
    if (idx < 0) idx = 0;
    map_set(vcpu_lapic(m, idx)->irr, vector);
}

/* K6: an INIT arriving at context idx (Intel MP spec s.8.4: INIT resets
 * most architectural state and parks the cpu in wait-for-SIPI; the
 * LAPIC keeps its ID).  INIT asserted at the RUNNING context is the
 * "INIT self" case -- illegal on real silicon, we log-and-refuse. */
static void vcpu_take_init(machine_t *m, int idx) {
    vcpu_slot_t *s = &m->vcpu[idx];
    if (idx == m->cur_vcpu) {
        mlog(&m->log, "[lapic] INIT at the RUNNING vcpu%d ignored (self)", idx);
        return;
    }
    cpu_reset(&s->c);
    lapic_t keep = s->l;                 /* preserve the LAPIC ID value */
    lapic_t *ll = &s->l;
    uint32_t id = keep.id;
    memset(ll, 0, sizeof *ll);
    ll->id = id;
    ll->dfr = 0xFFFFFFFFu;
    ll->svr = 0x00FFu;
    for (int i = 0; i < 6; i++) ll->lvt[i] = LVT_MASKED;
    s->state = VCPU_WAIT_SIPI;
    mlog(&m->log, "[lapic] vcpu%d took INIT: state reset, parked in "
         "wait-for-SIPI", idx);
}

/* K6: SIPI lands only on a cpu parked in wait-for-SIPI (SDM Vol.3
 * s.8.4.2: a SIPI at a cpu in any other state is discarded, which is
 * also why the spec's duplicate SIPI is safe).  Execution starts in
 * real mode at (vector<<12):0 with the post-INIT register image. */
static void vcpu_take_sipi(machine_t *m, int idx, uint8_t vec) {
    vcpu_slot_t *s = &m->vcpu[idx];
    if (s->state != VCPU_WAIT_SIPI) {
        if (idx != m->cur_vcpu && s->state != VCPU_OFF)
            mlog(&m->log, "[lapic] vcpu%d SIPI vec=0x%02x discarded "
                 "(not in wait-for-SIPI)", idx, vec);
        return;
    }
    s->c.seg[SEG_CS].sel  = (uint16_t)(vec << 8);
    s->c.seg[SEG_CS].base = (uint64_t)vec << 12;
    s->c.rip = 0;
    s->c.halted = 0;
    s->sipi_cs_base = (uint64_t)vec << 12;
    s->state = VCPU_RUN;
    mlog(&m->log, "[lapic] vcpu%d took SIPI: entering real mode at "
         "0x%05llx:0", idx, (unsigned long long)s->sipi_cs_base);
}

static void lapic_icr_write(machine_t *m, uint32_t v) {
    lapic_t *l = &m->lapic;
    uint32_t dm = (v >> 8) & 7;
    int shorthand = (int)((v >> 18) & 3);
    if (dm != 0) {
        /* K5 metering + K6 delivery: INIT (dm=5, assert / level-deassert)
         * and STARTUP (dm=6) are architecture-legal send modes (Intel MP
         * spec s.B.4), not ESR "send illegal" material.  On one vCPU
         * they simply have no receiver (the K5 trace); with --smp=2 an
         * IPI whose destination LAPIC-id matches our parked AP actually
         * lands there. */
        const char *kind =
            dm == 5 ? ((v & (1u << 14)) ? "INIT-assert" : "INIT-deassert") :
            dm == 6 ? "SIPI" : "exotic";
        uint32_t dest = (l->icr_hi >> 24) & 0xFF;
        if (dm != 5 && dm != 6) {
            l->esr |= (1u << 5);                 /* genuinely illegal send */
            return;
        }
        if (dm == 5 && !(v & (1u << 14))) {
            mlog(&m->log, "[lapic] IPI INIT-deassert: dst=%u (no-op; INIT "
                 "already latched)", dest);
            return;                              /* level-deassert: nothing resets */
        }
        /* INIT-assert / SIPI: shorthand 0 targeted delivery; the kernel's
         * bring-up never uses the broadcast forms for these modes. */
        if (shorthand != 0) {
            mlog(&m->log, "[lapic] IPI %s: vec=0x%02x shorthand=%d "
                 "(broadcast form not modeled for INIT/SIPI)", kind,
                 (unsigned)(v & 0xFF), shorthand);
            return;
        }
        int idx = lapic_index_by_id(m, dest);
        if (idx < 0) {
            mlog(&m->log, "[lapic] IPI %s: vec=0x%02x dst=%u "
                 "(no such LAPIC id -- nothing home)",
                 kind, (unsigned)(v & 0xFF), dest);
            return;
        }
        if (dm == 5) vcpu_take_init(m, idx);
        else         vcpu_take_sipi(m, idx, (uint8_t)(v & 0xFF));
        return;                                  /* delivery completes now */
    }
    int vec = (int)(v & 0xFF);
    if (vec < 16) { l->esr |= (1u << 6); return; }  /* receive-illegal */
    /* K6: fixed IPIs honor the broadcast shorthands per-context.
     * shorthand: 0 targeted, 1 self, 2 all, 3 all-excluding-self. */
    if (shorthand == 2 || shorthand == 3) {
        for (int i = 0; i < m->n_vcpus && i < EM_MAX_VCPU; i++)
            if (shorthand == 2 || i != m->cur_vcpu)
                map_set(vcpu_lapic(m, i)->irr, vec);
        return;
    }
    if (shorthand == 0) {
        int idx = lapic_index_by_id(m, (l->icr_hi >> 24) & 0xFF);
        if (idx < 0 && ((l->icr_hi >> 24) & 0xFF) == 0xFF)
            idx = m->cur_vcpu;                   /* broadcast-dest reg: us */
        if (idx < 0) return;                     /* nobody home */
        map_set(vcpu_lapic(m, idx)->irr, vec);
        return;
    }
    map_set(l->irr, vec);                        /* shorthand 1: self */
}

/* --------------------------------------------------- MMIO handlers */

static uint64_t lapic_read(void *ctx, uint64_t addr, int size) {
    machine_t *m = ctx;
    lapic_t *l = &m->lapic;
    uint32_t v = 0;
    switch ((uint32_t)(addr - LAPIC_BASE) & ~0xFu) {   /* 16B register slots */
    case LAPIC_ID:   v = l->id; break;
    case LAPIC_VER:  v = 0x00050014u; break;  /* version 0x14, MaxLVT=5 */
    case LAPIC_TPR:  v = l->tpr; break;
    case LAPIC_APR:  v = 0; break;            /* single vCPU: no arbitration */
    case LAPIC_PPR:  v = lapic_ppr(l); break;
    case LAPIC_LDR:  v = l->ldr; break;
    case LAPIC_DFR:  v = l->dfr; break;
    case LAPIC_SVR:  v = l->svr; break;
    case LAPIC_IRR_BASE: case LAPIC_IRR_BASE+0x10: case LAPIC_IRR_BASE+0x20:
    case LAPIC_IRR_BASE+0x30: case LAPIC_IRR_BASE+0x40: case LAPIC_IRR_BASE+0x50:
    case LAPIC_IRR_BASE+0x60: case LAPIC_IRR_BASE+0x70: {
        uint32_t off = (uint32_t)(addr - LAPIC_BASE) & ~0xFu;
        int base = (int)(off - LAPIC_IRR_BASE) / 4;    /* 4 map bytes per reg */
        for (int i = 0; i < 4; i++) v |= (uint32_t)l->irr[base + i] << (8 * i);
        break; }
    case 0x180: case 0x190: case 0x1A0: case 0x1B0:
    case 0x1C0: case 0x1D0: case 0x1E0: case 0x1F0: {
        uint32_t off = (uint32_t)(addr - LAPIC_BASE) & ~0xFu;
        int base = (int)(off - 0x180) / 4;
        for (int i = 0; i < 4; i++) v |= (uint32_t)l->isr[base + i] << (8 * i);
        break; }
    case 0x200: case 0x210: case 0x220: case 0x230:
    case 0x240: case 0x250: case 0x260: case 0x270:
        v = 0; break;                        /* TMR: all edge */
    case LAPIC_ESR: v = l->esr; break;
    case LAPIC_ICR_LO: v = 0; break;         /* delivery-complete, always */
    case LAPIC_ICR_HI: v = l->icr_hi; break;
    case LAPIC_LVT_TMR: v = l->lvt[0]; break;
    case LAPIC_LVT_THM: v = l->lvt[1]; break;
    case LAPIC_LVT_PRF: v = l->lvt[2]; break;
    case LAPIC_LVT_LI0: v = l->lvt[3]; break;
    case LAPIC_LVT_LI1: v = l->lvt[4]; break;
    case LAPIC_LVT_ERR: v = l->lvt[5]; break;
    case LAPIC_TMICT:  v = l->tmict; break;
    case LAPIC_TMCCUR: v = l->tmccur; break;
    case LAPIC_DCR:    v = l->dcr; break;
    default: v = 0; break;                   /* unimplemented slot: reads 0 */
    }
    return v & (size == 1 ? 0xFFu : size == 2 ? 0xFFFFu : 0xFFFFFFFFu);
}

static void lapic_write(void *ctx, uint64_t addr, int size, uint64_t val) {
    machine_t *m = ctx;
    lapic_t *l = &m->lapic;
    uint32_t v = (uint32_t)val & (size == 1 ? 0xFFu : size == 2 ? 0xFFFFu : 0xFFFFFFFFu);
    switch ((uint32_t)(addr - LAPIC_BASE) & ~0xFu) {
    case LAPIC_ID:   l->id = v & 0xFF000000u; break;
    case LAPIC_TPR:  l->tpr = v & 0xFFu; break;
    case LAPIC_EOI:  lapic_eoi(m); break;
    case LAPIC_LDR:  l->ldr = v; break;
    case LAPIC_DFR:  l->dfr = v | 0x0FFFFFFFu; break;  /* model field stuck */
    case LAPIC_SVR:
        l->svr = v & 0x1FFu;
        mlog(&m->log, "[lapic] software %s (spurious vector 0x%02x)",
             (l->svr & 0x100) ? "ENABLED" : "disabled", l->svr & 0xFF);
        break;
    case LAPIC_ICR_HI: l->icr_hi = v & 0xFF000000u; break;
    case LAPIC_ICR_LO: lapic_icr_write(m, v); break;   /* write arms + sends */
    case LAPIC_LVT_TMR: l->lvt[0] = v & (LVT_TMR_PERIODIC | LVT_MASKED | 0xFFu); break;
    case LAPIC_LVT_THM: l->lvt[1] = v & (LVT_MASKED | 0x1FFFu); break;
    case LAPIC_LVT_PRF: l->lvt[2] = v & (LVT_MASKED | 0x1FFFu); break;
    case LAPIC_LVT_LI0: l->lvt[3] = v & (LVT_MASKED | 0x1FFFu); break;
    case LAPIC_LVT_LI1: l->lvt[4] = v & (LVT_MASKED | 0x1FFFu); break;
    case LAPIC_LVT_ERR: l->lvt[5] = v & (LVT_MASKED | 0x1FFFu); break;
    case LAPIC_ESR:   l->esr = 0; break;               /* clear-on-write-any */
    case LAPIC_TMICT:
        l->tmict = v;
        l->tmccur = v;                                 /* arm: copies down */
        l->accum = 0;
        break;
    case LAPIC_DCR: l->dcr = v & 0xB; l->accum = 0; break;
    default: break;                                     /* RI/O as documented */
    }
}

/* --------------------------------------------------------- init */

void lapic_init(machine_t *m) {
    memset(&m->lapic, 0, sizeof m->lapic);
    m->lapic.dfr = 0xFFFFFFFFu;
    m->lapic.svr = 0x00FFu;                     /* spurious 0xFF, DISABLED */
    for (int i = 0; i < 6; i++) m->lapic.lvt[i] = LVT_MASKED;
}

void lapic_mmio_register(machine_t *m) {
    mem_register_mmio(m, LAPIC_BASE, 0x1000, lapic_read, lapic_write, m, "LAPIC");
}
