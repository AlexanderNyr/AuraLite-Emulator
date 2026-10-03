/* pic.c -- Intel 8259A pair (CHIPSET H0+H1). H0: init, mask, INTA vector
 * delivery, EOI, cascade. H1: rotation, FN/SMM in-service gating, spurious
 * vectors, poll command, level-triggered lines. See pic.h for the contract. */
#include <string.h>
#include "machine.h"
#include "pic.h"
#include "ioapic.h"     /* H7: ISA lines fan out to the IOAPIC too */

void pic_init(machine_t *m) {
    m->pic.master = (pic_chip_t){ .imr = 0xFF, .vector_base = 0x08, .prio_low = 7 };
    m->pic.slave  = (pic_chip_t){ .imr = 0xFF, .vector_base = 0x70, .prio_low = 7 };
}

/* -------------------------------------------------- priority machinery */

/* Priority of a line, 0 (highest) .. 7 (lowest). prio_low names the line
 * with the lowest priority; priority order runs circularly from
 * prio_low+1 (highest) to prio_low (lowest). Reset prio_low=7 gives the
 * classic 0 > 1 > ... > 7 order. */
static int prio_of(const pic_chip_t *c, int line) {
    return (line - c->prio_low - 1) & 7;
}

/* Highest-priority line in a bitmask per the current rotation, or -1. */
static int best_line(const pic_chip_t *c, uint8_t bits) {
    for (int i = 0; i < 8; i++) {
        int line = (c->prio_low + 1 + i) & 7;
        if (bits & (1u << line)) return line;
    }
    return -1;
}

/* In-service gating: fully-nested mode blocks any candidate that does not
 * strictly outrank the highest-priority in-service line; special mask mode
 * blocks only a candidate whose own line is already in service. */
static int gating_allows(const pic_chip_t *c, int cand) {
    if (c->smm) return !(c->isr & (1u << cand));
    int insvc = best_line(c, c->isr);
    if (insvc < 0) return 1;
    return prio_of(c, cand) < prio_of(c, insvc);
}

/* Best deliverable line on one chip, or -1 (pending AND allowed).
 * Scans in priority order past gated candidates: in FN mode the first
 * gated candidate already implies all lower ones are gated, but in SMM
 * only same-line candidates are gated, so lower ones may deliver. */
static int chip_next(pic_chip_t *c) {
    uint8_t pend = (uint8_t)(c->irr & ~c->imr);
    for (int i = 0; i < 8; i++) {
        int line = (c->prio_low + 1 + i) & 7;
        if ((pend & (1u << line)) && gating_allows(c, line)) return line;
    }
    return -1;
}

/* Level-triggered re-assertion / de-assertion: in LTIM the IRR tracks the
 * pin levels (a line still high re-arms after claim/EOI). */
static void chip_sync_level(pic_chip_t *c) {
    if (c->ltim) c->irr |= c->lines;
}

/* Claim `line` on `c` (INTA or poll): IRR bit cleared, ISR set unless AEOI;
 * rotate-in-AEOI mode makes the just-serviced line lowest priority. */
static void claim_line(pic_chip_t *c, int line) {
    c->irr &= (uint8_t)~(1u << line);
    if (c->auto_eoi) { if (c->rotate_aeoi) c->prio_low = (uint8_t)line; }
    else             c->isr |= (uint8_t)(1u << line);
}

/* ---------------------------------------------------------------- lines */

void pic_raise_irq(machine_t *m, int irq) {
    if (irq < 0 || irq > 15) return;
    /* CHIPSET H7: the board wire also reaches the IOAPIC (same strobe). */
    ioapic_edge_gsi(m, irq);
    pic_chip_t *c = irq < 8 ? &m->pic.master : &m->pic.slave;
    c->irr |= (uint8_t)(1u << (irq & 7));       /* zero-width strobe: latch */
    if (irq >= 8 && (m->pic.slave.irr & ~m->pic.slave.imr))
        m->pic.master.irr |= 0x04;                        /* cascade edge */
}

