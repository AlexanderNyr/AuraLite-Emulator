/* src/e1000.c -- NET N0: Intel 82540EM (e1000) "RX-ready" model + TAP socket
 * ingress.  Ground rules (fixed, N0 artifact):
 *   - TAP socket: UDP datagrams = Ethernet frames, the fixed endpoint
 *     127.0.0.1:8080 (the emulator connects; the host-side responder binds).
 *   - MAC: the QEMU default 52:54:00:12:34:56, pre-loaded into RAL/RAH like
 *     QEMU's e1000_reset (RAH.AV set) and, since U2, also present in EEPROM
 *     words 0-2 -- the guest probes the EEPROM first (UPKEEP U0 measured
 *     three DONE timeouts per probe against the old storage cell), so the
 *     same address now arrives through its preferred source.
 *   - The device is OPT-IN (--tap=): no PCI function exists without the
 *     flag, so every historical lane (K0-K8, firmware, U5 tapes) keeps its
 *     byte-for-byte receipts.
 *   - QEMU compat: the reset state follows QEMU's e1000_reset (RXCSUM reads
 *     0 after CTRL.RST, RAL/RAH preloaded, STATUS.LU on link).
 *
 * N0 covered the PCI function + BAR0 register file + reset + RX ring DMA
 * (socket -> guest).  N1 adds the TX half: the TDT/TCTL doorbell walks TDH
 * over the legacy TX ring, the frame leaves through the TAP socket
 * (TCTL.EN gated) and the descriptors are completed (status.DD), so the
 * guest's e1000_send() DD spin-wait retires instead of printing
 * "[e1000] TX timeout: desc[N].status=0x00 TDH=0 TDT=N+1" (defect D2).
 * TX semantics are measured from QEMU hw/net/e1000.c start_xmit /
 * process_tx_desc / txdesc_writeback, and the guest side from
 * AuraLite-OS@0ed0d29 drivers/e1000/e1000.c e1000_send() (cmd 0x0B =
 * EOP|IFCS|RS, TDT tail doorbell, spin on status.DD, TDH read back for
 * the timeout receipt).
 *
 * U2 (UPKEEP_PLAN) gives the two command registers N0 left as storage
 * cells their hardware behind them: EERD reads a real 64-word EEPROM
 * (QEMU's template, the device MAC in words 0-2, 0xBABA checksum) with
 * DONE completing synchronously on the read, and MDIC drives a
 * 32-register M88E1000 PHY file at PHY address 1 with QEMU's capability
 * table, READY/ERROR semantics and the MDAC cause.  Values are measured
 * from QEMU hw/net/e1000.c (set_mdic, flash_eerd_read, phy_reg_init,
 * e1000_eeprom_template), e1000x_common.c (e1000x_core_prepare_eeprom)
 * and e1000x_regs.h (bit constants); the guest side is the frozen
 * driver's EERD probe (words 0-2, DONE poll). */
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
#define E1000_MDIC        0x00020   /* U2: Intel's offset (e1000x_regs.h
                                     * E1000_MDIC 0x00020).  N0's 0x200 was a
                                     * typo no consumer could observe: MDIC
                                     * was a storage cell and the guest
                                     * never writes it                        */
#define E1000_ICR         0x000C0
#define E1000_ICS         0x000C8
#define E1000_IMS         0x000D0
#define E1000_IMC         0x000D8
#define E1000_RCTL        0x00100
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
#define ICR_TXDW          (1u << 0)   /* TX descriptor written back */
#define ICR_LSC           (1u << 2)   /* link status change         */
#define ICR_RXDMT0        (1u << 4)
#define ICR_RXO           (1u << 6)
#define ICR_RXT0          (1u << 7)
#define ICR_TXQE          (1u << 7)   /* TX queue empty (QEMU: same bit slot,
                                       * distinct cause -- start_xmit always
                                       * raises it, the guest unmasks it)    */
#define ICR_MDAC          (1u << 9)   /* U2: MDI/O access complete (Intel
                                       * E1000_ICR_MDAC 0x200; set_mdic raises
                                       * it when MDIC INT_EN survives)          */
