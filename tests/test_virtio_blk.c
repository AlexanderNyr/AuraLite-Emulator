/* tests/test_virtio_blk.c -- VIRTIO V2 vectors: the virtio-blk device
 * half on the V1 transport (docs/plans/VIRTIO_PLAN.md, decision D5).
 *
 * The request forms are the measured guest's (AuraLite-OS
 * drivers/virtio_blk/virtio_blk.c): the blind status ritual
 * (0; ACK|DRIVER; |=FEATURES_OK; |=DRIVER_OK, no feature traffic,
 * finding N1), the three-descriptor chain (16-byte header, data,
 * one status byte), completion observed by polling used-idx.
 * Everything the guest would touch goes through real PCI config,
 * BAR MMIO and guest-RAM rings on the full machine fixture.
 *
 * One vector pins the device-config truth: capacity is the u64 at
 * the head of the device-config region.  The guest driver reads it
 * from PCI config 0x10/0x14 instead (its own BAR values -- finding
 * N2, guest-side); nothing here colludes with that.
 */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "devices.h"
#include "harness.h"
#include "snapshot.h"
#include "lapic.h"     /* V5: observe the restored MSI-X delivery */
#include "virtio.h"

static int fails;
#define EXPECT(cond, ...) do { if (!(cond)) { printf("FAIL " __VA_ARGS__); printf("\n"); fails++; } } while (0)

static machine_t M;

#define VBAR       0xFEB30000u
#define CCD(off)   (VBAR + (off))
#define DCFG(off)  (VBAR + 0x2000u + (off))
#define NOTIFY0    (VBAR + 0x3000u)

#define IMG_SECTORS 64
static uint8_t IMG[IMG_SECTORS * 512];

/* ring + buffer area (guest RAM at 2 MiB) */
#define QDESC  0x200000u
#define QAVAIL 0x201000u
#define QUSED  0x202000u
#define HDRB   0x210000u     /* request header page     */
#define DATAB  0x211000u     /* data page               */
#define STATB  0x212000u     /* status page             */

static uint8_t  rd8 (uint32_t a) { return (uint8_t) mem_read(&M, a, 1); }
static uint16_t rd16(uint32_t a) { return (uint16_t)mem_read(&M, a, 2); }
static uint32_t rd32(uint32_t a) { return (uint32_t)mem_read(&M, a, 4); }
static uint64_t rd64(uint32_t a) { return mem_read(&M, a, 8); }
static void wr8 (uint32_t a, uint8_t v)  { mem_write(&M, a, 1, v); }
static void wr16(uint32_t a, uint16_t v) { mem_write(&M, a, 2, v); }
static void wr32(uint32_t a, uint32_t v) { mem_write(&M, a, 4, v); }
static void wr64(uint32_t a, uint64_t v) { mem_write(&M, a, 8, v); }

static void dev_status(uint8_t st) { wr8(CCD(0x14), st); }

/* drivers/virtio_blk/virtio_blk.c, verbatim. */
static void guest_blind_init(void) {
    dev_status(0);
    dev_status(1 | 2);
    dev_status((uint8_t)(rd8(CCD(0x14)) | 8));
    dev_status((uint8_t)(rd8(CCD(0x14)) | 4));
    wr16(CCD(0x16), 0);
    wr16(CCD(0x18), rd16(CCD(0x18)));          /* read, clamp, write back */
    wr64(CCD(0x20), QDESC);
    wr64(CCD(0x28), QAVAIL);
    wr64(CCD(0x30), QUSED);
    wr16(CCD(0x1C), 1);
}

/* The by-the-book ritual (net shape) against a VERSION_1-only offer. */
static void guest_full_init(void) {
    dev_status(0); dev_status(1); dev_status(3);
    wr32(CCD(0x08), 0); wr32(CCD(0x0C), 0);    /* acked: nothing local */
    wr32(CCD(0x08), 1); wr32(CCD(0x0C), 1);    /* acked: VERSION_1 */
    dev_status((uint8_t)(rd8(CCD(0x14)) | 8));
    wr16(CCD(0x16), 0);
    wr16(CCD(0x18), rd16(CCD(0x18)));
    wr64(CCD(0x20), QDESC);
    wr64(CCD(0x28), QAVAIL);
    wr64(CCD(0x30), QUSED);
    wr16(CCD(0x1C), 1);
    dev_status((uint8_t)(rd8(CCD(0x14)) | 4));
}

