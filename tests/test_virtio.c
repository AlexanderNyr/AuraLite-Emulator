/* tests/test_virtio.c -- VIRTIO V1 vectors: the modern virtio-pci
 * transport core (docs/plans/VIRTIO_PLAN.md, decisions D1-D4).
 *
 * The fixture is the full machine (devices_init_common) with a fake
 * echo device attached at the exact coordinates virtio-blk will use
 * (0:05.0, BAR 0xFEB30000, IRQ 5), so V2 inherits pinned placement.
 * PCI config goes through the real 0xCF8/0xCFC mechanism, MMIO
 * through mem_read/mem_write and rings live in guest RAM: the
 * vectors see precisely what the measured guests see -- the
 * by-the-book virtio-net flow (full negotiation, FEATURES_OK and
 * queue_enable readback) and the virtio-blk flow (no negotiation at
 * all, straight to DRIVER_OK -- finding N1, kept working by the D3
 * gate split).
 *
 * The echo device: offers device feature bit 0 and refuses
 * FEATURES_OK without it (validate_features), has two queues
 * (max 8 and 16) and an 8-byte device config; on notify it pops
 * chains and copies the out-stream onto the in-stream.
 */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "devices.h"
#include "harness.h"
#include "virtio.h"
#include "lapic.h"     /* V5: observe MSI-X delivery */

static int fails;
#define EXPECT(cond, ...) do { if (!(cond)) { printf("FAIL " __VA_ARGS__); printf("\n"); fails++; } } while (0)

static machine_t M;   /* static: machine_t is ~2.8MB */

#define VBAR        0xFEB30000u
#define CCD(off)    (VBAR + (off))            /* common cfg */
#define ISR_AT      (VBAR + 0x1000u)
#define DCFG(off)   (VBAR + 0x2000u + (off))
#define NOTIFY(q)   (VBAR + 0x3000u + (q) * 4u)
#define FAKE_BIT    (1ull << 0)

/* guest RAM testbed (ring pages, 2 MiB area, no firmware above) */
#define Q0_DESC   0x200000u
#define Q0_AVAIL  0x201000u
#define Q0_USED   0x202000u
#define Q1_DESC   0x210000u
#define Q1_AVAIL  0x211000u
#define Q1_USED   0x212000u
#define BUF0      0x220000u     /* 4 pages of descriptor buffers */
#define BUF1      0x221000u

static uint8_t  rd8 (uint32_t a) { return (uint8_t) mem_read(&M, a, 1); }
static uint16_t rd16(uint32_t a) { return (uint16_t)mem_read(&M, a, 2); }
static uint32_t rd32(uint32_t a) { return (uint32_t)mem_read(&M, a, 4); }
static uint64_t rd64(uint32_t a) { return mem_read(&M, a, 8); }
static void wr8 (uint32_t a, uint8_t v)  { mem_write(&M, a, 1, v); }
static void wr16(uint32_t a, uint16_t v) { mem_write(&M, a, 2, v); }
static void wr32(uint32_t a, uint32_t v) { mem_write(&M, a, 4, v); }
static void wr64(uint32_t a, uint64_t v) { mem_write(&M, a, 8, v); }

/* PCI mechanism #1 (test_xhci.c recipe), function 0:05.0 */
#define VDEV 5
static void pci_addr(int reg) {
    io_write(&M, 0xCF8, 4, 0x80000000u | ((uint32_t)VDEV << 11) | ((uint32_t)reg & 0xFCu));
}
static uint32_t pci_rd32(int reg) { pci_addr(reg); return (uint32_t)io_read(&M, 0xCFC, 4); }
static void pci_wr32(int reg, uint32_t v) { pci_addr(reg); io_write(&M, 0xCFC, 4, v); }
static uint8_t pci_rd8(int reg) { pci_addr(reg); return (uint8_t)io_read(&M, 0xCFC + (reg & 3), 1); }

/* ------------------------- the fake echo device ------------------------ */
typedef struct {
    virtio_dev_t vio;
    virtio_req_t req;          /* ~8 KiB scratch, out of the stack  */
    int notifies, validates, resets, cfgwrites;
    uint32_t cfgw_off, cfgw_val; int cfgw_size;
} fake_t;
static fake_t g_fake;

static const uint8_t g_fake_cfg[8] = { 0x10, 0x32, 0x54, 0x76, 0x98, 0xBA, 0xDC, 0xFE };

static void fake_notify(virtio_dev_t *v, int qi) {
    fake_t *f = v->ctx;
    f->notifies++;
    int rc;
    while ((rc = virtio_pop(v, qi, &f->req)) != 0) {
        if (rc < 0) continue;                       /* malformed: core ate it */
        uint8_t tmp[512];
        uint32_t n = virtio_req_read(&f->req, 0, tmp, sizeof tmp);
        uint32_t w = virtio_req_write(&f->req, 0, tmp, n);
        virtio_complete(v, &f->req, w);
    }
}
static int fake_validate(virtio_dev_t *v, uint64_t acked) {
    fake_t *f = v->ctx;
    f->validates++;
    return (acked & FAKE_BIT) ? 0 : -1;             /* bit 0 is mandatory here */
}
static void fake_reset(virtio_dev_t *v) { ((fake_t *)v->ctx)->resets++; }
static void fake_cfg_write(virtio_dev_t *v, uint32_t off, uint32_t val, int size) {
    fake_t *f = v->ctx;
    f->cfgwrites++;
    f->cfgw_off = off; f->cfgw_val = val; f->cfgw_size = size;
}
static const virtio_device_desc_t g_fake_desc = {
    .features = FAKE_BIT,
    .num_queues = 2,
    .queue_max = { 8, 16 },
    .config = g_fake_cfg,
    .config_len = sizeof g_fake_cfg,
    .notify = fake_notify,
    .validate_features = fake_validate,
    .reset = fake_reset,
    .config_write = fake_cfg_write,
};

