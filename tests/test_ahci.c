/* tests/test_ahci.c -- STORE S1-S5 vectors: AHCI ABAR register model,
 * command engine, breadth matrix machine half, large transfers,
 * writethrough and the honest INTx line.
 *
 * Baseline before S1 (measured 2026-10-07 on b00c4d9): the PCI function
 * 0:31.2 existed but BAR5 was empty; the guest printed "controller 0:
 * BAR5 empty, skipping" and every storage mount stayed offline.  The
 * register semantics asserted here are the guest driver's exact contract
 * (AuraLite-OS/drivers/ahci/ahci.c): CAP.NP field, GHC.AE readback, PI
 * mask, VS, PxSSTS.DET (3 = attached, 0 = dark), PxSIG == 0x101 for SATA
 * disks, PxCLB/PxFB store, PxCMD ST/FRE mirrored into CR/FR fast enough
 * for the stop/start spin loops, PxIS/PxSERR W1C, PxSCTL COMRESET pulses,
 * PxTFD with BSY|DRQ clear on an attached disk, and a latched PxCI (the
 * command engine itself is S2).
 */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "machine.h"
#include "ahci.h"
#include "harness.h"

#define ABAR AHCI_ABAR
#define P(p, sub) (ABAR + 0x100u + (p) * 0x80u + (sub))

typedef struct { machine_t m; } fx_t;

static void fx_init(fx_t *f) { setup_machine(&f->m); ahci_register(&f->m); }

/* LeakSanitizer-clean teardown: our regions are registered non-owned, so
 * the fixture frees the mmio list itself (rw2-owned regions only exist via
 * devices_init_common, which these tests never call). */
static void fx_done(fx_t *f) {
    mmio_region_t *r = f->m.mmio_list;
    while (r) { mmio_region_t *n = r->next; free(r); r = n; }
    f->m.mmio_list = NULL;
}

/* 1 MiB synthetic disk image attached to the fixture.  S4: attach rows are
 * per controller -- the historical helper pins controller 0; the S4
 * vectors use fx_attach_disk_c with a second image on controller 1. */
static uint8_t *gx_disk;
static uint8_t *gx_disk2;
static void fx_attach_disk_c(fx_t *f, int ctrl, int port, size_t bytes, uint8_t *img) {
    if (!img) { img = calloc(1, 4 * 1024 * 1024); }
    assert(port >= 0 && port < AHCI_MAX_ATTACH && bytes <= 4 * 1024 * 1024);
    f->m.sata_img[ctrl][port] = img;
    f->m.sata_len[ctrl][port] = bytes;
}
static void fx_attach_disk(fx_t *f, int port, size_t bytes) {
    if (!gx_disk) gx_disk = calloc(1, 4 * 1024 * 1024);
    fx_attach_disk_c(f, 0, port, bytes, gx_disk);
}

static void test_global_regs(void) {
    fx_t f; fx_init(&f);
    assert(mem_read(&f.m, ABAR + 0x00, 4) == 0x80040005u); /* CAP: S64A|SAM|NP=6 */
    assert(mem_read(&f.m, ABAR + 0x0C, 4) == 0x3Fu);       /* PI: ports 0..5 */
    assert(mem_read(&f.m, ABAR + 0x10, 4) == 0x00010300u); /* VS: 1.3 */
    assert(mem_read(&f.m, ABAR + 0x04, 4) == 0u);          /* GHC: reset 0 */
    mem_write(&f.m, ABAR + 0x04, 4, 0x80000003u);          /* AE|IE|HR bits */
    assert(mem_read(&f.m, ABAR + 0x04, 4) == 0x80000003u); /* guest's GHC readback */
    fx_done(&f);
    printf("ok global regs\n");
}

static void test_dark_ports(void) {
    fx_t f; fx_init(&f);
    for (int p = 0; p < 6; p++) {
        assert((mem_read(&f.m, P(p, 0x28), 4) & 0x0Fu) == 0u);   /* DET=0: nothing attached */
        assert(mem_read(&f.m, P(p, 0x24), 4) == 0xFFFFFFFFu);    /* SIG dark */
        assert(mem_read(&f.m, P(p, 0x20), 4) == 0x7Fu);          /* dark-port task file */
    }
    fx_done(&f);
    fx_done(&f);
    printf("ok dark ports\n");
}

static void test_attached_port(void) {
    fx_t f; fx_init(&f);
    fx_attach_disk(&f, 0, 1024 * 1024);
    assert((mem_read(&f.m, P(0, 0x28), 4) & 0x0Fu) == 3u);       /* DET=3 */
    assert((mem_read(&f.m, P(0, 0x28), 4) >> 4 & 0x0Fu) == 2u);  /* SPD nibble Gen2 */
    assert(mem_read(&f.m, P(0, 0x24), 4) == 0x00000101u);        /* guest's SATA_SIG_ATA test */
    assert((mem_read(&f.m, P(0, 0x20), 4) & 0x88u) == 0u);       /* BSY|DRQ clear: guest may issue */
    assert((mem_read(&f.m, P(0, 0x20), 4) & 1u) == 0u);          /* no error */
    /* neighbour port stays dark */
    assert((mem_read(&f.m, P(1, 0x28), 4) & 0x0Fu) == 0u);
    fx_done(&f);
    fx_done(&f);
    printf("ok attached port\n");
}

