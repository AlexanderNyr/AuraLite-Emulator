/* virtio_net.c -- VIRTIO V4: virtio-net on the V1 core (D6).
 *
 * The guest contract is the measured by-the-book driver
 * (drivers/virtio_net/virtio_net.c): two queues (0 RX, 1 TX)
 * used at size 16, a 12-byte virtio_net_hdr with num_buffers
 * under VERSION_1 even without MRG_RXBUF, single-descriptor
 * 2048-byte RX buffers it recycles, used.len = hdr + frame,
 * and an INTx handler that acks by reading the ISR byte.
 * Everything here is shaped so that driver's checks pass
 * verbatim; nothing beyond MAC is offered, because nothing
 * beyond MAC is implemented (plan rule 1).
 *
 * RX buffers are consumed by ingress (virtio_net_rx_frame /
 * virtio_net_tick), never by the RX doorbell -- the guest's
 * rx_fill notify is a no-op here, like on hardware.  TX frames
 * are gathered across the chain's readable segments, the
 * header stripped, and the datagram handed to the same TAP
 * UDP endpoint the e1000 uses; completion never depends on a
 * responder being up (the e1000 N4 rule), and counters are
 * the only per-frame trace.
 */
#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>
#include "virtio.h"
#include "virtio_net.h"

#define VNET_F_MAC        (1ull << 5)
#define VNET_HDR_LEN      12
#define VNET_QSIZE        128          /* offered max; guest clamps to 16 */
#define VNET_BUF          2048

static const uint8_t VNET_MAC[6] = { 0x52, 0x54, 0x00, 0x12, 0x34, 0x56 };

typedef struct {
    machine_t   *m;
    virtio_dev_t vio;
    virtio_req_t req;
    int          tap_fd;
    int          attached;
    uint8_t      config[6];            /* mac (the whole config) */
    uint8_t      txbuf[VNET_BUF];      /* one gathered frame + hdr */
    uint64_t     tx_sent, tx_dropped;
    uint64_t     rx_done, rx_dropped, rx_truncated;
} vnet_t;

static vnet_t g_vnet;
static virtio_device_desc_t g_vnet_desc;

/* ---------------------------------- TX ---------------------------------- */

static void vnet_tx(vnet_t *n) {
    for (;;) {
        int rc = virtio_pop(&n->vio, 1, &n->req);
        if (rc == 0) break;
        if (rc < 0) continue;          /* malformed: core finished it */
        virtio_req_t *r = &n->req;
        if (r->out_len < VNET_HDR_LEN || r->out_len > VNET_BUF) {
            n->tx_dropped++;           /* no header, or never gatherable */
        } else {
            uint32_t got = virtio_req_read(r, 0, n->txbuf, r->out_len);
            const uint8_t *frame = n->txbuf + VNET_HDR_LEN;
            unsigned flen = got >= VNET_HDR_LEN ? got - VNET_HDR_LEN : 0;
            if (n->tap_fd >= 0 &&
                send(n->tap_fd, frame, flen, 0) == (ssize_t)flen)
                n->tx_sent++;
            else
                n->tx_dropped++;
        }
        virtio_complete(&n->vio, r, 0);
    }
}

static void vnet_notify(virtio_dev_t *v, int qi) {
    if (qi == 1) vnet_tx(v->ctx);
    /* qi == 0: RX buffers are for the ingress path; the doorbell
     * alone must not consume them. */
}

/* ---------------------------------- RX ---------------------------------- */

void virtio_net_rx_frame(machine_t *m, const uint8_t *frame, unsigned len) {
    vnet_t *n = &g_vnet;
    if (m != n->m || !n->attached) return;
    if (!virtio_queue_ready(&n->vio, 0)) { n->rx_dropped++; return; }
    for (;;) {
        int rc = virtio_pop(&n->vio, 0, &n->req);
        if (rc == 0) { n->rx_dropped++; return; }        /* no buffer: drop */
        if (rc < 0) continue;                            /* ate a bad one */
        virtio_req_t *r = &n->req;
        if (r->in_len <= VNET_HDR_LEN) {                 /* unusable buffer */
            virtio_complete(&n->vio, r, 0);
            n->rx_dropped++;
            return;
        }
        uint32_t room = r->in_len - VNET_HDR_LEN;
        uint32_t payload = len < room ? len : room;
        if (payload < len) n->rx_truncated++;
        uint8_t hdr[VNET_HDR_LEN] = { 0 };
        hdr[10] = 1;                                     /* num_buffers */
        virtio_req_write(r, 0, hdr, VNET_HDR_LEN);
        virtio_req_write(r, VNET_HDR_LEN, frame, payload);
        virtio_complete(&n->vio, r, VNET_HDR_LEN + payload);
        n->rx_done++;
        return;
    }
}