static void fake_add(void) {
    virtio_pci_add(&M, &g_fake.vio, VIRTIO_BLK_PCI_DEV, "virtio-fake",
                   VIRTIO_PCI_BLK_ID, 0x01, 0x80, 0x00,
                   VIRTIO_BLK_BAR_BASE, VIRTIO_BLK_IRQ, &g_fake_desc, &g_fake);
}

/* ----------------------- guest-side helper rituals --------------------- */
static void dev_status(uint8_t st) { wr8(CCD(0x14), st); }

/* The virtio-net.c ritual: reset, ACK, DRIVER, exchange features,
 * FEATURES_OK (re-read!), queue dance, DRIVER_OK. */
static void full_init(void) {
    dev_status(0);
    dev_status(0x01);
    dev_status(0x01 | 0x02);
    wr32(CCD(0x08), 0); wr32(CCD(0x0C), 0x01);        /* ack bit 0 */
    wr32(CCD(0x08), 1); wr32(CCD(0x0C), 0x01);        /* ack VERSION_1 */
    dev_status(0x01 | 0x02 | 0x08);
}

/* The virtio_blk.c ritual verbatim: no feature traffic at all --
 * status = 0; = ACK|DRIVER; |= FEATURES_OK; |= DRIVER_OK.  The two
 * RMWs read back whatever the device decided about FEATURES_OK. */
static void blind_init(void) {
    dev_status(0);
    dev_status(1 | 2);
    dev_status((uint8_t)(rd8(CCD(0x14)) | 8));
    dev_status((uint8_t)(rd8(CCD(0x14)) | 4));
}

static void queue_setup(int qi, uint32_t desc, uint32_t avail, uint32_t used) {
    memset(M.ram + desc, 0, 0x3000);
    wr16(CCD(0x16), (uint16_t)qi);
    wr16(CCD(0x18), rd16(CCD(0x18)));                 /* take the offered size */
    wr64(CCD(0x20), desc);
    wr64(CCD(0x28), avail);
    wr64(CCD(0x30), used);
    wr16(CCD(0x1C), 1);
}

struct tdesc { uint64_t addr; uint32_t len; uint16_t flags, next; };
static void put_desc(uint32_t table, int i, uint64_t addr, uint32_t len,
                     uint16_t flags, uint16_t next) {
    struct tdesc d = { addr, len, flags, next };
    memcpy(M.ram + table + 16u * (uint32_t)i, &d, sizeof d);
}
static uint16_t used_idx(uint32_t used) {
    return (uint16_t)(M.ram[used + 2] | (M.ram[used + 3] << 8));
}
static void used_elem(uint32_t used, int slot, uint32_t *id, uint32_t *len) {
    memcpy(id,  M.ram + used + 4 + 8 * slot, 4);
    memcpy(len, M.ram + used + 8 + 8 * slot, 4);
}
static void avail_post(uint32_t avail, int slot, uint16_t head) {
    M.ram[avail + 4 + 2 * slot] = (uint8_t)head;
    M.ram[avail + 5 + 2 * slot] = (uint8_t)(head >> 8);
    uint16_t idx = (uint16_t)(M.ram[avail + 2] | (M.ram[avail + 3] << 8));
    idx++;
    M.ram[avail + 2] = (uint8_t)idx;
    M.ram[avail + 3] = (uint8_t)(idx >> 8);
}
static int irq5(void) { return (M.pic.master.lines & (1u << 5)) != 0; }

static int log_has(const char *needle) {
    for (int i = 0; i < M.log.count; i++)
        if (strstr(M.log.lines[i], needle)) return 1;
    return 0;
}

/* ------------------------------ vectors -------------------------------- */
static void v_pci_identity(void) {
    int f0 = fails;
    /* the guest probe: vendor 1AF4 anywhere on bus 0 finds 0:05.0 */
    int found = -1;
    for (int d = 0; d < 32 && found < 0; d++) {
        io_write(&M, 0xCF8, 4, 0x80000000u | ((uint32_t)d << 11));
        if ((io_read(&M, 0xCFC, 4) & 0xFFFFu) == 0x1AF4u) found = d;
    }
    EXPECT(found == 5, "1AF4 scan finds dev 5 (got %d)", found);
    EXPECT(pci_rd32(0x00) == 0x10421AF4u, "1af4:1042 (got %08x)", pci_rd32(0x00));
    EXPECT(pci_rd32(0x08) == 0x01800001u, "class 01/80/00 rev 1 (got %08x)", pci_rd32(0x08));
    EXPECT(pci_rd32(0x2C) == 0x10421AF4u, "subsystem 1af4:1042 (got %08x)", pci_rd32(0x2C));
    EXPECT(pci_rd8(0x3C) == 5, "interrupt line 5 (got %u)", pci_rd8(0x3C));
    EXPECT(pci_rd8(0x3D) == 1, "interrupt pin INTA#");
    EXPECT(pci_rd32(0x10) == VBAR, "BAR0 seeded (got %08x)", pci_rd32(0x10));
    EXPECT(pci_rd32(0x14) == 0 && pci_rd32(0x24) == 0 && pci_rd32(0x30) == 0,
           "unimplemented BARs read 0");
    if (fails == f0) printf("ok pci identity: 1af4:1042 at 0:05.0 IRQ5 INTA#\n");
}

