/* tests/test_chipset.c -- CHIPSET H5 (+ UPKEEP U5) vectors: A20 gate + system reset + CF9 CPU INIT.
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
    /* PCH RST_CNT (U5 bit roles): bit1 SYS_RST = type, bit2 RST_CPU =
     * the 0->1 trigger edge. The type bit alone is inert storage. */
    io_write(&f.m, 0xCF9, 1, 0x02);
    assert(f.m.chipset.resets == 0 && f.m.chipset.reset_pending == 0);
    assert(io_read(&f.m, 0xCF9, 1) == 0x02);
    /* 0x04 alone: RST_CPU rising with SYS_RST clear = INIT#, a CPU INIT
     * request -- NOT a full reset */
    io_write(&f.m, 0xCF9, 1, 0x04);
    assert(f.m.chipset.resets == 0);
    assert(f.m.chipset.reset_pending == RESET_KIND_CPU_INIT);
    assert(io_read(&f.m, 0xCF9, 1) == 0x04);
    f.m.cpu.rip = 0x1234;                         /* somewhere non-reset */
    assert(cpu_step(&f.m.cpu) == 0);              /* boundary consumption */
    assert(f.m.chipset.resets == 0);
    assert(f.m.chipset.reset_pending == 0);
    assert(f.m.cpu.rip == 0xFFF0);                /* ...and the CPU took INIT */
    assert(io_read(&f.m, 0xCF9, 1) == 0x04);      /* cf9 itself rides through */
    /* single-shot hard reset: SYS_RST already set in the firing value */
    io_write(&f.m, 0xCF9, 1, 0x00);
    io_write(&f.m, 0xCF9, 1, 0x06);
    assert(f.m.chipset.reset_pending == RESET_KIND_FULL);
    assert(cpu_step(&f.m.cpu) == 0);
    assert(f.m.chipset.resets == 1);
    /* the value our own FADT advertises (P2): 0x0E full-flavor hard */
    io_write(&f.m, 0xCF9, 1, 0x0E);
    assert(f.m.chipset.reset_pending == RESET_KIND_FULL);
    assert(cpu_step(&f.m.cpu) == 0);
    assert(f.m.chipset.resets == 2);
    /* the classic dance: 0x02 then 0x06 */
    io_write(&f.m, 0xCF9, 1, 0x02);
    io_write(&f.m, 0xCF9, 1, 0x06);
    assert(f.m.chipset.reset_pending == RESET_KIND_FULL);
    assert(f.m.chipset.resets == 2);              /* not until the boundary */
    assert(cpu_step(&f.m.cpu) == 0);
    assert(f.m.chipset.resets == 3);
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

/* --------------------------------------------------- UPKEEP U5 ----- */

static const uint8_t TRIG_INIT[] = {    /* mov al,4; mov dx,0xCF9; out dx,al; tombstone; jmp $ */
    0xB0,0x04, 0xBA,0xF9,0x0C, 0xEE, 0xC6,0x06,0x06,0x20,0xEE, 0xEB,0xFE };

