/* tests/nic_net.c -- NET N2 wire payload: a guest stand-in that runs the
 * frozen guest's own online self-test (DHCP -> ARP -> ICMP echo) against the
 * host responder, through the real e1000 rings and the real TAP socket.
 *
 * Why this exists.  N2's DoD receipt (`[dhcp] PASS: IP 10.0.2.15, gateway
 * 10.0.2.2` + `[net] PASS: ping 10.0.2.2 successful`) belongs to the frozen
 * AuraLite-OS kernel, and that kernel cannot be built in every tree (it needs
 * clang --target=x86_64-elf, ld.lld, rustc and mtools).  Leaving the phase
 * unmeasured is not the house style, so this payload reproduces the guest's
 * wire behaviour line by line and the harness SKIPs the real guest on top of
 * it.  It is a *stand-in*, and it is written to be one honestly:
 *
 *   - the packets are built by the same code shape as AuraLite-OS@0ed0d29
 *     kernel/net/net.c (net_eth_send's 60-byte pad, the DISCOVER/REQUEST
 *     builders, net_ping's ident 0x1234 / seq 1 / 0x41..0x60 payload), and
 *     the replies are accepted by the same filters, in the same order;
 *   - the rings are programmed the way drivers/e1000/e1000.c e1000_init does
 *     it (TX: TDLEN 32*16, TCTL EN|CT|COLD; RX: RDLEN 64*16, RDT = N-1,
 *     MTA[0..127] = ~0, RCTL 0x0400801a) and drained the way
 *     e1000_hw_rx_drain does (RDH delta, length from the descriptor, status
 *     cleared, RDT re-armed to last_rdh + N - 1);
 *   - the receipt lines are the guest's strings, so a tape from this payload
 *     and a tape from the real kernel are directly comparable.
 *
 * It is NOT a second implementation of the stack: no sockets, no timers, no
 * allocator.  Timeouts are bounded spin counts, which keeps the run
 * deterministic (see the N2 determinism pair in docs/plans/NET_PLAN.md).
 *
 * Build: see the Makefile's `tests/nic_net.elf` rule (freestanding, the K1
 * fixture's flags).  Entry is _start in long mode with paging on; the identity
 * map kload_boot installs covers 0..4 GiB, so physical addresses are used
 * directly.
 */
#include <stdint.h>

/* ---- e1000 register file (BAR0 0xFEBC0000, seeded by e1000_add) ---------- */
#define BAR        0xFEBC0000u
#define REG(r)     (*(volatile uint32_t *)(uintptr_t)(BAR + (r)))
#define E_CTRL     0x0000
#define E_STATUS   0x0008
#define E_ICR      0x00C0
#define E_IMS      0x00D0
#define E_RCTL     0x0100
#define E_TCTL     0x0400
#define E_RDBAL    0x2800
#define E_RDBAH    0x2804
#define E_RDLEN    0x2808
#define E_RDH      0x2810
#define E_RDT      0x2818
#define E_TDBAL    0x3800
#define E_TDBAH    0x3804
#define E_TDLEN    0x3808
#define E_TDH      0x3810
#define E_TDT      0x3818
#define E_MTA      0x5200
#define E_RAL      0x5400
#define E_RAH      0x5404

#define CTRL_FD    (1u << 0)
#define CTRL_SLU   (1u << 6)
/* RX descriptor status byte (descriptor offset 12), bit 0 = DD.  Same value
 * and same byte offset as the emulator's src/e1000.c RXD_STAT_DD. */
#define RXD_STAT_DD  0x01
#define STATUS_LU  (1u << 1)
#define TCTL_EN    (1u << 1)
#define RCTL_EN    (1u << 1)

/* ---- guest constants (drivers/e1000/e1000.c) ----------------------------- */
#define NUM_TX_DESC   32
#define NUM_RX_DESC   64
#define PKT_BUF_SIZE  2048

/* ---- slirp addressing, as the guest hardcodes it (kernel/net/net.c:39-47) - */
#define OUR_IP   0x0A00020Fu      /* 10.0.2.15, host order like the guest's */
#define GW_IP    0x0A000202u      /* 10.0.2.2  */
#define DNS_IP   0x0A000203u      /* 10.0.2.3, the slirp DNS proxy (dns.c:32) */

/* ---- DNS, as kernel/net/dns.h + dns.c measure it -------------------------- */
#define DNS_LOCAL_PORT 12345u     /* dns.h:19 -- the guest's source port */
#define DNS_SERVER_PORT 53u
#define DNS_QTYPE_A     1u
#define DNS_RTYPE_A     1u
#define DNS_RTYPE_CNAME 5u

/* ---- TCP, as kernel/net/tcp.c measures it -------------------------------- */
#define TCP_FIN 0x01
#define TCP_SYN 0x02
#define TCP_RST 0x04
#define TCP_PSH 0x08
#define TCP_ACK 0x10
#define TCP_WINDOW      64240u    /* tcp.c:44, what the guest advertises */
#define TCP_MSS         1460u     /* NETL3_V4_MSS, netl3.h:35 */
#define TCP_MAX_RETRIES 3         /* tcp.c:48 */
#define TCP_MAX_CONNS   16        /* tcp.h:53, tcp_x5_self_test's target */
/* tcp.c:1496 `tcp_x5_self_test` opens TCP_MAX_CONNS connections to
 * 10.0.2.3:53 one after another, holds them all open, then closes them all --
 * so this payload does the same, in the same order. */
#define TCP_X5_PORT     53u

/* ---- fixed physical layout.  kload_boot puts this image at 0x100000 and the
 * ---- boot_info at 0x20000, so the rings live above both; RAM_SIZE is 128
 * ---- MiB, and everything here is a few hundred KiB. */
#define TX_RING_PHYS   0x00300000u
#define TX_BUF_PHYS    0x00310000u
#define RX_RING_PHYS   0x00340000u
#define RX_BUF_PHYS    0x00350000u

struct tx_desc {                    /* e1000.c:93, 16 bytes */
    uint64_t addr;
    uint16_t length;
    uint8_t  cso;
    uint8_t  cmd;
    uint8_t  status;
    uint8_t  css;
    uint16_t special;
} __attribute__((packed));

struct rx_desc {
    uint64_t addr;
    uint16_t length;
    uint16_t csum;
    uint8_t  status;
    uint8_t  errors;
    uint16_t special;
} __attribute__((packed));

static volatile struct tx_desc *const tx_ring =
    (volatile struct tx_desc *)(uintptr_t)TX_RING_PHYS;
static volatile struct rx_desc *const rx_ring =
    (volatile struct rx_desc *)(uintptr_t)RX_RING_PHYS;
static uint8_t *const tx_buf = (uint8_t *)(uintptr_t)TX_BUF_PHYS;
static uint8_t *const rx_buf = (uint8_t *)(uintptr_t)RX_BUF_PHYS;

static uint8_t our_mac[6];
static uint8_t gw_mac[6];
static uint32_t our_ip, gw_ip;
static uint32_t tx_tail, rx_last;

static uint8_t  dns_mac[6];                 /* ARP cache for 10.0.2.3 */
static uint32_t dns_ip;
static int      dns_mac_known;
static uint32_t dns_ip_resolved;            /* the A record we were handed */
static uint16_t dns_query_id = 0xA5F1u;     /* fixed: a deterministic tape */

