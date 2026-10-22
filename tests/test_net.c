/* tests/test_net.c -- NET N0+N1 vectors: the e1000 82540EM model.
 *
 * Contract measured from AuraLite-OS@0ed0d29 (drivers/e1000/e1000.c,
 * kernel/arch/i386/net32.c) and QEMU's hw/net/e1000.c e1000_reset:
 *   - PCI 8086:100E, BAR0 128 KiB register file;
 *   - CTRL.RST (bit 26) self-clears after the register reset;
 *   - RAL/RAH preloaded with the QEMU default MAC 52:54:00:12:34:56
 *     (RAH.AV set); since U2 the same MAC also sits in EEPROM words
 *     0-2, the source the guest's probe actually asks first (the N0
 *     header's "prefers it over EEPROM" measured the fallback order
 *     backwards -- U0 found EERD first, RAL/RAH only on timeout);
 *   - RXCSUM reads 0 after reset (QEMU compat);
 *   - RX ring ingress: frame DMA into the head buffer, DD status, RDH
 *     advance, ICR.RXT0; RCTL.EN gates the receiver, RDH==RDT drops.
 *
 * N1 adds the TX ring (QEMU start_xmit / process_tx_desc / txdesc_writeback,
 * guest side e1000_send()):
 *   - the doorbell is a TDT write AND a TCTL write -- both map to QEMU's
 *     set_tctl, which stores, masks TDT to 16 bits and calls start_xmit();
 *   - TCTL.EN gates the walk ("tx disabled" -> return);
 *   - the walk runs while TDH != TDT, DD is written back inside the doorbell
 *     store (the guest spins on status&0x01 with a bounded PAUSE budget),
 *     CMD stays intact (QEMU's txdesc_writeback does not clear it), and
 *     TDH wraps with "++TDH * 16 >= TDLEN -> TDH = 0";
 *   - ICR gets TXDW (bit 0) for RS descriptors and TXQE (bit 7) for every
 *     completed walk;
 *   - a frame without CMD.EOP accumulates: no wire, no DD, no TXDW.
 *
 * U2 adds the EEPROM + PHY vectors (QEMU hw/net/e1000.c set_mdic /
 * flash_eerd_read / phy_reg_init, e1000x_core_prepare_eeprom):
 *   - the guest's exact EERD probe (START | word<<8, DONE poll, data in
 *     bits 31:16) returns the RAL/RAH MAC from words 0-2 with DONE on
 *     the first poll, the identity words read 0x100E/0x8086, all 64
 *     words sum to 0xBABA, out-of-range completes with zero data, and
 *     CTRL.RST leaves the image alone;
 *   - MDIC reaches the PHY file at address 1: ID/status/template
 *     values, RW round-trips, ERROR on read-only / unimplemented /
 *     wrong-PHY accesses (with QEMU's previous-value echo), the BMCR
 *     store mask (RESET/ANRESTART self-clear, file not reset), the
 *     MDAC cause onto the INTx wire, and reset restoring the template.
 */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "devices.h"
#include "harness.h"
#include "machine.h"

#define E1000_MMIO  0xFEBC0000ull

typedef struct { machine_t m; } fx_t;

static void fx_init(fx_t *f) {
    setup_machine(&f->m);
    devices_init_common(&f->m);
    e1000_test_add(&f->m);
}

static void fx_done(fx_t *f) { devices_done(&f->m); }

static uint32_t rd(fx_t *f, uint32_t off) {
    return mem_read(&f->m, E1000_MMIO + off, 4);
}
static void wr(fx_t *f, uint32_t off, uint32_t v) {
    mem_write(&f->m, E1000_MMIO + off, 4, v);
}

static void test_pci_reset_contract(void) {
    fx_t f; fx_init(&f);
    /* QEMU reset core: RXCSUM 0, RAL/RAH preload, link up */
    assert(rd(&f, 0x5000) == 0);                   /* RXCSUM after reset */
    assert(rd(&f, 0x5400) == 0x12005452u);         /* RAL0: 52:54:00:12 */
    assert(rd(&f, 0x5404) == 0x80005634u);         /* RAH0: AV | 34:56 */
    assert(rd(&f, 0x0008) & (1u << 1));            /* STATUS.LU */
    /* CTRL.RST self-clears (the guest polls on this bit) */
    wr(&f, 0x0000, (1u << 26));
    assert(!(rd(&f, 0x0000) & (1u << 26)));
    assert(rd(&f, 0x5400) == 0x12005452u);         /* preload survives */
    /* ICR is read-to-clear */
    wr(&f, 0x00C0, 0);
    /* PCI BAR0 readback over config mechanism #1 (0xCF8/0xCFC) -- the guest
     * maps the register file through pci_get_bar(); the direct-load lane has
     * no BIOS to assign the BAR, so the model seeds it (regression vector:
     * the N0 lane measured "BAR0 at phys 0x0" before the fix). */
    io_write(&f.m, 0xCF8, 4, 0x80002010u);          /* 0:4.0 cfg addr, BAR0 */
    assert(io_read(&f.m, 0xCFC, 4) == 0xFEBC0000u); /* BAR0 phys, type bits 0 */
    fx_done(&f);
    printf("ok e1000 pci/reset contract (RXCSUM 0, RAL/RAH preload, RST self-clear)\n");
}

