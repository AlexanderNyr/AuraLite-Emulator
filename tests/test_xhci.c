/* tests/test_xhci.c -- XHCI X1 vectors: PCI identity + register file.
 *
 * Everything pinned here is the measured guest contract (XHCI_PLAN.md
 * section 1a: AuraLite-OS drivers/usb/xhci.c) and the QEMU identity
 * (section 1b: qemu-xhci = 1b36:000d rev 01).  The fixture is the full
 * machine (devices_init_common), PCI config goes through the real
 * 0xCF8/0xCFC mechanism and MMIO through mem_read/mem_write, so the
 * vectors see exactly what the guest driver sees.
 */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "devices.h"
#include "harness.h"
#include "snapshot.h"     /* MSI M4: save/load round-trip */

static int fails;
#define EXPECT(cond, ...) do { if (!(cond)) { printf("FAIL " __VA_ARGS__); printf("\n"); fails++; } } while (0)

static machine_t M;   /* static: machine_t is ~2.8MB */

#define XHCI_BAR   0xFEB20000u
#define OP(off)    (XHCI_BAR + 0x40u + (off))          /* operational */
#define PORT(p)    (XHCI_BAR + 0x40u + 0x400u + (p) * 0x10u)
#define RT(off)    (XHCI_BAR + 0x800u + (off))         /* runtime */

static uint32_t rd32(uint32_t a) { return (uint32_t)mem_read(&M, a, 4); }
static void     wr32(uint32_t a, uint32_t v) { mem_write(&M, a, 4, v); }

/* PCI mechanism #1, the test_pam.c recipe */
static void pci_addr(int dev, int reg) {
    io_write(&M, 0xCF8, 4, 0x80000000u | ((uint32_t)dev << 11) | ((uint32_t)reg & 0xFCu));
}
static uint32_t pci_rd32(int dev, int reg) { pci_addr(dev, reg); return (uint32_t)io_read(&M, 0xCFC, 4); }
static void pci_wr32(int dev, int reg, uint32_t v) { pci_addr(dev, reg); io_write(&M, 0xCFC, 4, v); }
static uint8_t pci_rd8(int dev, int reg) { pci_addr(dev, reg); return (uint8_t)io_read(&M, 0xCFC + (reg & 3), 1); }

/* ---- v_pci_identity: the guest's class probe finds 1b36:000d ---------- */
static void v_pci_identity(void) {
    int f0 = fails;
    int found = -1;
    for (int d = 0; d < 32 && found < 0; d++) {
        if ((pci_rd32(d, 0) & 0xFFFFu) == 0xFFFFu) continue;
        if (pci_rd8(d, 0x0B) == 0x0C && pci_rd8(d, 0x0A) == 0x03 && pci_rd8(d, 0x09) == 0x30)
            found = d;
    }
    EXPECT(found == 20, "class 0C/03/30 scan finds dev 20 (got %d)", found);
    EXPECT(pci_rd32(20, 0x00) == 0x000D1B36u, "vendor:device 1b36:000d (got %08x)", pci_rd32(20, 0x00));
    EXPECT(pci_rd32(20, 0x08) == 0x0C033001u, "class/revision dword (got %08x)", pci_rd32(20, 0x08));
    EXPECT(pci_rd32(20, 0x2C) == 0x11001AF4u, "subsystem 1af4:1100 (got %08x)", pci_rd32(20, 0x2C));
    EXPECT(pci_rd8(20, 0x3C) == 10, "interrupt line 10 (got %u)", pci_rd8(20, 0x3C));
    EXPECT(pci_rd8(20, 0x3D) == 1, "interrupt pin INTA# (got %u)", pci_rd8(20, 0x3D));
    EXPECT(pci_rd32(20, 0x10) == (XHCI_BAR | 0x4u), "BAR0 seeded 64-bit (got %08x)", pci_rd32(20, 0x10));
    EXPECT(pci_rd32(20, 0x14) == 0, "BAR1 (upper dword) 0");
    EXPECT(pci_rd32(20, 0x18) == 0 && pci_rd32(20, 0x30) == 0, "unimplemented BARs read 0");
    if (fails == f0) printf("ok pci identity: 1b36:000d class 0C/03/30 at 0:20.0, IRQ 10 INTA#\n");
}

/* ---- v_bar_probe: sizing reads the 64 KiB mask, relocation rebinds ---- */
static void v_bar_probe(void) {
    int f0 = fails;
    pci_wr32(20, 0x10, 0xFFFFFFFFu);
    EXPECT(pci_rd32(20, 0x10) == 0xFFFF0004u, "BAR0 probe mask (got %08x)", pci_rd32(20, 0x10));
    pci_wr32(20, 0x14, 0xFFFFFFFFu);
    EXPECT(pci_rd32(20, 0x14) == 0, "BAR1 probe reads 0 (no bits above 32)");
    /* place the BAR back; the MMIO window must decode at the seeded base */
    pci_wr32(20, 0x10, XHCI_BAR | 0x4u);
    pci_wr32(20, 0x14, 0);
    EXPECT(pci_rd32(20, 0x10) == (XHCI_BAR | 0x4u), "BAR0 restored");
    EXPECT(rd32(XHCI_BAR) == 0x01000040u, "caps dword decodes at the BAR base");
    if (fails == f0) printf("ok bar probe: sizing mask + restore + decode\n");
}

/* ---- v_caps: the capability block the guest parses -------------------- */
static void v_caps(void) {
    int f0 = fails;
    EXPECT(rd32(XHCI_BAR + 0x00) == 0x01000040u, "CAPLENGTH=0x40 HCIVERSION=1.00");
    uint32_t h1 = rd32(XHCI_BAR + 0x04);
    EXPECT((h1 & 0xFFu) == 32, "HCSPARAMS1 MaxSlots=32");
    EXPECT(((h1 >> 8) & 0xFFu) == 1, "HCSPARAMS1 MaxIntrs=1");
    EXPECT((h1 >> 24) == 4, "HCSPARAMS1 MaxPorts=4");
    EXPECT(rd32(XHCI_BAR + 0x08) == 0, "HCSPARAMS2: no scratchpads");
    EXPECT(rd32(XHCI_BAR + 0x10) == 1, "HCCPARAMS1: AC64 only (CSZ=0, PPC=0)");
    EXPECT(rd32(XHCI_BAR + 0x14) == 0x1000u, "DBOFF");
    EXPECT(rd32(XHCI_BAR + 0x18) == 0x800u, "RTSOFF");
    /* sub-dword reads compose (test_usb.c EHCI-cap precedent) */
    EXPECT(mem_read(&M, XHCI_BAR, 1) == 0x40, "CAPLENGTH byte read");
    EXPECT(mem_read(&M, XHCI_BAR + 2, 2) == 0x0100, "HCIVERSION word read");
    /* capability space is read-only */
    wr32(XHCI_BAR + 0x00, 0xDEADBEEFu);
    EXPECT(rd32(XHCI_BAR + 0x00) == 0x01000040u, "caps ignore writes");
    if (fails == f0) printf("ok caps: version 1.00, 32 slots, 4 ports, AC64, RO\n");
}

/* ---- v_op_reset: the halt/reset/run handshake (guest init 1-3, 10) ---- */
static void v_op_reset(void) {
    int f0 = fails;
    EXPECT(rd32(OP(0x04)) & 1u, "power-on USBSTS.HCH set");
    wr32(OP(0x00), 0);                                   /* halt */
    EXPECT(rd32(OP(0x04)) & 1u, "HCH after USBCMD=0");
    wr32(OP(0x00), 4u | 1u);                             /* INTE|RUN */
    EXPECT(!(rd32(OP(0x04)) & 1u), "HCH clears with RUN");
    /* program state, then HCRST must wipe it */
    wr32(OP(0x38), 32);                                  /* CONFIG */
    wr32(OP(0x30), 0x00030000u);                         /* DCBAAP lo */
    wr32(OP(0x18), 0x00040001u);                         /* CRCR ptr|RCS */
    wr32(OP(0x00), 2u);                                  /* HCRST */
    EXPECT(rd32(OP(0x00)) == 0, "HCRST self-clears (USBCMD=0)");
    EXPECT(!(rd32(OP(0x04)) & (1u << 11)), "CNR clear after reset");
    EXPECT(rd32(OP(0x04)) & 1u, "HCH set after reset");
    EXPECT(rd32(OP(0x38)) == 0 && rd32(OP(0x30)) == 0 && rd32(OP(0x18)) == 0,
           "CONFIG/DCBAAP/CRCR reset to 0");
    if (fails == f0) printf("ok op reset: halt -> HCRST (self-clear, CNR=0) -> state wiped\n");
}