/* ---- COM1 ---------------------------------------------------------------- */
static void outb(uint16_t port, uint8_t v) {
    __asm__ volatile("outb %0, %1" : : "a"(v), "dN"(port));
}
static uint8_t inb(uint16_t port) {
    uint8_t v; __asm__ volatile("inb %1, %0" : "=a"(v) : "dN"(port)); return v;
}
static void putc_(char c) {
    while (!(inb(0x3FD) & 0x20)) { }
    outb(0x3F8, (uint8_t)c);
}
static void puts_(const char *s) { for (; *s; s++) putc_(*s); }
static void hex(uint32_t v, int digits) {
    static const char d[] = "0123456789abcdef";
    for (int i = digits - 1; i >= 0; i--) putc_(d[(v >> (i * 4)) & 0xF]);
}
static void dec(uint32_t v) {
    char b[10]; int n = 0;
    if (!v) { putc_('0'); return; }
    while (v) { b[n++] = (char)('0' + v % 10); v /= 10; }
    while (n) putc_(b[--n]);
}
static void ipdots(uint32_t ip) {          /* the guest prints host order */
    dec((ip >> 24) & 0xFF); putc_('.');
    dec((ip >> 16) & 0xFF); putc_('.');
    dec((ip >> 8) & 0xFF);  putc_('.');
    dec(ip & 0xFF);
}
static void macdots(const uint8_t *m) {
    for (int i = 0; i < 6; i++) { if (i) putc_(':'); hex(m[i], 2); }
}

/* ---- byte order + checksum, as net.c:156-198 -----------------------------
 * The guest's own htons_/htonl_ are not reproduced here: this payload builds
 * every header byte-wise (put_be32) and reads them back with be32, which is
 * the same wire order with one less place to get a swap wrong.  `checksum()`
 * IS reproduced bit for bit, because a wrong header checksum is exactly the
 * class of bug a lenient peer would hide. */
static uint16_t checksum(const void *data, uint32_t len) {
    const uint8_t *p = data;
    uint32_t sum = 0;
    for (uint32_t i = 0; i + 1 < len; i += 2)
        sum += (uint32_t)((p[i] << 8) | p[i + 1]);
    /* RFC 1071: an odd trailing byte is the HIGH byte of a word whose low byte
     * is zero -- `p[0] << 8`, exactly as net.c:181 does it.  Adding the byte
     * unshifted was a latent bug in this payload: every length it had checksummed
     * until N3 was even (IP header 20, ICMP echo 40), and the first odd-length
     * one -- a 49-byte TCP segment carrying a 29-byte DNS query -- produced a
     * sum that was wrong by 0xFF00, which the responder then correctly refused.
     * `tcp_csum` below has its own copy of the same tail rule for the same
     * reason. */
    if (len & 1) sum += (uint32_t)p[len - 1] << 8;
    while (sum >> 16) sum = (sum & 0xFFFF) + (sum >> 16);
    return (uint16_t)(~sum & 0xFFFF);
}
static uint32_t be32(const uint8_t *p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}
static void put_be32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);  p[3] = (uint8_t)v;
}

/* ---- the rings ----------------------------------------------------------- */
static void nic_init(void) {
    for (uint32_t i = 0; i < NUM_TX_DESC * sizeof(struct tx_desc); i++)
        ((uint8_t *)(uintptr_t)TX_RING_PHYS)[i] = 0;
    for (uint32_t i = 0; i < NUM_RX_DESC * sizeof(struct rx_desc); i++)
        ((uint8_t *)(uintptr_t)RX_RING_PHYS)[i] = 0;

    our_mac[0] = 0x52; our_mac[1] = 0x54; our_mac[2] = 0x00;
    our_mac[3] = 0x12; our_mac[4] = 0x34; our_mac[5] = 0x56;

    REG(E_CTRL) = CTRL_FD | CTRL_SLU;
    REG(E_IMS)  = 0xD5;                       /* the guest's mask */

    /* TX, exactly as e1000_init's step 6 */
    REG(E_TDBAL) = TX_RING_PHYS;
    REG(E_TDBAH) = 0;
    REG(E_TDLEN) = NUM_TX_DESC * 16;
    REG(E_TDH)   = 0;
    REG(E_TDT)   = 0;
    REG(E_TCTL)  = TCTL_EN | (0x10u << 4) | (0x40u << 12);

    /* RX, exactly as e1000_init's step 7: one buffer per descriptor, tail
     * armed to N-1 so the ring is never "full" (RDH == RDT drops). */
    for (uint32_t i = 0; i < NUM_RX_DESC; i++)
        rx_ring[i].addr = RX_BUF_PHYS + (uint64_t)i * PKT_BUF_SIZE;
    REG(E_RDBAL) = RX_RING_PHYS;
    REG(E_RDBAH) = 0;
    REG(E_RDLEN) = NUM_RX_DESC * 16;
    REG(E_RDH)   = 0;
    REG(E_RDT)   = NUM_RX_DESC - 1;
    REG(E_RAL)   = (uint32_t)our_mac[0] | ((uint32_t)our_mac[1] << 8) |
                   ((uint32_t)our_mac[2] << 16) | ((uint32_t)our_mac[3] << 24);
    REG(E_RAH)   = (uint32_t)our_mac[4] | ((uint32_t)our_mac[5] << 8) |
                   (1u << 31);
    for (int i = 0; i < 128; i++) REG(E_MTA + i * 4) = 0xFFFFFFFFu;
    REG(E_RCTL)  = RCTL_EN | (1u << 26) | (1u << 15) | (1u << 4) | (1u << 3);

    tx_tail = 0;
    rx_last = 0;
}

/* e1000_send(): one descriptor, cmd 0x0B, TDT doorbell, then spin on DD. */
static int nic_send(const uint8_t *frame, uint32_t len) {
    if (!(REG(E_STATUS) & STATUS_LU)) { puts_("[e1000] link down\n"); return -1; }
    if (len > PKT_BUF_SIZE) len = PKT_BUF_SIZE;
    uint32_t slot = tx_tail;
    for (uint32_t i = 0; i < len; i++) tx_buf[i] = frame[i];
    tx_ring[slot].addr   = TX_BUF_PHYS;
    tx_ring[slot].length = (uint16_t)len;
    tx_ring[slot].cso    = 0;
    tx_ring[slot].cmd    = 0x0B;              /* EOP | IFCS | RS */
    tx_ring[slot].status = 0;
    tx_tail = (tx_tail + 1) % NUM_TX_DESC;
    REG(E_TDT) = tx_tail;                     /* the doorbell */
    /* The guest's own budget is 100000 pause-spins (drivers/e1000/e1000.c:565),
     * and the number matters: it is what makes the no-device lane report
     * instead of hanging.  Copied, not invented. */
    for (int32_t timeout = 100000; timeout-- > 0;) {
        if (tx_ring[slot].status & 0x01) return 0;
        __asm__ volatile("pause");
    }
    puts_("[e1000] TX timeout: desc["); dec(slot);
    puts_("].status=0x"); hex(tx_ring[slot].status, 2);
    puts_(" TDH="); dec(REG(E_TDH));
    puts_(" TDT="); dec(REG(E_TDT)); putc_('\n');
    return -1;
}

/* e1000_hw_rx_drain() (drivers/e1000/e1000.c:504): RDH delta, `length` from
 * the descriptor, `pkt_len > 0` as the gate, status cleared, RDT re-armed.
 * Returns one frame per call, 0 when the ring holds nothing usable.
 *
 * Two fidelity notes, both measured rather than assumed:
 *  - The guest gates on `length`, NOT on DD.  So removing the emulator's DD
 *    writeback (src/e1000.c:373) does not make the frozen guest fail; the
 *    length writeback at src/e1000.c:372 is the one that does.  That is why
 *    N2's emulator-side negative control removes the length, not the DD.
 *  - This stand-in additionally requires DD, because that is what the SDM and
 *    QEMU say a completed RX descriptor carries, and N2 must not pass on an
 *    emulator that publishes a stale length with no completion.  It is a
 *    strictly tighter gate than the guest's: everything the guest accepts,
 *    this accepts too.
 * A zero-length or DD-less slot is consumed exactly as the guest consumes it
 * (status cleared, head advanced, RDT re-armed) but reports nothing. */
