/* tests/test_virtio_net.c -- VIRTIO V4 vectors: the virtio-net device
 * half on the V1 transport (docs/plans/VIRTIO_PLAN.md, decision D6).
 *
 * The guest side is replayed from AuraLite-OS
 * drivers/virtio_net/virtio_net.c: full negotiation (requires
 * VERSION_1, acks VERSION_1|(MAC if offered), re-reads both
 * FEATURES_OK and queue_enable), queue size clamped to 16 at
 * queue setup, MAC read from the device-config region when F_MAC
 * is offered, RX prefilled after DRIVER_OK with single
 * 2048-byte write descriptors, used.len = 12-byte header + frame,
 * descriptor recycled by re-posting, completion both polled and
 * interrupt-woken (line from cfg 0x3C, ack = ISR read).
 * Frames enter through virtio_net_rx_frame (the e1000_rx_frame
 * lane seam); the TAP socket leg itself is the e1000's existing
 * infrastructure, exercised by the net e2e lanes.
 */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "devices.h"
#include "harness.h"
#include "virtio.h"
#include "virtio_net.h"
#include "lapic.h"     /* V5: observe MSI-X delivery */

static int fails;
#define EXPECT(cond, ...) do { if (!(cond)) { printf("FAIL " __VA_ARGS__); printf("\n"); fails++; } } while (0)

static machine_t M;

#define VBAR       0xFEB34000u
#define CCD(off)   (VBAR + (off))
#define ISR_AT     (VBAR + 0x1000u)
#define DCFG(off)  (VBAR + 0x2000u + (off))
#define NOTIFY(q)  (VBAR + 0x3000u + (q) * 4u)

#define Q0D 0x200000u
#define Q0A 0x201000u
#define Q0U 0x202000u
#define Q1D 0x210000u
#define Q1A 0x211000u
#define Q1U 0x212000u
#define RXBUF(i) (0x220000u + (uint32_t)(i) * 0x1000u)
#define TXBUF    0x228000u

static uint8_t  rd8 (uint32_t a) { return (uint8_t) mem_read(&M, a, 1); }
static uint16_t rd16(uint32_t a) { return (uint16_t)mem_read(&M, a, 2); }
static uint32_t rd32(uint32_t a) { return (uint32_t)mem_read(&M, a, 4); }
static void wr8 (uint32_t a, uint8_t v)  { mem_write(&M, a, 1, v); }
static void wr16(uint32_t a, uint16_t v) { mem_write(&M, a, 2, v); }
static void wr32(uint32_t a, uint32_t v) { mem_write(&M, a, 4, v); }
static void wr64(uint32_t a, uint64_t v) { mem_write(&M, a, 8, v); }

static void dev_status(uint8_t st) { wr8(CCD(0x14), st); }
static int irq7(void) { return (M.pic.master.lines & (1u << 7)) != 0; }

struct tdesc { uint64_t addr; uint32_t len; uint16_t flags, next; };
static void put_desc(uint32_t table, int i, uint64_t addr,
                     uint32_t len, uint16_t flags, uint16_t next) {
    struct tdesc d = { addr, len, flags, next };
    memcpy(M.ram + table + 16u * (uint32_t)i, &d, sizeof d);
}
static uint16_t ring16(uint32_t a, uint32_t off) {
    return (uint16_t)(M.ram[a + off] | (M.ram[a + off + 1] << 8));
}
static void used_elem(uint32_t used, int slot, uint32_t *id, uint32_t *len) {
    memcpy(id,  M.ram + used + 4 + 8 * slot, 4);
    memcpy(len, M.ram + used + 8 + 8 * slot, 4);
}
static void post(uint32_t avail, uint16_t head) {
    uint16_t idx = ring16(avail, 2);
    M.ram[avail + 4 + 2 * idx] = (uint8_t)head;
    M.ram[avail + 5 + 2 * idx] = (uint8_t)(head >> 8);
    idx++;
    M.ram[avail + 2] = (uint8_t)idx;
    M.ram[avail + 3] = (uint8_t)(idx >> 8);
}

