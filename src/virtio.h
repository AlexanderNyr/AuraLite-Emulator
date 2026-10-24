/* virtio.h -- VIRTIO V1: the modern (1.x) virtio-pci transport core
 * (docs/plans/VIRTIO_PLAN.md).  Split virtqueues, vendor-capability
 * transport, synchronous completion on notify.  Device halves
 * (virtio-blk V2, virtio-net V4) plug in through virtio_device_desc_t.
 *
 * What the core advertises is exactly what it implements (plan rule
 * 1): the transport offer is VIRTIO_F_VERSION_1 and nothing else
 * (no INDIRECT_DESC 28, no EVENT_IDX 29, no PACKED 34); the device
 * desc adds its own feature bits, each pinned by a vector in the
 * phase that offers it.  Interrupts are legacy INTx behind the ISR
 * byte until the guest turns on the MSI-X capability (V5): then the
 * pin retires and queue/config sources fire the vectors the guest
 * assigned through msix_config / queue_msix_vector, with the table
 * and PBA living in the BAR0 tail like the XHCI's (MSI M1/M3 shape,
 * including masked-pending release on unmask).
 *
 * The consumer contract is measured (V0 section 2): the guest's
 * virtio-net driver negotiates by the book and re-reads
 * FEATURES_OK, while its virtio-blk driver negotiates nothing and
 * goes straight to DRIVER_OK (findings N1/N2).  Decision D3 splits
 * the difference exactly where QEMU does: FEATURES_OK survives iff
 * VERSION_1 was acked (the spec's readback), but the queue engine
 * runs under DRIVER_OK + queue_enable regardless, so the blind
 * driver still transfers.  Both halves are pinned in
 * tests/test_virtio.c.
 */
#ifndef EMU_VIRTIO_H
#define EMU_VIRTIO_H

#include <stdint.h>
#include "machine.h"
#include "snapshot.h"

struct pci_dev;

/* ---- PCI identity (D1) ------------------------------------------------- */
#define VIRTIO_PCI_VENDOR     0x1AF4
#define VIRTIO_PCI_NET_ID     0x1041   /* modern device ids: 0x1040+type */
#define VIRTIO_PCI_BLK_ID     0x1042
#define VIRTIO_BLK_PCI_DEV    5        /* 0:05.0 (0:04.0 is the opt-in e1000) */
#define VIRTIO_NET_PCI_DEV    6        /* 0:06.0 */
#define VIRTIO_BLK_BAR_BASE   0xFEB30000u
#define VIRTIO_NET_BAR_BASE   0xFEB34000u
#define VIRTIO_BLK_IRQ        5        /* measured free ISA lines (V1 census:
                                        * 0 PIT, 1 KBC, 8 RTC, 9 SCI, 10 XHCI,
                                        * 11 e1000, 14/15 AHCI are taken) */
#define VIRTIO_NET_IRQ        7

/* One 32-bit memory BAR0, 16 KiB, QEMU's region offsets (D1). */
#define VIRTIO_BAR_SIZE       0x4000u
#define VIRTIO_OFF_COMMON     0x0000
#define VIRTIO_OFF_ISR        0x1000
#define VIRTIO_OFF_DEVICE     0x2000
#define VIRTIO_OFF_NOTIFY     0x3000
#define VIRTIO_NOTIFY_MULT    4
/* V5: MSI-X table/PBA in the notify page's unused tail (the
 * doorbells consume 4 bytes per queue at its head).  The notify
 * capability keeps its QEMU offset and page length; drivers map
 * the BAR and hit table/PBA through the MSI-X cap's own offsets,
 * which is exactly where QEMU puts them relative to the page. */
#define VIRTIO_OFF_MSIX_TABLE 0x3800
#define VIRTIO_OFF_MSIX_PBA   0x3C00
#define VIRTIO_CAP_MSIX       0x84     /* cfg offset, after vendor caps */

/* ---- transport feature bits & status bits ------------------------------ */
#define VIRTIO_F_VERSION_1    32
#define VIRTIO_MSI_NO_VECTOR  0xFFFF