static uint32_t nic_poll(uint8_t *out, uint32_t cap) {
    uint32_t rdh = REG(E_RDH);
    if (rdh == rx_last) return 0;
    uint32_t idx = rx_last % NUM_RX_DESC;
    uint32_t len = rx_ring[idx].length;
    uint8_t  st  = rx_ring[idx].status;
    if (len > PKT_BUF_SIZE) len = PKT_BUF_SIZE;
    if (len > cap) len = cap;
    if (len && (st & RXD_STAT_DD)) {
        const uint8_t *src = rx_buf + (uint64_t)idx * PKT_BUF_SIZE;
        for (uint32_t i = 0; i < len; i++) out[i] = src[i];
    } else {
        len = 0;
    }
    rx_ring[idx].status = 0;
    rx_last = (rx_last + 1) % NUM_RX_DESC;
    REG(E_RDT) = (rx_last + NUM_RX_DESC - 1) % NUM_RX_DESC;
    return len;
}

/* Bounded receive: the emulator drains the TAP socket from devices_tick every
 * 256 instructions, so the reply lands on a tick -- which only happens while
 * the guest executes.  A spin budget, not a clock, keeps that deterministic. */
#define RECV_SPINS 3000000u
static uint32_t nic_recv(uint8_t *out, uint32_t cap) {
    for (uint32_t t = 0; t < RECV_SPINS; t++) {
        uint32_t n = nic_poll(out, cap);
        if (n) return n;
        __asm__ volatile("pause");
    }
    return 0;
}

/* ---- Ethernet / IP / UDP framing, as net.c:293 --------------------------- */
static uint32_t eth_build(uint8_t *frame, const uint8_t dst[6],
                          uint16_t ethertype, const uint8_t *payload,
                          uint32_t plen) {
    for (int i = 0; i < 6; i++) { frame[i] = dst[i]; frame[6 + i] = our_mac[i]; }
    frame[12] = (uint8_t)(ethertype >> 8);
    frame[13] = (uint8_t)ethertype;
    for (uint32_t i = 0; i < plen; i++) frame[14 + i] = payload[i];
    uint32_t total = 14 + plen;
    while (total < 60) frame[total++] = 0;    /* the 60-byte minimum */
    return total;
}

static void ip_build(uint8_t *p, uint8_t proto, uint32_t src, uint32_t dst,
                     const uint8_t *body, uint32_t blen, uint16_t ident) {
    p[0] = 0x45; p[1] = 0;
    p[2] = (uint8_t)((20 + blen) >> 8); p[3] = (uint8_t)(20 + blen);
    p[4] = (uint8_t)(ident >> 8);       p[5] = (uint8_t)ident;
    p[6] = 0; p[7] = 0;
    p[8] = 64; p[9] = proto; p[10] = 0; p[11] = 0;
    put_be32(p + 12, src);
    put_be32(p + 16, dst);
    uint16_t cs = checksum(p, 20);
    p[10] = (uint8_t)(cs >> 8); p[11] = (uint8_t)cs;
    for (uint32_t i = 0; i < blen; i++) p[20 + i] = body[i];
}

/* ---- DHCP, as net.c:801 -------------------------------------------------- */
#define DHCP_FIXED 236
static uint32_t dhcp_build(uint8_t *buf, uint8_t msgtype, uint32_t xid,
                           int with_request_opts, uint32_t req_ip,
                           uint32_t srv_ip) {
    for (uint32_t i = 0; i < DHCP_FIXED; i++) buf[i] = 0;
    buf[0] = 1; buf[1] = 1; buf[2] = 6; buf[3] = 0;
    put_be32(buf + 4, xid);
    buf[10] = 0x80; buf[11] = 0;                 /* broadcast flag */
    for (int i = 0; i < 6; i++) buf[28 + i] = our_mac[i];
    put_be32(buf + 232, 0x63825363u);            /* magic cookie */

    uint32_t o = DHCP_FIXED;
    buf[o++] = 53; buf[o++] = 1; buf[o++] = msgtype;
    if (!with_request_opts) {                    /* DISCOVER: parameter list */
        buf[o++] = 55; buf[o++] = 3;
        buf[o++] = 1; buf[o++] = 3; buf[o++] = 6;
    } else {                                     /* REQUEST: 50 then 54 */
        buf[o++] = 50; buf[o++] = 4; put_be32(buf + o, req_ip); o += 4;
        buf[o++] = 54; buf[o++] = 4; put_be32(buf + o, srv_ip); o += 4;
    }
    buf[o++] = 255;
    return o;
}

static const uint8_t *dhcp_find_option(const uint8_t *opts, int len, uint8_t code,
                                       int *out_len) {
    int i = 0;                                   /* net.c:777 verbatim */
    while (i < len) {
        uint8_t c = opts[i];
        if (c == 255) break;
        if (c == 0) { i++; continue; }
        if (i + 1 >= len) break;
        uint8_t l = opts[i + 1];
        if (c == code) { if (out_len) *out_len = l; return opts + i + 2; }
        i += 2 + l;
    }
    return 0;
}

static uint32_t dhcp_xid = 0x12345678u;          /* net.c:808 */

static int dhcp_send(uint8_t msgtype, uint32_t req_ip, uint32_t srv_ip) {
    uint8_t pkt[576], frame[700];
    uint32_t dlen = dhcp_build(pkt, msgtype, dhcp_xid, msgtype == 3,
                               req_ip, srv_ip);
    uint32_t udp_len = 8 + dlen;
    uint8_t *udp = frame + 14 + 20;
    udp[0] = (uint8_t)(68 >> 8); udp[1] = (uint8_t)68;   /* src port 68 */
    udp[2] = (uint8_t)(67 >> 8); udp[3] = (uint8_t)67;   /* dst port 67 */
    udp[4] = (uint8_t)(udp_len >> 8); udp[5] = (uint8_t)udp_len;
    udp[6] = 0; udp[7] = 0;                      /* no UDP checksum, as the guest */
    for (uint32_t i = 0; i < dlen; i++) udp[8 + i] = pkt[i];
    uint8_t bcast[6] = {0xFF,0xFF,0xFF,0xFF,0xFF,0xFF};
    ip_build(frame + 14, 17, 0x00000000u, 0xFFFFFFFFu, udp, udp_len,
             msgtype == 1 ? 0x1234 : 0x1235);
    uint32_t total = 14 + 20 + udp_len;
    while (total < 60) frame[total++] = 0;
    for (int i = 0; i < 6; i++) { frame[i] = bcast[i]; frame[6 + i] = our_mac[i]; }
    frame[12] = 0x08; frame[13] = 0x00;
    return nic_send(frame, total);
}

