/* tests/test_pic.c -- CHIPSET H0 vectors: INTR delivery through the PIC.
 *
 * Every test drives the real port interface (0x20/0x21/0xA0/0xA1) with
 * guest code, asserts IRQ lines through the public pic_raise_irq(), and
 * checks delivery by watching a handler-side memory indicator. PC default
 * pin values (vector bases 0x08/0x70, IMR=0xFF) are measured, per
 * CHIPSET_PLAN D5.
 */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "machine.h"
#include "platform.h"
#include "pic.h"
#include "harness.h"

/* guest program frame: after PIC init + optional mask writes the program
 * executes STI, writes the marker at 0x2001, then spins in a JMP $ loop
 * (or HLTs, for the wake test). */

static const uint8_t INIT_MASTER_BASE20[] = {
    0xB0,0x11, 0xE6,0x20,   /* mov al,0x11; out 0x20,al  ICW1: edge,cascade,ICW4 */
    0xB0,0x20, 0xE6,0x21,   /* mov al,0x20; out 0x21,al  ICW2: base 0x20 */
    0xB0,0x04, 0xE6,0x21,   /* mov al,0x04; out 0x21,al  ICW3: slave on irq2 */
    0xB0,0x01, 0xE6,0x21,   /* mov al,0x01; out 0x21,al  ICW4: 8086, no AEOI */
};
static const uint8_t INIT_SLAVE_BASE28[] = {
    0xB0,0x11, 0xE6,0xA0,
    0xB0,0x28, 0xE6,0xA1,   /* base 0x28 */
    0xB0,0x02, 0xE6,0xA1,   /* slave id 2 */
    0xB0,0x01, 0xE6,0xA1,
};

/* handler templates at fixed phys addresses */
static const uint8_t H_MARK_EOI_IRET[] =   /* mov byte[0x2000],1; mov al,0x20; out 0x20,al; iret */
    { 0xC6,0x06,0x00,0x20,0x01, 0xB0,0x20, 0xE6,0x20, 0xCF };
static const uint8_t H_MARK_IRET[] =       /* mov byte[0x2000],1; iret (no EOI) */
    { 0xC6,0x06,0x00,0x20,0x01, 0xCF };
static const uint8_t H_A_EOI_IRET[] =      /* mov byte[0x2020],1; eoi; iret */
    { 0xC6,0x06,0x20,0x20,0x01, 0xB0,0x20, 0xE6,0x20, 0xCF };
static const uint8_t H_B_ORDER_EOI_IRET[] =/* if [0x2020]==1: [0x2021]=1; eoi; iret */
    { 0x80,0x3E,0x20,0x20,0x01, 0x75,0x05, 0xC6,0x06,0x21,0x20,0x01,
      0xB0,0x20, 0xE6,0x20, 0xCF };
static const uint8_t H_SLAVE_MARK[] =      /* mov byte[0x2030],0x2A; eoi both; iret */
    { 0xC6,0x06,0x30,0x20,0x2A, 0xB0,0x20, 0xE6,0x20, 0xE6,0xA0, 0xCF };

#define PH0 0x0100
#define PH1 0x0110
#define PH2 0x0120
#define PH3 0x0130

static void ivt(machine_t *m, int vec, uint16_t off) {
    mem_write(m, (uint64_t)vec * 4,     2, off);
    mem_write(m, (uint64_t)vec * 4 + 2, 2, 0);
}

typedef struct { machine_t m; } fx_t;
static void fx_init(fx_t *f) {
    setup_machine(&f->m);
    pic_init(&f->m);
    pic_io_register(&f->m);
}

/* assemble: init sequences + optional 2 mask bytes + tail; run until the
 * marker byte is set (or cap), then caller raises IRQs and steps more. */
static void fx_load_run(fx_t *f, const uint8_t *init, size_t init_len,
                        uint8_t mask_m, uint8_t mask_s, int with_masks,
                        const uint8_t *tail, size_t tail_len, int cap) {
    uint8_t prog[256]; size_t n = 0;
    memcpy(prog + n, init, init_len); n += init_len;
    if (with_masks) {
        uint8_t w[8] = { 0xB0,mask_m, 0xE6,0x21, 0xB0,mask_s, 0xE6,0xA1 };
        memcpy(prog + n, w, sizeof w); n += sizeof w;
    }
    memcpy(prog + n, tail, tail_len); n += tail_len;
    memcpy(f->m.ram, prog, n);
    f->m.cpu.rip = 0;
    for (int i = 0; i < cap && !mem_read(&f->m, 0x2001, 1); i++)
        if (cpu_step(&f->m.cpu) != 0) break;
}