static void v_caps(void) {
    int f0 = fails;
    EXPECT((pci_rd8(0x06) & 0x10) != 0, "status: capabilities list");
    uint8_t cap = pci_rd8(0x34) & 0xFC;
    EXPECT(cap == 0x40, "capabilities pointer 0x40 (got %02x)", cap);
    static const struct { uint8_t type, len; uint32_t off; } want[4] = {
        { 1, 16, 0x0000 }, { 2, 20, 0x3000 }, { 3, 16, 0x1000 }, { 4, 16, 0x2000 },
    };
    for (int i = 0; i < 4; i++) {
        EXPECT(cap != 0, "cap %d present", i);
        EXPECT(pci_rd8(cap) == 0x09, "cap %d vendor", i);
        EXPECT(pci_rd8(cap + 2) == want[i].len, "cap %d len %u (got %u)",
               i, want[i].len, pci_rd8(cap + 2));
        EXPECT(pci_rd8(cap + 3) == want[i].type, "cap %d type %u (got %u)",
               i, want[i].type, pci_rd8(cap + 3));
        EXPECT(pci_rd8(cap + 4) == 0, "cap %d in BAR0", i);
        EXPECT(pci_rd32(cap + 8) == want[i].off, "cap %d off 0x%x (got %08x)",
               i, want[i].off, pci_rd32(cap + 8));
        EXPECT(pci_rd32(cap + 12) == 0x1000u, "cap %d length 0x1000", i);
        if (want[i].type == 2)
            EXPECT(pci_rd32(cap + 16) == 4u, "notify multiplier 4 (got %u)",
                   pci_rd32(cap + 16));
        cap = pci_rd8(cap + 1) & 0xFC;
    }
    /* V5: the MSI-X capability terminates the chain */
    EXPECT(cap == 0x84, "MSI-X cap follows the vendor caps (got %02x)", cap);
    EXPECT(pci_rd8(cap) == 0x11, "capability id 0x11 (MSI-X)");
    EXPECT((pci_rd32(cap) >> 16) == 2u, "Message Control: table size 3, off");
    EXPECT(pci_rd32(cap + 4) == 0x3800u, "table at BAR0+0x3800 (BIR 0)");
    EXPECT(pci_rd32(cap + 8) == 0x3C00u, "PBA at BAR0+0x3C00 (BIR 0)");
    EXPECT(pci_rd8(cap + 1) == 0, "cap chain terminates at MSI-X");
    /* cfg writes cannot vandalise the RO capability bytes */
    pci_wr32(0x40, 0xDEADBEEFu);
    EXPECT(pci_rd8(0x40) == 0x09 && pci_rd8(0x43) == 1, "caps re-asserted after cfg write");
    pci_wr32(0x84, 0x00000000u);        /* size bits are RO too */
    EXPECT(pci_rd8(0x84) == 0x11 && (pci_rd32(0x84) >> 16) == 2u,
           "MSI-X cap RO fields re-asserted");
    if (fails == f0) printf("ok caps: common/notify(x4)/isr/device + MSI-X at 0x84\n");
}

static void v_bar_probe(void) {
    int f0 = fails;
    pci_wr32(0x10, 0xFFFFFFFFu);
    EXPECT(pci_rd32(0x10) == 0xFFFFC000u, "BAR0 probe mask 16 KiB (got %08x)", pci_rd32(0x10));
    pci_wr32(0x14, 0xFFFFFFFFu);
    EXPECT(pci_rd32(0x14) == 0, "BAR1 probe reads 0");
    pci_wr32(0x10, 0xFEB40000u);                       /* relocate 64 KiB up */
    EXPECT(pci_rd32(0x10) == 0xFEB40000u, "BAR0 relocated");
    EXPECT((uint16_t)mem_read(&M, 0xFEB40012u, 2) == 2, "num_queues decodes at the new base");
    pci_wr32(0x10, VBAR);                              /* restore for later vectors */
    EXPECT(rd16(CCD(0x12)) == 2, "num_queues back at the seeded base");
    if (fails == f0) printf("ok bar probe: mask + relocation + restore\n");
}

