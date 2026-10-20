// xhci.c -- USB 3.0 host controller (XHCI plan, docs/plans/XHCI_PLAN.md).
//
// X1: PCI identity 1b36:000d (QEMU qemu-xhci parity) + the full register
// file: capability, operational, runtime/interrupter-0, doorbell and
// root-port registers, the HCRST/CNR handshake and PORTSC RW1C
// discipline.  Rings and transfers are X2+; doorbells are accepted and
// ignored here.  Everything the guest driver touches is 32-bit MMIO
// (AuraLite-OS@main drivers/usb/xhci.c, XHCI_PLAN section 1a); other
// sizes compose from the containing dword like the AHCI model does.
#include <stdint.h>
#include <string.h>
#include "xhci.h"
#include "pci.h"
#include "devices.h"        /* X4: usb_stick_serve (shared MSC model) */

/* capability register values (BAR0-relative dword reads) */
#define X_CAP_LEN        0x40u
#define X_HCI_VERSION    0x0100u
#define X_HCSPARAMS1     ((XHCI_MAX_PORTS << 24) | (1u << 8) | XHCI_MAX_SLOTS)
#define X_HCCPARAMS1     0x1u          /* AC64; CSZ=0 (32B contexts), PPC=0 */
#define X_DBOFF          0x1000u
#define X_RTSOFF         0x0800u
/* operational register offsets (from BAR0 + X_CAP_LEN) */
#define OP_USBCMD        0x00
#define OP_USBSTS        0x04
#define OP_PAGESIZE      0x08
#define OP_DNCTRL        0x14
#define OP_CRCR          0x18
#define OP_DCBAAP        0x30
#define OP_CONFIG        0x38
#define OP_PORT_BASE     0x400
#define OP_PORT_STRIDE   0x10
/* USBCMD bits */
#define CMD_RUN          (1u << 0)
#define CMD_HCRST        (1u << 1)
#define CMD_INTE         (1u << 2)
#define CMD_HSEE         (1u << 3)
#define CMD_LHCRST       (1u << 7)
/* USBSTS bits */
#define STS_HCH          (1u << 0)
#define STS_EINT         (1u << 3)
#define STS_PCD          (1u << 4)
/* CRCR bits */
#define CRCR_CRR         (1u << 3)
/* PORTSC bits */
#define P_CCS            (1u << 0)
#define P_PED            (1u << 1)
#define P_PR             (1u << 4)
#define P_PP             (1u << 9)
#define P_SPEED_HS       (3u << 10)
#define P_WPR            (1u << 31)
#define P_CSC            (1u << 17)
#define P_WRC            (1u << 19)
#define P_PRC            (1u << 21)
#define P_WAKE           ((1u << 25) | (1u << 26) | (1u << 27))
#define P_CHANGE         ((1u << 17) | (1u << 18) | (1u << 19) | (1u << 20) | \
                          (1u << 21) | (1u << 22) | (1u << 23))
/* runtime: interrupter 0 offsets (from BAR0 + X_RTSOFF) */
#define RT_IMAN          0x20
#define RT_IMOD          0x24
#define RT_ERSTSZ        0x28
#define RT_ERSTBA        0x30
#define RT_ERDP          0x38
#define IMAN_IP          (1u << 0)
#define IMAN_IE          (1u << 1)

/* X3: per-endpoint transfer-ring state.  `state` uses the spec EP State
 * values written to the output context: 0 Disabled, 1 Running,
 * 2 Halted, 3 Stopped. */
typedef struct {
    uint64_t deq;
    int ccs;
    int state;
    uint16_t max_packet;
    uint8_t type;           /* EP Type from the input context */
} xhci_ep_t;

typedef struct {
    int addressed;          /* Address Device has run (slot state >= 2) */
    uint8_t config;         /* SET_CONFIGURATION value seen on EP0 */
    xhci_ep_t ep[31];       /* ep_id 1..31 at [0..30] */
} xhci_slot_t;

typedef struct {
    machine_t *m;
    uint32_t base;          /* live BAR0 decode base */
    int bar_probe;          /* BAR sizing probe in flight */
    uint32_t usbcmd, config, dnctrl;
    uint64_t crcr, dcbaap;
    uint32_t port_change[XHCI_MAX_PORTS];   /* latched RW1C bits */
    uint32_t port_wake[XHCI_MAX_PORTS];     /* WCE|WDE|WOE */
    uint8_t port_ped[XHCI_MAX_PORTS];       /* X5: reset has enabled it */
    int iman_ie;
    uint32_t imod, erstsz;
    uint64_t erstba, erdp;
    int intx_level;         /* last level driven on XHCI_IRQ */
    /* X2: command ring consumer + event ring producer.  ev_prod/ev_cons
     * are absolute event counts; the ring position and producer cycle
     * derive from them (cycle of event n = 1 while n < ring size). */
    uint64_t cmd_deq;       /* next command TRB to fetch */
    int cmd_ccs;            /* command consumer cycle state */
    uint32_t slot_mask;     /* enabled slots (bit s-1 = slot s) */
    uint64_t ev_prod, ev_cons;
    int iman_ip;            /* interrupter pending latch (EINT mirror) */
    xhci_slot_t slots[XHCI_MAX_SLOTS];   /* X3: slot s at [s-1] */
} xhci_t;

static xhci_t g_xhci;

/* X5: the INTx pin.  Asserted while interrupter 0 has an unmasked
 * pending event (IMAN.IP & IMAN.IE & USBCMD.INTE) -- level-driven, on
 * change only (the AHCI S5 convention; pic_set_irq fans out to the
 * I/O APIC board wire). */
static void xhci_intx_update(xhci_t *x) {
    int lvl = (x->iman_ip && x->iman_ie && (x->usbcmd & CMD_INTE)) ? 1 : 0;
    if (lvl != x->intx_level) {
        x->intx_level = lvl;
        if (x->m) pic_set_irq(x->m, XHCI_IRQ, lvl);
    }
}

static int ev_post(xhci_t *x, uint64_t param, uint32_t status, uint32_t control);

