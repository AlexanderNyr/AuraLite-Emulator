// devices.c -- "the rest of the chipset": a generic RAM-backed MMIO helper
// used for CAR / GPU VRAM / DDR-controller register windows, PCI device
// stubs placed at the same bus:dev:func the firmware itself probes (confirmed by
// disassembly: LPC=0:31:0, SATA/AHCI=0:31:2, EHCI=0:3:0, GPU=0:2:0), a
// behavioural EHCI controller that walks real qTD/QH chains to perform USB
// Mass-Storage bulk transfers against a backing disk image, and small I/O
// sinks for CMOS/Super I/O ports the firmware pokes but never reads back.
#include <stdlib.h>
#include <string.h>
#include "machine.h"
#include "pci.h"
#include "platform.h"
#include "devices.h"
#include "cpu.h"    /* USB U3: cpu_devices_tick hook */
#include "serial.h"
#include "ahci.h"

/* ============================ generic RAM window ========================== */
typedef struct { uint8_t *buf; uint64_t base, size; uint32_t clear_bits_on_read; const char *name; } rw2_t;

static uint64_t rw2_read(void *ctx, uint64_t addr, int size) {
    rw2_t *w = ctx;
    uint64_t off = addr - w->base;
    uint64_t v = 0;
    for (int i = 0; i < size && off+i < w->size; i++) v |= (uint64_t)w->buf[off+i] << (8*i);
    if (size == 4) v &= ~((uint64_t)w->clear_bits_on_read);
    return v;
}
static void rw2_write(void *ctx, uint64_t addr, int size, uint64_t val) {
    rw2_t *w = ctx;
    uint64_t off = addr - w->base;
    for (int i = 0; i < size && off+i < w->size; i++) w->buf[off+i] = (uint8_t)(val >> (8*i));
}
static rw2_t *make_ramwindow(machine_t *m, uint64_t base, uint64_t size, uint32_t clear_bits_on_read, const char *name) {
    rw2_t *w = calloc(1, sizeof *w);
    w->buf = calloc(1, size); w->base = base; w->size = size; w->clear_bits_on_read = clear_bits_on_read; w->name = name;
    mem_register_mmio_owned(m, base, size, rw2_read, rw2_write, w, name);
    return w;
}

/* ============================== SPD / DDR3 raw ============================ */
/* the firmware polls byte0 bit1 ("ready") then reads byte5 as a size code. We make
 * byte0 always report ready and provide a plausible non-zero size byte. */
static uint64_t spd_read(void *ctx, uint64_t addr, int size) {
    rw2_t *w = ctx; uint64_t off = addr - w->base;
    if (off == 0) return 0x02; /* ready bit always set */
    if (off == 5) return 0x08; /* arbitrary plausible "density" code */
    uint64_t v=0; for (int i=0;i<size && off+i<w->size;i++) v |= (uint64_t)w->buf[off+i]<<(8*i);
    return v;
}
static rw2_t *make_spd_window(machine_t *m, uint64_t base, uint64_t size, const char *name) {
    rw2_t *w = calloc(1, sizeof *w);
    w->buf = calloc(1, size); w->base = base; w->size = size; w->name = name;
    mem_register_mmio_owned(m, base, size, spd_read, rw2_write, w, name);
    return w;
}

/* ================================= EHCI/USB ================================ */
#define DISK_SECTOR 512

/* U3: the register map is real now.  CAPLENGTH stays 0 on purpose: the
 * sample firmware's hand-tested dance adds the CAPLENGTH byte to BAR0
 * (and then re-derives opbase = BAR0 + byte[0]) while the AuraLite-OS
 * driver computes opbase = BAR0 + caplength -- CAPLENGTH=0 is the one
 * value where both land on the same BAR-relative map (measured U0:
 * "CAPLENGTH byte (offset 0) small so capability/operational math the
 * firmware performs stays within this same register window").
 * The capability bytes share the front of the window and are read once
 * at driver init, before the operational writes reuse those offsets:
 *   CAPLENGTH@0x00 HCIVERSION@0x02 HCSPARAMS@0x04 HCCPARAMS@0x08
 *   USBCMD@0x00 USBSTS@0x04 USBINTR@0x08 FRINDEX@0x0C
 *   CTRLDSSEGMENT@0x10 PERIODICLISTBASE@0x14 ASYNCLISTADDR@0x18
 *   CONFIGFLAG@0x40 PORTSC0@0x44                                    */
#define EHCI_USBCMD        0x00
#define EHCI_USBSTS        0x04
#define EHCI_ASYNCLIST     0x18
#define EHCI_CONFIGFLAG    0x40
#define EHCI_PORTSC0       0x44

#define USBCMD_RS       (1u << 0)
#define USBCMD_HCRESET  (1u << 1)
#define USBCMD_PSE      (1u << 4)
#define USBCMD_ASE      (1u << 5)
#define USBCMD_IAAD     (1u << 6)

/* USBSTS bits match the AuraLite-OS driver's map (measured): HCHALTED=12,
 * RECLAMATION=13, PSS=14, ASS=15. */
#define USBSTS_HCHALTED (1u << 12)
#define USBSTS_PSS      (1u << 14)
#define USBSTS_ASS      (1u << 15)

/* PORTSC bits match the driver's map (measured): CCS0 CSC1 PEC3 OCA4,
 * PR8, LS=11:10 (K=low-speed), PP12, OWNER13. */
#define PORTSC_CCS      (1u << 0)
#define PORTSC_CSC      (1u << 1)   /* W1C */
#define PORTSC_PED      (1u << 2)
#define PORTSC_PEC      (1u << 3)   /* W1C */
#define PORTSC_PR       (1u << 8)
#define PORTSC_LS_J     (2u << 10)  /* J-state: full/high-speed device */
#define PORTSC_PP       (1u << 12)
#define PORTSC_OWNER    (1u << 13)

uint16_t usb_msc_be16(const uint8_t *p) {
    return (uint16_t)(((uint16_t)p[0] << 8) | p[1]);
}

typedef struct {
    rw2_t *regs;         /* BAR-backed register window, also used as scratch */
    uint64_t base;
    machine_t *m;
    uint32_t hcs;        /* HCSPARAMS snapshot: +0x04 is aliased until op mode */
    int op;              /* set on first USBCMD write: window switched to op regs */
} ehci_t;

static uint32_t rd32(machine_t *m, uint64_t a){ return (uint32_t)mem_read(m,a,4); }
static void     wr32(machine_t *m, uint64_t a, uint32_t v){ mem_write(m,a,4,v); }

/* Bulk-Only Transport + control state for the single-LUN stick.  U1: the
 * CSW is honest (tag echo, residue, OK/FAILED/PHASE).  U2: the SCSI
 * enumeration set answers from the image.  U3: SETUP requests run
 * against a minimal device model so the guest can enumerate. */