static void test_rx_ingress(void) {
    fx_t f; fx_init(&f);
    const uint64_t ring = 0x100000;                /* 8 descriptors */
    const uint64_t buf  = 0x200000;
    /* head descriptor -> buf */
    mem_write(&f.m, ring + 0, 4, (uint32_t)buf);
    mem_write(&f.m, ring + 4, 4, 0);
    wr(&f, 0x2800, (uint32_t)ring);                /* RDBAL */
    wr(&f, 0x2804, 0);                             /* RDBAH */
    wr(&f, 0x2808, 8 * 16);                        /* RDLEN */
    wr(&f, 0x2810, 0);                             /* RDH */
    wr(&f, 0x2818, 7);                             /* RDT */
    wr(&f, 0x0100, (1u << 1) | (1u << 15) | (1u << 26)); /* RCTL EN|BAM|SECRC */

    /* receiver off: the frame is dropped */
    wr(&f, 0x0100, 0);
    e1000_rx_frame(&f.m, (const uint8_t *)"drop", 4);
    assert(rd(&f, 0x2810) == 0);                   /* RDH unchanged */
    wr(&f, 0x0100, (1u << 1) | (1u << 15) | (1u << 26));

    /* the ingress lane: frame lands in the buffer, DD set, RDH++ */
    e1000_rx_frame(&f.m, (const uint8_t *)"hello", 5);
    assert(mem_read(&f.m, buf, 4) == 0x6c6c6568);  /* "hell" LE */
    assert(mem_read(&f.m, buf + 4, 1) == 'o');
    assert(mem_read(&f.m, ring + 8, 2) == 5);      /* desc.length */
    assert(mem_read(&f.m, ring + 12, 1) & 0x01);   /* desc.status DD */
    assert(rd(&f, 0x2810) == 1);                   /* RDH advanced */
    assert(rd(&f, 0x00C0) & (1u << 7));            /* ICR.RXT0 */
    assert(rd(&f, 0x00C0) == 0);                   /* read-to-clear */

    /* ring full (RDH == RDT): drop */
    wr(&f, 0x2818, 1);                             /* RDT = RDH = 1 */
    e1000_rx_frame(&f.m, (const uint8_t *)"x", 1);
    assert(rd(&f, 0x2810) == 1);                   /* unchanged */
    fx_done(&f);
    printf("ok e1000 rx ingress (DMA + DD + RDH, EN/full gating)\n");
}

/* ---- N1 helpers: one legacy TX descriptor, exactly as e1000_send() builds it
 * (length, cso=0, cmd=0x0B = EOP|IFCS|RS, status=0). ---- */
static void tx_desc_put(fx_t *f, uint64_t ring, unsigned idx, uint64_t buf,
                        uint16_t len, uint8_t cmd) {
    uint64_t d = ring + idx * 16;
    mem_write(&f->m, d + 0, 4, (uint32_t)buf);          /* addr lo */
    mem_write(&f->m, d + 4, 4, 0);                      /* addr hi */
    mem_write(&f->m, d + 8, 4, (uint32_t)len | ((uint32_t)cmd << 24));
    mem_write(&f->m, d + 12, 4, 0);                     /* status/css/popts */
}
static uint8_t tx_desc_status(fx_t *f, uint64_t ring, unsigned idx) {
    return (uint8_t)mem_read(&f->m, ring + idx * 16 + 12, 1);
}
static uint8_t tx_desc_cmd(fx_t *f, uint64_t ring, unsigned idx) {
    return (uint8_t)mem_read(&f->m, ring + idx * 16 + 11, 1);
}

/* Program the TX ring the way the guest's e1000_init() does: TDLEN =
 * NUM_TX_DESC*16, TDH = TDT = 0, TCTL = EN | (0x10<<4) | (0x40<<12).
 * The wire counters are device state, so a CTRL.RST first gives every
 * vector its own baseline instead of inheriting the previous one's. */
static void tx_ring_setup(fx_t *f, uint64_t ring, uint32_t ndesc) {
    wr(f, 0x0000, (1u << 26));                          /* CTRL.RST */
    wr(f, 0x3800, (uint32_t)ring);                      /* TDBAL */
    wr(f, 0x3804, 0);                                   /* TDBAH */
    wr(f, 0x3808, ndesc * 16);                          /* TDLEN */
    wr(f, 0x3810, 0);                                   /* TDH */
    wr(f, 0x3818, 0);                                   /* TDT */
    wr(f, 0x0400, (1u << 1) | (0x10u << 4) | (0x40u << 12));
}

