/* virtio_blk.h -- VIRTIO V2: modern virtio-blk on the V1 transport
 * core (docs/plans/VIRTIO_PLAN.md, decision D5).  One queue, a RAM
 * copy of the host image (the sata_img convention: reads/writes hit
 * the in-memory copy, the host file is never rewritten), the honest
 * 8-byte device config (`capacity` in 512-byte sectors -- the
 * guest's own driver reads it out of the wrong place, finding N2,
 * but the bytes are where the spec puts them), and no device
 * feature bits beyond the transport's VERSION_1.
 */
#ifndef EMU_VIRTIO_BLK_H
#define EMU_VIRTIO_BLK_H

#include <stdint.h>
#include <stddef.h>
#include "machine.h"
#include "snapshot.h"

/* CLI entry (--virtio-blk=path): reads the whole host image into
 * RAM and publishes the 1af4:1042 function at 0:05.0.  0 ok,
 * -1 with the reason already logged (main treats it as fatal --
 * a disk that silently is not there is worse than a refusal). */
int  virtio_blk_attach(machine_t *m, const char *path);

/* Unit entry (e1000_test_add shape): caller-owned image buffer,
 * no file IO. */
void virtio_blk_test_add(machine_t *m, uint8_t *img, size_t len);

int  virtio_blk_present(void);
void virtio_blk_snap(machine_t *m, snap_t *s);

#endif /* EMU_VIRTIO_BLK_H */
