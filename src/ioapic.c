/* ioapic.c -- 82093AA I/O APIC (CHIPSET H7). Contract in ioapic.h. */
#include <stdio.h>
#include <string.h>
#include "machine.h"
#include "ioapic.h"
#include "lapic.h"

#define RTE_VECTOR   0x000000FFu
#define RTE_DELMOD   0x00000700u
#define RTE_DESTMOD  0x00000800u
#define RTE_DELIVS   0x00001000u
#define RTE_POLARITY 0x00002000u
#define RTE_RIRR     0x00004000u
#define RTE_TRIGGER  0x00008000u
#define RTE_MASK     0x00010000u

/* ---------------------------------------------------------- delivery */

static void rte_evaluate(machine_t *m, int pin) {
    ioapic_t *ia = &m->ioapic;
    if (pin < 0 || pin >= IOAPIC_MAXRED) return;
    ioapic_rte_t *e = &ia->rte[pin];
    if (e->lo & RTE_MASK) return;
    if ((e->lo & RTE_DELMOD) != 0) return;     /* fixed only (documented) */
    int vec = (int)(e->lo & RTE_VECTOR);
    if (vec < 16) return;                      /* reserved, like the LAPIC */

    if (e->lo & RTE_TRIGGER) {                 /* level */
        int asserted = ((ia->lines & (1u << pin)) != 0) ^ ((e->lo & RTE_POLARITY) != 0);
        if (!asserted) return;
        if (ia->remote_irr & (1u << pin)) return;         /* awaiting EOI */
        ia->remote_irr |= (1u << pin);
        lapic_set_irr(m, vec);
    } else {                                   /* edge */
        if (!(ia->edge_pend & (1u << pin))) return;       /* nothing latched */
        ia->edge_pend &= ~(1u << pin);
        lapic_set_irr(m, vec);
    }
}

/* -------------------------------------------------- board line drives */

void ioapic_set_gsi(struct machine *m, int irq, int level) {
    if (irq < 0 || irq >= IOAPIC_MAXRED) return;
    ioapic_t *ia = &m->ioapic;
    uint32_t bit = 1u << irq;
    int was = (ia->lines & bit) != 0;
    if (level == was) return;
    if (level) ia->lines |= bit; else ia->lines &= ~bit;
    /* a transition toward the pin's programmed-asserted side is an edge
     * event for edge-triggered entries (latched when masked) */
    int asserted = level ^ ((ia->rte[irq].lo & RTE_POLARITY) != 0);
    if (asserted && !(ia->rte[irq].lo & RTE_TRIGGER))
        ia->edge_pend |= bit;
    rte_evaluate(m, irq);
}

void ioapic_edge_gsi(struct machine *m, int irq) {
    if (irq < 0 || irq >= IOAPIC_MAXRED) return;
    /* a zero-width strobe: assert then release, edge semantics implied */
    ioapic_set_gsi(m, irq, 1);
    ioapic_set_gsi(m, irq, 0);
}

/* LAPIC EOI clears our remote_IRR for level entries on that vector;
 * a still-asserted line then redelivers (evaluated immediately). */
void ioapic_eoi_notify(machine_t *m, int vector) {
    ioapic_t *ia = &m->ioapic;
    for (int pin = 0; pin < IOAPIC_MAXRED; pin++) {
        if (!(ia->remote_irr & (1u << pin))) continue;
        ioapic_rte_t *e = &ia->rte[pin];
        if (!(e->lo & RTE_TRIGGER)) continue;
        if ((int)(e->lo & RTE_VECTOR) != vector) continue;
        ia->remote_irr &= ~(1u << pin);
        rte_evaluate(m, pin);
    }
}

/* ------------------------------------------------------------ MMIO */

static uint32_t ioapic_reg_read(machine_t *m, uint32_t idx) {
    ioapic_t *ia = &m->ioapic;
    if (idx == 0x00) return ia->id;
    if (idx == 0x01) return ((IOAPIC_MAXRED - 1) << 16) | 0x11u;
    if (idx == 0x02) return ia->id & 0x0F000000u;
    if (idx >= 0x10 && idx < 0x10 + IOAPIC_MAXRED * 2) {
        int pin = (int)(idx - 0x10) / 2;
        uint32_t v = (idx & 1) ? ia->rte[pin].hi : ia->rte[pin].lo;
        if (!(idx & 1)) {
            v &= ~RTE_RIRR;
            if (ia->remote_irr & (1u << pin)) v |= RTE_RIRR;   /* live */
        }
        return v;
    }
    return 0;                                   /* reserved: reads 0 */
}

static void ioapic_reg_write(machine_t *m, uint32_t idx, uint32_t v) {
    ioapic_t *ia = &m->ioapic;
    if (idx == 0x00) { ia->id = (ia->id & 0xF0FFFFFFu) | (v & 0x0F000000u); return; }
    if (idx >= 0x10 && idx < 0x10 + IOAPIC_MAXRED * 2) {
        int pin = (int)(idx - 0x10) / 2;
        if (idx & 1) ia->rte[pin].hi = v & 0xFF000000u;
        else ia->rte[pin].lo = (v & (RTE_MASK | RTE_TRIGGER | RTE_POLARITY |
                                     RTE_DESTMOD | RTE_DELMOD | RTE_VECTOR));
        /* reprogramming re-evaluates: pending edges flush, still-asserted
         * level entries get one re-try (remote_IRR semantics preserved) */
        rte_evaluate(m, pin);
        return;
    }
    /* 0x01/0x02 read-only, other indices reserved */
}

static uint64_t ioapic_mmio_read(void *ctx, uint64_t addr, int size) {
    machine_t *m = ctx;
    uint64_t off = addr - IOAPIC_BASE;
    uint32_t v = 0;
    if (off == 0x00) v = m->ioapic.regsel;
    else if (off == 0x10) v = ioapic_reg_read(m, m->ioapic.regsel);
    return v & (size == 1 ? 0xFFu : size == 2 ? 0xFFFFu : 0xFFFFFFFFu);
}

static void ioapic_mmio_write(void *ctx, uint64_t addr, int size, uint64_t val) {
    machine_t *m = ctx;
    (void)size;
    uint64_t off = addr - IOAPIC_BASE;
    uint32_t v = (uint32_t)val;
    if (off == 0x00) m->ioapic.regsel = (uint8_t)v;
    else if (off == 0x10) ioapic_reg_write(m, m->ioapic.regsel, v);
}

/* ------------------------------------------------------------ init */

void ioapic_init(machine_t *m) {
    memset(&m->ioapic, 0, sizeof m->ioapic);
    for (int i = 0; i < IOAPIC_MAXRED; i++)
        m->ioapic.rte[i].lo = RTE_MASK;         /* all entries masked */
}

void ioapic_mmio_register(machine_t *m) {
    mem_register_mmio(m, IOAPIC_BASE, IOAPIC_SIZE,
                      ioapic_mmio_read, ioapic_mmio_write, m, "IOAPIC");
}