#define RXD_STAT_DD       0x01

/* N1: legacy TX descriptor (Intel 8254x SDM, 16 bytes).  lower.data =
 * length | cso<<16 | cmd<<24; upper.data = status | css<<8 | popts<<16 |
 * special<<24, i.e. status is the BYTE at descriptor offset 12 and cmd the
 * byte at offset 11.  The guest driver's struct tx_desc
 * (drivers/e1000/e1000.c:93) agrees byte for byte:
 * addr[0:8) length[8:10) cso[10] cmd[11] status[12] css[13] special[14:16). */
#define TXD_CMD_EOP       (1u << 24)  /* CMD bit0: end of packet             */
#define TXD_CMD_RS        (1u << 27)  /* CMD bit3: report status (writeback) */
#define TXD_CMD_DEXT      (1u << 29)  /* CMD bit5: 0 = legacy descriptor     */
/* STATUS byte (descriptor offset 12).  QEMU's e1000 model instead defines
 * E1000_TXD_STAT_DD as 0x01000000, i.e. bit 24 of the dword read at offset
 * 12 -- which lands in descriptor byte 15 (the VLAN ID byte), not the status
 * byte.  Measured against the two things this project is pinned to, the
 * byte-granular form is the contract: the Intel 8254x SDM puts DD/EC/LC/TU
 * in bits 0..3 of the status byte, and the frozen guest driver tests
 * `tx_ring[old_tail].status & 0x01` where `status` is the u8 at struct
 * offset 12 (drivers/e1000/e1000.c:93,571).  The RX half of this very file
 * already writes its DD that way (RXD_STAT_DD 0x01 at the same offset) and
 * that is what the N0 guest receipt showed. */
#define TXD_STAT_DD       0x01u       /* STATUS bit0: descriptor done        */
#define TXD_STAT_EC       0x02u       /* STATUS bit1: excess collisions      */
#define TXD_STAT_LC       0x04u       /* STATUS bit2: late collision         */
#define TXD_STAT_TU       0x08u       /* STATUS bit3: TX underrun            */

#define E1000_TX_DATA_MAX 0x10000u    /* QEMU s->tx.data[] size               */
#define E1000_TX_DESC_SZ   16u
#define E1000_DLEN_MASK    0xfff80u   /* QEMU set_dlen: TDLEN/RDLEN are 128B
                                       * multiples (both rings)                */

/* NET N4 (ledger D4): the INTx line.  Board wire: the device asserts ISA
 * line E1000_IRQ (published at PCI cfg 0x3C = line, 0x3D = pin INTA#)
 * through pic_set_irq(), which fans the same line out to the I/O APIC
 * (CHIPSET H7 board wire) -- the AHCI S5 pattern.  The line is held exactly
 * while the interrupt condition holds, the plan's `(ICR & IMS) != 0`:
 * a cause that is still masked stays quiet, and the natural clear paths
 * (ICR read-to-clear, ICR W1C, IMC masking) drop the line, so a guest
 * handler's "read ICR, then EOI" sequence re-services correctly.
 *
 * The NIC payloads unmask IMS=0xD5 but run at IF=0 on a polled path and
 * drain ICR before idling, so the wire is machine honesty -- pinned by the
 * unit vectors below, invisible to their receipts. */
#define E1000_IRQ          11