/* X5: the stick sits on root port 1 (the uhci_port_attached rule). */
static int port_attached(xhci_t *x, int p) {
    return p == 0 && x->m && x->m->disk_len > 0;
}

/* ---- host-controller reset: every software-visible register to its
 * power-on value (XHCI_PLAN X1; the guest's init step 2 waits on the
 * HCRST bit self-clearing and on CNR clearing, both immediate here). */
static void xhci_hw_reset(xhci_t *x) {
    machine_t *m = x->m;
    uint32_t base = x->base;
    int probe = x->bar_probe;
    if (x->intx_level && m) pic_set_irq(m, XHCI_IRQ, 0);
    memset(x, 0, sizeof *x);
    x->m = m;
    x->base = base;
    x->bar_probe = probe;
}

static uint32_t op_read(xhci_t *x, uint32_t off) {
    switch (off) {
    case OP_USBCMD:  return x->usbcmd;
    case OP_USBSTS: {
        uint32_t v = 0;
        if (!(x->usbcmd & CMD_RUN)) v |= STS_HCH;
        if (x->iman_ip) v |= STS_EINT;
        for (int p = 0; p < XHCI_MAX_PORTS; p++)
            if (x->port_change[p]) { v |= STS_PCD; break; }
        return v;
    }
    case OP_PAGESIZE: return 1;                 /* 4 KiB pages */
    case OP_DNCTRL:   return x->dnctrl;
    case OP_CRCR: {
        uint32_t v = (uint32_t)(x->crcr & ~0xFu) | (uint32_t)(x->crcr & 1u);
        if ((x->usbcmd & CMD_RUN) && (x->crcr & ~0xFull)) v |= CRCR_CRR;
        return v;
    }
    case OP_CRCR + 4:  return (uint32_t)(x->crcr >> 32);
    case OP_DCBAAP:    return (uint32_t)x->dcbaap;
    case OP_DCBAAP + 4:return (uint32_t)(x->dcbaap >> 32);
    case OP_CONFIG:    return x->config;
    default: break;
    }
    if (off >= OP_PORT_BASE && off < OP_PORT_BASE + XHCI_MAX_PORTS * OP_PORT_STRIDE) {
        int p = (int)((off - OP_PORT_BASE) / OP_PORT_STRIDE);
        uint32_t sub = (off - OP_PORT_BASE) % OP_PORT_STRIDE;
        if (sub == 0) {   /* PORTSC: PPC=0 -> power always on */
            uint32_t v = P_PP | x->port_change[p] | x->port_wake[p];
            if (port_attached(x, p)) {
                v |= P_CCS | P_SPEED_HS;        /* USB2 stick, EHCI parity */
                if (x->port_ped[p]) v |= P_PED;
            }
            return v;
        }
        return 0;       /* PORTPMSC / PORTLI / reserved */
    }
    return 0;
}

static void op_write(xhci_t *x, uint32_t off, uint32_t val) {
    switch (off) {
    case OP_USBCMD: {
        if (val & CMD_HCRST) { xhci_hw_reset(x); return; }
        int was_run = (x->usbcmd & CMD_RUN) != 0;
        x->usbcmd = val & (CMD_RUN | CMD_INTE | CMD_HSEE);
        if (!was_run && (x->usbcmd & CMD_RUN)) {
            /* X5: silicon reports connected ports once the controller
             * runs -- CSC + a Port Status Change event per attached
             * port (the guest's hotplug handler peeks the ring head
             * for type 34; its boot scan reads CCS directly). */
            for (int p = 0; p < XHCI_MAX_PORTS; p++) {
                if (!port_attached(x, p)) continue;
                x->port_change[p] |= P_CSC;
                ev_post(x, ((uint64_t)(uint32_t)(p + 1) << 24),
                        1u << 24, 34u << 10);
            }
        }
        xhci_intx_update(x);
        return;
    }
    case OP_USBSTS:  return;                    /* HSE/EINT never latch in X1 */
    case OP_DNCTRL:  x->dnctrl = val; return;
    case OP_CRCR:
        x->crcr = (x->crcr & 0xFFFFFFFF00000000ull) | (val & ~0x6u);
        x->cmd_deq = x->crcr & ~0xFull;         /* X2: consumer restarts */
        x->cmd_ccs = (int)(x->crcr & 1u);
        return;
    case OP_CRCR + 4:
        x->crcr = (x->crcr & 0xFFFFFFFFull) | ((uint64_t)val << 32);
        x->cmd_deq = x->crcr & ~0xFull;
        return;
    case OP_DCBAAP:
        x->dcbaap = (x->dcbaap & 0xFFFFFFFF00000000ull) | (val & ~0x3Fu);
        return;
    case OP_DCBAAP + 4:
        x->dcbaap = (x->dcbaap & 0xFFFFFFFFull) | ((uint64_t)val << 32);
        return;
    case OP_CONFIG:  x->config = val; return;
    default: break;
    }
    if (off >= OP_PORT_BASE && off < OP_PORT_BASE + XHCI_MAX_PORTS * OP_PORT_STRIDE) {
        int p = (int)((off - OP_PORT_BASE) / OP_PORT_STRIDE);
        uint32_t sub = (off - OP_PORT_BASE) % OP_PORT_STRIDE;
        if (sub == 0) {   /* PORTSC: RW1C change bits + wake enables */
            x->port_change[p] &= ~(val & P_CHANGE);
            x->port_wake[p] = val & P_WAKE;
            /* X5: Port Reset / Warm Reset complete synchronously, like
             * everything else in this engine: PR never reads back set
             * (the guest's wait exits at once), the port enables, and
             * the completion latches + posts its Port Status Change. */
            if (port_attached(x, p) && (val & (P_PR | P_WPR))) {
                x->port_ped[p] = 1;
                x->port_change[p] |= (val & P_PR) ? P_PRC : P_WRC;
                if (x->usbcmd & CMD_RUN)
                    ev_post(x, ((uint64_t)(uint32_t)(p + 1) << 24),
                            1u << 24, 34u << 10);
            }
        }
        return;
    }
}