static void test_tx_doorbell_completion(void) {
    fx_t f; fx_init(&f);
    const uint64_t ring = 0x100000;                     /* 32 descriptors */
    const uint64_t buf  = 0x200000;
    tx_ring_setup(&f, ring, 32);
    assert(rd(&f, 0x3808) == 32 * 16);                  /* TDLEN stored */
    (void)rd(&f, 0x00C0);                               /* drain setup TXQE */

    /* The guest's frame: broadcast dst, our MAC src, a tagged payload. */
    static const uint8_t frame[20] = {
        0xff,0xff,0xff,0xff,0xff,0xff, 0x52,0x54,0x00,0x12,0x34,0x56,
        0x08,0x00, 'N','1','T','X','O','K'
    };
    for (unsigned i = 0; i < sizeof frame; i++)
        mem_write(&f.m, buf + i, 1, frame[i]);
    tx_desc_put(&f, ring, 0, buf, sizeof frame, 0x0B);  /* EOP|IFCS|RS */
    assert(tx_desc_status(&f, ring, 0) == 0);

    /* The doorbell: one TDT store.  DD must be back before it returns --
     * e1000_send() spins on status&0x01 with a 100000-PAUSE budget and
     * prints "[e1000] TX timeout" when it never lands (defect D2). */
    wr(&f, 0x3818, 1);
    assert(tx_desc_status(&f, ring, 0) == 0x01);        /* DD */
    assert(tx_desc_cmd(&f, ring, 0) == 0x0B);           /* CMD intact (QEMU) */
    assert(rd(&f, 0x3810) == 1);                        /* TDH advanced */
    assert(rd(&f, 0x3818) == 1);                        /* TDT as written */

    /* ICR: TXDW (RS writeback) + TXQE (QEMU raises it on every walk). */
    uint32_t icr = rd(&f, 0x00C0);
    assert(icr & (1u << 0));                            /* TXDW  */
    assert(icr & (1u << 7));                            /* TXQE  */
    assert(rd(&f, 0x00C0) == 0);                        /* read-to-clear */

    /* Second descriptor in the same ring: the walk is repeatable. */
    tx_desc_put(&f, ring, 1, buf, sizeof frame, 0x0B);
    wr(&f, 0x3818, 2);
    assert(tx_desc_status(&f, ring, 1) == 0x01);
    assert(rd(&f, 0x3810) == 2);

    /* The socketless lane drops the frames but still completes them (QEMU
     * with a null backend); the counters are the observable proof. */
    uint64_t sent = 1, dropped = 0;
    e1000_tx_stats(&sent, &dropped);
    assert(sent == 0 && dropped == 2);
    fx_done(&f);
    printf("ok e1000 tx doorbell completion (DD + CMD intact + TDH + TXDW/TXQE)\n");
}

static void test_tx_tctl_gate_and_doorbell(void) {
    fx_t f; fx_init(&f);
    const uint64_t ring = 0x100000, buf = 0x200000;
    tx_ring_setup(&f, ring, 32);
    tx_desc_put(&f, ring, 0, buf, 14, 0x0B);
    /* tx_ring_setup's own TCTL write is a doorbell (QEMU maps [TCTL] to
     * set_tctl): with TDH==TDT the walk is empty but start_xmit still ends
     * with set_ics(..., TXQE), so drain the cause register before asserting
     * on the gated one.  Measured: ICR reads 0x80 right after the setup. */
    assert(rd(&f, 0x00C0) == (1u << 7));                 /* TXQE only */
    (void)rd(&f, 0x00C0);                                /* read-to-clear */

    /* Transmitter disabled: QEMU start_xmit() returns before the walk, so
     * the doorbell stores TDT but consumes nothing -- not even TXQE. */
    wr(&f, 0x0400, 0);                                   /* TCTL without EN */
    wr(&f, 0x3818, 1);
    assert(rd(&f, 0x3818) == 1);                        /* TDT stored */
    assert(rd(&f, 0x3810) == 0);                        /* TDH unmoved */
    assert(tx_desc_status(&f, ring, 0) == 0);           /* no DD */
    assert(rd(&f, 0x00C0) == 0);                        /* no cause */

    /* A TCTL write is the second doorbell: QEMU maps [TCTL] and [TDT] to the
     * same op, so enabling the transmitter drains the pending descriptor. */
    wr(&f, 0x0400, (1u << 1) | (0x10u << 4) | (0x40u << 12));
    assert(rd(&f, 0x3810) == 1);                        /* TDH advanced */
    assert(tx_desc_status(&f, ring, 0) == 0x01);        /* DD */
    assert(rd(&f, 0x00C0) & (1u << 0));                 /* TXDW */

    /* TDT is masked to 16 bits by that same op (QEMU set_tctl). */
    wr(&f, 0x3818, 0x00030001u);
    assert(rd(&f, 0x3818) == 1);
    fx_done(&f);
    printf("ok e1000 tx TCTL.EN gate + TCTL-write doorbell + TDT 16-bit mask\n");
}