typedef struct {
    machine_t *m;
    uint32_t regs[E1000_REG_BYTES / 4];
    int tap_fd;            /* -1 = no TAP socket */
    int attached;
    /* N1: TX assembly state (QEMU struct e1000_tx).  A frame may span
     * several descriptors, so bytes accumulate here until CMD.EOP; `busy`
     * is QEMU's re-entrancy guard (a doorbell inside the walk is a no-op). */
    uint8_t  tx_data[E1000_TX_DATA_MAX];
    uint32_t tx_size;
    int      tx_busy;
    /* Counters, so the walk stays deterministic and the log stays quiet:
     * every frame that reached the TAP socket, and every frame dropped
     * because no socket is attached (the socketless unit lane). */
    uint64_t tx_frames, tx_dropped;
    /* N4: current INTx line level (the E1000_IRQ board wire).  Tracked so
     * pic_set_irq is driven on CHANGE, the AHCI S5 convention -- correct for
     * both edge- and level-triggered PIC configurations. */
    int      intx;
    /* U2: the 64-word EEPROM image behind EERD and the 32-register PHY
     * file behind MDIC.  Host-side device state only: the e1000 exists
     * solely in --tap runs (which snapshot save/load refuses) and e1000_t
     * is not part of machine_t, so the snapshot layout does not move. */
    uint16_t eeprom[64];
    uint16_t phy[0x20];
} e1000_t;
static e1000_t g_e1000;

/* QEMU's e1000_reset behavioural core: zeroed register file, link up,
 * pre-initialized RAL/RAH (AddrValid + the QEMU default MAC), RXCSUM 0. */
/* N4: the INTx board wire.  Level = ((ICR & IMS) != 0), driven on change
 * onto ISA line E1000_IRQ (AHCI S5 pattern; pic_set_irq fans out to the
 * I/O APIC).  Every mutation of ICR or IMS -- causes, read-to-clear,
 * W1C, mask, unmask, reset -- must call this. */
static void e1000_intx_update(e1000_t *e) {
    int line = (e->regs[E1000_ICR / 4] & e->regs[E1000_IMS / 4]) != 0;
    if (line != e->intx) {
        e->intx = line;
        pic_set_irq(e->m, E1000_IRQ, line);
    }
}

/* ===================== U2: EEPROM (EERD) + PHY (MDIC) =====================
 * Measured from QEMU hw/net/e1000.c + e1000x_common.c (the N0 ground rule
 * pins this device to QEMU behaviour) with Intel's e1000x_regs.h constants.
 *
 * EEPROM: QEMU's e1000_eeprom_template with the device id 0x100E inserted
 * in words 11/13 and the MAC in words 0-2, closed by the word-0x3F
 * checksum that makes all 64 words sum to 0xBABA
 * (e1000x_core_prepare_eeprom).  Like QEMU it is prepared once at device
 * add, not rebuilt by CTRL.RST -- and nothing can write it: there is no
 * EEWR, and the EECD Microwire bit-bang stays an honest stub (the frozen
 * guest probes through EERD and never touches EECD).  An EERD write is a
 * plain store; the read completes the transaction synchronously
 * (flash_eerd_read): START set -> DONE with the word in bits 31:16, an
 * address past the checksum word 0x3F -> DONE with the stored data field,
 * START clear -> the stored value, so the guest's DONE poll retires on
 * its first iteration instead of timing out 100,000 times per word.
 *
 * PHY: the M88E1000 register file at MDIC PHY address 1, reset to QEMU's
 * phy_reg_init with PHYID2 = 0x0C20 (E1000_PHY_ID2_8254xx_DEFAULT, the
 * 82540EM row of QEMU's device table).  QEMU's link can be down, so it
 * reaches link-up through a 500 ms virtual autonegotiation timer; this
 * model's link is permanently up (STATUS.LU from reset, RX always live),
 * so the file is seeded at the steady state that timer settles into
 * (e1000x_update_regs_on_autoneg_done: BMSR.AN_COMP, ANLPAR.ACK) and a
 * BMCR.ANRESTART write completes synchronously -- there is no link flap
 * to observe, and none is fabricated.  MDIC mirrors set_mdic exactly:
 * READY is set by every store; ERROR for a wrong PHY address (which also
 * echoes the PREVIOUS MDIC value rather than the rejected command), for
 * a capability the register does not have (the phy_regcap table), and the
 * MDAC cause whenever INT_EN survives into the final value.  BMCR stores
 * through set_phy_ctrl's mask: reserved bits 5:0 and the RESET /
 * ANRESTART bits self-clear, and QEMU does not reset the register file
 * on BMCR.RESET, so neither do we. */