/* ---- v_programming: store registers read back exactly ----------------- */
static void v_programming(void) {
    int f0 = fails;
    wr32(OP(0x38), 32);
    EXPECT(rd32(OP(0x38)) == 32, "CONFIG MaxSlotsEn store");
    wr32(OP(0x30), 0x00030000u); wr32(OP(0x34), 0x2u);
    EXPECT(rd32(OP(0x30)) == 0x00030000u && rd32(OP(0x34)) == 2u, "DCBAAP 64-bit store");
    wr32(OP(0x14), 0xFFFFu);
    EXPECT(rd32(OP(0x14)) == 0xFFFFu, "DNCTRL store");
    EXPECT(rd32(OP(0x08)) == 1, "PAGESIZE reads 1 (4 KiB)");
    wr32(OP(0x08), 0xFFFFu);
    EXPECT(rd32(OP(0x08)) == 1, "PAGESIZE is RO");
    wr32(OP(0x18), 0x00040001u);
    EXPECT(rd32(OP(0x18)) == 0x00040001u, "CRCR ptr|RCS store, CRR=0 while stopped");
    wr32(OP(0x00), 4u | 1u);                             /* INTE|RUN */
    EXPECT(rd32(OP(0x18)) == (0x00040001u | (1u << 3)), "CRR set once running");
    wr32(OP(0x00), 0);
    if (fails == f0) printf("ok programming: CONFIG/DCBAAP/CRCR/DNCTRL/PAGESIZE semantics\n");
}

/* ---- v_ports: empty ports, PPC=0 power, wake bits, no fake changes ---- */
static void v_ports(void) {
    int f0 = fails;
    for (int p = 0; p < 4; p++) {
        uint32_t ps = rd32(PORT(p));
        EXPECT(ps == (1u << 9), "port %d empty: PP=1 only (got %08x)", p, ps);
        EXPECT(rd32(PORT(p) + 4) == 0 && rd32(PORT(p) + 8) == 0, "port %d PMSC/LI read 0", p);
    }
    /* the guest's bring-up writes: PP alone, then PP|WCE|WDE */
    wr32(PORT(0), 1u << 9);
    EXPECT(rd32(PORT(0)) == (1u << 9), "PP-only write is stable");
    wr32(PORT(0), (1u << 9) | (1u << 25) | (1u << 26));
    EXPECT(rd32(PORT(0)) == ((1u << 9) | (1u << 25) | (1u << 26)), "wake bits store");
    /* an all-ones write must not fabricate change/CCS/PED bits */
    wr32(PORT(1), 0xFFFFFFFFu);
    uint32_t ps = rd32(PORT(1));
    EXPECT(!(ps & 0x3u), "all-ones write: CCS/PED stay 0 (got %08x)", ps);
    EXPECT(!(ps & (0x7Fu << 17)), "all-ones write: no change bits fabricated");
    EXPECT((ps & (7u << 25)) == (7u << 25), "all-ones write: wake bits taken");
    if (fails == f0) printf("ok ports: PP=1 (PPC=0), wake store, writes fabricate nothing\n");
}

/* ---- v_interrupter: runtime registers store, IP quiet in X1 ----------- */
static void v_interrupter(void) {
    int f0 = fails;
    wr32(RT(0x28), 1);
    EXPECT(rd32(RT(0x28)) == 1, "ERSTSZ store");
    wr32(RT(0x30), 0x00050000u); wr32(RT(0x34), 0);
    EXPECT(rd32(RT(0x30)) == 0x00050000u, "ERSTBA store");
    wr32(RT(0x38), 0x00060000u); wr32(RT(0x3C), 0);
    EXPECT(rd32(RT(0x38)) == 0x00060000u, "ERDP store");
    wr32(RT(0x24), 0);
    EXPECT(rd32(RT(0x24)) == 0, "IMOD store");
    wr32(RT(0x20), 2u);                                  /* IMAN.IE */
    EXPECT(rd32(RT(0x20)) == 2u, "IMAN: IE=1, IP=0");
    EXPECT(rd32(RT(0x40)) == 0, "interrupter 1 absent (MaxIntrs=1)");
    if (fails == f0) printf("ok interrupter: ERST/ERDP/IMAN/IMOD store semantics\n");
}

/* ---- v_doorbell: X1 accepts and ignores (X2 rings the engine) --------- */
static void v_doorbell(void) {
    int f0 = fails;
    wr32(XHCI_BAR + 0x1000u, 0);                         /* DB0: command ring */
    wr32(XHCI_BAR + 0x1004u, 1u);                        /* DB1: slot 1 EP0 */
    EXPECT(rd32(XHCI_BAR + 0x1000u) == 0, "doorbells read 0");
    if (fails == f0) printf("ok doorbell: writes accepted, reads 0 (engine is X2)\n");
}

/* ---- v_guest_init: the guest's 11-step bring-up, in miniature ----------
 * Replays xhci_init's register sequence (XHCI_PLAN 1a steps 1-11) with
 * dummy DMA addresses; every wait the guest spins on must complete
 * immediately, and the port scan must see 4 empty ports. */
static void v_guest_init(void) {
    int f0 = fails;
    wr32(OP(0x00), 2u);                                  /* fresh HCRST */
    int t = 1000000;
    wr32(OP(0x00), 0);
    while (!(rd32(OP(0x04)) & 1u) && t-- > 0) { }
    EXPECT(t > 0, "step 1: halt waits HCH");
    wr32(OP(0x00), 2u);
    t = 1000000;
    while ((rd32(OP(0x00)) & 2u) && t-- > 0) { }
    EXPECT(t > 0, "step 2: HCRST clears");
    t = 1000000;
    while ((rd32(OP(0x04)) & (1u << 11)) && t-- > 0) { }
    EXPECT(t > 0, "step 3: CNR clears");
    wr32(OP(0x38), 32);                                  /* step 4: CONFIG */
    wr32(OP(0x30), 0x00030000u); wr32(OP(0x34), 0);      /* steps 5-6: DCBAAP */
    wr32(OP(0x18), 0x00040000u | 1u); wr32(OP(0x1C), 0); /* step 7: CRCR|RCS */
    wr32(RT(0x28), 1);                                   /* step 8: interrupter 0 */
    wr32(RT(0x30), 0x00050000u); wr32(RT(0x34), 0);
    wr32(RT(0x38), 0x00060000u); wr32(RT(0x3C), 0);
    wr32(RT(0x20), 2u); wr32(RT(0x24), 0);
    for (int p = 0; p < 4; p++) {                        /* step 9: port power */
        uint32_t ps = rd32(PORT(p));
        if (!(ps & (1u << 9))) wr32(PORT(p), 1u << 9);
    }
    wr32(OP(0x00), 4u | 1u);                             /* step 10: INTE|RUN */
    for (int p = 0; p < 4; p++) {
        uint32_t ps = rd32(PORT(p));
        wr32(PORT(p), (ps & (1u << 9)) | (1u << 25) | (1u << 26));
    }
    t = 1000000;
    while ((rd32(OP(0x04)) & 1u) && t-- > 0) { }
    EXPECT(t > 0, "step 10: controller running (HCH clear)");
    int ready = 0;                                       /* step 11: port scan */
    for (int p = 0; p < 4; p++) if (rd32(PORT(p)) & 1u) ready++;
    EXPECT(ready == 0, "step 11: no devices attached yet (X4 binds the sticks)");
    if (fails == f0) printf("ok guest init replay: steps 1-11 complete, 0 devices (X1 state)\n");
}

/* ================= X2: command ring engine =============================
 * The fixture programs the controller exactly as xhci_init does (XHCI_PLAN
 * section 1a) and submits commands with the guest's own discipline: TRB
 * written with the current cycle, Link TRB rewritten at the wrap, event
 * consumed + ERDP|EHB written back after each command. */
#define DCBAA_ADDR 0x00030000u
#define CMDRING    0x00040000u
#define EVTRING    0x00041000u
#define ERST_ADDR  0x00042000u
#define DB0        (XHCI_BAR + 0x1000u)

static void ram_wr32(uint32_t a, uint32_t v) { mem_write(&M, a, 4, v); }
static uint32_t ram_rd32(uint32_t a) { return (uint32_t)mem_read(&M, a, 4); }

static int t_cmd_idx, t_cmd_cycle, t_ev_idx, t_ev_cycle;

static void hc_program(int maxslots) {
    for (uint32_t a = DCBAA_ADDR; a < DCBAA_ADDR + 0x1000; a += 4) ram_wr32(a, 0);
    for (uint32_t a = CMDRING; a < CMDRING + 0x1000; a += 4) ram_wr32(a, 0);
    for (uint32_t a = EVTRING; a < EVTRING + 0x1000; a += 4) ram_wr32(a, 0);
    for (uint32_t a = ERST_ADDR; a < ERST_ADDR + 0x100; a += 4) ram_wr32(a, 0);
    ram_wr32(CMDRING + 255 * 16 + 0, CMDRING);                       /* Link TRB */
    ram_wr32(CMDRING + 255 * 16 + 12, (6u << 10) | (1u << 1) | 1u);  /* type|TC|cycle */
    ram_wr32(ERST_ADDR + 0, EVTRING);
    ram_wr32(ERST_ADDR + 8, 256);
    wr32(OP(0x38), (uint32_t)maxslots);                              /* CONFIG */
    wr32(OP(0x30), DCBAA_ADDR); wr32(OP(0x34), 0);                   /* DCBAAP */
    wr32(OP(0x18), CMDRING | 1u); wr32(OP(0x1C), 0);                 /* CRCR|RCS */
    wr32(RT(0x28), 1);                                               /* ERSTSZ */
    wr32(RT(0x30), ERST_ADDR); wr32(RT(0x34), 0);                    /* ERSTBA */
    wr32(RT(0x38), EVTRING); wr32(RT(0x3C), 0);                      /* ERDP */
    wr32(RT(0x20), 2u); wr32(RT(0x24), 0);                           /* IMAN.IE, IMOD */
    wr32(OP(0x00), 4u | 1u);                                         /* INTE|RUN */
    t_cmd_idx = 0; t_cmd_cycle = 1; t_ev_idx = 0; t_ev_cycle = 1;
}
static void hc_reset_program(int maxslots) {
    wr32(OP(0x00), 2u);                                              /* HCRST */
    hc_program(maxslots);
}