static uint32_t rt_read(xhci_t *x, uint32_t off) {
    switch (off) {
    case RT_IMAN:    return (x->iman_ie ? IMAN_IE : 0) | (x->iman_ip ? IMAN_IP : 0);
    case RT_IMOD:    return x->imod;
    case RT_ERSTSZ:  return x->erstsz;
    case RT_ERSTBA:  return (uint32_t)x->erstba;
    case RT_ERSTBA + 4: return (uint32_t)(x->erstba >> 32);
    case RT_ERDP:    return (uint32_t)x->erdp | (x->iman_ip ? 0x8u : 0);  /* EHB */
    case RT_ERDP + 4:   return (uint32_t)(x->erdp >> 32);
    default:         return 0;   /* MFINDEX + interrupters 1.. (MaxIntrs=1) */
    }
}

static void ev_consume_update(xhci_t *x);

static void rt_write(xhci_t *x, uint32_t off, uint32_t val) {
    switch (off) {
    case RT_IMAN:
        if (val & IMAN_IP) x->iman_ip = 0;              /* RW1C */
        x->iman_ie = (val & IMAN_IE) != 0;
        xhci_intx_update(x);
        return;
    case RT_IMOD:    x->imod = val; return;
    case RT_ERSTSZ:  x->erstsz = val & 0xFFFFu; return;
    case RT_ERSTBA:
        x->erstba = (x->erstba & 0xFFFFFFFF00000000ull) | (val & ~0x3Fu);
        x->ev_prod = x->ev_cons = 0;                   /* X2: fresh table */
        return;
    case RT_ERSTBA + 4:
        x->erstba = (x->erstba & 0xFFFFFFFFull) | ((uint64_t)val << 32);
        return;
    case RT_ERDP:
        x->erdp = (x->erdp & 0xFFFFFFFF00000000ull) | (val & ~0xFu);
        if (val & 0x8u) x->iman_ip = 0;                 /* EHB write-1-clear */
        ev_consume_update(x);
        xhci_intx_update(x);
        return;
    case RT_ERDP + 4:
        x->erdp = (x->erdp & 0xFFFFFFFFull) | ((uint64_t)val << 32);
        ev_consume_update(x);
        return;
    default: return;
    }
}

/* =================== X2: command ring + event ring =====================
 * The guest polls the event ring after ringing a doorbell (XHCI_PLAN
 * section 1a), so completion is synchronous at doorbell-write time: the
 * consumer walks the command ring while the Cycle bit matches the
 * consumer cycle state, executes, and posts Command Completion events.
 * Guest memory goes through mem_read/mem_write with physical addresses
 * (the AHCI gdr/gdw pattern).  Completion codes are the spec values the
 * guest branches on only as cc==1 vs not: 1 Success, 5 TRB Error,
 * 9 No Slots Available, 11 Slot Not Enabled. */

static uint32_t g_rd32(xhci_t *x, uint64_t pa) { return (uint32_t)mem_read(x->m, pa, 4); }
static void g_wr32(xhci_t *x, uint64_t pa, uint32_t v) { mem_write(x->m, pa, 4, v); }

/* total event-ring capacity in TRBs, walked from the guest's ERST */
static uint32_t ev_total(xhci_t *x) {
    uint32_t total = 0;
    if (!x->erstba || !x->erstsz) return 0;
    for (uint32_t i = 0; i < x->erstsz && i < 32; i++)
        total += g_rd32(x, x->erstba + (uint64_t)i * 16 + 8);
    return total;
}

/* physical address of ring position `pos` (linear across segments) */
static uint64_t ev_addr(xhci_t *x, uint32_t pos) {
    for (uint32_t i = 0; i < x->erstsz && i < 32; i++) {
        uint64_t ent = x->erstba + (uint64_t)i * 16;
        uint64_t base = (uint64_t)g_rd32(x, ent) | ((uint64_t)g_rd32(x, ent + 4) << 32);
        uint32_t size = g_rd32(x, ent + 8);
        if (pos < size) return base + (uint64_t)pos * 16;
        pos -= size;
    }
    return 0;
}

/* consumer mirror: advance ev_cons to the ERDP the guest just wrote */
static void ev_consume_update(xhci_t *x) {
    uint32_t total = ev_total(x);
    if (!total) return;
    uint64_t deq = x->erdp & ~0xFull;
    uint32_t pos = 0;
    int found = 0;
    for (uint32_t i = 0; i < x->erstsz && i < 32 && !found; i++) {
        uint64_t ent = x->erstba + (uint64_t)i * 16;
        uint64_t base = (uint64_t)g_rd32(x, ent) | ((uint64_t)g_rd32(x, ent + 4) << 32);
        uint32_t size = g_rd32(x, ent + 8);
        if (deq >= base && deq < base + (uint64_t)size * 16) {
            pos += (uint32_t)((deq - base) / 16);
            found = 1;
        } else {
            pos += size;
        }
    }
    if (!found) return;
    uint32_t cur = (uint32_t)(x->ev_cons % total);
    x->ev_cons += (pos + total - cur) % total;
}

/* post one event TRB; -1 if the ring is unprogrammed or full (dropped) */
static int ev_post(xhci_t *x, uint64_t param, uint32_t status, uint32_t control) {
    uint32_t total = ev_total(x);
    if (!total) return -1;
    if (x->ev_prod - x->ev_cons >= total) return -1;   /* full: never overwrite */
    uint32_t pos = (uint32_t)(x->ev_prod % total);
    uint32_t cycle = ((x->ev_prod / total) & 1) ? 0 : 1;
    uint64_t at = ev_addr(x, pos);
    if (!at) return -1;
    g_wr32(x, at + 0, (uint32_t)param);
    g_wr32(x, at + 4, (uint32_t)(param >> 32));
    g_wr32(x, at + 8, status);
    g_wr32(x, at + 12, control | cycle);
    x->ev_prod++;
    x->iman_ip = 1;
    xhci_intx_update(x);
    return 0;
}