#define VIO_STATUS_ACK        0x01
#define VIO_STATUS_DRIVER     0x02
#define VIO_STATUS_DRIVER_OK  0x04
#define VIO_STATUS_FEATURES_OK 0x08
#define VIO_STATUS_NEEDS_RESET 0x40
#define VIO_STATUS_FAILED     0x80

/* ---- split-ring constants ---------------------------------------------- */
#define VRING_DESC_F_NEXT     1
#define VRING_DESC_F_WRITE    2
#define VRING_DESC_F_INDIRECT 4
#define VRING_AVAIL_F_NO_INTERRUPT 1
#define VRING_USED_F_NO_NOTIFY     1

#define VIRTIO_ISR_QUEUE      0x01
#define VIRTIO_ISR_CONFIG     0x02

#define VIRTIO_MAX_QUEUES     4
#define VIRTIO_MAX_QSIZE      256

/* One guest-memory segment of a descriptor chain.  "out" segments are
 * device-readable (driver -> device), "in" segments device-writable. */
typedef struct {
    uint64_t addr;
    uint32_t len;
} virtio_seg_t;

/* A popped chain (virtio_pop).  The struct is ~8 KiB by design (a
 * 256-entry chain worst case): device code keeps one as scratch in
 * its own state instead of on a deep stack. */
typedef struct virtio_req {
    struct virtio_dev *v;
    int      queue;
    uint16_t head;                 /* descriptor index for the used elem */
    int      n_out, n_in;
    uint32_t out_len, in_len;      /* summed byte counts */
    virtio_seg_t out[VIRTIO_MAX_QSIZE];
    virtio_seg_t in[VIRTIO_MAX_QSIZE];
} virtio_req_t;

/* Device-half description: everything device-specific the core needs.
 * `features` holds device bits only; the core ORs in VERSION_1.
 * Callbacks may be NULL: no notify (device polls via its own tick),
 * no feature validation (accept any subset), no reset extras,
 * read-only device config. */
struct virtio_dev;
typedef struct {
    uint64_t      features;
    int           num_queues;
    uint16_t      queue_max[VIRTIO_MAX_QUEUES];
    const uint8_t *config;        /* device-config bytes (may be NULL) */
    uint32_t      config_len;
    void (*notify)(struct virtio_dev *v, int queue);
    int  (*validate_features)(struct virtio_dev *v, uint64_t acked);
    void (*reset)(struct virtio_dev *v);
    void (*config_write)(struct virtio_dev *v, uint32_t off,
                         uint32_t val, int size);
} virtio_device_desc_t;

typedef struct {
    uint16_t size;                /* effective size (max until written) */
    uint16_t enable;
    uint64_t desc, driver, device;
    uint16_t last_avail;          /* avail idx last consumed by the core */
    uint16_t msix_vector;         /* queue_msix_vector (V5) */
} virtio_queue_t;

typedef struct virtio_dev {
    machine_t        *m;
    struct pci_dev   *pci;
    const virtio_device_desc_t *desc;
    void             *ctx;        /* device-driver state (blk/net struct) */
    uint32_t          bar_base;
    int               bar_probe;
    int               irq;
    uint8_t           status;
    uint8_t           isr;
    uint8_t           generation;       /* config_generation (D3: still
                                         * unless virtio_config_changed) */
    int               intx_level;
    uint64_t          guest_features;   /* acked, masked to the offer */
    uint32_t          dfselect, gfselect;
    uint16_t          queue_select;
    unsigned          bad_chains;       /* D2: log budget for bad chains */
    virtio_queue_t    q[VIRTIO_MAX_QUEUES];

    /* --- MSI-X (V5) ------------------------------------------------------
     * Table size is num_queues+1 (entry 0 serves the config source
     * when the guest assigns it, queues take the rest); the table
     * lives in caller-visible MMIO, each entry 16 bytes.  Pending
     * is kept per interrupt SOURCE (index 0 = config, 1+i = queue i)
     * rather than per vector, in the MSI_PLAN 2 shape: the source
     * keeps its own pending state and an unmask re-evaluates it.
     * The PBA reads the sources' bits projected through their
     * assigned vectors.  Enable/Function-Mask are the Message
     * Control RW bits; the table + these bits are PCI-function
     * state (they survive a device reset, which only re-arms the
     * vector assignments to NO_VECTOR). */
    uint16_t          msix_config;
    struct { uint32_t addr_lo, addr_hi, data, ctrl; }
                      msix_table[VIRTIO_MAX_QUEUES + 1];
    uint8_t           msix_pending[VIRTIO_MAX_QUEUES + 1];
    int               msix_enable;
    int               msix_fmask;
} virtio_dev_t;

