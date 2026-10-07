/* pit.c -- Intel 8254 PIT (CHIPSET H2). Contract in pit.h; mode-change
 * semantics follow the Intel 8254 datasheet: a control-word write forces
 * OUT per the new mode immediately and leaves the counting element alone;
 * counting (re)starts on the last byte of a count-register write. */
#include <string.h>
#include "machine.h"
#include "pit.h"
#include "pic.h"

void pit_init(machine_t *m) {
    memset(&m->pit, 0, sizeof m->pit);
    m->pit.instr_per_tick = PIT_DEFAULT_IPT;
    m->pit.last_instr = m->vtime_instr; /* K6 master clock */
}

/* Effective mode for behavior: 6->2, 7->3 (clone-compatible); 1/4/5 run
 * mode-0-like but never raise IRQ (see pit.h). */
static int meff(const pit_counter_t *c) {
    switch (c->mode) {
    case 6: return 2;
    case 7: return 3;
    default: return c->mode;
    }
}

static int64_t period_of(const pit_counter_t *c) {
    return c->cr ? c->cr : 65536;
}

/* One terminal event on counter 0: IRQ0 edge strobe (modes with IRQ
 * semantics only). Collapses to one latched IRR per strobe; the `fired`
 * counter keeps the full count for debug.
 * K2: the pin is wired to BOTH legacy sink (8259 IRQ0) and I/O APIC INTIN2
 * (GSI2) -- the standard MADT IRQ0->GSI2 override every PC carries, and
 * exactly how the AuraLite kernel routes "PIT@GSI2" once it masks the PIC
 * and switches to APIC delivery. Pulse both. */
static void fire_once(machine_t *m, pit_counter_t *c) {
    c->fired++;
    if (c == &m->pit.ch[0]) {
        pic_raise_irq(m, 0);
        ioapic_edge_gsi(m, 2);
    }
}

/* Advance one counter by `t` ticks. Mode 3 also refreshes its OUT level:
 * high for the first (P+1)/2 ticks of each period (Intel's odd-count
 * rule), low for the rest. */
static void advance(machine_t *m, pit_counter_t *c, uint64_t t) {
    if (!c->counting || !t) return;
    if (c == &m->pit.ch[2] && !(m->pit.port_b & 1)) return; /* GATE2 low */
    int64_t P = period_of(c);
    int me = meff(c);
    switch (me) {
    case 0: { /* interrupt on terminal count: one edge, then hold OUT high */
        if (c->out) { c->rem -= (int64_t)t; while (c->rem <= 0) c->rem += P; return; }
        if ((int64_t)t >= c->rem) {
            c->out = 1;
            if (c == &m->pit.ch[2] && m->dbg_pit1)
                mlog(&m->log, "[pit-dbg] ch2 OUT=1 rem=%lld t=%llu vtime=%llu",
                     (long long)c->rem, (unsigned long long)t,
                     (unsigned long long)m->vtime_instr);
            fire_once(m, c);
        }
        c->rem -= (int64_t)t;
        while (c->rem <= 0) c->rem += P;
        return; }
    case 2: case 3: { /* rate generator / square wave: strobe per period */
        int64_t rem = c->rem;
        if ((int64_t)t >= rem) {
            uint64_t cycles = 1 + ((uint64_t)(t - (uint64_t)rem) / (uint64_t)P);
            for (uint64_t k = 0; k < cycles; k++) fire_once(m, c);
            rem = rem - (int64_t)t + (int64_t)cycles * P;
        } else rem -= (int64_t)t;
        c->rem = rem;
        if (me == 3) { /* square: position within period */
            int64_t pos = P - rem;                 /* 0..P-1 since reload */
            c->out = (pos < (P + 1) / 2) ? 1 : 0;
        } else c->out = 1;
        return; }
    default: /* 1,4,5: stored, mode-0-like counting, no IRQ (documented) */
        c->rem -= (int64_t)t; while (c->rem <= 0) c->rem += P;
        if (c->rem <= 0) c->rem = P;
        return;
    }
}

void pit_tick(machine_t *m) {
    if (!m->pit.instr_per_tick) return;   /* pit not initialized */
    uint64_t now = m->vtime_instr;  /* K6: machine-wide master clock (== instr_count UP) */
    uint64_t delta = now - m->pit.last_instr;
    if (!delta) return;
    m->pit.last_instr = now;
    m->pit.accum += delta;
    if (m->pit.accum < m->pit.instr_per_tick) return;  /* fast path */
    uint64_t ticks = m->pit.accum / m->pit.instr_per_tick;
    m->pit.accum %= m->pit.instr_per_tick;
    for (int i = 0; i < 3; i++) advance(m, &m->pit.ch[i], ticks);
}