#define CC_SUCCESS        1u
#define CC_TRB_ERROR      5u
#define CC_STALL          6u
#define CC_NO_SLOTS       9u
#define CC_SLOT_NOT_EN    11u
#define CC_SHORT_PACKET   13u
#define CC_EP_NOT_EN      14u

/* ==================== X3: contexts + device commands ===================
 * 32-byte contexts (HCCPARAMS1 CSZ=0).  Input Context: ICC at +0 (Drop
 * flags dword0, Add flags dword1), Slot at +32, EP[ep_id] at
 * +(1+ep_id)*32.  Output Device Context via DCBAA[slot]: Slot at +0,
 * EP[ep_id] at +ep_id*32.  What the guest reads back (xhci.c:1215-1226)
 * is the Slot State + hardware address in slot dword3 -- state must say
 * Addressed -- so the output contexts are written from the same
 * internal state that runs the rings.  Hardware address = slot id. */

#define CTX 32u

static uint64_t devctx_base(xhci_t *x, uint32_t slot) {
    if (!x->dcbaap) return 0;
    uint64_t ent = x->dcbaap + (uint64_t)slot * 8;
    return (uint64_t)g_rd32(x, ent) | ((uint64_t)g_rd32(x, ent + 4) << 32);
}

static void ep_from_ctx(xhci_t *x, xhci_ep_t *ep, uint64_t ctx) {
    uint32_t d1 = g_rd32(x, ctx + 4);
    uint32_t d2 = g_rd32(x, ctx + 8);
    uint32_t d3 = g_rd32(x, ctx + 12);
    ep->type = (uint8_t)((d1 >> 3) & 7u);
    ep->max_packet = (uint16_t)(d1 >> 16);
    ep->deq = (((uint64_t)d3 << 32) | d2) & ~0xFull;
    ep->ccs = (int)(d2 & 1u);
    ep->state = 1;                              /* Running */
}

/* copy an input EP context to the output context with the live EP State
 * and the consumer dequeue pointer the ring actually uses */
static void ep_to_ctx(xhci_t *x, const xhci_ep_t *ep, uint64_t in, uint64_t out) {
    g_wr32(x, out + 0, (g_rd32(x, in) & ~7u) | (uint32_t)ep->state);
    g_wr32(x, out + 4, g_rd32(x, in + 4));
    g_wr32(x, out + 8, (uint32_t)ep->deq | (uint32_t)ep->ccs);
    g_wr32(x, out + 12, (uint32_t)(ep->deq >> 32));
    g_wr32(x, out + 16, g_rd32(x, in + 16));
}

static void slot_state_wr(xhci_t *x, uint64_t out, uint32_t slot, uint32_t state) {
    uint32_t d3 = g_rd32(x, out + 12);
    g_wr32(x, out + 12, (d3 & 0x07FFFF00u) | (state << 27) | (slot & 0xFFu));
}

static uint32_t cmd_ctx(xhci_t *x, uint32_t type, const uint32_t trb[4], uint32_t slot) {
    if (slot == 0 || slot > XHCI_MAX_SLOTS) return CC_TRB_ERROR;
    if (!(x->slot_mask & (1u << (slot - 1)))) return CC_SLOT_NOT_EN;
    xhci_slot_t *sl = &x->slots[slot - 1];
    uint64_t param = (uint64_t)trb[0] | ((uint64_t)trb[1] << 32);
    uint32_t epid = (trb[3] >> 16) & 0x1Fu;
    uint64_t out = devctx_base(x, slot);

    switch (type) {
    case 11: {                                  /* Address Device */
        if (!out) return CC_TRB_ERROR;          /* no DCBAA entry yet */
        uint64_t in = param;
        for (int i = 0; i < 3; i++)
            g_wr32(x, out + 4 * i, g_rd32(x, in + CTX + 4 * i));
        ep_from_ctx(x, &sl->ep[0], in + 2 * CTX);
        ep_to_ctx(x, &sl->ep[0], in + 2 * CTX, out + CTX);
        sl->addressed = 1;
        slot_state_wr(x, out, slot, (trb[3] & (1u << 9)) ? 1 : 2);  /* BSR */
        return CC_SUCCESS;
    }
    case 12: {                                  /* Configure Endpoint */
        if (!sl->addressed || !out) return CC_SLOT_NOT_EN;
        if (trb[3] & (1u << 9)) {               /* DC: deconfigure */
            for (int e = 1; e < 31; e++) sl->ep[e].state = 0;
            slot_state_wr(x, out, slot, 2);
            return CC_SUCCESS;
        }
        uint64_t in = param;
        uint32_t drop = g_rd32(x, in), add = g_rd32(x, in + 4);
        if (add & 1u)
            for (int i = 0; i < 2; i++)
                g_wr32(x, out + 4 * i, g_rd32(x, in + CTX + 4 * i));
        for (uint32_t e = 1; e < 32; e++) {
            if (drop & (1u << e)) {
                sl->ep[e - 1].state = 0;
                g_wr32(x, out + e * CTX, g_rd32(x, out + e * CTX) & ~7u);
            } else if (add & (1u << e)) {
                ep_from_ctx(x, &sl->ep[e - 1], in + (1 + e) * CTX);
                ep_to_ctx(x, &sl->ep[e - 1], in + (1 + e) * CTX, out + e * CTX);
            }
        }
        slot_state_wr(x, out, slot, 3);         /* Configured */
        return CC_SUCCESS;
    }
    case 13: {  /* Evaluate Context: MaxPacket/type refresh only.  The
                 * input's dequeue field carries ring base + producer
                 * cycle (xhci_update_max_packet0) and must NOT move the
                 * consumer -- measured against the guest. */
        if (!sl->addressed || !out) return CC_SLOT_NOT_EN;
        uint64_t in = param;
        uint32_t add = g_rd32(x, in + 4);
        for (uint32_t e = 1; e < 32; e++) {
            if (!(add & (1u << e))) continue;
            xhci_ep_t *ep = &sl->ep[e - 1];
            if (!ep->state) return CC_EP_NOT_EN;
            uint32_t d1 = g_rd32(x, in + (1 + e) * CTX + 4);
            ep->type = (uint8_t)((d1 >> 3) & 7u);
            ep->max_packet = (uint16_t)(d1 >> 16);
            g_wr32(x, out + e * CTX + 4, d1);
        }
        return CC_SUCCESS;
    }
    case 14:                                    /* Stop Endpoint */
    case 15: {                                  /* Reset Endpoint */
        if (epid == 0) return CC_TRB_ERROR;
        xhci_ep_t *ep = &sl->ep[epid - 1];
        if (!ep->state) return CC_EP_NOT_EN;
        ep->state = (type == 15) ? 1 : 3;
        if (out) {
            uint64_t o = out + (uint64_t)epid * CTX;
            g_wr32(x, o, (g_rd32(x, o) & ~7u) | (uint32_t)ep->state);
            g_wr32(x, o + 8, (uint32_t)ep->deq | (uint32_t)ep->ccs);
            g_wr32(x, o + 12, (uint32_t)(ep->deq >> 32));
        }
        return CC_SUCCESS;
    }
    case 16: {                                  /* Set TR Dequeue Pointer */
        if (epid == 0) return CC_TRB_ERROR;
        xhci_ep_t *ep = &sl->ep[epid - 1];
        if (!ep->state) return CC_EP_NOT_EN;
        ep->deq = param & ~0xFull;
        ep->ccs = (int)(param & 1u);
        if (out) {
            uint64_t o = out + (uint64_t)epid * CTX;
            g_wr32(x, o + 8, (uint32_t)ep->deq | (uint32_t)ep->ccs);
            g_wr32(x, o + 12, (uint32_t)(ep->deq >> 32));
        }
        return CC_SUCCESS;
    }
    default:
        return CC_TRB_ERROR;
    }
}