void pic_set_irq(machine_t *m, int irq, int level) {
    if (irq < 0 || irq > 15) return;
    /* CHIPSET H7: the board wire also reaches the IOAPIC (same voltage). */
    ioapic_set_gsi(m, irq, level);
    pic_chip_t *c = irq < 8 ? &m->pic.master : &m->pic.slave;
    uint8_t bit = (uint8_t)(1u << (irq & 7));
    if (level) {
        if (!(c->lines & bit)) c->irr |= bit;    /* rising edge latches */
        c->lines |= bit;
        chip_sync_level(c);
    } else {
        c->lines &= (uint8_t)~bit;
        if (c->ltim) c->irr &= (uint8_t)~bit;    /* level: IRR follows pin */
    }
    if (irq >= 8 && (m->pic.slave.irr & ~m->pic.slave.imr))
        m->pic.master.irr |= 0x04;
}

/* --------------------------------------------------------------- INTA */

/* What an INTA right now would acknowledge:
 *   -1        nothing pending at all -> master spurious vector (base+7)
 *   0..7      a master line
 *   8..15     a slave line (8 + slave line)
 *   DRAINED   master IRQ2 is up but the slave has drained since -> the
 *             cascade INTA still consumes master slot 2 and the slave
 *             answers with ITS spurious vector (base+7), no slave ISR. */
#define PIC_DRAINED 16
static int next_line(machine_t *m) {
    pic_chip_t *mr = &m->pic.master, *sl = &m->pic.slave;
    chip_sync_level(mr); chip_sync_level(sl);
    int cand = chip_next(mr);
    if (cand < 0) return -1;
    if (cand != 2) return cand;
    if (!gating_allows(mr, 2)) return -1;
    int scand = chip_next(sl);
    if (scand < 0) return PIC_DRAINED;
    return 8 + scand;
}

int pic_pending(machine_t *m) {
    return next_line(m) >= 0;
}

int pic_intack(machine_t *m) {
    pic_chip_t *mr = &m->pic.master, *sl = &m->pic.slave;
    int n = next_line(m);
    if (n < 0) return mr->vector_base + 7;       /* spurious IRQ7: no ISR */
    if (n < 8) { claim_line(mr, n); return mr->vector_base + n; }
    if (n == PIC_DRAINED) {
        mr->irr &= 0xFB;
        if (!mr->auto_eoi) mr->isr |= 0x04;
        else if (mr->rotate_aeoi) mr->prio_low = 2;
        return sl->vector_base + 7;              /* spurious IRQ15: no slave ISR */
    }
    claim_line(mr, 2);
    claim_line(sl, n - 8);
    if (!(sl->irr & ~sl->imr)) mr->irr &= 0xFB;
    return sl->vector_base + (n - 8);
}

/* ---------------------------------------------------------------- ports */

static void pic_cmd_write(machine_t *m, pic_chip_t *c, uint16_t port, uint32_t val) {
    (void)m; (void)port;
    uint8_t v = (uint8_t)val;
    if (v & 0x10) { /* ICW1: begin the init sequence */
        c->icw_step = 1;
        c->icw3_needed = !(v & 0x02);
        c->icw4_needed = v & 0x01;
        c->ltim = (v >> 3) & 1;
        c->read_isr = 0;
        c->smm = 0; c->prio_low = 7; c->poll_armed = 0; c->rotate_aeoi = 0;
        return;
    }
    if (c->icw_step) return; /* cmd writes while initializing are ignored */
    if (!(v & 0x08)) { /* OCW2: R/SL/EOI in bits 7:5 */
        int line;
        switch (v >> 5) {
        case 1: /* non-specific EOI: clear highest-priority in-service */
            line = best_line(c, c->isr);
            if (line >= 0) c->isr &= (uint8_t)~(1u << line);
            break;
        case 3: /* specific EOI */
            c->isr &= (uint8_t)~(1u << (v & 7));
            break;
        case 5: /* rotate on non-specific EOI */
            line = best_line(c, c->isr);
            if (line >= 0) { c->isr &= (uint8_t)~(1u << line); c->prio_low = (uint8_t)line; }
            break;
        case 7: /* rotate on specific EOI */
            c->isr &= (uint8_t)~(1u << (v & 7));
            c->prio_low = (uint8_t)(v & 7);
            break;
        case 6: c->prio_low = (uint8_t)(v & 7); break; /* set priority */
        case 4: c->rotate_aeoi = 1; break;             /* rotate-in-AEOI set */
        case 0: c->rotate_aeoi = 0; break;             /* rotate-in-AEOI clear */
        default: break;                                /* 0x40: no operation */
        }
        chip_sync_level(c); /* EOI may release a held level line */
        return;
    }
    /* OCW3 (bit3=1). RR=D1, RIS=D0 for the read select; P=D2 arms the poll
     * byte; ESMM=D6/SMM=D5 manage special mask mode. */
    if (v & 0x40) c->smm = (v >> 5) & 1;
    if (v & 0x04) c->poll_armed = 1;
    c->read_isr = v & 1;
}

