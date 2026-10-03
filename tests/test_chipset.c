/* tests/test_chipset.c -- CHIPSET H5 vectors: A20 gate + system reset.
 *
 * A20 vectors drive the real ports (0x92, KBC 0xD0/0xD1) and observe the
 * physical-address consequences through mem_read/mem_write (the very bus
 * the CPU fetches through): with the gate closed, every odd megabyte must
 * alias onto the even one below -- the 1MB wrap DOS extenders probe for.
 *
 * Reset vectors run a guest trigger program that pulls one of the reset
 * sources, then watch the machine return to the reset vector: a ROM stub
 * at 0xFFFFFFF0 far-jumps to a RAM reentry handler that increments
 * [0x2007]. Pre-reset device state (PIC mask, KBC command byte) must be
 * wiped, RAM must ride through, and the gate must be open again after --
 * the full warm-reset ritual, per source. The byte at [0x2006] (written
 * by the instruction right after the trigger OUT) pins the boundary
 * semantics: it must stay 0, i.e. the reset lands BETWEEN instructions.
 */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "machine.h"
#include "platform.h"
#include "chipset.h"
#include "pic.h"
#include "pit.h"
#include "rtc.h"
#include "kbc.h"
#include "harness.h"

typedef struct { machine_t m; } fx_t;
static void fx_init(fx_t *f) {
    setup_machine(&f->m);        /* includes chipset_init: gate open (H5) */
    chipset_io_register(&f->m);
    pic_init(&f->m);  pic_io_register(&f->m);
    pit_init(&f->m);
    rtc_init(&f->m);
    kbc_init(&f->m);  kbc_io_register(&f->m);
}

/* ------------------------------------------------ A20 vectors (host-side) */

static void test_a20_default_open(void) {
    fx_t f; fx_init(&f);
    /* measured H5 baseline: the sample firmware never opens the gate yet
     * runs above 1MB, so this board powers up open, via port-0x92 bit1. */
    assert(f.m.chipset.a20 == 1);
    assert(io_read(&f.m, 0x92, 1) == 0x02);
    mem_write(&f.m, 0x00010, 1, 0x11);
    mem_write(&f.m, 0x100010, 1, 0x22);
    assert(mem_read(&f.m, 0x00010, 1) == 0x11);
    assert(mem_read(&f.m, 0x100010, 1) == 0x22);  /* distinct, not wrap */
    printf("ok: a20_default_open\n");
}

static void test_a20_wrap_via_p92(void) {
    fx_t f; fx_init(&f);
    mem_write(&f.m, 0x00010, 1, 0x11);
    mem_write(&f.m, 0x100010, 1, 0x22);

    io_write(&f.m, 0x92, 1, 0x00);                /* close (bit0 0->0: no reset) */
    assert(f.m.chipset.a20 == 0);
    assert(f.m.chipset.resets == 0);
    assert(io_read(&f.m, 0x92, 1) == 0x00);
    assert(mem_read(&f.m, 0x100010, 1) == 0x11);  /* aliases to 0x00010 */
    mem_write(&f.m, 0x100010, 1, 0x33);           /* write through the wrap */
    assert(mem_read(&f.m, 0x00010, 1) == 0x33);
    /* the wrap is bus-global: every odd megabyte aliases down */
    mem_write(&f.m, 0x301234, 1, 0x44);
    assert(mem_read(&f.m, 0x201234, 1) == 0x44);

    io_write(&f.m, 0x92, 1, 0x02);                /* reopen on its own */
    assert(f.m.chipset.a20 == 1);
    assert(f.m.chipset.resets == 0);
    assert(mem_read(&f.m, 0x100010, 1) == 0x22);  /* high copy preserved */
    assert(mem_read(&f.m, 0x00010, 1) == 0x33);
    assert(mem_read(&f.m, 0x301234, 1) != 0x44 || mem_read(&f.m, 0x201234, 1) == 0x44);
    printf("ok: a20_wrap_via_p92\n");
}