/* one guest-shaped submit + consume; ev[] gets the raw event TRB */
static uint32_t t_last_cmd_addr;
/* consume one event exactly like the guest's ring reader does */
static void next_event(uint32_t ev[4]) {
    uint32_t eb = EVTRING + (uint32_t)t_ev_idx * 16;
    for (int i = 0; i < 4; i++) ev[i] = ram_rd32(eb + 4 * (uint32_t)i);
    t_ev_idx++;
    if (t_ev_idx >= 256) { t_ev_idx = 0; t_ev_cycle ^= 1; }
    wr32(RT(0x38), (EVTRING + (uint32_t)t_ev_idx * 16) | 0x8u);      /* ERDP|EHB */
    wr32(RT(0x3C), 0);
}
static void cmd_submit_p(uint32_t flags, uint32_t p0, uint32_t p1, uint32_t ev[4]) {
    uint32_t base = CMDRING + (uint32_t)t_cmd_idx * 16;
    t_last_cmd_addr = base;
    ram_wr32(base + 0, p0); ram_wr32(base + 4, p1); ram_wr32(base + 8, 0);
    ram_wr32(base + 12, flags | (uint32_t)t_cmd_cycle);
    t_cmd_idx++;
    if (t_cmd_idx >= 255) {
        ram_wr32(CMDRING + 255 * 16 + 0, CMDRING);
        ram_wr32(CMDRING + 255 * 16 + 12, (6u << 10) | (1u << 1) | (uint32_t)t_cmd_cycle);
        t_cmd_idx = 0;
        t_cmd_cycle ^= 1;
    }
    wr32(DB0, 0);
    next_event(ev);
}
static void cmd_submit(uint32_t flags, uint32_t ev[4]) { cmd_submit_p(flags, 0, 0, ev); }
#define EV_CC(ev)    (((ev)[2] >> 24) & 0xFFu)
#define EV_TYPE(ev)  (((ev)[3] >> 10) & 0x3Fu)
#define EV_SLOT(ev)  (((ev)[3] >> 24) & 0xFFu)
#define EV_CYCLE(ev) ((ev)[3] & 1u)
#define TRB_CMD(type) ((uint32_t)(type) << 10)

/* ---- v_cmd_noop: round-trip + IP/EINT latch ---------------------------- */
static void v_cmd_noop(void) {
    int f0 = fails;
    hc_reset_program(32);
    uint32_t ev[4];
    cmd_submit(TRB_CMD(23) | (1u << 5) /* IOC */, ev);
    EXPECT(EV_TYPE(ev) == 33, "completion event type 33 (got %u)", EV_TYPE(ev));
    EXPECT(EV_CC(ev) == 1, "No Op cc=1 (got %u)", EV_CC(ev));
    EXPECT(ev[0] == CMDRING && ev[1] == 0, "event param = command TRB addr");
    EXPECT(EV_CYCLE(ev) == 1, "event cycle = producer cycle 1");
    EXPECT(EV_SLOT(ev) == 0, "No Op completion slot 0");
    /* submit one more without consuming through the helper to see the latch */
    ram_wr32(CMDRING + 1 * 16 + 12, TRB_CMD(23) | 1u);
    wr32(DB0, 0);
    EXPECT(rd32(RT(0x20)) & 1u, "IMAN.IP set after an unconsumed event");
    EXPECT(rd32(OP(0x04)) & (1u << 3), "USBSTS.EINT mirrors IP");
    EXPECT(rd32(RT(0x38)) & 0x8u, "ERDP.EHB reads set while pending");
    wr32(RT(0x38), (EVTRING + 2 * 16) | 0x8u); wr32(RT(0x3C), 0);    /* consume it */
    EXPECT(!(rd32(RT(0x20)) & 1u), "ERDP|EHB write clears IP");
    EXPECT(!(rd32(OP(0x04)) & (1u << 3)), "EINT clears with IP");
    if (fails == f0) printf("ok cmd noop: cc=1 round-trip, param echo, IP/EINT latch + EHB clear\n");
}

/* ---- v_cmd_wrap: 256 No Ops across a ring wrap (the guest U1 gate) ----- */
static void v_cmd_wrap(void) {
    int f0 = fails;
    hc_reset_program(32);
    int ok = 0;
    for (int i = 0; i < 256; i++) {
        uint32_t ev[4];
        int want_cycle = t_ev_cycle;
        cmd_submit(TRB_CMD(23) | (1u << 5), ev);
        if (EV_TYPE(ev) != 33 || EV_CC(ev) != 1) break;
        if (ev[0] != t_last_cmd_addr) break;
        if ((int)EV_CYCLE(ev) != want_cycle) break;
        ok++;
    }
    EXPECT(ok == 256, "256/256 No Ops across a ring wrap (got %d)", ok);
    /* and the ring still works after the wrap */
    uint32_t ev[4];
    cmd_submit(TRB_CMD(23), ev);
    EXPECT(EV_CC(ev) == 1 && ev[0] == t_last_cmd_addr, "ring alive after wrap");
    if (fails == f0) printf("ok cmd wrap: 256/256 No Ops, params+cycles exact\n");
}

/* ---- v_enable_slot: allocate / free / reuse / refuse ------------------- */
static void v_enable_slot(void) {
    int f0 = fails;
    hc_reset_program(32);
    uint32_t ev[4];
    cmd_submit(TRB_CMD(9), ev);
    EXPECT(EV_CC(ev) == 1 && EV_SLOT(ev) == 1, "Enable Slot -> 1 (cc=%u slot=%u)", EV_CC(ev), EV_SLOT(ev));
    cmd_submit(TRB_CMD(9), ev);
    EXPECT(EV_SLOT(ev) == 2, "Enable Slot -> 2");
    cmd_submit(TRB_CMD(9), ev);
    EXPECT(EV_SLOT(ev) == 3, "Enable Slot -> 3");
    cmd_submit(TRB_CMD(10) | (2u << 24), ev);
    EXPECT(EV_CC(ev) == 1 && EV_SLOT(ev) == 2, "Disable Slot 2 ok");
    cmd_submit(TRB_CMD(9), ev);
    EXPECT(EV_SLOT(ev) == 2, "Enable Slot reuses first free (2)");
    cmd_submit(TRB_CMD(10) | (2u << 24), ev);
    EXPECT(EV_CC(ev) == 1, "Disable Slot 2 again ok");
    cmd_submit(TRB_CMD(10) | (2u << 24), ev);
    EXPECT(EV_CC(ev) == 11, "Disable of disabled slot -> cc 11 Slot Not Enabled (got %u)", EV_CC(ev));
    cmd_submit(TRB_CMD(10) | (0u << 24), ev);
    EXPECT(EV_CC(ev) == 5, "Disable slot 0 -> cc 5 TRB Error (got %u)", EV_CC(ev));
    cmd_submit(TRB_CMD(10) | (33u << 24), ev);
    EXPECT(EV_CC(ev) == 5, "Disable slot 33 -> cc 5 (got %u)", EV_CC(ev));
    /* CONFIG.MaxSlotsEn=1 gates allocation (No Slots Available, cc 9) */
    hc_reset_program(1);
    cmd_submit(TRB_CMD(9), ev);
    EXPECT(EV_CC(ev) == 1 && EV_SLOT(ev) == 1, "MaxSlotsEn=1: first Enable ok");
    cmd_submit(TRB_CMD(9), ev);
    EXPECT(EV_CC(ev) == 9, "MaxSlotsEn=1: second Enable -> cc 9 (got %u)", EV_CC(ev));
    if (fails == f0) printf("ok slots: alloc/free/reuse, cc 11/5 refusals, CONFIG gate cc 9\n");
}