static void test_cmd_stop_start_handshake(void) {
    fx_t f; fx_init(&f);
    /* guest port_stop(): clear ST, spin while CR; clear FRE, spin while FR.
     * Immediate mirroring makes those spins exit at once. */
    mem_write(&f.m, P(0, 0x18), 4, 0x11u);         /* ST|FRE */
    assert((mem_read(&f.m, P(0, 0x18), 4) & 0x8011u) == 0x8011u); /* CR|FR|ST|FRE */
    mem_write(&f.m, P(0, 0x18), 4, 0x00u);         /* engine off */
    assert((mem_read(&f.m, P(0, 0x18), 4) & 0x8011u) == 0u);
    /* guest port_start(): waits CR==0, then sets FRE then ST. */
    mem_write(&f.m, P(0, 0x18), 4, 0x10u);
    assert((mem_read(&f.m, P(0, 0x18), 4) & (1u << 14)) != 0u);   /* FR mirrors FRE */
    mem_write(&f.m, P(0, 0x18), 4, 0x11u);
    assert((mem_read(&f.m, P(0, 0x18), 4) & (1u << 15)) != 0u);   /* CR mirrors ST */
    fx_done(&f);
    printf("ok cmd handshake\n");
}

static void test_dma_bases_store(void) {
    fx_t f; fx_init(&f);
    mem_write(&f.m, P(0, 0x00), 4, 0x12345000u);
    mem_write(&f.m, P(0, 0x04), 4, 0x9u);
    mem_write(&f.m, P(0, 0x08), 4, 0x12346000u);
    mem_write(&f.m, P(0, 0x0C), 4, 0x8u);
    assert(mem_read(&f.m, P(0, 0x00), 4) == 0x12345000u);
    assert(mem_read(&f.m, P(0, 0x04), 4) == 0x9u);
    assert(mem_read(&f.m, P(0, 0x08), 4) == 0x12346000u);
    assert(mem_read(&f.m, P(0, 0x0C), 4) == 0x8u);
    fx_done(&f);
    fx_done(&f);
    printf("ok dma bases store\n");
}

static void test_w1c_regs(void) {
    fx_t f; fx_init(&f);
    /* W1C: writing all-ones to a zeroed reg is a no-op... */
    mem_write(&f.m, P(0, 0x10), 4, 0xFFFFFFFFu);
    assert(mem_read(&f.m, P(0, 0x10), 4) == 0u);
    mem_write(&f.m, P(0, 0x30), 4, 0xFFFFFFFFu);
    assert(mem_read(&f.m, P(0, 0x30), 4) == 0u);
    /* ...and latched bits clear only where the write has 1s.  (No engine
     * yet, so nothing sets PxIS/PxSERR here; FFFF-then-check is the
     * receipt.) */
    mem_write(&f.m, ABAR + 0x08, 4, 0xFFFFFFFFu);  /* HBA IS W1C */
    assert(mem_read(&f.m, ABAR + 0x08, 4) == 0u);
    fx_done(&f);
    printf("ok w1c\n");
}

static void test_comreset_pulse(void) {
    fx_t f; fx_init(&f);
    fx_attach_disk(&f, 2, 512 * 1024);
    mem_write(&f.m, P(2, 0x2C), 4, 0x1u);   /* DET=1: initiate COMRESET */
    assert(mem_read(&f.m, P(2, 0x2C), 4) == 1u);
    /* attached disk renegotiates instantly in this model */
    assert((mem_read(&f.m, P(2, 0x28), 4) & 0x0Fu) == 3u);
    mem_write(&f.m, P(2, 0x2C), 4, 0x0u);   /* DET=0: release */
    assert((mem_read(&f.m, P(2, 0x28), 4) & 0x0Fu) == 3u);
    /* W1C of PxSERR after COMRESET (guest's sequence) */
    mem_write(&f.m, P(2, 0x30), 4, 0xFFFFFFFFu);
    assert(mem_read(&f.m, P(2, 0x30), 4) == 0u);
    fx_done(&f);
    printf("ok comreset\n");
}

static void test_pxci_latches(void) {
    fx_t f; fx_init(&f);
    fx_attach_disk(&f, 0, 1024 * 1024);
    /* S2: PxCI now triggers the engine synchronously; with no command list
     * programmed the header at CLB 0 fails the CFL check and the error
     * channel is TFES -- the slot still clears immediately. */
    mem_write(&f.m, P(0, 0x38), 4, 0x1u);
    assert((mem_read(&f.m, P(0, 0x38), 4) & 1u) == 0u);
    assert((mem_read(&f.m, P(0, 0x10), 4) & (1u << 30)) != 0u);
    fx_done(&f);
    printf("ok pxci latch\n");
}