struct tdesc { uint64_t addr; uint32_t len; uint16_t flags, next; };
static void put_desc(int i, uint64_t addr, uint32_t len, uint16_t flags, uint16_t next) {
    struct tdesc d = { addr, len, flags, next };
    memcpy(M.ram + QDESC + 16u * (uint32_t)i, &d, sizeof d);
}
static uint16_t used_idx(void) {
    return (uint16_t)(M.ram[QUSED + 2] | (M.ram[QUSED + 3] << 8));
}
static void used_elem(int slot, uint32_t *id, uint32_t *len) {
    memcpy(id,  M.ram + QUSED + 4 + 8 * slot, 4);
    memcpy(len, M.ram + QUSED + 8 + 8 * slot, 4);
}
static unsigned avail_seq;
static void post(uint16_t head) {
    M.ram[QAVAIL + 4 + 2 * avail_seq] = (uint8_t)head;
    M.ram[QAVAIL + 5 + 2 * avail_seq] = (uint8_t)(head >> 8);
    avail_seq++;
    M.ram[QAVAIL + 2] = (uint8_t)avail_seq;
    M.ram[QAVAIL + 3] = (uint8_t)(avail_seq >> 8);
    wr16(NOTIFY0, 0);
}
static void zero_rings(void) {
    memset(M.ram + QDESC, 0, 0x3000);
    avail_seq = 0;
}

/* One guest-shaped transfer: header desc + data desc + status desc.
 * Returns the status byte; *used receives the used-elem len. */
static uint8_t xfer(uint32_t type, uint64_t sector, uint32_t bytes,
                    uint32_t *used) {
    memset(M.ram + HDRB, 0, 16);
    memcpy(M.ram + HDRB, &type, 4);
    memcpy(M.ram + HDRB + 8, &sector, 8);
    M.ram[STATB] = 0xFF;                            /* guest sentinel */
    put_desc(0, HDRB, 16, VRING_DESC_F_NEXT, 1);
    put_desc(1, DATAB, bytes,
             VRING_DESC_F_NEXT | (type == 0 ? VRING_DESC_F_WRITE : 0), 2);
    put_desc(2, STATB, 1, VRING_DESC_F_WRITE, 0);
    uint16_t before = used_idx();
    post(0);
    uint32_t id = 0xFEFE, len = 0xFEFE;
    used_elem(before, &id, &len);
    if (used) *used = len;
    EXPECT(used_idx() == before + 1 && id == 0,
           "xfer type %u completed synchronously (idx %u->%u id %u)",
           type, before, used_idx(), id);
    return M.ram[STATB];
}

static void v_identity(void) {
    int f0 = fails;
    wr32(CCD(0x00), 0);
    EXPECT(rd32(CCD(0x04)) == (1u << 9),
           "exactly VIRTIO_BLK_F_FLUSH offered (D5+V5; got %08x)", rd32(CCD(0x04)));
    wr32(CCD(0x00), 1);
    EXPECT(rd32(CCD(0x04)) == 1, "transport offers exactly VERSION_1");
    EXPECT(rd16(CCD(0x12)) == 1, "one queue");
    wr16(CCD(0x16), 0);
    EXPECT(rd16(CCD(0x18)) == 128, "queue max 128 (got %u)", rd16(CCD(0x18)));
    /* the honest capacity, where the spec keeps it */
    EXPECT(rd64(DCFG(0)) == IMG_SECTORS, "device-config capacity = 64 sectors");
    EXPECT(rd32(DCFG(8)) == 0, "config beyond capacity reads 0 (no extra fields offered)");
    if (fails == f0) printf("ok identity: VERSION_1 + FLUSH, capacity in device cfg\n");
}