static void pic_data_write(machine_t *m, pic_chip_t *c, int is_master, uint32_t val) {
    (void)m;
    uint8_t v = (uint8_t)val;
    switch (c->icw_step) {
    case 1: c->vector_base = v & 0xF8;
            c->icw_step = c->icw3_needed ? 2 : (c->icw4_needed ? 3 : 0);
            return;
    case 2: (void)is_master; /* ICW3: slave map / slave id — cascade is fixed */
            c->icw_step = c->icw4_needed ? 3 : 0;
            return;
    case 3: c->auto_eoi = (v >> 1) & 1;     /* ICW4 */
            c->ms  = (v >> 2) & 1;
            c->buf = (v >> 3) & 1;
            c->sfnm = (v >> 4) & 1;
            c->icw_step = 0;
            return;
    default: c->imr = v; return;            /* OCW1 */
    }
}

/* Poll byte: like an INTA the guest drives itself. bit7=request pending,
 * bits 2:0=highest-priority line; acknowledges exactly like INTA. Master
 * poll with a slave request reports line 2 (the cascade slot). */
static uint32_t poll_byte(machine_t *m, pic_chip_t *c) {
    c->poll_armed = 0;
    int cand = chip_next(c);
    if (cand < 0) return 0x00;
    claim_line(c, cand);
    if (c == &m->pic.master && cand == 2) {
        pic_chip_t *sl = &m->pic.slave;
        int s = chip_next(sl);
        if (s >= 0) { claim_line(sl, s); if (!(sl->irr & ~sl->imr)) m->pic.master.irr &= 0xFB; }
        /* drained slave: the byte still reports slot 2, like INTA would */
    }
    return 0x80u | (uint32_t)cand;
}

static uint32_t pic_port_read(void *ctx, uint16_t port, int size) {
    machine_t *m = ctx;
    pic_chip_t *mr = &m->pic.master, *sl = &m->pic.slave;
    (void)size;
    chip_sync_level(mr); chip_sync_level(sl);
    switch (port) {
    case 0x20: return mr->poll_armed ? poll_byte(m, mr) : (mr->read_isr ? mr->isr : mr->irr);
    case 0x21: return mr->imr;
    case 0xA0: return sl->poll_armed ? poll_byte(m, sl) : (sl->read_isr ? sl->isr : sl->irr);
    case 0xA1: return sl->imr;
    }
    return 0xFF;
}

static void pic_port_write(void *ctx, uint16_t port, int size, uint32_t val) {
    machine_t *m = ctx;
    (void)size;
    switch (port) {
    case 0x20: pic_cmd_write(m, &m->pic.master, port, val); break;
    case 0x21: pic_data_write(m, &m->pic.master, 1, val); break;
    case 0xA0: pic_cmd_write(m, &m->pic.slave, port, val); break;
    case 0xA1: pic_data_write(m, &m->pic.slave, 0, val); break;
    }
}

void pic_io_register(machine_t *m) {
    io_register(m, 0x20, 2, pic_port_read, pic_port_write, m, "8259A master");
    io_register(m, 0xA0, 2, pic_port_read, pic_port_write, m, "8259A slave");
}
