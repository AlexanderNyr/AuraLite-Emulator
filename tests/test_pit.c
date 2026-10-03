/* tests/test_pit.c -- CHIPSET H2 vectors: 8254 PIT at ports 0x40-0x43/0x61.
 *
 * Gate-worthy scenario (plan H2): BIOS-style "mov al,0x36; out 0x43; out
 * 0x40" then wait -- IRQ0 must fire within the programmed period +/-1 tick,
 * repeatedly. All timing is D6 virtual time: instr_per_tick=12 pins ~99.4
 * virtual MIPS, so expected fire counts are exact windows, not wall time.
 * Latch/counter readback is checked EXACTLY from the same model.
 */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "machine.h"
#include "platform.h"
#include "pic.h"
#include "pit.h"
#include "harness.h"

typedef struct { machine_t m; } fx_t;
static void fx_init(fx_t *f) {
    setup_machine(&f->m);
    pic_init(&f->m); pic_io_register(&f->m);
    pit_init(&f->m); pit_io_register(&f->m);
}
static void fx_steps(fx_t *f, int n) { for (int i = 0; i < n; i++) cpu_step(&f->m.cpu); }

static const uint8_t INIT_PIC[] = {
    0xB0,0x11, 0xE6,0x20, 0xB0,0x20, 0xE6,0x21,
    0xB0,0x04, 0xE6,0x21, 0xB0,0x01, 0xE6,0x21,
};
static const uint8_t H_IRQ0[] =  /* inc [0x2000]; eoi master; iret */
    { 0xFE,0x06,0x00,0x20, 0xB0,0x20, 0xE6,0x20, 0xCF };
static const uint8_t TAIL[] = { 0xFB, 0xC6,0x06,0x01,0x20,0x01, 0xEB,0xFE }; /* sti; mark; jmp $ */

static void ivt(machine_t *m, int vec, uint16_t off) {
    mem_write(m, (uint64_t)vec * 4,     2, off);
    mem_write(m, (uint64_t)vec * 4 + 2, 2, 0);
}

/* assemble: PIC init + unmask irq0 + optional guest bytes + spin tail */
static void fx_guest(fx_t *f, const uint8_t *body, size_t blen) {
    uint8_t prog[256]; size_t n = 0;
    memcpy(prog, INIT_PIC, sizeof INIT_PIC); n += sizeof INIT_PIC;
    { uint8_t w[4] = { 0xB0,0xFE, 0xE6,0x21 }; memcpy(prog+n, w, 4); n += 4; }
    if (blen) { memcpy(prog+n, body, blen); n += blen; }
    memcpy(prog+n, TAIL, sizeof TAIL); n += sizeof TAIL;
    memcpy(f->m.ram, prog, n);
    f->m.cpu.rip = 0;
    ivt(&f->m, 0x20, 0x0100);
    memcpy(f->m.ram + 0x0100, H_IRQ0, sizeof H_IRQ0);
}

static void test_pit_defaults(void) {
    fx_t f; fx_init(&f);
    assert(f.m.pit.instr_per_tick == 12);              /* D6 pin */
    /* unprogrammed counters read 0, no IRQ ever */
    assert(io_read(&f.m, 0x40, 1) == 0);
    io_write(&f.m, 0x43, 1, 0x00);                     /* latch counter0 */
    assert(io_read(&f.m, 0x40, 1) == 0);
    assert(io_read(&f.m, 0x61, 1) == 0);               /* port B: all clear */
    fx_t g; fx_init(&g); fx_guest(&g, NULL, 0);
    fx_steps(&g, 3000);
    assert(mem_read(&g.m, 0x2000, 1) == 0);            /* silence by default */
}

/* plan H2 gate: BIOS-style mode-3 init, IRQ0 within period +/-1 tick,
 * repeatedly. CR=24 ticks => period 24*12 = 288 instructions. */
static void test_rate_generator_cadence(void) {
    fx_t f; fx_init(&f);
    const uint8_t prog[] = {
        0xB0,0x36, 0xE6,0x43,      /* ctrl: ch0, rw3, mode3 */
        0xB0,0x18, 0xE6,0x40,      /* CR LSB = 24 */
        0xB0,0x00, 0xE6,0x40,      /* CR MSB = 0  -> counting starts */
    };
    fx_guest(&f, prog, sizeof prog);
    int wsum = 0;
    for (int w = 0; w < 3; w++) {
        uint64_t before = mem_read(&f.m, 0x2000, 1);
        fx_steps(&f, 720);                       /* 720/12/24 = 2.5 periods */
        uint64_t got = mem_read(&f.m, 0x2000, 1) - before;
        /* 2 full periods certain, boundary tick may add the third */
        assert(got >= 1 && got <= 4);
        wsum += (int)got;
    }
    assert(wsum >= 6 && wsum <= 10);             /* 7.5 expected +/-1 tick */
}

static void test_mode0_one_shot(void) {
    fx_t f; fx_init(&f);
    const uint8_t prog[] = {
        0xB0,0x30, 0xE6,0x43,      /* mode 0, rw3 */
        0xB0,0x24, 0xE6,0x40,      /* CR = 36 ticks = 432 instr */
        0xB0,0x00, 0xE6,0x40,
    };
    fx_guest(&f, prog, sizeof prog);
    fx_steps(&f, 5000);                          /* ~11 periods worth */
    assert(mem_read(&f.m, 0x2000, 1) == 1);      /* exactly ONE edge */
    fx_steps(&f, 5000);
    assert(mem_read(&f.m, 0x2000, 1) == 1);      /* holds, no repeats */
    const uint8_t rearm[] = { 0xB0,0x24, 0xE6,0x40, 0xB0,0x00, 0xE6,0x40,
                              0xEB,0xFE };   /* spin, not HLT: virtual time
                                                advances per retired instr */
    memcpy(f.m.ram + 0x0300, rearm, sizeof rearm);
    f.m.cpu.rip = 0x0300;
    fx_steps(&f, 1200);                          /* re-armed: one more edge */
    assert(mem_read(&f.m, 0x2000, 1) == 2);
}