static void fx_steps(fx_t *f, int n) {
    for (int i = 0; i < n; i++) cpu_step(&f->m.cpu);
}

/* STI; marker; jmp $ */
static const uint8_t TAIL_SPIN[] = { 0xFB, 0xC6,0x06,0x01,0x20,0x01, 0xEB,0xFE };
/* CLI; STI; marker; HLT (for the wake test: IF=1 when sleeping) */
static const uint8_t TAIL_HLT[]  = { 0xFB, 0xC6,0x06,0x01,0x20,0x01, 0xF4 };

/* ------------------------------------------------------------------ */

static void test_defaults(void) {
    fx_t f; fx_init(&f);
    /* PC power-on/reset values, measured per D5 */
    assert(f.m.pic.master.imr == 0xFF && f.m.pic.slave.imr == 0xFF);
    assert(f.m.pic.master.vector_base == 0x08 && f.m.pic.slave.vector_base == 0x70);
    assert(f.m.pic.master.irr == 0 && f.m.pic.master.isr == 0);
    assert(!pic_pending(&f.m));
    assert(pic_intack(&f.m) == -1);
    /* command-port reads default to IRR = 0 */
    const uint8_t prog[] = { 0xE4,0x20, 0xA2,0x40,0x20, 0xF4 }; /* in al,0x20; mov [0x2040],al */
    memcpy(f.m.ram, prog, sizeof prog);
    for (int i = 0; i < 8; i++) cpu_step(&f.m.cpu);
    assert(mem_read(&f.m, 0x2040, 1) == 0);
}

static void test_cli_gate_blocks_delivery(void) {
    fx_t f; fx_init(&f);
    uint8_t buf[128]; size_t n = 0;
    memcpy(buf, INIT_MASTER_BASE20, sizeof INIT_MASTER_BASE20); n += sizeof INIT_MASTER_BASE20;
    /* unmask irq0 but leave IF=0 (no STI) */
    uint8_t rest[] = { 0xB0,0xFE, 0xE6,0x21,          /* out 0x21, imr=0xFE */
                       0xC6,0x06,0x01,0x20,0x01,      /* marker */
                       0xEB,0xFE };                   /* jmp $ */
    memcpy(buf + n, rest, sizeof rest); n += sizeof rest;
    memcpy(f.m.ram, buf, n);
    ivt(&f.m, 0x20, PH0);
    memcpy(f.m.ram + PH0, H_MARK_EOI_IRET, sizeof H_MARK_EOI_IRET);
    f.m.cpu.rip = 0;
    for (int i = 0; i < 32 && !mem_read(&f.m, 0x2001, 1); i++) cpu_step(&f.m.cpu);
    assert(mem_read(&f.m, 0x2001, 1) == 1);
    pic_raise_irq(&f.m, 0);
    assert(pic_pending(&f.m));
    fx_steps(&f, 16);
    assert(mem_read(&f.m, 0x2000, 1) == 0);   /* IF=0: never delivered */
}

static void test_delivery_round_trip(void) {
    fx_t f; fx_init(&f);
    ivt(&f.m, 0x20, PH0);
    memcpy(f.m.ram + PH0, H_MARK_IRET, sizeof H_MARK_IRET); /* no guest EOI on purpose */
    fx_load_run(&f, INIT_MASTER_BASE20, sizeof INIT_MASTER_BASE20,
                0xFE, 0xFF, 1, TAIL_SPIN, sizeof TAIL_SPIN, 64); /* unmask irq0 */
    assert(mem_read(&f.m, 0x2001, 1) == 1);
    uint64_t sp_before = f.m.cpu.gpr[RSP];
    pic_raise_irq(&f.m, 0);
    fx_steps(&f, 16);
    assert(mem_read(&f.m, 0x2000, 1) == 1);         /* handler ran */
    assert(f.m.cpu.gpr[RSP] == sp_before);          /* IRET unwound the frame */
    assert(f.m.cpu.rflags & FLAG_IF);               /* IF restored from frame */
    /* ISR still latched (no guest EOI): read it through OCW3 */
    assert((f.m.pic.master.isr & 1) == 1);
}

