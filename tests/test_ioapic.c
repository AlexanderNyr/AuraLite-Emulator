/* tests/test_ioapic.c -- CHIPSET H7 vectors: 82093AA I/O APIC.
 *
 * Board wiring pinned here (documented in ioapic.h): every ISA IRQ line
 * reaches BOTH the 8259 pair and the same-numbered IOAPIC INTIN pin --
 * the fan-out lives inside pic_raise_irq/pic_set_irq, so tests drive
 * board lines through those exact functions (the very devices' path).
 * The PAIR-TEST is the plan gate: an 8254 channel-0 period must surface
 * through a programmed redirection-table entry, the LAPIC, and the CPU
 * instruction boundary, with the 8259 pair fully masked -- so the only
 * way the guest handler can run is the whole 8254->IOAPIC->LAPIC->CPU
 * chain being real.
 */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "machine.h"
#include "platform.h"
#include "pic.h"
#include "pit.h"
#include "lapic.h"
#include "ioapic.h"
#include "harness.h"

typedef struct { machine_t m; } fx_t;

/* ASan lane: free every fixture's two MMIO nodes (lapic + ioapic) at exit. */
#define MAX_FX_NODES 64
static mmio_region_t *FX_NODES[MAX_FX_NODES];
static int FX_NODES_N;
static void fx_nodes_cleanup(void) {
    for (int i = 0; i < FX_NODES_N; i++) free(FX_NODES[i]);
    FX_NODES_N = 0;
}
static void fx_init(fx_t *f) {
    if (!FX_NODES_N) atexit(fx_nodes_cleanup);
    setup_machine(&f->m);
    pic_init(&f->m);  pic_io_register(&f->m);
    pit_init(&f->m);  pit_io_register(&f->m);
    lapic_init(&f->m);  lapic_mmio_register(&f->m);
    ioapic_init(&f->m); ioapic_mmio_register(&f->m);
    FX_NODES[FX_NODES_N++] = f->m.mmio_list;
    FX_NODES[FX_NODES_N++] = f->m.mmio_list->next;
}

static void ivt(machine_t *m, int vec, uint16_t off) {
    mem_write(m, (uint64_t)vec * 4,     2, off);
    mem_write(m, (uint64_t)vec * 4 + 2, 2, 0);
}

#define PH_COUNT 0x0200   /* inc byte [0x2101]; iret */
static const uint8_t H_COUNT[] = { 0xFE,0x06,0x01,0x21, 0xCF };
static const uint8_t T_STI_SPIN[] = { 0xFB, 0xEB, 0xFE };

/* IOAPIC indirect-register access through the real MMIO window */
static uint32_t ia_read(fx_t *f, uint32_t idx) {
    mem_write(&f->m, IOAPIC_BASE + 0x00, 4, idx);
    return (uint32_t)mem_read(&f->m, IOAPIC_BASE + 0x10, 4);
}
static void ia_write(fx_t *f, uint32_t idx, uint32_t v) {
    mem_write(&f->m, IOAPIC_BASE + 0x00, 4, idx);
    mem_write(&f->m, IOAPIC_BASE + 0x10, 4, v);
}
/* LAPIC register helpers */
static uint32_t llr(fx_t *f, uint32_t off) { return (uint32_t)mem_read(&f->m, LAPIC_BASE + off, 4); }
static void     llw(fx_t *f, uint32_t off, uint32_t v) { mem_write(&f->m, LAPIC_BASE + off, 4, v); }

#define RTE_LO(pin) (0x10 + (pin) * 2)
#define RTE_HI(pin) (0x11 + (pin) * 2)

/* -------------------------------------------------------- registers */