static void test_a20_kbc_or_semantics(void) {
    fx_t f; fx_init(&f);
    mem_write(&f.m, 0x00020, 1, 0x55);
    mem_write(&f.m, 0x100020, 1, 0x66);

    /* output port defaults: SRST# high, A20 contribution low */
    io_write(&f.m, 0x64, 1, 0xD0);
    assert(io_read(&f.m, 0x60, 1) == 0x01);

    io_write(&f.m, 0x92, 1, 0x00);                /* p92 source off */
    assert(f.m.chipset.a20 == 0);

    io_write(&f.m, 0x64, 1, 0xD1);                /* KBC source on alone */
    io_write(&f.m, 0x60, 1, 0x03);                /* bit1=A20, bit0=SRST# kept high */
    assert(f.m.chipset.a20_kbc == 1);
    assert(f.m.chipset.a20 == 1);                 /* OR: reopens the line */
    assert(f.m.chipset.resets == 0);              /* bit0 was never low */
    io_write(&f.m, 0x64, 1, 0xD0);
    assert(io_read(&f.m, 0x60, 1) == 0x03);       /* latch readback */
    assert(mem_read(&f.m, 0x100020, 1) == 0x66);  /* distinct again */
    assert(mem_read(&f.m, 0x00020, 1) == 0x55);

    io_write(&f.m, 0x64, 1, 0xD1);                /* KBC source off again */
    io_write(&f.m, 0x60, 1, 0x01);
    assert(f.m.chipset.a20 == 0);
    assert(mem_read(&f.m, 0x100020, 1) == 0x55);  /* wrap is back */
    printf("ok: a20_kbc_or_semantics\n");
}

/* ------------------------------------------------------- reset vectors */

/* reset-vector stub (fetched at 0xFFFFFFF0, top-of-4G ROM window):
 * far jmp 0000:0500 to the RAM reentry handler below. */
static const uint8_t RESET_STUB[] = { 0xEA, 0x00, 0x05, 0x00, 0x00 };
/* reentry handler: inc byte [0x2007]; jmp $ */
static const uint8_t REENTRY[] = { 0xFE, 0x06, 0x07, 0x20, 0xEB, 0xFE };

static void install_reset_path(fx_t *f) {
    memcpy(f->m.rom + (ROM_TOPOF4G_BASE == 0xFFFE0000ULL ? 0x1FFF0 : 0), RESET_STUB,
           sizeof RESET_STUB);   /* offset of 0xFFFFFFF0 within the 128K image */
    memcpy(f->m.ram + 0x500, REENTRY, sizeof REENTRY);
}

/* set device state that must not survive, and RAM that must */
static void dirty_machine(fx_t *f) {
    io_write(&f->m, 0x21, 1, 0xFD);               /* PIC: unmask IRQ1 */
    io_write(&f->m, 0x64, 1, 0x60);               /* KBC: write command byte */
    io_write(&f->m, 0x60, 1, 0x45);
    mem_write(&f->m, 0x600, 1, 0x42);             /* warm-reset RAM survivor */
}

static void run_trigger(fx_t *f, const uint8_t *prog, size_t n) {
    memcpy(f->m.ram, prog, n);
    f->m.cpu.rip = 0;
    for (int i = 0; i < 64 && !mem_read(&f->m, 0x2007, 1); i++)
        if (cpu_step(&f->m.cpu) != 0) break;
}

static void assert_warm_reset_ritual(fx_t *f) {
    /* ...back at the reset vector: stub ran, reentry handler ran */
    assert(mem_read(&f->m, 0x2007, 1) == 1);
    /* boundary semantics: the instruction after the trigger OUT never ran */
    assert(mem_read(&f->m, 0x2006, 1) == 0);
    assert(f->m.chipset.resets == 1);
    assert(f->m.chipset.reset_pending == 0);
    assert(f->m.cpu.seg[SEG_CS].base == 0);       /* stub far-jumped to 0:0x500 */
    /* devices reinitialized */
    assert(io_read(&f->m, 0x21, 1) == 0xFF);      /* PIC mask back */
    io_write(&f->m, 0x64, 1, 0x20);               /* KBC cmd byte back to 0 */
    assert(io_read(&f->m, 0x60, 1) == 0x00);
    /* RAM rides through, the gate is open again */
    assert(mem_read(&f->m, 0x600, 1) == 0x42);
    assert(f->m.chipset.a20 == 1);
    assert(io_read(&f->m, 0x92, 1) == 0x02);
}

static const uint8_t TRIG_P92[] = {     /* mov al,1; out 0x92,al; tombstone; jmp $ */
    0xB0,0x01, 0xE6,0x92, 0xC6,0x06,0x06,0x20,0xEE, 0xEB,0xFE };
static const uint8_t TRIG_CF9[] = {     /* mov al,6; mov dx,0xCF9; out dx,al; ... */
    0xB0,0x06, 0xBA,0xF9,0x0C, 0xEE, 0xC6,0x06,0x06,0x20,0xEE, 0xEB,0xFE };