static void test_cpu_init_via_cf9_ritual(void) {
    fx_t f; fx_init(&f);
    install_reset_path(&f); dirty_machine(&f);
    /* extra sentinels a full reset would clear and an INIT# must not */
    f.m.pit.ch[0].cr = 0xBEEF;                    /* PIT reload value */
    f.m.rtc.ram[0x40] = 0xA5;                     /* a battery-backed byte */
    int64_t rtc_base = f.m.rtc.base_sec;
    /* p92 storage with its A20 source OFF (a full reset re-seeds 0x02);
     * keep the line itself open through the 8042 contribution so the
     * reset-vector fetch takes the board's normal path */
    io_write(&f.m, 0x64, 1, 0xD1);
    io_write(&f.m, 0x60, 1, 0x03);                /* KBC A20 on, SRST# high */
    io_write(&f.m, 0x92, 1, 0x00);                /* p92 source off */
    assert(f.m.chipset.a20 == 1 && f.m.chipset.a20_kbc == 1 &&
           f.m.chipset.a20_p92 == 0);

    run_trigger(&f, TRIG_INIT, sizeof TRIG_INIT);
    /* the CPU restarted at the reset vector, BETWEEN instructions */
    assert(mem_read(&f.m, 0x2007, 1) == 1);
    assert(mem_read(&f.m, 0x2006, 1) == 0);       /* tombstone never ran */
    /* ...but no system reset happened and no device was re-initialised */
    assert(f.m.chipset.resets == 0);
    assert(f.m.chipset.reset_pending == 0);
    assert(io_read(&f.m, 0x21, 1) == 0xFD);       /* PIC mask intact */
    io_write(&f.m, 0x64, 1, 0x20);
    assert(io_read(&f.m, 0x60, 1) == 0x45);       /* KBC command byte intact */
    assert(f.m.pit.ch[0].cr == 0xBEEF);           /* PIT intact */
    assert(f.m.rtc.ram[0x40] == 0xA5);            /* RTC CMOS intact */
    assert(f.m.rtc.base_sec == rtc_base);
    assert(mem_read(&f.m, 0x600, 1) == 0x42);     /* RAM intact */
    assert(io_read(&f.m, 0x92, 1) == 0x00);       /* p92 NOT re-seeded to 0x02 */
    assert(f.m.chipset.a20_kbc == 1 && f.m.chipset.a20_p92 == 0 &&
           f.m.chipset.a20 == 1);                 /* gate line state intact */
    assert(io_read(&f.m, 0xCF9, 1) == 0x04);      /* INIT is not a chipset reset */
    printf("ok: cpu_init_via_cf9_ritual\n");
}

static void test_cpu_init_preserves_cpu_state(void) {
    fx_t f; fx_init(&f);
    cpu_t *c = &f.m.cpu;
    int found;
    /* SDM Table 9-1 INIT column: configure some "unchanged" state */
    cpu_set_msr(c, 0xC0000081u, 0x1122334455667788ULL);   /* STAR */
    cpu_set_msr(c, LAPIC_MSR_APICBASE, 0xFEE80900ULL);    /* relocated LAPIC */
    cpu_set_msr(c, 0xC0000100u, 0xABCD0000ULL);           /* FS_BASE -> 0 by INIT */
    c->efer = 0x500; cpu_set_msr(c, 0xC0000080u, 0x500);  /* EFER -> 0 */
    c->kgs_base = 0x77770000ULL;
    c->mxcsr = 0x3F80u;
    c->xmm[3][0] = 0x1122334455667788ULL; c->xmm[3][1] = 0x99AABBCCDDEEFF00ULL;
    c->fst[2] = -2.5L;                                    /* ST pattern, stale post-INIT */
    c->fcw = 0x027F; c->ftop = 3;
    c->gpr[0] = 0xDEAD;                                   /* EAX must reset */
    f.m.cpu.rip = 0x1234;
    uint64_t steps = c->executed_steps;

    io_write(&f.m, 0xCF9, 1, 0x04);
    assert(f.m.chipset.reset_pending == RESET_KIND_CPU_INIT);
    assert(cpu_step(c) == 0);

    assert(c->rip == 0xFFF0 && c->seg[SEG_CS].sel == 0xF000 &&
           c->seg[SEG_CS].base == 0xFFFF0000ULL);
    assert(c->cr0 == 0x60000010ULL);
    assert(c->gpr[0] == 0);
    assert(cpu_get_msr(c, 0xC0000081u, &found) == 0x1122334455667788ULL && found);
    assert(cpu_get_msr(c, LAPIC_MSR_APICBASE, &found) == 0xFEE80900ULL && found);
    assert(cpu_get_msr(c, 0xC0000100u, &found) == 0);      /* FS_BASE reset */
    assert(cpu_get_msr(c, 0xC0000080u, &found) == 0);      /* EFER reset */
    assert(c->efer == 0);
    assert(c->kgs_base == 0x77770000ULL);
    assert(c->mxcsr == 0x3F80u);
    assert(c->xmm[3][0] == 0x1122334455667788ULL && c->xmm[3][1] == 0x99AABBCCDDEEFF00ULL);
    assert(c->fst[2] == -2.5L);                           /* stale bits ride through */
    assert(c->fcw == 0x037F && c->ftw == 0xFFFF && c->fsw == 0 && c->ftop == 0);
    assert(c->executed_steps == steps);                   /* monotonic bookkeeping */
    printf("ok: cpu_init_preserves_cpu_state\n");
}