typedef struct {
    uint32_t lba, blocks;
    uint32_t tag, expected, moved;
    uint8_t  status;    /* 0=OK 1=FAILED 2=PHASE */
    int      have_cmd;  /* READ(10)/READ(12) data phase pending */
    int      csw_due;   /* a command finished; next status phase settles it */
    uint8_t  resp[64];
    uint32_t resp_len;
    uint8_t  sense_key, sense_asc, sense_ascq;
} bot_state_t;

static struct {
    bot_state_t bot;
    uint32_t usbcmd;
    int      csc, pec, ped;           /* PORTSC latches / enable */
    int      pp, owner, pr;           /* PORTSC stored RW bits */
    uint8_t  dev_addr, dev_config;    /* control device model */
} g_st;

static const char *bot_status_name[] = { "OK", "FAILED", "PHASE" };

/* ---- U3: minimal device model (control transfers) ---- */

static const uint8_t desc_device[18] = {
    18, 1,              /* bLength, DEVICE */
    0x00, 0x02,         /* bcdUSB 2.00 */
    0, 0, 0,            /* class/subclass/protocol: per interface */
    64,                 /* bMaxPacketSize0 */
    0x25, 0x05,         /* idVendor  0x0525 */
    0xA5, 0xA4,         /* idProduct 0xa4a5 */
    0x00, 0x01,         /* bcdDevice 1.00 */
    1, 2, 3,            /* iManufacturer, iProduct, iSerial */
    1                   /* bNumConfigurations */
};

static const uint8_t desc_config[32] = {
    9, 2, 32, 0, 1, 1, 0, 0x80, 50,   /* config: wTotalLength=32, 1 interface */
    9, 4, 0, 0, 2, 0x08, 0x06, 0x50, 0, /* MSC / SCSI / BOT, 2 endpoints */
    7, 5, 0x81, 2, 0x00, 0x02, 0,     /* EP 0x81 bulk IN  512 */
    7, 5, 0x02, 2, 0x00, 0x02, 0      /* EP 0x02 bulk OUT 512 */
};

static const uint8_t desc_string0[4] = { 4, 3, 0x09, 0x04 };

static uint32_t build_string_desc(uint8_t *out, const char *s) {
    uint32_t n = 0; while (s[n]) n++;
    out[0] = (uint8_t)(2 + 2 * n); out[1] = 3;
    for (uint32_t i = 0; i < n; i++) { out[2 + 2*i] = (uint8_t)s[i]; out[3 + 2*i] = 0; }
    return 2 + 2 * n;
}

static void bot_fail(bot_state_t *st, uint8_t key, uint8_t asc) {
    st->status = 1;
    st->sense_key = key; st->sense_asc = asc; st->sense_ascq = 0;
}

static void setup_request(machine_t *m, bot_state_t *st, const uint8_t *t) {
    uint8_t type = t[0], req = t[1];
    uint32_t wvalue = (uint32_t)t[2] | ((uint32_t)t[3] << 8);
    uint32_t wlength = (uint32_t)t[6] | ((uint32_t)t[7] << 8);
    st->resp_len = 0; st->moved = 0;
    st->csw_due = 0; st->have_cmd = 0;
    if (type == 0x80 && req == 0x06) { /* GET_DESCRIPTOR */
        uint8_t dtype = (uint8_t)(wvalue >> 8), idx = (uint8_t)wvalue;
        const uint8_t *src = NULL;
        uint32_t n = 0;
        if (dtype == 1) { src = desc_device; n = sizeof desc_device; }
        else if (dtype == 2) { src = desc_config; n = sizeof desc_config; }
        else if (dtype == 3 && idx == 0) { src = desc_string0; n = sizeof desc_string0; }
        else if (dtype == 3) {
            static const char *strings[3] = { "AURALITE", "USB DISK", "1234567890" };
            n = build_string_desc(st->resp, strings[(idx - 1) % 3]);
        }
        if (src) { memcpy(st->resp, src, n); }
        if (n > wlength) n = wlength;
        st->resp_len = n;
        mlog(&m->log, "[usb-ctl] GET_DESCRIPTOR type=%u idx=%u len=%u", dtype, idx, n);
    } else if (type == 0x00 && req == 0x05) { /* SET_ADDRESS */
        g_st.dev_addr = (uint8_t)wvalue;
        mlog(&m->log, "[usb-ctl] SET_ADDRESS %u", g_st.dev_addr);
    } else if (type == 0x00 && req == 0x09) { /* SET_CONFIGURATION */
        g_st.dev_config = (uint8_t)wvalue;
        mlog(&m->log, "[usb-ctl] SET_CONFIGURATION %u", g_st.dev_config);
    } else if (type == 0x80 && req == 0x00) { /* GET_STATUS */
        st->resp[0] = 0; st->resp[1] = 0;
        st->resp_len = wlength < 2 ? wlength : 2;
    } else if (type == 0xA1 && req == 0xFE) { /* GET_MAX_LUN (class) */
        st->resp[0] = 0; /* one LUN */
        st->resp_len = wlength < 1 ? wlength : 1;
        mlog(&m->log, "[usb-ctl] GET_MAX_LUN -> 0");
    } else if (type == 0x21 && req == 0xFF) { /* BOT reset (class) */
        mlog(&m->log, "[usb-ctl] BOT RESET");
    } else {
        mlog(&m->log, "[usb-ctl] unhandled request type=0x%02x req=0x%02x", type, req);
    }
}

/* Serves one transfer stage against the virtual USB stick / device model --
 * shared core for the U3 EHCI qTD walker and the U4 UHCI TD walker.
 * Branch order matters: a 13-byte IN is the BOT status phase while a
 * command is due (measured: the sample firmware's status qTD token is
 * 0x000D0180); response data (U2/U3) is served next.  Returns the bytes
 * written into the stage buffer (the UHCI TD completion field). */