/* ---- S2: command engine vectors ----
 *
 * Build a guest command list + table in harness RAM the exact way the
 * guest driver does (gas: hdr dw0 = CFL5 | PRDTL<<16, H2D Register FIS,
 * single/multi PRDT) and verify byte-identical DMA against the attached
 * image.  Guest-physical addresses stay in low RAM. */
#define T_CLB  0x20000u
#define T_CT   0x21000u
#define T_BUF1 0x30000u
#define T_BUF2 0x31000u
#define T_BUF3 0x32000u



static void t_hdr(machine_t *m, int prdtl, int wr) {
    for (int i = 0; i < 8; i++) mem_write(m, T_CLB + i * 4, 4, 0);
    mem_write(m, T_CLB, 4, 5u | (wr ? (1u << 6) : 0u) | ((uint32_t)prdtl << 16));
    mem_write(m, T_CLB + 8, 4, T_CT);
}

static void t_fis(machine_t *m, uint8_t cmd, uint64_t lba, uint16_t count) {
    for (int i = 0; i < 64; i++) mem_write(m, T_CT + i, 1, 0);
    mem_write(m, T_CT + 0, 1, 0x27); mem_write(m, T_CT + 1, 1, 0x80);
    mem_write(m, T_CT + 2, 1, cmd);  mem_write(m, T_CT + 7, 1, 0x40);
    mem_write(m, T_CT + 4, 1, lba & 0xFF);        mem_write(m, T_CT + 5, 1, (lba >> 8) & 0xFF);
    mem_write(m, T_CT + 6, 1, (lba >> 16) & 0xFF); mem_write(m, T_CT + 8, 1, (lba >> 24) & 0xFF);
    mem_write(m, T_CT + 12, 1, count & 0xFF);     mem_write(m, T_CT + 13, 1, count >> 8);
}

static void t_prdt(machine_t *m, int i, uint64_t dba, uint32_t bytes) {
    mem_write(m, T_CT + 0x80 + i * 16, 4, (uint32_t)dba);
    mem_write(m, T_CT + 0x84 + i * 16, 4, (uint32_t)(dba >> 32));
    mem_write(m, T_CT + 0x88 + i * 16, 4, 0);
    mem_write(m, T_CT + 0x8C + i * 16, 4, (bytes - 1) & 0x003FFFFF);
}

static void test_engine_read_single(void) {
    fx_t f; fx_init(&f);
    fx_attach_disk(&f, 0, 4 * 1024 * 1024);
    for (size_t i = 0; i < 4u * 1024 * 1024; i++) gx_disk[i] = (uint8_t)(i * 7 + 5);
    mem_write(&f.m, P(0, 0x00), 4, T_CLB);  /* PxCLB */
    t_hdr(&f.m, 1, 0);
    t_fis(&f.m, 0x25, 2, 2);                /* READ DMA EXT lba=2 count=2 */
    t_prdt(&f.m, 0, T_BUF1, 1024);
    for (int i = 0; i < 1024; i++) mem_write(&f.m, T_BUF1 + i, 1, 0xEE);
    mem_write(&f.m, P(0, 0x38), 4, 1);      /* PxCI=1 */
    assert((mem_read(&f.m, P(0, 0x38), 4) & 1u) == 0u);   /* cleared */
    assert(mem_read(&f.m, T_CLB + 4, 4) == 1024);         /* prdbc */
    assert((mem_read(&f.m, P(0, 0x20), 4) & 1u) == 0u);   /* no ERR */
    assert((mem_read(&f.m, P(0, 0x10), 4) & (1u << 30)) == 0u); /* no TFES */
    int bad = 0;
    for (int i = 0; i < 1024; i++)
        if (mem_read(&f.m, T_BUF1 + i, 1) != gx_disk[2 * 512 + i]) { bad = i + 1; break; }
    assert(bad == 0);                                    /* golden bytes */
    fx_done(&f);
    printf("ok engine read single\n");
}

static void test_engine_read_multi_prdt(void) {
    fx_t f; fx_init(&f);
    fx_attach_disk(&f, 0, 4 * 1024 * 1024);
    for (size_t i = 0; i < 4u * 1024 * 1024; i++) gx_disk[i] = (uint8_t)(i * 7 + 5);
    mem_write(&f.m, P(0, 0x00), 4, T_CLB);
    t_hdr(&f.m, 2, 0);
    t_fis(&f.m, 0x25, 4, 2);                            /* 1024 bytes split 768+256 */
    t_prdt(&f.m, 0, T_BUF1, 768);
    t_prdt(&f.m, 1, T_BUF2, 256);
    mem_write(&f.m, P(0, 0x38), 4, 1);
    assert((mem_read(&f.m, P(0, 0x38), 4) & 1u) == 0u);
    assert(mem_read(&f.m, T_CLB + 4, 4) == 1024);
    int bad = 0;
    for (int i = 0; i < 768; i++)
        if (mem_read(&f.m, T_BUF1 + i, 1) != gx_disk[4 * 512 + i])       { bad = 1; break; }
    for (int i = 0; i < 256; i++)
        if (mem_read(&f.m, T_BUF2 + i, 1) != gx_disk[4 * 512 + 768 + i]) { bad = 2; break; }
    assert(bad == 0);
    fx_done(&f);
    printf("ok engine read multi-prdt\n");
}