static void v_read_guest_shape(void) {
    int f0 = fails;
    guest_blind_init();                    /* N1 ritual, then poll */
    EXPECT((rd8(CCD(0x14)) & 0x0F) == 0x07,
           "blind init: FEATURES_OK rejected, DRIVER_OK set");
    zero_rings();
    memset(M.ram + DATAB, 0, 0x1000);
    uint32_t used;
    uint8_t st = xfer(0, 2, 1024, &used);              /* T_IN, LBA 2, 2 sectors */
    EXPECT(st == 0, "S_OK (got %u)", st);
    EXPECT(used == 1025, "used len = data + status (got %u)", used);
    EXPECT(memcmp(M.ram + DATAB, IMG + 2 * 512, 1024) == 0, "sector bytes exact");
    dev_status(0);
    if (fails == f0) printf("ok read: guest chain, S_OK, byte-exact, used 1025\n");
}

static void v_write_readback(void) {
    int f0 = fails;
    guest_full_init();
    EXPECT((rd8(CCD(0x14)) & 0x0F) == 0x0F, "full negotiation sticks on blk too");
    zero_rings();
    for (int i = 0; i < 512; i++) M.ram[DATAB + i] = (uint8_t)(i * 7 + 3);
    uint32_t used;
    uint8_t st = xfer(1, 5, 512, &used);               /* T_OUT, LBA 5 */
    EXPECT(st == 0 && used == 1, "write S_OK, used 1 (st %u used %u)", st, used);
    int landed = 1;
    for (int i = 0; i < 512; i++) landed &= IMG[5 * 512 + i] == (uint8_t)(i * 7 + 3);
    EXPECT(landed, "image holds the written sector");
    memset(M.ram + DATAB, 0, 512);
    st = xfer(0, 5, 512, &used);
    int back = 1;
    for (int i = 0; i < 512; i++) back &= M.ram[DATAB + i] == (uint8_t)(i * 7 + 3);
    EXPECT(st == 0 && back, "read-back returns the written bytes");
    if (fails == f0) printf("ok write+readback: RAM image is the disk\n");
}

static void v_split_chain(void) {
    int f0 = fails;
    /* still under the full init from v_write_readback: one sector
     * read into two 256-byte pieces + separate status descriptor --
     * the status byte must land in the LAST piece (in-stream tail),
     * i.e. in the one-byte status desc, not in the data. */
    memset(M.ram + DATAB, 0, 0x1000);
    memset(M.ram + HDRB, 0, 16);
    uint32_t type = 0;
    uint64_t sector = 7;
    memcpy(M.ram + HDRB, &type, 4);
    memcpy(M.ram + HDRB + 8, &sector, 8);
    M.ram[STATB] = 0xFF;
    put_desc(0, HDRB, 16, VRING_DESC_F_NEXT, 1);
    put_desc(1, DATAB,        256, VRING_DESC_F_NEXT | VRING_DESC_F_WRITE, 2);
    put_desc(2, DATAB + 0x400, 256, VRING_DESC_F_NEXT | VRING_DESC_F_WRITE, 3);
    put_desc(3, STATB, 1, VRING_DESC_F_WRITE, 0);
    uint16_t before = used_idx();
    post(0);
    uint32_t id, len;
    used_elem(before, &id, &len);
    EXPECT(used_idx() == before + 1 && len == 513, "split read used 513 (got %u)", len);
    EXPECT(M.ram[STATB] == 0, "status in the tail descriptor (got %u)", M.ram[STATB]);
    EXPECT(memcmp(M.ram + DATAB, IMG + 7 * 512, 256) == 0 &&
           memcmp(M.ram + DATAB + 0x400, IMG + 7 * 512 + 256, 256) == 0,
           "both halves carry their part of the sector");
    if (fails == f0) printf("ok split chain: stream copy crosses descriptors\n");
}

