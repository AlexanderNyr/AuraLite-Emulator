/* virtio.c -- VIRTIO V1: the modern (1.x) virtio-pci transport core
 * (docs/plans/VIRTIO_PLAN.md sections 4-5, decisions D1-D4/D7).
 *
 * Layout (D1): one 32-bit memory BAR0 of 16 KiB -- common config at
 * +0x0000, ISR at +0x1000, device config at +0x2000, notify at
 * +0x3000 with multiplier 4 (QEMU's offsets; the BAR index is the
 * one deliberate deviation, and every measured consumer reads it
 * back from the capability, never assumes it).  The vendor
 * capabilities ride in the function's own cfg[0x40..] and are
 * re-asserted after every config write (the XHCI MSI-X precedent:
 * the default cfg store is fully writable, so RO bytes must be
 * defended in the write hook).
 *
 * The queue engine is synchronous (plan rule 3): a notify write,
 * an RX ingress or any other device-half trigger walks the avail
 * ring to completion inside the call.  No request is ever "in
 * flight" across an instruction boundary, which is what keeps
 * snapshots and determinism tapes simple (D7).
 */
#include <string.h>
#include "virtio.h"
#include "pci.h"
#include "pic.h"
#include "lapic.h"   /* msi_deliver (MSI M2) */

/* ============================== small helpers ============================ */

static uint16_t vio_rd16(machine_t *m, uint64_t pa) { return (uint16_t)mem_read(m, pa, 2); }
static uint32_t vio_rd32(machine_t *m, uint64_t pa) { return (uint32_t)mem_read(m, pa, 4); }
static uint64_t vio_rd64(machine_t *m, uint64_t pa) { return mem_read(m, pa, 8); }
static void vio_wr16(machine_t *m, uint64_t pa, uint16_t v) { mem_write(m, pa, 2, v); }
static void vio_wr32(machine_t *m, uint64_t pa, uint32_t v) { mem_write(m, pa, 4, v); }

static uint16_t le16_of(const uint8_t *p) {
    return (uint16_t)(p[0] | (p[1] << 8));
}
static uint32_t le32_of(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static uint64_t le64_of(const uint8_t *p) {
    return (uint64_t)le32_of(p) | ((uint64_t)le32_of(p + 4) << 32);
}
static void put16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static void put32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}
static void put64(uint8_t *p, uint64_t v) { put32(p, (uint32_t)v); put32(p + 4, (uint32_t)(v >> 32)); }

/* D2 hard bound: descriptor and ring ranges must live inside guest
 * RAM.  A descriptor aimed at an MMIO window is rejected here, never
 * dereferenced -- the core cannot be turned into a confused deputy. */
static int vio_ram_ok(uint64_t pa, uint64_t len) {
    return len == 0 || (pa < RAM_SIZE && len <= (uint64_t)RAM_SIZE - pa);
}

static void vio_bad(virtio_dev_t *v, const char *what, int qi, unsigned detail) {
    if (v->bad_chains < 8)
        mlog(&v->m->log, "[virtio] %s: bad chain on queue %d (%s %u)\n",
             v->pci ? v->pci->name : "?", qi, what, detail);
    v->bad_chains++;
}

/* pci_dev -> virtio_dev registry.  pci_dev.ctx belongs to the host
 * bridge (it carries machine_t*), so the core binds by pointer
 * identity instead -- at most a handful of functions exist. */
#define VIO_REGISTRY_MAX 8
static virtio_dev_t *g_vio[VIO_REGISTRY_MAX];
static int g_vio_n;

static virtio_dev_t *vio_of_pci(pci_dev_t *d) {
    for (int i = 0; i < g_vio_n; i++)
        if (g_vio[i]->pci == d) return g_vio[i];
    return NULL;
}

/* ============================== interrupts =============================
 *
 * Two planes (route decided at raise time, like XHCI's M3):
 * - MSI-X disabled: the classic INTx level, driven off the ISR byte
 *   (which latches in every mode, QEMU-style, so an ISR read-ack
 *   works no matter how the guest routes delivery).
 * - MSI-X enabled: INTx must stay silent (PCI rule); each source
 *   raises a per-source pending flag and an unmasked, vector-
 *   assigned source fires its table message immediately.  A masked
 *   source just keeps the flag (it is what the PBA reports through
 *   the assigned vector) until an unmask, a Function-Mask clear or
 *   an Enable edge re-evaluates it -- the MSI_PLAN 2 rule that the
 *   source keeps its own pending state, extended from the XHCI's
 *   single level to per-source flags because virtio completions
 *   are edges, not levels. */

static int vio_msix_nvec(virtio_dev_t *v) { return v->desc->num_queues + 1; }