static void test_tx_multidescriptor_and_wrap(void) {
    fx_t f; fx_init(&f);
    const uint64_t ring = 0x100000;
    tx_ring_setup(&f, ring, 32);
    (void)rd(&f, 0x00C0);                               /* drain setup TXQE */

    /* A frame split over two descriptors: the first carries no EOP, so it
     * only accumulates -- ONE frame reaches the wire, not two.  Both
     * descriptors still get DD, because QEMU calls txdesc_writeback() for
     * every descriptor of the walk (EOP or not) and both have CMD.RS. */
    tx_desc_put(&f, ring, 0, 0x200000, 8, 0x0A);        /* IFCS|RS, no EOP */
    tx_desc_put(&f, ring, 1, 0x200008, 6, 0x0B);        /* EOP|IFCS|RS     */
    wr(&f, 0x3818, 2);
    assert(tx_desc_status(&f, ring, 0) == 0x01);        /* RS -> DD */
    assert(tx_desc_status(&f, ring, 1) == 0x01);
    assert(rd(&f, 0x3810) == 2);
    uint64_t sent = 1, dropped = 0;
    e1000_tx_stats(&sent, &dropped);
    assert(sent == 0 && dropped == 1);                  /* ONE frame, not two */
    (void)rd(&f, 0x00C0);                               /* drain TXDW|TXQE */

    /* No RS: QEMU's txdesc_writeback returns 0 without touching upper.data,
     * so DD never lands -- the pin that keeps the writeback conditional.
     * TXQE is still raised (every walk ends with set_ics(..., TXQE)). */
    tx_desc_put(&f, ring, 2, 0x200000, 14, 0x01);       /* EOP only */
    wr(&f, 0x3818, 3);
    assert(tx_desc_status(&f, ring, 2) == 0);           /* no DD without RS */
    assert(rd(&f, 0x3810) == 3);                        /* consumed anyway */
    assert(!(rd(&f, 0x00C0) & (1u << 0)));              /* no TXDW */

    /* Ring wrap: TDH 31 -> 0 via "++TDH * 16 >= TDLEN".  The head is parked
     * with the pending descriptor already in slot 31, so the single TDT
     * store that opens the slot both transmits it and wraps the head. */
    tx_desc_put(&f, ring, 31, 0x200000, 14, 0x0B);
    wr(&f, 0x3810, 31);                                 /* TDH = last slot */
    assert(rd(&f, 0x3810) == 31);
    wr(&f, 0x3818, 0);                                  /* TDT = wrap point */
    assert(tx_desc_status(&f, ring, 31) == 0x01);       /* slot 31 consumed */
    assert(rd(&f, 0x3810) == 0);                        /* head wrapped */
    assert(rd(&f, 0x3818) == 0);

    /* TDLEN is a set_dlen register: val & 0xfff80 (128-byte multiples). */
    wr(&f, 0x3808, 0xFFFFFFFFu);
    assert(rd(&f, 0x3808) == 0xFFF80u);
    wr(&f, 0x3808, 32 * 16);
    fx_done(&f);
    printf("ok e1000 tx multi-descriptor EOP, RS-gated writeback, TDH wrap, TDLEN mask\n");
}

static void test_interrupt_register_semantics(void) {
    fx_t f; fx_init(&f);

    /* IMS is QEMU's set_ims: `IMS |= val`, not an assign.  The guest writes
     * it once at init (RXT0|RXDMT0|RXO|TXDW|LSC) and never re-masks. */
    wr(&f, 0x00D0, 0x01);
    wr(&f, 0x00D0, 0x80);
    assert(rd(&f, 0x00D0) == 0x81);
    /* IMC clears the written bits out of IMS. */
    wr(&f, 0x00D8, 0x80);
    assert(rd(&f, 0x00D0) == 0x01);

    /* ICR is QEMU's set_icr: `ICR &= ~val` (the N0 model stored nothing).
     * ICR is also read-to-clear, so a read cannot be used to observe a
     * partial clear -- the sequence below proves &= ~val with one read: if
     * the write had been a no-op (N0) the read would return 0x81. */
    wr(&f, 0x00C8, (1u << 0) | (1u << 7));              /* ICS: raise causes */
    wr(&f, 0x00C0, (1u << 0));                          /* clear TXDW only */
    assert(rd(&f, 0x00C0) == 0x80);                     /* TXQE survives */
    assert(rd(&f, 0x00C0) == 0);                        /* read-to-clear */

    /* ICS is QEMU's set_ics: causes OR into ICR (write-only register). */
    wr(&f, 0x00C8, (1u << 4));                          /* RXDMT0 */
    wr(&f, 0x00C8, (1u << 2));                          /* LSC */
    assert(rd(&f, 0x00C0) == 0x14);

    /* TDH/RDH are set_16bit registers. */
    wr(&f, 0x3810, 0x00020003u);
    assert(rd(&f, 0x3810) == 3);
    wr(&f, 0x2810, 0x00020004u);
    assert(rd(&f, 0x2810) == 4);
    fx_done(&f);
    printf("ok e1000 interrupt/register write-op parity (IMS|=, ICR&=~val, 16-bit heads)\n");
}