/* Returns the message type accepted (2/5), or 0 on timeout. */
static int dhcp_recv(uint32_t *yiaddr, uint32_t *server_id, uint32_t *mask,
                     uint32_t *dns, uint32_t *router) {
    uint8_t rbuf[2048];
    uint32_t n = nic_recv(rbuf, sizeof rbuf);
    if (!n) return 0;
    if (n < 14 + 20 + 8 + DHCP_FIXED) return 0;
    if (rbuf[12] != 0x08 || rbuf[13] != 0x00) return 0;
    if (rbuf[14 + 9] != 17) return 0;
    if (rbuf[34] != 0 || rbuf[35] != 67) return 0;      /* udp.src == 67 */
    if (rbuf[36] != 0 || rbuf[37] != 68) return 0;      /* udp.dst == 68 */
    const uint8_t *dhcp = rbuf + 42;
    int opts_len = (int)n - (14 + 20 + 8 + DHCP_FIXED);
    if (opts_len < 4) return 0;
    if (dhcp[0] != 2) return 0;                          /* BOOTREPLY */
    /* net.c:1058 -- the ACK path compares ntohl_(ack->xid) with the xid it
     * sent; the OFFER path does not, but a responder that cannot echo an xid
     * is not worth talking to, so this stand-in checks it in both. */
    if (be32(dhcp + 4) != dhcp_xid) return 0;
    const uint8_t *opts = dhcp + DHCP_FIXED;
    int l;
    const uint8_t *mt = dhcp_find_option(opts, opts_len, 53, &l);
    if (!mt || l != 1) return 0;
    *yiaddr = be32(dhcp + 16);                   /* net.c:927 ntohl_(yiaddr) */
    const uint8_t *sid = dhcp_find_option(opts, opts_len, 54, &l);
    if (sid && l == 4) *server_id = be32(sid);
    const uint8_t *sm = dhcp_find_option(opts, opts_len, 1, &l);
    if (sm && l == 4) *mask = be32(sm);
    const uint8_t *dn = dhcp_find_option(opts, opts_len, 6, &l);
    if (dn && l >= 4) *dns = be32(dn);
    const uint8_t *rt = dhcp_find_option(opts, opts_len, 3, &l);
    if (rt && l >= 4) *router = be32(rt);
    return mt[0];
}

static int run_dhcp(void) {
    puts_("[dhcp] starting DHCP discovery...\n");
    uint32_t yiaddr = 0, sid = 0, mask = 0, dns = 0, router = 0;

    if (dhcp_send(1, 0, 0) < 0) {
        puts_("[dhcp] FAIL: DISCOVER transmit failed (link down or TX timeout)\n");
        return -1;
    }
    puts_("[dhcp] DISCOVER sent (xid=0x"); hex(dhcp_xid, 8); puts_(")\n");

    int mt = dhcp_recv(&yiaddr, &sid, &mask, &dns, &router);
    if (mt != 2) { puts_("[dhcp] FAIL: no OFFER received\n"); return -1; }
    puts_("[dhcp] OFFER: IP "); ipdots(yiaddr);
    puts_(", server "); ipdots(sid); putc_('\n');

    if (dhcp_send(3, yiaddr, sid) < 0) {
        puts_("[dhcp] FAIL: REQUEST transmit failed (link down or TX timeout)\n");
        return -1;
    }
    puts_("[dhcp] REQUEST sent (requesting "); ipdots(yiaddr); puts_(")\n");

    uint32_t ayi = 0, asid = 0, amask = mask, adns = dns, arouter = 0;
    mt = dhcp_recv(&ayi, &asid, &amask, &adns, &arouter);
    if (mt != 5) { puts_("[dhcp] FAIL: no ACK received\n"); return -1; }
    if (ayi) yiaddr = ayi;
    if (arouter) router = arouter;
    our_ip = yiaddr;
    if (router) gw_ip = router;
    if (mask) {
        puts_("[dhcp] subnet mask: "); ipdots(mask); putc_('\n');
    }
    if (dns) {
        puts_("[dhcp] DNS server: "); ipdots(dns); puts_(" (1 total)\n");
    }
    puts_("[dhcp] PASS: IP "); ipdots(our_ip);
    puts_(", gateway "); ipdots(gw_ip); putc_('\n');
    return 0;
}

/* ---- ARP, as net.c:356 --------------------------------------------------- */
static int run_arp(void) {
    uint8_t bcast[6] = {0xFF,0xFF,0xFF,0xFF,0xFF,0xFF};
    uint8_t payload[28], frame[60];
    payload[0] = 0; payload[1] = 1;               /* hw type 1 */
    payload[2] = 0x08; payload[3] = 0x00;         /* proto 0x0800 */
    payload[4] = 6; payload[5] = 4;
    payload[6] = 0; payload[7] = 1;               /* opcode: request */
    for (int i = 0; i < 6; i++) payload[8 + i] = our_mac[i];
    put_be32(payload + 14, our_ip);
    for (int i = 0; i < 6; i++) payload[14 + 6 + i] = 0;
    put_be32(payload + 24, gw_ip);
    uint32_t len = eth_build(frame, bcast, 0x0806, payload, 28);
    if (nic_send(frame, len) < 0) {
        puts_("[net] ARP request TX failed\n"); return -1;
    }
    puts_("[net] ARP request sent for "); ipdots(gw_ip); putc_('\n');

    uint8_t rbuf[2048];
    uint32_t n = nic_recv(rbuf, sizeof rbuf);
    if (!n) { puts_("[net] ARP timeout\n"); return -1; }
    if (n < 14 + 28) { puts_("[net] ARP reply too short\n"); return -1; }
    if (rbuf[12] != 0x08 || rbuf[13] != 0x06) {
        puts_("[net] ARP: unexpected ethertype\n"); return -1;
    }
    if (rbuf[20] != 0 || rbuf[21] != 2) {
        puts_("[net] ARP: opcode is not a reply\n"); return -1;
    }
    for (int i = 0; i < 6; i++) gw_mac[i] = rbuf[22 + i];
    puts_("[net] ARP reply: gateway is "); macdots(gw_mac); putc_('\n');
    return 0;
}

/* ---- ICMP echo, as net.c:476 -------------------------------------------- */
static int run_ping(void) {
    uint8_t pkt[14 + 20 + 8 + 32];
    uint8_t *icmp = pkt + 34;
    for (int i = 0; i < 32; i++) icmp[8 + i] = (uint8_t)(i + 0x41);
    icmp[0] = 8; icmp[1] = 0; icmp[2] = 0; icmp[3] = 0;
    icmp[4] = 0x12; icmp[5] = 0x34;               /* ident 0x1234 */
    icmp[6] = 0; icmp[7] = 1;                     /* seq 1 */
    uint16_t cs = checksum(icmp, 40);
    icmp[2] = (uint8_t)(cs >> 8); icmp[3] = (uint8_t)cs;
    ip_build(pkt + 14, 1, our_ip, gw_ip, icmp, 40, 1);
    for (int i = 0; i < 6; i++) { pkt[i] = gw_mac[i]; pkt[6 + i] = our_mac[i]; }
    pkt[12] = 0x08; pkt[13] = 0x00;
    if (nic_send(pkt, sizeof pkt) < 0) {
        puts_("[net] ICMP echo request TX failed\n"); return -1;
    }
    puts_("[net] ICMP echo request sent to "); ipdots(gw_ip); putc_('\n');

    uint8_t rbuf[2048];
    uint32_t n = nic_recv(rbuf, sizeof rbuf);
    if (!n) { puts_("[net] ICMP echo reply timeout\n"); return -1; }
    if (n < 14 + 20 + 8) { puts_("[net] ICMP reply too short\n"); return -1; }
    if (rbuf[12] != 0x08 || rbuf[13] != 0x00) {
        puts_("[net] ICMP: unexpected ethertype\n"); return -1;
    }
    if (rbuf[14 + 9] != 1) { puts_("[net] ICMP: protocol is not 1\n"); return -1; }
    if (rbuf[34] != 0) { puts_("[net] ICMP: type is not echo reply\n"); return -1; }
    uint16_t seq = (uint16_t)((rbuf[40] << 8) | rbuf[41]);
    puts_("[net] ICMP echo reply received from "); ipdots(gw_ip);
    puts_(" (seq "); dec(seq); puts_(")\n");
    /* The guest stops at `type == 0`; this payload goes the extra step the
     * plan's "payload verified" wording asks for, and reports it separately. */
    int same = 1;
    for (int i = 0; i < 32; i++)
        if (rbuf[42 + i] != (uint8_t)(i + 0x41)) same = 0;
    uint16_t rid = (uint16_t)((rbuf[38] << 8) | rbuf[39]);
    puts_("[net] ICMP payload ");
    puts_(same && rid == 0x1234 && seq == 1 ? "verified (ident+seq+32 bytes)\n"
                                            : "MISMATCH\n");
    return same ? 0 : -1;
}