static uint16_t vio_msix_assign(virtio_dev_t *v, uint16_t vec) {
    if (vec != VIRTIO_MSI_NO_VECTOR && vec >= vio_msix_nvec(v))
        return VIRTIO_MSI_NO_VECTOR;      /* QEMU: unassignable -> none */
    return vec;
}

static uint16_t vio_src_vector(virtio_dev_t *v, int src) {
    return src == 0 ? v->msix_config : v->q[src - 1].msix_vector;
}

static void vio_msix_try(virtio_dev_t *v, int src) {
    if (!v->msix_enable || !v->msix_pending[src]) return;
    uint16_t vec = vio_src_vector(v, src);
    if (vec == VIRTIO_MSI_NO_VECTOR || vec >= vio_msix_nvec(v)) {
        v->msix_pending[src] = 0;         /* nowhere to deliver, ever */
        return;
    }
    if (v->msix_fmask || (v->msix_table[vec].ctrl & 1u)) return;
    v->msix_pending[src] = 0;
    if (v->m)
        msi_deliver(v->m,
                    ((uint64_t)v->msix_table[vec].addr_hi << 32) |
                        v->msix_table[vec].addr_lo,
                    v->msix_table[vec].data);
}

/* An interrupt event on source `src` (0=config, 1+i=queue i).  Under
 * INTx the ISR latch the caller just set is the whole record. */
static void vio_msix_raise(virtio_dev_t *v, int src) {
    if (!v->msix_enable) return;
    v->msix_pending[src] = 1;
    vio_msix_try(v, src);
}

static void vio_msix_scan(virtio_dev_t *v) {
    for (int src = 0; src < vio_msix_nvec(v); src++)
        vio_msix_try(v, src);
}

static void vio_intx_update(virtio_dev_t *v) {
    int want = (v->isr != 0) && !v->msix_enable;
    if (want == v->intx_level) return;
    v->intx_level = want;
    if (v->m) pic_set_irq(v->m, v->irq, want);
}

void virtio_config_changed(virtio_dev_t *v) {
    v->generation++;
    v->isr |= VIRTIO_ISR_CONFIG;
    vio_intx_update(v);
    vio_msix_raise(v, 0);
}

/* ============================== reset (D3) ============================= */

void virtio_reset_device(virtio_dev_t *v) {
    if (!v || !v->desc) return;
    v->status = 0;
    v->isr = 0;
    v->guest_features = 0;
    v->dfselect = v->gfselect = 0;
    v->queue_select = 0;
    for (int i = 0; i < VIRTIO_MAX_QUEUES; i++) {
        virtio_queue_t *q = &v->q[i];
        q->size = (i < v->desc->num_queues) ? v->desc->queue_max[i] : 0;
        q->enable = 0;
        q->desc = q->driver = q->device = 0;
        q->last_avail = 0;
        q->msix_vector = VIRTIO_MSI_NO_VECTOR;
    }
    /* Vector assignments are device state (reset re-arms them);
     * the MSI-X table and Enable/Function-Mask are PCI-function
     * state and survive a device reset untouched (XHCI rule). */
    v->msix_config = VIRTIO_MSI_NO_VECTOR;
    memset(v->msix_pending, 0, sizeof v->msix_pending);
    if (v->desc->reset) v->desc->reset(v);
    vio_intx_update(v);
}

/* FEATURES_OK gate (D3): the bit is kept iff the driver acked
 * VERSION_1 (the modern device must reject a feature word without
 * it) and the device half accepts the rest.  The queue engine's
 * own gate is separate and lenient -- see virtio_queue_ready. */
static void vio_set_status(virtio_dev_t *v, uint8_t val) {
    if (val == 0) { virtio_reset_device(v); return; }
    if (val & VIO_STATUS_FEATURES_OK) {
        int ok = (v->guest_features & (1ULL << VIRTIO_F_VERSION_1)) != 0;
        if (ok && v->desc->validate_features)
            ok = v->desc->validate_features(v, v->guest_features) == 0;
        if (!ok) val &= (uint8_t)~VIO_STATUS_FEATURES_OK;
    }
    v->status = val;
}

int virtio_queue_ready(virtio_dev_t *v, int qi) {
    if (qi < 0 || qi >= v->desc->num_queues) return 0;
    if (!(v->status & VIO_STATUS_DRIVER_OK)) return 0;
    virtio_queue_t *q = &v->q[qi];
    return q->enable && q->size && q->desc && q->driver && q->device;
}

/* ============================== ring engine ============================ */

/* Ring-region extents (no EVENT_IDX fields exist: the feature is
 * not offered, so neither ring carries the trailing event word). */