#define EERD_START        (1u << 0)
#define EERD_DONE         (1u << 4)   /* Intel E1000_EEPROM_RW_REG_DONE 0x10 */
#define MDIC_OP_WRITE     (1u << 26)
#define MDIC_OP_READ      (1u << 27)
#define MDIC_READY        (1u << 28)
#define MDIC_INT_EN       (1u << 29)
#define MDIC_ERROR        (1u << 30)

static const uint16_t e1000_eeprom_template[64] = {
    0x0000, 0x0000, 0x0000, 0x0000, 0xffff, 0x0000, 0x0000, 0x0000,
    0x3000, 0x1000, 0x6403, 0x0000, 0x8086, 0x0000, 0x8086, 0x3040,
    0x0008, 0x2000, 0x7e14, 0x0048, 0x1000, 0x00d8, 0x0000, 0x2700,
    0x6cc9, 0x3150, 0x0722, 0x040b, 0x0984, 0x0000, 0xc000, 0x0706,
    0x1008, 0x0000, 0x0f04, 0x7fff, 0x4d01, 0xffff, 0xffff, 0xffff,
    0xffff, 0xffff, 0xffff, 0xffff, 0xffff, 0xffff, 0xffff, 0xffff,
    0x0100, 0x4000, 0x121c, 0xffff, 0xffff, 0xffff, 0xffff, 0xffff,
    0xffff, 0xffff, 0xffff, 0xffff, 0xffff, 0xffff, 0xffff, 0x0000,
};

/* e1000x_core_prepare_eeprom for the one identity this model has: the QEMU
 * default MAC (the same bytes reset preloads into RAL/RAH -- the two must
 * never disagree, or the guest's address would depend on which source its
 * driver happened to ask first) and device id 0x100E. */
static void e1000_prepare_eeprom(void) {
    static const uint8_t mac[6] = { 0x52, 0x54, 0x00, 0x12, 0x34, 0x56 };
    memcpy(g_e1000.eeprom, e1000_eeprom_template,
           sizeof e1000_eeprom_template);
    for (int i = 0; i < 3; i++)
        g_e1000.eeprom[i] =
            (uint16_t)(mac[2 * i] | ((unsigned)mac[2 * i + 1] << 8));
    g_e1000.eeprom[11] = g_e1000.eeprom[13] = 0x100E;
    uint16_t sum = 0;
    for (int i = 0; i < 0x3F; i++)          /* EEPROM_CHECKSUM_REG */
        sum = (uint16_t)(sum + g_e1000.eeprom[i]);
    g_e1000.eeprom[0x3F] = (uint16_t)(0xBABA - sum);   /* EEPROM_SUM */
}

enum { PHY_R = 1, PHY_W = 2, PHY_RW = PHY_R | PHY_W };
static const uint8_t e1000_phy_cap[0x20] = {   /* QEMU phy_regcap */
    [0]  = PHY_RW,    /* BMCR                          */
    [1]  = PHY_R,     /* BMSR                          */
    [2]  = PHY_R,     /* PHYID1                        */
    [3]  = PHY_R,     /* PHYID2                        */
    [4]  = PHY_RW,    /* ANAR                          */
    [5]  = PHY_R,     /* ANLPAR                        */
    [6]  = PHY_R,     /* ANER                          */
    [9]  = PHY_RW,    /* CTRL1000                      */
    [10] = PHY_R,     /* STAT1000                      */
    [16] = PHY_RW,    /* M88E1000_PHY_SPEC_CTRL        */
    [17] = PHY_R,     /* M88E1000_PHY_SPEC_STATUS      */
    [20] = PHY_RW,    /* M88E1000_EXT_PHY_SPEC_CTRL    */
    [21] = PHY_R,     /* M88E1000_RX_ERR_CNTR          */
};

/* QEMU phy_reg_init at its autonegotiation-done steady state (see the
 * section comment): BMSR carries AN_COMP and ANLPAR carries ACK over the
 * raw init values, everything else is the init table verbatim. */