/* drivers/virtio_net/virtio_net.c: reset, ACK, DRIVER, read feats,
 * ack VERSION_1|MAC, FEATURES_OK (re-read!), queues at size 16
 * (enable re-read!), DRIVER_OK, then rx_fill.  Fills `mac`. */
static void guest_init(int rx_buffers, uint8_t mac[6]) {
    dev_status(0);
    dev_status(1);
    dev_status(1 | 2);
    wr32(CCD(0x00), 0); uint32_t lo = rd32(CCD(0x04));
    wr32(CCD(0x00), 1); uint32_t hi = rd32(CCD(0x04));
    EXPECT(hi & 1u, "guest: VERSION_1 is offered");
    uint32_t want_lo = (lo & (1u << 5)) ? (1u << 5) : 0;      /* VIRTIO_NET_F_MAC */
    wr32(CCD(0x08), 0); wr32(CCD(0x0C), want_lo);
    wr32(CCD(0x08), 1); wr32(CCD(0x0C), 1);                  /* VERSION_1 */
    dev_status((uint8_t)(rd8(CCD(0x14)) | 8));
    EXPECT((rd8(CCD(0x14)) & 8) != 0, "guest: FEATURES_OK re-reads set");
    if (mac && (want_lo & (1u << 5)))
        for (int i = 0; i < 6; i++) mac[i] = rd8(DCFG(i));
    static const uint32_t roots[2][3] = { { Q0D, Q0A, Q0U }, { Q1D, Q1A, Q1U } };
    for (int qi = 0; qi < 2; qi++) {
        memset(M.ram + roots[qi][0], 0, 0x3000);
        wr16(CCD(0x16), (uint16_t)qi);
        uint16_t qsz = rd16(CCD(0x18));
        if (qsz > 16) qsz = 16;                              /* VNET_Q_SIZE */
        wr16(CCD(0x18), qsz);
        wr64(CCD(0x20), roots[qi][0]);
        wr64(CCD(0x28), roots[qi][1]);
        wr64(CCD(0x30), roots[qi][2]);
        wr16(CCD(0x1C), 1);
        EXPECT(rd16(CCD(0x1C)) == 1, "guest: queue_enable %d reads back 1", qi);
        EXPECT(rd16(CCD(0x1E)) == qi, "guest: notify_off q%d", qi);
    }
    dev_status((uint8_t)(rd8(CCD(0x14)) | 4));
    for (int i = 0; i < rx_buffers; i++) {                   /* rx_fill */
        memset(M.ram + RXBUF(i), 0, 0x1000);
        put_desc(Q0D, i, RXBUF(i), 2048, VRING_DESC_F_WRITE, 0);
        post(Q0A, (uint16_t)i);
    }
    wr16(NOTIFY(0), 0);
}

static void frame_pattern(uint8_t *f, unsigned len, unsigned seed) {
    for (unsigned i = 0; i < len; i++) f[i] = (uint8_t)(seed + i * 3 + (i >> 4));
}

static void v_identity(void) {
    int f0 = fails;
    wr32(CCD(0x00), 0);
    EXPECT(rd32(CCD(0x04)) == (1u << 5), "offer = exactly VIRTIO_NET_F_MAC (D6)");
    wr32(CCD(0x00), 1);
    EXPECT(rd32(CCD(0x04)) == 1u, "plus VERSION_1 from the transport");
    EXPECT(rd16(CCD(0x12)) == 2, "two queues (RX, TX)");
    static const uint8_t MAC[6] = { 0x52, 0x54, 0x00, 0x12, 0x34, 0x56 };
    for (int i = 0; i < 6; i++)
        EXPECT(rd8(DCFG(i)) == MAC[i], "device-cfg mac byte %d", i);
    EXPECT(rd16(DCFG(6)) == 0, "no status word: F_STATUS not offered");
    if (fails == f0) printf("ok identity: VERSION_1|MAC, mac in device cfg, 2 queues\n");
}