static int vio_rings_ok(virtio_queue_t *q) {
    uint32_t n = q->size;
    return vio_ram_ok(q->desc,   16ull * n)
        && vio_ram_ok(q->driver, 4ull + 2ull * n)
        && vio_ram_ok(q->device, 4ull + 8ull * n);
}

static void vio_used_push(virtio_dev_t *v, int qi, uint16_t head,
                          uint32_t used_len) {
    virtio_queue_t *q = &v->q[qi];
    machine_t *m = v->m;
    uint16_t used_idx = vio_rd16(m, q->device + 2);
    uint32_t slot = used_idx % q->size;
    vio_wr32(m, q->device + 4 + 8ull * slot, head);
    vio_wr32(m, q->device + 8 + 8ull * slot, used_len);
    vio_wr16(m, q->device + 2, (uint16_t)(used_idx + 1));
    /* D2 interrupt policy: no EVENT_IDX, so the avail flags bit is
     * the whole rule.  The ISR byte latches in every mode; the INTx
     * level retires under MSI-X and the queue's own vector fires
     * instead (V5). */
    if (!(vio_rd16(m, q->driver) & VRING_AVAIL_F_NO_INTERRUPT)) {
        v->isr |= VIRTIO_ISR_QUEUE;
        vio_intx_update(v);
        vio_msix_raise(v, qi + 1);
    }
}

void virtio_complete(virtio_dev_t *v, virtio_req_t *r, uint32_t used_len) {
    vio_used_push(v, r->queue, r->head, used_len);
}

int virtio_pop(virtio_dev_t *v, int qi, virtio_req_t *r) {
    memset(r, 0, sizeof *r);
    r->v = v;
    r->queue = qi;
    if (!virtio_queue_ready(v, qi)) return 0;
    virtio_queue_t *q = &v->q[qi];
    machine_t *m = v->m;
    if (!vio_rings_ok(q)) { vio_bad(v, "ring outside RAM", qi, 0); return 0; }

    uint16_t avail_idx = vio_rd16(m, q->driver + 2);
    if (avail_idx == q->last_avail) return 0;

    uint16_t head = vio_rd16(m, q->driver + 4 + 2ull * (q->last_avail % q->size));
    q->last_avail++;
    if (head >= q->size) {
        /* The head itself is outside the table: completing it would
         * hand the guest a used elem for a descriptor it never had.
         * Drop it (QEMU's "desc index out of range" class). */
        vio_bad(v, "head index", qi, head);
        return -1;
    }
    r->head = head;

    uint16_t idx = head;
    int malformed = 0;
    const char *why = "";
    for (int count = 0; ; count++) {
        if (count >= q->size) { malformed = 1; why = "chain loop"; break; }
        uint64_t dp = q->desc + 16ull * idx;
        uint64_t addr = vio_rd64(m, dp);
        uint32_t len  = vio_rd32(m, dp + 8);
        uint16_t flags = vio_rd16(m, dp + 12);
        uint16_t next  = vio_rd16(m, dp + 14);
        if (flags & VRING_DESC_F_INDIRECT) { malformed = 1; why = "indirect not offered"; break; }
        if (!vio_ram_ok(addr, len))        { malformed = 1; why = "buffer outside RAM"; break; }
        virtio_seg_t seg = { addr, len };
        if (flags & VRING_DESC_F_WRITE) { r->in[r->n_in++] = seg;   r->in_len += len; }
        else                            { r->out[r->n_out++] = seg; r->out_len += len; }
        if (!(flags & VRING_DESC_F_NEXT)) break;
        if (next >= q->size) { malformed = 1; why = "next index"; break; }
        idx = next;
    }
    if (malformed) {
        /* The chain has a trustworthy head but a broken interior:
         * complete it with len 0 and no data movement, so a polling
         * guest observes a finished, failed request (for blk the
         * untouched 0xFF status sentinel says exactly that) instead
         * of hanging on a descriptor the device swallowed. */
        vio_bad(v, why, qi, head);
        vio_used_push(v, qi, head, 0);
        return -1;
    }
    return 1;
}

/* ============================== DMA helpers ============================ */

uint32_t virtio_dma_read(virtio_dev_t *v, uint64_t pa, void *buf, uint32_t len) {
    if (!v->m || !vio_ram_ok(pa, len)) return 0;
    uint8_t *dst = buf;
    uint32_t done = 0;
    while (done < len) {
        uint32_t left = len - done;
        if (left >= 8) {
            uint64_t x = mem_read(v->m, pa + done, 8);
            memcpy(dst + done, &x, 8);
            done += 8;
        } else {
            dst[done] = (uint8_t)mem_read(v->m, pa + done, 1);
            done += 1;
        }
    }
    return done;
}