static void test_engine_identify(void) {
    fx_t f; fx_init(&f);
    fx_attach_disk(&f, 0, 4 * 1024 * 1024);
    mem_write(&f.m, P(0, 0x00), 4, T_CLB);
    t_hdr(&f.m, 1, 0);
    t_fis(&f.m, 0xEC, 0, 0);                            /* IDENTIFY: lba/count ignored */
    t_prdt(&f.m, 0, T_BUF3, 512);
    mem_write(&f.m, P(0, 0x38), 4, 1);
    assert((mem_read(&f.m, P(0, 0x38), 4) & 1u) == 0u);
    assert(mem_read(&f.m, T_CLB + 4, 4) == 512);
    assert(mem_read(&f.m, T_BUF3, 1) == 0x40);          /* word0 non-removable */
    assert(mem_read(&f.m, T_BUF3 + 54, 1) == 'A');      /* model pair-swapped */
    assert(mem_read(&f.m, T_BUF3 + 55, 1) == 'u');
    uint32_t secs28 = (uint32_t)mem_read(&f.m, T_BUF3 + 60 * 2, 4);
    assert(secs28 == 4u * 1024 * 1024 / 512);           /* LBA28 capacity */
    fx_done(&f);
    printf("ok engine identify\n");
}

static void test_engine_read_oob_tfes(void) {
    fx_t f; fx_init(&f);
    fx_attach_disk(&f, 0, 4 * 1024 * 1024); /* 8192 sectors */
    mem_write(&f.m, P(0, 0x00), 4, T_CLB);
    t_hdr(&f.m, 1, 0);
    t_fis(&f.m, 0x25, 8192, 1);                         /* past end */
    t_prdt(&f.m, 0, T_BUF1, 512);
    mem_write(&f.m, P(0, 0x38), 4, 1);
    assert((mem_read(&f.m, P(0, 0x38), 4) & 1u) == 0u); /* cleared even on error */
    assert((mem_read(&f.m, P(0, 0x10), 4) & (1u << 30)) != 0u); /* TFES */
    assert((mem_read(&f.m, P(0, 0x20), 4) & 1u) == 1u);         /* TFD.ERR */
    mem_write(&f.m, P(0, 0x10), 4, 0xFFFFFFFFu);        /* guest W1C clears */
    assert(mem_read(&f.m, P(0, 0x10), 4) == 0u);
    /* next issue clears sticky ERR (guest's own exec does this) */
    fx_done(&f);
    printf("ok engine read oob tfes\n");
}

static void test_engine_write_single_readback(void) {
    fx_t f; fx_init(&f);
    fx_attach_disk(&f, 0, 4 * 1024 * 1024);
    for (size_t i = 0; i < 4u * 1024 * 1024; i++) gx_disk[i] = 0xAA;
    for (int i = 0; i < 1024; i++) mem_write(&f.m, T_BUF1 + i, 1, (uint8_t)(i * 13 + 1));
    mem_write(&f.m, P(0, 0x00), 4, T_CLB);
    t_hdr(&f.m, 1, 1);
    t_fis(&f.m, 0x35, 5, 2);                            /* WRITE lba=5 2 sectors */
    t_prdt(&f.m, 0, T_BUF1, 1024);
    mem_write(&f.m, P(0, 0x38), 4, 1);
    assert((mem_read(&f.m, P(0, 0x38), 4) & 1u) == 0u);
    assert(mem_read(&f.m, T_CLB + 4, 4) == 1024);
    assert((mem_read(&f.m, P(0, 0x20), 4) & 1u) == 0u);
    int bad = 0;
    for (int i = 0; i < 1024; i++)
        if (gx_disk[5 * 512 + i] != (uint8_t)(i * 13 + 1)) { bad = i + 1; break; }
    assert(bad == 0);                                    /* guest RAM -> image */
    /* header W bit is authoritative: rebuild it for the read direction */
    t_hdr(&f.m, 1, 0);
    for (int i = 0; i < 1024; i++) mem_write(&f.m, T_BUF3 + i, 1, 0);
    t_fis(&f.m, 0x25, 5, 2);                            /* READ back */
    t_prdt(&f.m, 0, T_BUF3, 1024);
    mem_write(&f.m, P(0, 0x38), 4, 1);
    assert((mem_read(&f.m, P(0, 0x38), 4) & 1u) == 0u);
    for (int i = 0; i < 1024; i++)
        if (mem_read(&f.m, T_BUF3 + i, 1) != (uint8_t)(i * 13 + 1)) { bad = -1; break; }
    assert(bad == 0);                                    /* write->read roundtrip */
    fx_done(&f);
    printf("ok engine write single+readback\n");
}

