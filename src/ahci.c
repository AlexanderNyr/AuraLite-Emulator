// src/ahci.c -- STORE plan S1+: AHCI HBA register model.
//
// Contract measured from the guest driver (AuraLite-OS/drivers/ahci/ahci.c):
// BAR5 -> 8 KiB map; CAP.NP (bits 4:0, 0-based) and GHC.AE (bit 31) are the
// only global fields consumed; per port at 0x100+0x80*p the guest reads
// PxSSTS.DET (==3 attached, ==0 empty, ==1 -> COMRESET via PxSCTL), requires
// PxSIG == 0x00000101 to accept a disk, programs PxCLB/CLBU + PxFB/FBU,
// drives PxCMD ST/FRE and spins on CR/FR, W1C-clears PxIS/PxSERR, keeps
// PxIE = 0 (polled completion), and issues commands through PxCI.
//
// S4: the model is per-controller -- ahci_register_ctrl() brings up ctrl
// < AHCI_CTRLS, each with its own ABAR window, port files and attach row
// (machine_t.sata_img[ctrl][port]).  Controller 0 is the onboard 0:31:2
// function; controller 1 appears under --ahci2 at 0:31:3 (the guest's
// class scan is bus-0 dev/func ascending -- measured -- so 0:31:3 binds
// second and "controller 0 at PCI 0:31.2" stays the S1-S3 receipt).
// Guest-visible log lines: controller 0 keeps the historical
// "[ahci] port %d" spelling; controller 1 is tagged "[ahci2]".

#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "ahci.h"

/* ---- register state ---- */

#define AHCI_CAP_VAL  (0x80040005u)      /* S64A(31) | SAM(18) | NP=6 (0-based: 5) */
#define AHCI_PI_VAL   ((1u << AHCI_HW_PORTS) - 1u)
#define AHCI_VS_VAL   0x00010300u        /* AHCI 1.3 */

/* PxCMD bits the guest drives. */
#define PXCMD_ST   (1u << 0)
#define PXCMD_FRE  (1u << 4)
#define PXCMD_FR   (1u << 14)
#define PXCMD_CR   (1u << 15)

#define SATA_SIG_ATA  0x00000101u
#define SATA_SIG_NONE 0xFFFFFFFFu

/* Present ports: device idle-ready (DRDY set, BSY/DRQ clear) -- the exact
 * 0x88 mask the guest polls must be 0 for it to issue a command.  Absent
 * ports hold BSY+SECTOR bits like real dark silicon (driver never gets
 * there: DET==0 short-circuits enumeration). */
#define TFD_ATTACHED  0x40u
#define TFD_DARK      0x7Fu

/* Present ports: SPD=1 (Gen1), IPM=1 (active), DET=3 (established). */
#define SSTS_ATTACHED 0x00000123u
#define SSTS_DARK     0x00000000u

typedef struct {
    uint32_t clb, clbu, fb, fbu;   /* guest-programmed DMA descriptors */
    uint32_t is, ie;               /* PxIS holds latched bits (W1C); PxIE stored */
    uint32_t cmd;                  /* guest-written ST/FRE + other bits */
    uint32_t sctl;                 /* DET pulses drive COMRESET */
    uint32_t serr;                 /* W1C diagnostic bits */
    uint32_t ci;                   /* command-issue latch, engine clears */
    uint32_t tfd_err;              /* sticky ERR until the next good command */
} port_regs_t;

typedef struct {
    machine_t   *m;
    int          ctrl;             /* S4: controller index (0..AHCI_CTRLS-1) */
    uint32_t     ghc;
    uint32_t     is;               /* controller HBA IS (aggregates PxIS, S5) */
    int          intx;             /* S5: current board-line assertion state */
    port_regs_t  port[AHCI_HW_PORTS];
} ahci_t;

static ahci_t g_ahci[AHCI_CTRLS];

/* S5: --sata-writethrough host files, one per attach slot (see ahci.h). */
static FILE *g_wt[AHCI_CTRLS][AHCI_MAX_ATTACH];

int ahci_wt_attach(machine_t *m, int ctrl, int port, const char *path) {
    (void)m;
    if (ctrl < 0 || ctrl >= AHCI_CTRLS || port < 0 || port >= AHCI_MAX_ATTACH)
        return -1;
    if (g_wt[ctrl][port]) fclose(g_wt[ctrl][port]);
    g_wt[ctrl][port] = fopen(path, "r+b");      /* existing image, read/write */
    return g_wt[ctrl][port] ? 0 : -1;
}