uint32_t virtio_dma_write(virtio_dev_t *v, uint64_t pa, const void *buf, uint32_t len) {
    if (!v->m || !vio_ram_ok(pa, len)) return 0;
    const uint8_t *src = buf;
    uint32_t done = 0;
    while (done < len) {
        uint32_t left = len - done;
        if (left >= 8) {
            uint64_t x;
            memcpy(&x, src + done, 8);
            mem_write(v->m, pa + done, 8, x);
            done += 8;
        } else {
            mem_write(v->m, pa + done, 1, src[done]);
            done += 1;
        }
    }
    return done;
}

static uint32_t vio_stream_copy(virtio_req_t *r, const virtio_seg_t *segs,
                                int n, uint32_t off, void *buf,
                                uint32_t len, int to_guest) {
    uint8_t *p = buf;
    uint32_t done = 0;
    for (int i = 0; i < n && done < len; i++) {
        if (off >= segs[i].len) { off -= segs[i].len; continue; }
        uint32_t chunk = segs[i].len - off;
        if (chunk > len - done) chunk = len - done;
        uint32_t moved = to_guest
            ? virtio_dma_write(r->v, segs[i].addr + off, p + done, chunk)
            : virtio_dma_read(r->v, segs[i].addr + off, p + done, chunk);
        done += moved;
        if (moved < chunk) break;
        off = 0;
    }
    return done;
}

uint32_t virtio_req_read(virtio_req_t *r, uint32_t off, void *buf, uint32_t len) {
    return vio_stream_copy(r, r->out, r->n_out, off, buf, len, 0);
}

uint32_t virtio_req_write(virtio_req_t *r, uint32_t off, const void *buf, uint32_t len) {
    return vio_stream_copy(r, r->in, r->n_in, off, (void *)buf, len, 1);
}

/* ============================== common config ========================== */
/*
 * The common-cfg block is field-typed (u8 status, u16 queue fields,
 * u32 feature banks, u64 ring addresses) and guests measured use
 * exact-width accesses -- but an image-based register file makes
 * every sub-width or unaligned access compose for free: reads
 * serialize state into a 0x40 image, writes merge into the image
 * and re-apply the fields with their semantics.  Fields are applied
 * selects-first (so queue fields land on the newly selected queue),
 * status last (so a same-access FEATURES_OK sees the feature word
 * written alongside it).
 */
#define VIO_COMMON_SIZE 0x40

static void vio_common_image(virtio_dev_t *v, uint8_t img[VIO_COMMON_SIZE]) {
    memset(img, 0, VIO_COMMON_SIZE);
    uint64_t offered = v->desc->features | (1ULL << VIRTIO_F_VERSION_1);
    put32(img + 0x00, v->dfselect);
    put32(img + 0x04, v->dfselect == 0 ? (uint32_t)offered
                      : v->dfselect == 1 ? (uint32_t)(offered >> 32) : 0);
    put32(img + 0x08, v->gfselect);
    put32(img + 0x0C, v->gfselect == 0 ? (uint32_t)v->guest_features
                      : v->gfselect == 1 ? (uint32_t)(v->guest_features >> 32) : 0);
    put16(img + 0x10, v->msix_config);
    put16(img + 0x12, (uint16_t)v->desc->num_queues);
    img[0x14] = v->status;
    img[0x15] = v->generation;
    put16(img + 0x16, v->queue_select);
    if (v->queue_select < (unsigned)v->desc->num_queues) {
        virtio_queue_t *q = &v->q[v->queue_select];
        put16(img + 0x18, q->size);
        put16(img + 0x1A, q->msix_vector);
        put16(img + 0x1C, q->enable);
        put16(img + 0x1E, v->queue_select);   /* queue_notify_off = index */
        put64(img + 0x20, q->desc);
        put64(img + 0x28, q->driver);
        put64(img + 0x30, q->device);
    }
    /* 0x38 queue_notif_config_data / 0x3A queue_reset: read 0 -- the
     * features behind them are not offered. */
}

/* Only fields the access actually touched take their semantics --
 * re-applying the whole image would re-run a status-0 reset on
 * every queue-register write during early bring-up. */
static int vio_touched(uint32_t off, int size, uint32_t field, uint32_t flen) {
    return off < field + flen && off + (uint32_t)size > field;
}

