/* tests/test_usb.c -- USB U1 vectors: Bulk-Only Transport CBW/CSW channel.
 *
 * The fixture drives the same behavioural EHCI block the sample firmware
 * boots through: build a QH + qTD chain in guest RAM, ring the doorbell
 * (a write to ASYNCLISTADDR), then read the CSW back out of RAM.
 * U1 pins the honest CSW fields: dCSWTag echoes dCBWTag (the AuraLite-OS
 * msc.c validates it), dCSWDataResidue is expected-minus-moved, and
 * bCSWStatus distinguishes OK / FAILED / PHASE.  The firmware-shaped
 * vector is the negative control: its recipe (tag=1, READ(10) LBA 0)
 * must keep round-tripping with status=OK.
 */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "devices.h"
#include "harness.h"

#define EHCI_BASE   0xFEB00000u
#define CBW_ADDR    0x00002000u
#define QH_ADDR     0x00003000u
#define QTD0        0x00003100u
#define DATA_ADDR   0x00010000u
#define CSW_ADDR    0x00003800u

typedef struct { machine_t m; uint8_t stick[4096]; } fx_t;

static void fx_init(fx_t *f) {
    setup_machine(&f->m);
    devices_init_common(&f->m);
    for (int i = 0; i < (int)sizeof f->stick; i++)
        f->stick[i] = (uint8_t)(i * 3 + 7);
    f->m.disk = f->stick;
    f->m.disk_len = sizeof f->stick;
}

static void fx_done(fx_t *f) { devices_done(&f->m); }