static const uint16_t e1000_phy_init[0x20] = {
    [0]  = 0x1140,    /* BMCR: SPEED1000 | FD | AUTOEN                 */
    [1]  = 0x796D,    /* BMSR: 10/100 caps | EXTSTAT | MFPS | AN_COMP
                       *       | AUTONEG | LINK_ST | EXTCAP            */
    [2]  = 0x0141,    /* PHYID1                                        */
    [3]  = 0x0C20,    /* PHYID2: E1000_PHY_ID2_8254xx_DEFAULT          */
    [4]  = 0x0DE1,    /* ANAR: pause(+asym) | 100/10 FD+HD | CSMA/CD   */
    [5]  = 0x41E0,    /* ANLPAR: ACK | 100/10 FD+HD                    */
    [9]  = 0x0E00,    /* CTRL1000: MASTER | PORT(DTE) | FULL           */
    [10] = 0x3C00,    /* STAT1000: LOK | ROK | FULL | HALF             */
    [16] = 0x0360,    /* M88E1000_PHY_SPEC_CTRL                        */
    [17] = 0xAC00,    /* M88E1000_PHY_SPEC_STATUS                      */
    [20] = 0x0D60,    /* M88E1000_EXT_PHY_SPEC_CTRL                    */
};

/* flash_eerd_read: the transaction completes on the read itself.  QEMU
 * shifts the stored command right by 8 unmasked (data bits a guest wrote
 * into 31:16 become part of the index and simply land out of range) and
 * ORs the result over the stored command minus START; both quirks are
 * kept so a guest sees QEMU's register, not an idealised one. */
static uint32_t e1000_eerd_read(e1000_t *e) {
    uint32_t v = e->regs[E1000_EERD / 4];
    if (!(v & EERD_START))
        return v;
    uint32_t r = v & ~EERD_START;
    unsigned index = r >> 8;                    /* E1000_EEPROM_RW_ADDR_SHIFT */
    if (index > 0x3Fu)                          /* EEPROM_CHECKSUM_REG */
        return r | EERD_DONE;
    return ((uint32_t)e->eeprom[index] << 16) | EERD_DONE | r;
}

/* set_mdic, branch for branch (see the section comment).  The QEMU op
 * tests are bit tests in read-then-write order, so OP=2'b11 reads and
 * OP=2'b00 stores the command untouched apart from READY. */
static void e1000_set_mdic(e1000_t *e, uint32_t val) {
    uint32_t data = val & 0xFFFFu;
    unsigned reg = (val >> 16) & 0x1Fu;         /* MDIC_REG */
    if (((val >> 21) & 0x1Fu) != 1) {           /* MDIC_PHY: the PHY is #1 */
        val = e->regs[E1000_MDIC / 4] | MDIC_ERROR;
    } else if (val & MDIC_OP_READ) {
        if (!(e1000_phy_cap[reg] & PHY_R))
            val |= MDIC_ERROR;
        else
            val = (val ^ data) | e->phy[reg];
    } else if (val & MDIC_OP_WRITE) {
        if (!(e1000_phy_cap[reg] & PHY_W)) {
            val |= MDIC_ERROR;
        } else if (reg == 0) {
            /* set_phy_ctrl: bits 5:0 reserved, RESET (15) and ANRESTART
             * (9) self-clearing.  QEMU would restart its negotiation
             * timer on ANRESTART here; this link never goes down, so the
             * completed state above already IS the restart's outcome. */
            e->phy[0] = (uint16_t)(data & ~(0x3Fu | (1u << 15) | (1u << 9)));
        } else {
            e->phy[reg] = (uint16_t)data;
        }
    }
    e->regs[E1000_MDIC / 4] = val | MDIC_READY;
    if (val & MDIC_INT_EN) {                    /* QEMU: set_ics(MDAC) */
        e->regs[E1000_ICR / 4] |= ICR_MDAC;
        e->regs[E1000_ICS / 4] |= ICR_MDAC;
        e1000_intx_update(e);
    }
}