static void vio_common_apply(virtio_dev_t *v, const uint8_t img[VIO_COMMON_SIZE],
                             uint32_t off, int size) {
    uint64_t offered = v->desc->features | (1ULL << VIRTIO_F_VERSION_1);
    if (vio_touched(off, size, 0x00, 4)) v->dfselect = le32_of(img + 0x00);
    if (vio_touched(off, size, 0x08, 4)) v->gfselect = le32_of(img + 0x08);
    /* D3: driver_feature is masked to the offer (QEMU precedent --
     * an unoffered bit can never become acked state). */
    if (vio_touched(off, size, 0x0C, 4)) {
        if (v->gfselect == 0)
            v->guest_features = (v->guest_features & ~0xFFFFFFFFull)
                              | (le32_of(img + 0x0C) & (uint32_t)offered);
        else if (v->gfselect == 1)
            v->guest_features = (v->guest_features & 0xFFFFFFFFull)
                              | ((uint64_t)(le32_of(img + 0x0C) & (uint32_t)(offered >> 32)) << 32);
    }
    if (vio_touched(off, size, 0x10, 2))
        v->msix_config = vio_msix_assign(v, le16_of(img + 0x10));
    if (vio_touched(off, size, 0x16, 2)) v->queue_select = le16_of(img + 0x16);
    if (v->queue_select < (unsigned)v->desc->num_queues) {
        virtio_queue_t *q = &v->q[v->queue_select];
        if (vio_touched(off, size, 0x18, 2)) {
            uint16_t want = le16_of(img + 0x18);
            uint16_t max_size = v->desc->queue_max[v->queue_select];
            q->size = (want == 0 || want > max_size) ? max_size : want;
        }
        if (vio_touched(off, size, 0x1A, 2))
            q->msix_vector = vio_msix_assign(v, le16_of(img + 0x1A));
        if (vio_touched(off, size, 0x1C, 2)) q->enable = le16_of(img + 0x1C) & 1;
        if (vio_touched(off, size, 0x20, 8)) q->desc   = le64_of(img + 0x20);
        if (vio_touched(off, size, 0x28, 8)) q->driver = le64_of(img + 0x28);
        if (vio_touched(off, size, 0x30, 8)) q->device = le64_of(img + 0x30);
    }
    if (vio_touched(off, size, 0x14, 1)) vio_set_status(v, img[0x14]);
}

static uint64_t vio_common_read(virtio_dev_t *v, uint32_t off, int size) {
    uint8_t img[VIO_COMMON_SIZE + 8];
    memset(img, 0, sizeof img);
    vio_common_image(v, img);
    uint64_t val = 0;
    for (int i = 0; i < size && off + i < sizeof img; i++)
        val |= (uint64_t)img[off + i] << (8 * i);
    return val;
}

static void vio_common_write(virtio_dev_t *v, uint32_t off, int size, uint64_t val) {
    uint8_t img[VIO_COMMON_SIZE + 8];
    memset(img, 0, sizeof img);
    vio_common_image(v, img);
    for (int i = 0; i < size && off + i < sizeof img; i++)
        img[off + i] = (uint8_t)(val >> (8 * i));
    vio_common_apply(v, img, off, size);
}

/* ============================== BAR0 MMIO ============================== */

/* V5: MSI-X table (RW) + PBA (RO) in the notify page tail.  The table
 * is image-based like the common config, so sub-dword guest accesses
 * compose.  The PBA projects each source's pending flag through its
 * assigned vector (NO_VECTOR sources never show). */
#define VIO_MSIX_TABLE_END(v) \
    ((uint32_t)VIRTIO_OFF_MSIX_TABLE + 16u * (uint32_t)vio_msix_nvec(v))

static void vio_msix_image(virtio_dev_t *v, uint8_t *img) {
    for (int i = 0; i < vio_msix_nvec(v); i++) {
        put32(img + 16 * i + 0,  v->msix_table[i].addr_lo);
        put32(img + 16 * i + 4,  v->msix_table[i].addr_hi);
        put32(img + 16 * i + 8,  v->msix_table[i].data);
        put32(img + 16 * i + 12, v->msix_table[i].ctrl);
    }
}

static void vio_msix_table_write(virtio_dev_t *v, uint32_t off, int size,
                                 uint64_t val) {
    uint8_t img[(VIRTIO_MAX_QUEUES + 1) * 16];
    memset(img, 0, sizeof img);
    vio_msix_image(v, img);
    uint32_t at = off - VIRTIO_OFF_MSIX_TABLE;
    for (int i = 0; i < size && at + i < sizeof img; i++)
        img[at + i] = (uint8_t)(val >> (8 * i));
    for (int i = 0; i < vio_msix_nvec(v); i++) {
        v->msix_table[i].addr_lo = le32_of(img + 16 * i + 0);
        v->msix_table[i].addr_hi = le32_of(img + 16 * i + 4);
        v->msix_table[i].data    = le32_of(img + 16 * i + 8);
        /* control: bit 0 is the per-vector mask, the rest reads 0 */
        v->msix_table[i].ctrl    = le32_of(img + 16 * i + 12) & 1u;
    }
    vio_msix_scan(v);   /* an unmask here may release a pending message */
}