/* ---- ARP for an arbitrary target, as net.c:356 --------------------------
 * `run_arp` above resolves the gateway and keeps the receipt the N2 harness
 * asserts; this is the same exchange for any on-link address, with the cache
 * the guest's `net_arp_resolve` has (net.c:405+: a known target returns
 * immediately, so 16 TCP connections to 10.0.2.3 ARP once, not sixteen
 * times).  An off-subnet target routes via the gateway and is never ARPed
 * for -- which is why the responder only answers inside 10.0.2.0/24. */
static int arp_resolve(uint32_t target, uint8_t out_mac[6]) {
    if (target == dns_ip && dns_mac_known) {
        for (int i = 0; i < 6; i++) out_mac[i] = dns_mac[i];
        return 0;
    }
    uint8_t bcast[6] = {0xFF,0xFF,0xFF,0xFF,0xFF,0xFF};
    uint8_t payload[28], frame[60], rbuf[2048];
    payload[0] = 0; payload[1] = 1;
    payload[2] = 0x08; payload[3] = 0x00;
    payload[4] = 6; payload[5] = 4;
    payload[6] = 0; payload[7] = 1;               /* opcode: request */
    for (int i = 0; i < 6; i++) payload[8 + i] = our_mac[i];
    put_be32(payload + 14, our_ip);
    for (int i = 0; i < 6; i++) payload[20 + i] = 0;
    put_be32(payload + 24, target);
    uint32_t len = eth_build(frame, bcast, 0x0806, payload, 28);
    if (nic_send(frame, len) < 0) {
        puts_("[net] ARP TX failed for "); ipdots(target); putc_('\n');
        return -1;
    }
    puts_("[net] ARP request sent for "); ipdots(target); putc_('\n');
    uint32_t n = nic_recv(rbuf, sizeof rbuf);
    if (!n) { puts_("[net] ARP timeout for "); ipdots(target); putc_('\n'); return -1; }
    if (n < 14 + 28 || rbuf[12] != 0x08 || rbuf[13] != 0x06 ||
        rbuf[20] != 0 || rbuf[21] != 2) {
        puts_("[net] ARP: not a reply for "); ipdots(target); putc_('\n');
        return -1;
    }
    /* net.c:441-465 checks length, ethertype and `opcode == 2` and then takes
     * sender_mac verbatim -- it never compares the target address, so neither
     * does this. */
    for (int i = 0; i < 6; i++) out_mac[i] = rbuf[22 + i];
    if (target == dns_ip) {
        for (int i = 0; i < 6; i++) dns_mac[i] = out_mac[i];
        dns_mac_known = 1;
    }
    puts_("[net] ARP reply: "); ipdots(target); puts_(" is at ");
    macdots(out_mac); putc_('\n');
    return 0;
}

/* ---- DNS, as kernel/net/dns.c ------------------------------------------- */

/* dns_parse.c:37 `dns_encode_name`: lowercased, length-prefixed labels.  This
 * payload only ever asks for "example.com" -- the name `dns_self_test`
 * resolves at boot (kernel/kernel.c:429) -- so the encoder is literal rather
 * than general, and says so. */
static int dns_encode_example_com(uint8_t *out) {
    static const uint8_t enc[] = {7,'e','x','a','m','p','l','e',3,'c','o','m',0};
    for (unsigned i = 0; i < sizeof enc; i++) out[i] = enc[i];
    return (int)sizeof enc;                       /* 13 */
}

/* dns.c:120 `dns_build_query_type`: id, RD, QDCOUNT=1, QNAME, QTYPE, IN. */
static uint32_t dns_build_query(uint8_t *buf, uint16_t id, uint16_t qtype) {
    buf[0] = (uint8_t)(id >> 8); buf[1] = (uint8_t)id;
    buf[2] = 0x01; buf[3] = 0x00;                 /* RD */
    buf[4] = 0;    buf[5] = 1;                    /* QDCOUNT */
    for (int i = 6; i < 12; i++) buf[i] = 0;      /* AN/NS/ARCOUNT */
    int nlen = dns_encode_example_com(buf + 12);
    int q = 12 + nlen;
    buf[q]     = (uint8_t)(qtype >> 8);
    buf[q + 1] = (uint8_t)qtype;
    buf[q + 2] = 0; buf[q + 3] = 1;               /* class IN */
    return (uint32_t)(q + 4);
}

/* net.c `net_udp_sendto`: ARP first, then eth + ip(ident 2) + udp, csum 0. */
static int udp_sendto(uint32_t dst_ip, const uint8_t dst_mac[6],
                      uint16_t dport, uint16_t sport,
                      const uint8_t *data, uint32_t dlen,
                      uint8_t *frame, uint32_t *flen) {
    uint8_t udp[8 + 1500];
    udp[0] = (uint8_t)(sport >> 8); udp[1] = (uint8_t)sport;
    udp[2] = (uint8_t)(dport >> 8); udp[3] = (uint8_t)dport;
    udp[4] = (uint8_t)((8 + dlen) >> 8); udp[5] = (uint8_t)(8 + dlen);
    udp[6] = 0; udp[7] = 0;                       /* no UDP checksum */
    for (uint32_t i = 0; i < dlen; i++) udp[8 + i] = data[i];
    uint8_t ip[20 + 8 + 1500];
    ip_build(ip, 17, our_ip, dst_ip, udp, 8 + dlen, 2);
    *flen = eth_build(frame, dst_mac, 0x0800, ip, 20 + 8 + dlen);
    return nic_send(frame, *flen);
}

/* The gates the guest actually applies, in its order: net.c
 * `net_udp_recvfrom` (length, ethertype, ip.protocol, udp.dst == our local
 * port, udp.length sane), then dns.c:172 (`got_ip`/`got_port`) and dns.c:177
 * (the id must be ours), then dns_parse.c:132 `dns_parse_response` (QR set, TC
 * clear, skip QDCOUNT questions, then find an IN/A RR whose owner matches and
 * whose RDLENGTH is 4). */