/* N4 (ledger D4): the INTx line -- the plan's `(ICR & IMS) != 0` condition
 * on the E1000_IRQ board wire (pin level on ISA line 11 = slave bit 3,
 * published at PCI cfg 0x3C/0x3D).  The NIC payloads unmask IMS=0xD5 but
 * run at IF=0 and drain ICR before idling, so their receipts must not see
 * this line -- but the machine must, and this pins it: a masked cause stays
 * quiet, unmasking asserts, and every clear path (IMC, ICR read-to-clear,
 * ICR W1C, CTRL.RST) drops the pin again. */
static void test_intx_line(void) {
    fx_t f; fx_init(&f);
    const uint64_t line_bit = 1u << 3;              /* ISA IRQ 11 -> slave */

    /* the published route: Interrupt Line 11, Interrupt Pin INTA# (the
     * config window is dword-aligned, so both bytes read at 0x3C) */
    io_write(&f.m, 0xCF8, 4, 0x8000203Cu);          /* 0:4.0 cfg addr, reg 0x3C */
    {
        uint32_t v = io_read(&f.m, 0xCFC, 4);
        assert((v & 0xFF) == 11);                   /* cfg 0x3C: IRQ line */
        assert(((v >> 8) & 0xFF) == 1);             /* cfg 0x3D: pin INTA# */
    }

    /* quiet out of reset */
    assert(!(f.m.pic.slave.lines & line_bit));
    assert(!pic_pending(&f.m));

    /* a masked cause must NOT assert: ICS raises TXQE with IMS=0 */
    wr(&f, 0x00C8, (1u << 7));                      /* ICS: TXQE */
    assert(!(f.m.pic.slave.lines & line_bit));
    assert(!pic_pending(&f.m));

    /* The PIC powers up with IMR=0xFF; a guest programs its masks at boot
     * (here: unmask master IRQ2/cascade and slave IRQ11) BEFORE the device
     * raises anything.  With nothing deliverable the pin is still quiet. */
    io_write(&f.m, 0x21, 1, 0xFB);                  /* unmask master IRQ2 (cascade) */
    io_write(&f.m, 0xA1, 1, 0xF7);                  /* unmask slave IRQ11 */
    assert(!pic_pending(&f.m));

    /* unmasking the pending cause asserts the line */
    wr(&f, 0x00D0, (1u << 7));                      /* IMS |= TXQE */
    assert(f.m.pic.slave.lines & line_bit);         /* the pin is high */
    assert(f.m.pic.slave.irr & line_bit);           /* edge latched into IRR */
    assert(pic_pending(&f.m));                      /* and deliverable now */

    /* the complete handler loop the plan's "EOI" element means:
     * INTA claims the line, the handler drains ICR (pin drops), EOI. */
    assert(pic_intack(&f.m) == 0x70 + 3);           /* vector base + 11 */
    assert(rd(&f, 0x00C0) == (1u << 7));            /* handler: read-to-clear */
    assert(!(f.m.pic.slave.lines & line_bit));
    io_write(&f.m, 0xA0, 1, 0x20);                  /* slave EOI (OCW2) */
    io_write(&f.m, 0x20, 1, 0x20);                  /* master EOI */
    assert(!pic_pending(&f.m));                     /* quiet after the loop */

    /* IMC masking drops the pin (level follows the condition) */
    wr(&f, 0x00C8, (1u << 7));                      /* ICS: raise TXQE again */
    assert(f.m.pic.slave.lines & line_bit);
    wr(&f, 0x00D8, (1u << 7));                      /* IMC: mask TXQE again */
    assert(!(f.m.pic.slave.lines & line_bit));
    wr(&f, 0x00D0, (1u << 7));                      /* unmask for the next legs */
    (void)rd(&f, 0x00C0);                           /* drain for a clean slate */

    /* ICR read-to-clear drops the line (the handler's natural clear) */
    wr(&f, 0x00C8, (1u << 7));
    assert(f.m.pic.slave.lines & line_bit);
    assert(rd(&f, 0x00C0) == (1u << 7));            /* drain TXQE */
    assert(!(f.m.pic.slave.lines & line_bit));

    /* ICR W1C drops the line too */
    wr(&f, 0x00C8, (1u << 7));                      /* ICS: TXQE */
    assert(f.m.pic.slave.lines & line_bit);
    wr(&f, 0x00C0, (1u << 7));                      /* W1C TXQE */
    assert(!(f.m.pic.slave.lines & line_bit));

    /* a real TX doorbell asserts with IMS unmasked; draining ICR drops it */
    {
        const uint64_t ring = 0x100000, buf = 0x200000;
        tx_ring_setup(&f, ring, 32);                /* CTRL.RST + rings + TCTL */
        tx_desc_put(&f, ring, 0, buf, 14, 0x0B);
        (void)rd(&f, 0x00C0);                       /* drain setup TXQE */
        wr(&f, 0x00D0, (1u << 7));                  /* IMS |= TXQE (RST cleared it) */
        assert(!(f.m.pic.slave.lines & line_bit));  /* drained: quiet */
        wr(&f, 0x0000, (1u << 0) | (1u << 6));      /* CTRL FD|SLU */
        wr(&f, 0x3818, 1);                          /* the doorbell */
        assert(f.m.pic.slave.lines & line_bit);     /* TXDW|TXQE unmasked */
        (void)rd(&f, 0x00C0);                       /* handler drain */
        assert(!(f.m.pic.slave.lines & line_bit));
    }

    /* RX completion asserts on RXT0; the drain drops it */
    {
        const uint64_t ring = 0x300000, buf = 0x400000;
        mem_write(&f.m, ring + 0, 4, (uint32_t)buf);
        mem_write(&f.m, ring + 4, 4, 0);
        wr(&f, 0x2800, (uint32_t)ring); wr(&f, 0x2804, 0);
        wr(&f, 0x2808, 8 * 16); wr(&f, 0x2810, 0); wr(&f, 0x2818, 7);
        wr(&f, 0x0100, (1u << 1) | (1u << 15) | (1u << 26));
        wr(&f, 0x00D0, (1u << 7));                  /* IMS |= RXT0 */
        e1000_rx_frame(&f.m, (const uint8_t *)"\xaa\xbb", 2);
        assert(f.m.pic.slave.lines & line_bit);
        (void)rd(&f, 0x00C0);
        assert(!(f.m.pic.slave.lines & line_bit));
    }

    /* CTRL.RST drops an asserted line as part of the register reset */
    wr(&f, 0x00D0, (1u << 7));
    wr(&f, 0x00C8, (1u << 7));                      /* ICS: TXQE -> assert */
    assert(f.m.pic.slave.lines & line_bit);
    wr(&f, 0x0000, (1u << 26));                     /* CTRL.RST */
    assert(!(f.m.pic.slave.lines & line_bit));

    fx_done(&f);
    printf("ok e1000 intx line (cfg 0x3C/0x3D, (ICR&IMS) level, every clear path)\n");
}