static void cmd_process(xhci_t *x) {
    if (!(x->usbcmd & CMD_RUN)) return;
    for (int budget = 0; budget < 1024; budget++) {
        uint32_t trb[4];
        for (int i = 0; i < 4; i++) trb[i] = g_rd32(x, x->cmd_deq + 4 * i);
        if ((trb[3] & 1u) != (uint32_t)x->cmd_ccs) break;   /* not owned */
        uint32_t type = (trb[3] >> 10) & 0x3Fu;
        uint64_t at = x->cmd_deq;
        if (type == 6) {                                    /* Link TRB */
            x->cmd_deq = (((uint64_t)trb[1] << 32) | trb[0]) & ~0xFull;
            if (trb[3] & 2u) x->cmd_ccs ^= 1;               /* TC */
            continue;
        }
        x->cmd_deq += 16;
        uint32_t slot = (trb[3] >> 24) & 0xFFu, cc = CC_SUCCESS;
        switch (type) {
        case 23:                                            /* No Op */
            slot = 0;
            break;
        case 9: {                                           /* Enable Slot */
            uint32_t maxen = x->config & 0xFFu;
            if (maxen > XHCI_MAX_SLOTS) maxen = XHCI_MAX_SLOTS;
            uint32_t s = 1;
            while (s <= maxen && (x->slot_mask & (1u << (s - 1)))) s++;
            if (s > maxen) { cc = CC_NO_SLOTS; slot = 0; }
            else { x->slot_mask |= (1u << (s - 1)); slot = s; }
            break;
        }
        case 10:                                            /* Disable Slot */
            if (slot == 0 || slot > XHCI_MAX_SLOTS) cc = CC_TRB_ERROR;
            else if (!(x->slot_mask & (1u << (slot - 1)))) cc = CC_SLOT_NOT_EN;
            else {
                x->slot_mask &= ~(1u << (slot - 1));
                memset(&x->slots[slot - 1], 0, sizeof x->slots[0]);
            }
            break;
        case 11: case 12: case 13: case 14: case 15: case 16:
            cc = cmd_ctx(x, type, trb, slot);               /* X3 */
            break;
        default:                                            /* other cmds */
            cc = CC_TRB_ERROR;
            break;
        }
        ev_post(x, at, cc << 24, (33u << 10) | (slot << 24));
    }
}

/* ==================== X3: EP0 control transfers ========================
 * One TD (Setup -> [Data] -> Status) runs to completion at doorbell
 * time and posts exactly one Transfer Event: the guest takes the first
 * event matching slot+endpoint and parks the rest (xhci.c:1396-1420),
 * so a second event would be consumed as the next transfer's answer.
 * The residue lands in the event Status dword[23:0] (its `control`
 * field) and is only believed on cc=13 (xhci.c:1442-1448, 1584): a
 * short IN must therefore complete Short Packet, never Success, or
 * the guest trusts bytes that were never written.  A stalled request
 * halts the endpoint until Reset Endpoint + Set TR Dequeue -- the
 * guest's recovery path (xhci.c:1330-1360).  The device behind EP0 is
 * the same MSC stick the UHCI/EHCI models expose (0525:a4a5); bulk
 * traffic joins it in X4. */

static const uint8_t x_desc_device[18] = {
    18, 1, 0x00, 0x02, 0, 0, 0, 64,
    0x25, 0x05, 0xA5, 0xA4, 0x00, 0x01, 1, 2, 3, 1
};
static const uint8_t x_desc_config[32] = {
    9, 2, 32, 0, 1, 1, 0, 0x80, 50,
    9, 4, 0, 0, 2, 0x08, 0x06, 0x50, 0,
    7, 5, 0x81, 2, 0x00, 0x02, 0,
    7, 5, 0x02, 2, 0x00, 0x02, 0
};
static const uint8_t x_desc_str0[4] = { 4, 3, 0x09, 0x04 };