static void test_regs(void) {
    fx_t f; fx_init(&f);
    assert(ia_read(&f, 0x01) == (((IOAPIC_MAXRED - 1) << 16) | 0x11u));  /* VER */
    assert(ia_read(&f, 0x00) == 0);                                    /* ID */
    ia_write(&f, 0x00, 0x0A000000u);
    assert(ia_read(&f, 0x00) == 0x0A000000u);
    assert(ia_read(&f, 0x02) == 0x0A000000u);                          /* ARB mirror */
    ia_write(&f, 0x01, 0xFFFFFFFFu);                                   /* VER is RO */
    assert(ia_read(&f, 0x01) == (((IOAPIC_MAXRED - 1) << 16) | 0x11u));
    for (int p = 0; p < IOAPIC_MAXRED; p++)
        assert(ia_read(&f, RTE_LO(p)) == 0x10000u);                    /* all masked */
    /* full field readback on rte[2]: vector/dm/pol/trig/mask + dest */
    ia_write(&f, RTE_LO(2), 0x0001A040u);      /* mask|trig|pol|vec 0x40 */
    ia_write(&f, RTE_HI(2), 0x3C000000u);
    assert(ia_read(&f, RTE_LO(2)) == 0x0001A040u);
    assert(ia_read(&f, RTE_HI(2)) == 0x3C000000u);
    assert(ia_read(&f, 0x08) == 0);            /* reserved index: 0 */
    printf("ok: regs\n");
}

/* ---------------------------------------------------- edge semantics */

static void test_edge_latched_while_masked_flushes_on_unmask(void) {
    fx_t f; fx_init(&f);
    /* rte[1]: edge, high-pol, vector 0x40, MASKED */
    ia_write(&f, RTE_LO(1), 0x10000u | 0x40);
    pic_set_irq(&f.m, 1, 1);                     /* board line rises */
    pic_set_irq(&f.m, 1, 0);
    assert(llr(&f, 0x120) == 0);                     /* nothing delivered */
    ia_write(&f, RTE_LO(1), 0x40);               /* unmask -> pending edge flushes */
    assert(llr(&f, 0x120) == 1);                 /* LAPIC IRR vector 64 */
    assert(ia_read(&f, RTE_LO(1)) == 0x40);      /* edge: no remote-IRR state */
    llw(&f, LAPIC_EOI, 0);
    printf("ok: edge_latched_while_masked_flushes_on_unmask\n");
}

static void test_level_entry_held_line_and_eoi_feedback(void) {
    fx_t f; fx_init(&f);
    llw(&f, LAPIC_SVR, 0x1FF);                   /* delivery needs the LAPIC on */
    /* rte[2]: LEVEL, high-pol, vector 0x50, unmasked */
    ia_write(&f, RTE_LO(2), 0x8000u | 0x50);
    pic_set_irq(&f.m, 2, 1);                     /* line asserts */
    assert(llr(&f, 0x120) == 0x00010000u);       /* IRR bit 80 (0x50) */
    assert(ia_read(&f, RTE_LO(2)) & 0x4000);     /* remote-IRR visible in RTE */
    pic_set_irq(&f.m, 2, 0);                     /* deassert while in service */
    /* intack + EOI feed back through the LAPIC into remote_IRR */
    assert(lapic_intack(&f.m) == 0x50);
    /* (no live line -> EOI just clears our remote_IRR, no redelivery) */
    llw(&f, LAPIC_EOI, 0);
    assert(!(ia_read(&f, RTE_LO(2)) & 0x4000));
    /* re-assert the line: delivers again (new assertion) */
    pic_set_irq(&f.m, 2, 1);
    assert(llr(&f, 0x120) == 0x00010000u);
    /* EOI with the line STILL HELD -> the level redelivers immediately */
    (void)lapic_intack(&f.m);
    llw(&f, LAPIC_EOI, 0);
    assert(llr(&f, 0x120) == 0x00010000u);       /* redelivered */
    /* drop the line, then EOI: nothing left to redeliver */
    pic_set_irq(&f.m, 2, 0);
    (void)lapic_intack(&f.m);
    llw(&f, LAPIC_EOI, 0);
    assert(llr(&f, 0x120) == 0);
    printf("ok: level_entry_held_line_and_eoi_feedback\n");
}

static void test_polarity_and_mask_level(void) {
    fx_t f; fx_init(&f);
    llw(&f, LAPIC_SVR, 0x1FF);
    /* rte[3]: level, LOW-asserted polarity, vector 0x41, masked at first */
    ia_write(&f, RTE_LO(3), 0x10000u | 0x8000u | 0x2000u | 0x41);
    pic_set_irq(&f.m, 3, 0);                     /* voltage low = asserted */
    assert(llr(&f, 0x120) == 0);                 /* masked: nothing */
    ia_write(&f, RTE_LO(3), 0x8000u | 0x2000u | 0x41);   /* unmask: re-eval */
    assert(llr(&f, 0x120) == 2);                 /* IRR vector 65 */
    pic_set_irq(&f.m, 3, 1);                     /* voltage high = deasserted */
    (void)lapic_intack(&f.m);
    llw(&f, LAPIC_EOI, 0);
    assert(llr(&f, 0x120) == 0);                 /* deasserted line: quiet */
    printf("ok: polarity_and_mask_level\n");
}