static void test_tx_link_gate(void) {
    fx_t f; fx_init(&f);
    const uint64_t ring = 0x100000, buf = 0x200000;
    tx_ring_setup(&f, ring, 32);
    tx_desc_put(&f, ring, 0, buf, 14, 0x0B);
    (void)rd(&f, 0x00C0);                               /* drain setup TXQE */

    /* e1000_send() refuses before touching the ring when STATUS.LU is clear
     * ("link down or TX timeout"), so the machine must keep LU readable and
     * must still complete a descriptor the guest submitted anyway -- the
     * device does not gate TX on the link (QEMU does not either). */
    assert(rd(&f, 0x0008) & (1u << 1));                 /* LU on attach */
    wr(&f, 0x0000, (1u << 0) | (1u << 6));              /* CTRL FD|SLU */
    assert(rd(&f, 0x0008) & (1u << 1));
    wr(&f, 0x3818, 1);
    assert(tx_desc_status(&f, ring, 0) == 0x01);

    /* CTRL.RST resets the ring registers and the assembly state. */
    wr(&f, 0x0000, (1u << 26));
    assert(rd(&f, 0x3808) == 0);                        /* TDLEN cleared */
    assert(rd(&f, 0x3810) == 0);                        /* TDH cleared */
    assert(rd(&f, 0x5400) == 0x12005452u);              /* MAC preload back */
    fx_done(&f);
    printf("ok e1000 tx link gate + CTRL.RST resets the TX ring\n");
}

/* ============================ U2: EEPROM ================================== */
/* The guest's exact EEPROM transaction (AuraLite-OS@0ed0d29
 * drivers/e1000/e1000.c): write START | word<<8 to EERD, poll DONE
 * (bit 4) -- the guest budgets 100,000 polls per word and falls back
 * to RAL/RAH on timeout -- then take the data from bits 31:16. */