static void test_masked_line_not_delivered(void) {
    fx_t f; fx_init(&f);
    ivt(&f.m, 0x20, PH0);
    memcpy(f.m.ram + PH0, H_MARK_EOI_IRET, sizeof H_MARK_EOI_IRET);
    /* imr stays 0xFF: irq0 masked */
    fx_load_run(&f, INIT_MASTER_BASE20, sizeof INIT_MASTER_BASE20,
                0, 0, 0, TAIL_SPIN, sizeof TAIL_SPIN, 64);
    pic_raise_irq(&f.m, 0);
    assert(!pic_pending(&f.m));
    fx_steps(&f, 16);
    assert(mem_read(&f.m, 0x2000, 1) == 0);
}

static void test_priority_fixed_lowest_first(void) {
    fx_t f; fx_init(&f);
    ivt(&f.m, 0x23, PH1);  /* irq3 handler: sets [0x2020]; eoi; iret */
    ivt(&f.m, 0x25, PH2);  /* irq5 handler: [0x2021]=1 iff A ran first */
    memcpy(f.m.ram + PH1, H_A_EOI_IRET, sizeof H_A_EOI_IRET);
    memcpy(f.m.ram + PH2, H_B_ORDER_EOI_IRET, sizeof H_B_ORDER_EOI_IRET);
    fx_load_run(&f, INIT_MASTER_BASE20, sizeof INIT_MASTER_BASE20,
                0xD7, 0xFF, 1, TAIL_SPIN, sizeof TAIL_SPIN, 64); /* unmask 3 and 5 */
    assert(mem_read(&f.m, 0x2020, 1) == 0);
    assert(mem_read(&f.m, 0x2021, 1) == 0);
    pic_raise_irq(&f.m, 5);
    pic_raise_irq(&f.m, 3);
    fx_steps(&f, 32);
    assert(mem_read(&f.m, 0x2020, 1) == 1);   /* irq3 delivered (first) */
    assert(mem_read(&f.m, 0x2021, 1) == 1);   /* irq5 saw irq3's flag: ran second */
}

static void test_slave_cascade_vector(void) {
    fx_t f; fx_init(&f);
    ivt(&f.m, 0x2A, PH3);  /* vector for slave irq2 = line 10 */
    memcpy(f.m.ram + PH3, H_SLAVE_MARK, sizeof H_SLAVE_MARK);
    uint8_t init[64]; size_t n = 0;
    memcpy(init, INIT_MASTER_BASE20, sizeof INIT_MASTER_BASE20); n += sizeof INIT_MASTER_BASE20;
    memcpy(init + n, INIT_SLAVE_BASE28, sizeof INIT_SLAVE_BASE28); n += sizeof INIT_SLAVE_BASE28;
    fx_load_run(&f, init, n,
                0xFB, 0x00, 1, TAIL_SPIN, sizeof TAIL_SPIN, 64); /* cascade + all slave */
    assert(mem_read(&f.m, 0x2001, 1) == 1);
    pic_raise_irq(&f.m, 10);
    assert(pic_pending(&f.m));
    fx_steps(&f, 16);
    assert(mem_read(&f.m, 0x2030, 1) == 0x2A);
    assert(f.m.pic.master.isr == 0 && f.m.pic.slave.isr == 0); /* guest EOIed both */
}

static void test_eoi_releases_isr_via_port(void) {
    fx_t f; fx_init(&f);
    ivt(&f.m, 0x20, PH0);
    memcpy(f.m.ram + PH0, H_MARK_IRET, sizeof H_MARK_IRET); /* no EOI in handler */
    fx_load_run(&f, INIT_MASTER_BASE20, sizeof INIT_MASTER_BASE20,
                0xFE, 0xFF, 1, TAIL_SPIN, sizeof TAIL_SPIN, 64); /* unmask irq0 */
    pic_raise_irq(&f.m, 0);
    fx_steps(&f, 16);
    assert(mem_read(&f.m, 0x2000, 1) == 1);
    /* guest now reads ISR through OCW3, then EOIs, then reads again */
    const uint8_t prog2[] = { 0xB0,0x0B, 0xE6,0x20,      /* mov al,0x0B; out 0x20,al (read ISR) */
                              0xE4,0x20, 0xA2,0x40,0x20, /* in al,0x20; mov [0x2040],al */
                              0xB0,0x20, 0xE6,0x20,      /* EOI */
                              0xE4,0x20, 0xA2,0x41,0x20, /* in al,0x20; mov [0x2041],al */
                              0xF4 };
    memcpy(f.m.ram + 0x0300, prog2, sizeof prog2);
    f.m.cpu.rip = 0x0300;
    fx_steps(&f, 16);
    assert(mem_read(&f.m, 0x2040, 1) == 1);   /* ISR bit0 was set */
    assert(mem_read(&f.m, 0x2041, 1) == 0);   /* EOI cleared it */
}

