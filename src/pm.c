/* pm.c -- ACPI PM register block (COMPAT G5).  Contract in pm.h. */
#include <string.h>
#include "machine.h"
#include "pit.h"
#include "pm.h"

#define PM_BLOCK_LEN   0x40      /* PMBASE..PMBASE+0x3f                    */
#define PM_TMR_OFF     0x08      /* PM1a_EVT_BLK (4) + PM1a_CNT_BLK (2) .. */
#define PM_TMR_MASK    0x00FFFFFFu
#define PM_TMR_RATIO   3u        /* 3.579545 MHz / 1.193182 MHz, exactly 3 */

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
    if (ticks) p->count = (uint32_t)((p->count + ticks) & PM_TMR_MASK);
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
    /* Assemble the Timer register from its three live bytes.  Bits 31:24
     * are reserved and read as zero (ICH9: TMR is a 24-bit counter). */
    uint32_t tmr = pm_hits_tmr(off, size) ? pm_timer_now(m) : 0;
    uint32_t v = 0;
    for (int i = 0; i < size; i++) {
        unsigned a = off + (unsigned)i;
        uint8_t b = 0;
        if (a >= PM_TMR_OFF && a < PM_TMR_OFF + 3)
            b = (uint8_t)(tmr >> (8 * (a - PM_TMR_OFF)));
        v |= (uint32_t)b << (8 * i);
    }
    return v;
}

static uint32_t pm_io_read(void *ctx, uint16_t port, int size) {
    machine_t *m = ctx;
    uint16_t off = (uint16_t)(port - m->pm.base);
    /* Only the Timer register has state; the rest of the block reads 0.
     * A read that straddles the Timer keeps its live bytes (see above). */
    if (port >= m->pm.base && off < PM_BLOCK_LEN) {
        if (pm_hits_tmr(off, size)) m->pm.reads++;
        return pm_read_byte_stream(m, off, size);
    }
    return size == 1 ? 0xFFu : size == 2 ? 0xFFFFu : 0xFFFFFFFFu;
}

static void pm_io_write(void *ctx, uint16_t port, int size, uint32_t val) {
    machine_t *m = ctx;
    (void)size; (void)val;
    uint16_t off = (uint16_t)(port - m->pm.base);
    if (port < m->pm.base || off >= PM_BLOCK_LEN) return;
    /* The Timer is read-only; PM1_STS would be W1C and PM1_CNT a sleep
     * transition, neither of which this behavioural model implements.
     * Swallowing the write is the honest option: nothing is fabricated. */
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