/* ---- v_cmd_unknown + stopped: honest refusals, ring survives ----------- */
static void v_cmd_unknown(void) {
    int f0 = fails;
    hc_reset_program(32);
    uint32_t ev[4];
    cmd_submit(TRB_CMD(11) | (1u << 24), ev);        /* Address, slot not enabled */
    EXPECT(EV_CC(ev) == 11, "Address on unenabled slot -> cc 11 (got %u)", EV_CC(ev));
    cmd_submit(TRB_CMD(23), ev);
    EXPECT(EV_CC(ev) == 1, "ring keeps working after a refused command");
    /* a TRB the consumer does not own yet (cycle flipped) is left alone */
    uint32_t base = CMDRING + (uint32_t)t_cmd_idx * 16;
    ram_wr32(base + 12, TRB_CMD(23) | (uint32_t)(t_cmd_cycle ^ 1));
    uint32_t eb = EVTRING + (uint32_t)t_ev_idx * 16;
    wr32(DB0, 0);
    EXPECT((ram_rd32(eb + 12) & 1u) != (uint32_t)t_ev_cycle, "unowned TRB not consumed");
    ram_wr32(base + 12, TRB_CMD(23) | (uint32_t)t_cmd_cycle);       /* hand it over */
    wr32(DB0, 0);
    EXPECT((ram_rd32(eb + 12) & 1u) == (uint32_t)t_ev_cycle, "owned TRB consumed on next doorbell");
    /* doorbell while stopped: nothing moves */
    hc_reset_program(32);
    ram_wr32(CMDRING + 12, TRB_CMD(23) | 1u);
    wr32(OP(0x00), 0);                                               /* stop */
    wr32(DB0, 0);
    EXPECT((ram_rd32(EVTRING + 12) & 1u) == 0, "no event while RUN=0");
    wr32(OP(0x00), 4u | 1u);                                         /* run again */
    wr32(DB0, 0);
    EXPECT((ram_rd32(EVTRING + 12) & 1u) == 1, "event lands once running");
    EXPECT(((ram_rd32(EVTRING + 8) >> 24) & 0xFFu) == 1, "and it is the queued No Op");
    if (fails == f0) printf("ok cmd edges: unenabled-slot refusal cc11, cycle ownership, stopped doorbell\n");
}

/* ================= X3: contexts + EP0 control ==========================
 * The fixture replays the guest's xhci_address_device() (input context
 * shapes copied field-for-field) and drives control transfers with the
 * guest's Setup/Data/Status TRB shapes. */
#define INCTX   0x00043000u
#define DEVCTX  0x00044000u
#define EP0RING 0x00045000u
#define EPXRING 0x00046000u
#define DATABUF 0x00047000u
#define DBS(s)  (DB0 + 4u * (uint32_t)(s))
#define EV_RES(ev) ((ev)[2] & 0xFFFFFFu)
#define EV_EP(ev)  (((ev)[3] >> 16) & 0x1Fu)

static int t_ep_idx, t_ep_cycle;
static uint32_t rd8(uint32_t a) { return (uint32_t)mem_read(&M, a, 1); }
static void zero_range(uint32_t base, uint32_t len) {
    for (uint32_t a = base; a < base + len; a += 4) ram_wr32(a, 0);
}

static void ep_enqueue(uint32_t d0, uint32_t d1, uint32_t d2, uint32_t d3) {
    uint32_t b = EP0RING + (uint32_t)t_ep_idx * 16;
    ram_wr32(b + 0, d0); ram_wr32(b + 4, d1); ram_wr32(b + 8, d2);
    ram_wr32(b + 12, d3 | (uint32_t)t_ep_cycle);
    t_ep_idx++;
    if (t_ep_idx >= 255) {
        ram_wr32(EP0RING + 255 * 16 + 0, EP0RING);
        ram_wr32(EP0RING + 255 * 16 + 12, (6u << 10) | (1u << 1) | (uint32_t)t_ep_cycle);
        t_ep_idx = 0;
        t_ep_cycle ^= 1;
    }
}

static void build_address_input(uint32_t mps0) {
    zero_range(INCTX, 0x400); zero_range(DEVCTX, 0x400); zero_range(EP0RING, 0x1000);
    ram_wr32(EP0RING + 255 * 16 + 0, EP0RING);
    ram_wr32(EP0RING + 255 * 16 + 12, (6u << 10) | (1u << 1) | 1u);
    ram_wr32(INCTX + 4, 0x3);                                   /* Add A0|A1 */
    ram_wr32(INCTX + 32 + 0, (3u << 20) | (1u << 27));          /* speed HS, 1 entry */
    ram_wr32(INCTX + 32 + 4, (1u << 16));                       /* root port 1 */
    ram_wr32(INCTX + 64 + 4, (3u << 1) | (4u << 3) | (mps0 << 16));
    ram_wr32(INCTX + 64 + 8, EP0RING | 1u);                     /* dequeue|DCS */
    ram_wr32(INCTX + 64 + 16, 8);
}

static uint32_t do_address(uint32_t ev[4]) {
    cmd_submit(TRB_CMD(9), ev);                                 /* Enable Slot */
    uint32_t slot = EV_SLOT(ev);
    ram_wr32(DCBAA_ADDR + slot * 8, DEVCTX);
    ram_wr32(DCBAA_ADDR + slot * 8 + 4, 0);
    build_address_input(64);
    cmd_submit_p(TRB_CMD(11) | (slot << 24), INCTX, 0, ev);
    t_ep_idx = 0; t_ep_cycle = 1;
    return slot;
}

static void ctl_transfer(uint32_t slot, uint32_t s0, uint32_t s1, int have_data,
                         int data_in, uint32_t dlen, uint32_t ev[4]) {
    uint32_t trt = have_data ? (data_in ? 3u : 2u) : 0u;
    ep_enqueue(s0, s1, 8, (2u << 10) | (1u << 6) | (trt << 16));         /* IDT */
    if (have_data)
        ep_enqueue(DATABUF, 0, dlen,
                   (3u << 10) | (1u << 2) | (data_in ? (1u << 16) : 0));  /* ISP */
    ep_enqueue(0, 0, 0,
               (4u << 10) | (1u << 5) | ((!have_data || !data_in) ? (1u << 16) : 0));
    wr32(DBS(slot), 1);
    next_event(ev);
}
#define SETUP_LO(rt, rq, wv) ((uint32_t)(rt) | ((uint32_t)(rq) << 8) | ((uint32_t)(wv) << 16))
#define SETUP_HI(wi, wl)     ((uint32_t)(wi) | ((uint32_t)(wl) << 16))
#define GET_DESC(slot_, type_, idx_, len_, ev_) \
    ctl_transfer((slot_), SETUP_LO(0x80, 6, ((type_) << 8) | (idx_)), \
                 SETUP_HI(0, len_), 1, 1, (len_), (ev_))

/* ---- v_address: Address Device + the guest's slot-state gate ----------- */
static void v_address(void) {
    int f0 = fails;
    hc_reset_program(32);
    uint32_t ev[4];
    uint32_t slot = do_address(ev);
    EXPECT(EV_CC(ev) == 1 && slot == 1, "Address Device cc=1 (cc=%u slot=%u)", EV_CC(ev), slot);
    uint32_t d3 = ram_rd32(DEVCTX + 12);
    EXPECT((d3 >> 27) == 2, "output Slot State = Addressed (got %u)", d3 >> 27);
    EXPECT((d3 & 0xFFu) == slot, "hardware device address reads back");
    EXPECT((ram_rd32(DEVCTX + 32) & 7u) == 1, "output EP0 State = Running");
    EXPECT((ram_rd32(DEVCTX + 32 + 8) & ~0xFu) == EP0RING, "output EP0 dequeue echoed");
    EXPECT(ram_rd32(DEVCTX + 0) == ((3u << 20) | (1u << 27)), "slot ctx dwords copied");
    cmd_submit_p(TRB_CMD(11) | (slot << 24), INCTX, 0, ev);
    EXPECT(EV_CC(ev) == 1, "re-Address succeeds");
    cmd_submit(TRB_CMD(10) | (slot << 24), ev);                 /* Disable */
    cmd_submit_p(TRB_CMD(11) | (slot << 24), INCTX, 0, ev);
    EXPECT(EV_CC(ev) == 11, "Address after Disable -> cc 11 (got %u)", EV_CC(ev));
    if (fails == f0) printf("ok address: slot Addressed in output ctx, EP0 Running, refusals\n");
}