void virtio_net_tick(void) {
    /* pull TAP datagrams into the RX queue at the device cadence
     * (e1000_tick's exact discipline) */
    vnet_t *n = &g_vnet;
    if (n->tap_fd < 0 || !n->attached) return;
    uint8_t buf[VNET_BUF];
    for (int i = 0; i < 8; i++) {
        ssize_t got = recv(n->tap_fd, buf, sizeof buf, 0);
        if (got <= 0) break;
        virtio_net_rx_frame(n->m, buf, (unsigned)got);
    }
}

void virtio_net_stats(uint64_t *tx_sent, uint64_t *tx_dropped,
                      uint64_t *rx_done, uint64_t *rx_dropped) {
    if (tx_sent)    *tx_sent    = g_vnet.tx_sent;
    if (tx_dropped) *tx_dropped = g_vnet.tx_dropped;
    if (rx_done)    *rx_done    = g_vnet.rx_done;
    if (rx_dropped) *rx_dropped = g_vnet.rx_dropped;
}

/* -------------------------------- attach -------------------------------- */

static void vnet_publish(machine_t *m, vnet_t *n) {
    n->m = m;
    memcpy(n->config, VNET_MAC, sizeof VNET_MAC);
    memset(&g_vnet_desc, 0, sizeof g_vnet_desc);
    g_vnet_desc.features = VNET_F_MAC;             /* D6: MAC and no more */
    g_vnet_desc.num_queues = 2;
    g_vnet_desc.queue_max[0] = VNET_QSIZE;
    g_vnet_desc.queue_max[1] = VNET_QSIZE;
    g_vnet_desc.config = n->config;
    g_vnet_desc.config_len = sizeof n->config;
    g_vnet_desc.notify = vnet_notify;
    virtio_pci_add(m, &n->vio, VIRTIO_NET_PCI_DEV, "virtio-net",
                   VIRTIO_PCI_NET_ID, 0x02, 0x00, 0x00,
                   VIRTIO_NET_BAR_BASE, VIRTIO_NET_IRQ, &g_vnet_desc, n);
    n->attached = 1;
}

void virtio_net_test_add(machine_t *m) {
    memset(&g_vnet, 0, sizeof g_vnet);
    g_vnet.tap_fd = -1;                            /* frames counted, dropped */
    vnet_publish(m, &g_vnet);
}

/* NET N0 ground rule shared with the e1000: the fixed endpoint
 * 127.0.0.1:8080 (UDP).  main refuses --tap + --virtio-net in one
 * run -- one endpoint, one owner. */
int virtio_net_attach(machine_t *m, const char *hostport) {
    char host[64] = "127.0.0.1";
    unsigned port = 8080;
    if (hostport && strchr(hostport, ':')) {
        if (sscanf(hostport, "%63[^:]:%u", host, &port) != 2) return -1;
    } else if (hostport && hostport[0]) {
        snprintf(host, sizeof host, "%s", hostport);
    }
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) return -1;
    struct sockaddr_in sa;
    memset(&sa, 0, sizeof sa);
    sa.sin_family = AF_INET;
    sa.sin_port = htons((uint16_t)port);
    sa.sin_addr.s_addr = inet_addr(host);
    if (connect(fd, (struct sockaddr *)&sa, sizeof sa) != 0) {
        close(fd);
        return -1;
    }
    fcntl(fd, F_SETFL, O_NONBLOCK);
    memset(&g_vnet, 0, sizeof g_vnet);
    g_vnet.tap_fd = fd;
    vnet_publish(m, &g_vnet);
    mlog(&m->log, "[virtio-net] TAP endpoint %s:%u (UDP), MAC 52:54:00:12:34:56\n",
         host, port);
    return 0;
}

int virtio_net_present(void) { return g_vnet.attached; }