static void test_engine_write_multi_prdt(void) {
    fx_t f; fx_init(&f);
    fx_attach_disk(&f, 0, 4 * 1024 * 1024);
    for (size_t i = 0; i < 4u * 1024 * 1024; i++) gx_disk[i] = 0;
    for (int i = 0; i < 512; i++) mem_write(&f.m, T_BUF1 + i, 1, 0x11u);
    for (int i = 0; i < 256; i++) mem_write(&f.m, T_BUF2 + i, 1, 0x22u);
    for (int i = 0; i < 256; i++) mem_write(&f.m, T_BUF3 + i, 1, 0x33u);
    mem_write(&f.m, P(0, 0x00), 4, T_CLB);
    t_hdr(&f.m, 3, 1);
    t_fis(&f.m, 0x35, 3, 2);                            /* 1024 B across 3 PRDTs */
    t_prdt(&f.m, 0, T_BUF1, 512);
    t_prdt(&f.m, 1, T_BUF2, 256);
    t_prdt(&f.m, 2, T_BUF3, 256);
    mem_write(&f.m, P(0, 0x38), 4, 1);
    assert((mem_read(&f.m, P(0, 0x38), 4) & 1u) == 0u);
    assert(mem_read(&f.m, T_CLB + 4, 4) == 1024);
    int ok = 1;
    for (int i = 0; i < 512; i++) if (gx_disk[3 * 512 + i] != 0x11) ok = 0;
    for (int i = 0; i < 256; i++) if (gx_disk[3 * 512 + 512 + i] != 0x22) ok = 0;
    for (int i = 0; i < 256; i++) if (gx_disk[3 * 512 + 768 + i] != 0x33) ok = 0;
    assert(ok);
    fx_done(&f);
    printf("ok engine write multi-prdt\n");
}

static void test_engine_write_oob_tfes(void) {
    fx_t f; fx_init(&f);
    fx_attach_disk(&f, 0, 4 * 1024 * 1024);
    for (size_t i = 0; i < 512; i++) gx_disk[8191 * 512 + i] = 0x7Eu;
    mem_write(&f.m, P(0, 0x00), 4, T_CLB);
    t_hdr(&f.m, 1, 1);
    t_fis(&f.m, 0x35, 8192, 1);                         /* OOB write */
    t_prdt(&f.m, 0, T_BUF1, 512);
    mem_write(&f.m, P(0, 0x38), 4, 1);
    assert((mem_read(&f.m, P(0, 0x38), 4) & 1u) == 0u);
    assert((mem_read(&f.m, P(0, 0x10), 4) & (1u << 30)) != 0u); /* TFES */
    assert((mem_read(&f.m, P(0, 0x20), 4) & 1u) == 1u);
    assert(gx_disk[8191 * 512] == 0x7E);                /* tails untouched */
    fx_done(&f);
    printf("ok engine write oob tfes\n");
}

/* ---- S4: breadth vectors (controller matrix, port placement) ----
 *
 * The guest's class scan is bus-0 dev/func ASCENDING (measured:
 * AuraLite-OS drivers/pci/pci.c pci_find_class_after), so controller
 * numbering follows slot order: onboard 0:31:2 is controller 0, the
 * --ahci2 function at 0:31:3 is controller 1.  These vectors pin the
 * machine half of the matrix lanes: the second ABAR window does not
 * alias the first, port placement is honoured by the engine, and the
 * two controllers' engines never cross images. */

#define ABAR2 AHCI_ABAR2
#define P2(p, sub) (ABAR2 + 0x100u + (p) * 0x80u + (sub))

static void test_ctrl1_window_independent(void) {
    fx_t f; fx_init(&f);
    ahci_register_ctrl(&f.m, 1);
    /* Controller 1 answers with its own identical HBA identity... */
    assert(mem_read(&f.m, ABAR2 + 0x00, 4) == 0x80040005u);
    assert(mem_read(&f.m, ABAR2 + 0x0C, 4) == 0x3Fu);
    assert(mem_read(&f.m, ABAR2 + 0x10, 4) == 0x00010300u);
    /* ...but its ports are independent: attach on ctrl0 p0 must NOT
     * light up ctrl1 p0 (no window aliasing between ABAR and ABAR2). */
    fx_attach_disk(&f, 0, 1024 * 1024);
    assert((mem_read(&f.m, P(0, 0x28), 4) & 0x0Fu) == 3u);        /* ctrl0 p0 present */
    assert((mem_read(&f.m, P2(0, 0x28), 4) & 0x0Fu) == 0u);       /* ctrl1 p0 dark */
    assert(mem_read(&f.m, P2(0, 0x24), 4) == 0xFFFFFFFFu);
    /* attach on ctrl1 p2 lights only that port */
    if (!gx_disk2) gx_disk2 = calloc(1, 4 * 1024 * 1024);
    fx_attach_disk_c(&f, 1, 2, 512 * 1024, gx_disk2);
    assert((mem_read(&f.m, P2(2, 0x28), 4) & 0x0Fu) == 3u);
    assert((mem_read(&f.m, P2(0, 0x28), 4) & 0x0Fu) == 0u);       /* sibling port stays dark */
    /* GHC state is per controller */
    mem_write(&f.m, ABAR + 0x04, 4, 0x80000001u);
    assert(mem_read(&f.m, ABAR2 + 0x04, 4) == 0u);
    fx_done(&f);
    printf("ok ctrl1 window independent\n");
}

