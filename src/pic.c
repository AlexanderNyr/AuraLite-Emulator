/* pic.c -- 8259A sketch (CHIPSET H0): init sequence, masking, fixed
 * priority, INTA vector delivery, EOI, cascade. See pic.h for scope. */
#include <string.h>
#include "machine.h"
#include "pic.h"

void pic_init(machine_t *m) {
    m->pic.master = (pic_chip_t){ .imr = 0xFF, .vector_base = 0x08, .read_isr = 0 };
    m->pic.slave  = (pic_chip_t){ .imr = 0xFF, .vector_base = 0x70, .read_isr = 0 };
}

/* ---------------------------------------------------------------- lines */

void pic_raise_irq(machine_t *m, int irq) {
    if (irq < 0 || irq > 15) return;
    if (irq < 8) m->pic.master.irr |= (uint8_t)(1u << irq);
    else {
        m->pic.slave.irr |= (uint8_t)(1u << (irq - 8));
        /* cascade: a slave request shows up as master IRQ2 (edge) */
        if (m->pic.slave.irr & ~m->pic.slave.imr)
            m->pic.master.irr |= 0x04;
    }
}

int pic_pending(machine_t *m) {
    return (m->pic.master.irr & ~m->pic.master.imr) != 0;
}

static int claim_line(pic_chip_t *c, int line) {
    if (c->auto_eoi) c->irr &= (uint8_t)~(1u << line);
    else { c->isr |= (uint8_t)(1u << line); c->irr &= (uint8_t)~(1u << line); }
    return c->vector_base + line;
}

int pic_intack(machine_t *m) {
    pic_chip_t *mr = &m->pic.master, *sl = &m->pic.slave;
    uint8_t pend = mr->irr & ~mr->imr;
    for (int i = 0; i < 8; i++) {
        if (!(pend & (1u << i))) continue;
        if (i != 2) return claim_line(mr, i);
        /* cascade: find the slave's own highest-priority line */
        if (!(mr->imr & 0x04)) {
            uint8_t spend = sl->irr & ~sl->imr;
            for (int j = 0; j < 8; j++)
                if (spend & (1u << j)) {
                    int vec = claim_line(sl, j);
                    if (!sl->auto_eoi && !mr->auto_eoi) { mr->isr |= 0x04; }
                    mr->irr &= 0xFB;
                    if (!(sl->irr & ~sl->imr)) mr->irr &= 0xFB;
                    return vec;
                }
        }
    }
    return -1;
}

/* ---------------------------------------------------------------- ports */

static void pic_cmd_write(machine_t *m, pic_chip_t *c, uint16_t port, uint32_t val) {
    (void)m; (void)port;
    uint8_t v = (uint8_t)val;
    if (v & 0x10) { /* ICW1: begin the init sequence */
        c->icw_step = 1;
        c->icw3_needed = !(v & 0x02);
        c->icw4_needed = v & 0x01;
        c->read_isr = 0;
        return;
    }
    if (c->icw_step) return; /* cmd writes while initializing are ignored */
    if (!(v & 0x08)) { /* OCW2 */
        if ((v & 0x60) == 0x20) {           /* non-specific EOI */
            for (int i = 0; i < 8; i++)
                if (c->isr & (1u << i)) { c->isr &= (uint8_t)~(1u << i); break; }
        } else if ((v & 0x60) == 0x60)      /* specific EOI */
            c->isr &= (uint8_t)~(1u << (v & 7));
        /* rotation / mode bits are H1 */
        return;
    }
    /* OCW3 (bit3=1): only the IRR/ISR read select is H0 scope.
     * RR is D1, RIS is D0: 0x0A reads IRR, 0x0B reads ISR. */
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
    case 3: c->auto_eoi = (v >> 1) & 1;     /* ICW4 bit1 */
            c->icw_step = 0;
            return;
    default: c->imr = v; return;            /* OCW1 */
    }
}

static uint32_t pic_port_read(void *ctx, uint16_t port, int size) {
    machine_t *m = ctx;
    pic_chip_t *mr = &m->pic.master, *sl = &m->pic.slave;
    (void)size;
    switch (port) {
    case 0x20: return mr->read_isr ? mr->isr : mr->irr;
    case 0x21: return mr->imr;
    case 0xA0: return sl->read_isr ? sl->isr : sl->irr;
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