static void v_rx_path(void) {
    int f0 = fails;
    uint8_t mac[6] = { 0 };
    guest_init(2, mac);
    EXPECT(mac[0] == 0x52 && mac[5] == 0x56, "guest read the offered MAC");
    uint8_t frame[64];
    frame_pattern(frame, sizeof frame, 0x21);
    virtio_net_rx_frame(&M, frame, sizeof frame);
    EXPECT(ring16(Q0U, 2) == 1, "one RX buffer completed");
    uint32_t id, len;
    used_elem(Q0U, 0, &id, &len);
    EXPECT(id == 0 && len == 12 + 64, "used = hdr+frame (id %u len %u)", id, len);
    uint8_t *buf = M.ram + RXBUF(0);
    int hdr_ok = 1;
    for (int i = 0; i < 12; i++) hdr_ok &= buf[i] == (i == 10 ? 1 : 0);
    EXPECT(hdr_ok, "12-byte hdr zero except num_buffers=1");
    EXPECT(memcmp(buf + 12, frame, 64) == 0, "frame bytes behind the header");
    /* the interrupt half of the guest's life */
    EXPECT(irq7(), "INTx (IRQ7) asserted on RX completion");
    EXPECT(rd8(ISR_AT) == 1, "guest IRQ handler: ISR ack reads queue bit");
    EXPECT(!irq7() && rd8(ISR_AT) == 0, "ack dropped the line");
    uint64_t s, d, rd_, rdr;
    virtio_net_stats(&s, &d, &rd_, &rdr);
    EXPECT(rd_ == 1 && rdr == 0, "rx counters (done %llu dropped %llu)",
           (unsigned long long)rd_, (unsigned long long)rdr);
    if (fails == f0) printf("ok rx: hdr-12 frame, used len, IRQ-wake via ISR ack\n");
}

static void v_rx_recycle_and_starve(void) {
    int f0 = fails;
    /* continues v_rx_path's init: 2 buffers, one consumed. */
    uint8_t frame[100];
    frame_pattern(frame, sizeof frame, 0x77);
    virtio_net_rx_frame(&M, frame, sizeof frame);            /* second buffer */
    EXPECT(ring16(Q0U, 2) == 2, "second buffer completed");
    (void)rd8(ISR_AT);
    /* ring starved now: the frame must drop, the ring must not move */
    uint64_t s, d, rd_, rdr;
    virtio_net_stats(&s, &d, &rd_, &rdr);
    virtio_net_rx_frame(&M, frame, sizeof frame);
    uint64_t rdr2;
    virtio_net_stats(&s, &d, &rd_, &rdr2);
    EXPECT(ring16(Q0U, 2) == 2 && rdr2 == rdr + 1, "starved ring drops + counts");
    /* the guest recycles: re-post descriptor 0 and the frame lands */
    put_desc(Q0D, 0, RXBUF(0), 2048, VRING_DESC_F_WRITE, 0);
    post(Q0A, 0);
    frame_pattern(frame, 40, 0x05);
    virtio_net_rx_frame(&M, frame, 40);
    EXPECT(ring16(Q0U, 2) == 3, "recycled descriptor takes the next frame");
    EXPECT(memcmp(M.ram + RXBUF(0) + 12, frame, 40) == 0, "in the recycled buffer");
    if (fails == f0) printf("ok rx recycle/starve: buffer economy is the guest's\n");
}