void ahci_wt_close_all(void) {
    for (int c = 0; c < AHCI_CTRLS; c++)
        for (int p = 0; p < AHCI_MAX_ATTACH; p++) {
            if (g_wt[c][p]) { fclose(g_wt[c][p]); g_wt[c][p] = NULL; }
        }
}

/* S5: honest INTx.  AHCI 1.3 interrupt logic: the signal is generated when
 * (HBA IS != 0 and GHC.IE) or any (PxIS & PxIE) is nonzero.  HBA IS is
 * kept as a live per-port aggregation of "PxIS != 0" (W1C on HBA IS is
 * accepted, but the mirror re-derives at the next event -- which is what
 * the spec's "clear PxIS to clear the bit" path does anyway).  The board
 * wire is pic_set_irq(AHCI_IRQ0 + ctrl, asserted) -- assert = level 1 in
 * the pic_set_irq convention (H2-H4 devices drive the same way); the fan-out
 * to the I/O APIC happens inside pic_set_irq (CHIPSET H7 board wire). */
static void intx_update(ahci_t *a) {
    int intr = 0;
    for (int p = 0; p < AHCI_HW_PORTS; p++) {
        if (a->port[p].is) a->is |= 1u << p;
        else               a->is &= ~(1u << p);
        if ((a->port[p].is & a->port[p].ie) != 0) intr = 1;
    }
    if (a->is && (a->ghc & 2u)) intr = 1;   /* HBA IS & GHC.IE */
    if (intr != a->intx) {
        a->intx = intr;
        pic_set_irq(a->m, AHCI_IRQ0 + a->ctrl, intr);
    }
}

static int port_attached(const ahci_t *a, int p) {
    return p < AHCI_MAX_ATTACH && a->m->sata_img[a->ctrl][p] != NULL;
}

static int port_of(uint64_t off) {
    if (off < 0x100 || off >= 0x100u + AHCI_HW_PORTS * 0x80u) return -1;
    return (int)((off - 0x100u) / 0x80u);
}

/* S4: the log tag pins which controller a line belongs to; controller 0
 * keeps the exact historical spelling so no S1-S3 receipt greps change. */
static const char *tagc(const ahci_t *a) { return a->ctrl ? "[ahci2]" : "[ahci]"; }

/* ---- S2 command engine: FIS decode + ATA READ DMA EXT / IDENTIFY ----
 *
 * Contract measured from the guest driver: it builds ONE command header
 * (CFL=5, W flag for writes, PRDTL in bits16..31), points CTBA at a command
 * table holding an H2D Register FIS (type 0x27, device 0x40 LBA48) and a
 * single PRDT entry (dbc = bytes-1).  It writes PxCI=1, then polls until the
 * machine clears it; PxIS.TFES (bit 30) + PxTFD.ERR are the error channel.
 * DMA here means host-memory copies between the attached image and guest
 * physical RAM, executed synchronously inside the PxCI write -- fast
 * hardware: BSY is never observable.
 *
 * S2 scope: READ DMA EXT (0x25) and IDENTIFY DEVICE (0xEC).  WRITE DMA EXT
 * (0x35) takes the TFES error path until S3 -- the guest's own boot
 * receipts isolate that cleanly (format attempts report write failures and
 * boot continues).
 */

#define ATA_CMD_READ_DMA_EXT   0x25
#define ATA_CMD_WRITE_DMA_EXT  0x35
#define ATA_CMD_IDENTIFY       0xEC

static uint32_t gdr(machine_t *m, uint64_t pa)          { return (uint32_t)mem_read(m, pa, 4); }
static void     gdw(machine_t *m, uint64_t pa, uint32_t v) { mem_write(m, pa, 4, v); }

static uint64_t fis_u64(const uint8_t *f, int i) {
    return ((uint64_t)f[4 + i] & 0xFF);
}

/* Sector DMA between the image and guest RAM.  w=0 device->guest
 * (READ), w=1 guest->device (WRITE); S5 decides host write-through
 * (today the image is in-memory, the USB stick's own convention). */