/* ---- v_control_desc: descriptors over EP0, short-packet residue -------- */
static void v_control_desc(void) {
    int f0 = fails;
    hc_reset_program(32);
    uint32_t ev[4];
    uint32_t slot = do_address(ev);
    GET_DESC(slot, 1, 0, 18, ev);
    EXPECT(EV_TYPE(ev) == 32 && EV_CC(ev) == 1, "GET_DESCRIPTOR device: Transfer Event cc=1 (cc=%u)", EV_CC(ev));
    EXPECT(EV_EP(ev) == 1 && EV_SLOT(ev) == slot, "event stamped slot+EP0");
    EXPECT(rd8(DATABUF + 0) == 18 && rd8(DATABUF + 1) == 1, "device descriptor header");
    EXPECT(rd8(DATABUF + 7) == 64, "bMaxPacketSize0 = 64");
    EXPECT(rd8(DATABUF + 8) == 0x25 && rd8(DATABUF + 9) == 0x05, "VID 0525 (the shared stick)");
    EXPECT(rd8(DATABUF + 10) == 0xA5 && rd8(DATABUF + 11) == 0xA4, "PID a4a5");
    GET_DESC(slot, 1, 0, 8, ev);                                 /* MPS discovery read */
    EXPECT(EV_CC(ev) == 1 && EV_RES(ev) == 0, "8-byte discovery read exact");
    GET_DESC(slot, 2, 0, 9, ev);
    EXPECT(EV_CC(ev) == 1 && rd8(DATABUF + 2) == 32, "config header: wTotalLength 32");
    GET_DESC(slot, 2, 0, 256, ev);                               /* variable-length read */
    EXPECT(EV_CC(ev) == 13, "256-byte ask on 32-byte config -> cc 13 Short Packet (got %u)", EV_CC(ev));
    EXPECT(EV_RES(ev) == 256 - 32, "residue = untransferred bytes (got %u)", EV_RES(ev));
    EXPECT(rd8(DATABUF + 9) == 9 && rd8(DATABUF + 10) == 4, "interface descriptor landed intact");
    GET_DESC(slot, 3, 2, 255, ev);
    EXPECT(EV_CC(ev) == 13 && rd8(DATABUF + 0) == 18 && rd8(DATABUF + 2) == 'U',
           "string 2 'USB DISK' short read");
    /* SET_CONFIGURATION / GET_CONFIGURATION / GET_STATUS round-trip */
    ctl_transfer(slot, SETUP_LO(0x00, 9, 1), SETUP_HI(0, 0), 0, 0, 0, ev);
    EXPECT(EV_CC(ev) == 1, "SET_CONFIGURATION 1 cc=1");
    ctl_transfer(slot, SETUP_LO(0x80, 8, 0), SETUP_HI(0, 1), 1, 1, 1, ev);
    EXPECT(EV_CC(ev) == 1 && rd8(DATABUF) == 1, "GET_CONFIGURATION reads back 1");
    ctl_transfer(slot, SETUP_LO(0x80, 0, 0), SETUP_HI(0, 2), 1, 1, 2, ev);
    EXPECT(EV_CC(ev) == 1 && rd8(DATABUF) == 0 && rd8(DATABUF + 1) == 0, "GET_STATUS 0");
    ctl_transfer(slot, SETUP_LO(0xA1, 0xFE, 0), SETUP_HI(0, 1), 1, 1, 1, ev);
    EXPECT(EV_CC(ev) == 1 && rd8(DATABUF) == 0, "GET_MAX_LUN 0");
    if (fails == f0) printf("ok control: descriptors exact, cc13+residue on short reads, config r/w\n");
}

/* ---- v_control_stall: Stall halts; Reset + Set Dequeue recovers -------- */
static void v_control_stall(void) {
    int f0 = fails;
    hc_reset_program(32);
    uint32_t ev[4];
    uint32_t slot = do_address(ev);
    GET_DESC(slot, 0xFF, 0, 64, ev);                             /* no such descriptor */
    EXPECT(EV_CC(ev) == 6, "unknown descriptor -> cc 6 Stall (got %u)", EV_CC(ev));
    EXPECT((ram_rd32(DEVCTX + 32) & 7u) == 2, "EP0 Halted in output context");
    /* a fresh TD queues but cannot move while Halted */
    ep_enqueue(SETUP_LO(0x80, 6, 0x100), SETUP_HI(0, 18), 8, (2u << 10) | (1u << 6) | (3u << 16));
    ep_enqueue(DATABUF, 0, 18, (3u << 10) | (1u << 2) | (1u << 16));
    ep_enqueue(0, 0, 0, (4u << 10) | (1u << 5));
    wr32(DBS(slot), 1);
    EXPECT((ram_rd32(EVTRING + (uint32_t)t_ev_idx * 16 + 12) & 1u) != (uint32_t)t_ev_cycle,
           "no event while the endpoint is Halted");
    /* the guest's recovery: Reset Endpoint, then Set TR Dequeue to the
     * producer's enqueue point with the producer cycle */
    cmd_submit(TRB_CMD(15) | (1u << 16) | (slot << 24), ev);
    EXPECT(EV_CC(ev) == 1, "Reset Endpoint cc=1");
    uint32_t deq = (EP0RING + (uint32_t)t_ep_idx * 16) | (uint32_t)t_ep_cycle;
    cmd_submit_p(TRB_CMD(16) | (1u << 16) | (slot << 24), deq, 0, ev);
    EXPECT(EV_CC(ev) == 1, "Set TR Dequeue cc=1");
    GET_DESC(slot, 1, 0, 18, ev);
    EXPECT(EV_CC(ev) == 1 && rd8(DATABUF + 1) == 1, "control transfers run again");
    if (fails == f0) printf("ok stall: cc6 halts EP0, queue frozen, Reset+SetDequeue recovers\n");
}

/* ---- v_configure_eval: Configure Endpoint + Evaluate w/o dequeue move -- */
static void v_configure_eval(void) {
    int f0 = fails;
    hc_reset_program(32);
    uint32_t ev[4];
    uint32_t slot = do_address(ev);
    /* Configure a bulk OUT endpoint (ep_id 2) */
    zero_range(INCTX, 0x400); zero_range(EPXRING, 0x1000);
    ram_wr32(EPXRING + 255 * 16 + 0, EPXRING);
    ram_wr32(EPXRING + 255 * 16 + 12, (6u << 10) | (1u << 1) | 1u);
    ram_wr32(INCTX + 4, (1u << 2) | 1u);                          /* Add A2|A0 */
    ram_wr32(INCTX + 32 + 0, (3u << 20) | (2u << 27));            /* entries=2 */
    ram_wr32(INCTX + 32 + 4, (1u << 16));
    ram_wr32(INCTX + 96 + 4, (3u << 1) | (2u << 3) | (512u << 16));  /* bulk OUT 512 */
    ram_wr32(INCTX + 96 + 8, EPXRING | 1u);
    ram_wr32(INCTX + 96 + 16, 512);
    cmd_submit_p(TRB_CMD(12) | (slot << 24), INCTX, 0, ev);
    EXPECT(EV_CC(ev) == 1, "Configure Endpoint cc=1 (got %u)", EV_CC(ev));
    EXPECT((ram_rd32(DEVCTX + 64) & 7u) == 1, "EP2 Running in output context");
    EXPECT(((ram_rd32(DEVCTX + 12) >> 27) & 0x1Fu) == 3, "slot state Configured");
    /* Evaluate: move EP0's consumer first, then prove Evaluate didn't */
    GET_DESC(slot, 1, 0, 18, ev);
    EXPECT(EV_CC(ev) == 1, "pre-Evaluate transfer ok");
    zero_range(INCTX, 0x400);
    ram_wr32(INCTX + 4, 0x2);                                     /* A1 only */
    ram_wr32(INCTX + 64 + 4, (3u << 1) | (4u << 3) | (32u << 16));   /* MPS 32 */
    ram_wr32(INCTX + 64 + 8, EP0RING | 1u);                       /* guest shape: base|cycle */
    ram_wr32(INCTX + 64 + 16, 8);
    cmd_submit_p(TRB_CMD(13) | (slot << 24), INCTX, 0, ev);
    EXPECT(EV_CC(ev) == 1, "Evaluate Context cc=1 (got %u)", EV_CC(ev));
    EXPECT(((ram_rd32(DEVCTX + 32 + 4) >> 16) & 0xFFFFu) == 32, "EP0 MPS updated to 32");
    GET_DESC(slot, 1, 0, 18, ev);
    EXPECT(EV_CC(ev) == 1, "consumer unmoved by Evaluate (next TD answers)");
    /* refusals */
    cmd_submit_p(TRB_CMD(13) | (2u << 24), INCTX, 0, ev);         /* slot 2 never enabled */
    EXPECT(EV_CC(ev) == 11, "Evaluate on unenabled slot -> cc 11 (got %u)", EV_CC(ev));
    cmd_submit(TRB_CMD(15) | (5u << 16) | (slot << 24), ev);      /* EP5 not configured */
    EXPECT(EV_CC(ev) == 14, "Reset of unconfigured EP -> cc 14 (got %u)", EV_CC(ev));
    if (fails == f0) printf("ok configure/evaluate: EP2 Running, slot Configured, MPS swap keeps ring\n");
}

/* ================= X4: bulk (BOT) + interrupt rings ==================== */
#define OUTRING  0x00048000u
#define INRING   0x00049000u
#define INTRRING 0x0004A000u
#define BOTBUF   0x0004B000u
#define BOTDATA  0x0004C000u