static void v_rx_truncate(void) {
    int f0 = fails;
    /* one buffer left in the ring (desc 1 was consumed; recycle it) */
    put_desc(Q0D, 1, RXBUF(1), 2048, VRING_DESC_F_WRITE, 0);
    post(Q0A, 1);
    static uint8_t big[3000];
    frame_pattern(big, sizeof big, 0x40);
    uint16_t before = ring16(Q0U, 2);
    virtio_net_rx_frame(&M, big, sizeof big);
    uint32_t id, len;
    used_elem(Q0U, before, &id, &len);
    EXPECT(ring16(Q0U, 2) == before + 1 && len == 2048,
           "oversize frame truncated to the buffer (used %u)", len);
    EXPECT(memcmp(M.ram + RXBUF(1) + 12, big, 2036) == 0,
           "payload = the first 2036 bytes, no spill past the buffer");
    dev_status(0);
    if (fails == f0) printf("ok rx truncate: fits the buffer, never writes past it\n");
}

static void v_tx_path(void) {
    int f0 = fails;
    guest_init(1, NULL);
    uint64_t ts0, td0, x, y;
    virtio_net_stats(&ts0, &td0, &x, &y);
    /* guest shape: one descriptor, hdr(12, zeroed) + frame */
    memset(M.ram + TXBUF, 0, 12);
    frame_pattern(M.ram + TXBUF + 12, 60, 0x33);
    put_desc(Q1D, 0, TXBUF, 72, 0, 0);
    post(Q1A, 0);
    wr16(NOTIFY(1), 1);
    uint32_t id, len;
    used_elem(Q1U, 0, &id, &len);
    EXPECT(ring16(Q1U, 2) == 1 && len == 0,
           "TX completed synchronously, used 0 (idx elem len %u)", len);
    /* gathered: header in its own descriptor, frame in the next */
    put_desc(Q1D, 1, TXBUF, 12, VRING_DESC_F_NEXT, 2);
    put_desc(Q1D, 2, TXBUF + 12, 60, 0, 0);
    post(Q1A, 1);
    wr16(NOTIFY(1), 1);
    EXPECT(ring16(Q1U, 2) == 2, "split TX chain gathered + completed");
    /* header shorter than 12 bytes total: completed, never sent */
    put_desc(Q1D, 3, TXBUF, 6, 0, 0);
    post(Q1A, 3);
    wr16(NOTIFY(1), 1);
    EXPECT(ring16(Q1U, 2) == 3, "short TX chain completes, no wedge");
    uint64_t ts, td;
    virtio_net_stats(&ts, &td, &x, &y);
    EXPECT(ts == ts0 && td == td0 + 3,
           "socketless unit mode counts 3 dropped, 0 sent (completion first)");
    dev_status(0);
    if (fails == f0) printf("ok tx: single+split gather, short chain bounded, stats honest\n");
}

static void v_not_ready(void) {
    int f0 = fails;
    /* queues programmed but DRIVER_OK never set: nothing may land */
    dev_status(0); dev_status(1); dev_status(3);
    wr32(CCD(0x08), 1); wr32(CCD(0x0C), 1);
    dev_status((uint8_t)(rd8(CCD(0x14)) | 8));
    memset(M.ram + Q0D, 0, 0x3000);
    wr16(CCD(0x16), 0);
    wr16(CCD(0x18), 16);
    wr64(CCD(0x20), Q0D); wr64(CCD(0x28), Q0A); wr64(CCD(0x30), Q0U);
    wr16(CCD(0x1C), 1);
    memset(M.ram + RXBUF(0), 0, 0x1000);
    put_desc(Q0D, 0, RXBUF(0), 2048, VRING_DESC_F_WRITE, 0);
    post(Q0A, 0);
    uint8_t frame[32];
    frame_pattern(frame, sizeof frame, 0x19);
    uint64_t s, d, rd_, rdr;
    virtio_net_stats(&s, &d, &rd_, &rdr);
    virtio_net_rx_frame(&M, frame, sizeof frame);
    uint64_t rdr2;
    virtio_net_stats(&s, &d, &rd_, &rdr2);
    EXPECT(ring16(Q0U, 2) == 0 && rdr2 == rdr + 1,
           "RX before DRIVER_OK drops, buffer unconsumed");
    dev_status((uint8_t)(rd8(CCD(0x14)) | 4));
    virtio_net_rx_frame(&M, frame, sizeof frame);
    EXPECT(ring16(Q0U, 2) == 1, "the same buffer serves once DRIVER_OK lands");
    dev_status(0);
    if (fails == f0) printf("ok not-ready: ingress waits for DRIVER_OK like the spec dance\n");
}