static uint32_t x_str_desc(uint8_t idx, uint8_t *out) {
    static const char *s[3] = { "AURALITE", "USB DISK", "1234567890" };
    const char *p = s[(idx - 1) % 3];
    uint32_t n = 0;
    while (p[n]) n++;
    out[0] = (uint8_t)(2 + 2 * n); out[1] = 3;
    for (uint32_t i = 0; i < n; i++) { out[2 + 2 * i] = (uint8_t)p[i]; out[3 + 2 * i] = 0; }
    return 2 + 2 * n;
}

/* run one control request; for IN transfers *actual is how many bytes
 * the device offers (wLength-bounded).  Returns 0, or -1 for Stall. */
static int dev_control(xhci_slot_t *sl, const uint8_t su[8], uint8_t *buf,
                       uint32_t *actual) {
    uint8_t rt = su[0], req = su[1];
    uint32_t wv = su[2] | ((uint32_t)su[3] << 8);
    uint32_t wl = su[6] | ((uint32_t)su[7] << 8);
    *actual = 0;
    if (rt == 0x80 && req == 0x06) {            /* GET_DESCRIPTOR */
        uint32_t dt = wv >> 8, idx = wv & 0xFF, n;
        if (dt == 1)      { memcpy(buf, x_desc_device, sizeof x_desc_device); n = sizeof x_desc_device; }
        else if (dt == 2) { memcpy(buf, x_desc_config, sizeof x_desc_config); n = sizeof x_desc_config; }
        else if (dt == 3 && idx == 0) { memcpy(buf, x_desc_str0, 4); n = 4; }
        else if (dt == 3 && idx >= 1 && idx <= 3) n = x_str_desc((uint8_t)idx, buf);
        else return -1;                         /* no such descriptor */
        *actual = n < wl ? n : wl;
        return 0;
    }
    if (rt == 0x00 && req == 0x09) { sl->config = (uint8_t)wv; return 0; }
    if (rt == 0x00 && req == 0x05) return 0;    /* SET_ADDRESS: host-owned */
    if ((rt & 0x81) == 0x80 && req == 0x00) {   /* GET_STATUS */
        buf[0] = buf[1] = 0;
        *actual = wl < 2 ? wl : 2;
        return 0;
    }
    if (rt == 0x80 && req == 0x08) {            /* GET_CONFIGURATION */
        buf[0] = sl->config;
        *actual = wl < 1 ? wl : 1;
        return 0;
    }
    if (rt == 0xA1 && req == 0xFE) {            /* GET_MAX_LUN */
        buf[0] = 0;
        *actual = wl < 1 ? wl : 1;
        return 0;
    }
    if (req == 0x01 || req == 0x03)            /* CLEAR/SET_FEATURE */
        if ((rt & 0x60) == 0) return 0;
    if (rt == 0x01 && req == 0x0B) return 0;    /* SET_INTERFACE */
    if (rt == 0x21 && req == 0xFF) return 0;    /* BOT reset */
    return -1;
}

/* fetch the next owned TRB on an endpoint ring (Link handled inside) */
static int ep_next(xhci_t *x, xhci_ep_t *ep, uint32_t trb[4], uint64_t *addr) {
    for (int guard = 0; guard < 4; guard++) {
        for (int i = 0; i < 4; i++) trb[i] = g_rd32(x, ep->deq + 4 * i);
        if ((trb[3] & 1u) != (uint32_t)ep->ccs) return 0;
        *addr = ep->deq;
        ep->deq += 16;
        if (((trb[3] >> 10) & 0x3Fu) == 6) {    /* Link */
            ep->deq = (((uint64_t)trb[1] << 32) | trb[0]) & ~0xFull;
            if (trb[3] & 2u) ep->ccs ^= 1;
            continue;
        }
        return 1;
    }
    return 0;
}

static void tr_event(xhci_t *x, uint32_t slot, uint32_t epid, uint64_t trb_addr,
                     uint32_t cc, uint32_t residue) {
    ev_post(x, trb_addr, (cc << 24) | (residue & 0xFFFFFFu),
            (32u << 10) | (epid << 16) | (slot << 24));
}

static void ep0_process(xhci_t *x, uint32_t slot) {
    xhci_slot_t *sl = &x->slots[slot - 1];
    xhci_ep_t *ep = &sl->ep[0];
    uint64_t out = devctx_base(x, slot);
    for (int budget = 0; budget < 64 && ep->state == 1; budget++) {
        uint32_t su[4], trb[4];
        uint64_t a_setup, a_data = 0, a_status;
        if (!ep_next(x, ep, su, &a_setup)) break;
        if (((su[3] >> 10) & 0x3Fu) != 2) {     /* not a Setup Stage */
            tr_event(x, slot, 1, a_setup, CC_TRB_ERROR, 0);
            continue;
        }
        uint8_t setup[8];
        if (su[3] & (1u << 6)) {                /* IDT: setup in the TRB */
            for (int i = 0; i < 4; i++) setup[i] = (uint8_t)(su[0] >> (8 * i));
            for (int i = 0; i < 4; i++) setup[4 + i] = (uint8_t)(su[1] >> (8 * i));
        } else {
            uint64_t p = (uint64_t)su[0] | ((uint64_t)su[1] << 32);
            for (int i = 0; i < 8; i++) setup[i] = (uint8_t)mem_read(x->m, p + i, 1);
        }
        uint32_t trt = (su[3] >> 16) & 3u;
        uint32_t data_len = 0;
        uint64_t data_buf = 0;
        if (trt) {
            if (!ep_next(x, ep, trb, &a_data)) break;   /* half a TD: leave */
            if (((trb[3] >> 10) & 0x3Fu) != 3) {
                tr_event(x, slot, 1, a_data, CC_TRB_ERROR, 0);
                continue;
            }
            data_buf = (uint64_t)trb[0] | ((uint64_t)trb[1] << 32);
            data_len = trb[2] & 0x1FFFFu;
        }
        if (!ep_next(x, ep, trb, &a_status)) break;     /* half a TD: leave */
        if (((trb[3] >> 10) & 0x3Fu) != 4) {
            tr_event(x, slot, 1, a_status, CC_TRB_ERROR, 0);
            continue;
        }
        uint8_t buf[256];
        uint32_t actual = 0;
        if (dev_control(sl, setup, buf, &actual) != 0) {
            ep->state = 2;                              /* Halted */
            if (out) {
                uint64_t o = out + CTX;
                g_wr32(x, o, (g_rd32(x, o) & ~7u) | 2u);
            }
            tr_event(x, slot, 1, trt ? a_data : a_setup, CC_STALL, data_len);
            break;
        }
        uint32_t cc = CC_SUCCESS, residue = 0;
        if (trt == 3) {                                 /* IN data stage */
            uint32_t n = actual < data_len ? actual : data_len;
            for (uint32_t i = 0; i < n; i++)
                mem_write(x->m, data_buf + i, 1, buf[i]);
            residue = data_len - n;
            if (actual < data_len) cc = CC_SHORT_PACKET;
        }
        tr_event(x, slot, 1, a_status, cc, residue);
    }
    if (out) {                                          /* dequeue mirror */
        uint64_t o = out + CTX;
        g_wr32(x, o + 8, (uint32_t)ep->deq | (uint32_t)ep->ccs);
        g_wr32(x, o + 12, (uint32_t)(ep->deq >> 32));
    }
}