static int dns_check_reply(const uint8_t *rbuf, uint32_t n, uint16_t want_id,
                           uint32_t *a_ip) {
    if (n < 14 + 20 + 8) return -1;
    if (rbuf[12] != 0x08 || rbuf[13] != 0x00) return -2;
    if (rbuf[14 + 9] != 17) return -3;
    uint16_t sport = (uint16_t)((rbuf[34] << 8) | rbuf[35]);
    uint16_t dport = (uint16_t)((rbuf[36] << 8) | rbuf[37]);
    uint16_t ulen  = (uint16_t)((rbuf[38] << 8) | rbuf[39]);
    if (dport != DNS_LOCAL_PORT) return -4;
    if (sport != DNS_SERVER_PORT) return -5;
    if (ulen < 8 || (uint32_t)(14 + 20) + ulen > n) return -6;
    if (be32(rbuf + 26) != dns_ip) return -7;     /* dns.c:172 got_ip */
    const uint8_t *msg = rbuf + 42;
    int mlen = (int)ulen - 8;
    if (mlen < 12) return -8;
    if ((uint16_t)((msg[0] << 8) | msg[1]) != want_id) return -9;
    uint16_t flags = (uint16_t)((msg[2] << 8) | msg[3]);
    if (!(flags & 0x8000)) return -10;            /* QR */
    if (flags & 0x0200) return -11;               /* TC: would force TCP */
    uint16_t qd = (uint16_t)((msg[4] << 8) | msg[5]);
    uint16_t an = (uint16_t)((msg[6] << 8) | msg[7]);
    int pos = 12;
    for (int i = 0; i < qd; i++) {
        while (pos < mlen) {
            uint8_t b = msg[pos];
            if ((b & 0xC0) == 0xC0) { pos += 2; break; }   /* compressed */
            if (b == 0) { pos++; break; }
            pos += 1 + b;
        }
        pos += 4;                                 /* QTYPE + QCLASS */
        if (pos > mlen) return -12;
    }
    for (int i = 0; i < an; i++) {
        /* owner name: labels or a compression pointer */
        while (pos < mlen) {
            uint8_t b = msg[pos];
            if ((b & 0xC0) == 0xC0) { pos += 2; break; }
            if (b == 0) { pos++; break; }
            pos += 1 + b;
        }
        if (pos + 10 > mlen) return -13;
        uint16_t type  = (uint16_t)((msg[pos] << 8) | msg[pos + 1]);
        uint16_t klass = (uint16_t)((msg[pos + 2] << 8) | msg[pos + 3]);
        uint16_t rdlen = (uint16_t)((msg[pos + 8] << 8) | msg[pos + 9]);
        int rdata = pos + 10;
        if (rdata + rdlen > mlen) return -14;
        if (klass == 1 && type == DNS_RTYPE_A && rdlen == 4) {
            *a_ip = be32(msg + rdata);
            return 0;
        }
        pos = rdata + rdlen;
    }
    return -15;                                   /* NODATA/NXDOMAIN */
}

static int run_dns(void) {
    if (arp_resolve(dns_ip, dns_mac) != 0) {
        puts_("[dns] FAIL: could not resolve the server's MAC\n");
        return -1;
    }
    uint8_t msg[512], frame[1518], rbuf[2048];
    uint32_t mlen = dns_build_query(msg, dns_query_id, DNS_QTYPE_A);
    uint32_t flen;
    puts_("[dns] cache MISS 'example.com' — querying\n");
    if (udp_sendto(dns_ip, dns_mac, DNS_SERVER_PORT, DNS_LOCAL_PORT,
                   msg, mlen, frame, &flen) < 0) {
        puts_("[dns] FAIL: query TX failed\n");
        return -1;
    }
    uint32_t n = nic_recv(rbuf, sizeof rbuf);
    if (!n) { puts_("[dns] FAIL: no response (timeout)\n"); return -1; }
    uint32_t a_ip = 0;
    int rc = dns_check_reply(rbuf, n, dns_query_id, &a_ip);
    if (rc != 0) {
        puts_("[dns] FAIL: response rejected by the guest's own parser (");
        dec((uint32_t)(-rc)); puts_(")\n");
        return -1;
    }
    dns_ip_resolved = a_ip;
    puts_("[dns] PASS: 'example.com' -> "); ipdots(a_ip); putc_('\n');
    return 0;
}

/* ---- TCP, as kernel/net/tcp.c ------------------------------------------- */

struct tcp_conn {
    uint16_t sport;
    uint32_t snd_nxt, rcv_nxt;
    int      established;
};
static struct tcp_conn tcp_conns[TCP_MAX_CONNS];

/* tcp.c:196 leaves the checksum 0 and lets the L3 output path fill in the
 * pseudo-header sum; this payload computes it here, over pseudo-header +
 * segment, with the field itself zero (RFC 793). */
static uint16_t tcp_csum(uint32_t src, uint32_t dst,
                         const uint8_t *seg, uint32_t seglen) {
    uint8_t ps[12];
    put_be32(ps, src);
    put_be32(ps + 4, dst);
    ps[8] = 0; ps[9] = 6;                       /* zero + protocol TCP */
    ps[10] = (uint8_t)(seglen >> 8); ps[11] = (uint8_t)seglen;
    uint32_t total = 0;
    for (uint32_t i = 0; i + 1 < 12; i += 2)
        total += (uint32_t)((ps[i] << 8) | ps[i + 1]);
    uint32_t j = 0;
    while (j + 1 < seglen) {
        uint16_t w = (uint16_t)((seg[j] << 8) | seg[j + 1]);
        if (j == 16) w = 0;                     /* the checksum field itself */
        total += w;
        j += 2;
    }
    if (j < seglen) total += (uint32_t)seg[j] << 8;   /* RFC 1071 odd tail */
    while (total >> 16) total = (total & 0xFFFFu) + (total >> 16);
    return (uint16_t)(~total & 0xFFFFu);
}

/* tcp.c:161 `tcp_send_segment_at`: window TCP_WINDOW, urgent 0, options ride
 * the SYN only ("data segments stay 20 bytes"). */
static uint32_t tcp_build_seg(uint8_t *seg, uint16_t sport, uint16_t dport,
                              uint32_t seq, uint32_t ack, uint8_t flags,
                              int with_opts, const uint8_t *data, uint32_t dlen) {
    uint32_t hdr = with_opts ? 28u : 20u;
    seg[0] = (uint8_t)(sport >> 8); seg[1] = (uint8_t)sport;
    seg[2] = (uint8_t)(dport >> 8); seg[3] = (uint8_t)dport;
    put_be32(seg + 4, seq);
    put_be32(seg + 8, ack);
    seg[12] = (uint8_t)((hdr >> 2) << 4);
    seg[13] = flags;
    seg[14] = (uint8_t)(TCP_WINDOW >> 8); seg[15] = (uint8_t)TCP_WINDOW;
    seg[16] = 0; seg[17] = 0;                   /* checksum, filled below */
    seg[18] = 0; seg[19] = 0;                   /* urgent pointer */
    if (with_opts) {
        /* MSS 1460 then SACK-permitted, the order and the padding the guest's
         * own `tcpm6c_build_syn_opts` emits (8 bytes: 02 04 05 b4 04 02 00 00).
         * The responder parses these and the reply carries the same pair, which
         * is what makes `tcpm6c_parse_opts` run on both sides. */
        seg[20] = 2; seg[21] = 4;
        seg[22] = (uint8_t)(TCP_MSS >> 8); seg[23] = (uint8_t)TCP_MSS;
        seg[24] = 4; seg[25] = 2;
        seg[26] = 0; seg[27] = 0;               /* EOL padding to a 4-byte edge */
    }
    for (uint32_t i = 0; i < dlen; i++) seg[hdr + i] = data[i];
    uint32_t seglen = hdr + dlen;
    uint16_t cs = tcp_csum(our_ip, dns_ip, seg, seglen);
    seg[16] = (uint8_t)(cs >> 8); seg[17] = (uint8_t)cs;
    return seglen;
}

static int tcp_send_seg(uint16_t sport, uint16_t dport, uint32_t seq,
                        uint32_t ack, uint8_t flags, int with_opts,
                        const uint8_t *data, uint32_t dlen,
                        uint8_t *frame, uint32_t *flen) {
    uint8_t seg[20 + 8 + 1500];
    uint32_t seglen = tcp_build_seg(seg, sport, dport, seq, ack, flags,
                                    with_opts, data, dlen);
    uint8_t ip[20 + 20 + 8 + 1500];
    ip_build(ip, 6, our_ip, dns_ip, seg, seglen, 2);
    *flen = eth_build(frame, dns_mac, 0x0800, ip, 20 + seglen);
    return nic_send(frame, *flen);
}