static void v_features(void) {
    int f0 = fails;
    dev_status(0);
    dev_status(1); dev_status(3);
    /* offer banks: bit0 (fake) low, VERSION_1 high, nothing else */
    wr32(CCD(0x00), 0); EXPECT(rd32(CCD(0x04)) == 0x1u, "device features low");
    wr32(CCD(0x00), 1); EXPECT(rd32(CCD(0x04)) == 0x1u, "device features high = VERSION_1");
    wr32(CCD(0x00), 2); EXPECT(rd32(CCD(0x04)) == 0u, "device features bank 2 = 0");
    /* acked word is masked to the offer even when the guest sprays 1s */
    wr32(CCD(0x08), 0); wr32(CCD(0x0C), 0xFFFFFFFFu);
    EXPECT(rd32(CCD(0x0C)) == 0x1u, "driver features masked low (got %08x)", rd32(CCD(0x0C)));
    wr32(CCD(0x08), 1); wr32(CCD(0x0C), 0xFFFFFFFFu);
    EXPECT(rd32(CCD(0x0C)) == 0x1u, "driver features masked high");
    /* FEATURES_OK gate, half 1: the device bit is acked (validator
     * satisfied) but VERSION_1 is not -> rejected by the VERSION_1
     * rule alone (mutation-discriminating: each gate kills this
     * vector only if the other is removed) */
    dev_status(0); dev_status(1); dev_status(3);
    wr32(CCD(0x08), 0); wr32(CCD(0x0C), 1);           /* ack bit 0 only */
    dev_status((uint8_t)(rd8(CCD(0x14)) | 8));
    EXPECT(!(rd8(CCD(0x14)) & 8), "FEATURES_OK cleared: no VERSION_1 acked (net shape)");
    /* half 2: VERSION_1 alone, validate_features demands bit 0 too */
    int v0 = g_fake.validates;
    wr32(CCD(0x08), 0); wr32(CCD(0x0C), 0);
    wr32(CCD(0x08), 1); wr32(CCD(0x0C), 1);
    dev_status((uint8_t)(rd8(CCD(0x14)) | 8));
    EXPECT(!(rd8(CCD(0x14)) & 8), "FEATURES_OK cleared: validate_features said no");
    EXPECT(g_fake.validates > v0, "validate_features was consulted");
    /* half 3: the legal set sticks */
    wr32(CCD(0x08), 0); wr32(CCD(0x0C), 1);
    dev_status((uint8_t)(rd8(CCD(0x14)) | 8));
    EXPECT((rd8(CCD(0x14)) & 8) != 0, "FEATURES_OK kept: VERSION_1 + device bit");
    dev_status(0);
    if (fails == f0) printf("ok features: banks, offer mask, FEATURES_OK gate (D3)\n");
}

static void v_queue_regs(void) {
    int f0 = fails;
    full_init();
    EXPECT(rd16(CCD(0x10)) == 0xFFFF, "msix_config reads NO_VECTOR at reset");
    EXPECT(rd8(CCD(0x15)) == 0, "config_generation still");
    wr16(CCD(0x16), 1);
    EXPECT(rd16(CCD(0x18)) == 16, "q1 size reads the max before any write");
    wr16(CCD(0x18), 99);
    EXPECT(rd16(CCD(0x18)) == 16, "q1 size write clamps to the max (99->16)");
    wr16(CCD(0x18), 8);
    EXPECT(rd16(CCD(0x18)) == 8, "q1 smaller size sticks (net driver writes 16 of 16)");
    wr16(CCD(0x18), 0);
    EXPECT(rd16(CCD(0x18)) == 16, "q1 size write 0 falls back to the max");
    EXPECT(rd16(CCD(0x1A)) == 0xFFFF, "queue_msix_vector NO_VECTOR at reset");
    EXPECT(rd16(CCD(0x1E)) == 1, "q1 notify_off = queue index");
    /* 64-bit ring pointers, guest shape (one 8-byte MMIO write)... */
    wr64(CCD(0x20), 0x210000);
    EXPECT(rd64(CCD(0x20)) == 0x210000u, "queue_desc 8-byte write");
    /* ...and as two 32-bit halves: the low write must keep the high */
    wr64(CCD(0x28), 0x100000000ull);
    wr32(CCD(0x28), 0x211000);
    EXPECT(rd64(CCD(0x28)) == 0x100211000ull, "queue_driver halves compose (got %llx)",
           (unsigned long long)rd64(CCD(0x28)));
    wr16(CCD(0x1C), 1);
    EXPECT(rd16(CCD(0x1C)) == 1, "queue_enable readback 1 (net driver checks!)");
    wr16(CCD(0x1C), 0);
    EXPECT(rd16(CCD(0x1C)) == 0, "queue_enable readback 0");
    /* a bogus queue_select reads the spec's empty hand and harms none */
    wr16(CCD(0x16), 7);
    EXPECT(rd16(CCD(0x18)) == 0 && rd16(CCD(0x1C)) == 0 && rd64(CCD(0x20)) == 0,
           "queue_select beyond num_queues reads 0");
    wr64(CCD(0x20), 0xDEAD0000);
    wr16(CCD(0x16), 1);
    EXPECT(rd64(CCD(0x20)) == 0x210000u, "bogus-select writes did not reach q1");
    dev_status(0);
    if (fails == f0) printf("ok queue regs: size clamp/stick, enable readback, halves, bogus select\n");
}