/* ==================== X4: bulk transfers =============================
 * A Normal-TRB chain (CH on all but the last, IOC on the last, ISP set)
 * runs against the shared MSC stick (usb_stick_serve, ctl 2) and posts
 * one Transfer Event per chain, as the guest's bulk path expects
 * (xhci.c:1636-1727).  Direction comes from the endpoint context type
 * (2 Bulk OUT / 6 Bulk IN) -- Normal TRBs carry no direction bit.  An
 * IN shortfall ends the chain early with cc 13 and the byte residue,
 * the only form the guest trusts (it discards residues off cc 13).
 * Interrupt endpoints have no device behind them on this machine
 * (there is no HID model on any bus), so their armed TRB simply stays
 * armed -- NAK, no event -- which is exactly the guest's non-blocking
 * poll contract (xhci.c:1731-1868): an idle HID poll returns 0. */

static void ep_out_sync(xhci_t *x, uint64_t out, uint32_t epid, const xhci_ep_t *ep) {
    if (!out) return;
    uint64_t o = out + (uint64_t)epid * CTX;
    g_wr32(x, o + 8, (uint32_t)ep->deq | (uint32_t)ep->ccs);
    g_wr32(x, o + 12, (uint32_t)(ep->deq >> 32));
}

static void bulk_process(xhci_t *x, uint32_t slot, uint32_t epid) {
    xhci_ep_t *ep = &x->slots[slot - 1].ep[epid - 1];
    if (ep->state != 1) return;
    if (ep->type != 2 && ep->type != 6) return;  /* intr: armed forever; isoch: X5+ */
    int pid_in = (ep->type == 6);
    uint64_t out = devctx_base(x, slot);
    for (int budget = 0; budget < 64 && ep->state == 1; budget++) {
        uint32_t trb[4];
        uint64_t last = 0;
        if (!ep_next(x, ep, trb, &last)) break;
        if (((trb[3] >> 10) & 0x3Fu) != 1) {    /* not a Normal TRB */
            tr_event(x, slot, epid, last, CC_TRB_ERROR, 0);
            continue;
        }
        uint32_t len_sum = 0, moved_sum = 0;
        int short_hit = 0;
        for (;;) {
            uint32_t len = trb[2] & 0x1FFFFu;
            uint64_t buf = (uint64_t)trb[0] | ((uint64_t)trb[1] << 32);
            len_sum += len;
            if (!short_hit && len) {
                uint32_t mv = usb_stick_serve(x->m, 2, pid_in ? 1 : 0, len, (uint32_t)buf);
                if (pid_in) {
                    moved_sum += mv;
                    if (mv < len) short_hit = 1;
                } else {
                    moved_sum += len;           /* OUT: device took the bytes */
                }
            } else if (!pid_in) {
                moved_sum += len;
            }
            if (!(trb[3] & (1u << 4))) break;   /* !CH: chain end */
            if (!ep_next(x, ep, trb, &last)) break;     /* half-queued chain */
            if (((trb[3] >> 10) & 0x3Fu) != 1) { short_hit = 1; break; }
        }
        uint32_t cc = (pid_in && moved_sum < len_sum) ? CC_SHORT_PACKET : CC_SUCCESS;
        tr_event(x, slot, epid, last, cc, len_sum - moved_sum);
    }
    ep_out_sync(x, out, epid, ep);
}

static uint32_t xhci_read32(xhci_t *x, uint32_t off) {
    if (off < X_CAP_LEN) {
        switch (off) {
        case 0x00: return (X_HCI_VERSION << 16) | X_CAP_LEN;
        case 0x04: return X_HCSPARAMS1;
        case 0x10: return X_HCCPARAMS1;
        case 0x14: return X_DBOFF;
        case 0x18: return X_RTSOFF;
        default:   return 0;   /* HCSPARAMS2/3, HCCPARAMS2 */
        }
    }
    if (off >= X_RTSOFF && off < X_DBOFF) return rt_read(x, off - X_RTSOFF);
    if (off >= X_DBOFF) return 0;               /* doorbells read 0 */
    return op_read(x, off - X_CAP_LEN);
}

static void xhci_write32(xhci_t *x, uint32_t off, uint32_t val) {
    (void)val;
    if (off < X_CAP_LEN) return;                /* capability space is RO */
    if (off >= X_RTSOFF && off < X_DBOFF) { rt_write(x, off - X_RTSOFF, val); return; }
    if (off >= X_DBOFF) {                       /* doorbells */
        if (off == X_DBOFF) { cmd_process(x); return; }
        uint32_t slot = (off - X_DBOFF) / 4;
        uint32_t epid = val & 0xFFu;            /* guest: db_wr(slot, ep) */
        if ((x->usbcmd & CMD_RUN) && slot >= 1 && slot <= XHCI_MAX_SLOTS
            && x->slots[slot - 1].addressed) {
            if (epid == 1) ep0_process(x, slot);
            else if (epid >= 2 && epid < 32) bulk_process(x, slot, epid);
        }
        return;
    }
    op_write(x, off - X_CAP_LEN, val);
}