static void test_port_placement_engine(void) {
    fx_t f; fx_init(&f);
    fx_attach_disk(&f, 3, 4 * 1024 * 1024);      /* disk on port 3 only */
    for (size_t i = 0; i < 4u * 1024 * 1024; i++) gx_disk[i] = (uint8_t)(i * 3 + 1);
    /* port 0 is dark: a command issued there must take the TFES path even
     * though the machine has a disk elsewhere (placement is per port). */
    mem_write(&f.m, P(0, 0x00), 4, T_CLB);
    t_hdr(&f.m, 1, 0);
    t_fis(&f.m, 0x25, 0, 1);
    t_prdt(&f.m, 0, T_BUF1, 512);
    mem_write(&f.m, P(0, 0x38), 4, 1);
    assert((mem_read(&f.m, P(0, 0x38), 4) & 1u) == 0u);
    assert((mem_read(&f.m, P(0, 0x10), 4) & (1u << 30)) != 0u);  /* TFES on dark port */
    /* the very same command on port 3 runs clean and returns its image */
    mem_write(&f.m, P(3, 0x10), 4, 0xFFFFFFFFu);  /* W1C any stray state on p3 */
    mem_write(&f.m, P(3, 0x00), 4, T_CLB);
    t_hdr(&f.m, 1, 0);
    t_fis(&f.m, 0x25, 0, 1);
    t_prdt(&f.m, 0, T_BUF1, 512);
    mem_write(&f.m, P(3, 0x38), 4, 1);
    assert((mem_read(&f.m, P(3, 0x38), 4) & 1u) == 0u);
    assert((mem_read(&f.m, P(3, 0x10), 4) & (1u << 30)) == 0u);  /* clean */
    int bad = 0;
    for (int i = 0; i < 512; i++)
        if (mem_read(&f.m, T_BUF1 + i, 1) != gx_disk[i]) { bad = i + 1; break; }
    assert(bad == 0);
    fx_done(&f);
    printf("ok port placement engine\n");
}

static void test_cross_controller_isolation(void) {
    fx_t f; fx_init(&f);
    ahci_register_ctrl(&f.m, 1);
    if (!gx_disk2) gx_disk2 = calloc(1, 4 * 1024 * 1024);
    fx_attach_disk(&f, 0, 4 * 1024 * 1024);                    /* ctrl0 p0 */
    fx_attach_disk_c(&f, 1, 0, 4 * 1024 * 1024, gx_disk2);     /* ctrl1 p0 */
    for (size_t i = 0; i < 4u * 1024 * 1024; i++) {
        gx_disk[i]  = 0xA0;                                    /* distinct patterns */
        gx_disk2[i] = 0xB1;
    }
    /* ctrl0 READ LBA0 -> gx_disk pattern */
    mem_write(&f.m, P(0, 0x00), 4, T_CLB);
    t_hdr(&f.m, 1, 0);
    t_fis(&f.m, 0x25, 0, 1);
    t_prdt(&f.m, 0, T_BUF1, 512);
    mem_write(&f.m, P(0, 0x38), 4, 1);
    assert((mem_read(&f.m, P(0, 0x10), 4) & (1u << 30)) == 0u);
    assert(mem_read(&f.m, T_BUF1, 1) == 0xA0);
    /* ctrl1 READ LBA0 -> gx_disk2 pattern (no bleed through the shared
     * command-list address: each controller has its own PxCLB slot) */
    mem_write(&f.m, P2(0, 0x00), 4, T_CLB);
    t_hdr(&f.m, 1, 0);
    t_fis(&f.m, 0x25, 0, 1);
    t_prdt(&f.m, 0, T_BUF2, 512);
    mem_write(&f.m, P2(0, 0x38), 4, 1);
    assert((mem_read(&f.m, P2(0, 0x10), 4) & (1u << 30)) == 0u);
    assert(mem_read(&f.m, T_BUF2, 1) == 0xB1);
    /* WRITE on ctrl1 must land in gx_disk2 and leave gx_disk pristine */
    for (int i = 0; i < 512; i++) mem_write(&f.m, T_BUF3 + i, 1, 0x5Au);
    mem_write(&f.m, P2(0, 0x00), 4, T_CLB);
    t_hdr(&f.m, 1, 1);
    t_fis(&f.m, 0x35, 2, 1);
    t_prdt(&f.m, 0, T_BUF3, 512);
    mem_write(&f.m, P2(0, 0x38), 4, 1);
    assert(gx_disk2[2 * 512] == 0x5Au);
    assert(gx_disk[2 * 512] == 0xA0u);
    fx_done(&f);
    printf("ok cross-controller isolation\n");
}