static uint16_t eerd_read(fx_t *f, unsigned word, int *polls) {
    wr(f, 0x0014, 1u | (word << 8));                     /* EERD START */
    uint32_t v = 0;
    int n = 0;
    do { v = rd(f, 0x0014); n++; } while (!(v & (1u << 4)) && n < 100000);
    if (polls) *polls = n;
    assert(v & (1u << 4));                               /* DONE arrived */
    return (uint16_t)(v >> 16);
}

static void test_eeprom_eerd(void) {
    fx_t f; fx_init(&f);
    /* The north-star probe: words 0-2 must yield the same MAC the
     * RAL/RAH fallback used to provide, DONE on the first poll. */
    static const uint8_t mac[6] = { 0x52, 0x54, 0x00, 0x12, 0x34, 0x56 };
    for (int i = 0; i < 3; i++) {
        int polls = 0;
        uint16_t w = eerd_read(&f, (unsigned)i, &polls);
        assert(polls == 1);                              /* no timeout walk */
        assert((w & 0xFF) == mac[2 * i] && (w >> 8) == mac[2 * i + 1]);
    }
    /* ... byte for byte the RAL/RAH preload the guest used to settle for. */
    assert(rd(&f, 0x5400) == 0x12005452u);
    assert(rd(&f, 0x5404) == 0x80005634u);
    /* Identity words (QEMU template + prepare): subsystem / device ids. */
    assert(eerd_read(&f, 0x0A, 0) == 0x6403);
    assert(eerd_read(&f, 0x0B, 0) == 0x100E);            /* subsystem device */
    assert(eerd_read(&f, 0x0C, 0) == 0x8086);            /* subsystem vendor */
    assert(eerd_read(&f, 0x0D, 0) == 0x100E);            /* device id */
    assert(eerd_read(&f, 0x0E, 0) == 0x8086);            /* vendor id */
    /* Word 0x3F closes the image: all 64 words sum to 0xBABA. */
    uint16_t sum = 0;
    for (unsigned i = 0; i < 64; i++)
        sum = (uint16_t)(sum + eerd_read(&f, i, 0));
    assert(sum == 0xBABA);
    /* START clear: the stored command reads back, nothing completes. */
    wr(&f, 0x0014, (2u << 8));
    assert(rd(&f, 0x0014) == (2u << 8));
    /* Out of range completes with the written (zero) data field: QEMU's
     * cap is the checksum word 0x3F, not the 8-bit address field. */
    assert(eerd_read(&f, 0x40, 0) == 0);
    assert(eerd_read(&f, 0xFF, 0) == 0);
    /* CTRL.RST does not rebuild the EEPROM (QEMU prepares it at add). */
    wr(&f, 0x0000, (1u << 26));
    assert(eerd_read(&f, 0, 0) == 0x5452);
    fx_done(&f);
    printf("ok e1000 EEPROM via EERD (MAC words, identity, 0xBABA checksum, first-poll DONE)\n");
}

/* ============================= U2: PHY ==================================== */
#define MDIC_CMD(op, phy, reg, data) \
    (((uint32_t)(op) << 26) | ((uint32_t)(phy) << 21) | \
     ((uint32_t)(reg) << 16) | (uint32_t)(uint16_t)(data))

static uint16_t mdic_read(fx_t *f, unsigned reg) {
    wr(f, 0x0020, MDIC_CMD(2, 1, reg, 0));               /* OP=read, PHY 1 */
    uint32_t v = rd(f, 0x0020);
    assert(v & (1u << 28));                              /* READY */
    assert(!(v & (1u << 30)));                           /* no ERROR */
    return (uint16_t)v;
}
static void mdic_write(fx_t *f, unsigned reg, uint16_t data) {
    wr(f, 0x0020, MDIC_CMD(1, 1, reg, data));            /* OP=write, PHY 1 */
    uint32_t v = rd(f, 0x0020);
    assert(v & (1u << 28));
    assert(!(v & (1u << 30)));
}