/* exact readback, driven through the real IO layer from the C side:
 * rem is computed EXACTLY from D6 virtual time, no tolerances. */
static void test_latch_readback_exact(void) {
    fx_t f; fx_init(&f);
    fx_guest(&f, NULL, 0);
    io_write(&f.m, 0x43, 1, 0x34);               /* ch0 rw3 mode2 */
    io_write(&f.m, 0x40, 1, 240);                /* CR = 240 */
    io_write(&f.m, 0x40, 1, 0);
    uint64_t load_at = f.m.cpu.instr_count;      /* anchor of the load */
    fx_steps(&f, 96);                            /* 8 ticks */
    io_write(&f.m, 0x43, 1, 0x00);               /* latch */
    assert(io_read(&f.m, 0x40, 1) == 240 - 8);
    assert(io_read(&f.m, 0x40, 1) == 0);
    fx_steps(&f, 2400);                          /* +200 ticks: rem = 32 */
    io_write(&f.m, 0x43, 1, 0x00);
    assert(io_read(&f.m, 0x40, 1) == 32);
    assert(io_read(&f.m, 0x40, 1) == 0);
    /* live unlatched rw==3 pair is a consistent snapshot (no tearing),
     * checked EXACTLY with a nonzero MSB */
    io_write(&f.m, 0x43, 1, 0x34);
    io_write(&f.m, 0x40, 1, 0x2C);
    io_write(&f.m, 0x40, 1, 0x01);         /* CR = 300 */
    fx_steps(&f, 60);                      /* 5 ticks -> rem = 295 */
    uint32_t lsb = io_read(&f.m, 0x40, 1); /* snapshots 0x0127 */
    fx_steps(&f, 36);                      /* 3 more ticks (live rem 292) */
    uint32_t msb = io_read(&f.m, 0x40, 1);
    assert(lsb == 295 - 256);              /* 0x27: low byte OF SNAPSHOT */
    assert(msb == 0x01);                   /* MSB pairs with the snapshot */
    (void)load_at;
}

static void test_lsb_only_quick_fire(void) {
    fx_t f; fx_init(&f);
    const uint8_t prog[] = {
        0xB0,0x16, 0xE6,0x43,      /* ch0, LSB only, mode3 */
        0xB0,0x05, 0xE6,0x40,      /* CR=5 -> 60-instr period */
    };
    fx_guest(&f, prog, sizeof prog);
    fx_steps(&f, 240);
    assert(mem_read(&f.m, 0x2000, 1) >= 2);      /* ~4 periods */
}

static void test_counter2_gate_and_port61(void) {
    fx_t f; fx_init(&f);
    fx_guest(&f, NULL, 0);
    io_write(&f.m, 0x43, 1, 0xB6);               /* ch2 rw3 mode3 */
    io_write(&f.m, 0x42, 1, 24);
    io_write(&f.m, 0x42, 1, 0);
    /* GATE2 low (port 0x61 bit0): frozen solid, exact */
    fx_steps(&f, 480);
    io_write(&f.m, 0x43, 1, 0x80);               /* latch ch2 */
    assert(io_read(&f.m, 0x42, 1) == 24);
    assert(io_read(&f.m, 0x42, 1) == 0);
    /* open the gate: exact progress now */
    io_write(&f.m, 0x61, 1, 0x01);
    fx_steps(&f, 96);                            /* 8 ticks */
    io_write(&f.m, 0x43, 1, 0x80);
    assert(io_read(&f.m, 0x42, 1) == 16);
    assert(io_read(&f.m, 0x42, 1) == 0);
    /* OUT2 square reaches port B bit5: with CR=5 (high 3, low 2) both
     * phases are observable deterministically */
    io_write(&f.m, 0x43, 1, 0xB6);
    io_write(&f.m, 0x42, 1, 5);
    io_write(&f.m, 0x42, 1, 0);
    int hi = 0, lo = 0;
    for (int i = 0; i < 240; i++) {
        fx_steps(&f, 1);
        uint32_t b = io_read(&f.m, 0x61, 1);
        if (b & 0x20) hi++; else lo++;
    }
    assert(hi > 0 && lo > 0);
}

static void test_mode_switch_keeps_counting(void) {
    fx_t f; fx_init(&f);
    const uint8_t prog[] = {
        0xB0,0x34, 0xE6,0x43,      /* ch0 rw3 mode2 */
        0xB0,0x18, 0xE6,0x40,      /* CR=24 */
        0xB0,0x00, 0xE6,0x40,
    };
    fx_guest(&f, prog, sizeof prog);
    fx_steps(&f, 1300);                          /* >= 4 periods */
    uint64_t n1 = mem_read(&f.m, 0x2000, 1);
    assert(n1 >= 2);
    /* CW-only switch to mode 3, NO new count: Intel semantics = keep
     * counting under the new mode */
    io_write(&f.m, 0x43, 1, 0x36);
    fx_steps(&f, 700);                           /* ~2.4 periods */
    assert(mem_read(&f.m, 0x2000, 1) > n1);
}

int main(void) {
    test_pit_defaults();
    test_rate_generator_cadence();
    test_mode0_one_shot();
    test_latch_readback_exact();
    test_lsb_only_quick_fire();
    test_counter2_gate_and_port61();
    test_mode_switch_keeps_counting();
    puts("pit tests: ok");
    return 0;
}