typedef struct { uint32_t base; int idx, cyc; } tring_t;
static void tring_init(tring_t *r, uint32_t base) {
    zero_range(base, 0x1000);
    ram_wr32(base + 255 * 16 + 0, base);
    ram_wr32(base + 255 * 16 + 12, (6u << 10) | (1u << 1) | 1u);
    r->base = base; r->idx = 0; r->cyc = 1;
}
static void tring_push(tring_t *r, uint32_t buf, uint32_t len, uint32_t flags) {
    uint32_t b = r->base + (uint32_t)r->idx * 16;
    ram_wr32(b + 0, buf); ram_wr32(b + 4, 0); ram_wr32(b + 8, len);
    ram_wr32(b + 12, flags | (uint32_t)r->cyc);
    r->idx++;
    if (r->idx >= 255) {
        ram_wr32(r->base + 255 * 16 + 0, r->base);
        ram_wr32(r->base + 255 * 16 + 12, (6u << 10) | (1u << 1) | (uint32_t)r->cyc);
        r->idx = 0;
        r->cyc ^= 1;
    }
}
#define NORMAL_FLAGS(ioc, ch) ((1u << 10) | (1u << 2) | /* ISP */ \
                               ((ioc) ? (1u << 5) : 0) | ((ch) ? (1u << 4) : 0))

static void cfg_ep(uint32_t slot, uint32_t epid, uint32_t type, uint32_t ring,
                   uint32_t mps, uint32_t entries, uint32_t ev[4]) {
    zero_range(INCTX, 0x400);
    ram_wr32(INCTX + 4, (1u << epid) | 1u);
    ram_wr32(INCTX + 32 + 0, (3u << 20) | (entries << 27));
    ram_wr32(INCTX + 32 + 4, (1u << 16));
    uint32_t e = INCTX + (1 + epid) * 32;
    ram_wr32(e + 4, (3u << 1) | (type << 3) | (mps << 16));
    ram_wr32(e + 8, ring | 1u);
    ram_wr32(e + 16, mps);
    cmd_submit_p(TRB_CMD(12) | (slot << 24), INCTX, 0, ev);
}

static tring_t r_out, r_in;
static uint32_t g_slot;
static void bot_cbw(uint32_t tag, uint32_t xlen, uint8_t flags,
                    const uint8_t *cb, int cblen, uint32_t ev[4]) {
    zero_range(BOTBUF, 64);
    ram_wr32(BOTBUF + 0, 0x43425355u);                /* 'USBC' */
    ram_wr32(BOTBUF + 4, tag);
    ram_wr32(BOTBUF + 8, xlen);
    mem_write(&M, BOTBUF + 12, 1, flags);
    mem_write(&M, BOTBUF + 14, 1, (uint32_t)cblen);
    for (int i = 0; i < cblen; i++) mem_write(&M, BOTBUF + 15 + i, 1, cb[i]);
    tring_push(&r_out, BOTBUF, 31, NORMAL_FLAGS(1, 0));
    wr32(DBS(g_slot), 2);
    next_event(ev);
}
static void bot_data_in(uint32_t len, uint32_t ev[4]) {
    tring_push(&r_in, BOTDATA, len, NORMAL_FLAGS(1, 0));
    wr32(DBS(g_slot), 3);
    next_event(ev);
}
static void bot_csw(uint32_t tag, uint32_t status, uint32_t residue, uint32_t ev[4]) {
    zero_range(BOTDATA, 16);
    bot_data_in(13, ev);
    EXPECT(EV_CC(ev) == 1, "CSW transfer cc=1");
    EXPECT(ram_rd32(BOTDATA) == 0x53425355u, "CSW signature USBS");
    EXPECT(ram_rd32(BOTDATA + 4) == tag, "CSW tag echoed");
    EXPECT(ram_rd32(BOTDATA + 8) == residue, "CSW residue %u (got %u)", residue, ram_rd32(BOTDATA + 8));
    EXPECT(rd8(BOTDATA + 12) == status, "CSW status %u (got %u)", status, rd8(BOTDATA + 12));
}

/* ---- v_bulk_bot: the shared MSC stick over Normal-TRB chains ---------- */
static uint8_t img[8 * 512];
static void v_bulk_bot(void) {
    int f0 = fails;
    hc_reset_program(32);
    uint32_t ev[4];
    g_slot = do_address(ev);
    tring_init(&r_out, OUTRING);
    tring_init(&r_in, INRING);
    cfg_ep(g_slot, 2, 2, OUTRING, 512, 2, ev);        /* bulk OUT */
    EXPECT(EV_CC(ev) == 1, "bulk OUT configured");
    cfg_ep(g_slot, 3, 6, INRING, 512, 3, ev);         /* bulk IN */
    EXPECT(EV_CC(ev) == 1, "bulk IN configured");
    /* no medium: TEST UNIT READY fails honestly end-to-end */
    static const uint8_t tur[6] = { 0x00, 0, 0, 0, 0, 0 };
    bot_cbw(0xA0, 0, 0x00, tur, 6, ev);
    EXPECT(EV_CC(ev) == 1 && EV_EP(ev) == 2, "CBW transfer event on EP2");
    bot_csw(0xA0, 1 /* FAILED */, 0, ev);
    /* medium attached */
    for (int s = 0; s < 8; s++)
        for (int i = 0; i < 512; i++) img[s * 512 + i] = (uint8_t)(0x10 + s);
    img[510] = 0x55; img[511] = 0xAA;
    M.disk = img; M.disk_len = sizeof img;
    bot_cbw(0xA1, 0, 0x00, tur, 6, ev);
    bot_csw(0xA1, 0, 0, ev);
    /* INQUIRY */
    static const uint8_t inq[6] = { 0x12, 0, 0, 0, 36, 0 };
    bot_cbw(0xA2, 36, 0x80, inq, 6, ev);
    bot_data_in(36, ev);
    EXPECT(EV_CC(ev) == 1 && EV_RES(ev) == 0, "INQUIRY data exact");
    char vend[9] = { 0 };
    for (int i = 0; i < 8; i++) vend[i] = (char)rd8(BOTDATA + 8 + i);
    EXPECT(strcmp(vend, "AURALITE") == 0, "INQUIRY vendor (got %.8s)", vend);
    bot_csw(0xA2, 0, 0, ev);
    /* READ CAPACITY(10) */
    static const uint8_t cap[10] = { 0x25, 0, 0, 0, 0, 0, 0, 0, 0, 0 };
    bot_cbw(0xA3, 8, 0x80, cap, 10, ev);
    bot_data_in(8, ev);
    EXPECT(rd8(BOTDATA + 3) == 7 && rd8(BOTDATA + 6) == 0x02, "last LBA 7, 512-byte blocks");
    bot_csw(0xA3, 0, 0, ev);
    /* READ(10) LBA2, 2 blocks, as one CH-chained transfer: ONE event */
    static const uint8_t rd2[10] = { 0x28, 0, 0, 0, 0, 2, 0, 0, 2, 0 };
    bot_cbw(0xA4, 1024, 0x80, rd2, 10, ev);
    tring_push(&r_in, BOTDATA, 512, NORMAL_FLAGS(0, 1) /* CH, no IOC */);
    tring_push(&r_in, BOTDATA + 512, 512, NORMAL_FLAGS(1, 0));
    wr32(DBS(g_slot), 3);
    next_event(ev);
    EXPECT(EV_CC(ev) == 1 && EV_RES(ev) == 0, "chained READ cc=1");
    EXPECT((ram_rd32(EVTRING + (uint32_t)t_ev_idx * 16 + 12) & 1u) != (uint32_t)t_ev_cycle,
           "one event for the whole chain");
    EXPECT(rd8(BOTDATA) == 0x12 && rd8(BOTDATA + 511) == 0x12 &&
           rd8(BOTDATA + 512) == 0x13 && rd8(BOTDATA + 1023) == 0x13,
           "both sectors streamed across the chain");
    bot_csw(0xA4, 0, 0, ev);
    /* short data phase: last sector only, 2 asked -> cc13 + CSW residue */
    static const uint8_t rd_short[10] = { 0x28, 0, 0, 0, 0, 7, 0, 0, 2, 0 };
    bot_cbw(0xA5, 1024, 0x80, rd_short, 10, ev);
    bot_data_in(1024, ev);
    EXPECT(EV_CC(ev) == 13 && EV_RES(ev) == 512, "short READ -> cc13 residue 512 (cc=%u res=%u)",
           EV_CC(ev), EV_RES(ev));
    EXPECT(rd8(BOTDATA) == 0x17, "sector 7 content arrived");
    bot_csw(0xA5, 0, 512, ev);
    /* malformed CBW -> PHASE error CSW (the shared model's verdict) */
    zero_range(BOTBUF, 64);
    ram_wr32(BOTBUF, 0xDEADBEEFu);
    ram_wr32(BOTBUF + 4, 0xA6);
    tring_push(&r_out, BOTBUF, 31, NORMAL_FLAGS(1, 0));
    wr32(DBS(g_slot), 2);
    next_event(ev);
    bot_csw(0xA6, 2 /* PHASE */, 0, ev);
    M.disk = NULL; M.disk_len = 0;
    if (fails == f0) printf("ok bulk BOT: TUR/INQUIRY/CAP/READ over chains, short+phase honest\n");
}

