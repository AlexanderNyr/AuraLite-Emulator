/* pm.c -- ACPI PM register block (COMPAT G5).  Contract in pm.h. */
#include <string.h>
#include "machine.h"
#include "pit.h"
#include "pm.h"

#define PM_BLOCK_LEN   0x40      /* PMBASE..PMBASE+0x3f                    */
#define PM_TMR_OFF     0x08      /* PM1a_EVT_BLK (4) + PM1a_CNT_BLK (2) .. */
#define PM_TMR_MASK    0x00FFFFFFu
#define PM_TMR_RATIO   3u        /* 3.579545 MHz / 1.193182 MHz, exactly 3 */

/* PM P1: PM1 register bits (ICH9 PMBASE layout: STS +0x00, EN +0x02,
 * CNT +0x04, TMR +0x08; GPE0 arrives in P2). */
#define PM1_TMR_STS    0x0001u
#define PM1_STS_W1C    0x8501u   /* TMR(0) PWRBTN(8) RTC(10) WAK(15) */
#define PM1_CNT_SCI_EN 0x0001u
#define PM1_CNT_BM_RLD 0x0002u
#define PM1_SLP_EN     0x2000u
#define PM1_SLP_TYP(v) (((v) >> 10) & 7u)
#define PM_SLP_TYP_S5  7u        /* Q35/PIIX4 encoding; P2's DSDT matches */
#define PM_SLP_TYP_S1  1u        /* published as \_S1 by the P3 DSDT      */
#define PM1_WAK_STS    0x8000u

/* SCI is a level like the rest of the fleet: asserted while SCI_EN and
 * any enabled event latches (PM1 now, GPE0 joins in P2). */
static void pm_sci_update(machine_t *m) {
    pm_t *p = &m->pm;
    int lvl = ((p->cnt & PM1_CNT_SCI_EN) &&
               ((p->sts & p->en) || (p->gpe_sts & p->gpe_en))) ? 1 : 0;
    if (lvl != p->sci_level) {
        p->sci_level = lvl;
        pic_set_irq(m, PM_SCI_IRQ, lvl);
    }
}

/* Advance the 24-bit counter to "now".  Called on every read, so the value
 * a guest sees never depends on how often the block was polled; the whole
 * cadence is derived from vtime_instr, like pit_tick(). */
static void pm_advance(machine_t *m) {
    pm_t *p = &m->pm;
    uint64_t ipt = m->pit.instr_per_tick ? m->pit.instr_per_tick
                                         : PIT_DEFAULT_IPT;
    uint64_t now = m->vtime_instr;
    uint64_t delta = now - p->last_instr;
    p->last_instr = now;
    if (!delta) return;
    p->accum += delta * PM_TMR_RATIO;
    uint64_t ticks = p->accum / ipt;
    p->accum %= ipt;
    if (ticks) {
        /* PM P1: the 24-bit wrap is the TMR_STS event, as on silicon
         * (one overflow per ~4.7 s of virtual time). */
        if ((uint64_t)p->count + ticks > PM_TMR_MASK) p->sts |= PM1_TMR_STS;
        p->count = (uint32_t)((p->count + ticks) & PM_TMR_MASK);
        pm_sci_update(m);
    }
}

void pm_tick(machine_t *m) {
    pm_advance(m);
    pm_sci_update(m);
}

uint32_t pm_timer_now(machine_t *m) {
    pm_advance(m);
    return m->pm.count & PM_TMR_MASK;
}

/* Does [off, off+size) touch the Timer register's three live bytes? */
static int pm_hits_tmr(uint16_t off, int size) {
    return off < PM_TMR_OFF + 3 && (uint32_t)off + (uint32_t)size > PM_TMR_OFF;
}

static uint32_t pm_read_byte_stream(machine_t *m, uint16_t off, int size) {
    /* Assemble each modelled register byte-wise: PM1_STS/EN/CNT (P1) and
     * the Timer's three live bytes (bits 31:24 reserved-zero).  SLP_EN is
     * write-only and reads back 0 because pm.cnt never stores it. */
    uint32_t tmr = pm_hits_tmr(off, size) ? pm_timer_now(m) : 0;
    uint16_t regs[3] = { m->pm.sts, m->pm.en, m->pm.cnt };
    uint32_t v = 0;
    for (int i = 0; i < size; i++) {
        unsigned a = off + (unsigned)i;
        uint8_t b = 0;
        if (a < 6)
            b = (uint8_t)(regs[a / 2] >> (8 * (a & 1u)));
        else if (a >= PM_TMR_OFF && a < PM_TMR_OFF + 3)
            b = (uint8_t)(tmr >> (8 * (a - PM_TMR_OFF)));
        else if (a >= 0x20 && a < 0x24)   /* GPE0_STS (PM P2) */
            b = (uint8_t)(m->pm.gpe_sts >> (8 * (a - 0x20)));
        else if (a >= 0x24 && a < 0x28)   /* GPE0_EN */
            b = (uint8_t)(m->pm.gpe_en >> (8 * (a - 0x24)));
        v |= (uint32_t)b << (8 * i);
    }
    return v;
}