static int ahci_dma_xfer(ahci_t *a, int p, uint64_t lba, uint32_t bytes,
                         uint32_t prdtl, uint64_t ct, int w) {
    machine_t *m = a->m;
    uint8_t *img = m->sata_img[a->ctrl][p];
    uint64_t off = lba * 512ull;
    if (lba > UINT64_MAX / 512 || off + bytes > m->sata_len[a->ctrl][p]) return -1; /* OOB: TFES */
    uint64_t remain = bytes;
    for (uint32_t i = 0; i < prdtl && remain; i++) {
        uint64_t dba  = gdr(m, ct + 0x80 + i * 16)
                     | ((uint64_t)gdr(m, ct + 0x84 + i * 16) << 32);
        uint32_t dbc  = (gdr(m, ct + 0x8C + i * 16) & 0x003FFFFFu) + 1u;
        if (dbc > remain) dbc = remain;
        if (w) {
            for (uint32_t k = 0; k < dbc; k++)
                img[off + k] = (uint8_t)mem_read(m, dba + k, 1);
            /* S5: --sata-writethrough -- the same bytes land in the host
             * image file at the same offset (policy otherwise stays the
             * S3 copy-on-attach). */
            if (g_wt[a->ctrl][p]) {
                FILE *fp = g_wt[a->ctrl][p];
                if (fseek(fp, (long)off, SEEK_SET) == 0)
                    fwrite(img + off, 1, dbc, fp);
            }
        } else
            for (uint32_t k = 0; k < dbc; k++)
                mem_write(m, dba + k, 1, img[off + k]);
        off += dbc; remain -= dbc;
    }
    return (int)(bytes - remain);
}

/* IDENTIFY DEVICE: the standard 512-byte block, machine-honest fields. */
static void ahci_identify(ahci_t *a, int p, uint32_t prdtl, uint64_t ct) {
    machine_t *m = a->m;
    static uint8_t idb[512];
    memset(idb, 0, sizeof idb);
    idb[0] = 0x40;                                  /* word0: non-removable */
    /* ATA model string (words 27..46) is byte-pair swapped. */
    static const char model[] = "AuraLite EmuDisk HBA   ";
    for (int w = 27; w <= 46 && (size_t)(w - 27) * 2 < sizeof(model); w++) {
        idb[w * 2]     = model[(w - 27) * 2];
        idb[w * 2 + 1] = model[(w - 27) * 2 + 1];
    }
    idb[49 * 2] = 0x00; idb[49 * 2 + 1] = 0x03;     /* word49: LBA + DMA supported */
    uint32_t secs28 = (uint32_t)(m->sata_len[a->ctrl][p] / 512);
    if (secs28 > 0x0FFFFFFF) secs28 = 0x0FFFFFFF;   /* LBA28 ceiling */
    idb[60 * 2] = secs28 & 0xFF; idb[60 * 2 + 1] = (secs28 >> 8) & 0xFF;
    idb[61 * 2] = (secs28 >> 16) & 0xFF; idb[61 * 2 + 1] = (secs28 >> 24) & 0xFF;
    idb[83 * 2] = 0x00; idb[83 * 2 + 1] = 0x04;     /* word83 bit10: LBA48 */
    uint64_t secs48 = m->sata_len[a->ctrl][p] / 512;
    for (int i = 0; i < 4; i++) {
        idb[(100 + i) * 2]     = (secs48 >> (16 * i)) & 0xFF;
        idb[(100 + i) * 2 + 1] = (secs48 >> (16 * i + 8)) & 0xFF;
    }
    for (uint32_t i = 0; i < prdtl; i++) {
        uint64_t dba = gdr(m, ct + 0x80 + i * 16)
                    | ((uint64_t)gdr(m, ct + 0x84 + i * 16) << 32);
        uint32_t dbc = (gdr(m, ct + 0x8C + i * 16) & 0x003FFFFFu) + 1u;
        for (uint32_t k = 0; k < dbc && k < 512; k++)
            mem_write(m, dba + k, 1, idb[k]);
        break;
    }
}

