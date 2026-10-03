/* tests/test_rtc.c -- CHIPSET H3 vectors: MC146818A RTC/CMOS at 0x70/0x71.
 *
 * At baseline these ports were unclaimed: writes vanished, reads floated
 * 0xFF (measured). Everything below is exact deterministic virtual time
 * (D6): one second == 1193182 ticks == 14318184 retired instructions
 * (instr_per_tick=12), epoch pinned at 2026-01-01 00:00:00 UTC.
 */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "machine.h"
#include "platform.h"
#include "pic.h"
#include "rtc.h"
#include "harness.h"

/* Tests run the RTC's virtual clock at instr_per_tick=1 (12x faster wall
 * time, EXACT same deterministic semantics -- the divisor is an explicit
 * parameter). The production pin ipt=12 is asserted in the defaults test. */
#define IPT 1ull
#define SEC_INSTR (1193182ull * IPT)
#define UIP_INSTR (2048ull * IPT)

typedef struct { machine_t m; } fx_t;
static void fx_init(fx_t *f) {
    setup_machine(&f->m);
    pic_init(&f->m); pic_io_register(&f->m);
    rtc_init(&f->m); rtc_io_register(&f->m);
    f->m.rtc.instr_per_tick = (uint32_t)IPT;
    /* a real infinite spin: an empty-RAM "spin" (00 00 = add [mem],al)
     * walks IP forward and eventually faults into a frozen machine --
     * measured in bring-up. Virtual time only flows while the guest runs. */
    f->m.ram[0] = 0xEB; f->m.ram[1] = 0xFE;
    f->m.cpu.rip = 0;
}
static void fx_steps(fx_t *f, uint64_t n) { for (uint64_t i = 0; i < n; i++) cpu_step(&f->m.cpu); }

/* select CMOS register `sel`, read data port */
static uint32_t cmos(fx_t *f, uint8_t sel) {
    io_write(&f->m, 0x70, 1, sel);
    return io_read(&f->m, 0x71, 1);
}
static void cmos_w(fx_t *f, uint8_t sel, uint8_t v) {
    io_write(&f->m, 0x70, 1, sel);
    io_write(&f->m, 0x71, 1, v);
}

static void test_cmos_content_and_ports(void) {
    fx_t f; fx_init(&f);
    assert(f.m.rtc.instr_per_tick == (uint32_t)IPT);   /* test override */
    { /* the production pin is 12 (D6, pinned in rtc_init) */
        fx_t p; setup_machine(&p.m); rtc_init(&p.m);
        assert(p.m.rtc.instr_per_tick == 12); }
    /* real data where the baseline floated 0xFF */
    assert(cmos(&f, 0x15) == 0x80 && cmos(&f, 0x16) == 0x02);   /* base = 640 KB */
    assert(cmos(&f, 0x14) == 0x10);                            /* equipment byte */
    unsigned ext = cmos(&f, 0x17) | (cmos(&f, 0x18) << 8);
    assert(ext == (RAM_SIZE / 1024 - 1024 > 65535 ? 65535 : RAM_SIZE / 1024 - 1024));
    assert(cmos(&f, 0x30) == (ext & 0xFF) && cmos(&f, 0x31) == (ext >> 8));
    assert(cmos(&f, 0x2E) == 0 && cmos(&f, 0x2F) == 0);        /* checksum: zero */
    /* epoch-pinned clock, BCD: 2026-01-01 00:00:00 Thursday */
    assert(cmos(&f, 0x00) == 0x00 && cmos(&f, 0x02) == 0x00 && cmos(&f, 0x04) == 0x00);
    assert(cmos(&f, 0x07) == 0x01 && cmos(&f, 0x08) == 0x01);
    assert(cmos(&f, 0x09) == 0x26 && cmos(&f, 0x32) == 0x20);
    assert(cmos(&f, 0x06) == 0x05);                            /* Thursday */
    assert(cmos(&f, 0x0D) == 0x80);                            /* VART set */
    /* index port: bit7 echoed (NMI-disable bit stored), 6:0 select works */
    io_write(&f.m, 0x70, 1, 0x80 | 0x15);
    assert(io_read(&f.m, 0x71, 1) == 0x80);
    /* RAM byte round-trip; computed regs ignore writes */
    cmos_w(&f, 0x50, 0xA5);
    assert(cmos(&f, 0x50) == 0xA5);
    cmos_w(&f, 0x00, 0x55);
    assert(cmos(&f, 0x00) == 0x00);                            /* clock is computed */
    assert(cmos(&f, 0x0A) == 0x26);                            /* reg A pin, UIP=0 now */
    assert(cmos(&f, 0x0B) == 0x02);                            /* reg B pin: 24h/BCD */
}