/* One PING -> PONG-sized echo over q0 with the caller's init ritual. */
static void echo_round(const char *tag) {
    memset(M.ram + Q0_DESC, 0, 0x3000);
    memset(M.ram + BUF0, 0, 0x2000);
    queue_setup(0, Q0_DESC, Q0_AVAIL, Q0_USED);
    memcpy(M.ram + BUF0, "PING", 4);
    put_desc(Q0_DESC, 0, BUF0,        4, VRING_DESC_F_NEXT, 1);
    put_desc(Q0_DESC, 1, BUF0 + 0x100, 4, VRING_DESC_F_WRITE, 0);
    int n0 = g_fake.notifies;
    avail_post(Q0_AVAIL, 0, 0);
    wr16(NOTIFY(0), 0);                                /* guest writes queue idx */
    EXPECT(g_fake.notifies == n0 + 1, "%s: notify reached the device", tag);
    EXPECT(used_idx(Q0_USED) == 1, "%s: used idx advanced synchronously", tag);
    uint32_t id, len;
    used_elem(Q0_USED, 0, &id, &len);
    EXPECT(id == 0 && len == 4, "%s: used elem {0,4} (got {%u,%u})", tag, id, len);
    EXPECT(memcmp(M.ram + BUF0 + 0x100, "PING", 4) == 0, "%s: bytes landed in the in-seg", tag);
    /* spill-over checks: the out-seg and its neighbours are untouched */
    EXPECT(memcmp(M.ram + BUF0, "PING", 4) == 0 && M.ram[BUF0 + 0x104] == 0,
           "%s: no stray writes", tag);
}

static void v_echo_net_shape(void) {
    int f0 = fails;
    full_init();
    dev_status((uint8_t)(rd8(CCD(0x14)) | 4));         /* DRIVER_OK */
    EXPECT((rd8(CCD(0x14)) & 0x0F) == 0x0F, "status ACK|DRIVER|FEATURES_OK|DRIVER_OK");
    echo_round("net-shape");
    /* completion interrupt: ISR byte set, line up; read drops both */
    EXPECT(irq5(), "INTx asserted after completion");
    EXPECT(rd8(ISR_AT) == 1, "ISR reads queue bit");
    EXPECT(!irq5() && rd8(ISR_AT) == 0, "ISR read cleared byte and line");
    dev_status(0);
    if (fails == f0) printf("ok echo (net shape): full negotiation, ISR/INTx read-ack\n");
}

static void v_echo_blk_shape(void) {
    int f0 = fails;
    blind_init();                                      /* virtio_blk.c, verbatim */
    /* FEATURES_OK was rejected (nothing acked) -- visible in the very
     * readback the guest driver never looks at (finding N1) ... */
    EXPECT((rd8(CCD(0x14)) & 0x0F) == (0x01 | 0x02 | 0x04),
           "blind init: FEATURES_OK rejected in readback (got %02x)", rd8(CCD(0x14)));
    /* ... and per D3 the engine still runs under DRIVER_OK, so the
     * guest's used-idx poll succeeds instead of timing out. */
    echo_round("blk-shape");
    dev_status(0);
    if (fails == f0) printf("ok echo (blk shape): lenient engine under rejected FEATURES_OK\n");
}

static void v_second_queue(void) {
    int f0 = fails;
    full_init();
    dev_status((uint8_t)(rd8(CCD(0x14)) | 4));
    memset(M.ram + BUF1, 0, 0x1000);
    queue_setup(1, Q1_DESC, Q1_AVAIL, Q1_USED);
    memcpy(M.ram + BUF1, "QUEUE1", 6);
    put_desc(Q1_DESC, 0, BUF1,        6, VRING_DESC_F_NEXT, 1);
    put_desc(Q1_DESC, 1, BUF1 + 0x80, 6, VRING_DESC_F_WRITE, 0);
    avail_post(Q1_AVAIL, 0, 0);
    wr16(NOTIFY(1), 1);                                /* q1's own doorbell */
    EXPECT(used_idx(Q1_USED) == 1, "q1 completed via notify_off*mult doorbell");
    EXPECT(memcmp(M.ram + BUF1 + 0x80, "QUEUE1", 6) == 0, "q1 payload echoed");
    EXPECT(used_idx(Q0_USED) == 0 || memcmp(M.ram + BUF0 + 0x100, "PING", 4) == 0,
           "q0 rings undisturbed by q1 traffic");
    dev_status(0);
    if (fails == f0) printf("ok second queue: independent doorbell per queue\n");
}

static void v_no_interrupt(void) {
    int f0 = fails;
    full_init();
    dev_status((uint8_t)(rd8(CCD(0x14)) | 4));
    memset(M.ram + Q0_DESC, 0, 0x3000);
    memset(M.ram + BUF0, 0, 0x2000);
    queue_setup(0, Q0_DESC, Q0_AVAIL, Q0_USED);
    M.ram[Q0_AVAIL] = VRING_AVAIL_F_NO_INTERRUPT;      /* avail flags */
    put_desc(Q0_DESC, 0, BUF0, 2, VRING_DESC_F_NEXT, 1);
    put_desc(Q0_DESC, 1, BUF0 + 0x100, 2, VRING_DESC_F_WRITE, 0);
    avail_post(Q0_AVAIL, 0, 0);
    wr16(NOTIFY(0), 0);
    EXPECT(used_idx(Q0_USED) == 1, "request still completed");
    EXPECT(rd8(ISR_AT) == 0 && !irq5(), "NO_INTERRUPT: no ISR, no line");
    dev_status(0);
    if (fails == f0) printf("ok no-interrupt: avail NO_INTERRUPT wins over completion\n");
}

