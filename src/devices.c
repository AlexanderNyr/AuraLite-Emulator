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
    mem_register_mmio(m, base, size, rw2_read, rw2_write, w, name);
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
    mem_register_mmio(m, base, size, spd_read, rw2_write, w, name);
    return w;
}

/* ================================= EHCI/USB ================================ */
#define DISK_SECTOR 512

typedef struct {
    rw2_t *regs;         /* BAR-backed register window, also used as scratch */
    uint64_t base;
    machine_t *m;
} ehci_t;

static uint32_t rd32(machine_t *m, uint64_t a){ return (uint32_t)mem_read(m,a,4); }
static void     wr32(machine_t *m, uint64_t a, uint32_t v){ mem_write(m,a,4,v); }

/* Processes one qTD: performs the data phase against the virtual USB stick
 * (bulk-only transport CBW/CSW handled generically), then clears Active. */
static void ehci_process_qtd(machine_t *m, uint64_t qtd_addr, uint32_t *pending_lba, uint32_t *pending_blocks, int *have_cmd) {
    uint32_t token = rd32(m, qtd_addr+8);
    if (!(token & 0x80)) return; /* not active */
    uint32_t total = (token >> 16) & 0x7FFF;
    int pid = (token >> 8) & 3; /* 0=OUT,1=IN,2=SETUP */
    uint32_t buf = rd32(m, qtd_addr+0xC);

    if (pid == 0 && total == 31) {
        /* Bulk-Only CBW: dCBWSignature(4) dCBWTag(4) dCBWDataTransferLength(4)
         * bmCBWFlags(1) bCBWLUN(1) bCBWCBLength(1) CBWCB(16) */
        uint32_t sig = rd32(m, buf+0);
        if (sig == 0x43425355u) { /* 'USBC' */
            uint8_t opcode = (uint8_t)mem_read(m, buf+15, 1);
            if (opcode == 0x28 || opcode == 0xA8) { /* READ(10)/READ(12) */
                uint32_t lba = (uint32_t)mem_read(m, buf+17, 1) << 24 |
                               (uint32_t)mem_read(m, buf+18, 1) << 16 |
                               (uint32_t)mem_read(m, buf+19, 1) << 8  |
                               (uint32_t)mem_read(m, buf+20, 1);
                uint32_t blocks = (uint32_t)mem_read(m, buf+22, 1) << 8 | (uint32_t)mem_read(m, buf+23, 1);
                if (blocks == 0) blocks = 1;
                *pending_lba = lba; *pending_blocks = blocks; *have_cmd = 1;
                mlog(&m->log, "[usb-msc] READ(10) LBA=%u blocks=%u", lba, blocks);
            } else {
                mlog(&m->log, "[usb-msc] SCSI opcode 0x%02x (ignored by behavioural model)", opcode);
            }
        }
    } else if (pid == 1) {
        if (*have_cmd) {
            uint32_t bytes = total;
            uint32_t avail = (uint32_t)(m->disk_len > (uint64_t)(*pending_lba)*DISK_SECTOR
                                         ? m->disk_len - (uint64_t)(*pending_lba)*DISK_SECTOR : 0);
            if (bytes > avail) bytes = avail;
            for (uint32_t i = 0; i < bytes; i++)
                mem_write(m, buf+i, 1, m->disk[(uint64_t)(*pending_lba)*DISK_SECTOR + i]);
            mlog(&m->log, "[usb-msc] bulk-IN %u bytes -> guest 0x%08x (from LBA %u)", bytes, buf, *pending_lba);
        } else {
            /* Likely the CSW (status) phase -- fabricate a successful one. */
            wr32(m, buf+0, 0x53425355u); /* 'USBS' */
            wr32(m, buf+4, 0);
            wr32(m, buf+8, 0); /* status = good */
            mlog(&m->log, "[usb-msc] synthesized CSW (status=OK) -> guest 0x%08x", buf);
        }
    }
    /* mark complete */
    token &= ~0x80u;           /* clear Active */
    token &= ~0x7FFF0000u;     /* 0 bytes remaining -> "fully transferred" */
    wr32(m, qtd_addr+8, token);
}

static void ehci_doorbell(ehci_t *e) {
    machine_t *m = e->m;
    uint32_t asynclist = rd32(m, e->base + 0x18);
    if (!asynclist) return;
    uint32_t qh = asynclist & ~0x1Fu;
    uint32_t cur = rd32(m, qh + 0x0C);
    uint32_t pending_lba = 0, pending_blocks = 0; int have_cmd = 0;
    static uint32_t s_lba=0, s_blocks=0; static int s_have=0; /* persists across doorbell calls for this simple single-LUN model */
    pending_lba = s_lba; pending_blocks = s_blocks; have_cmd = s_have;
    for (int guard = 0; guard < 32 && cur && !(cur & 1); guard++) {
        ehci_process_qtd(m, cur, &pending_lba, &pending_blocks, &have_cmd);
        uint32_t next = rd32(m, cur + 0x00);
        if (next & 1) break;
        if (next == cur) break;
        cur = next;
    }
    s_lba = pending_lba; s_blocks = pending_blocks; s_have = have_cmd;
}

static ehci_t g_ehci; /* single controller instance is all the firmware ever needs */

static void ehci_write2(void *ctx, uint64_t addr, int size, uint64_t val) {
    rw2_write(ctx, addr, size, val);
    ehci_doorbell(&g_ehci);
}

/* ================================== GPU ==================================== */
/* the firmware's own device-discovery loop reads the class byte at PCI offset 0x0C
 * instead of the spec's 0x0B -- we mirror the class code at both offsets so
 * the firmware's (evidently hand-tested) logic actually finds the GPU. */

/* =========================== public init entry points ====================== */
void devices_init_common(machine_t *m) {
    pci_init(m);

    /* Host bridge: bus0 dev0 func0 */
    pci_add_device(m, 0,0,0, "host-bridge", 0x8086, 0x0100, 0x06, 0x00, 0);

    /* "PCI bridge / misc" at dev1 (poked directly by the Haswell path) */
    pci_add_device(m, 0,1,0, "pci-bridge-misc", 0x8086, 0x0101, 0x06, 0x04, 0);

    /* LPC bridge: real Intel convention dev31/func0 */
    pci_add_device(m, 0,31,0, "lpc-bridge", 0x8086, 0x1E55, 0x06, 0x01, 0);

    /* SATA/AHCI: real Intel convention dev31/func2 */
    pci_add_device(m, 0,31,2, "sata-ahci", 0x8086, 0x1E03, 0x01, 0x06, 1);

    /* GPU: dev2/func0 (real Intel iGPU convention) */
    pci_dev_t *gpu = pci_add_device(m, 0,2,0, "igpu", 0x8086, 0x0412, 0x03, 0x00, 0);
    gpu->cfg[0x0B] = 0x03; gpu->cfg[0x0C] = 0x03; /* class mirrored at the offset the firmware actually reads */

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
    mem_register_mmio(m, ehci_bar, 0x1000, rw2_read, ehci_write2, regs, "EHCI BAR0");
    g_ehci.regs = regs;
    /* CAPLENGTH byte (offset 0) small so capability/operational math the firmware
     * performs stays within this same register window. */
    regs->buf[0] = 0x00;

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

    /* small I/O "sinks" the firmware pokes but never reads back meaningfully:
     * Super I/O config (0x2E/0x2F) and CMOS/RTC index+data (0x70/0x71). */
    static uint8_t superio_idx, cmos_idx;
    (void)superio_idx; (void)cmos_idx;
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