static uint32_t usb_serve(machine_t *m, bot_state_t *st, int pid, uint32_t total, uint32_t buf) {
    uint32_t moved_out = 0;

    if (pid == 0 && total == 31) {
        /* Bulk-Only CBW: dCBWSignature(4) dCBWTag(4) dCBWDataTransferLength(4)
         * bmCBWFlags(1) bCBWLUN(1) bCBWCBLength(1) CBWCB(16) */
        uint32_t sig = rd32(m, buf+0);
        st->tag      = rd32(m, buf+4);
        st->expected = rd32(m, buf+8);
        st->moved    = 0;
        st->status   = 0;
        st->csw_due  = 1;
        st->have_cmd = 0;
        st->resp_len = 0;
        if (sig == 0x43425355u) { /* 'USBC' */
            uint8_t opcode = (uint8_t)mem_read(m, buf+15, 1);
            if (opcode == 0x28 || opcode == 0xA8) { /* READ(10)/READ(12) */
                uint32_t lba = (uint32_t)mem_read(m, buf+17, 1) << 24 |
                               (uint32_t)mem_read(m, buf+18, 1) << 16 |
                               (uint32_t)mem_read(m, buf+19, 1) << 8  |
                               (uint32_t)mem_read(m, buf+20, 1);
                uint8_t transfer_len[2] = {
                    (uint8_t)mem_read(m, buf+22, 1),
                    (uint8_t)mem_read(m, buf+23, 1)
                };
                uint32_t blocks = usb_msc_be16(transfer_len);
                if (blocks == 0) blocks = 1;
                st->lba = lba; st->blocks = blocks; st->have_cmd = 1;
                mlog(&m->log, "[usb-msc] READ(10) LBA=%u blocks=%u", lba, blocks);
            } else if (opcode == 0x00) { /* TEST UNIT READY */
                if (m->disk_len > 0) {
                    st->sense_key = st->sense_asc = st->sense_ascq = 0;
                    mlog(&m->log, "[usb-msc] TEST UNIT READY (ready)");
                } else {
                    bot_fail(st, 0x02, 0x3A); /* NOT READY / MEDIUM NOT PRESENT */
                    mlog(&m->log, "[usb-msc] TEST UNIT READY failed (CSW status=FAILED)");
                }
            } else if (opcode == 0x03) { /* REQUEST SENSE */
                memset(st->resp, 0, 18);
                st->resp[0] = 0x70;  /* current errors, fixed format */
                st->resp[2] = st->sense_key;
                st->resp[7] = 0x0A;  /* additional sense length */
                st->resp[12] = st->sense_asc;
                st->resp[13] = st->sense_ascq;
                st->resp_len = 18;
                mlog(&m->log, "[usb-msc] REQUEST SENSE (key=0x%02x asc=0x%02x)",
                     st->sense_key, st->sense_asc);
                st->sense_key = st->sense_asc = st->sense_ascq = 0;
            } else if (opcode == 0x12) { /* INQUIRY */
                memset(st->resp, 0, 36);
                st->resp[0] = 0x00;  /* direct-access block device */
                st->resp[1] = 0x80;  /* RMB: removable */
                st->resp[2] = 0x05;  /* SPC-3 */
                st->resp[3] = 0x02;  /* response data format 2 */
                st->resp[4] = 31;    /* additional length */
                memcpy(st->resp + 8,  "AURALITE", 8);
                memcpy(st->resp + 16, "USB DISK        ", 16);
                memcpy(st->resp + 32, "1.0 ", 4);
                st->resp_len = 36;
                mlog(&m->log, "[usb-msc] INQUIRY: vendor 'AURALITE' product 'USB DISK'");
            } else if (opcode == 0x25) { /* READ CAPACITY(10) */
                if (m->disk_len >= DISK_SECTOR) {
                    uint32_t last = (uint32_t)(m->disk_len / DISK_SECTOR) - 1;
                    st->resp[0] = (uint8_t)(last >> 24); st->resp[1] = (uint8_t)(last >> 16);
                    st->resp[2] = (uint8_t)(last >> 8);  st->resp[3] = (uint8_t)last;
                    st->resp[4] = 0; st->resp[5] = 0; st->resp[6] = 0x02; st->resp[7] = 0x00;
                    st->resp_len = 8;
                    st->sense_key = st->sense_asc = st->sense_ascq = 0;
                    mlog(&m->log, "[usb-msc] READ CAPACITY: %u sectors x 512", last + 1);
                } else {
                    bot_fail(st, 0x02, 0x3A);
                    mlog(&m->log, "[usb-msc] READ CAPACITY failed (CSW status=FAILED)");
                }
            } else {
                bot_fail(st, 0x05, 0x20); /* ILLEGAL REQUEST */
                mlog(&m->log, "[usb-msc] SCSI opcode 0x%02x failed (CSW status=FAILED)", opcode);
            }
        } else {
            /* Malformed CBW: BOT says stall + PHASE on the status phase. */
            st->status = 2; /* PHASE */
            mlog(&m->log, "[usb-msc] malformed CBW signature 0x%08x (CSW status=PHASE)", sig);
        }
    } else if (pid == 2 && total == 8) {
        /* U3: SETUP stage of a control transfer. */
        uint8_t t[8];
        for (int i = 0; i < 8; i++) t[i] = (uint8_t)mem_read(m, buf + i, 1);
        setup_request(m, st, t);
    } else if (pid == 1 && total == 13 && st->csw_due) {
        /* Status phase.  The qTD carrying a CSW is the 13-byte IN (measured:
         * the sample firmware's status qTD token is 0x000D0180; BOT fixes the
         * CSW at 13 bytes, so a 13-byte IN is the status phase even when a
         * data phase was cut short). */
        if (buf <= RAM_SIZE - 13) {
            uint32_t residue = st->expected > st->moved ? st->expected - st->moved : 0;
            wr32(m, buf+0, 0x53425355u); /* 'USBS' */
            wr32(m, buf+4, st->tag);
            wr32(m, buf+8, residue);
            mem_write(m, buf+12, 1, st->status);
            moved_out = 13;
            mlog(&m->log, "[usb-msc] synthesized CSW (status=%s) -> guest 0x%08x",
                 bot_status_name[st->status & 3], buf);
        } else {
            mlog(&m->log, "[usb-msc] rejected CSW buffer 0x%08x outside guest RAM", buf);
        }
        st->csw_due = 0;
        st->have_cmd = 0;
    } else if (pid == 1 && st->resp_len > st->moved) {
        /* Response data phase (INQUIRY / READ CAPACITY / REQUEST SENSE /
         * control descriptors).  moved doubles as the response offset. */
        uint32_t off = st->moved;
        uint32_t avail = st->resp_len - off;
        uint32_t bytes = total < avail ? total : avail;
        if (buf >= RAM_SIZE) {
            bytes = 0;
            mlog(&m->log, "[usb-msc] rejected qTD buffer 0x%08x outside guest RAM", buf);
        } else if ((uint64_t)buf + bytes > RAM_SIZE) {
            bytes = (uint32_t)(RAM_SIZE - buf);
            mlog(&m->log, "[usb-msc] clipped qTD transfer at guest RAM end");
        }
        for (uint32_t i = 0; i < bytes; i++)
            mem_write(m, (uint64_t)buf+i, 1, st->resp[off + i]);
        st->moved += bytes;
        moved_out = bytes;
        mlog(&m->log, "[usb-msc] bulk-IN %u bytes -> guest 0x%08x (response)", bytes, buf);
    } else if (pid == 1 && st->have_cmd) {
        /* U4: moved is also the media offset -- the UHCI lane chunks bulk
         * data into 64-byte TDs and every chunk must continue the stream
         * (the single-qTD EHCI lanes never exercised this). */
        uint64_t disk_off = (uint64_t)(st->lba) * DISK_SECTOR + st->moved;
        uint64_t disk_avail = disk_off < m->disk_len ? m->disk_len - disk_off : 0;
        uint32_t bytes = total;
        if (disk_avail < bytes) bytes = (uint32_t)disk_avail;
        if (buf >= RAM_SIZE) {
            bytes = 0;
            mlog(&m->log, "[usb-msc] rejected qTD buffer 0x%08x outside guest RAM", buf);
        } else if ((uint64_t)buf + bytes > RAM_SIZE) {
            bytes = (uint32_t)(RAM_SIZE - buf);
            mlog(&m->log, "[usb-msc] clipped qTD transfer at guest RAM end");
        }
        for (uint32_t i = 0; i < bytes; i++)
            mem_write(m, (uint64_t)buf+i, 1, m->disk[disk_off + i]);
        st->moved += bytes;
        moved_out = bytes;
        mlog(&m->log, "[usb-msc] bulk-IN %u bytes -> guest 0x%08x (from LBA %u)", bytes, buf, st->lba);
        uint64_t expected = (uint64_t)(st->blocks ? st->blocks : 1) * DISK_SECTOR;
        if (st->moved >= expected)
            st->have_cmd = 0;
    } else if (pid == 1 && st->csw_due) {
        /* Data IN with nothing left to deliver (FAILED CBW, or a response
         * already drained): honest short transfer, 0 bytes -- residue
         * accounts for the rest. */
        mlog(&m->log, "[usb-msc] no data left for command (%u bytes requested)", total);
    } else if (pid == 1 && total == 0) {
        /* Control status stage: nothing to move. */
    } else if (pid == 1) {
        /* Unframed IN with no live command: legacy fallback -- settle a CSW. */
        if (buf <= RAM_SIZE - 13) {
            uint32_t residue = st->expected > st->moved ? st->expected - st->moved : 0;
            wr32(m, buf+0, 0x53425355u); /* 'USBS' */
            wr32(m, buf+4, st->tag);
            wr32(m, buf+8, residue);
            mem_write(m, buf+12, 1, st->status);
            moved_out = 13;
            mlog(&m->log, "[usb-msc] synthesized CSW (status=%s) -> guest 0x%08x",
                 bot_status_name[st->status & 3], buf);
        } else {
            mlog(&m->log, "[usb-msc] rejected CSW buffer 0x%08x outside guest RAM", buf);
        }
        st->csw_due = 0;
    }
    return moved_out;
}