/* V5: FLUSH is advertised (feature bit 9) and pinned.  On a
 * synchronous RAM-copy engine the barrier is honest by construction:
 * the write's bytes were already in the image before its own
 * completion was visible, so FLUSH answers S_OK with no data stream
 * and the following read must see the write. */
static void v_flush(void) {
    int f0 = fails;
    uint32_t used;
    for (int i = 0; i < 512; i++) M.ram[DATAB + i] = (uint8_t)(0x5A ^ (i & 0xF));
    EXPECT(xfer(1, 11, 512, &used) == 0, "write before flush");
    EXPECT(xfer(4, 0, 0, &used) == 0 && used == 1,
           "FLUSH (type 4) -> S_OK, used 1 (status only)");
    EXPECT(xfer(4, 0xFFFFFFFFull, 0, &used) == 0,
           "FLUSH ignores its sector field");
    memset(M.ram + DATAB, 0, 512);
    EXPECT(xfer(0, 11, 512, &used) == 0, "read after flush");
    int back = 1;
    for (int i = 0; i < 512; i++) back &= M.ram[DATAB + i] == (uint8_t)(0x5A ^ (i & 0xF));
    EXPECT(back, "bytes written before the flush are what reads see");
    if (fails == f0) printf("ok flush: feature 9 + type 4, S_OK, ordering honest\n");
}

static void v_requests_refused(void) {
    int f0 = fails;
    uint32_t used;
    /* range: last sector is 63; 64 and beyond refuse with IOERR */
    EXPECT(xfer(0, IMG_SECTORS, 512, &used) == 1 && used == 1, "LBA == capacity -> S_IOERR");
    EXPECT(xfer(1, IMG_SECTORS - 1, 1024, &used) == 1, "write spilling past end -> S_IOERR");
    EXPECT(xfer(0, 0xFFFFFFFFull, 512, &used) == 1, "absurd LBA -> S_IOERR (no wrap)");
    /* boundary sanity: the last whole sector still works */
    EXPECT(xfer(0, IMG_SECTORS - 1, 512, &used) == 0, "last sector reads fine");
    /* length discipline */
    EXPECT(xfer(0, 0, 511, &used) == 1, "non-sector-multiple length -> S_IOERR");
    EXPECT(xfer(0, 0, 0, &used) == 1, "zero-length read -> S_IOERR");
    /* unadvertised types: GET_ID(8), DISCARD(11), WRITE_ZEROES(13),
     * garbage(99) -- FLUSH(4) left this list in V5 (v_flush pins it) */
    EXPECT(xfer(8, 0, 20, &used) == 2, "GET_ID -> S_UNSUPP");
    EXPECT(xfer(11, 0, 512, &used) == 2 && used == 1, "DISCARD -> S_UNSUPP");
    EXPECT(xfer(13, 0, 512, &used) == 2, "WRITE_ZEROES -> S_UNSUPP");
    EXPECT(xfer(99, 0, 512, &used) == 2, "unknown type -> S_UNSUPP");
    if (fails == f0) printf("ok refusals: IOERR on range/length, UNSUPP off the offer\n");
}

static void v_short_header(void) {
    int f0 = fails;
    memset(M.ram + HDRB, 0, 16);
    M.ram[STATB] = 0xFF;
    put_desc(0, HDRB, 8, VRING_DESC_F_NEXT, 1);        /* truncated header */
    put_desc(1, STATB, 1, VRING_DESC_F_WRITE, 0);
    uint16_t before = used_idx();
    post(0);
    uint32_t id, len;
    used_elem(before, &id, &len);
    EXPECT(used_idx() == before + 1, "short header still completes");
    EXPECT(M.ram[STATB] == 1 && len == 1, "short header -> S_IOERR at the status byte");
    dev_status(0);
    if (fails == f0) printf("ok short header: completed as S_IOERR, never parsed\n");
}

/* Runs last on purpose: snap_load rewrites the whole machine image. */
static int msi_take(void) {
    int v = lapic_intack(&M);
    if (v >= 0) mem_write(&M, 0xFEE000B0u, 4, 0);       /* EOI */
    return v;
}