static uint32_t pm_io_read(void *ctx, uint16_t port, int size) {
    machine_t *m = ctx;
    uint16_t off = (uint16_t)(port - m->pm.base);
    /* PM1 + Timer have state; the rest of the block still reads 0.
     * A read that straddles the Timer keeps its live bytes (see above). */
    if (port >= m->pm.base && off < PM_BLOCK_LEN) {
        if (pm_hits_tmr(off, size)) m->pm.reads++;
        return pm_read_byte_stream(m, off, size);
    }
    return size == 1 ? 0xFFu : size == 2 ? 0xFFFFu : 0xFFFFFFFFu;
}

/* PM P1/P3: a sleep request.  S5 is the machine's deterministic stop
 * (the same endpoint the debugger uses).  S1 is zero-duration by
 * measured decision (PM_PLAN 2c: no consumer needs real parking, and
 * an already-pending wake event is spec-legal): WAK_STS latches, the
 * run continues, and the guest's resume path does what it must do on
 * any fast-waking machine.  Everything else is logged, not faked. */
static void pm_sleep(machine_t *m, unsigned typ) {
    if (typ == PM_SLP_TYP_S5) {
        mlog(&m->log, "[pm] S5 power-off via PM1_CNT -- stopping the run");
        m->stop_requested = 1;
    } else if (typ == PM_SLP_TYP_S1) {
        m->pm.sts |= PM1_WAK_STS;
        mlog(&m->log, "[pm] S1 suspend: zero-duration (WAK_STS latched)");
    } else {
        mlog(&m->log, "[pm] sleep type %u requested -- not implemented", typ);
    }
}

static void pm_io_write(void *ctx, uint16_t port, int size, uint32_t val) {
    machine_t *m = ctx;
    uint16_t off = (uint16_t)(port - m->pm.base);
    if (port < m->pm.base || off >= PM_BLOCK_LEN) return;
    /* Byte-wise, so the word/byte writes real drivers use compose the
     * same way the read stream assembles them.  The Timer is read-only. */
    for (int i = 0; i < size; i++) {
        unsigned a = off + (unsigned)i;
        uint32_t b = (val >> (8 * i)) & 0xFFu;
        if (a < 2) {                    /* PM1_STS: W1C on the RW1C bits */
            m->pm.sts &= (uint16_t)~((b << (8 * a)) & PM1_STS_W1C);
        } else if (a < 4) {             /* PM1_EN: plain storage */
            unsigned sh = 8 * (a - 2);
            m->pm.en = (uint16_t)((m->pm.en & ~(0xFFu << sh)) | (b << sh));
        } else if (a == 4) {            /* PM1_CNT low: SCI_EN, BM_RLD */
            m->pm.cnt = (uint16_t)((m->pm.cnt & 0xFF00u) |
                                   (b & (PM1_CNT_SCI_EN | PM1_CNT_BM_RLD)));
        } else if (a == 5) {            /* PM1_CNT high: SLP_TYP/SLP_EN */
            uint32_t hi = b << 8;
            if (hi & PM1_SLP_EN) pm_sleep(m, PM1_SLP_TYP(hi));
        } else if (a >= 0x20 && a < 0x24) {   /* GPE0_STS: W1C (PM P2) */
            m->pm.gpe_sts &= ~(b << (8 * (a - 0x20)));
        } else if (a >= 0x24 && a < 0x28) {   /* GPE0_EN: stored */
            unsigned sh = 8 * (a - 0x24);
            m->pm.gpe_en = (m->pm.gpe_en & ~(0xFFu << sh)) | (b << sh);
        }
    }
    pm_sci_update(m);
}

static void pm_register_block(machine_t *m, uint16_t base) {
    io_register(m, base, PM_BLOCK_LEN, pm_io_read, pm_io_write, m,
                "ACPI PM block");
}

static void pm_unregister_block(machine_t *m, uint16_t base) {
    io_register(m, base, PM_BLOCK_LEN, NULL, NULL, NULL, NULL);
}

void pm_set_base(machine_t *m, uint16_t base) {
    base = (uint16_t)(base & 0xFF80u);          /* PMBASE bits 15:7 */
    if (base == m->pm.base) return;
    pm_unregister_block(m, m->pm.base);
    m->pm.base = base;
    pm_register_block(m, base);
}

void pm_init(machine_t *m) {
    uint16_t old = m->pm.base;
    if (old) pm_unregister_block(m, old);
    memset(&m->pm, 0, sizeof m->pm);
    m->pm.base = 0xB000;                        /* Q35 / SeaBIOS default */
    m->pm.last_instr = m->vtime_instr;
    pm_register_block(m, m->pm.base);
}
