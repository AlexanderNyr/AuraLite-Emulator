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

/* ------------------------------------ IA32_APIC_BASE (UPKEEP U4) --- */

/* The MSR lives in each cpu_t's sparse store, seeded per-role by
 * cpu_reset. An absent entry (a bare fixture that never reset its cpu)
 * reads as the power-on default. */
static uint64_t apicbase_of(cpu_t *c) {
    int found;
    uint64_t v = cpu_get_msr(c, LAPIC_MSR_APICBASE, &found);
    return found ? v : LAPIC_MSR_DEFAULT;
}
/* cpu of context i (machine.h has the lapic twin, vcpu_lapic). */
static cpu_t *ctx_cpu(machine_t *m, int i) {
    if (m->n_vcpus <= 1 || i == m->cur_vcpu) return &m->cpu;
    return &m->vcpu[i].c;
}
static int apic_en(machine_t *m, int i) {
    return (apicbase_of(ctx_cpu(m, i)) & LAPIC_MSR_EN) != 0;
}
static uint64_t window_base(machine_t *m) {
    return apicbase_of(&m->cpu) & LAPIC_MSR_BASE_MASK;
}

void lapic_sync_mmio(machine_t *m) {
    for (mmio_region_t *r = m->mmio_list; r; r = r->next)
        if (r->name && strcmp(r->name, "LAPIC") == 0) {
            r->base = window_base(m);
            r->enabled = apic_en(m, m->cur_vcpu);
            return;
        }
}

/* LAPIC twin of cpu `c` anywhere in the machine (live pair or a parked
 * slot); NULL for a cpu foreign to this machine. */
static lapic_t *lapic_of_cpu(machine_t *m, cpu_t *c) {
    if (c == &m->cpu) return &m->lapic;
    for (int i = 0; i < EM_MAX_VCPU; i++)
        if (c == &m->vcpu[i].c) return &m->vcpu[i].l;
    return NULL;
}

int lapic_apicbase_write(machine_t *m, cpu_t *c, uint64_t val) {
    uint64_t oldv = apicbase_of(c);
    if (val & LAPIC_MSR_EXTD) {
        mlog(&m->log, "[lapic] IA32_APIC_BASE write 0x%llx rejected: "
             "x2APIC (bit 10) is not implemented (CPUID.1:ECX[21]=0)",
             (unsigned long long)val);
        return 0;
    }
    if (val & ~(LAPIC_MSR_BASE_MASK | LAPIC_MSR_BSP | LAPIC_MSR_EN)) {
        mlog(&m->log, "[lapic] IA32_APIC_BASE write 0x%llx rejected: "
             "reserved bits set (SDM Fig.10-5: bits 0-7,9 and "
             "MAXPHYADDR=40..63 are reserved; MSR unchanged)",
             (unsigned long long)val);
        return 0;
    }
    /* BSP is a machine strap, not software-writable (QEMU apic_set_base
     * preserves it; SDM 10.4.4 assigns it at reset). */
    uint64_t nv = (val & LAPIC_MSR_BASE_MASK) | (val & LAPIC_MSR_EN)
                | (oldv & LAPIC_MSR_BSP);
    cpu_set_msr(c, LAPIC_MSR_APICBASE, nv);
    if (!(nv & LAPIC_MSR_EN)) {
        lapic_t *l = lapic_of_cpu(m, c);
        if (l) l->svr &= ~0x100u;   /* the disabling write also drops the
                                     * SVR software enable (QEMU); the rest
                                     * of the register file is kept */
    }
    if (c == &m->cpu) lapic_sync_mmio(m);
    if (nv != oldv)
        mlog(&m->log, "[lapic] IA32_APIC_BASE <- 0x%llx (base 0x%llx, %s%s)",
             (unsigned long long)nv,
             (unsigned long long)(nv & LAPIC_MSR_BASE_MASK),
             (nv & LAPIC_MSR_EN) ? "EN" : "globally disabled",
             (nv & LAPIC_MSR_BSP) ? ", BSP" : "");
    return 1;
}

/* --------------------------------------------------- arbitration */

static uint32_t lapic_ppr(const lapic_t *l) {
    int isrv = map_highest(l->isr);
    if (isrv >= 0 && (isrv >> 4) > (int)(l->tpr >> 4))
        return (uint32_t)isrv & 0xF0u;   /* in-service class wins, sub = 0 */
    return l->tpr;
}