/* Processes one EHCI qTD against the device model, then clears Active. */
static void ehci_process_qtd(machine_t *m, uint64_t qtd_addr, bot_state_t *st) {
    uint32_t token = rd32(m, qtd_addr+8);
    if (!(token & 0x80)) return; /* not active */
    uint32_t total = (token >> 16) & 0x7FFF;
    int pid = (token >> 8) & 3; /* 0=OUT,1=IN,2=SETUP */
    uint32_t buf = rd32(m, qtd_addr+0xC);
    usb_serve(m, st, pid, total, buf);
    /* mark complete */
    token &= ~0x80u;           /* clear Active */
    token &= ~0x7FFF0000u;     /* 0 bytes remaining -> "fully transferred" */
    wr32(m, qtd_addr+8, token);
}

/* U3: walk the async schedule the way hardware does -- QH horizontal list
 * first (the guest links a transfer QH behind the head and polls its own
 * qTD array), then each QH's qTD chain via the CurrentQTD/NextQTD
 * overlay.  The firmware's chain lives in the head QH's overlay. */
static void ehci_walk(machine_t *m, uint64_t base) {
    uint32_t asynclist = rd32(m, base + EHCI_ASYNCLIST);
    if (!asynclist) return;
    uint32_t start = asynclist & ~0x1Fu;
    uint32_t qh = start;
    for (int qh_guard = 0; qh_guard < 8 && qh; qh_guard++) {
        uint32_t cur = rd32(m, qh + 0x0C);        /* CurrentQTD overlay */
        if (!cur || (cur & 1)) cur = rd32(m, qh + 0x10); /* NextQTD */
        for (int guard = 0; guard < 64 && cur && !(cur & 1); guard++) {
            ehci_process_qtd(m, cur, &g_st.bot);
            uint32_t next = rd32(m, cur + 0x00);
            if (next & 1) break;
            if (next == cur) break;
            cur = next;
        }
        uint32_t hlink = rd32(m, qh + 0x00);
        uint32_t next_qh = hlink & ~0x1Fu;
        if ((hlink & 1) || !next_qh || next_qh == start) break;
        qh = next_qh;
    }
}

static ehci_t g_ehci; /* single controller instance is all the firmware ever needs */

/* K4: COM1 is a real-ish 16550 register file now (src/serial.c -- the
 * 1-port 0x20-echo baseline made every guest stdin read rain spaces,
 * measured on the AuraLite shell). g_com1 keeps the lifetime simple:
 * one instance per machine, matching the rest of this file's globals. */
static serial_t *g_com1;

static uint32_t ehci_usbsts(void) {
    uint32_t v = 0;
    if (!(g_st.usbcmd & USBCMD_RS)) v |= USBSTS_HCHALTED;
    if ((g_st.usbcmd & (USBCMD_RS | USBCMD_ASE)) == (USBCMD_RS | USBCMD_ASE)) v |= USBSTS_ASS;
    if ((g_st.usbcmd & (USBCMD_RS | USBCMD_PSE)) == (USBCMD_RS | USBCMD_PSE)) v |= USBSTS_PSS;
    return v;
}

/* U3: an unpowered port has no device (PPC gating).  This is both
 * honest EHCI behaviour and what keeps the sample firmware's
 * hand-tested PORTSC poll (test CCS|PED == 0 before it ever powers
 * a port) from spinning: measured. */
static int ehci_ccs(void) {
    return g_ehci.m && g_ehci.m->disk_len > 0 && g_st.pp && !g_st.owner;
}

static uint32_t ehci_portsc(void) {
    uint32_t v = 0;
    if (ehci_ccs()) v |= PORTSC_CCS | PORTSC_LS_J;
    if (g_st.csc) v |= PORTSC_CSC;
    if (g_st.pec) v |= PORTSC_PEC;
    if (g_st.ped && ehci_ccs()) v |= PORTSC_PED;
    if (g_st.pr) v |= PORTSC_PR;
    if (g_st.pp) v |= PORTSC_PP;
    if (g_st.owner) v |= PORTSC_OWNER;
    return v;
}