static void v_malformed(void) {
    int f0 = fails;
    full_init();
    dev_status((uint8_t)(rd8(CCD(0x14)) | 4));
    memset(M.ram + Q0_DESC, 0, 0x3000);
    memset(M.ram + BUF0, 0, 0x2000);
    queue_setup(0, Q0_DESC, Q0_AVAIL, Q0_USED);
    uint16_t before = used_idx(Q0_USED);
    /* (a) head outside the table: dropped, NO used elem published */
    avail_post(Q0_AVAIL, 0, 500);
    wr16(NOTIFY(0), 0);
    EXPECT(used_idx(Q0_USED) == before, "head 500 dropped without a used elem");
    /* (b) self-looping chain: completed len 0, guest sees a failure */
    put_desc(Q0_DESC, 0, BUF0, 4, VRING_DESC_F_NEXT, 0);
    avail_post(Q0_AVAIL, 1, 0);
    wr16(NOTIFY(0), 0);
    uint32_t id, len;
    used_elem(Q0_USED, before, &id, &len);
    EXPECT(used_idx(Q0_USED) == before + 1 && len == 0, "loop chain completed len 0");
    /* (c) buffer spilling past RAM end: rejected, len 0 */
    put_desc(Q0_DESC, 0, RAM_SIZE - 2, 16, VRING_DESC_F_WRITE, 0);
    avail_post(Q0_AVAIL, 2, 0);
    wr16(NOTIFY(0), 0);
    used_elem(Q0_USED, before + 1, &id, &len);
    EXPECT(used_idx(Q0_USED) == before + 2 && len == 0, "RAM-spilling buffer completed len 0");
    /* (d) indirect flag (never offered): rejected, len 0 */
    put_desc(Q0_DESC, 0, BUF0, 16, VRING_DESC_F_INDIRECT, 0);
    avail_post(Q0_AVAIL, 3, 0);
    wr16(NOTIFY(0), 0);
    used_elem(Q0_USED, before + 2, &id, &len);
    EXPECT(used_idx(Q0_USED) == before + 3 && len == 0, "indirect chain completed len 0");
    EXPECT(log_has("bad chain"), "bad chains hit the device log");
    /* the ring still works after the garbage: last_avail stayed sane */
    memcpy(M.ram + BUF0, "OK", 2);
    put_desc(Q0_DESC, 0, BUF0, 2, VRING_DESC_F_NEXT, 1);
    put_desc(Q0_DESC, 1, BUF0 + 0x100, 2, VRING_DESC_F_WRITE, 0);
    avail_post(Q0_AVAIL, 4, 0);
    wr16(NOTIFY(0), 0);
    used_elem(Q0_USED, before + 3, &id, &len);
    EXPECT(used_idx(Q0_USED) == before + 4 && len == 2 &&
           memcmp(M.ram + BUF0 + 0x100, "OK", 2) == 0, "good chain after garbage");
    dev_status(0);
    if (fails == f0) printf("ok malformed: head/loop/spill/indirect all bounded (D2)\n");
}

static void v_reset(void) {
    int f0 = fails;
    blind_init();
    echo_round("pre-reset");
    int r0 = g_fake.resets;
    dev_status(0);
    EXPECT(rd8(CCD(0x14)) == 0, "status reads 0 immediately after reset write");
    EXPECT(g_fake.resets > r0, "device reset callback ran");
    wr16(CCD(0x16), 0);
    EXPECT(rd16(CCD(0x1C)) == 0, "queue_enable cleared by reset");
    EXPECT(rd64(CCD(0x20)) == 0 && rd64(CCD(0x30)) == 0, "ring addresses cleared");
    EXPECT(rd16(CCD(0x18)) == 8, "queue_size back to max");
    wr32(CCD(0x08), 0);
    EXPECT(rd32(CCD(0x0C)) == 0, "acked features cleared");
    /* a notify against the reset device is inert (DRIVER_OK gone) */
    uint16_t u = used_idx(Q0_USED);
    avail_post(Q0_AVAIL, 3, 0);
    wr16(NOTIFY(0), 0);
    EXPECT(used_idx(Q0_USED) == u, "post-reset notify processes nothing");
    (void)rd8(ISR_AT);
    EXPECT(!irq5(), "no dangling INTx after reset");
    if (fails == f0) printf("ok reset: synchronous, queues/features wiped, inert notify\n");
}

static void v_device_config(void) {
    int f0 = fails;
    for (int i = 0; i < 8; i++)
        EXPECT(rd8(DCFG(i)) == g_fake_cfg[i], "device cfg byte %d", i);
    uint64_t lo64;
    memcpy(&lo64, g_fake_cfg, 8);
    EXPECT(rd64(DCFG(0)) == lo64, "device cfg qword read composes");
    EXPECT(rd32(DCFG(0x40)) == 0, "device cfg beyond config_len reads 0");
    int w0 = g_fake.cfgwrites;
    wr32(DCFG(4), 0xA5A5A5A5u);
    EXPECT(g_fake.cfgwrites == w0 + 1 && g_fake.cfgw_off == 4 &&
           g_fake.cfgw_val == 0xA5A5A5A5u && g_fake.cfgw_size == 4,
           "config write routed to the device half");
    EXPECT(rd8(DCFG(4)) == g_fake_cfg[4], "core does not own config bytes");
    if (fails == f0) printf("ok device config: reads serve desc bytes, writes call back\n");
}