/* ---------------------------------------------------------------- ports */

static uint32_t pit_ch_read(machine_t *m, int ch) {
    pit_counter_t *c = &m->pit.ch[ch];
    if (c->rw != 3) {                       /* single-byte access */
        uint16_t v = c->latched ? c->latch : (uint16_t)c->rem;
        c->latched = 0;
        return c->rw == 2 ? (v >> 8) : (v & 0xFF);
    }
    if (c->rphase == 0) {                   /* LSB of the pair first */
        c->read_snap = c->latched ? c->latch : (uint16_t)c->rem;
        c->rphase = 1;
        return c->read_snap & 0xFF;
    }
    c->rphase = 0;
    c->latched = 0;                         /* pair complete: OL auto-unlatches */
    return c->read_snap >> 8;
}

static void pit_ch_write(machine_t *m, int ch, uint8_t v) {
    pit_counter_t *c = &m->pit.ch[ch];
    switch (c->rw) {
    case 1: c->cr = v; goto load;
    case 2: c->cr = (uint16_t)v << 8; goto load;
    case 3:
        if (!c->wphase) { c->cr = (c->cr & 0xFF00) | v; c->wphase = 1; return; }
        c->cr = (c->cr & 0x00FF) | ((uint16_t)v << 8); c->wphase = 0;
        goto load;
    default: return;                        /* CW not written yet: ignore */
    load:
        /* last count byte: load CE, start counting (datasheet: first
         * clock after the write; our boundary model anchors at the write) */
        c->rem = period_of(c);
        c->counting = 1;
        c->rphase = 0;
        if (meff(c) == 0) c->out = 0;       /* OUT low while counting */
        else c->out = 1;
        pit_tick(m);                        /* settle virtual time now */
        if (ch == 2 && m->dbg_pit1)
            mlog(&m->log, "[pit-dbg] ch2 load P=%lld vtime=%llu",
                 (long long)period_of(c), (unsigned long long)m->vtime_instr);
    }
}

static void pit_ctrl_write(machine_t *m, uint8_t v) {
    int sel = v >> 6;
    if (sel == 3) return;                   /* read-back command: H2 scope-out */
    pit_counter_t *c = &m->pit.ch[sel];
    int rw = (v >> 4) & 3;
    if (!rw) {                              /* counter latch command */
        pit_tick(m);
        c->latch = (uint16_t)c->rem;
        c->latched = 1;
        c->rphase = 0;
        return;
    }
    /* CW: new mode effective now; OUT forced per mode; CE and counting
     * state are left alone (Intel 8254 CW semantics). */
    c->rw = (uint8_t)rw;
    c->mode = (v >> 1) & 7;
    c->bcd = v & 1;
    c->wphase = 0; c->rphase = 0; c->latched = 0;
    int me = meff(c);
    c->out = (me == 0) ? 0 : 1;
}

static uint32_t pit_port_read(void *ctx, uint16_t port, int size) {
    machine_t *m = ctx;
    (void)size;
    pit_tick(m);
    switch (port) {
    case 0x40: return pit_ch_read(m, 0);
    case 0x41: return pit_ch_read(m, 1);
    case 0x42: return pit_ch_read(m, 2);
    case 0x43: return 0xFF;                 /* ctrl port read: undefined */
    case 0x61: return m->pit.port_b | ((m->pit.ch[2].out & 1) << 5);
    }
    return 0xFF;
}

static void pit_port_write(void *ctx, uint16_t port, int size, uint32_t val) {
    machine_t *m = ctx;
    (void)size;
    switch (port) {
    case 0x40: pit_ch_write(m, 0, (uint8_t)val); break;
    case 0x41: pit_ch_write(m, 1, (uint8_t)val); break;
    case 0x42: pit_ch_write(m, 2, (uint8_t)val); break;
    case 0x43: pit_ctrl_write(m, (uint8_t)val); break;
    case 0x61: m->pit.port_b = (uint8_t)(val & 3); break;
    }
}

void pit_io_register(machine_t *m) {
    io_register(m, 0x40, 4, pit_port_read, pit_port_write, m, "8254 PIT");
    io_register(m, 0x61, 1, pit_port_read, pit_port_write, m, "8254 port B");
}
