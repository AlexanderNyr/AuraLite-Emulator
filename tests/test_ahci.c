/* tests/test_ahci.c -- STORE S1 vectors: AHCI ABAR register model.
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

/* 1 MiB synthetic disk image attached to the fixture. */
static uint8_t *gx_disk;
static void fx_attach_disk(fx_t *f, int port, size_t bytes) {
    if (!gx_disk) { gx_disk = calloc(1, 4 * 1024 * 1024); }
    assert(port >= 0 && port < AHCI_MAX_ATTACH && bytes <= 4 * 1024 * 1024);
    f->m.sata_img[port] = gx_disk;
    f->m.sata_len[port] = bytes;
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
    printf("test_ahci: ALL PASS\n");
    return 0;
}