static void test_irr_readback_via_ocw3(void) {
    fx_t f; fx_init(&f);
    /* no delivery possible: all masked; raise irq1, select IRR, read */
    const uint8_t prog[] = { 0xB0,0x0A, 0xE6,0x20,       /* OCW3: read IRR */
                             0xE4,0x20, 0xA2,0x42,0x20,  /* in al,0x20; mov [0x2042],al */
                             0xF4 };
    pic_raise_irq(&f.m, 1);
    memcpy(f.m.ram, prog, sizeof prog);
    f.m.cpu.rip = 0;
    fx_steps(&f, 8);
    assert(mem_read(&f.m, 0x2042, 1) == 0x02);
}

static void test_hlt_wakes_and_delivers(void) {
    fx_t f; fx_init(&f);
    ivt(&f.m, 0x20, PH0);
    memcpy(f.m.ram + PH0, H_MARK_EOI_IRET, sizeof H_MARK_EOI_IRET);
    fx_load_run(&f, INIT_MASTER_BASE20, sizeof INIT_MASTER_BASE20,
                0xFE, 0xFF, 1, TAIL_HLT, sizeof TAIL_HLT, 64); /* unmask irq0 */
    assert(mem_read(&f.m, 0x2001, 1) == 1);
    fx_steps(&f, 2);   /* loader stops at the marker step; land in HLT */
    assert(f.m.cpu.halted);
    pic_raise_irq(&f.m, 0);
    fx_steps(&f, 16);
    assert(!f.m.cpu.halted);                  /* wake happened */
    assert(mem_read(&f.m, 0x2000, 1) == 1);   /* and IF=1 delivered vector 0x20 */
}

static void test_sti_shadow_delays_one_instruction(void) {
    fx_t f; fx_init(&f);
    /* IRQ0 is raised BEFORE the guest runs "sti; mov [0x2001],1; jmp $".
     * Machinery: IF=0 after reset, so the line pends silently. The sti
     * boundary is shadowed one instruction (hardware contract), so the
     * marker instruction MUST execute before delivery. The handler only
     * sets [0x2005]=1 if [0x2001]==1 -- a broken shadow would deliver it
     * immediately after sti, find 0, and never set it. */
    const uint8_t handler2[] = { 0x80,0x3E,0x01,0x20,0x01, 0x75,0x05,
                                 0xC6,0x06,0x05,0x20,0x01, 0xB0,0x20, 0xE6,0x20, 0xCF };
    ivt(&f.m, 0x20, PH0);
    memcpy(f.m.ram + PH0, handler2, sizeof handler2);
    uint8_t buf[192]; size_t n = 0;
    memcpy(buf, INIT_MASTER_BASE20, sizeof INIT_MASTER_BASE20); n += sizeof INIT_MASTER_BASE20;
    {
        uint8_t w[4] = { 0xB0, 0xFE, 0xE6, 0x21 };       /* unmask irq0 */
        memcpy(buf + n, w, sizeof w); n += sizeof w;
    }
    memcpy(buf + n, TAIL_SPIN, sizeof TAIL_SPIN); n += sizeof TAIL_SPIN;
    memcpy(f.m.ram, buf, n);
    f.m.cpu.rip = 0;
    pic_raise_irq(&f.m, 0);                               /* pending before sti */
    fx_steps(&f, 48);
    assert(mem_read(&f.m, 0x2001, 1) == 1);
    assert(mem_read(&f.m, 0x2005, 1) == 1);               /* shadow held: marker first */
}

int main(void) {
    test_defaults();
    test_cli_gate_blocks_delivery();
    test_delivery_round_trip();
    test_masked_line_not_delivered();
    test_priority_fixed_lowest_first();
    test_slave_cascade_vector();
    test_eoi_releases_isr_via_port();
    test_irr_readback_via_ocw3();
    test_hlt_wakes_and_delivers();
    test_sti_shadow_delays_one_instruction();
    puts("pic tests: ok");
    return 0;
}