static const uint8_t TRIG_KBC[] = {     /* mov al,0xFE; out 0x64,al; tombstone; jmp $ */
    0xB0,0xFE, 0xE6,0x64, 0xC6,0x06,0x06,0x20,0xEE, 0xEB,0xFE };
static const uint8_t TRIG_OUTP[] = {    /* 0xD1 with bit0=0: SRST# pulled low */
    0xB0,0xD1, 0xE6,0x64, 0xB0,0x00, 0xE6,0x60, 0xC6,0x06,0x06,0x20,0xEE, 0xEB,0xFE };

static void test_reset_via_p92(void) {
    fx_t f; fx_init(&f);
    install_reset_path(&f); dirty_machine(&f);
    run_trigger(&f, TRIG_P92, sizeof TRIG_P92);
    assert_warm_reset_ritual(&f);
    printf("ok: reset_via_p92\n");
}

static void test_reset_via_cf9_predicates(void) {
    fx_t f; fx_init(&f);
    /* neither 0x02 (CPU INIT only) nor 0x04 (SYS_RST without RST_CPU)
     * alone resets; both are stored and read back */
    io_write(&f.m, 0xCF9, 1, 0x02);
    assert(f.m.chipset.resets == 0 && f.m.chipset.reset_pending == 0);
    assert(io_read(&f.m, 0xCF9, 1) == 0x02);
    io_write(&f.m, 0xCF9, 1, 0x04);
    assert(f.m.chipset.resets == 0 && f.m.chipset.reset_pending == 0);
    assert(io_read(&f.m, 0xCF9, 1) == 0x04);
    /* the classic dance: 0x02 then 0x06 (SYS_RST rises with RST_CPU set) */
    io_write(&f.m, 0xCF9, 1, 0x02);
    io_write(&f.m, 0xCF9, 1, 0x06);
    assert(f.m.chipset.reset_pending == 1);
    assert(f.m.chipset.resets == 0);              /* not until the boundary */
    assert(cpu_step(&f.m.cpu) == 0);
    assert(f.m.chipset.resets == 1);
    assert(f.m.chipset.reset_pending == 0);
    printf("ok: reset_via_cf9_predicates\n");
}

static void test_reset_via_cf9(void) {
    fx_t f; fx_init(&f);
    install_reset_path(&f); dirty_machine(&f);
    run_trigger(&f, TRIG_CF9, sizeof TRIG_CF9);
    assert_warm_reset_ritual(&f);
    printf("ok: reset_via_cf9\n");
}

static void test_reset_via_kbc_fe(void) {
    fx_t f; fx_init(&f);
    install_reset_path(&f); dirty_machine(&f);
    run_trigger(&f, TRIG_KBC, sizeof TRIG_KBC);
    assert_warm_reset_ritual(&f);
    assert(f.m.kbc.reset_pulses == 1);
    /* pulse-train merge: two more 0xFE before the next boundary are
     * counted as two pulses but drive ONE reset -- the pending request
     * is a level, like the reset line they hold low (measured H5: the
     * boundary follows every instruction, so guest code can only merge
     * pulses back-to-back with no steps in between, done host-side). */
    io_write(&f.m, 0x64, 1, 0xFE);
    io_write(&f.m, 0x64, 1, 0xFE);
    assert(f.m.kbc.reset_pulses == 3);
    assert(f.m.chipset.reset_pending == 1);
    assert(cpu_step(&f.m.cpu) == 0);
    assert(f.m.kbc.reset_pulses == 3);              /* counter survives resets */
    assert(f.m.chipset.resets == 2);
    printf("ok: reset_via_kbc_fe\n");
}

static void test_reset_via_kbc_outport(void) {
    fx_t f; fx_init(&f);
    install_reset_path(&f); dirty_machine(&f);
    run_trigger(&f, TRIG_OUTP, sizeof TRIG_OUTP);
    assert_warm_reset_ritual(&f);
    assert(f.m.kbc.outport == 0x01);                /* relatched at warm reset */
    printf("ok: reset_via_kbc_outport\n");
}

int main(void) {
    test_a20_default_open();
    test_a20_wrap_via_p92();
    test_a20_kbc_or_semantics();
    test_reset_via_p92();
    test_reset_via_cf9_predicates();
    test_reset_via_cf9();
    test_reset_via_kbc_fe();
    test_reset_via_kbc_outport();
    printf("test_chipset: all vectors passed\n");
    return 0;
}