static void v_snapshot(void) {
    int f0 = fails;
    guest_full_init();
    zero_rings();
    /* V5: the MSI-X plane rides in the serialized transport.  Program
     * it before the save -- table entry, assignment, Enable -- and
     * drain one delivery, so the post-load completion below can only
     * arrive through the RESTORED state. */
    M.lapic.svr |= 0x100u;
    wr32(VBAR + 0x3800u + 16u + 0u,  0xFEE00000u);
    wr32(VBAR + 0x3800u + 16u + 8u,  0xA5u);
    wr32(VBAR + 0x3800u + 16u + 12u, 0u);
    wr16(CCD(0x16), 0);
    wr16(CCD(0x1A), 1);
    io_write(&M, 0xCF8, 4, 0x80000000u | (5u << 11) | 0x84u);
    io_write(&M, 0xCFC, 4, (0x8000u << 16) | 0x11u);    /* MSI-X Enable */
    /* write a marker sector, snapshot, trample the image, load */
    for (int i = 0; i < 512; i++) M.ram[DATAB + i] = (uint8_t)(0xA0 + (i & 7));
    uint32_t used;
    EXPECT(xfer(1, 9, 512, &used) == 0, "marker write before snapshot");
    EXPECT(msi_take() == 0xA5, "marker write signalled over MSI-X pre-save");
    static const snap_cfg_t CFG = { "Haswell", 0, 0 };
    const char *path = "/tmp/test_virtio_blk.snap";
    EXPECT(snap_save(&M, path, &CFG) == 0, "snapshot saves with blk attached");
    for (int i = 0; i < 512; i++) M.ram[DATAB + i] = 0x11;
    EXPECT(xfer(1, 9, 512, &used) == 0, "image trampled after save");
    EXPECT(snap_load(&M, path, &CFG) == 0, "snapshot loads");
    remove(path);
    memset(M.ram + DATAB, 0, 512);
    /* NB: the load rebuilt the ring state from the saved machine;
     * avail_seq on the test side must follow the restored cursor.
     * The device side (last_avail) was restored with the transport,
     * so re-derive the test cursor from used progress. */
    EXPECT(rd16(CCD(0x1C)) == 1, "queue still enabled after load (transport restored)");
    EXPECT(rd64(DCFG(0)) == IMG_SECTORS, "capacity intact after load");
    EXPECT(rd16(CCD(0x1A)) == 1, "queue MSI-X vector assignment restored");
    EXPECT(rd32(VBAR + 0x3800u + 16u + 8u) == 0xA5u, "MSI-X table message restored");
    /* post at the restored avail position: read avail idx from RAM */
    avail_seq = (uint16_t)(M.ram[QAVAIL + 2] | (M.ram[QAVAIL + 3] << 8));
    uint8_t st = xfer(0, 9, 512, &used);
    int marker = 1;
    for (int i = 0; i < 512; i++) marker &= M.ram[DATAB + i] == (uint8_t)(0xA0 + (i & 7));
    EXPECT(st == 0 && marker, "saved image state, not the trample, answers reads");
    EXPECT(msi_take() == 0xA5, "post-load completion arrived on the restored vector");
    dev_status(0);
    if (fails == f0) printf("ok snapshot: transport (+MSI-X) + image round-trip, requests continue\n");
}

int main(void) {
    setup_machine(&M);
    devices_init_common(&M);
    for (int i = 0; i < IMG_SECTORS * 512; i++) IMG[i] = (uint8_t)(i / 512 + (i & 0x3F));
    virtio_blk_test_add(&M, IMG, sizeof IMG);
    v_identity();
    v_read_guest_shape();
    v_write_readback();
    v_split_chain();
    v_flush();
    v_requests_refused();
    v_short_header();
    v_snapshot();
    devices_done(&M);
    if (fails) { printf("test-virtio-blk: %d FAILED\n", fails); return 1; }
    puts("test-virtio-blk: all vectors passed");
    return 0;
}