static uint64_t vio_msix_read(virtio_dev_t *v, uint32_t off, int size) {
    uint8_t img[(VIRTIO_MAX_QUEUES + 1) * 16 + 8];
    memset(img, 0, sizeof img);
    uint64_t val = 0;
    if (off >= VIRTIO_OFF_MSIX_TABLE && off < VIO_MSIX_TABLE_END(v)) {
        vio_msix_image(v, img);
        uint32_t at = off - VIRTIO_OFF_MSIX_TABLE;
        for (int i = 0; i < size && at + i < sizeof img; i++)
            val |= (uint64_t)img[at + i] << (8 * i);
        return val;
    }
    if (off >= VIRTIO_OFF_MSIX_PBA && off < VIRTIO_OFF_MSIX_PBA + 8u) {
        uint32_t bits = 0;
        for (int src = 0; src < vio_msix_nvec(v); src++) {
            uint16_t vec = vio_src_vector(v, src);
            if (v->msix_pending[src] && vec < vio_msix_nvec(v))
                bits |= 1u << vec;
        }
        put32(img, bits);
        uint32_t at = off - VIRTIO_OFF_MSIX_PBA;
        for (int i = 0; i < size && at + i < 8; i++)
            val |= (uint64_t)img[at + i] << (8 * i);
        return val;
    }
    return 0;
}

static uint64_t vio_mmio_read(void *ctx, uint64_t addr, int size) {
    virtio_dev_t *v = ctx;
    if (size <= 0 || size > 8) return 0;
    uint32_t off = (uint32_t)(addr - v->bar_base);
    if (off < VIRTIO_OFF_ISR) return vio_common_read(v, off, size);
    if (off < VIRTIO_OFF_DEVICE) {
        /* ISR: the byte at +0 reads-and-clears (which is what drops
         * the INTx level); the rest of the page reads 0. */
        if (off != VIRTIO_OFF_ISR) return 0;
        uint8_t isr = v->isr;
        v->isr = 0;
        vio_intx_update(v);
        return isr;
    }
    if (off < VIRTIO_OFF_NOTIFY) {
        uint32_t doff = off - VIRTIO_OFF_DEVICE;
        uint64_t val = 0;
        for (int i = 0; i < size; i++) {
            uint8_t b = (v->desc->config && doff + i < v->desc->config_len)
                      ? v->desc->config[doff + i] : 0;
            val |= (uint64_t)b << (8 * i);
        }
        return val;
    }
    return vio_msix_read(v, off, size);   /* notify page: the doorbells
                 * themselves are write-only (no NOTIF_CONFIG_DATA);
                 * the MSI-X table/PBA in the page tail read here */
}

static void vio_mmio_write(void *ctx, uint64_t addr, int size, uint64_t val) {
    virtio_dev_t *v = ctx;
    if (size <= 0 || size > 8) return;
    uint32_t off = (uint32_t)(addr - v->bar_base);
    if (off < VIRTIO_OFF_ISR) { vio_common_write(v, off, size, val); return; }
    if (off < VIRTIO_OFF_DEVICE) return;                    /* ISR: RO */
    if (off < VIRTIO_OFF_NOTIFY) {
        if (v->desc->config_write)
            v->desc->config_write(v, off - VIRTIO_OFF_DEVICE,
                                  (uint32_t)val, size);
        return;
    }
    if (off >= VIRTIO_OFF_MSIX_TABLE && off < VIO_MSIX_TABLE_END(v)) {
        vio_msix_table_write(v, off, size, val);
        return;
    }
    if (off >= VIRTIO_OFF_MSIX_PBA && off < VIRTIO_OFF_MSIX_PBA + 8u)
        return;                                            /* PBA: RO */
    unsigned qi = (off - VIRTIO_OFF_NOTIFY) / VIRTIO_NOTIFY_MULT;
    if (qi < (unsigned)v->desc->num_queues && v->desc->notify)
        v->desc->notify(v, (int)qi);
}

/* ============================== BAR plumbing =========================== */

static void vio_set_bar(virtio_dev_t *v, uint32_t base) {
    int enabled = base != 0;
    v->bar_base = base & ~(VIRTIO_BAR_SIZE - 1u);
    if (!v->m) return;
    for (mmio_region_t *r = v->m->mmio_list; r; r = r->next) {
        if (r->ctx == v && r->size == VIRTIO_BAR_SIZE) {
            r->base = v->bar_base;
            r->enabled = enabled;
            return;
        }
    }
}