static void w32r(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}
static uint32_t r32r(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static void cbw(fx_t *f, uint32_t tag, uint32_t expected, uint8_t flags,
                uint8_t opcode, uint32_t lba, uint16_t blocks) {
    uint8_t *p = f->m.ram + CBW_ADDR;
    memset(p, 0, 31);
    p[0] = 0x55; p[1] = 0x53; p[2] = 0x42; p[3] = 0x43; /* 'USBC' */
    w32r(p + 4, tag);
    w32r(p + 8, expected);
    p[12] = flags;      /* bmCBWFlags: 0x80 = IN */
    p[13] = 0;          /* bCBWLUN */
    p[14] = 10;         /* bCBWCBLength */
    p[15] = opcode;
    p[17] = (uint8_t)(lba >> 24); p[18] = (uint8_t)(lba >> 16);
    p[19] = (uint8_t)(lba >> 8);  p[20] = (uint8_t)lba;
    p[22] = (uint8_t)(blocks >> 8); p[23] = (uint8_t)blocks;
}

static void qtd(fx_t *f, uint32_t addr, uint32_t next, uint32_t total,
                int pid, uint32_t buf) {
    uint8_t *p = f->m.ram + addr;
    w32r(p + 0, next);                  /* bit0 set = terminate */
    w32r(p + 4, 0);
    w32r(p + 8, (total << 16) | ((uint32_t)pid << 8) | 0x80);
    w32r(p + 0xC, buf);
}

static void fire(fx_t *f) {
    uint8_t *qh = f->m.ram + QH_ADDR;
    w32r(qh + 0x0C, QTD0);
    mem_write(&f->m, EHCI_BASE + 0x18, 4, QH_ADDR); /* ASYNCLISTADDR: stores only */
    mem_write(&f->m, EHCI_BASE + 0x00, 4, 0x21);    /* USBCMD Run|ASE: the doorbell */
}

static void check_csw_at(fx_t *f, uint32_t addr, uint32_t tag,
                         uint32_t residue, uint8_t status) {
    const uint8_t *c = f->m.ram + addr;
    assert(r32r(c + 0) == 0x53425355u); /* 'USBS' */
    assert(r32r(c + 4) == tag);
    assert(r32r(c + 8) == residue);
    assert(c[12] == status);
}

static void test_be16(void) {
    uint8_t cb[31] = {0};
    cb[22] = 0x00; cb[23] = 0x01;
    assert(usb_msc_be16(&cb[22]) == 1);
    cb[22] = 0x01; cb[23] = 0x00;
    assert(usb_msc_be16(&cb[22]) == 256);
    printf("ok be16\n");
}

static void test_csw_tag_echo(void) {
    fx_t f; fx_init(&f);
    cbw(&f, 0xA5A5A5A5u, 512, 0x80, 0x28, 0, 1);
    qtd(&f, QTD0 + 0x00, QTD0 + 0x10, 31, 0, CBW_ADDR);
    qtd(&f, QTD0 + 0x10, QTD0 + 0x20, 512, 1, DATA_ADDR);
    qtd(&f, QTD0 + 0x20, 1, 13, 1, CSW_ADDR);
    fire(&f);
    check_csw_at(&f, CSW_ADDR, 0xA5A5A5A5u, 0, 0);
    assert(f.m.ram[DATA_ADDR] == 7);
    assert(f.m.ram[DATA_ADDR + 511] == (uint8_t)(511 * 3 + 7));
    fx_done(&f);
    printf("ok csw tag echo\n");
}

static void test_csw_residue_short_data(void) {
    fx_t f; fx_init(&f);
    /* Host expects 2 blocks but only one data qTD is posted. */
    cbw(&f, 0x22222222u, 1024, 0x80, 0x28, 0, 2);
    qtd(&f, QTD0 + 0x00, QTD0 + 0x10, 31, 0, CBW_ADDR);
    qtd(&f, QTD0 + 0x10, QTD0 + 0x20, 512, 1, DATA_ADDR);
    qtd(&f, QTD0 + 0x20, 1, 13, 1, CSW_ADDR);
    fire(&f);
    check_csw_at(&f, CSW_ADDR, 0x22222222u, 512, 0);
    fx_done(&f);
    printf("ok csw residue short data\n");
}

static void test_csw_residue_no_image(void) {
    fx_t f; fx_init(&f);
    f.m.disk_len = 0; /* nothing behind the read */
    cbw(&f, 0x33333333u, 512, 0x80, 0x28, 0, 1);
    qtd(&f, QTD0 + 0x00, QTD0 + 0x10, 31, 0, CBW_ADDR);
    qtd(&f, QTD0 + 0x10, QTD0 + 0x20, 512, 1, DATA_ADDR);
    qtd(&f, QTD0 + 0x20, 1, 13, 1, CSW_ADDR);
    fire(&f);
    check_csw_at(&f, CSW_ADDR, 0x33333333u, 512, 0);
    fx_done(&f);
    printf("ok csw residue no image\n");
}

static void test_csw_failed_opcode(void) {
    fx_t f; fx_init(&f);
    cbw(&f, 0x44444444u, 36, 0x80, 0xA0, 0, 0); /* REPORT LUNS: unsupported */
    qtd(&f, QTD0 + 0x00, QTD0 + 0x10, 31, 0, CBW_ADDR);
    qtd(&f, QTD0 + 0x10, QTD0 + 0x20, 36, 1, DATA_ADDR);
    qtd(&f, QTD0 + 0x20, 1, 13, 1, CSW_ADDR);
    fire(&f);
    check_csw_at(&f, CSW_ADDR, 0x44444444u, 36, 1); /* FAILED */
    for (int i = 0; i < 36; i++) assert(f.m.ram[DATA_ADDR + i] == 0);
    fx_done(&f);
    printf("ok csw failed opcode\n");
}

static void test_csw_phase_bad_cbw(void) {
    fx_t f; fx_init(&f);
    cbw(&f, 0x11111111u, 0, 0x80, 0x28, 0, 1);
    uint8_t *p = f.m.ram + CBW_ADDR;
    p[0] = 0x58; p[1] = 0x58; p[2] = 0x58; p[3] = 0x58; /* 'XXXX' */
    qtd(&f, QTD0 + 0x00, QTD0 + 0x10, 31, 0, CBW_ADDR);
    qtd(&f, QTD0 + 0x10, 1, 13, 1, CSW_ADDR);
    fire(&f);
    check_csw_at(&f, CSW_ADDR, 0x11111111u, 0, 2); /* PHASE */
    fx_done(&f);
    printf("ok csw phase bad cbw\n");
}

static void test_firmware_shaped_cbw(void) {
    /* Negative control: the sample firmware's exact recipe (tag=1,
     * READ(10) LBA=0 blocks=1, data at 0x00100000, CSW at 0x00020200). */
    fx_t f; fx_init(&f);
    cbw(&f, 1, 512, 0x80, 0x28, 0, 1);
    qtd(&f, QTD0 + 0x00, QTD0 + 0x10, 31, 0, CBW_ADDR);
    qtd(&f, QTD0 + 0x10, QTD0 + 0x20, 512, 1, 0x00100000u);
    qtd(&f, QTD0 + 0x20, 1, 13, 1, 0x00020200u);
    fire(&f);
    check_csw_at(&f, 0x00020200u, 1, 0, 0);
    assert(f.m.ram[0x00100000] == 7);
    fx_done(&f);
    printf("ok firmware shaped cbw\n");
}

static void test_tur_ready(void) {
    fx_t f; fx_init(&f);
    cbw(&f, 0x55555555u, 0, 0, 0x00, 0, 0); /* TEST UNIT READY: no data */
    qtd(&f, QTD0 + 0x00, QTD0 + 0x10, 31, 0, CBW_ADDR);
    qtd(&f, QTD0 + 0x10, 1, 13, 1, CSW_ADDR);
    fire(&f);
    check_csw_at(&f, CSW_ADDR, 0x55555555u, 0, 0);
    fx_done(&f);
    printf("ok tur ready\n");
}

static void test_tur_no_image(void) {
    /* U2 negative control: no image -> TUR must FAIL, not OK. */
    fx_t f; fx_init(&f);
    f.m.disk_len = 0;
    cbw(&f, 0x66666666u, 0, 0, 0x00, 0, 0);
    qtd(&f, QTD0 + 0x00, QTD0 + 0x10, 31, 0, CBW_ADDR);
    qtd(&f, QTD0 + 0x10, 1, 13, 1, CSW_ADDR);
    fire(&f);
    check_csw_at(&f, CSW_ADDR, 0x66666666u, 0, 1); /* FAILED */
    fx_done(&f);
    printf("ok tur no image failed\n");
}

static void test_request_sense(void) {
    /* The guest's retry loop: TUR fails -> REQUEST SENSE reports
     * NOT READY / MEDIUM NOT PRESENT (2/3A), then the sense is consumed. */
    fx_t f; fx_init(&f);
    f.m.disk_len = 0;
    cbw(&f, 1, 0, 0, 0x00, 0, 0);
    qtd(&f, QTD0 + 0x00, QTD0 + 0x10, 31, 0, CBW_ADDR);
    qtd(&f, QTD0 + 0x10, 1, 13, 1, CSW_ADDR);
    fire(&f);
    check_csw_at(&f, CSW_ADDR, 1, 0, 1);

    cbw(&f, 2, 18, 0x80, 0x03, 0, 0); /* REQUEST SENSE */
    qtd(&f, QTD0 + 0x00, QTD0 + 0x10, 31, 0, CBW_ADDR);
    qtd(&f, QTD0 + 0x10, QTD0 + 0x20, 18, 1, DATA_ADDR);
    qtd(&f, QTD0 + 0x20, 1, 13, 1, CSW_ADDR);
    fire(&f);
    check_csw_at(&f, CSW_ADDR, 2, 0, 0);
    const uint8_t *s = f.m.ram + DATA_ADDR;
    assert(s[0] == 0x70);
    assert(s[2] == 0x02);  /* NOT READY */
    assert(s[7] == 0x0A);
    assert(s[12] == 0x3A); /* MEDIUM NOT PRESENT */
    assert(s[13] == 0x00);

    cbw(&f, 3, 18, 0x80, 0x03, 0, 0); /* second read: sense consumed */
    qtd(&f, QTD0 + 0x00, QTD0 + 0x10, 31, 0, CBW_ADDR);
    qtd(&f, QTD0 + 0x10, QTD0 + 0x20, 18, 1, DATA_ADDR);
    qtd(&f, QTD0 + 0x20, 1, 13, 1, CSW_ADDR);
    fire(&f);
    check_csw_at(&f, CSW_ADDR, 3, 0, 0);
    assert(f.m.ram[DATA_ADDR + 2] == 0x00);
    fx_done(&f);
    printf("ok request sense not ready\n");
}

static void test_inquiry_response(void) {
    fx_t f; fx_init(&f);
    cbw(&f, 0x77777777u, 36, 0x80, 0x12, 0, 0); /* INQUIRY */
    qtd(&f, QTD0 + 0x00, QTD0 + 0x10, 31, 0, CBW_ADDR);
    qtd(&f, QTD0 + 0x10, QTD0 + 0x20, 36, 1, DATA_ADDR);
    qtd(&f, QTD0 + 0x20, 1, 13, 1, CSW_ADDR);
    fire(&f);
    check_csw_at(&f, CSW_ADDR, 0x77777777u, 0, 0);
    const uint8_t *r = f.m.ram + DATA_ADDR;
    assert(r[0] == 0x00);   /* direct-access */
    assert(r[1] == 0x80);   /* removable */
    assert(r[2] == 0x05);   /* SPC-3 */
    assert(r[3] == 0x02);   /* response data format 2 */
    assert(r[4] == 31);     /* additional length */
    assert(memcmp(r + 8, "AURALITE", 8) == 0);
    assert(memcmp(r + 16, "USB DISK        ", 16) == 0);
    assert(memcmp(r + 32, "1.0 ", 4) == 0);
    fx_done(&f);
    printf("ok inquiry response\n");
}

static void test_read_capacity_n(void) {
    fx_t f; fx_init(&f); /* 4096 bytes = 8 sectors */
    cbw(&f, 0x88888888u, 8, 0x80, 0x25, 0, 0); /* READ CAPACITY(10) */
    qtd(&f, QTD0 + 0x00, QTD0 + 0x10, 31, 0, CBW_ADDR);
    qtd(&f, QTD0 + 0x10, QTD0 + 0x20, 8, 1, DATA_ADDR);
    qtd(&f, QTD0 + 0x20, 1, 13, 1, CSW_ADDR);
    fire(&f);
    check_csw_at(&f, CSW_ADDR, 0x88888888u, 0, 0);
    const uint8_t *r = f.m.ram + DATA_ADDR;
    assert(r[0] == 0 && r[1] == 0 && r[2] == 0 && r[3] == 7); /* last LBA 7 */
    assert(r[4] == 0 && r[5] == 0 && r[6] == 0x02 && r[7] == 0); /* 512 */
    fx_done(&f);
    printf("ok read capacity n-block\n");
}

static void test_read_capacity_one(void) {
    fx_t f; fx_init(&f);
    f.m.disk_len = 512; /* exactly one block */
    cbw(&f, 0x99999999u, 8, 0x80, 0x25, 0, 0);
    qtd(&f, QTD0 + 0x00, QTD0 + 0x10, 31, 0, CBW_ADDR);
    qtd(&f, QTD0 + 0x10, QTD0 + 0x20, 8, 1, DATA_ADDR);
    qtd(&f, QTD0 + 0x20, 1, 13, 1, CSW_ADDR);
    fire(&f);
    check_csw_at(&f, CSW_ADDR, 0x99999999u, 0, 0);
    const uint8_t *r = f.m.ram + DATA_ADDR;
    assert(r[0] == 0 && r[1] == 0 && r[2] == 0 && r[3] == 0); /* last LBA 0 */
    fx_done(&f);
    printf("ok read capacity 1-block\n");
}

static void test_illegal_opcode_sense(void) {
    /* Unsupported opcode -> FAILED + ILLEGAL REQUEST sense (5/20). */
    fx_t f; fx_init(&f);
    cbw(&f, 0xAAAAAAAAu, 0, 0, 0xA0, 0, 0); /* REPORT LUNS, no data */
    qtd(&f, QTD0 + 0x00, QTD0 + 0x10, 31, 0, CBW_ADDR);
    qtd(&f, QTD0 + 0x10, 1, 13, 1, CSW_ADDR);
    fire(&f);
    check_csw_at(&f, CSW_ADDR, 0xAAAAAAAAu, 0, 1);

    cbw(&f, 0xBBBBBBBBu, 18, 0x80, 0x03, 0, 0);
    qtd(&f, QTD0 + 0x00, QTD0 + 0x10, 31, 0, CBW_ADDR);
    qtd(&f, QTD0 + 0x10, QTD0 + 0x20, 18, 1, DATA_ADDR);
    qtd(&f, QTD0 + 0x20, 1, 13, 1, CSW_ADDR);
    fire(&f);
    check_csw_at(&f, CSW_ADDR, 0xBBBBBBBBu, 0, 0);
    const uint8_t *s = f.m.ram + DATA_ADDR;
    assert(s[2] == 0x05);  /* ILLEGAL REQUEST */
    assert(s[12] == 0x20); /* INVALID COMMAND OPERATION CODE */
    fx_done(&f);
    printf("ok illegal opcode sense\n");
}

/* ---------------- U3: register file, doorbell, control transfers ---------------- */

static void test_ehci_caps(void) {
    fx_t f; fx_init(&f);
    assert(mem_read(&f.m, EHCI_BASE + 0x00, 1) == 0x00);       /* CAPLENGTH */
    assert(mem_read(&f.m, EHCI_BASE + 0x02, 2) == 0x0200);     /* HCIVERSION 2.00 */
    assert(mem_read(&f.m, EHCI_BASE + 0x04, 4) == 0x11);       /* N_PORTS=1 | PPC */
    assert(mem_read(&f.m, EHCI_BASE + 0x08, 4) == 0x00);       /* HCCPARAMS: 32-bit */
    fx_done(&f);
    printf("ok ehci caps\n");
}

static void test_ehci_halt_reset(void) {
    fx_t f; fx_init(&f);
    mem_write(&f.m, EHCI_BASE + 0x00, 4, 0x00);                /* USBCMD=0 (op mode) */
    assert(mem_read(&f.m, EHCI_BASE + 0x04, 4) & (1u << 12));  /* HCHALTED at rest */
    mem_write(&f.m, EHCI_BASE + 0x00, 4, 0x21);                /* Run|ASE */
    uint32_t st = (uint32_t)mem_read(&f.m, EHCI_BASE + 0x04, 4);
    assert(!(st & (1u << 12)));                                /* halted cleared */
    assert(st & (1u << 15));                                   /* ASS: async active */
    mem_write(&f.m, EHCI_BASE + 0x00, 4, 0x00);                /* stop */
    assert(mem_read(&f.m, EHCI_BASE + 0x04, 4) & (1u << 12));  /* halted again */
    mem_write(&f.m, EHCI_BASE + 0x00, 4, 0x02);                /* HCRESET */
    assert(!(mem_read(&f.m, EHCI_BASE + 0x00, 4) & 0x02));     /* self-cleared */
    assert(mem_read(&f.m, EHCI_BASE + 0x04, 4) & (1u << 12));  /* halted after reset */
    fx_done(&f);
    printf("ok ehci halt handshake\n");
}

static void test_doorbell_usbcmd_only(void) {
    /* U3 negative control: non-USBCMD register writes run no schedule. */
    fx_t f; fx_init(&f);
    cbw(&f, 0x12121212u, 512, 0x80, 0x28, 0, 1);
    qtd(&f, QTD0 + 0x00, QTD0 + 0x10, 31, 0, CBW_ADDR);
    qtd(&f, QTD0 + 0x10, QTD0 + 0x20, 512, 1, DATA_ADDR);
    qtd(&f, QTD0 + 0x20, 1, 13, 1, CSW_ADDR);
    uint8_t *qh = f.m.ram + QH_ADDR;
    w32r(qh + 0x0C, QTD0);
    mem_write(&f.m, EHCI_BASE + 0x18, 4, QH_ADDR);   /* ASYNCLISTADDR */
    mem_write(&f.m, EHCI_BASE + 0x04, 4, 0);         /* USBSTS */
    mem_write(&f.m, EHCI_BASE + 0x44, 4, 0x1000);    /* PORTSC PP */
    assert(r32r(f.m.ram + QTD0 + 8) & 0x80);         /* still Active: no walk */
    mem_write(&f.m, EHCI_BASE + 0x00, 4, 0x21);      /* USBCMD: walks now */
    assert(!(r32r(f.m.ram + QTD0 + 8) & 0x80));      /* completed */
    check_csw_at(&f, CSW_ADDR, 0x12121212u, 0, 0);
    fx_done(&f);
    printf("ok doorbell usbcmd only\n");
}

static void test_ehci_portsc(void) {
    fx_t f; fx_init(&f);
    /* Unpowered port has no device (PPC gating). */
    assert(!(mem_read(&f.m, EHCI_BASE + 0x44, 4) & 0x1));
    mem_write(&f.m, EHCI_BASE + 0x44, 4, 0x1000);            /* power on (PP) */
    uint32_t ps = (uint32_t)mem_read(&f.m, EHCI_BASE + 0x44, 4);
    assert(ps & 0x1);                                        /* CCS */
    assert(ps & 0x2);                                        /* CSC latched */
    assert((ps >> 10 & 3) == 2);                             /* J-state */
    mem_write(&f.m, EHCI_BASE + 0x44, 4, 0x1002);            /* W1C: clear CSC */
    assert(!(mem_read(&f.m, EHCI_BASE + 0x44, 4) & 0x2));
    /* Reset dance: PR set then released -> PED + PEC. */
    mem_write(&f.m, EHCI_BASE + 0x44, 4, 0x1100);            /* PR */
    assert(mem_read(&f.m, EHCI_BASE + 0x44, 4) & 0x100);
    mem_write(&f.m, EHCI_BASE + 0x44, 4, 0x1000);            /* PR released */
    ps = (uint32_t)mem_read(&f.m, EHCI_BASE + 0x44, 4);
    assert(ps & 0x4);                                        /* PED */
    assert(ps & 0x8);                                        /* PEC latched */
    /* Owner bit releases the port to a companion. */
    mem_write(&f.m, EHCI_BASE + 0x44, 4, 0x3000);            /* PP|OWNER */
    ps = (uint32_t)mem_read(&f.m, EHCI_BASE + 0x44, 4);
    assert(!(ps & 0x1));                                     /* no device */
    assert(ps & (1u << 13));                                 /* owner */
    fx_done(&f);
    printf("ok ehci portsc\n");
}

static void setup_pkt(fx_t *f, uint32_t addr, uint8_t type, uint8_t req,
                      uint16_t value, uint16_t index, uint16_t length) {
    uint8_t *p = f->m.ram + addr;
    p[0] = type; p[1] = req;
    p[2] = (uint8_t)value; p[3] = (uint8_t)(value >> 8);
    p[4] = (uint8_t)index; p[5] = (uint8_t)(index >> 8);
    p[6] = (uint8_t)length; p[7] = (uint8_t)(length >> 8);
}

static void run_control(fx_t *f, int with_data, uint32_t data_total, uint32_t data_buf) {
    qtd(f, QTD0 + 0x00, QTD0 + 0x10, 8, 2, CBW_ADDR);         /* SETUP */
    if (with_data) {
        qtd(f, QTD0 + 0x10, QTD0 + 0x20, data_total, 1, data_buf);
        qtd(f, QTD0 + 0x20, 1, 0, 1, CSW_ADDR);               /* status: 0-byte IN */
    } else {
        qtd(f, QTD0 + 0x10, 1, 0, 1, CSW_ADDR);
    }
    fire(f);
}

static void test_control_get_device(void) {
    fx_t f; fx_init(&f);
    setup_pkt(&f, CBW_ADDR, 0x80, 0x06, 0x0100, 0, 18);
    run_control(&f, 1, 18, DATA_ADDR);
    const uint8_t *d = f.m.ram + DATA_ADDR;
    assert(d[0] == 18 && d[1] == 1);
    assert(d[2] == 0x00 && d[3] == 0x02);                     /* bcdUSB 2.00 */
    assert(d[7] == 64);                                      /* bMaxPacketSize0 */
    assert(d[8] == 0x25 && d[9] == 0x05);                    /* idVendor 0x0525 */
    assert(d[10] == 0xA5 && d[11] == 0xA4);                  /* idProduct 0xa4a5 */
    fx_done(&f);
    printf("ok control get device descriptor\n");
}

static void test_control_get_config(void) {
    fx_t f; fx_init(&f);
    setup_pkt(&f, CBW_ADDR, 0x80, 0x06, 0x0200, 0, 32);
    run_control(&f, 1, 32, DATA_ADDR);
    const uint8_t *d = f.m.ram + DATA_ADDR;
    assert(d[0] == 9 && d[1] == 2);
    assert(d[2] == 32 && d[3] == 0);                         /* wTotalLength 32 */
    assert(d[14] == 0x08 && d[15] == 0x06 && d[16] == 0x50); /* MSC/SCSI/BOT */
    assert(d[18 + 2] == 0x81 && d[18 + 3] == 2);             /* bulk IN 512 */
    assert(d[25 + 2] == 0x02 && d[25 + 3] == 2);             /* bulk OUT 512 */
    fx_done(&f);
    printf("ok control get config descriptor\n");
}

static void test_control_string0(void) {
    fx_t f; fx_init(&f);
    setup_pkt(&f, CBW_ADDR, 0x80, 0x06, 0x0300, 0, 4);
    run_control(&f, 1, 4, DATA_ADDR);
    const uint8_t *d = f.m.ram + DATA_ADDR;
    assert(d[0] == 4 && d[1] == 3 && d[2] == 0x09 && d[3] == 0x04);
    fx_done(&f);
    printf("ok control string0\n");
}

static void test_control_set_address_max_lun(void) {
    fx_t f; fx_init(&f);
    setup_pkt(&f, CBW_ADDR, 0x00, 0x05, 7, 0, 0);            /* SET_ADDRESS 7 */
    run_control(&f, 0, 0, 0);
    assert(!(r32r(f.m.ram + QTD0 + 8) & 0x80));              /* completed */
    setup_pkt(&f, CBW_ADDR, 0xA1, 0xFE, 0, 0, 1);            /* GET_MAX_LUN */
    run_control(&f, 1, 1, DATA_ADDR);
    assert(f.m.ram[DATA_ADDR] == 0);                         /* one LUN */
    fx_done(&f);
    printf("ok control set address + max lun\n");
}

/* ---------------- U4: UHCI register file + TD walk ---------------- */

#define UHCI_IO   0xC040u
#define UHCI_FL   0x10000u   /* frame list page */
#define UHCI_QH   0x20000u
#define UHCI_TD0  0x20040u

static uint32_t uhci_tok(uint8_t pid, uint8_t dev, uint8_t ep, int dt, uint32_t len) {
    uint32_t t = pid | ((uint32_t)dev << 8) | ((uint32_t)ep << 15) | ((uint32_t)(dt & 1) << 19);
    t |= (len == 0 ? 0x7FFu : ((len - 1) & 0x7FF)) << 21;
    return t;
}

static void uhci_td(fx_t *f, uint32_t a, uint32_t link, uint32_t ctrl,
                    uint32_t token, uint32_t buf) {
    w32r(f->m.ram + a + 0, link);
    w32r(f->m.ram + a + 4, ctrl);
    w32r(f->m.ram + a + 8, token);
    w32r(f->m.ram + a + 12, buf);
}

static void uhci_arm(fx_t *f) {
    for (uint32_t i = 0; i < 1024; i++) w32r(f->m.ram + UHCI_FL + 4*i, UHCI_QH | 2);
    w32r(f->m.ram + UHCI_QH + 0, 0x1);            /* head link: T */
    w32r(f->m.ram + UHCI_QH + 4, UHCI_TD0);       /* element link */
    io_write(&f->m, UHCI_IO + 0x08, 4, UHCI_FL);  /* FLBASEADD */
    io_write(&f->m, UHCI_IO + 0x06, 2, 0);        /* FRNUM = 0 */
}

static void test_uhci_regs(void) {
    fx_t f; fx_init(&f);
    /* halt handshake in the guest's bit map (HCHALTED = bit 5, measured) */
    assert(io_read(&f.m, UHCI_IO + 0x02, 2) & (1u << 5));
    io_write(&f.m, UHCI_IO + 0x00, 2, 0x00C1);    /* RUN|CF|MAXPACKET */
    assert(!(io_read(&f.m, UHCI_IO + 0x02, 2) & (1u << 5)));
    io_write(&f.m, UHCI_IO + 0x00, 2, 0x0000);    /* stop */
    assert(io_read(&f.m, UHCI_IO + 0x02, 2) & (1u << 5));
    io_write(&f.m, UHCI_IO + 0x00, 2, 0x0002);    /* HCRESET */
    assert(io_read(&f.m, UHCI_IO + 0x00, 2) == 0);
    assert(io_read(&f.m, UHCI_IO + 0x02, 2) & (1u << 5));
    /* port 0 has the stick, port 1 is empty; reset dance latches + W1C */
    assert(io_read(&f.m, UHCI_IO + 0x10, 2) & 0x1);   /* CCS */
    assert(!(io_read(&f.m, UHCI_IO + 0x12, 2) & 0x1));
    io_write(&f.m, UHCI_IO + 0x10, 2, 0x0200);        /* PR */
    assert(io_read(&f.m, UHCI_IO + 0x10, 2) & 0x200);
    io_write(&f.m, UHCI_IO + 0x10, 2, 0x0000);        /* PR released */
    uint32_t ps = io_read(&f.m, UHCI_IO + 0x10, 2);
    assert(ps & 0x4);                                 /* PED */
    assert((ps & 0x2) && (ps & 0x8));                 /* CSC|ECSC latched */
    io_write(&f.m, UHCI_IO + 0x10, 2, 0x000A);        /* W1C CSC|ECSC */
    ps = io_read(&f.m, UHCI_IO + 0x10, 2);
    assert(!(ps & 0x2) && !(ps & 0x8) && (ps & 0x4));
    fx_done(&f);
    printf("ok uhci regs\n");
}

static void test_uhci_td_walk(void) {
    /* Control: GET_DESCRIPTOR device over the UHCI TD/QH walker. */
    fx_t f; fx_init(&f);
    setup_pkt(&f, 0x30000, 0x80, 0x06, 0x0100, 0, 18);
    uint32_t ctrl = 0x800000u | (3u << 27);           /* ACTIVE | CERR=3 */
    uhci_td(&f, UHCI_TD0 + 0x00, UHCI_TD0 + 0x10, ctrl, uhci_tok(0x2D, 1, 0, 0, 8), 0x30000);
    uhci_td(&f, UHCI_TD0 + 0x10, UHCI_TD0 + 0x20, ctrl, uhci_tok(0x69, 1, 0, 1, 18), 0x30100);
    uhci_td(&f, UHCI_TD0 + 0x20, 0x1, ctrl, uhci_tok(0xE1, 1, 0, 1, 0), 0);
    uhci_arm(&f);
    io_write(&f.m, UHCI_IO + 0x00, 2, 0x00C1);        /* RUN|CF|MAXPACKET */
    const uint8_t *d = f.m.ram + 0x30100;
    assert(d[0] == 18 && d[1] == 1 && d[2] == 0x00 && d[3] == 0x02);
    assert(d[7] == 64 && d[8] == 0x25 && d[9] == 0x05);
    for (int i = 0; i < 3; i++)                       /* Active cleared */
        assert(!(r32r(f.m.ram + UHCI_TD0 + 16*i + 4) & 0x800000u));
    assert((r32r(f.m.ram + UHCI_TD0 + 0x10 + 4) & 0x7FF) == 17);  /* len-1 */
    assert(r32r(f.m.ram + UHCI_QH + 4) == 0x1);       /* drained to T */

    /* Bulk: BOT READ(10) with the UHCI 64-byte chunk discipline. */
    cbw(&f, 0x51515151u, 512, 0x80, 0x28, 0, 1);
    uhci_td(&f, UHCI_TD0 + 0x00, UHCI_TD0 + 0x10, ctrl, uhci_tok(0xE1, 1, 2, 0, 31), CBW_ADDR);
    for (int i = 0; i < 8; i++)
        uhci_td(&f, UHCI_TD0 + 0x10 + 16*i, UHCI_TD0 + 0x20 + 16*i, ctrl,
                uhci_tok(0x69, 1, 1, i & 1, 64), DATA_ADDR + 64*i);
    uhci_td(&f, UHCI_TD0 + 0x90, 0x1, ctrl, uhci_tok(0x69, 1, 2, 1, 13), CSW_ADDR);
    w32r(f.m.ram + UHCI_QH + 4, UHCI_TD0);
    io_write(&f.m, UHCI_IO + 0x00, 2, 0x00C1);
    for (int i = 0; i < 512; i++)
        assert(f.m.ram[DATA_ADDR + i] == f.m.disk[i]);  /* byte-for-byte */
    check_csw_at(&f, CSW_ADDR, 0x51515151u, 0, 0);
    fx_done(&f);
    printf("ok uhci td walk\n");
}

static void test_uhci_halted_no_frames(void) {
    /* U4 negative control: the schedule only runs while the HC runs. */
    fx_t f; fx_init(&f);
    uint32_t ctrl = 0x800000u | (3u << 27);
    uhci_td(&f, UHCI_TD0 + 0x00, 0x1, ctrl, uhci_tok(0xE1, 1, 2, 0, 31), CBW_ADDR);
    cbw(&f, 0x13131313u, 512, 0x80, 0x28, 0, 1);
    uhci_arm(&f);
    io_write(&f.m, UHCI_IO + 0x00, 2, 0x0040);        /* CF only: halted */
    for (int i = 0; i < 8; i++) devices_tick(&f.m);
    assert(r32r(f.m.ram + UHCI_TD0 + 4) & 0x800000u); /* still Active */
    assert(r32r(f.m.ram + UHCI_QH + 4) == UHCI_TD0);  /* no advance */
    assert(io_read(&f.m, UHCI_IO + 0x02, 2) & (1u << 5));
    io_write(&f.m, UHCI_IO + 0x00, 2, 0x00C1);        /* RUN: frames go */
    assert(!(r32r(f.m.ram + UHCI_TD0 + 4) & 0x800000u));
    assert(r32r(f.m.ram + UHCI_QH + 4) == 0x1);
    fx_done(&f);
    printf("ok uhci halted runs no frames\n");
}

static void test_uhci_omitted(void) {
    /* USB U5: --no-usb-uhci machine shape -- the EHCI-only QEMU lane of
     * test_usb_ehci.sh (-device usb-ehci, no -usb).  The companion
     * function and its I/O window are absent: the port floats high like
     * unconnected hardware (io.c open-bus), no register file consumes
     * USBCMD writes, and EHCI keeps its full model. */
    fx_t f;
    setup_machine(&f.m);
    f.m.cfg_no_uhci = 1;
    devices_init_common(&f.m);
    for (int i = 0; i < (int)sizeof f.stick; i++)
        f.stick[i] = (uint8_t)(i * 3 + 7);
    f.m.disk = f.stick;
    f.m.disk_len = sizeof f.stick;
    assert(io_read(&f.m, UHCI_IO + 0x00, 2) == 0xFFFF);   /* open bus */
    assert(io_read(&f.m, UHCI_IO + 0x02, 2) == 0xFFFF);
    io_write(&f.m, UHCI_IO + 0x00, 2, 0x00C1);            /* RUN: nobody listens */
    assert(io_read(&f.m, UHCI_IO + 0x02, 2) == 0xFFFF);   /* no halt handshake */
    assert(io_read(&f.m, UHCI_IO + 0x10, 2) == 0xFFFF);   /* no port model */
    assert(mem_read(&f.m, EHCI_BASE + 0x02, 2) == 0x0200); /* EHCI alive */
    mem_write(&f.m, EHCI_BASE + 0x00, 4, 0x21);            /* Run|ASE */
    assert(!(mem_read(&f.m, EHCI_BASE + 0x04, 4) & (1u << 12)));
    fx_done(&f);
    printf("ok uhci omitted\n");
}

int main(void) {
    test_be16();
    test_csw_tag_echo();
    test_csw_residue_short_data();
    test_csw_residue_no_image();
    test_csw_failed_opcode();
    test_csw_phase_bad_cbw();
    test_firmware_shaped_cbw();
    test_tur_ready();
    test_tur_no_image();
    test_request_sense();
    test_inquiry_response();
    test_read_capacity_n();
    test_read_capacity_one();
    test_illegal_opcode_sense();
    test_ehci_caps();
    test_ehci_halt_reset();
    test_doorbell_usbcmd_only();
    test_ehci_portsc();
    test_control_get_device();
    test_control_get_config();
    test_control_string0();
    test_control_set_address_max_lun();
    test_uhci_regs();
    test_uhci_td_walk();
    test_uhci_halted_no_frames();
    test_uhci_omitted();
    printf("test_usb: ALL PASS\n");
    return 0;
}