static uint64_t xhci_mmio_read(void *ctx, uint64_t addr, int size) {
    xhci_t *x = ctx;
    uint32_t off = (uint32_t)(addr - x->base);
    if (size == 8) {
        return (uint64_t)xhci_read32(x, off & ~3u) |
               ((uint64_t)xhci_read32(x, (off & ~3u) + 4) << 32);
    }
    if (size != 4) {   /* guest is 32-bit only; compose like AHCI */
        uint32_t dw = xhci_read32(x, off & ~3u);
        uint32_t in = off & 3u;
        if ((uint32_t)size + in <= 4)
            return (dw >> (8 * in)) & ((size >= 4) ? 0xFFFFFFFFu : ((1u << (8 * size)) - 1u));
        return 0;
    }
    return xhci_read32(x, off);
}

static void xhci_mmio_write(void *ctx, uint64_t addr, int size, uint64_t val) {
    xhci_t *x = ctx;
    uint32_t off = (uint32_t)(addr - x->base);
    if (size == 8) {
        xhci_write32(x, off & ~3u, (uint32_t)val);
        xhci_write32(x, (off & ~3u) + 4, (uint32_t)(val >> 32));
        return;
    }
    if (size != 4) {
        uint32_t old = xhci_read32(x, off & ~3u);
        uint32_t in = off & 3u, mask = 0;
        for (int i = 0; i < size; i++) mask |= 0xFFu << (8 * (in + i));
        val = (old & ~mask) | ((val << (8 * in)) & mask);
        off &= ~3u;
    }
    xhci_write32(x, off, (uint32_t)val);
}

/* ---- BAR0 (64-bit, 64 KiB) probe + relocation, the AHCI BAR5 pattern -- */
static void xhci_set_base(xhci_t *x, uint64_t base) {
    int enabled = base != 0 && (base >> 32) == 0;
    x->base = enabled ? (uint32_t)base : 0;
    if (!x->m) return;
    for (mmio_region_t *r = x->m->mmio_list; r; r = r->next) {
        if (r->ctx == x && r->size == XHCI_MMIO_SIZE) {
            r->base = x->base;
            r->enabled = enabled;
            return;
        }
    }
}

static xhci_t *xhci_pci_controller(pci_dev_t *d) {
    if (!d || d->bus != 0 || d->dev != 20 || d->func != 0) return NULL;
    return &g_xhci;
}

static uint64_t xhci_bar_value(xhci_t *x, pci_dev_t *d) {
    if (x->bar_probe) return 0xFFFF0004ull;    /* mask | 64-bit type, hi=0 */
    uint64_t v = 0;
    for (int i = 0; i < 8; i++) v |= (uint64_t)d->cfg[0x10 + i] << (8 * i);
    return v;
}

uint64_t xhci_pci_cfg_read(struct pci_dev *dev, int reg, int size) {
    pci_dev_t *d = dev;
    xhci_t *x = xhci_pci_controller(d);
    if (!x || size <= 0 || size > 8) return 0;
    uint64_t bar = xhci_bar_value(x, d);
    uint64_t value = 0;
    for (int i = 0; i < size; i++) {
        int off = reg + i;
        uint8_t byte = 0;
        if (off >= 0x10 && off < 0x18)
            byte = (uint8_t)(bar >> (8 * (off - 0x10)));
        else if (!((off >= 0x18 && off < 0x28) || (off >= 0x30 && off < 0x34))
                 && off >= 0 && off < 256)
            byte = d->cfg[off];
        value |= (uint64_t)byte << (8 * i);
    }
    return value;
}

void xhci_pci_cfg_write(struct pci_dev *dev, int reg, int size, uint64_t val) {
    pci_dev_t *d = dev;
    xhci_t *x = xhci_pci_controller(d);
    if (!x || size <= 0 || size > 8) return;
    if ((reg == 0x10 || reg == 0x14) && size == 4 && (uint32_t)val == 0xFFFFFFFFu) {
        x->bar_probe = 1;
        return;
    }
    int touches_bar = reg < 0x18 && reg + size > 0x10;
    if (touches_bar) x->bar_probe = 0;
    pci_default_cfg_write(d, reg, size, val);
    /* BAR2..5 and the expansion-ROM BAR are not implemented. */
    memset(d->cfg + 0x18, 0, 0x10);
    memset(d->cfg + 0x30, 0, 4);
    if (touches_bar) {
        uint64_t req = 0;
        for (int i = 0; i < 8; i++) req |= (uint64_t)d->cfg[0x10 + i] << (8 * i);
        uint64_t base = req & ~(uint64_t)(XHCI_MMIO_SIZE - 1u) & ~0xFull;
        uint64_t stored = base | 0x4u;         /* 64-bit memory BAR */
        for (int i = 0; i < 8; i++) d->cfg[0x10 + i] = (uint8_t)(stored >> (8 * i));
        xhci_set_base(x, base);
    }
}

void xhci_register(machine_t *m) {
    xhci_t *x = &g_xhci;
    memset(x, 0, sizeof *x);
    x->m = m;
    x->base = XHCI_MMIO_BASE;
    mem_register_mmio(m, x->base, XHCI_MMIO_SIZE, xhci_mmio_read,
                      xhci_mmio_write, x, "XHCI BAR0");
    mlog(&m->log, "[xhci] BAR0 registered at 0x%x (%d ports, X1: register file)\n",
         x->base, XHCI_MAX_PORTS);
}

void xhci_snap(machine_t *m, snap_t *s) {
    (void)m;
    xhci_t t = g_xhci;
    if (!snap_loading(s)) t.m = NULL;
    snap_bytes(s, "dev.xhci", &t, sizeof t);
    if (snap_loading(s)) {
        t.m = g_xhci.m;
        g_xhci = t;
        xhci_set_base(&g_xhci, g_xhci.base);
    }
}