/* ---- v_interrupt_idle: an armed TRB with no source stays armed -------- */
static void v_interrupt_idle(void) {
    int f0 = fails;
    hc_reset_program(32);
    uint32_t ev[4];
    g_slot = do_address(ev);
    tring_init(&r_in, INTRRING);
    cfg_ep(g_slot, 5, 7 /* interrupt IN */, INTRRING, 8, 5, ev);
    EXPECT(EV_CC(ev) == 1, "interrupt IN configured");
    /* the guest's HID arm: one TRB + doorbell, non-blocking expectation */
    tring_push(&r_in, BOTDATA, 8, NORMAL_FLAGS(1, 0));
    wr32(DBS(g_slot), 5);
    EXPECT((ram_rd32(EVTRING + (uint32_t)t_ev_idx * 16 + 12) & 1u) != (uint32_t)t_ev_cycle,
           "no event for an endpoint nobody talks on (NAK)");
    wr32(DBS(g_slot), 5);
    EXPECT((ram_rd32(EVTRING + (uint32_t)t_ev_idx * 16 + 12) & 1u) != (uint32_t)t_ev_cycle,
           "TRB still armed after a second doorbell");
    /* and the rest of the controller is unaffected */
    GET_DESC(g_slot, 1, 0, 18, ev);
    EXPECT(EV_CC(ev) == 1 && EV_EP(ev) == 1, "EP0 answers while the intr TRB waits");
    if (fails == f0) printf("ok interrupt: armed TRB NAKs forever without a source, no fake reports\n");
}

/* ================= X5: interrupter pin + ports ======================== */
#define IRQ10_LEVEL ((M.pic.slave.lines >> 2) & 1u)   /* IRQ10 = slave line 2 */

/* ---- v_ports_live: presence, PSC events, RW1C, reset->enabled --------- */
static void v_ports_live(void) {
    int f0 = fails;
    for (int i = 0; i < (int)sizeof img; i++) img[i] = 0x42;
    M.disk = img; M.disk_len = sizeof img;
    hc_reset_program(32);
    uint32_t ev[4];
    uint32_t ps = rd32(PORT(0));
    EXPECT(ps & (1u << 0), "port 1 CCS with a disk attached");
    EXPECT(((ps >> 10) & 0xFu) == 3, "port 1 speed High-Speed (got %u)", (ps >> 10) & 0xFu);
    EXPECT(!(ps & (1u << 1)), "port 1 not enabled before reset");
    EXPECT(ps & (1u << 17), "CSC latched as the controller runs");
    EXPECT(ps & (1u << 9), "PP stays up (PPC=0)");
    EXPECT(rd32(OP(0x04)) & (1u << 4), "USBSTS.PCD mirrors the latch");
    EXPECT(IRQ10_LEVEL == 1, "INTx asserted: PSC pending with IE+INTE");
    EXPECT(!(rd32(PORT(1)) & 1u) && !(rd32(PORT(3)) & 1u), "ports 2 and 4 empty");
    /* the Port Status Change event heads the ring (the guest peeks it) */
    next_event(ev);
    EXPECT(EV_TYPE(ev) == 34, "event type 34 Port Status Change (got %u)", EV_TYPE(ev));
    EXPECT((ev[0] >> 24) == 1, "PSC names port 1 (got %u)", ev[0] >> 24);
    EXPECT(EV_CC(ev) == 1, "PSC completion code Success");
    EXPECT(IRQ10_LEVEL == 0, "consume + EHB ack drops INTx");
    /* the RW1C clear vector deferred since X1 (nothing could latch) */
    wr32(PORT(0), (1u << 9) | (1u << 17));
    EXPECT(!(rd32(PORT(0)) & (1u << 17)), "CSC clears on write-1");
    EXPECT(!(rd32(OP(0x04)) & (1u << 4)), "PCD clears with the last latch");
    /* port reset completes synchronously: PR gone, PED + PRC + PSC */
    ps = rd32(PORT(0));
    wr32(PORT(0), (ps & (1u << 9)) | (1u << 4));
    ps = rd32(PORT(0));
    EXPECT(!(ps & (1u << 4)), "PR reads back clear");
    EXPECT(ps & (1u << 1), "PED set after reset");
    EXPECT(ps & (1u << 21), "PRC latched");
    EXPECT(IRQ10_LEVEL == 1, "INTx re-asserts on the reset PSC");
    next_event(ev);
    EXPECT(EV_TYPE(ev) == 34 && (ev[0] >> 24) == 1, "reset posts a second PSC");
    wr32(PORT(0), (1u << 9) | (1u << 21));
    EXPECT(!(rd32(PORT(0)) & (1u << 21)), "PRC clears on write-1");
    /* end to end: enumeration above the enabled port */
    g_slot = do_address(ev);
    EXPECT(EV_CC(ev) == 1, "slot addresses on the enabled port");
    GET_DESC(g_slot, 1, 0, 18, ev);
    EXPECT(EV_CC(ev) == 1 && rd8(DATABUF + 8) == 0x25, "descriptors answer over the port");
    M.disk = NULL; M.disk_len = 0;
    hc_reset_program(32);
    EXPECT(!(rd32(PORT(0)) & 1u), "medium gone + reset: CCS clears");
    if (fails == f0) printf("ok ports: CCS/HS/CSC@RUN, RW1C clears, PR->PED+PRC, PSC x2, INTx cycle\n");
}

/* ---- v_irq_masking: IE / INTE gates and the two ack paths -------------- */
static void v_irq_masking(void) {
    int f0 = fails;
    M.disk = img; M.disk_len = sizeof img;
    hc_reset_program(32);
    uint32_t ev[4];
    EXPECT(IRQ10_LEVEL == 1, "PSC pending after RUN");
    wr32(RT(0x20), (1u << 1));                    /* IE=1, IP bit 0: no ack */
    EXPECT(IRQ10_LEVEL == 1, "IMAN write without the IP bit does not ack");
    wr32(RT(0x20), 0);                            /* IE=0 */
    EXPECT(IRQ10_LEVEL == 0 && (rd32(RT(0x20)) & 1u),
           "IE=0 masks the pin while IP stays latched");
    wr32(RT(0x20), (1u << 1) | 1u);               /* the guest handler's ack */
    EXPECT(!(rd32(RT(0x20)) & 1u) && IRQ10_LEVEL == 0, "RW1C ack clears IP");
    next_event(ev);
    EXPECT(EV_TYPE(ev) == 34, "drained the pending PSC afterwards");
    wr32(OP(0x00), 1u);                           /* RUN without INTE */
    ram_wr32(CMDRING + 12, TRB_CMD(23) | 1u);
    wr32(DB0, 0);
    EXPECT(rd32(RT(0x20)) & 1u, "No Op completed and latched IP");
    EXPECT(IRQ10_LEVEL == 0, "INTE=0 keeps the pin down");
    wr32(OP(0x00), 4u | 1u);
    EXPECT(IRQ10_LEVEL == 1, "INTE=1 re-asserts for the latched IP");
    next_event(ev);
    EXPECT(EV_TYPE(ev) == 33 && IRQ10_LEVEL == 0, "EHB ack settles the pin");
    M.disk = NULL; M.disk_len = 0;
    if (fails == f0) printf("ok irq masking: IE+INTE gates, IMAN RW1C and ERDP-EHB acks\n");
}

/* ---- v_determinism: same script, byte-identical event stream ---------- */
static void v_determinism(void) {
    int f0 = fails;
    uint32_t run1[8][4], run2[8][4], ev[4];
    for (int r = 0; r < 2; r++) {
        uint32_t (*dst)[4] = r ? run2 : run1;
        hc_reset_program(32);
        cmd_submit(TRB_CMD(9), ev);
        memcpy(dst[0], ev, sizeof ev);
        for (int i = 1; i < 8; i++) {
            cmd_submit(TRB_CMD(23), ev);
            memcpy(dst[i], ev, sizeof ev);
        }
    }
    EXPECT(memcmp(run1, run2, sizeof run1) == 0,
           "identical doorbell scripts give identical event streams");
    if (fails == f0) printf("ok determinism: full replay x2 byte-identical\n");
}