/* step until the reentry byte reaches n (the reset-vector stub has run n times) */
static void run_until_entry(fx_t *f, uint32_t n) {
    for (int i = 0; i < 256 && mem_read(&f->m, 0x2007, 1) < n; i++)
        (void)cpu_step(&f->m.cpu);
    assert(mem_read(&f->m, 0x2007, 1) == n);
}

static void test_cpu_init_edge_repeat_coalesce(void) {
    fx_t f; fx_init(&f);
    install_reset_path(&f);
    cpu_reset(&f.m.cpu);                          /* power-on state, not a counted reset */
    run_until_entry(&f, 1);                       /* power-on entry */
    /* first INIT# */
    io_write(&f.m, 0xCF9, 1, 0x04);
    assert(f.m.chipset.reset_pending == RESET_KIND_CPU_INIT);
    run_until_entry(&f, 2);
    assert(f.m.chipset.resets == 0);
    /* the stored bit is still set: writing 0x04 again is no new edge */
    io_write(&f.m, 0xCF9, 1, 0x04);
    assert(f.m.chipset.reset_pending == 0);
    for (int i = 0; i < 24; i++) (void)cpu_step(&f.m.cpu);
    assert(mem_read(&f.m, 0x2007, 1) == 2);       /* nothing happened */
    /* clear the trigger, set it again: a fresh edge, a fresh INIT# */
    io_write(&f.m, 0xCF9, 1, 0x00);
    io_write(&f.m, 0xCF9, 1, 0x04);
    assert(f.m.chipset.reset_pending == RESET_KIND_CPU_INIT);
    run_until_entry(&f, 3);
    /* coalescing: an INIT pending, then an 8042 reset pulse before the
     * boundary upgrades it into exactly one FULL reset */
    io_write(&f.m, 0x21, 1, 0xFD);                /* PIC sentinel */
    io_write(&f.m, 0xCF9, 1, 0x00);
    io_write(&f.m, 0xCF9, 1, 0x04);
    assert(f.m.chipset.reset_pending == RESET_KIND_CPU_INIT);
    io_write(&f.m, 0x64, 1, 0xFE);
    assert(f.m.chipset.reset_pending == RESET_KIND_FULL);
    run_until_entry(&f, 4);
    assert(f.m.chipset.resets == 1);
    assert(io_read(&f.m, 0x21, 1) == 0xFF);       /* devices WERE reset */
    /* ...and the reverse absorbs: an INIT behind a pending full reset */
    io_write(&f.m, 0x64, 1, 0xFE);
    assert(f.m.chipset.reset_pending == RESET_KIND_FULL);
    io_write(&f.m, 0xCF9, 1, 0x04);
    assert(f.m.chipset.reset_pending == RESET_KIND_FULL);
    run_until_entry(&f, 5);
    assert(f.m.chipset.resets == 2);
    printf("ok: cpu_init_edge_repeat_coalesce\n");
}