/* V5: the same guest shape over MSI-X.  RX and TX completions must
 * land as LAPIC messages on their own queue vectors, and the IRQ7
 * pin (the whole wake path of v_rx_path) must retire while Enable=1. */
static int msi_take(void) {
    int v = lapic_intack(&M);
    if (v >= 0) mem_write(&M, 0xFEE000B0u, 4, 0);       /* EOI */
    return v;
}

static void v_msix(void) {
    int f0 = fails;
    M.lapic.svr |= 0x100u;
    guest_init(2, NULL);
    /* entry 1 -> RX queue (LAPIC vector 0x61), entry 2 -> TX (0x62) */
    static const struct { int entry; uint32_t vec; } tab[2] = { { 1, 0x61 }, { 2, 0x62 } };
    for (int i = 0; i < 2; i++) {
        uint32_t at = VBAR + 0x3800u + 16u * (uint32_t)tab[i].entry;
        wr32(at + 0u, 0xFEE00000u);
        wr32(at + 8u, tab[i].vec);
        wr32(at + 12u, 0u);
    }
    wr16(CCD(0x16), 0); wr16(CCD(0x1A), 1);
    wr16(CCD(0x16), 1); wr16(CCD(0x1A), 2);
    io_write(&M, 0xCF8, 4, 0x80000000u | (6u << 11) | 0x84u);
    io_write(&M, 0xCFC, 4, (0x8000u << 16) | 0x11u);    /* MSI-X Enable */
    io_write(&M, 0xCF8, 4, 0x80000000u | (6u << 11) | 0x84u);
    EXPECT(((io_read(&M, 0xCFC, 4) >> 16) & 0xFFFFu) == 0x8002u,
           "net Message Control: table size 3, enabled");
    uint8_t frame[48];
    frame_pattern(frame, sizeof frame, 0x44);
    virtio_net_rx_frame(&M, frame, sizeof frame);
    EXPECT(ring16(Q0U, 2) == 1, "RX completed under MSI-X");
    EXPECT(msi_take() == 0x61, "RX arrived on queue 0's vector");
    EXPECT(!irq7(), "IRQ7 pin silent under MSI-X");
    EXPECT(memcmp(M.ram + RXBUF(0) + 12, frame, sizeof frame) == 0,
           "frame intact behind the header");
    (void)rd8(ISR_AT);
    memset(M.ram + TXBUF, 0, 12);
    frame_pattern(M.ram + TXBUF + 12, 40, 0x51);
    put_desc(Q1D, 0, TXBUF, 52, 0, 0);
    post(Q1A, 0);
    wr16(NOTIFY(1), 1);
    EXPECT(ring16(Q1U, 2) == 1, "TX completed under MSI-X");
    EXPECT(msi_take() == 0x62, "TX arrived on queue 1's vector");
    dev_status(0);
    io_write(&M, 0xCF8, 4, 0x80000000u | (6u << 11) | 0x84u);
    io_write(&M, 0xCFC, 4, 0x11u);                      /* MSI-X off */
    if (fails == f0) printf("ok msix: rx/tx on per-queue vectors, IRQ7 retired\n");
}

int main(void) {
    setup_machine(&M);
    devices_init_common(&M);
    virtio_net_test_add(&M);
    v_identity();
    v_rx_path();
    v_rx_recycle_and_starve();
    v_rx_truncate();
    v_tx_path();
    v_not_ready();
    v_msix();
    devices_done(&M);
    if (fails) { printf("test-virtio-net: %d FAILED\n", fails); return 1; }
    puts("test-virtio-net: all vectors passed");
    return 0;
}