/* ---- S5: large transfers, writethrough, honest INTx ----
 *
 * 128 KiB is the guest driver's per-port DMA bounce ceiling (measured:
 * AuraLite-OS drivers/ahci/ahci.c bounce buffer; the STORE_PLAN S2
 * contract records "buf_len <= 4 MiB, guest hosts a 128-KiB per-port
 * bounce buffer as its largest transfer").  These vectors pin the full
 * 256-sector READ/WRITE DMA EXT shape the large-file lane exercises. */
#define T_BIG1 0x40000u
#define T_BIG2 0x50000u
#define T_BIGSZ (128u * 1024u)

static void test_engine_read_128k(void) {
    fx_t f; fx_init(&f);
    fx_attach_disk(&f, 0, 4 * 1024 * 1024);
    for (size_t i = 0; i < 4u * 1024 * 1024; i++) gx_disk[i] = (uint8_t)(i * 5 + 3);
    mem_write(&f.m, P(0, 0x00), 4, T_CLB);
    t_hdr(&f.m, 2, 0);
    t_fis(&f.m, 0x25, 16, 256);                 /* READ DMA EXT: 256 sectors */
    t_prdt(&f.m, 0, T_BIG1, T_BIGSZ / 2);       /* two 64 KiB halves */
    t_prdt(&f.m, 1, T_BIG2, T_BIGSZ / 2);
    mem_write(&f.m, P(0, 0x38), 4, 1);
    assert((mem_read(&f.m, P(0, 0x38), 4) & 1u) == 0u);
    assert(mem_read(&f.m, T_CLB + 4, 4) == T_BIGSZ);            /* prdbc */
    assert((mem_read(&f.m, P(0, 0x10), 4) & (1u << 30)) == 0u);
    int bad = 0;
    for (uint32_t i = 0; i < T_BIGSZ / 2; i++) {
        if (mem_read(&f.m, T_BIG1 + i, 1) != gx_disk[16 * 512 + i]) { bad = 1; break; }
        if (mem_read(&f.m, T_BIG2 + i, 1) != gx_disk[16 * 512 + T_BIGSZ / 2 + i]) { bad = 2; break; }
    }
    assert(bad == 0);
    fx_done(&f);
    printf("ok engine read 128k\n");
}

static void test_engine_write_128k(void) {
    fx_t f; fx_init(&f);
    fx_attach_disk(&f, 0, 4 * 1024 * 1024);
    for (size_t i = 0; i < 4u * 1024 * 1024; i++) gx_disk[i] = 0;
    for (uint32_t i = 0; i < T_BIGSZ / 2; i++) {
        mem_write(&f.m, T_BIG1 + i, 1, (uint8_t)(i * 7 + 1));
        mem_write(&f.m, T_BIG2 + i, 1, (uint8_t)(i * 11 + 2));
    }
    mem_write(&f.m, P(0, 0x00), 4, T_CLB);
    t_hdr(&f.m, 2, 1);
    t_fis(&f.m, 0x35, 32, 256);                 /* WRITE DMA EXT: 256 sectors */
    t_prdt(&f.m, 0, T_BIG1, T_BIGSZ / 2);
    t_prdt(&f.m, 1, T_BIG2, T_BIGSZ / 2);
    mem_write(&f.m, P(0, 0x38), 4, 1);
    assert((mem_read(&f.m, P(0, 0x38), 4) & 1u) == 0u);
    assert(mem_read(&f.m, T_CLB + 4, 4) == T_BIGSZ);
    int bad = 0;
    for (uint32_t i = 0; i < T_BIGSZ / 2; i++) {
        if (gx_disk[32 * 512 + i] != (uint8_t)(i * 7 + 1)) { bad = 1; break; }
        if (gx_disk[32 * 512 + T_BIGSZ / 2 + i] != (uint8_t)(i * 11 + 2)) { bad = 2; break; }
    }
    assert(bad == 0);
    fx_done(&f);
    printf("ok engine write 128k\n");
}

/* S5: --sata-writethrough -- a WRITE DMA EXT must reach the host file,
 * not only the in-memory image (the S3 policy kept the host pristine). */