static void e1000_reset_regs(void) {
    memset(g_e1000.regs, 0, sizeof g_e1000.regs);
    /* N1: the TX assembly state and the wire counters are device state, so a
     * reset (device add, or CTRL.RST) starts them clean -- a half-assembled
     * frame must not survive into the next ring. */
    g_e1000.tx_size = 0;
    g_e1000.tx_busy = 0;
    g_e1000.tx_frames = 0;
    g_e1000.tx_dropped = 0;
    /* U2: QEMU's e1000_reset reseeds the PHY file; the EEPROM image is
     * add-time state and survives (e1000_prepare_eeprom). */
    memcpy(g_e1000.phy, e1000_phy_init, sizeof e1000_phy_init);
    g_e1000.regs[E1000_STATUS / 4] = STATUS_LU;    /* link up on attach */
    /* 52:54:00:12:34:56 -> RAL = 0x12005452, RAH = AV | 0x5634 */
    g_e1000.regs[E1000_RA / 4] = 0x12005452u;
    g_e1000.regs[(E1000_RA + 4) / 4] = RAH_AV | 0x5634u;
    e1000_intx_update(&g_e1000);   /* N4: reset drops the INTx line */
}

/* ============================ TX ring egress ============================== */
/* N1.  Measured from QEMU hw/net/e1000.c:
 *
 *   set_tctl()   -- BOTH [TCTL] and [TDT] map to it: it stores the value,
 *                   masks TDT to 16 bits, then calls start_xmit().  So the
 *                   doorbell is a TDT *or* a TCTL write (the guest writes
 *                   TCTL once at init with TDH==TDT, which is a no-op walk).
 *   start_xmit() -- returns immediately unless TCTL.EN; guards re-entrancy
 *                   with tx.busy; walks TDH != TDT; per descriptor calls
 *                   process_tx_desc() then txdesc_writeback(); advances TDH
 *                   with "++TDH * 16 >= TDLEN -> TDH = 0"; breaks on the
 *                   TDH wraparound and on a bogus TDLEN ("nothing too
 *                   intelligent we could do about this"); finally raises
 *                   ICS_TXQE unconditionally ORed with the TXDW causes the
 *                   writebacks returned.
 *   process_tx_desc() -- legacy path: split_size = lower.data & 0xffff
 *                   (the length field), clamped to the remaining tx.data
 *                   room, DMA-read at buffer_addr into tx.data+tx.size.
 *                   CMD.EOP absent -> accumulate only, the frame is not on
 *                   the wire yet; CMD.EOP -> xmit_seg() (send) and reset the
 *                   assembly state.  Context (CMD.DEXT) and TSO descriptors
 *                   are out of scope: the guest only ever submits legacy
 *                   cmd 0x0B.
 *   txdesc_writeback() -- called by start_xmit for EVERY descriptor of the
 *                   walk, EOP or not (it is a separate call, not part of
 *                   process_tx_desc), so a mid-frame RS descriptor is
 *                   completed too.  Returns 0 unless CMD.RS; otherwise
 *                   upper.data = (upper.data | DD) & ~(EC|LC|TU), written
 *                   back to the descriptor, and returns ICS_TXDW.  Note QEMU
 *                   does NOT clear CMD here.
 *
 * The guest contract this satisfies (drivers/e1000/e1000.c e1000_send):
 * memcpy into tx_buffers[tail], length=len, cmd=0x0B (EOP|IFCS|RS),
 * status=0, TDT=tail+1, then spin <=100000 PAUSEs on status&0x01 -- so DD
 * must be written back *inside* the TDT store, not on a later tick.
 */
static void e1000_send_frame(e1000_t *e) {
    if (e->tx_size == 0) return;
    if (e->tap_fd < 0) {
        /* No host endpoint: the frame is dropped but the descriptors still
         * complete, exactly like QEMU's NIC with a null backend (and like
         * the socketless unit lane, which asserts DD/TDH, not the wire). */
        e->tx_dropped++;
        return;
    }
    ssize_t n = send(e->tap_fd, e->tx_data, e->tx_size, 0);
    if (n < 0) {
        /* UDP to a connected endpoint: ECONNREFUSED arrives when nothing is
         * bound at 127.0.0.1:8080.  Drop, do not fail the descriptor -- the
         * guest's TX path must not depend on the host responder being up
         * (that is N2's lane, and QEMU behaves the same way). */
        e->tx_dropped++;
        return;
    }
    e->tx_frames++;
}