int lapic_deliverable(machine_t *m) {
    lapic_t *l = &m->lapic;
    if (!apic_en(m, m->cur_vcpu)) return -1;         /* U4: globally off */
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
    if (!apic_en(m, m->cur_vcpu)) return;   /* U4: no APIC to latch into */
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
    /* U4: a globally disabled APIC has no timer. The clock snapshot
     * above keeps advancing so the disabled interval does not dump into
     * the counter when the APIC is re-enabled; the count just freezes. */
    if (!apic_en(m, m->cur_vcpu)) return;
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
    if (!apic_en(m, idx)) return;   /* U4: a disabled target drops the message */
    map_set(vcpu_lapic(m, idx)->irr, vector);
}

/* MSI M2: one PCI MSI/MSI-X message, decoded per the x86 message format
 * (addr = 0xFEE00000 | dest[19:12] | RH[3] | DM[2]; data = trigger[15] |
 * level[14] | delivery[10:8] | vector[7:0]) and landed on the same sink
 * the IOAPIC uses.  Delivery is edge-shaped by construction: the source
 * keeps its own pending state (MSI_PLAN 2), so trigger/level are
 * advisory.  Fixed lands as addressed.  Lowest Priority has no arbiter
 * to consult on this machine and degenerates to Fixed at the named
 * destination.  SMI/NMI/INIT/ExtINT and reserved modes are logged and
 * dropped -- never fabricated into injections this core cannot honour.
 * RH (redirection hint) only steers Lowest Priority arbitration, so it
 * is decoded and set aside with it. */
void msi_deliver(machine_t *m, uint64_t addr, uint32_t data) {
    if ((addr & 0xFFF00000ull) != 0xFEE00000ull) return;
    uint32_t dest = (uint32_t)((addr >> 12) & 0xFFu);
    int logical = (int)((addr >> 2) & 1u);
    uint32_t vector = data & 0xFFu;
    uint32_t dmode = (data >> 8) & 7u;
    if (dmode != 0 && dmode != 1) {
        mlog(&m->log, "[msi] delivery mode %u dropped (vec %u dest %u)",
             dmode, vector, dest);
        return;
    }
    if (!logical) {
        lapic_set_irr_dest(m, dest, (int)vector);
        return;
    }
    /* logical (flat): dest is a bitmask over LDR[31:24]; first claimant
     * wins, nobody-claims-it falls back to the BSP like the IOAPIC. */
    for (int i = 0; i < m->n_vcpus && i < EM_MAX_VCPU; i++) {
        lapic_t *li = vcpu_lapic(m, i);
        if (dest & (li->ldr >> 24)) {
            if (vector < 16) { m->lapic.esr |= (1u << 6); return; }
            if (apic_en(m, i)) map_set(li->irr, (int)vector);  /* U4 gate */
            return;
        }
    }
    lapic_set_irr_dest(m, 0, (int)vector);
}

/* U5: the LAPIC register file after an INIT (SDM 10.4.7.3: "the same
 * as it is after a power-up or hardware reset, except that the APIC
 * ID ... [is] not affected"). IA32_APIC_BASE rides inside the CPU's
 * MSR store, preserved by cpu_init_reset. */
static void lapic_state_after_init(lapic_t *l) {
    uint32_t id = l->id;
    memset(l, 0, sizeof *l);
    l->id = id;
    l->dfr = 0xFFFFFFFFu;
    l->svr = 0x00FFu;
    for (int i = 0; i < 6; i++) l->lvt[i] = LVT_MASKED;
}

/* K6: an INIT IPI arriving at context idx (Intel MP spec s.8.4: INIT
 * resets most architectural state and parks the cpu in wait-for-SIPI;
 * the LAPIC keeps its ID).  INIT asserted at the RUNNING context is
 * the "INIT self" case -- illegal on real silicon, we log-and-refuse.
 * CPU state goes through the one INIT definition, cpu_init_reset
 * (SDM Table 9-1; U5 unified it with the chipset INIT# below). */
static void vcpu_take_init(machine_t *m, int idx) {
    vcpu_slot_t *s = &m->vcpu[idx];
    if (idx == m->cur_vcpu) {
        mlog(&m->log, "[lapic] INIT at the RUNNING vcpu%d ignored (self)", idx);
        return;
    }
    cpu_init_reset(&s->c);
    lapic_state_after_init(&s->l);
    s->state = VCPU_WAIT_SIPI;
    mlog(&m->log, "[lapic] vcpu%d took INIT: state reset, parked in "
         "wait-for-SIPI", idx);
}

/* U5: the chipset INIT# (CF9 RST_CPU with SYS_RST clear). Unlike an
 * INIT IPI it is platform-wide and necessarily includes the RUNNING
 * context (the pin is asserted at every processor, writer included):
 * every context takes the INIT state, the BSP (context 0) stays
 * runnable at the reset vector, and every AP parks in wait-for-SIPI
 * (SDM 10.4.7.3 -- the BSP does not wait for a SIPI, APs do; QEMU
 * apic_init_reset makes the same split). Devices never see it. */