static void smp_fx(fx_t *f) {
    fx_init(f);
    install_reset_path(f);
    f->m.n_vcpus = 3;
    f->m.vcpu[0].c = f->m.cpu; f->m.vcpu[0].l = f->m.lapic;
    f->m.vcpu[0].state = VCPU_RUN;
    for (int i = 1; i < 3; i++) {
        f->m.vcpu[i].c.mach = &f->m;
        cpu_reset(&f->m.vcpu[i].c);
        f->m.vcpu[i].l = f->m.lapic;
        f->m.vcpu[i].l.id = (uint32_t)i << 24;
        f->m.vcpu[i].state = VCPU_RUN;
    }
    f->m.cur_vcpu = 0;
}

static void test_cpu_init_smp_all_contexts(void) {
    /* (a) the writer is the BSP: every context takes INIT, APs park */
    fx_t f; smp_fx(&f);
    dirty_machine(&f);
    f.m.vcpu[1].c.rip = 0x1234; f.m.vcpu[1].c.gpr[0] = 0xA1;
    f.m.vcpu[2].l.tpr = 0x77;
    cpu_set_msr(&f.m.vcpu[2].c, LAPIC_MSR_APICBASE, 0xFEE80800ULL); /* relocated */
    run_trigger(&f, TRIG_INIT, sizeof TRIG_INIT);     /* executes on the BSP */
    assert(mem_read(&f.m, 0x2007, 1) == 1);           /* BSP at reset vector */
    assert(mem_read(&f.m, 0x2006, 1) == 0);           /* boundary respected */
    assert(f.m.chipset.resets == 0);
    for (int i = 1; i < 3; i++) {
        assert(f.m.vcpu[i].state == VCPU_WAIT_SIPI);
        assert(f.m.vcpu[i].c.rip == 0xFFF0);
        assert(f.m.vcpu[i].c.halted == 0);
        assert(f.m.vcpu[i].l.id == ((uint32_t)i << 24));   /* ID kept */
        assert(f.m.vcpu[i].l.svr == 0x00FFu);              /* regs reset */
    }
    int found;
    assert(cpu_get_msr(&f.m.vcpu[2].c, LAPIC_MSR_APICBASE, &found) ==
           0xFEE80800ULL && found);                     /* base kept */
    assert(f.m.vcpu[2].l.tpr == 0);                     /* LVT/TPR wiped */
    assert(io_read(&f.m, 0x21, 1) == 0xFD);             /* devices untouched */

    /* (b) the writer is an AP: the INIT is still platform-wide -- the
     * BSP restarts, the writing AP parks itself. */
    fx_t g; smp_fx(&g);
    vcpu_load(&g.m, 1);
    g.m.cpu.seg[SEG_CS].sel = 0; g.m.cpu.seg[SEG_CS].base = 0; g.m.cpu.rip = 0;
    memcpy(g.m.ram, TRIG_INIT, sizeof TRIG_INIT);
    int parked = 0;
    for (int i = 0; i < 32 && !parked; i++) {
        (void)cpu_step(&g.m.cpu);
        parked = (g.m.cpu.halted == 1 && g.m.chipset.reset_pending == 0);
    }
    assert(parked);
    assert(g.m.vcpu[1].state == VCPU_WAIT_SIPI);        /* writer parked */
    assert(g.m.cpu.halted == 1);                        /* and holds still */
    assert(g.m.cpu.rip == 0xFFF0);
    vcpu_store(&g.m, 1);
    assert(g.m.vcpu[0].state == VCPU_RUN);              /* BSP did NOT park */
    assert(g.m.vcpu[0].c.rip == 0xFFF0);                /* ...but was INIT'd */
    assert(g.m.vcpu[2].state == VCPU_WAIT_SIPI);
    assert(g.m.vcpu[2].c.rip == 0xFFF0);
    vcpu_load(&g.m, 0);
    printf("ok: cpu_init_smp_all_contexts\n");
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
    test_cpu_init_via_cf9_ritual();
    test_cpu_init_preserves_cpu_state();
    test_cpu_init_edge_repeat_coalesce();
    test_cpu_init_smp_all_contexts();
    printf("test_chipset: all vectors passed\n");
    return 0;
}