/* One descriptor of the walk.  Returns the ICR cause bits its writeback
 * produced (QEMU: `cause |= txdesc_writeback(...)`). */
static uint32_t e1000_tx_desc(machine_t *m, uint64_t desc) {
    e1000_t *e = &g_e1000;
    uint64_t addr = (uint64_t)mem_read(m, desc, 4)
                  | ((uint64_t)mem_read(m, desc + 4, 4) << 32);
    uint32_t lower = (uint32_t)mem_read(m, desc + 8, 4);
    uint32_t upper = (uint32_t)mem_read(m, desc + 12, 4);
    uint32_t split = lower & 0xFFFFu;               /* legacy: length field */

    if (!(lower & TXD_CMD_DEXT)) {                  /* legacy descriptor */
        uint32_t room = E1000_TX_DATA_MAX - e->tx_size;
        if (split > room) split = room;             /* QEMU: MIN(sizeof data
                                                     * - size, split_size)  */
        for (uint32_t i = 0; i < split; i++)
            e->tx_data[e->tx_size + i] = (uint8_t)mem_read(m, addr + i, 1);
        e->tx_size += split;
    }

    if (lower & TXD_CMD_EOP) {
        e1000_send_frame(e);
        e->tx_size = 0;
    }

    /* QEMU calls txdesc_writeback() for EVERY descriptor of the walk, EOP or
     * not: a mid-frame descriptor with CMD.RS is completed just the same, so
     * the guest can reuse it as soon as the frame it belongs to goes out. */
    if (!(lower & TXD_CMD_RS))
        return 0;                                   /* QEMU: no writeback */
    upper = (upper | TXD_STAT_DD) & ~(TXD_STAT_EC | TXD_STAT_LC | TXD_STAT_TU);
    mem_write(m, desc + 12, 4, upper);
    return ICR_TXDW;
}