static uint32_t vio_bar_value(virtio_dev_t *v, pci_dev_t *d) {
    if (v->bar_probe) return ~(VIRTIO_BAR_SIZE - 1u) & 0xFFFFFFF0u;
    return le32_of(d->cfg + 0x10);
}

/* Vendor capabilities (virtio 1.x 4.1.4): common(1), notify(2, with
 * the multiplier), isr(3), device(4 -- only when the device has a
 * config space to point at).  Page-granular `length` like QEMU, so
 * a driver mapping by length always covers the structure. */
static void vio_seed_caps(virtio_dev_t *v) {
    pci_dev_t *d = v->pci;
    static const struct { uint8_t at, type, len, next; uint16_t off; } caps[4] = {
        { 0x40, 1, 16, 0x50, VIRTIO_OFF_COMMON },
        { 0x50, 2, 20, 0x64, VIRTIO_OFF_NOTIFY },
        { 0x64, 3, 16, 0x74, VIRTIO_OFF_ISR    },
        { 0x74, 4, 16, 0x00, VIRTIO_OFF_DEVICE },
    };
    int ncap = v->desc->config_len ? 4 : 3;
    d->cfg[0x06] |= 0x10;                       /* Status: Cap List */
    d->cfg[0x34] = 0x40;                        /* Capabilities Ptr */
    memset(d->cfg + 0x40, 0, 0x50);
    for (int i = 0; i < ncap; i++) {
        uint8_t *c = d->cfg + caps[i].at;
        c[0] = 0x09;                            /* vendor-specific */
        c[1] = (i + 1 < ncap) ? caps[i].next : VIRTIO_CAP_MSIX;
        c[2] = caps[i].len;
        c[3] = caps[i].type;
        c[4] = 0;                               /* BAR 0 */
        put32(c + 8, caps[i].off);
        put32(c + 12, 0x1000);
    }
    put32(d->cfg + 0x50 + 16, VIRTIO_NOTIFY_MULT);
    /* V5: MSI-X capability terminates the chain (XHCI M1 shape).
     * Table Size-1 and both offsets are RO and re-asserted on every
     * cfg write; Enable/Function-Mask are the guest's bits, captured
     * into the device state in vio_cfg_write BEFORE this re-seed. */
    uint8_t *mc = d->cfg + VIRTIO_CAP_MSIX;
    mc[0] = 0x11;                               /* Capability ID: MSI-X */
    mc[1] = 0x00;                               /* Next: end of list */
    put16(mc + 2, (uint16_t)((vio_msix_nvec(v) - 1)
                  | (v->msix_fmask ? 0x4000 : 0)
                  | (v->msix_enable ? 0x8000 : 0)));
    put32(mc + 4, VIRTIO_OFF_MSIX_TABLE);       /* BIR 0 in low bits */
    put32(mc + 8, VIRTIO_OFF_MSIX_PBA);
}

static uint64_t vio_cfg_read(pci_dev_t *d, int reg, int size) {
    virtio_dev_t *v = vio_of_pci(d);
    if (!v || size <= 0 || size > 8) return 0;
    uint32_t bar = vio_bar_value(v, d);
    uint64_t value = 0;
    for (int i = 0; i < size; i++) {
        int off = reg + i;
        uint8_t byte = 0;
        if (off >= 0x10 && off < 0x14)
            byte = (uint8_t)(bar >> (8 * (off - 0x10)));
        else if (!((off >= 0x14 && off < 0x28) || (off >= 0x30 && off < 0x34))
                 && off >= 0 && off < 256)
            byte = d->cfg[off];
        value |= (uint64_t)byte << (8 * i);
    }
    return value;
}