static void test_time_advances_bcd_exact(void) {
    fx_t f; fx_init(&f);
    fx_steps(&f, SEC_INSTR + 8000 * IPT);          /* 1 s + margin past UIP */
    assert(cmos(&f, 0x00) == 0x01);
    assert(cmos(&f, 0x02) == 0x00 && cmos(&f, 0x04) == 0x00);
    /* 61.4 s more -> 62.04 s total: seconds wrap into minutes: 01:02 */
    fx_steps(&f, 61 * SEC_INSTR + 32000 * IPT);
    assert(cmos(&f, 0x00) == 0x02);
    assert(cmos(&f, 0x02) == 0x01);              /* BCD 01, not binary */
    assert(cmos(&f, 0x04) == 0x00);
}

/* UIP window: during the first 2048 ticks of a second the clock still
 * shows the PREVIOUS second; it commits when UIP drops. Exact phases. */
static void test_uip_freeze_then_commit(void) {
    fx_t f; fx_init(&f);
    fx_steps(&f, SEC_INSTR + SEC_INSTR / 2);     /* 1.5 s: mid-second, no UIP */
    assert((cmos(&f, 0x0A) & 0x80) == 0);
    assert(cmos(&f, 0x00) == 0x01);
    fx_steps(&f, SEC_INSTR / 2 + 1000 * IPT);    /* cross boundary, inside UIP */
    assert(cmos(&f, 0x0A) & 0x80);               /* UIP up */
    assert(cmos(&f, 0x00) == 0x01);              /* still the previous second */
    fx_steps(&f, UIP_INSTR + 2000 * IPT);        /* window closed */
    assert((cmos(&f, 0x0A) & 0x80) == 0);
    assert(cmos(&f, 0x00) == 0x02);              /* committed now */
}

/* register C flags lifecycle with UIE=0 (no IRQF, no IRQ8) */
static void test_regc_flags_no_uie(void) {
    fx_t f; fx_init(&f);
    assert(cmos(&f, 0x0C) == 0x00);
    fx_steps(&f, SEC_INSTR + UIP_INSTR + 4000 * IPT); /* past first update */
    uint32_t c = cmos(&f, 0x0C);
    assert(c == 0x10);                           /* UF set, IRQF clear (UIE=0) */
    assert(cmos(&f, 0x0C) == 0x00);              /* read clears */
    assert(!pic_pending(&f.m));                  /* no IRQ8 without UIE */
}

/* IRQ8 with UIE=1: once per virtual second, through the slave cascade. */
static void test_irq8_update_ended(void) {
    fx_t f; fx_init(&f);
    /* PIC pair: master base 0x20 / slave 0x28, unmask cascade + slave IRQ0 */
    const uint8_t init[] = {
        0xB0,0x11, 0xE6,0x20, 0xB0,0x20, 0xE6,0x21, 0xB0,0x04, 0xE6,0x21, 0xB0,0x01, 0xE6,0x21,
        0xB0,0x11, 0xE6,0xA0, 0xB0,0x28, 0xE6,0xA1, 0xB0,0x02, 0xE6,0xA1, 0xB0,0x01, 0xE6,0xA1,
        0xB0,0xFB, 0xE6,0x21, 0xB0,0xFE, 0xE6,0xA1,
        0xFB, 0xC6,0x06,0x01,0x20,0x01, 0xEB,0xFE,   /* sti; mark; spin */
    };
    memcpy(f.m.ram, init, sizeof init);
    f.m.cpu.rip = 0;
    /* vector 0x28: inc [0x2000]; eoi slave+master; iret */
    const uint8_t h[] = { 0xFE,0x06,0x00,0x20, 0xB0,0x20, 0xE6,0xA0, 0xE6,0x20, 0xCF };
    mem_write(&f.m, 0x28 * 4, 2, 0x0100); mem_write(&f.m, 0x28 * 4 + 2, 2, 0);
    memcpy(f.m.ram + 0x0100, h, sizeof h);
    /* enable UIE: reg B = 24h|UIE = 0x12 */
    cmos_w(&f, 0x0B, 0x12);
    assert(cmos(&f, 0x0B) == 0x12);
    fx_steps(&f, SEC_INSTR + UIP_INSTR + 4000 * IPT);   /* just past first update */
    assert(mem_read(&f.m, 0x2000, 1) == 1);        /* exactly one IRQ8 */
    uint32_t c = cmos(&f, 0x0C);
    assert(c == 0x90);                             /* UF + IRQF, read-clears */
    assert(cmos(&f, 0x0C) == 0x00);
    fx_steps(&f, 2 * SEC_INSTR + SEC_INSTR / 2);   /* ~2.5 s more */
    assert(mem_read(&f.m, 0x2000, 1) == 3);        /* two more seconds */
}

int main(void) {
    test_cmos_content_and_ports();
    test_time_advances_bcd_exact();
    test_uip_freeze_then_commit();
    test_regc_flags_no_uie();
    test_irq8_update_ended();
    puts("rtc tests: ok");
    return 0;
}
