/* src/e1000.c -- NET N0: Intel 82540EM (e1000) "RX-ready" model + TAP socket
 * ingress.  Ground rules (fixed, N0 artifact):
 *   - TAP socket: UDP datagrams = Ethernet frames, the fixed endpoint
 *     127.0.0.1:8080 (the emulator connects; the host-side responder binds).
 *   - MAC: the QEMU default 52:54:00:12:34:56, pre-loaded into RAL/RAH like
 *     QEMU's e1000_reset (RAH.AV set) -- the guest prefers it over EEPROM.
 *   - The device is OPT-IN (--tap=): no PCI function exists without the
 *     flag, so every historical lane (K0-K8, firmware, U5 tapes) keeps its
 *     byte-for-byte receipts.
 *   - QEMU compat: the reset state follows QEMU's e1000_reset (RXCSUM reads
 *     0 after CTRL.RST, RAL/RAH preloaded, STATUS.LU on link).
 *
 * N0 covers the PCI function + BAR0 register file + reset + RX ring DMA
 * (socket -> guest).  TX (TDT doorbell -> socket) is N1. */
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>
#include "devices.h"
#include "machine.h"
#include "pci.h"

/* ============================ register file ================================ */
#define E1000_MMIO_BASE   0xFEBC0000u
#define E1000_MMIO_SIZE   0x20000u

#define E1000_CTRL        0x00000
#define E1000_STATUS      0x00008
#define E1000_EERD        0x00014
#define E1000_ICR         0x000C0
#define E1000_IMS         0x000D0
#define E1000_IMC         0x000D8
#define E1000_RCTL        0x00100
#define E1000_MDIC        0x00200
#define E1000_TCTL        0x00400
#define E1000_RDBAL       0x02800
#define E1000_RDBAH       0x02804
#define E1000_RDLEN       0x02808
#define E1000_RDH         0x02810
#define E1000_RDT         0x02818
#define E1000_TDBAL       0x03800
#define E1000_TDBAH       0x03804
#define E1000_TDLEN       0x03808
#define E1000_TDH         0x03810
#define E1000_TDT         0x03818
#define E1000_RXCSUM      0x05000
#define E1000_MTA         0x05200
#define E1000_RA          0x05400          /* RAL0/RAH0 at 0x5400/0x5404 */
#define E1000_REG_BYTES   0x5800

#define CTRL_FD           (1u << 0)
#define CTRL_SLU          (1u << 6)
#define CTRL_RST          (1u << 26)
#define STATUS_LU         (1u << 1)
#define RCTL_EN           (1u << 1)
#define RCTL_BAM          (1u << 15)
#define RCTL_SECRC        (1u << 26)
#define TCTL_EN           (1u << 1)
#define TCTL_PSP          (1u << 3)
#define RAH_AV            (1u << 31)
#define ICR_RXT0          (1u << 7)
#define RXD_STAT_DD       0x01

typedef struct {
    machine_t *m;
    uint32_t regs[E1000_REG_BYTES / 4];
    int tap_fd;            /* -1 = no TAP socket */
    int attached;
} e1000_t;
static e1000_t g_e1000;

/* QEMU's e1000_reset behavioural core: zeroed register file, link up,
 * pre-initialized RAL/RAH (AddrValid + the QEMU default MAC), RXCSUM 0. */
static void e1000_reset_regs(void) {
    memset(g_e1000.regs, 0, sizeof g_e1000.regs);
    g_e1000.regs[E1000_STATUS / 4] = STATUS_LU;    /* link up on attach */
    /* 52:54:00:12:34:56 -> RAL = 0x12005452, RAH = AV | 0x5634 */
    g_e1000.regs[E1000_RA / 4] = 0x12005452u;
    g_e1000.regs[(E1000_RA + 4) / 4] = RAH_AV | 0x5634u;
}

/* =============================== MMIO ===================================== */
uint64_t e1000_mmio_read(void *ctx, uint64_t addr, int size) {
    e1000_t *e = ctx;
    (void)size;
    uint32_t off = (uint32_t)(addr - E1000_MMIO_BASE) & ~3u;
    if (off + 4 > E1000_REG_BYTES) return 0;
    if (off == E1000_ICR) {                 /* read-to-clear */
        uint32_t v = e->regs[E1000_ICR / 4];
        e->regs[E1000_ICR / 4] = 0;
        return v;
    }
    return e->regs[off / 4];
}

void e1000_mmio_write(void *ctx, uint64_t addr, int size, uint64_t val) {
    e1000_t *e = ctx;
    (void)size;
    uint32_t off = (uint32_t)(addr - E1000_MMIO_BASE) & ~3u;
    if (off + 4 > E1000_REG_BYTES) return;
    if (off == E1000_CTRL) {
        if (val & CTRL_RST) {
            e1000_reset_regs();             /* QEMU: RST self-clears */
            return;
        }
        e->regs[E1000_CTRL / 4] = (uint32_t)val;
        return;
    }
    if (off == E1000_ICR || off == E1000_IMC) {
        /* ICR is read-to-clear (W1C-nothing); IMC clears the mask */
        if (off == E1000_IMC) e->regs[E1000_IMS / 4] &= ~(uint32_t)val;
        return;
    }
    e->regs[off / 4] = (uint32_t)val;       /* RAL/RAH, rings, RCTL, ... */
}