static void vio_cfg_write(pci_dev_t *d, int reg, int size, uint64_t val) {
    virtio_dev_t *v = vio_of_pci(d);
    if (!v || size <= 0 || size > 8) return;
    if (reg == 0x10 && size == 4 && (uint32_t)val == 0xFFFFFFFFu) {
        v->bar_probe = 1;
        return;
    }
    int touches_bar = reg < 0x14 && reg + size > 0x10;
    if (touches_bar) v->bar_probe = 0;
    pci_default_cfg_write(d, reg, size, val);
    /* BAR1..5 and the expansion-ROM BAR are not implemented. */
    memset(d->cfg + 0x14, 0, 0x14);
    memset(d->cfg + 0x30, 0, 4);
    if (touches_bar) {
        uint32_t base = le32_of(d->cfg + 0x10) & ~(VIRTIO_BAR_SIZE - 1u) & ~0xFu;
        put32(d->cfg + 0x10, base);      /* 32-bit memory BAR: type 0 */
        vio_set_bar(v, base);
    }
    /* V5: capture the MSI-X Message Control RW bits before the
     * re-seed below rewrites the cap around them.  A write that
     * never touched the cap leaves the seeded bits in place, so
     * this capture is a no-op then. */
    int old_enable = v->msix_enable;
    uint16_t mctl = le16_of(d->cfg + VIRTIO_CAP_MSIX + 2);
    v->msix_enable = (mctl & 0x8000) != 0;
    v->msix_fmask  = (mctl & 0x4000) != 0;
    vio_seed_caps(v);                    /* RO bytes survive any write */
    /* Route edges: disabling hands the latched ISR back to INTx and
     * retires the pending flags with it; enabling or unmasking may
     * release a held message. */
    if (old_enable && !v->msix_enable)
        memset(v->msix_pending, 0, sizeof v->msix_pending);
    vio_intx_update(v);
    vio_msix_scan(v);
}

/* ============================== attach ================================= */

void virtio_pci_add(machine_t *m, virtio_dev_t *v, int pci_dev_no,
                    const char *name, uint16_t device_id,
                    uint8_t class_, uint8_t subclass, uint8_t progif,
                    uint32_t bar_base, int irq,
                    const virtio_device_desc_t *desc, void *ctx) {
    memset(v, 0, sizeof *v);
    v->m = m;
    v->desc = desc;
    v->ctx = ctx;
    v->irq = irq;
    v->bar_base = bar_base;
    /* MSI-X power-on: every table entry masked (the XHCI rule). */
    for (int i = 0; i < VIRTIO_MAX_QUEUES + 1; i++)
        v->msix_table[i].ctrl = 1;

    pci_dev_t *p = pci_add_device(m, 0, pci_dev_no, 0, name,
                                  VIRTIO_PCI_VENDOR, device_id,
                                  class_, subclass, progif);
    v->pci = p;
    p->cfg[0x08] = 0x01;                             /* revision 1 */
    /* subsystem vendor 1af4, subsystem id = device id (QEMU modern) */
    p->cfg[0x2C] = 0xF4; p->cfg[0x2D] = 0x1A;
    p->cfg[0x2E] = (uint8_t)device_id; p->cfg[0x2F] = (uint8_t)(device_id >> 8);
    p->cfg[0x3C] = (uint8_t)irq;                     /* Interrupt Line */
    p->cfg[0x3D] = 0x01;                             /* Interrupt Pin INTA# */
    put32(p->cfg + 0x10, bar_base);                  /* seeded BAR0 */
    p->cfg_read = vio_cfg_read;
    p->cfg_write = vio_cfg_write;
    vio_seed_caps(v);

    virtio_reset_device(v);
    mem_register_mmio(m, bar_base, VIRTIO_BAR_SIZE,
                      vio_mmio_read, vio_mmio_write, v, name);
    int known = 0;
    for (int i = 0; i < g_vio_n; i++) known |= g_vio[i] == v;
    if (!known && g_vio_n < VIO_REGISTRY_MAX) g_vio[g_vio_n++] = v;
    mlog(&m->log, "[virtio] %s 1af4:%04x at 0:%02d.0 BAR0 0x%x IRQ %d (%d queues)\n",
         name, device_id, pci_dev_no, bar_base, irq, desc->num_queues);
}

/* ============================== snapshot =============================== */

void virtio_snap(virtio_dev_t *v, snap_t *s, const char *tag) {
    virtio_dev_t t = *v;
    if (!snap_loading(s)) {
        t.m = NULL; t.pci = NULL; t.desc = NULL; t.ctx = NULL;
    }
    snap_bytes(s, tag, &t, sizeof t);
    if (snap_loading(s)) {
        machine_t *m = v->m;
        pci_dev_t *p = v->pci;
        const virtio_device_desc_t *d = v->desc;
        void *c = v->ctx;
        *v = t;
        v->m = m; v->pci = p; v->desc = d; v->ctx = c;
        /* pci_snap restored the cfg BAR bytes but does not rebind the
         * decode; the serialized live base does (ahci_snap rule), and
         * the INTx level is re-driven from the restored ISR byte.
         * The MSI-X plane rides in the same struct image (table,
         * enable, assignments, pending); a source that was pending
         * at save time re-fires on load exactly as it would have on
         * the next unmask of the uninterrupted run. */
        vio_set_bar(v, v->bar_base);
        v->intx_level = 0;
        vio_intx_update(v);
        vio_msix_scan(v);
    }
}