/* Register one modern virtio PCI function: seeds config space
 * (revision 1, subsystem vendor 1af4 with the subsystem id equal
 * to the device id -- no measured consumer reads either), lays
 * the vendor capabilities into cfg[0x40..], installs the BAR0
 * MMIO window at `bar_base` and arms INTx on `irq`.  `v` is
 * caller-owned storage (the device half keeps it inside its own
 * state struct). */
void virtio_pci_add(machine_t *m, virtio_dev_t *v, int pci_dev_no,
                    const char *name, uint16_t device_id,
                    uint8_t class_, uint8_t subclass, uint8_t progif,
                    uint32_t bar_base, int irq,
                    const virtio_device_desc_t *desc, void *ctx);

/* Full synchronous device reset -- the same path a guest triggers by
 * writing 0 to device_status (status reads back 0 immediately). */
void virtio_reset_device(virtio_dev_t *v);

/* Snapshot visitor for the transport state (D7): status/features,
 * queue addresses+sizes+consumed cursor, ISR, BAR placement; the
 * load leg rebinds the live BAR window and re-drives INTx (the
 * ahci_snap placement rule -- virtio_blk_snap runs from
 * snapshot.c's visit() next to ahci_snap, before the MMIO map is
 * compared).  Host pointers never enter the file. */
void virtio_snap(virtio_dev_t *v, snap_t *s, const char *tag);

/* The queue engine's run gate (D3): DRIVER_OK set and queue enabled,
 * with ring addresses programmed. */
int  virtio_queue_ready(virtio_dev_t *v, int qi);

/* Pop one chain from queue `qi`'s avail ring into `r`.
 * Returns 1 when `r` holds a valid chain, 0 when the ring is empty,
 * -1 when a malformed chain was consumed and completed with len 0
 * (the burst caller should simply keep popping). */
int  virtio_pop(virtio_dev_t *v, int qi, virtio_req_t *r);

/* Publish the used element for a popped chain (used_len = bytes the
 * device wrote into the in-segments) and signal per the NO_INTERRUPT
 * policy (D2): INTx off the ISR byte, or the queue's MSI-X vector
 * when the guest enabled MSI-X (V5). */
void virtio_complete(virtio_dev_t *v, virtio_req_t *r, uint32_t used_len);

/* Config-change interrupt (ISR bit 1 + generation bump).  No current
 * device mutates config after attach, but the wire is core-owned. */
void virtio_config_changed(virtio_dev_t *v);

/* Data movement helpers over a popped chain's segment streams.
 * Offsets are stream offsets (0 = first byte of the first segment);
 * short copies truncate and return the byte count moved. */
uint32_t virtio_req_read(virtio_req_t *r, uint32_t off,
                         void *buf, uint32_t len);        /* out -> buf */
uint32_t virtio_req_write(virtio_req_t *r, uint32_t off,
                          const void *buf, uint32_t len); /* buf -> in */

/* Bounds-checked DMA against guest RAM (D2): a range that leaves
 * RAM copies nothing and returns 0, so a hostile or buggy guest can
 * never aim a descriptor at an MMIO window. */
uint32_t virtio_dma_read(virtio_dev_t *v, uint64_t pa,
                         void *buf, uint32_t len);
uint32_t virtio_dma_write(virtio_dev_t *v, uint64_t pa,
                          const void *buf, uint32_t len);

#endif /* EMU_VIRTIO_H */
