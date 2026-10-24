/* virtio_blk.c -- VIRTIO V2: virtio-blk on the V1 core (D5).
 *
 * Request model (virtio 1.x section 5.2): a 16-byte header
 * {type, reserved, sector} at the head of the out-stream, data in
 * the middle (out-stream for writes, in-stream for reads), and the
 * status byte as the LAST byte of the in-stream -- which is what
 * makes both the guest's three-descriptor chain and SeaBIOS's
 * split chains land in the same place.  used_len counts bytes
 * actually written into the writable side (data+1 on a read, 1 on
 * a write or an error).
 *
 * Completion is synchronous inside the notify write (house rule):
 * the polling guest observes its used-idx advance by the time the
 * doorbell store retires.  The offer is VERSION_1 + FLUSH (V5):
 * FLUSH (feature bit 9, request type 4) is honest here precisely
 * because there is no volatile layer to drain -- a write's bytes
 * are already in the RAM image before its completion is visible,
 * so the barrier S_OK never promises more durability than the
 * medium (a RAM copy) itself has.  Every other unadvertised type
 * still earns S_UNSUPP.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "virtio.h"
#include "virtio_blk.h"

#define VBLK_SECTOR       512u
#define VBLK_QSIZE        128          /* offered max (guest clamps to 256) */

#define VBLK_F_FLUSH      9            /* feature bit (VIRTIO_BLK_F_FLUSH) */

#define VBLK_T_IN         0            /* read  */
#define VBLK_T_OUT        1            /* write */
#define VBLK_T_FLUSH      4            /* request type (feature bit 9) */
#define VBLK_T_GET_ID     8

#define VBLK_S_OK         0
#define VBLK_S_IOERR      1
#define VBLK_S_UNSUPP     2

typedef struct {
    machine_t   *m;
    virtio_dev_t vio;
    virtio_req_t req;                  /* ~8 KiB scratch (echo pattern) */
    uint8_t     *img;
    size_t       img_len;
    uint64_t     capacity;             /* sectors */
    uint8_t      config[8];            /* capacity LE, the whole config */
    int          attached;
    unsigned     err_logs;             /* first-N protocol error budget */
} vblk_t;

static vblk_t g_vblk;
static virtio_device_desc_t g_vblk_desc;   /* filled at attach (config ptr) */

static void vblk_err(vblk_t *b, const char *what, uint64_t a, uint64_t c) {
    if (b->err_logs < 8)
        mlog(&b->m->log, "[virtio-blk] request error: %s (%llu, %llu)\n",
             what, (unsigned long long)a, (unsigned long long)c);
    b->err_logs++;
}

static uint32_t le32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static uint64_t le64(const uint8_t *p) {
    return (uint64_t)le32(p) | ((uint64_t)le32(p + 4) << 32);
}

/* Finish one request: the status byte is always the last byte of
 * the in-stream (when the guest gave us a writable byte at all). */
static void vblk_finish(vblk_t *b, virtio_req_t *r,
                        uint8_t status, uint32_t used_len) {
    if (r->in_len >= 1)
        virtio_req_write(r, r->in_len - 1, &status, 1);
    else
        used_len = 0;
    virtio_complete(&b->vio, r, used_len);
}

static void vblk_process(vblk_t *b, virtio_req_t *r) {
    uint8_t hdr[16];
    if (virtio_req_read(r, 0, hdr, sizeof hdr) < sizeof hdr) {
        vblk_err(b, "header shorter than 16 bytes", le32(hdr), r->out_len);
        vblk_finish(b, r, VBLK_S_IOERR, 1);
        return;
    }
    uint32_t type = le32(hdr + 0);
    uint64_t sector = le64(hdr + 8);

    if (type == VBLK_T_FLUSH) {
        /* V5: header + status only, no data stream, sector ignored.
         * Honest because completion is synchronous: every earlier
         * write is already in the image when this S_OK lands. */
        vblk_finish(b, r, VBLK_S_OK, 1);
        return;
    }
    if (type != VBLK_T_IN && type != VBLK_T_OUT) {
        /* GET_ID / DISCARD / WRITE_ZEROES / garbage: none of their
         * feature bits are offered, so the truthful answer is UNSUPP. */
        vblk_finish(b, r, VBLK_S_UNSUPP, 1);
        return;
    }

    uint32_t data_len = (type == VBLK_T_IN)
        ? (r->in_len >= 1 ? r->in_len - 1 : 0)
        : (r->out_len >= 16 ? r->out_len - 16 : 0);
    uint64_t count = data_len / VBLK_SECTOR;
    if (data_len == 0 || data_len % VBLK_SECTOR != 0 ||
        sector > b->capacity || count > b->capacity - sector) {
        vblk_err(b, (type == VBLK_T_IN) ? "read out of range" : "write out of range",
                 sector, count);
        vblk_finish(b, r, VBLK_S_IOERR, 1);
        return;
    }

    /* Sector-chunked streams: the core's stream copy crosses
     * descriptor boundaries, so split chains (SeaBIOS, the guest's
     * 3-descriptor form, anything between) are all the same here. */
    uint8_t buf[VBLK_SECTOR];
    uint64_t base = sector * VBLK_SECTOR;
    for (uint32_t done = 0; done < data_len; done += VBLK_SECTOR) {
        if (type == VBLK_T_IN) {
            if (virtio_req_write(r, done, b->img + base + done, VBLK_SECTOR)
                    != VBLK_SECTOR) {
                vblk_finish(b, r, VBLK_S_IOERR, 1);
                return;
            }
        } else {
            if (virtio_req_read(r, 16 + done, buf, VBLK_SECTOR) != VBLK_SECTOR) {
                vblk_finish(b, r, VBLK_S_IOERR, 1);
                return;
            }
            memcpy(b->img + base + done, buf, VBLK_SECTOR);
        }
    }
    vblk_finish(b, r, VBLK_S_OK,
                type == VBLK_T_IN ? data_len + 1 : 1);
}