/* Execute whatever the guest latched into PxCI for port p. */
static void ahci_engine_port(ahci_t *a, int p) {
    machine_t *m = a->m;
    port_regs_t *r = &a->port[p];
    if (!r->ci) return;
    uint64_t clb = (uint64_t)r->clb | ((uint64_t)r->clbu << 32);
    for (int s = 0; s < 32 && r->ci; s++) {
        if (!(r->ci & (1u << s))) continue;
        uint64_t hdr = clb + (uint64_t)s * 32;
        uint32_t dw0 = gdr(m, hdr);
        uint32_t prdtl = (dw0 >> 16) & 0xFFFFu;
        int      wr    = (dw0 >> 6) & 1;
        if (((dw0 >> 0) & 0x1Fu) != 5 || prdtl == 0) { goto tfes; } /* CFL!=5 / no PRDT */
        uint64_t ct = (uint64_t)gdr(m, hdr + 8) | ((uint64_t)gdr(m, hdr + 12) << 32);

        uint8_t cfis[20];
        for (size_t i = 0; i < sizeof cfis; i++) cfis[i] = (uint8_t)mem_read(m, ct + i, 1);
        if (cfis[0] != 0x27 || !(cfis[1] & 0x80)) goto tfes;        /* H2D register FIS */

        uint8_t  cmd  = cfis[2];
        /* H2D Register FIS: lba0..2 at [4..6], LBA48 high half at [8..10]. */
        uint64_t lba  = fis_u64(cfis, 0) | (fis_u64(cfis, 1) << 8) | (fis_u64(cfis, 2) << 16)
                      | (fis_u64(cfis, 4) << 24) | (fis_u64(cfis, 5) << 32) | (fis_u64(cfis, 6) << 40);
        uint32_t cnt  = ((uint32_t)cfis[12] | ((uint32_t)cfis[13] << 8));
        if (cnt == 0) cnt = 65536;
        int done = 0;
        switch (cmd) {
        case ATA_CMD_READ_DMA_EXT:
        case ATA_CMD_WRITE_DMA_EXT: {
            /* S3: WRITE DMA EXT admitted; guest data travels PRDT -> image. */
            done = ahci_dma_xfer(a, p, lba, cnt * 512u, prdtl, ct, wr);
            if (done < 0) goto tfes;
            break;
        }
        case ATA_CMD_IDENTIFY: {
            if (wr) goto tfes;
            ahci_identify(a, p, prdtl, ct);
            done = 512;
            break;
        }
        default:
            mlog(&m->log, "%s port %d: unsupported ATA cmd 0x%02x (S3 scope)\n", tagc(a), p, cmd);
            goto tfes;
        }
        gdw(m, hdr + 4, (uint32_t)done);       /* prdbc: bytes transferred */
        r->ci &= ~(1u << s);                    /* slot complete */
        r->is |= 1u;                            /* DHRS: FIS D2H received */
        intx_update(a);                         /* S5: IS change -> line */
        continue;
    tfes:
        gdw(m, hdr + 4, 0);
        r->ci &= ~(1u << s);
        r->is |= (1u << 30);                    /* TFES */
        r->tfd_err = 1;
        intx_update(a);                         /* S5 */
    }
}

static uint64_t port_read(ahci_t *a, int p, uint32_t sub) {
    port_regs_t *r = &a->port[p];
    switch (sub) {
    case 0x00: return r->clb;
    case 0x04: return r->clbu;
    case 0x08: return r->fb;
    case 0x0C: return r->fbu;
    case 0x10: return r->is;
    case 0x14: return r->ie;
    case 0x18: {
        /* PxCMD: CR mirrors ST and FR mirrors FRE immediately -- real
         * silicon clears them asynchronously; the guest's stop/start spin
         * loops accept the already-stable value. */
        uint32_t v = r->cmd;
        if (v & PXCMD_ST)  v |= PXCMD_CR; else v &= ~PXCMD_CR;
        if (v & PXCMD_FRE) v |= PXCMD_FR; else v &= ~PXCMD_FR;
        return v;
    }
    case 0x1C: return 0;                     /* reserved */
    case 0x20: {
        uint32_t v = port_attached(a, p) ? TFD_ATTACHED : TFD_DARK;
        if (r->tfd_err) v |= 1;              /* ERR sticky until next ok */
        return v;
    }
    case 0x24: return port_attached(a, p) ? SATA_SIG_ATA : SATA_SIG_NONE;
    case 0x28: return port_attached(a, p) ? SSTS_ATTACHED : SSTS_DARK;
    case 0x2C: return r->sctl;
    case 0x30: return r->serr;
    case 0x34: return 0;                     /* PxSACT: no queued commands */
    case 0x38: return r->ci;
    default:   return 0;
    }
}