/* tcp.c:290 `tcp_recv_segment_timeout` filters on the 4-tuple only: protocol
 * TCP, src == the peer, src_port == the port we dialled, dst_port == our
 * ephemeral port, data_offset in 5..15 words, and the payload length taken
 * from the IP total rather than the frame (the NIC pads to 60).  It never
 * checks seq/ack. */
static int tcp_recv_seg(uint16_t sport, uint16_t dport, uint8_t *flags_out,
                        uint32_t *seq_out, uint32_t *ack_out,
                        uint8_t *data, uint32_t cap, uint32_t *dlen,
                        uint16_t *win_out) {
    uint8_t rbuf[2048];
    uint32_t n = nic_recv(rbuf, sizeof rbuf);
    if (!n) return -1;
    if (n < 14 + 20 + 20) return -2;
    if (rbuf[12] != 0x08 || rbuf[13] != 0x00) return -3;
    if (rbuf[14 + 9] != 6) return -4;
    if (be32(rbuf + 26) != dns_ip) return -5;
    uint16_t ip_total = (uint16_t)((rbuf[16] << 8) | rbuf[17]);
    uint16_t ip_ihl = (uint16_t)((rbuf[14] & 0x0F) * 4);
    if (ip_total < (uint16_t)(ip_ihl + 20) || (uint32_t)14 + ip_total > n)
        return -6;
    const uint8_t *t = rbuf + 34;
    uint32_t seglen = (uint32_t)ip_total - ip_ihl;
    uint16_t tsport = (uint16_t)((t[0] << 8) | t[1]);
    uint16_t tdport = (uint16_t)((t[2] << 8) | t[3]);
    if (tsport != dport || tdport != sport) return -7;
    uint32_t words = (uint32_t)(t[12] >> 4);
    if (words < 5 || words > 15) return -8;
    uint32_t thdr = words * 4;
    if (thdr > seglen) return -9;
    *flags_out = t[13];
    *seq_out = be32(t + 4);
    *ack_out = be32(t + 8);
    *win_out = (uint16_t)((t[14] << 8) | t[15]);
    uint32_t pl = seglen - thdr;
    if (pl > cap) pl = cap;
    for (uint32_t i = 0; i < pl; i++) data[i] = t[thdr + i];
    *dlen = pl;
    return 0;
}

/* tcp.c:675 `tcp_open_addr`: SYN, then up to TCP_MAX_RETRIES RTOs waiting for
 * SYN|ACK (a RST is ECONNREFUSED), then a bare ACK and ESTABLISHED.  The ISN
 * here is the guest's own scheme, `0x1000 + h * 0x100`, and the ephemeral port
 * comes from its monotonic 40000..64999 wheel (tcp.c:756) -- both so the tape
 * shows the same numbers a real guest would. */
static int tcp_open_one(uint32_t h, uint16_t sport, uint16_t dport,
                        struct tcp_conn *c) {
    uint8_t frame[1518], data[1500];
    uint32_t flen, dlen;
    uint8_t flags;
    uint32_t rseq, rack;
    uint16_t rwin;

    c->sport = sport;
    c->snd_nxt = 0x1000u + h * 0x100u;
    c->rcv_nxt = 0;
    c->established = 0;

    puts_("[tcp] SYN options: 8 bytes, hdr=28, mss=1460 sack-perm\n");
    if (tcp_send_seg(sport, dport, c->snd_nxt, 0, TCP_SYN, 1, 0, 0,
                     frame, &flen) < 0) {
        puts_("[tcp] SYN TX failed\n");
        return -1;
    }
    c->snd_nxt++;                                   /* SYN consumes one */

    for (int attempt = 0; attempt <= TCP_MAX_RETRIES; attempt++) {
        if (tcp_recv_seg(sport, dport, &flags, &rseq, &rack, data, sizeof data,
                         &dlen, &rwin) == 0) {
            if (flags & TCP_RST) {
                puts_("[tcp] connection refused (RST)\n");
                return -2;
            }
            if ((flags & (TCP_SYN | TCP_ACK)) == (TCP_SYN | TCP_ACK)) {
                c->rcv_nxt = rseq + 1;
                if (tcp_send_seg(sport, dport, c->snd_nxt, c->rcv_nxt, TCP_ACK,
                                 0, 0, 0, frame, &flen) < 0) {
                    puts_("[tcp] handshake ACK TX failed\n");
                    return -1;
                }
                c->established = 1;
                puts_("[tcp] ESTABLISHED (peer_wnd=");
                dec(rwin ? rwin : TCP_WINDOW); puts_(")\n");
                return 0;
            }
            puts_("[tcp] expected SYN-ACK, got flags=0x");
            hex(flags, 2); putc_('\n');
        }
        if (attempt < TCP_MAX_RETRIES) {
            puts_("[tcp] RTO waiting for SYN-ACK, retransmitting SYN\n");
            if (tcp_send_seg(sport, dport, c->snd_nxt - 1, 0, TCP_SYN, 1, 0, 0,
                             frame, &flen) < 0)
                return -1;
        }
    }
    puts_("[tcp] timeout waiting for SYN-ACK\n");
    return -3;
}

/* tcp.c:1343 `tcp_close` from ESTABLISHED: FIN|ACK, then accept a segment;
 * a single FIN|ACK back satisfies both of its branches (ACK -> FIN_WAIT_2,
 * FIN -> TIME_WAIT after ACKing us), so the close is one round trip. */
static int tcp_close_one(struct tcp_conn *c, uint16_t dport) {
    uint8_t frame[1518], data[1500];
    uint32_t flen, dlen;
    uint8_t flags;
    uint32_t rseq, rack;
    uint16_t rwin;
    if (!c->established) return 0;
    if (tcp_send_seg(c->sport, dport, c->snd_nxt, c->rcv_nxt,
                     TCP_FIN | TCP_ACK, 0, 0, 0, frame, &flen) < 0) {
        puts_("[tcp] FIN TX failed\n");
        return -1;
    }
    c->snd_nxt++;                                   /* FIN consumes one */
    for (int attempt = 0; attempt <= TCP_MAX_RETRIES; attempt++) {
        if (tcp_recv_seg(c->sport, dport, &flags, &rseq, &rack, data,
                         sizeof data, &dlen, &rwin) == 0) {
            if (flags & TCP_FIN) c->rcv_nxt = rseq + 1;
            if ((flags & (TCP_FIN | TCP_ACK)) == (TCP_FIN | TCP_ACK)) {
                if (tcp_send_seg(c->sport, dport, c->snd_nxt, c->rcv_nxt,
                                 TCP_ACK, 0, 0, 0, frame, &flen) < 0)
                    return -1;
                c->established = 0;
                return 0;
            }
        }
    }
    puts_("[tcp] RTO waiting for FIN ACK\n");
    c->established = 0;
    return -2;
}

/* dns.c:187 `dns_wire_query_tcp`: a fresh id, a 2-byte length prefix and the
 * message sent as two separate `tcp_send_h` calls, then the reply accumulated
 * across segment boundaries -- so this sends it the same way and reads the
 * prefix before the message. */