static void test_nonfixed_delivery_modes_are_stored_not_delivered(void) {
    fx_t f; fx_init(&f);
    ia_write(&f, RTE_LO(4), (4u << 8) | 0x42);   /* NMI delivery mode */
    assert(ia_read(&f, RTE_LO(4)) == ((4u << 8) | 0x42));
    pic_set_irq(&f.m, 4, 1);                     /* edge on the line */
    pic_set_irq(&f.m, 4, 0);
    assert(llr(&f, 0x120) == 0);                 /* DM!=fixed: not delivered */
    printf("ok: nonfixed_delivery_modes_are_stored_not_delivered\n");
}

/* ------------------------------------------------ PAIR-TEST (the gate) */

static void test_pair_pit_chain_into_lapic_and_cpu(void) {
    fx_t f; fx_init(&f);
    ivt(&f.m, 0x50, PH_COUNT);
    memcpy(f.m.ram + PH_COUNT, H_COUNT, sizeof H_COUNT);
    memcpy(f.m.ram, T_STI_SPIN, sizeof T_STI_SPIN);

    /* 8259 pair fully masked: the PIC CANNOT be what delivers today */
    io_write(&f.m, 0x21, 1, 0xFF);
    io_write(&f.m, 0xA1, 1, 0xFF);

    /* IOAPIC rte[0]: edge, high-pol, vector 0x50, unmasked */
    ia_write(&f, RTE_LO(0), 0x50);
    /* LAPIC software-enabled, spurious 0xFF */
    llw(&f, LAPIC_SVR, 0x1FF);

    /* 8254 channel 0, mode 3 (square), reload 32 */
    io_write(&f.m, 0x43, 1, 0x36);
    io_write(&f.m, 0x40, 1, 0x20);
    io_write(&f.m, 0x40, 1, 0x00);

    f.m.cpu.rip = 0;
    for (int i = 0; i < 5000 && mem_read(&f.m, 0x2101, 1) < 1; i++)
        (void)cpu_step(&f.m.cpu);
    assert(mem_read(&f.m, 0x2101, 1) == 1);      /* fire #1 through the chain */
    assert(f.m.pic.master.irr & 1);              /* the LINE hit the PIC too */
    assert(!pic_pending(&f.m));                  /* ...which stayed masked out */
    assert(llr(&f, 0x1A0) == 0x00010000u);       /* LAPIC ISR vector 0x50 */

    /* real-mode handler can't write the MMIO window (measured H6), so the
     * host EOI stands in for the guest's here; the boundary must hand the
     * very next 8254 period to the guest again */
    llw(&f, LAPIC_EOI, 0);
    for (int i = 0; i < 5000 && mem_read(&f.m, 0x2101, 1) < 2; i++)
        (void)cpu_step(&f.m.cpu);
    assert(mem_read(&f.m, 0x2101, 1) == 2);      /* fire #2 */
    printf("ok: pair_pit_chain_into_lapic_and_cpu\n");
}

static void test_warm_reset_masks_all_entries(void) {
    fx_t f; fx_init(&f);
    ia_write(&f, RTE_LO(5), 0x41);
    machine_reset(&f.m);
    assert(ia_read(&f, RTE_LO(5)) == 0x10000u);  /* masked again */
    assert(llr(&f, LAPIC_SVR) == 0x00FF);        /* LAPIC soft-disabled */
    printf("ok: warm_reset_masks_all_entries\n");
}

int main(void) {
    test_regs();
    test_edge_latched_while_masked_flushes_on_unmask();
    test_level_entry_held_line_and_eoi_feedback();
    test_polarity_and_mask_level();
    test_nonfixed_delivery_modes_are_stored_not_delivered();
    test_pair_pit_chain_into_lapic_and_cpu();
    test_warm_reset_masks_all_entries();
    printf("test_ioapic: all vectors passed\n");
    return 0;
}