static void port_write(ahci_t *a, int p, uint32_t sub, uint32_t v) {
    machine_t *m = a->m;
    port_regs_t *r = &a->port[p];
    switch (sub) {
    case 0x00: r->clb  = v; break;
    case 0x04: r->clbu = v; break;
    case 0x08: r->fb   = v; break;
    case 0x0C: r->fbu  = v; break;
    case 0x10: r->is  &= ~v; intx_update(a); break;  /* W1C (S5: line follows) */
    case 0x14: r->ie   = v; intx_update(a); break;   /* S5: unmask can assert */
    case 0x18:
        /* Preserve guest-writable flag bits wholesale; CR/FR are derived. */
        r->cmd = v & (PXCMD_ST | PXCMD_FRE | 0x0FF00000u);
        break;
    case 0x2C: {
        /* PxSCTL.DET COMRESET: the guest writes DET=1, holds >= 1 ms, then
         * DET=0 and polls PxSSTS.DET==3.  An attached disk renegotiates
         * instantly in this model; an absent one stays dark. */
        r->sctl = v & 0x0Fu;
        mlog(&m->log, "%s port %d: COMRESET %s\n", tagc(a), p,
             (v & 0xF) == 1 ? "initiate" : "release");
        break;
    }
    case 0x30: r->serr &= ~v; break;         /* W1C */
    case 0x38:
        r->tfd_err = 0;                     /* fresh issue attempt clears sticky ERR */
        r->ci |= v;
        ahci_engine_port(a, p);             /* S2: instant DMA, clears bits */
        break;
    default: break;
    }
}

static uint64_t abar_read(void *ctx, uint64_t addr, int size) {
    ahci_t *a = ctx;
    uint64_t off = addr - (a->ctrl ? AHCI_ABAR2 : AHCI_ABAR);
    if (size != 4) {
        /* The guest reads 32-bit only (dword array indexing).  Assemble
         * byte/other sizes from the containing dword for robustness. */
        uint64_t dw = abar_read(ctx, addr & ~3ull, 4);
        uint64_t in = addr & 3ull;
        if ((uint64_t)size + in <= 4) return (dw >> (8 * in)) & ((1ull << (8 * size)) - 1);
        return 0;
    }
    if (off < 0x100) {
        switch (off) {
        case 0x00: return AHCI_CAP_VAL;
        case 0x04: return a->ghc;
        case 0x08: return a->is;
        case 0x0C: return AHCI_PI_VAL;
        case 0x10: return AHCI_VS_VAL;
        default:   return 0;
        }
    }
    int p = port_of(off);
    if (p < 0) return 0;
    return port_read(a, p, (uint32_t)(off & 0x7Fu));
}

static void abar_write(void *ctx, uint64_t addr, int size, uint64_t val) {
    ahci_t *a = ctx;
    uint64_t off = addr - (a->ctrl ? AHCI_ABAR2 : AHCI_ABAR);
    if (size != 4) {
        uint32_t old = (uint32_t)abar_read(ctx, addr & ~3ull, 4);
        uint64_t in = addr & 3ull, mask = 0;
        for (int i = 0; i < size; i++) mask |= 0xFFull << (8 * (in + i));
        val = (old & ~mask) | ((val << (8 * in)) & mask);
        addr &= ~3ull; off = addr - (a->ctrl ? AHCI_ABAR2 : AHCI_ABAR);
    }
    if (off < 0x100) {
        switch (off) {
        case 0x04: a->ghc  = val; intx_update(a); break; /* AE + HR + IE; IE feeds INTx (S5) */
        case 0x08: a->is  &= ~val; break;   /* HBA IS is W1C (mirror re-derives) */
        default: break;                        /* CAP/PI/VS fixed */
        }
        return;
    }
    int p = port_of(off);
    if (p >= 0) port_write(a, p, (uint32_t)(off & 0x7Fu), (uint32_t)val);
}

void ahci_register_ctrl(machine_t *m, int ctrl) {
    if (ctrl < 0 || ctrl >= AHCI_CTRLS) return;
    ahci_t *a = &g_ahci[ctrl];
    memset(a, 0, sizeof *a);
    a->m = m;
    a->ctrl = ctrl;
    uint32_t base = ctrl ? AHCI_ABAR2 : AHCI_ABAR;
    mem_register_mmio(m, base, AHCI_ABAR_SIZE, abar_read, abar_write,
                      a, ctrl ? "AHCI ABAR2" : "AHCI ABAR");
    mlog(&m->log, "%s ABAR registered at 0x%x (%u ports, S1: registers+presence)\n",
         tagc(a), base, AHCI_HW_PORTS);
}

void ahci_register(machine_t *m) {
    ahci_register_ctrl(m, 0);
}