/* ============================ RX ring ingress ============================== */
/* One Ethernet frame from the TAP socket into the guest's RX ring: DMA the
 * frame into the buffer of the head descriptor, mark DD, advance RDH. */
void e1000_rx_frame(machine_t *m, const uint8_t *frame, unsigned len) {
    if (m != g_e1000.m || !g_e1000.attached) return;
    uint32_t rctl = g_e1000.regs[E1000_RCTL / 4];
    if (!(rctl & RCTL_EN)) return;                 /* receiver off: drop */
    if (len > 2048) len = 2048;                    /* one rx buffer */
    uint64_t base = (uint64_t)g_e1000.regs[E1000_RDBAL / 4] |
                    ((uint64_t)g_e1000.regs[E1000_RDBAH / 4] << 32);
    uint32_t rdlen = g_e1000.regs[E1000_RDLEN / 4];
    if (!base || rdlen < 16) return;
    uint32_t n = rdlen / 16;                       /* descriptor count */
    uint32_t rdh = g_e1000.regs[E1000_RDH / 4] % n;
    uint32_t rdt = g_e1000.regs[E1000_RDT / 4] % n;
    if (rdh == rdt) return;                        /* ring full: drop */
    /* descriptor: addr_lo, addr_hi, length, csum, status, errors, special */
    uint64_t desc = base + (uint64_t)rdh * 16;
    uint32_t addr_lo = mem_read(m, desc, 4);
    uint32_t addr_hi = mem_read(m, desc + 4, 4);
    uint64_t addr = (uint64_t)addr_lo | ((uint64_t)addr_hi << 32);
    if (addr) {
        for (unsigned i = 0; i < len; i++)
            mem_write(m, addr + i, 1, frame[i]);
    }
    mem_write(m, desc + 8, 2, len);                /* length */
    mem_write(m, desc + 12, 1, RXD_STAT_DD);       /* status: DD */
    g_e1000.regs[E1000_RDH / 4] = (rdh + 1) % n;
    g_e1000.regs[E1000_ICR / 4] |= ICR_RXT0;
}

/* ===================== device model + TAP attach ========================== */
static void e1000_add(machine_t *m) {
    g_e1000.m = m;
    g_e1000.attached = 1;
    e1000_reset_regs();
    /* PCI 8086:100E + the BAR0 register file.  BAR0 is seeded into config
     * space (the house pattern, cf. EHCI/UHCI/AHCI): the guest maps the
     * window through pci_get_bar() and the direct-load lane has no BIOS
     * to assign it. */
    pci_dev_t *e_pci = pci_add_device(m, 0, 4, 0, "e1000", 0x8086, 0x100E, 0x02, 0x00, 0x00);
    e_pci->cfg[0x10] = (uint8_t)(E1000_MMIO_BASE);
    e_pci->cfg[0x11] = (uint8_t)(E1000_MMIO_BASE >> 8);
    e_pci->cfg[0x12] = (uint8_t)(E1000_MMIO_BASE >> 16);
    e_pci->cfg[0x13] = (uint8_t)(E1000_MMIO_BASE >> 24);
    mem_register_mmio(m, E1000_MMIO_BASE, E1000_MMIO_SIZE,
                      e1000_mmio_read, e1000_mmio_write, &g_e1000,
                      "e1000 MMIO");
}

/* Unit entry point: the device model without any socket. */
void e1000_test_add(machine_t *m) {
    g_e1000.tap_fd = -1;
    e1000_add(m);
}

/* NET N0 ground rule: the fixed endpoint 127.0.0.1:8080 (UDP). */
int e1000_attach(machine_t *m, const char *hostport) {
    char host[64] = "127.0.0.1";
    unsigned port = 8080;
    if (strchr(hostport, ':')) {
        if (sscanf(hostport, "%63[^:]:%u", host, &port) != 2) return -1;
    } else if (hostport[0]) {
        /* bare host: keep the fixed port */
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
    g_e1000.tap_fd = fd;
    e1000_add(m);
    return 0;
}

void e1000_tick(void) {
    /* pull TAP datagrams into the RX ring at the device cadence */
    if (g_e1000.tap_fd < 0 || !g_e1000.attached) return;
    uint8_t buf[2048];
    for (int i = 0; i < 8; i++) {
        ssize_t n = recv(g_e1000.tap_fd, buf, sizeof buf, 0);
        if (n <= 0) break;
        e1000_rx_frame(g_e1000.m, buf, (unsigned)n);
    }
}