static void ehci_portsc_write(machine_t *m, uint32_t val) {
    int before = ehci_ccs();
    if (val & PORTSC_CSC) g_st.csc = 0;           /* W1C */
    if (val & PORTSC_PEC) g_st.pec = 0;           /* W1C */
    g_st.pp = !!(val & PORTSC_PP);
    g_st.owner = !!(val & PORTSC_OWNER);
    int pr = !!(val & PORTSC_PR);
    if (g_st.pr && !pr) {
        /* Reset released: complete the sequence -- port enabled, change. */
        if (m->disk_len > 0 && g_st.pp && !g_st.owner) {
            g_st.ped = 1;
            g_st.pec = 1;
        } else {
            g_st.ped = 0;
        }
    }
    g_st.pr = pr;
    if (ehci_ccs() != before) g_st.csc = 1;       /* connect-status change */
}

static void ehci_usbcmd_write(machine_t *m, uint64_t base, uint32_t val) {
    g_ehci.op = 1; /* first op write retires the capability view of +0x04 */
    if (val & USBCMD_HCRESET) {
        /* HCRESET self-clears; the controller comes back halted. */
        g_st.usbcmd = 0;
    } else {
        g_st.usbcmd = val & 0x0000FF7Fu; /* RW bits incl. ITC */
    }
    /* U3: the doorbell runs on USBCMD writes only (measured defect: it
     * used to fire on every register write). */
    if (g_st.usbcmd & USBCMD_RS)
        ehci_walk(m, base);
}

static uint64_t ehci_read2(void *ctx, uint64_t addr, int size) {
    rw2_t *w = ctx;
    uint64_t off = addr - w->base;
    if (size == 8 && off == EHCI_USBCMD) {
        /* The sample firmware slurps the capability root as a QWORD
         * (mov rbx,[bar]) and uses the low dword as a scratch pointer: with
         * the dword reading 0 its usb_ff store lands on RAM 0 and the
         * byte re-read degenerates opbase to BAR0 (measured baseline).  A
         * HCIVERSION living at +0x02 would turn that dword into 0x02000000
         * and the store would triple-fault -- so the 8-byte slurp answers 0
         * until the first USBCMD write retires the capability root.  After
         * that the same offset answers USBCMD for the firmware's QWORD
         * read-modify-write dance; the driver itself only ever does 1/2/4
         * -byte accesses (measured). */
        return g_ehci.op ? g_st.usbcmd : 0;
    }
    if (size == 4 && off == EHCI_USBSTS) {
        /* +0x04 is HCSPARAMS in the capability view and USBSTS in the
         * operational one.  The driver reads the caps once at init and only
         * then starts writing USBCMD (measured), so the first USBCMD write
         * retires the capability view; before it, +0x04 answers HCSPARAMS. */
        return g_ehci.op ? ehci_usbsts() : g_ehci.hcs;
    }
    if (size == 4 && off == EHCI_PORTSC0) return ehci_portsc();
    return rw2_read(ctx, addr, size);
}

static void ehci_write2(void *ctx, uint64_t addr, int size, uint64_t val) {
    rw2_t *w = ctx;
    uint64_t off = addr - w->base;
    if ((size == 4 || size == 8) && off == EHCI_USBCMD) {
        /* size 8: the firmware's `mov [r14],r12` dance (low dword is the
         * command; the high dword lands on the +0x04 alias and is dropped). */
        ehci_usbcmd_write(g_ehci.m, w->base, (uint32_t)val);
        return; /* USBCMD is kept in g_st, not in the scratch window */
    }
    if (size == 4 && off == EHCI_PORTSC0) {
        if (g_ehci.m) ehci_portsc_write(g_ehci.m, (uint32_t)val);
        return;
    }
    rw2_write(ctx, addr, size, val);
}

/* U3: the async schedule advances with controller time (real EHCI walks
 * it continuously while ASE=1).  The guest's ehci_run_async() links a QH
 * and polls its own qTD array without touching MMIO (measured), so the
 * walk is sampled at a deterministic instruction cadence.  The MMIO
 * doorbell (USBCMD writes) still walks immediately. */
static void uhci_tick(machine_t *m);   /* U4: frame clock (below) */

void devices_tick(machine_t *m) {
    /* Called from the cpu.c cadence divider (every 256 instructions). */
    if (g_ehci.m && g_ehci.m == m &&
        (g_st.usbcmd & (USBCMD_RS | USBCMD_ASE)) == (USBCMD_RS | USBCMD_ASE))
        ehci_walk(m, g_ehci.base);
    uhci_tick(m);
}

/* ================================ UHCI (USB 1.1) =========================== */
/* U4: QEMU piix3-usb-uhci parity (run_qemu_usb_msc.sh / test_usb_msc.sh lane).
 * Contract measured from AuraLite-OS@0ed0d29 drivers/usb/uhci.c: I/O BAR4
 * register file, 1024-entry frame list, TD/QH walk, and the guest's own
 * status-bit map -- USBSTS.HCHALTED is bit 5 there (measured).  The guest
 * links a QH into every frame-list slot and polls its own TD array in RAM
 * without touching MMIO, so the frame walk is sampled at the same
 * instruction cadence as the EHCI schedule (devices_tick).  The UHCI
 * channel keeps its own BOT state; the device model behind it is the
 * single shared stick. */

#define UHCI_USBCMD    0x00
#define UHCI_USBSTS    0x02
#define UHCI_USBINTR   0x04
#define UHCI_FRNUM     0x06
#define UHCI_FLBASEADD 0x08
#define UHCI_SOFMOD    0x0C
#define UHCI_PORTSC1   0x10
#define UHCI_PORTSC2   0x12

#define UHCI_USBCMD_RUN       (1u << 0)
#define UHCI_USBCMD_HCRESET   (1u << 1)
#define UHCI_USBCMD_GRESET    (1u << 2)
#define UHCI_USBCMD_CF        (1u << 6)
#define UHCI_USBCMD_MAXPACKET (1u << 7)

#define UHCI_USBSTS_USBINT   (1u << 0)
#define UHCI_USBSTS_HCHALTED (1u << 5)  /* the guest's map (measured) */

#define UHCI_PORTSC_CCS    (1u << 0)
#define UHCI_PORTSC_CSC    (1u << 1)   /* W1C */
#define UHCI_PORTSC_PED    (1u << 2)
#define UHCI_PORTSC_ECSC   (1u << 3)   /* W1C */
#define UHCI_PORTSC_LSDA   (1u << 8)
#define UHCI_PORTSC_PR     (1u << 9)
#define UHCI_PORTSC_SUSPEND (1u << 12)

#define UHCI_TD_ACTIVE (1u << 23)
#define UHCI_TD_IOC    (1u << 24)

#define UHCI_PID_SETUP 0x2D
#define UHCI_PID_IN    0x69
#define UHCI_PID_OUT   0xE1

#define UHCI_IOBASE 0xC040u   /* BAR4: QEMU piix3-usb-uhci-style I/O window */