static void vblk_notify(virtio_dev_t *v, int qi) {
    vblk_t *b = v->ctx;
    if (qi != 0) return;
    for (;;) {
        int rc = virtio_pop(v, 0, &b->req);
        if (rc == 0) break;
        if (rc < 0) continue;          /* malformed: core already finished it */
        vblk_process(b, &b->req);
    }
}

static void vblk_publish(machine_t *m, vblk_t *b) {
    b->m = m;
    b->capacity = b->img_len / VBLK_SECTOR;
    for (int i = 0; i < 8; i++) b->config[i] = (uint8_t)(b->capacity >> (8 * i));
    memset(&g_vblk_desc, 0, sizeof g_vblk_desc);
    g_vblk_desc.features = 1ULL << VBLK_F_FLUSH;   /* D5 + V5 FLUSH */
    g_vblk_desc.num_queues = 1;
    g_vblk_desc.queue_max[0] = VBLK_QSIZE;
    g_vblk_desc.config = b->config;
    g_vblk_desc.config_len = sizeof b->config;
    g_vblk_desc.notify = vblk_notify;
    virtio_pci_add(m, &b->vio, VIRTIO_BLK_PCI_DEV, "virtio-blk",
                   VIRTIO_PCI_BLK_ID, 0x01, 0x80, 0x00,
                   VIRTIO_BLK_BAR_BASE, VIRTIO_BLK_IRQ, &g_vblk_desc, b);
    b->attached = 1;
}

void virtio_blk_test_add(machine_t *m, uint8_t *img, size_t len) {
    memset(&g_vblk, 0, sizeof g_vblk);
    g_vblk.img = img;
    g_vblk.img_len = len;
    vblk_publish(m, &g_vblk);
}

int virtio_blk_attach(machine_t *m, const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) {
        mlog(&m->log, "[virtio-blk] cannot open %s\n", path);
        return -1;
    }
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *img = NULL;
    if (n > 0) {
        img = malloc((size_t)n);
        if (img && fread(img, 1, (size_t)n, f) != (size_t)n) {
            free(img);
            img = NULL;
        }
    }
    fclose(f);
    if (!img) {
        mlog(&m->log, "[virtio-blk] cannot read %s\n", path);
        return -1;
    }
    memset(&g_vblk, 0, sizeof g_vblk);
    g_vblk.img = img;
    g_vblk.img_len = (size_t)n;
    vblk_publish(m, &g_vblk);
    mlog(&m->log, "[virtio-blk] attached %s (%zu bytes, %llu sectors)\n",
         path, g_vblk.img_len, (unsigned long long)g_vblk.capacity);
    return 0;
}

int virtio_blk_present(void) { return g_vblk.attached; }

/* Present-only section (snapshot.c visit() calls this unconditionally):
 * a machine without the opt-in function produces exactly the
 * historical record stream, and the pci.%02x.%02x.%x identity check
 * refuses blk<->no-blk crossings before this record is reached.
 * The image goes through the same sparse record as the SATA images,
 * whose length field refuses a differently-sized file at load. */
void virtio_blk_snap(machine_t *m, snap_t *s) {
    (void)m;
    if (!g_vblk.attached) return;
    virtio_snap(&g_vblk.vio, s, "dev.vblk");
    snap_sparse(s, "disk.vblk", g_vblk.img, g_vblk.img_len);
}