static int run_dns_tcp(void) {
    struct tcp_conn c;
    uint8_t frame[1518], msg[512], acc[2 + 1024], data[1500];
    uint32_t flen, dlen;
    uint8_t flags;
    uint32_t rseq, rack;
    uint16_t rwin;
    uint16_t id = (uint16_t)(dns_query_id + 1);

    if (arp_resolve(dns_ip, dns_mac) != 0) return -1;   /* cached: no frame */
    if (tcp_open_one(0, 40000, TCP_X5_PORT, &c) != 0) return -1;

    uint32_t mlen = dns_build_query(msg, id, DNS_QTYPE_A);
    uint8_t pfx[2] = { (uint8_t)(mlen >> 8), (uint8_t)mlen };
    if (tcp_send_seg(c.sport, TCP_X5_PORT, c.snd_nxt, c.rcv_nxt,
                     TCP_ACK | TCP_PSH, 0, pfx, 2, frame, &flen) < 0)
        return -1;
    c.snd_nxt += 2;
    if (tcp_send_seg(c.sport, TCP_X5_PORT, c.snd_nxt, c.rcv_nxt,
                     TCP_ACK | TCP_PSH, 0, msg, mlen, frame, &flen) < 0)
        return -1;
    c.snd_nxt += mlen;

    int have = 0, want = 2;
    for (int spin = 0; spin < 8 && have < want; spin++) {
        if (tcp_recv_seg(c.sport, TCP_X5_PORT, &flags, &rseq, &rack, data,
                         sizeof data, &dlen, &rwin) != 0)
            continue;
        if (dlen == 0) continue;                    /* the bare ACK for our data */
        for (uint32_t i = 0; i < dlen && have < (int)sizeof acc; i++)
            acc[have++] = data[i];
        if (want == 2 && have >= 2) {
            int body = (acc[0] << 8) | acc[1];
            if (body <= 0 || body > (int)sizeof acc - 2) break;
            want = 2 + body;
        }
    }
    int rc = tcp_close_one(&c, TCP_X5_PORT);
    if (have < want) {
        puts_("[dns] FAIL: TCP carriage returned "); dec((uint32_t)have);
        puts_(" of "); dec((uint32_t)want); puts_(" bytes\n");
        return -1;
    }
    int body = ((acc[0] << 8) | acc[1]);
    if (body != have - 2) {
        puts_("[dns] FAIL: TCP length prefix "); dec((uint32_t)body);
        puts_(" != the "); dec((uint32_t)(have - 2)); puts_(" bytes behind it\n");
        return -1;
    }
    uint32_t a_ip = 0;
    /* The same parser as the UDP leg, over a message that arrived framed
     * rather than in one datagram.  `dns_check_reply` expects a frame, so this
     * re-frames the message: what matters is that the parse is the guest's. */
    uint8_t framed[14 + 20 + 8 + 1024];
    uint32_t ulen = (uint32_t)(8 + (uint32_t)body);
    for (int i = 0; i < 6; i++) { framed[i] = our_mac[i]; framed[6 + i] = dns_mac[i]; }
    framed[12] = 0x08; framed[13] = 0x00;
    framed[14] = 0x45; framed[15] = 0;
    framed[16] = (uint8_t)((20 + ulen) >> 8); framed[17] = (uint8_t)(20 + ulen);
    for (int i = 18; i < 26; i++) framed[i] = 0;
    framed[22] = 64; framed[23] = 17; framed[24] = 0; framed[25] = 0;
    put_be32(framed + 26, dns_ip);
    put_be32(framed + 30, our_ip);
    framed[34] = (uint8_t)(DNS_SERVER_PORT >> 8); framed[35] = (uint8_t)DNS_SERVER_PORT;
    framed[36] = (uint8_t)(DNS_LOCAL_PORT >> 8);  framed[37] = (uint8_t)DNS_LOCAL_PORT;
    framed[38] = (uint8_t)(ulen >> 8); framed[39] = (uint8_t)ulen;
    framed[40] = 0; framed[41] = 0;
    for (int i = 0; i < body; i++) framed[42 + i] = acc[2 + i];
    int prc = dns_check_reply(framed, 14 + 20 + ulen, id, &a_ip);
    if (prc != 0) {
        puts_("[dns] FAIL: TCP answer rejected by the parser (");
        dec((uint32_t)(-prc)); puts_(")\n");
        return -1;
    }
    if (a_ip != dns_ip_resolved) {
        puts_("[dns] FAIL: TCP answer "); ipdots(a_ip);
        puts_(" != the UDP answer "); ipdots(dns_ip_resolved); putc_('\n');
        return -1;
    }
    puts_("[dns] PASS: 'example.com' -> "); ipdots(a_ip);
    puts_(" over TCP ("); dec((uint32_t)body); puts_("-byte message, prefix ok)\n");
    return rc;
}

/* tcp.c:1496 `tcp_x5_self_test`, same order: open 16, hold them, close 16. */
static int run_tcp_x5(void) {
    int established = 0, closed = 0;
    puts_("[tcp-x5] probing "); dec(TCP_MAX_CONNS);
    puts_(" concurrent TCP connections...\n");
    if (arp_resolve(dns_ip, dns_mac) != 0) return -1;
    for (uint32_t h = 0; h < TCP_MAX_CONNS; h++) {
        if (tcp_open_one(h, (uint16_t)(40000u + h), TCP_X5_PORT,
                         &tcp_conns[h]) != 0)
            break;
        established++;
    }
    if (established != TCP_MAX_CONNS) {
        puts_("[tcp-x5] FAIL: reached only "); dec((uint32_t)established);
        puts_("/"); dec(TCP_MAX_CONNS); puts_(" concurrent connections\n");
        return -1;
    }
    for (uint32_t i = 0; i < TCP_MAX_CONNS; i++)
        if (tcp_close_one(&tcp_conns[i], TCP_X5_PORT) == 0) closed++;
    if (closed != TCP_MAX_CONNS) {
        puts_("[tcp-x5] FAIL: closed only "); dec((uint32_t)closed);
        puts_("/"); dec(TCP_MAX_CONNS); putc_('\n');
        return -1;
    }
    puts_("[tcp-x5] PASS: "); dec(TCP_MAX_CONNS);
    puts_(" concurrent connections held and closed cleanly\n");
    return 0;
}

/* ---- entry --------------------------------------------------------------- */
__attribute__((section(".text._start")))
void _start(void) {
    __asm__ volatile("cli");
    nic_init();

    dns_ip = DNS_IP;

    int fail = 0;
    if (run_dhcp() != 0) {
        puts_("[net] FAIL: no DHCP lease\n");
        fail = 1;
    }
    if (!fail && run_arp() != 0) {
        puts_("[net] FAIL: gateway ARP unanswered\n");
        fail = 1;
    }
    if (!fail && run_ping() != 0) {
        puts_("[net] FAIL: no ICMP echo reply\n");
        fail = 1;
    }
    if (!fail) {
        /* The N2 composite receipt.  Kept verbatim: `n2_responder.sh` lane A
         * asserts it, and N3 only adds to the tape -- it does not rewrite the
         * receipts N2 measured. */
        puts_("[net] PASS: lease + ARP + echo reply (payload verified)\n");
    }

    /* N3: the online receipts the frozen guest runs at boot once DHCP has
     * succeeded -- kernel/kernel.c:428-430 calls net_self_test() (the ping
     * above), net_dns_self_test() and tcp_x5_self_test() in this order.  Only
     * reached with a lease, exactly like the guest, which skips the online
     * self-tests on the fallback-IP path. */
    if (!fail && run_dns() != 0) {
        puts_("[net] FAIL: no DNS answer\n");
        fail = 1;
    }
    if (!fail && run_dns_tcp() != 0) {
        puts_("[net] FAIL: no DNS answer over the TCP carriage\n");
        fail = 1;
    }
    if (!fail && run_tcp_x5() != 0) {
        puts_("[net] FAIL: the concurrent-connection gate did not hold\n");
        fail = 1;
    }
    if (!fail) {
        puts_("[net] PASS: DNS over UDP + DNS over TCP + "
              "16 concurrent TCP connections\n");
    }
    puts_(fail ? "N3FAIL\n" : "N3OK\n");
    for (;;) { __asm__ volatile("hlt"); }
}