static void v_dma_bounds(void) {
    int f0 = fails;
    uint8_t buf[16];
    memset(buf, 0x5A, sizeof buf);
    /* writes that would leave RAM copy nothing, and in-RAM tails
     * of a spilling range are NOT partially written */
    M.ram[RAM_SIZE - 1] = 0xAA;
    EXPECT(virtio_dma_write(&g_fake.vio, RAM_SIZE - 2, buf, 8) == 0,
           "spilling dma_write refused whole");
    EXPECT(M.ram[RAM_SIZE - 1] == 0xAA, "no partial spill write");
    EXPECT(virtio_dma_read(&g_fake.vio, RAM_SIZE - 2, buf, 8) == 0,
           "spilling dma_read refused whole");
    /* descriptor aimed at the device's own MMIO must never land */
    EXPECT(virtio_dma_write(&g_fake.vio, VBAR, buf, 4) == 0,
           "MMIO-aimed dma_write refused");
    /* stream copies truncate at the shorter stream */
    blind_init();
    memset(M.ram + Q0_DESC, 0, 0x3000);
    memset(M.ram + BUF0, 0, 0x2000);
    queue_setup(0, Q0_DESC, Q0_AVAIL, Q0_USED);
    memcpy(M.ram + BUF0, "WXYZ", 4);
    put_desc(Q0_DESC, 0, BUF0, 4, 0, 0);                /* out only, 4 bytes */
    avail_post(Q0_AVAIL, 0, 0);
    wr16(NOTIFY(0), 0);
    uint32_t id, len;
    used_elem(Q0_USED, 0, &id, &len);
    EXPECT(len == 0, "echo into a zero-length in-stream completes len 0");
    dev_status(0);
    if (fails == f0) printf("ok dma bounds: spill/MMIO refused, stream truncation sane\n");
}

/* ---------------------------- V5: MSI-X ------------------------------ */
#define MSIX_AT(entry, word) (VBAR + 0x3800u + 16u * (entry) + 4u * (word))
#define PBA_AT (VBAR + 0x3C00u)

static int msi_take(void) {
    int v = lapic_intack(&M);
    if (v >= 0) mem_write(&M, 0xFEE000B0u, 4, 0);       /* EOI */
    return v;
}
static void msix_ctl(uint16_t mc) {        /* Message Control RW bits */
    pci_wr32(0x84, ((uint32_t)mc << 16) | 0x11u);
}
static void msix_entry(int i, uint32_t vec, int unmask) {
    wr32(MSIX_AT(i, 0), 0xFEE00000u);      /* msg addr: BSP, physical */
    wr32(MSIX_AT(i, 1), 0);
    wr32(MSIX_AT(i, 2), vec);              /* msg data: the bare vector */
    wr32(MSIX_AT(i, 3), unmask ? 0u : 1u); /* vector control */
}
static void msix_assign(int qi, uint16_t vec) {
    wr16(CCD(0x16), (uint16_t)qi);
    wr16(CCD(0x1A), vec);
}

/* Continuing pings (echo_round restarts its rings on purpose, which
 * would desync last_avail across rounds; the slot cursors live here). */
static int msix_slot[2];
static void msix_ping(int qi, const char *payload, uint32_t n) {
    uint32_t desc  = qi ? Q1_DESC  : Q0_DESC;
    uint32_t avail = qi ? Q1_AVAIL : Q0_AVAIL;
    uint32_t used  = qi ? Q1_USED  : Q0_USED;
    uint32_t buf   = qi ? BUF1     : BUF0;
    memcpy(M.ram + buf, payload, n);
    memset(M.ram + buf + 0x100, 0, n);
    put_desc(desc, 0, buf,        n, VRING_DESC_F_NEXT, 1);
    put_desc(desc, 1, buf + 0x100, n, VRING_DESC_F_WRITE, 0);
    uint16_t before = used_idx(used);
    avail_post(avail, msix_slot[qi]++, 0);
    wr16(NOTIFY(qi), qi);
    EXPECT(used_idx(used) == before + 1, "msix ping q%d completed", qi);
    EXPECT(memcmp(M.ram + buf + 0x100, payload, n) == 0, "msix ping q%d echoed", qi);
}

