/* tests/test_net.c -- NET N0 vectors: the e1000 82540EM "RX-ready" model.
 *
 * Contract measured from AuraLite-OS@0ed0d29 (drivers/e1000/e1000.c,
 * kernel/arch/i386/net32.c) and QEMU's hw/net/e1000.c e1000_reset:
 *   - PCI 8086:100E, BAR0 128 KiB register file;
 *   - CTRL.RST (bit 26) self-clears after the register reset;
 *   - RAL/RAH preloaded with the QEMU default MAC 52:54:00:12:34:56
 *     (RAH.AV set) -- the guest prefers it over EEPROM;
 *   - RXCSUM reads 0 after reset (QEMU compat);
 *   - RX ring ingress: frame DMA into the head buffer, DD status, RDH
 *     advance, ICR.RXT0; RCTL.EN gates the receiver, RDH==RDT drops.
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

int main(void) {
    test_pci_reset_contract();
    test_rx_ingress();
    printf("test-net: all tests passed\n");
    return 0;
}