static void test_phy_mdic(void) {
    fx_t f; fx_init(&f);
    const uint32_t line_bit = 1u << 3;                   /* ISA IRQ11, slave */
    /* Identity and the autonegotiation-done steady state (QEMU
     * phy_reg_init + the values its link-up path settles into). */
    assert(mdic_read(&f, 2) == 0x0141);                  /* PHYID1 */
    assert(mdic_read(&f, 3) == 0x0C20);                  /* PHYID2, 82540EM */
    assert(mdic_read(&f, 0) == 0x1140);                  /* BMCR 1000/FD/AUTOEN */
    assert(mdic_read(&f, 1) == 0x796D);                  /* BMSR link+AN done */
    assert(mdic_read(&f, 4) == 0x0DE1);                  /* ANAR */
    assert(mdic_read(&f, 5) == 0x41E0);                  /* ANLPAR + ACK */
    assert(mdic_read(&f, 9) == 0x0E00);                  /* CTRL1000 */
    assert(mdic_read(&f, 10) == 0x3C00);                 /* STAT1000 */
    assert(mdic_read(&f, 16) == 0x0360);                 /* spec ctrl */
    assert(mdic_read(&f, 17) == 0xAC00);                 /* spec status */
    assert(mdic_read(&f, 20) == 0x0D60);                 /* ext spec ctrl */
    /* RW registers round-trip. */
    mdic_write(&f, 4, 0x05E1);
    assert(mdic_read(&f, 4) == 0x05E1);
    mdic_write(&f, 16, 0x0260);
    assert(mdic_read(&f, 16) == 0x0260);
    /* OP=0 is not a transaction: READY, no ERROR, data stored verbatim,
     * no register touched.  OP=3 reads (QEMU tests the read bit first). */
    wr(&f, 0x0020, MDIC_CMD(0, 1, 4, 0xBEEF));
    uint32_t v = rd(&f, 0x0020);
    assert((v & (1u << 28)) && !(v & (1u << 30)) && (v & 0xFFFF) == 0xBEEF);
    assert(mdic_read(&f, 4) == 0x05E1);
    wr(&f, 0x0020, MDIC_CMD(3, 1, 4, 0));
    assert((rd(&f, 0x0020) & 0xFFFF) == 0x05E1);         /* read won */
    /* A write to a read-only register: ERROR+READY, value unchanged. */
    wr(&f, 0x0020, MDIC_CMD(1, 1, 1, 0xFFFF));
    v = rd(&f, 0x0020);
    assert((v & (1u << 30)) && (v & (1u << 28)));
    assert(mdic_read(&f, 1) == 0x796D);
    /* Outside the capability table: ERROR on read and on write. */
    wr(&f, 0x0020, MDIC_CMD(2, 1, 7, 0));
    assert(rd(&f, 0x0020) & (1u << 30));
    wr(&f, 0x0020, MDIC_CMD(1, 1, 31, 0x1234));
    assert(rd(&f, 0x0020) & (1u << 30));
    /* The wrong PHY address echoes the PREVIOUS MDIC value with ERROR
     * (QEMU's set_mdic), not the rejected command. */
    uint32_t prev = rd(&f, 0x0020);
    wr(&f, 0x0020, MDIC_CMD(2, 2, 2, 0));
    assert(rd(&f, 0x0020) == (prev | (1u << 30) | (1u << 28)));
    /* BMCR stores through set_phy_ctrl's mask: reserved 5:0 + RESET +
     * ANRESTART never stick, and the file is NOT reset by BMCR.RESET. */
    mdic_write(&f, 0, 0xFFFF);
    assert(mdic_read(&f, 0) == 0x7DC0);
    assert(mdic_read(&f, 4) == 0x05E1);                  /* ANAR untouched */
    assert(mdic_read(&f, 2) == 0x0141);                  /* identity untouched */
    /* INT_EN raises the MDAC cause onto the INTx wire. */
    assert(!(f.m.pic.slave.lines & line_bit));
    (void)rd(&f, 0x00C0);                                /* drain ICR */
    wr(&f, 0x00D0, (1u << 9));                           /* IMS.MDAC */
    wr(&f, 0x0020, MDIC_CMD(2, 1, 2, 0) | (1u << 29));   /* read + INT_EN */
    assert(rd(&f, 0x0020) & (1u << 28));
    assert(f.m.pic.slave.lines & line_bit);              /* pin high */
    assert(rd(&f, 0x00C0) == (1u << 9));                 /* ICR.MDAC, clears */
    assert(!(f.m.pic.slave.lines & line_bit));           /* pin dropped */
    /* Device reset restores the PHY template; both MAC paths consistent. */
    wr(&f, 0x0000, (1u << 26));
    assert(mdic_read(&f, 0) == 0x1140);
    assert(mdic_read(&f, 4) == 0x0DE1);
    assert(eerd_read(&f, 0, 0) == 0x5452);               /* EEPROM intact */
    assert(rd(&f, 0x5400) == 0x12005452u);               /* RAL intact */
    fx_done(&f);
    printf("ok e1000 PHY via MDIC (ID/status, RW, ERROR paths, BMCR mask, MDAC wire, reset)\n");
}

int main(void) {
    test_pci_reset_contract();
    test_rx_ingress();
    test_tx_doorbell_completion();
    test_tx_tctl_gate_and_doorbell();
    test_tx_multidescriptor_and_wrap();
    test_interrupt_register_semantics();
    test_intx_line();
    test_tx_link_gate();
    test_eeprom_eerd();
    test_phy_mdic();
    printf("test-net: all tests passed\n");
    return 0;
}