void lapic_broadcast_init(machine_t *m) {
    int n = m->n_vcpus > 1 ? m->n_vcpus : 1;
    for (int i = 0; i < n && i < EM_MAX_VCPU; i++) {
        if (i == m->cur_vcpu || n == 1) {
            cpu_init_reset(&m->cpu);
            lapic_state_after_init(&m->lapic);
            if (i != 0) {
                /* the writing AP parks itself; halted so the scheduler
                 * will not run its reset-vector state before a SIPI
                 * (vcpu_take_sipi clears the flag again). */
                m->vcpu[i].state = VCPU_WAIT_SIPI;
                m->cpu.halted = 1;
            }
        } else {
            vcpu_slot_t *s = &m->vcpu[i];
            cpu_init_reset(&s->c);
            lapic_state_after_init(&s->l);
            s->state = (i == 0) ? VCPU_RUN : VCPU_WAIT_SIPI;
        }
    }
    lapic_sync_mmio(m);   /* base/EN may ride the preserved MSR value */
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
        /* INIT-assert / SIPI: shorthand 0 is targeted delivery (kernel smp_init);
         * shorthand 2/3 are broadcast forms (SeaBIOS smp_setup uses shorthand 3,
         * all-excluding-self, during POST).  When n_vcpus <= 1, keep the
         * historical UP log line unchanged. */
        if (shorthand != 0) {
            if (m->n_vcpus <= 1 || (shorthand != 2 && shorthand != 3)) {
                mlog(&m->log, "[lapic] IPI %s: vec=0x%02x shorthand=%d "
                     "(broadcast form not modeled for INIT/SIPI)", kind,
                     (unsigned)(v & 0xFF), shorthand);
                return;
            }
            for (int i = 0; i < m->n_vcpus && i < EM_MAX_VCPU; i++) {
                if (shorthand == 3 && i == m->cur_vcpu) continue;
                if (dm == 5) vcpu_take_init(m, i);
                else         vcpu_take_sipi(m, i, (uint8_t)(v & 0xFF));
            }
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
    /* U4: fixed IPIs to a globally disabled target are dropped at the
     * latch (INIT/SIPI above are NOT gated: SDM 10.4.7.2 still answers
     * them in the software-disabled state, and INIT is the recovery path). */
    if (shorthand == 2 || shorthand == 3) {
        for (int i = 0; i < m->n_vcpus && i < EM_MAX_VCPU; i++)
            if ((shorthand == 2 || i != m->cur_vcpu) && apic_en(m, i))
                map_set(vcpu_lapic(m, i)->irr, vec);
        return;
    }
    if (shorthand == 0) {
        int idx = lapic_index_by_id(m, (l->icr_hi >> 24) & 0xFF);
        if (idx < 0 && ((l->icr_hi >> 24) & 0xFF) == 0xFF)
            idx = m->cur_vcpu;                   /* broadcast-dest reg: us */
        if (idx < 0 || !apic_en(m, idx)) return;  /* nobody home / U4 gate */
        map_set(vcpu_lapic(m, idx)->irr, vec);
        return;
    }
    /* shorthand 1 (self): the ICR write reached us, so our own window
     * is mapped and EN is 1 by construction -- no gate needed. */
    map_set(l->irr, vec);                        /* shorthand 1: self */
}

/* --------------------------------------------------- MMIO handlers */

static uint64_t lapic_read(void *ctx, uint64_t addr, int size) {
    machine_t *m = ctx;
    lapic_t *l = &m->lapic;
    uint64_t base = window_base(m);   /* U4: the window moves with the MSR */
    uint32_t v = 0;
    switch ((uint32_t)(addr - base) & ~0xFu) {   /* 16B register slots */
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
        uint32_t off = (uint32_t)(addr - base) & ~0xFu;
        int base = (int)(off - LAPIC_IRR_BASE) / 4;    /* 4 map bytes per reg */
        for (int i = 0; i < 4; i++) v |= (uint32_t)l->irr[base + i] << (8 * i);
        break; }
    case 0x180: case 0x190: case 0x1A0: case 0x1B0:
    case 0x1C0: case 0x1D0: case 0x1E0: case 0x1F0: {
        uint32_t off = (uint32_t)(addr - base) & ~0xFu;
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
    switch ((uint32_t)(addr - window_base(m)) & ~0xFu) {
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
    lapic_sync_mmio(m);   /* U4: adopt the current context's MSR decode */
}