static void v_msix(void) {
    int f0 = fails;
    M.lapic.svr |= 0x100u;                 /* APIC software-enable */
    EXPECT(lapic_deliverable(&M) < 0, "LAPIC quiet at start");
    dev_status(0);
    EXPECT(rd32(MSIX_AT(0, 3)) == 1u && rd32(MSIX_AT(1, 3)) == 1u &&
           rd32(MSIX_AT(2, 3)) == 1u, "MSI-X table masked at power-on");
    /* assignments latch and read back; a value past the table size
     * clamps to NO_VECTOR (the QEMU readback rule -- Linux checks
     * exactly this when it falls back to fewer vectors) */
    wr16(CCD(0x10), 0);
    EXPECT(rd16(CCD(0x10)) == 0, "msix_config assigned 0");
    msix_assign(0, 1);
    EXPECT(rd16(CCD(0x1A)) == 1, "q0 assigned vector 1");
    msix_assign(1, 7);
    EXPECT(rd16(CCD(0x1A)) == 0xFFFF, "vector past the table -> NO_VECTOR");
    /* a device reset re-arms the assignments (device state) */
    full_init();
    EXPECT(rd16(CCD(0x10)) == 0xFFFF, "reset re-armed msix_config");
    wr16(CCD(0x16), 1);
    EXPECT(rd16(CCD(0x1A)) == 0xFFFF, "reset re-armed queue vectors");
    queue_setup(0, Q0_DESC, Q0_AVAIL, Q0_USED);
    queue_setup(1, Q1_DESC, Q1_AVAIL, Q1_USED);
    msix_slot[0] = msix_slot[1] = 0;
    dev_status((uint8_t)(rd8(CCD(0x14)) | 4));          /* DRIVER_OK */
    msix_entry(0, 0x53, 1);
    msix_entry(1, 0x51, 1);
    msix_entry(2, 0x52, 1);
    EXPECT(rd32(MSIX_AT(1, 2)) == 0x51u && rd32(MSIX_AT(2, 0)) == 0xFEE00000u,
           "table entries read back");
    wr16(CCD(0x10), 0);
    msix_assign(0, 1);
    msix_assign(1, 2);
    /* MSI-X still off: INTx is the plane that talks */
    msix_ping(0, "IN", 2);
    EXPECT(irq5() && lapic_deliverable(&M) < 0, "pre-enable: INTx fires, no message");
    EXPECT(rd8(ISR_AT) == 1 && !irq5(), "ISR read-ack drops the line");
    /* Enable: the pin retires, per-queue messages land in the LAPIC */
    msix_ctl(0x8000);
    EXPECT(((pci_rd32(0x84) >> 16) & 0x8000u) != 0, "Enable bit reads back");
    msix_ping(0, "P0", 2);
    EXPECT(!irq5(), "INTx retired under MSI-X");
    EXPECT(msi_take() == 0x51, "q0 completion arrived as message 0x51");
    EXPECT(rd8(ISR_AT) == 1, "ISR byte still latches under MSI-X");
    msix_ping(1, "P1", 2);
    EXPECT(msi_take() == 0x52, "q1 completed on its own vector 0x52");
    (void)rd8(ISR_AT);
    /* per-vector mask: the event pends; the PBA shows it through
     * the assigned vector; the unmask releases exactly one message */
    wr32(MSIX_AT(1, 3), 1);
    msix_ping(0, "M0", 2);
    EXPECT(lapic_deliverable(&M) < 0, "masked vector delivers nothing");
    EXPECT(rd32(PBA_AT) == (1u << 1), "PBA bit for the held completion");
    wr32(MSIX_AT(1, 3), 0);
    EXPECT(msi_take() == 0x51, "unmask released the pending message");
    EXPECT(rd32(PBA_AT) == 0, "PBA drained after release");
    (void)rd8(ISR_AT);
    /* Function Mask: the same holding pattern one level up */
    msix_ctl(0xC000);
    msix_ping(1, "F1", 2);
    EXPECT(lapic_deliverable(&M) < 0 && rd32(PBA_AT) == (1u << 2),
           "Function Mask holds q1 pending");
    msix_ctl(0x8000);
    EXPECT(msi_take() == 0x52, "function unmask released 0x52");
    (void)rd8(ISR_AT);
    /* the config source rides vector 0, generation in step */
    uint8_t gen0 = rd8(CCD(0x15));
    virtio_config_changed(&g_fake.vio);
    EXPECT(msi_take() == 0x53, "config change arrived on vector 0");
    EXPECT(rd8(CCD(0x15)) == (uint8_t)(gen0 + 1), "config_generation advanced");
    (void)rd8(ISR_AT);
    /* an unassigned queue under Enable=1 has no wire at all: PCI
     * forbids the pin once MSI-X is on (the QEMU rule) */
    msix_assign(1, 0xFFFF);
    msix_ping(1, "NV", 2);
    EXPECT(lapic_deliverable(&M) < 0 && !irq5() && rd32(PBA_AT) == 0,
           "NO_VECTOR queue: no message, no pin, no PBA bit");
    (void)rd8(ISR_AT);
    /* disabling hands the plane back to INTx */
    msix_ctl(0x0000);
    msix_ping(0, "OF", 2);
    EXPECT(irq5() && lapic_deliverable(&M) < 0, "disable: INTx back in charge");
    EXPECT(rd8(ISR_AT) == 1 && !irq5(), "ISR read-ack after hand-off");
    /* the table itself is PCI-plane state: a device reset keeps
     * the messages and only re-arms the assignments */
    EXPECT(rd32(MSIX_AT(1, 2)) == 0x51u, "table data intact at the end");
    dev_status(0);
    EXPECT(rd32(MSIX_AT(1, 2)) == 0x51u, "device reset keeps the MSI-X table");
    EXPECT(rd16(CCD(0x10)) == 0xFFFF, "while the assignments re-arm");
    if (fails == f0) printf("ok msix: delivery, masks + PBA, config vector, INTx hand-off\n");
}

int main(void) {
    setup_machine(&M);
    devices_init_common(&M);
    fake_add();
    v_pci_identity();
    v_caps();
    v_bar_probe();
    v_features();
    v_queue_regs();
    v_echo_net_shape();
    v_echo_blk_shape();
    v_second_queue();
    v_no_interrupt();
    v_malformed();
    v_reset();
    v_device_config();
    v_dma_bounds();
    v_msix();
    devices_done(&M);
    if (fails) { printf("test-virtio: %d FAILED\n", fails); return 1; }
    puts("test-virtio: all vectors passed");
    return 0;
}