typedef struct {
    machine_t *m;
    uint16_t usbcmd, usbsts, usbintr, frnum, sofm;
    uint32_t flbase;
    struct { int csc, ecsc, ped, pr, suspend; } port[2];
    bot_state_t bot;      /* U4: the UHCI channel has its own BOT state */
} uhci_t;

static uhci_t g_uhci;

/* The stick sits behind the shared connector: both the EHCI port and its
 * UHCI companion see the one image (single-device model, two host sides --
 * the guest's scan order (UHCI first, measured) binds MSC to the UHCI
 * instance). */
static int uhci_port_attached(int p) {
    return p == 0 && g_uhci.m && g_uhci.m->disk_len > 0;
}

static uint32_t uhci_portsc(int p) {
    uint32_t v = 0;
    if (uhci_port_attached(p)) v |= UHCI_PORTSC_CCS | (1u << 4); /* J-state */
    if (g_uhci.port[p].csc) v |= UHCI_PORTSC_CSC;
    if (g_uhci.port[p].ecsc) v |= UHCI_PORTSC_ECSC;
    if (g_uhci.port[p].ped && uhci_port_attached(p)) v |= UHCI_PORTSC_PED;
    if (g_uhci.port[p].pr) v |= UHCI_PORTSC_PR;
    if (g_uhci.port[p].suspend) v |= UHCI_PORTSC_SUSPEND;
    /* LSDA clear: full-speed device on the UHCI side (QEMU parity) */
    return v;
}

static void uhci_portsc_write(int p, uint32_t val) {
    if (val & UHCI_PORTSC_CSC) g_uhci.port[p].csc = 0;   /* W1C */
    if (val & UHCI_PORTSC_ECSC) g_uhci.port[p].ecsc = 0; /* W1C */
    if (val & UHCI_PORTSC_PED) g_uhci.port[p].ped = 1;   /* the guest ORs PED on */
    g_uhci.port[p].suspend = !!(val & UHCI_PORTSC_SUSPEND);
    int pr = !!(val & UHCI_PORTSC_PR);
    if (g_uhci.port[p].pr && !pr) {
        /* Reset released: the port enables and reports a change. */
        if (uhci_port_attached(p)) {
            g_uhci.port[p].ped = 1;
            g_uhci.port[p].csc = 1;
            g_uhci.port[p].ecsc = 1;
        } else {
            g_uhci.port[p].ped = 0;
        }
    }
    g_uhci.port[p].pr = pr;
}

static void uhci_reset(void) {
    machine_t *m = g_uhci.m;
    memset(&g_uhci, 0, sizeof g_uhci);
    g_uhci.m = m;
    g_uhci.usbsts = UHCI_USBSTS_HCHALTED;
}

static uint32_t uhci_process_td(machine_t *m, uint64_t td) {
    uint32_t link = rd32(m, td + 0);
    uint32_t ctrl = rd32(m, td + 4);
    uint32_t token = rd32(m, td + 8);
    uint32_t buf = rd32(m, td + 12);
    if (!(ctrl & UHCI_TD_ACTIVE)) return link;  /* idempotent re-walks */
    int pid = -1;
    switch (token & 0xFF) {
    case UHCI_PID_SETUP: pid = 2; break;
    case UHCI_PID_IN:    pid = 1; break;
    case UHCI_PID_OUT:   pid = 0; break;
    default: break;
    }
    uint32_t flen = (token >> 21) & 0x7FF;
    uint32_t total = (flen == 0x7FF) ? 0 : flen + 1;  /* 0x7FF = zero bytes */
    uint32_t moved = 0;
    if (pid >= 0) moved = usb_serve(m, &g_uhci.bot, pid, total, buf);
    /* Completion: Active clears, ctrl[10:0] carries the actual length
     * (bytes-1; 0x7FF = 0 bytes) -- the guest's interrupt path reads it. */
    ctrl &= ~UHCI_TD_ACTIVE;
    ctrl = (ctrl & ~0x7FFu) | ((moved == 0) ? 0x7FFu : ((moved - 1) & 0x7FF));
    wr32(m, td + 4, ctrl);
    if ((ctrl & UHCI_TD_IOC) && (g_uhci.usbintr & 1u))
        g_uhci.usbsts |= UHCI_USBSTS_USBINT;
    return link;
}

/* One frame of the schedule: frame_list[FRNUM] -> QH (bit1) or TD chain. */
static void uhci_walk(machine_t *m) {
    if (!(g_uhci.usbcmd & UHCI_USBCMD_RUN) || (g_uhci.usbcmd & UHCI_USBCMD_GRESET))
        return; /* U4 NC: a halted controller runs no frames */
    if (!g_uhci.flbase) return;
    uint32_t ent = rd32(m, (uint64_t)g_uhci.flbase + 4u * (g_uhci.frnum & 0x3FF));
    if (!ent || (ent & 1)) return;
    if (ent & 2) { /* QH: advance element_link as TDs complete */
        uint64_t qh = ent & ~0xFu;
        for (int g = 0; g < 64; g++) {
            uint32_t el = rd32(m, qh + 4);
            if (el & 1) break;
            uint32_t link = uhci_process_td(m, el & ~0xFu);
            wr32(m, qh + 4, link);
            if (link & 1) break;
        }
    } else { /* raw TD chain */
        uint32_t cur = ent;
        for (int g = 0; g < 64 && !(cur & 1); g++)
            cur = uhci_process_td(m, cur & ~0xFu);
    }
}

/* Frame clock + schedule sampler (called from the cpu cadence hook). */
static void uhci_tick(machine_t *m) {
    if (!g_uhci.m || g_uhci.m != m) return;
    if (!(g_uhci.usbcmd & UHCI_USBCMD_RUN) || (g_uhci.usbcmd & UHCI_USBCMD_GRESET))
        return;
    g_uhci.frnum = (uint16_t)((g_uhci.frnum + 1) & 0x3FF);
    uhci_walk(m);
}

static uint32_t uhci_io_read(void *ctx, uint16_t port, int size) {
    (void)ctx;
    (void)size;
    uint32_t v = 0;
    switch (port - UHCI_IOBASE) {
    case UHCI_USBCMD:  v = g_uhci.usbcmd; break;
    case UHCI_USBSTS:  v = g_uhci.usbsts & ~(UHCI_USBSTS_HCHALTED);
                       if (!(g_uhci.usbcmd & UHCI_USBCMD_RUN)) v |= UHCI_USBSTS_HCHALTED;
                       break;
    case UHCI_USBINTR: v = g_uhci.usbintr; break;
    case UHCI_FRNUM:   v = g_uhci.frnum; break;
    case UHCI_FLBASEADD: v = g_uhci.flbase; break;
    case UHCI_SOFMOD:  v = g_uhci.sofm; break;
    case UHCI_PORTSC1: v = uhci_portsc(0); break;
    case UHCI_PORTSC2: v = uhci_portsc(1); break;
    default: break;
    }
    return v;
}