/* ================= MSI M1: MSI-X capability ============================ */
/* ---- v_msix_cap: QEMU shape, RO discipline, table MMIO ---------------- */
static void v_msix_cap(void) {
    int f0 = fails;
    EXPECT(pci_rd8(20, 0x06) & 0x10, "Status: Capabilities List present");
    EXPECT(pci_rd8(20, 0x34) == 0x90, "Capabilities Pointer 0x90 (got %02x)", pci_rd8(20, 0x34));
    EXPECT(pci_rd8(20, 0x90) == 0x11, "capability ID 0x11 (MSI-X)");
    EXPECT(pci_rd8(20, 0x91) == 0x00, "next capability: none");
    uint32_t MC = pci_rd32(20, 0x90) >> 16;
    EXPECT((MC & 0x7FFu) == 15, "Table Size = 16 entries (MC=%04x)", MC);
    EXPECT(pci_rd32(20, 0x94) == 0x3000u, "table BAR0+0x3000 BIR0 (got %08x)", pci_rd32(20, 0x94));
    EXPECT(pci_rd32(20, 0x98) == 0x3800u, "PBA BAR0+0x3800 BIR0 (got %08x)", pci_rd32(20, 0x98));
    /* RO discipline: a blast write moves only Enable/Function Mask */
    pci_wr32(20, 0x90, 0xFFFFFFFFu);
    pci_wr32(20, 0x94, 0xFFFFFFFFu);
    pci_wr32(20, 0x98, 0xFFFFFFFFu);
    uint32_t dw = pci_rd32(20, 0x90);
    EXPECT((dw & 0xFFFFu) == 0x0011u, "ID/next survive the blast");
    MC = dw >> 16;
    EXPECT((MC & 0x7FFu) == 15 && (MC & 0x3800u) == 0, "Table Size + reserved bits RO");
    EXPECT((MC & 0xC000u) == 0xC000u, "Enable + Function Mask took the write");
    EXPECT(pci_rd32(20, 0x94) == 0x3000u && pci_rd32(20, 0x98) == 0x3800u, "offset/BIR dwords RO");
    pci_wr32(20, 0x90, 0x00000011u);                       /* MC := 0 */
    EXPECT(((pci_rd32(20, 0x90) >> 16) & 0xC000u) == 0, "Enable/FMask clear again");
    /* vector table MMIO: reset-masked, storage round-trips, PBA reads 0 */
    uint32_t T = XHCI_BAR + 0x3000u;
    EXPECT(rd32(T + 12) == 1, "entry 0 masked at power-on (Vector Control bit0)");
    wr32(T + 0, 0xFEE00000u); wr32(T + 4, 0); wr32(T + 8, 0x42u); wr32(T + 12, 0);
    EXPECT(rd32(T + 0) == 0xFEE00000u && rd32(T + 8) == 0x42u && rd32(T + 12) == 0,
           "entry 0 round-trips");
    wr32(T + 15 * 16 + 8, 0x77u);
    EXPECT(rd32(T + 15 * 16 + 8) == 0x77u, "entry 15 storage exists");
    EXPECT(rd32(T + 16 * 16) == 0, "past the table reads 0");
    EXPECT(rd32(XHCI_BAR + 0x3800u) == 0 && rd32(XHCI_BAR + 0x3804u) == 0,
           "PBA reads 0 (v1: no pending accumulation)");
    /* HCRST is an HC reset, not a PCI reset: MSI-X state persists */
    hc_reset_program(32);
    EXPECT(rd32(T + 8) == 0x42u && rd32(T + 12) == 0, "table survives HCRST");
    if (fails == f0) printf("ok msix cap: QEMU shape @0x90, RO held, table MMIO, HCRST-persistent\n");
}

/* ---- v_msix_route: messages replace the pin, fallback restores it ---- */
static void queue_noop(void) {
    uint32_t b = CMDRING + (uint32_t)t_cmd_idx * 16;
    ram_wr32(b + 0, 0); ram_wr32(b + 4, 0); ram_wr32(b + 8, 0);
    ram_wr32(b + 12, TRB_CMD(23) | (uint32_t)t_cmd_cycle);
    t_cmd_idx++;
}
static void lapic_eoi(void) { mem_write(&M, 0xFEE000B0u, 4, 0); }

static void v_msix_route(void) {
    int f0 = fails;
    M.lapic.svr |= 0x100u;                       /* APIC software-enable */
    hc_reset_program(32);
    uint32_t ev[4];
    uint32_t T = XHCI_BAR + 0x3000u;
    /* arm entry 0 (vector 0x61 -> CPU0, unmasked) and enable MSI-X */
    wr32(T + 0, 0xFEE00000u); wr32(T + 4, 0); wr32(T + 8, 0x61u); wr32(T + 12, 0);
    pci_wr32(20, 0x90, (0x8000u << 16) | 0x11u);
    queue_noop();
    wr32(DB0, 0);
    EXPECT(rd32(RT(0x20)) & 1u, "IP latched by the completion");
    EXPECT(!(M.pic.slave.lines & (1u << 2)), "INTx pin silent under MSI-X");
    EXPECT(lapic_intack(&M) == 0x61, "message vector 0x61 delivered");
    lapic_eoi();
    /* the level still pends: a second completion must not re-send */
    queue_noop();
    wr32(DB0, 0);
    EXPECT(lapic_intack(&M) < 0, "no duplicate message for a held level");
    next_event(ev); next_event(ev);              /* consume + EHB ack */
    queue_noop();
    wr32(DB0, 0);
    EXPECT(lapic_intack(&M) == 0x61, "edge re-armed after the ack");
    lapic_eoi();
    next_event(ev);
    /* per-vector mask: pending rises silently, unmask releases it */
    wr32(T + 12, 1u);
    queue_noop();
    wr32(DB0, 0);
    EXPECT(lapic_intack(&M) < 0, "masked vector delivers nothing");
    wr32(T + 12, 0);
    EXPECT(lapic_intack(&M) == 0x61, "unmask releases the held level");
    lapic_eoi();
    next_event(ev);
    /* Function Mask behaves the same at capability level */
    pci_wr32(20, 0x90, (0xC000u << 16) | 0x11u);
    queue_noop();
    wr32(DB0, 0);
    EXPECT(lapic_intack(&M) < 0, "Function Mask delivers nothing");
    pci_wr32(20, 0x90, (0x8000u << 16) | 0x11u);
    EXPECT(lapic_intack(&M) == 0x61, "function unmask releases it");
    lapic_eoi();
    next_event(ev);
    /* MSI-X off: the pin returns, byte-identical fallback */
    pci_wr32(20, 0x90, 0x11u);
    queue_noop();
    wr32(DB0, 0);
    EXPECT(M.pic.slave.lines & (1u << 2), "INTx back with MSI-X disabled");
    next_event(ev);
    EXPECT(!(M.pic.slave.lines & (1u << 2)), "pin acked clean");
    if (fails == f0) printf("ok msix route: edge-once messages, mask/unmask release, INTx fallback\n");
}

/* ---- v_msix_snapshot: capability + table + router survive save/load -- */
/* Runs last on purpose: snap_load rewrites the whole machine image. */
static void v_msix_snapshot(void) {
    int f0 = fails;
    M.lapic.svr |= 0x100u;
    hc_reset_program(32);
    uint32_t T = XHCI_BAR + 0x3000u;
    wr32(T + 0, 0xFEE00000u); wr32(T + 4, 0); wr32(T + 8, 0x71u); wr32(T + 12, 0);
    pci_wr32(20, 0x90, (0x8000u << 16) | 0x11u);        /* MSI-X Enable */
    static const snap_cfg_t CFG = { "Haswell", 0, 0 };
    const char *path = "m4_snap_tmp.bin";
    EXPECT(snap_save(&M, path, &CFG) == 0, "snapshot saves");
    /* vandalise everything the snapshot is supposed to preserve */
    pci_wr32(20, 0x90, 0x11u);
    wr32(T + 8, 0x22u); wr32(T + 12, 1u);
    hc_reset_program(32);
    EXPECT(((pci_rd32(20, 0x90) >> 16) & 0x8000u) == 0, "live state really off");
    EXPECT(rd32(T + 8) == 0x22u, "table scribble is live");
    EXPECT(snap_load(&M, path, &CFG) == 0, "snapshot loads");
    EXPECT(((pci_rd32(20, 0x90) >> 16) & 0x8000u) != 0, "MSI-X Enable back (pci cfg tag)");
    EXPECT(rd32(T + 8) == 0x71u && rd32(T + 12) == 0, "vector entry back (dev.xhci tag)");
    /* RAM came from the snapshot too: re-sync the fixture's own counters
     * to the save-time ones and prove the router is live, not just stored */
    t_cmd_idx = 0; t_cmd_cycle = 1; t_ev_idx = 0; t_ev_cycle = 1;
    queue_noop();
    wr32(DB0, 0);
    EXPECT(lapic_intack(&M) == 0x71, "message delivered right after load");
    lapic_eoi();
    EXPECT(!(M.pic.slave.lines & (1u << 2)), "pin still silent after load");
    remove(path);
    if (fails == f0) printf("ok msix snapshot: enable+table+router survive save/load\n");
}

int main(void) {
    setup_machine(&M);
    devices_init_common(&M);
    v_pci_identity();
    v_bar_probe();
    v_caps();
    v_op_reset();
    v_programming();
    v_ports();
    v_interrupter();
    v_doorbell();
    v_guest_init();
    v_cmd_noop();
    v_cmd_wrap();
    v_enable_slot();
    v_cmd_unknown();
    v_address();
    v_control_desc();
    v_control_stall();
    v_configure_eval();
    v_bulk_bot();
    v_interrupt_idle();
    v_ports_live();
    v_irq_masking();
    v_determinism();
    v_msix_cap();
    v_msix_route();
    v_msix_snapshot();
    devices_done(&M);
    if (fails) { printf("test-xhci: %d FAILED\n", fails); return 1; }
    puts("test-xhci: all vectors passed");
    return 0;
}
