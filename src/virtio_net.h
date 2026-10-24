/* virtio_net.h -- VIRTIO V4: modern virtio-net on the V1 transport
 * core (docs/plans/VIRTIO_PLAN.md, decision D6).  RX queue 0 /
 * TX queue 1, the 12-byte VERSION_1 header, feature offer
 * VERSION_1 | VIRTIO_NET_F_MAC and nothing else -- the exact set
 * the guest's by-the-book driver negotiates.  The backend is the
 * same TAP UDP endpoint the e1000 uses (one owner only: main
 * refuses --tap together with --virtio-net), and snapshots are
 * refused while attached for the same reason (--tap today).
 */
#ifndef EMU_VIRTIO_NET_H
#define EMU_VIRTIO_NET_H

#include <stdint.h>
#include "machine.h"

/* CLI entry (--virtio-net[=host:port]): binds the TAP endpoint
 * (empty hostport = the 127.0.0.1:8080 ground rule) and publishes
 * the 1af4:1041 function at 0:06.0.  0 ok, -1 on failure. */
int  virtio_net_attach(machine_t *m, const char *hostport);

/* Unit entry (e1000_test_add shape): the device model, no socket;
 * TX frames count as dropped, RX is driven by virtio_net_rx_frame. */
void virtio_net_test_add(machine_t *m);

int  virtio_net_present(void);

/* One Ethernet frame into the guest's RX queue (the e1000_rx_frame
 * contract): dropped silently with a counter when the queue is
 * not ready or out of buffers; a frame longer than the guest's
 * buffer is truncated (the e1000 precedent) -- counted too. */
void virtio_net_rx_frame(machine_t *m, const uint8_t *frame, unsigned len);

/* TAP ingress at the device cadence (devices_tick, next to
 * e1000_tick): up to 8 datagrams per call. */
void virtio_net_tick(void);

/* Lane observability (e1000_tx_stats shape): the device logs
 * nothing per frame, so harnesses read counters instead. */
void virtio_net_stats(uint64_t *tx_sent, uint64_t *tx_dropped,
                      uint64_t *rx_done, uint64_t *rx_dropped);

#endif /* EMU_VIRTIO_NET_H */