static void uhci_io_write(void *ctx, uint16_t port, int size, uint32_t val) {
    (void)ctx;
    uint16_t off = (uint16_t)(port - UHCI_IOBASE);
    if (size == 1) { /* RMW-merge the byte lane into the 16-bit register */
        uint32_t cur = uhci_io_read(ctx, (uint16_t)(port & ~1), 2);
        int sh = 8 * (port & 1);
        val = (cur & ~(0xFFu << sh)) | ((val & 0xFF) << sh);
        off &= (uint16_t)~1;
    }
    switch (off) {
    case UHCI_USBCMD:
        if (val & UHCI_USBCMD_HCRESET) {
            uhci_reset();
            return;
        }
        g_uhci.usbcmd = (uint16_t)(val & 0x00FF);
        if (g_uhci.usbcmd & UHCI_USBCMD_RUN) uhci_walk(g_uhci.m);
        return;
    case UHCI_USBSTS: /* W1C latched status bits; HCHALTED is computed */
        g_uhci.usbsts &= (uint16_t)~(val & 0x003F);
        return;
    case UHCI_USBINTR: g_uhci.usbintr = (uint16_t)(val & 0x000F); return;
    case UHCI_FRNUM:   g_uhci.frnum = (uint16_t)(val & 0x3FF); return;
    case UHCI_SOFMOD:  g_uhci.sofm = (uint16_t)(val & 0xFF); return;
    case UHCI_PORTSC1: uhci_portsc_write(0, val); return;
    case UHCI_PORTSC2: uhci_portsc_write(1, val); return;
    default: return; /* FLBASEADD is written 32-bit; see uhci_io_write32 */
    }
}

static uint32_t uhci_io_read32(void *ctx, uint16_t port, int size) {
    if (port == UHCI_IOBASE + UHCI_FLBASEADD && size == 4) return g_uhci.flbase;
    return uhci_io_read(ctx, port, size);
}

static void uhci_io_write32(void *ctx, uint16_t port, int size, uint32_t val) {
    if (port == UHCI_IOBASE + UHCI_FLBASEADD && size == 4) {
        g_uhci.flbase = val & ~0xFFFu; /* 4K-aligned frame list */
        return;
    }
    uhci_io_write(ctx, port, size, val);
}

/* ================================== GPU ==================================== */
/* the firmware's own device-discovery loop reads the class byte at PCI offset 0x0C
 * instead of the spec's 0x0B -- we mirror the class code at both offsets so
 * the firmware's (evidently hand-tested) logic actually finds the GPU. */

/* =========================== public init entry points ====================== */
void devices_init_common(machine_t *m) {
    pci_init(m);

    /* CHIPSET H0: the 8259A pair exists identically on all five platforms */
    pic_init(m);
    pic_io_register(m);
    pit_init(m);
    pit_io_register(m);
    /* CHIPSET H3: 0x70/0x71 are a real MC146818A now -- long gone as the
     * silent sinks they were at baseline. The Super I/O config ports
     * (0x2E/0x2F) stay unclaimed: reads float high, and the sample
     * firmware never depends on them (measured). */
    rtc_init(m);
    rtc_io_register(m);
    /* CHIPSET H4: the 8042 keyboard controller at 0x60/0x64 (IRQ1 level-
     * driven off OBF); since H5 its 0xFE pulses and output-port bit0
     * actually reset the machine. */
    kbc_init(m);
    kbc_io_register(m);
    /* CHIPSET H5: A20 gate + system-control port 0x92 + reset control
     * 0xCF9 (chipset.[ch], consumed by cpu_step at the boundary). */
    chipset_init(m);
    chipset_io_register(m);
    /* CHIPSET H6: local APIC MMIO window at 0xFEE00000 -- CPUID.1:EDX
     * advertised APIC long before the window existed (measured). */
    lapic_init(m);
    lapic_mmio_register(m);
    /* CHIPSET H7: 82093AA I/O APIC at 0xFEC00000; ISA lines are fanned
     * out to it inside pic_raise_irq/pic_set_irq (the board wire). */
    ioapic_init(m);
    ioapic_mmio_register(m);

    g_com1 = serial_alloc(m);
    serial_io_register(m, g_com1);

    /* Host bridge: bus0 dev0 func0 */
    pci_add_device(m, 0,0,0, "host-bridge", 0x8086, 0x0100, 0x06, 0x00, 0);

    /* "PCI bridge / misc" at dev1 (poked directly by the Haswell path) */
    pci_add_device(m, 0,1,0, "pci-bridge-misc", 0x8086, 0x0101, 0x06, 0x04, 0);

    /* LPC bridge: real Intel convention dev31/func0 */
    pci_add_device(m, 0,31,0, "lpc-bridge", 0x8086, 0x1E55, 0x06, 0x01, 0);

    /* SATA/AHCI: real Intel convention dev31/func2.  STORE S1: BAR5 is a
     * real MMIO window now (was an empty stub -- the guest's own boot
     * receipt "controller 0: BAR5 empty, skipping" measured the gap). */
    pci_dev_t *sata_pci = pci_add_device(m, 0,31,2, "sata-ahci", 0x8086, 0x1E03, 0x01, 0x06, 1);
    sata_pci->cfg[0x24] = (uint8_t)(AHCI_ABAR);
    sata_pci->cfg[0x25] = (uint8_t)(AHCI_ABAR >> 8);
    sata_pci->cfg[0x26] = (uint8_t)(AHCI_ABAR >> 16);
    sata_pci->cfg[0x27] = (uint8_t)(AHCI_ABAR >> 24);
    ahci_register(m);

    /* GPU: dev2/func0 (real Intel iGPU convention) */
    pci_dev_t *gpu = pci_add_device(m, 0,2,0, "igpu", 0x8086, 0x0412, 0x03, 0x00, 0);
    gpu->cfg[0x0B] = 0x03; gpu->cfg[0x0C] = 0x03; /* class mirrored at the offset the firmware actually reads */

    /* UHCI USB1.1 companion: 0:1.2 with the piix3-usb-uhci identity
     * (QEMU parity: run_qemu_usb_msc.sh / test_usb_msc.sh attach their
     * usb-storage here).  BAR4 is an I/O window -- the guest reads the
     * BAR, masks ~0xF and uses it as the register file base.  USB U5:
     * --no-usb-uhci omits the whole function (the EHCI-only QEMU shape
     * of test_usb_ehci.sh). */
    if (!m->cfg_no_uhci) {
        pci_dev_t *uhci_pci = pci_add_device(m, 0,1,2, "uhci", 0x8086, 0x7020, 0x0C, 0x03, 0x00);
        uint32_t uhci_bar = UHCI_IOBASE | 0x1u;
        uhci_pci->cfg[0x20] = (uint8_t)(uhci_bar);
        uhci_pci->cfg[0x21] = (uint8_t)(uhci_bar>>8);
        uhci_pci->cfg[0x22] = (uint8_t)(uhci_bar>>16);
        uhci_pci->cfg[0x23] = (uint8_t)(uhci_bar>>24);
        uhci_reset();
        g_uhci.m = m;
        io_register(m, UHCI_IOBASE, 32, uhci_io_read32, uhci_io_write32, NULL, "UHCI BAR4");
    }

    /* EHCI USB2 controller: dev3/func0 (matches the mechanism#1 probe
     * 0x80001810 the firmware issues: bus0 dev3 func0 reg0x10 = BAR0) */
    pci_dev_t *ehci_pci = pci_add_device(m, 0,3,0, "ehci", 0x8086, 0x1E26, 0x0C, 0x03, 0x20);
    uint32_t ehci_bar = 0xFEB00000u;
    ehci_pci->cfg[0x10] = (uint8_t)(ehci_bar);
    ehci_pci->cfg[0x11] = (uint8_t)(ehci_bar>>8);
    ehci_pci->cfg[0x12] = (uint8_t)(ehci_bar>>16);
    ehci_pci->cfg[0x13] = (uint8_t)(ehci_bar>>24);
    g_ehci.base = ehci_bar; g_ehci.m = m;
    rw2_t *regs = calloc(1, sizeof *regs);
    regs->buf = calloc(1, 0x1000); regs->base = ehci_bar; regs->size = 0x1000; regs->name = "EHCI BAR0";
    mem_register_mmio_owned(m, ehci_bar, 0x1000, ehci_read2, ehci_write2, regs, "EHCI BAR0");
    g_ehci.regs = regs;
    /* U3: capability bytes seeded (read once by the AuraLite-OS driver
     * before operational writes reuse the offsets).  CAPLENGTH stays 0 so
     * the firmware's dance and the driver share one BAR-relative map. */
    memset(&g_st, 0, sizeof g_st);              /* fresh machine: clean latch state */
    cpu_devices_tick = devices_tick;            /* USB U3: cadence walk hook */
    regs->buf[0] = 0x00;                    /* CAPLENGTH = 0 */
    regs->buf[2] = 0x00; regs->buf[3] = 0x02; /* HCIVERSION 2.00 */
    regs->buf[4] = 0x11;                    /* HCSPARAMS: N_PORTS=1 | PPC */
    g_ehci.hcs = 0x11; g_ehci.op = 0;       /* +0x04 alias: caps until op mode */
    /* HCCPARAMS @0x08 = 0: 32-bit addressing only */

    /* GPU VRAM / framebuffer, BAR0 = 0xD0000000 as the firmware's own "enable:" code
     * sets it; this buffer doubles as the pixel buffer shown in the GUI. */
    m->fb_w = 800; m->fb_h = 600;
    rw2_t *vram = make_ramwindow(m, 0xD0000000ULL, (uint64_t)m->fb_w*m->fb_h*4, 0, "GPU VRAM/Framebuffer");
    m->fb = (uint32_t*)vram->buf;

    /* CAR (cache-as-RAM) scratch window -- real silicon has no DRAM behind
     * this until memory controller training finishes; our flat memory model
     * just gives it an ordinary writable buffer, which is all the firmware needs
     * since it only uses it as early stack + scratch space. */
    make_ramwindow(m, 0xFEF00000ULL, 0x00400000ULL, 0, "CAR scratch window");

}