static void e1000_start_xmit(machine_t *m) {
    e1000_t *e = &g_e1000;
    if (!(e->regs[E1000_TCTL / 4] & TCTL_EN))
        return;                                     /* "tx disabled" */
    if (e->tx_busy) return;                         /* QEMU re-entrancy guard */
    e->tx_busy = 1;

    uint32_t cause = ICR_TXQE;                      /* always raised */
    uint32_t tdh_start = e->regs[E1000_TDH / 4];
    uint64_t base = (uint64_t)e->regs[E1000_TDBAL / 4]
                  | ((uint64_t)e->regs[E1000_TDBAH / 4] << 32);

    while (e->regs[E1000_TDH / 4] != e->regs[E1000_TDT / 4]) {
        uint32_t tdh = e->regs[E1000_TDH / 4];
        cause |= e1000_tx_desc(m, base + (uint64_t)tdh * E1000_TX_DESC_SZ);
        if (++e->regs[E1000_TDH / 4] * E1000_TX_DESC_SZ >= e->regs[E1000_TDLEN / 4])
            e->regs[E1000_TDH / 4] = 0;
        /* QEMU's two escape hatches for a guest that programs bogus
         * TDT/TDLEN: the head came back round to where it started, or the
         * start was already past the ring.  Without them a TDH==0/TDT==32
         * ring with TDLEN==0 would spin forever. */
        if (e->regs[E1000_TDH / 4] == tdh_start ||
            tdh_start >= e->regs[E1000_TDLEN / 4] / E1000_TX_DESC_SZ)
            break;
    }

    e->tx_busy = 0;
    /* set_ics(): the cause bits land in ICR (and ICS); the INTx line (N4,
     * ledger D4) follows the plan's `(ICR & IMS) != 0` condition -- the
     * bookkeeping-only era of N1 is over. */
    e->regs[E1000_ICR / 4] |= cause;
    e1000_intx_update(e);
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
        e1000_intx_update(e);               /* N4: clearing the cause drops the line */
        return v;
    }
    if (off == E1000_EERD)                  /* U2: completes on the read */
        return e1000_eerd_read(e);
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
    /* N1: QEMU write-op parity for the interrupt and ring registers.  The
     * N0 fallback stored every one of these verbatim, which measured wrong
     * against hw/net/e1000.c macreg_writeops[]: IMS is set_ims (OR, not
     * assign), ICR is set_icr (clear the written bits, not a no-op), ICS is
     * set_ics (OR into the cause), TDH/RDH are set_16bit, TDLEN/RDLEN are
     * set_dlen (val & 0xfff80), and TDT shares TCTL's op -- store, mask TDT
     * to 16 bits, then walk.  No N0 vector pinned the old behaviour. */
    switch (off) {
    case E1000_ICS:
        /* QEMU set_ics -> set_interrupt_cause: the written causes join ICR
         * (and ICS).  ICS is write-only on this part, so only ICR matters
         * to the guest; both are updated to keep the register file honest. */
        e->regs[E1000_ICR / 4] |= (uint32_t)val;
        e->regs[E1000_ICS / 4] |= (uint32_t)val;
        e1000_intx_update(e);
        return;
    case E1000_ICR:
        e->regs[E1000_ICR / 4] &= ~(uint32_t)val;
        e1000_intx_update(e);
        return;
    case E1000_IMS:
        e->regs[E1000_IMS / 4] |= (uint32_t)val;
        e1000_intx_update(e);
        return;
    case E1000_IMC:
        e->regs[E1000_IMS / 4] &= ~(uint32_t)val;
        e1000_intx_update(e);
        return;
    case E1000_TDLEN: case E1000_RDLEN:
        e->regs[off / 4] = (uint32_t)val & E1000_DLEN_MASK;
        return;
    case E1000_TDH: case E1000_RDH: case E1000_TDT:
        e->regs[off / 4] = (uint32_t)val & 0xFFFFu;
        if (off == E1000_TDT) e1000_start_xmit(e->m);   /* the doorbell */
        return;
    case E1000_TCTL:
        e->regs[E1000_TCTL / 4] = (uint32_t)val;
        e->regs[E1000_TDT / 4] &= 0xFFFFu;              /* QEMU set_tctl */
        e1000_start_xmit(e->m);
        return;
    case E1000_MDIC:
        e1000_set_mdic(e, (uint32_t)val);               /* U2: QEMU set_mdic */
        return;
    default:
        break;
    }
    /* RAL/RAH, rings, RCTL, ... and EERD (U2: a plain store like QEMU's
     * putreg -- the EEPROM transaction completes on the read). */
    e->regs[off / 4] = (uint32_t)val;
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
    e1000_intx_update(&g_e1000);                   /* N4: RX completion raises INTx */
}

/* ===================== device model + TAP attach ========================== */
static void e1000_add(machine_t *m) {
    g_e1000.m = m;
    g_e1000.attached = 1;
    e1000_prepare_eeprom();   /* U2: QEMU prepares the EEPROM at realize */
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
    /* N4 (ledger D4): publish the interrupt route -- pin INTA# and the ISA
     * line the board wire above drives, so a guest that routes interrupts
     * reads the truth instead of "IRQ line 0". */
    e_pci->cfg[0x3C] = (uint8_t)E1000_IRQ;    /* Interrupt Line */
    e_pci->cfg[0x3D] = 0x01;                  /* Interrupt Pin: INTA# */
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

/* N1 observability for the integration lane: the wire counters.  The device
 * logs nothing on the RX or TX path (parity with N0), so a harness that has
 * to prove "the frame reached the wire" reads these instead of grepping a
 * log whose line count would otherwise depend on how many frames the host
 * responder happened to answer with -- which would cost the lane its
 * byte-identical determinism pair. */
void e1000_tx_stats(uint64_t *sent, uint64_t *dropped) {
    if (sent)    *sent    = g_e1000.tx_frames;
    if (dropped) *dropped = g_e1000.tx_dropped;
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