static void test_writethrough(void) {
    fx_t f; fx_init(&f);
    fx_attach_disk(&f, 0, 1024 * 1024);
    const char *path = "test_ahci_wt.img";
    FILE *fp = fopen(path, "wb");
    assert(fp);
    assert(fwrite(gx_disk, 1, 1024 * 1024, fp) == 1024 * 1024);
    fclose(fp);
    assert(ahci_wt_attach(&f.m, 0, 0, path) == 0);
    for (int i = 0; i < 512; i++) mem_write(&f.m, T_BUF1 + i, 1, (uint8_t)(i * 3 + 7));
    mem_write(&f.m, P(0, 0x00), 4, T_CLB);
    t_hdr(&f.m, 1, 1);
    t_fis(&f.m, 0x35, 9, 1);                    /* WRITE LBA9 1 sector */
    t_prdt(&f.m, 0, T_BUF1, 512);
    mem_write(&f.m, P(0, 0x38), 4, 1);
    assert((mem_read(&f.m, P(0, 0x20), 4) & 1u) == 0u);
    ahci_wt_close_all();                         /* flush + close */
    fp = fopen(path, "rb");
    assert(fp);
    assert(fseek(fp, 9 * 512, SEEK_SET) == 0);
    int bad = 0;
    for (int i = 0; i < 512; i++) {
        int ch = fgetc(fp);
        if (ch != (uint8_t)(i * 3 + 7)) { bad = i + 1; break; }
    }
    fclose(fp);
    remove(path);
    assert(bad == 0);                            /* host file bytes moved */
    fx_done(&f);
    printf("ok writethrough\n");
}

/* S5: honest INTx.  (PxIS & PxIE) | (HBA IS & GHC.IE) drives the board
 * line AHCI_IRQ0+ctrl through pic_set_irq (which fans out to the I/O
 * APIC).  The guest's own driver polls with PxIE=0 and never sets
 * GHC.IE (measured) -- its receipts must be unchanged -- so this line is
 * machine honesty, pinned here. */
static void test_intx_line(void) {
    fx_t f; fx_init(&f);
    fx_attach_disk(&f, 0, 1024 * 1024);
    /* completion with PxIE=0 (guest contract): line must stay quiet */
    mem_write(&f.m, P(0, 0x00), 4, T_CLB);
    t_hdr(&f.m, 1, 0);
    t_fis(&f.m, 0x25, 0, 1);
    t_prdt(&f.m, 0, T_BUF1, 512);
    mem_write(&f.m, P(0, 0x38), 4, 1);
    assert(mem_read(&f.m, P(0, 0x10), 4) & 1u);          /* PxIS.DHRS latched */
    assert(!(f.m.pic.slave.lines & (1u << 6)));          /* IRQ14 quiet */
    assert(!(f.m.pic.slave.irr & (1u << 6)));
    /* enable PxIE.DHRS: the latched-but-masked interrupt asserts the line */
    mem_write(&f.m, P(0, 0x14), 4, 1u);
    assert(f.m.pic.slave.lines & (1u << 6));             /* asserted */
    assert(f.m.pic.slave.irr & (1u << 6));               /* edge latch into IRR */
    /* W1C PxIS drops the line again */
    mem_write(&f.m, P(0, 0x10), 4, 0xFFFFFFFFu);
    assert(!(f.m.pic.slave.lines & (1u << 6)));
    /* GHC.IE + HBA IS aggregate path: completion asserts with GHC.IE
     * alone (no PxIE) */
    mem_write(&f.m, P(0, 0x14), 4, 0u);
    mem_write(&f.m, ABAR + 0x04, 4, 2u);                 /* GHC.IE */
    mem_write(&f.m, P(0, 0x00), 4, T_CLB);
    t_hdr(&f.m, 1, 0);
    t_fis(&f.m, 0x25, 0, 1);
    t_prdt(&f.m, 0, T_BUF1, 512);
    mem_write(&f.m, P(0, 0x38), 4, 1);
    assert(mem_read(&f.m, ABAR + 0x08, 4) & 1u);         /* HBA IS bit0 */
    assert(f.m.pic.slave.lines & (1u << 6));
    /* level-mode guests see IRR follow the pin down (LTIM on the slave) */
    f.m.pic.slave.ltim = 1;
    mem_write(&f.m, ABAR + 0x04, 4, 0u);                 /* GHC.IE off -> drop */
    assert(!(f.m.pic.slave.lines & (1u << 6)));
    assert(!(f.m.pic.slave.irr & (1u << 6)));            /* level: IRR follows */
    fx_done(&f);
    printf("ok intx line\n");
}

int main(void) {
    test_global_regs();
    test_dark_ports();
    test_attached_port();
    test_cmd_stop_start_handshake();
    test_dma_bases_store();
    test_w1c_regs();
    test_comreset_pulse();
    test_pxci_latches();
    test_engine_read_single();
    test_engine_read_multi_prdt();
    test_engine_identify();
    test_engine_read_oob_tfes();
    test_engine_write_single_readback();
    test_engine_write_multi_prdt();
    test_engine_write_oob_tfes();
    test_ctrl1_window_independent();
    test_port_placement_engine();
    test_cross_controller_isolation();
    test_engine_read_128k();
    test_engine_write_128k();
    test_writethrough();
    test_intx_line();
    printf("test_ahci: ALL PASS\n");
    return 0;
}