/* STORE S4: optional second AHCI controller (--ahci2).  QEMU's
 * `-device ahci,id=ahci1` parity: a second class-01/06 function so the
 * guest's multi-scan `ctrl_count` path (RESIDUE2 T3) runs.  Placement at
 * 0:31:3 is deliberate and measured: the guest class scan is bus-0
 * dev/func ASCENDING (drivers/pci/pci.c pci_find_class_after), so 0:31:3
 * binds AFTER the onboard 0:31:2 and every S1-S3 receipt ("controller 0
 * at PCI 0:31.2") keeps its numbering.  BAR5 -> AHCI_ABAR2 (8 KiB apart
 * from ABAR because the guest maps 8 KiB per BAR5). */
void devices_add_ahci2(machine_t *m) {
    pci_dev_t *p = pci_add_device(m, 0,31,3, "sata-ahci2", 0x8086, 0x2922, 0x01, 0x06, 1);
    p->cfg[0x24] = (uint8_t)(AHCI_ABAR2);
    p->cfg[0x25] = (uint8_t)(AHCI_ABAR2 >> 8);
    p->cfg[0x26] = (uint8_t)(AHCI_ABAR2 >> 16);
    p->cfg[0x27] = (uint8_t)(AHCI_ABAR2 >> 24);
    ahci_register_ctrl(m, 1);
}

void devices_init_platform(machine_t *m) {
    const platform_t *p = m->plat;
    switch (p->id) {
    case PLAT_SANDYBRIDGE:
    case PLAT_IVYBRIDGE:
        /* DDR3 raw SPD-style controller polled at 0xE00FB000 */
        make_spd_window(m, 0xE00FB000ULL, 0x1000, "DDR3 SPD controller");
        /* generic register block at 0xFED10000 used for the final enable
         * writes -- plain RAM-like storage is sufficient since this path
         * never polls a busy bit on it. */
        make_ramwindow(m, 0xFED10000ULL, 0x10000, 0, "DDR3 channel registers");
        break;
    case PLAT_HASWELL:
    case PLAT_BROADWELL:
        /* DDR4 training controller: busy-bit (0x80000000) auto-clears on
         * every read, matching the firmware's own polling convention exactly. */
        make_ramwindow(m, 0xFED10000ULL, 0x10000, 0x80000000u, "DDR4 training controller");
        break;
    case PLAT_BAYTRAIL:
        make_spd_window(m, 0xE00FB000ULL, 0x1000, "DDR3L SPD controller");
        break;
    default: break;
    }
}

/* C10: teardown keeps the ASan/UBSan CI lane leak-clean -- every device
 * window allocated by devices_init_* is an rw2_t hanging off the mmio list. */
void devices_done(machine_t *m) {
    mmio_region_t *r = m->mmio_list;
    while (r) {
        mmio_region_t *next = r->next;
        if (r->owned_ctx) {
            rw2_t *w = r->ctx;
            free(w->buf); free(w);
        }
        free(r);
        r = next;
    }
    m->mmio_list = NULL;
    pci_done(m);                                   /* USB U1: LSan lane */
    if (g_com1) { serial_free(g_com1); g_com1 = NULL; }   /* K5: LSan lane */
}
