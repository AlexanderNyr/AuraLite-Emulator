/* tests/test_lapic.c -- CHIPSET H6 vectors: local APIC (xAPIC).
 *
 * Fixture style: real-mode guests cannot reach the 0xFEE00000 MMIO window
 * (real-mode linear addresses stop just above 1MB), so real-mode vectors
 * program the LAPIC from the host (the POST would do it via unreal mode;
 * mechanics identical) while the GUEST sets IVT/IF and runs. Guest-side
 * MMIO programming and guest-side EOI are proven for real in the
 * long-mode vector, where 64-bit code drives the window directly --
 * including the IDT-gate delivery path.
 *
 * Caps: tsc_per_instr is 92 on the haswell fixture, so at DCR=/1 one
 * timer decrement costs ~1 instruction; TMICT=100-200 fires in a few
 * hundred steps.
 */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "machine.h"
#include "platform.h"
#include "lapic.h"
#include "harness.h"

typedef struct { machine_t m; } fx_t;

/* The ASan lane requires leak-free exits. lapic_mmio_register() heap-
 * allocates one mmio_region_t per fixture machine (stack-scoped, the
 * node itself outlives the frame); track every node here and free the
 * batch at exit -- functionally identical to devices_done() on the CLI
 * path, without dragging all of devices.c into this suite. */
#define MAX_FX_NODES 64
static mmio_region_t *FX_NODES[MAX_FX_NODES];
static int FX_NODES_N;
static void fx_nodes_cleanup(void) {
    for (int i = 0; i < FX_NODES_N; i++) free(FX_NODES[i]);
    FX_NODES_N = 0;
}
/* K7: production profiles moved to tsc_per_instr=1 (coherent in-order
 * single-issue model under the K7 PIT pin).  This suite's timer
 * arithmetic (step counts vs decrement rate) was built for the legacy
 * fast-TSC fixture at 92; keep THAT fixture locally so the vectors stay
 * exact, and let the production profiles be asserted where they live. */
static platform_t FX_PLAT = {
    PLAT_HASWELL, "Haswell (K7 test fixture)", 0x3C, 0x000306C3u, 92
};
static void fx_init(fx_t *f) {
    if (!FX_NODES_N) atexit(fx_nodes_cleanup);
    setup_machine(&f->m);                 /* cpu_reset seeds MSR 0x1B (H6) */
    f->m.plat = &FX_PLAT;
    lapic_init(&f->m);
    lapic_mmio_register(&f->m);
    FX_NODES[FX_NODES_N++] = f->m.mmio_list;   /* the one LAPIC node */
}

static void ivt(machine_t *m, int vec, uint16_t off) {
    mem_write(m, (uint64_t)vec * 4,     2, off);
    mem_write(m, (uint64_t)vec * 4 + 2, 2, 0);
}

/* handlers above 0x0200: vector 0x44's IVT slot reaches 0x110 (measured
 * the hard way -- 0x0100 IS vector 0x40's IVT entry) */
#define PH_MARK   0x0200   /* mov byte [0x2100],1; iret */
#define PH_COUNT  0x0240   /* inc byte [0x2101];   iret */
#define PH_MARK2  0x0280   /* mov byte [0x2103],1; iret */
#define PH_MARK3  0x02C0   /* mov byte [0x2104],1; iret */
static const uint8_t H_MARK[]  = { 0xC6,0x06,0x00,0x21,0x01, 0xCF };
static const uint8_t H_COUNT[] = { 0xFE,0x06,0x01,0x21, 0xCF };
static const uint8_t H_MARK2[] = { 0xC6,0x06,0x03,0x21,0x01, 0xCF };
static const uint8_t H_MARK3[] = { 0xC6,0x06,0x04,0x21,0x01, 0xCF };
/* guest main: sti; jmp $ */
static const uint8_t T_STI_SPIN[] = { 0xFB, 0xEB, 0xFE };

static void load(fx_t *f, uint16_t at, const uint8_t *code, size_t n) {
    memcpy(f->m.ram + at, code, n);
}
static void run_until(fx_t *f, uint64_t cell, int cap) {
    f->m.cpu.rip = 0;
    for (int i = 0; i < cap && !mem_read(&f->m, cell, 1); i++)
        (void)cpu_step(&f->m.cpu);             /* ignore -1: HLT retries */
}
static uint32_t lr(fx_t *f, uint32_t off)  { return (uint32_t)mem_read(&f->m, LAPIC_BASE + off, 4); }
static void     lw(fx_t *f, uint32_t off, uint32_t v) { mem_write(&f->m, LAPIC_BASE + off, 4, v); }

/* IRR/ISR register for vector v and its bit within */
static uint32_t irr_reg(int v) { return LAPIC_IRR_BASE + (v / 32) * 0x10; }
static uint32_t isr_reg(int v) { return 0x180 + (v / 32) * 0x10; }

/* host-side arming used by several vectors */
static void arm_timer(fx_t *f, uint32_t lvt, uint32_t count, uint32_t dcr) {
    lw(f, LAPIC_DCR, dcr);
    lw(f, LAPIC_LVT_TMR, lvt);
    lw(f, LAPIC_TMICT, count);
}

static void test_mmio_registers(void) {
    fx_t f; fx_init(&f);
    /* the window exists now (pre-H6 this read floated 0xFF.., measured) */
    assert(lr(&f, LAPIC_VER) == 0x00050014u);
    lw(&f, LAPIC_VER, 0);                        /* RO */
    assert(lr(&f, LAPIC_VER) == 0x00050014u);
    lw(&f, LAPIC_ID, 0xAA000000u);
    assert(lr(&f, LAPIC_ID) == 0xAA000000u);
    lw(&f, LAPIC_ID, 0x00FFFFFFu);               /* low bits not writable */
    assert(lr(&f, LAPIC_ID) == 0u);
    lw(&f, LAPIC_TPR, 0x40); assert(lr(&f, LAPIC_TPR) == 0x40);
    lw(&f, LAPIC_LDR, 0x55); assert(lr(&f, LAPIC_LDR) == 0x55);
    lw(&f, LAPIC_DFR, 0);                        /* model field stuck high */
    assert(lr(&f, LAPIC_DFR) == 0x0FFFFFFFu);
    assert(lr(&f, LAPIC_SVR) == 0x00FFu);        /* spurious 0xFF, disabled */
    assert(lr(&f, LAPIC_LVT_TMR) == LVT_MASKED);
    assert(lr(&f, LAPIC_LVT_LI0) == LVT_MASKED);
    assert(lr(&f, 0x210) == 0);                  /* TMR: all edge */
    assert(lr(&f, LAPIC_ESR) == 0);
    assert(lr(&f, LAPIC_TMCCUR) == 0);
    assert(lr(&f, 0x2F0) == 0);                  /* unimplemented slot */
    printf("ok: mmio_registers\n");
}

static void test_svr_gates_delivery_not_latching(void) {
    fx_t f; fx_init(&f);
    ivt(&f.m, 0x40, PH_MARK); load(&f, PH_MARK, H_MARK, sizeof H_MARK);
    load(&f, 0, T_STI_SPIN, sizeof T_STI_SPIN);
    lw(&f, LAPIC_SVR, 0x0FF);                    /* enabled bit clear */
    arm_timer(&f, 0x40, 100, 0x8);               /* /1, one-shot, vec 0x40 */
    for (int i = 0; i < 400; i++) (void)cpu_step(&f.m.cpu);
    assert(lr(&f, irr_reg(0x40)) == 1);          /* fired and LATCHED... */
    assert(lapic_deliverable(&f.m) == -1);       /* ...but not deliverable */
    assert(mem_read(&f.m, 0x2100, 1) == 0);
    lw(&f, LAPIC_SVR, 0x1FF);                    /* software-enable */
    assert(lapic_deliverable(&f.m) == 0x40);
    run_until(&f, 0x2100, 20);
    assert(mem_read(&f.m, 0x2100, 1) == 1);      /* lands the moment we enable */
    printf("ok: svr_gates_delivery_not_latching\n");
}

static void test_timer_oneshot_real_mode(void) {
    fx_t f; fx_init(&f);
    ivt(&f.m, 0x40, PH_MARK); load(&f, PH_MARK, H_MARK, sizeof H_MARK);
    load(&f, 0, T_STI_SPIN, sizeof T_STI_SPIN);
    lw(&f, LAPIC_SVR, 0x1FF);
    arm_timer(&f, 0x40, 200, 0x8);
    assert(lr(&f, LAPIC_TMCCUR) == 200);         /* arm copies init->current */
    run_until(&f, 0x2100, 1000);
    assert(mem_read(&f.m, 0x2100, 1) == 1);
    assert(lr(&f, LAPIC_TMCCUR) == 0);           /* one-shot consumed */
    assert(lr(&f, isr_reg(0x40)) == 1);          /* in service */
    assert(lr(&f, LAPIC_PPR) == 0x40);           /* in-service class reported */
    lw(&f, LAPIC_EOI, 0);
    assert(lr(&f, isr_reg(0x40)) == 0);
    assert(lr(&f, LAPIC_PPR) == 0);
    /* one-shot does not refire even after EOI */
    for (int i = 0; i < 500; i++) (void)cpu_step(&f.m.cpu);
    assert(mem_read(&f.m, 0x2103, 1) == 0 && lr(&f, LAPIC_TMCCUR) == 0);
    printf("ok: timer_oneshot_real_mode\n");
}

static void test_timer_periodic_real_mode(void) {
    fx_t f; fx_init(&f);
    ivt(&f.m, 0x40, PH_COUNT); load(&f, PH_COUNT, H_COUNT, sizeof H_COUNT);
    load(&f, 0, T_STI_SPIN, sizeof T_STI_SPIN);
    lw(&f, LAPIC_SVR, 0x1FF);
    arm_timer(&f, LVT_TMR_PERIODIC | 0x40, 100, 0x8);
    run_until(&f, 0x2101, 1000);
    assert(mem_read(&f.m, 0x2101, 1) == 1);      /* fire #1 */
    /* a same-class delivery cannot nest over its own ISR: the second
     * fire stays latched in IRR until the (host-side) EOI clears it */
    for (int i = 0; i < 400; i++) (void)cpu_step(&f.m.cpu);
    assert(lr(&f, irr_reg(0x40)) == 1);
    assert(lapic_deliverable(&f.m) == -1);       /* PPR 0x40 blocks class 4 */
    lw(&f, LAPIC_EOI, 0);
    f.m.cpu.rip = 0;
    for (int i = 0; i < 1000 && mem_read(&f.m, 0x2101, 1) < 2; i++)
        (void)cpu_step(&f.m.cpu);
    assert(mem_read(&f.m, 0x2101, 1) == 2);      /* fire #2 after EOI */
    printf("ok: timer_periodic_real_mode\n");
}

static void test_tpr_arbitration(void) {
    fx_t f; fx_init(&f);
    ivt(&f.m, 0x40, PH_MARK); load(&f, PH_MARK, H_MARK, sizeof H_MARK);
    load(&f, 0, T_STI_SPIN, sizeof T_STI_SPIN);
    lw(&f, LAPIC_SVR, 0x1FF);
    lw(&f, LAPIC_TPR, 0x40);                     /* class-4 threshold */
    arm_timer(&f, 0x40, 100, 0x8);
    for (int i = 0; i < 400; i++) (void)cpu_step(&f.m.cpu);
    assert(lr(&f, irr_reg(0x40)) == 1);          /* fired but class-blocked */
    assert(lapic_deliverable(&f.m) == -1);
    assert(mem_read(&f.m, 0x2100, 1) == 0);
    lw(&f, LAPIC_TPR, 0);                        /* threshold gone */
    assert(lapic_deliverable(&f.m) == 0x40);
    run_until(&f, 0x2100, 20);
    assert(mem_read(&f.m, 0x2100, 1) == 1);
    /* higher class CAN nest over an in-service one (PPR arbitration) */
    lw(&f, LAPIC_ICR_LO, (1u << 18) | 0x50);     /* self-IPI vector 0x50 */
    assert(lapic_deliverable(&f.m) == 0x50);     /* 0x50 class 5 > PPR 4 */
    /* ...and an equal class cannot:  */
    lw(&f, LAPIC_ICR_LO, (1u << 18) | 0x45);
    assert(lr(&f, irr_reg(0x45)) != 0);
    assert(lapic_intack(&f.m) == 0x50);          /* highest first */
    assert(lapic_deliverable(&f.m) == -1);       /* 0x45 still class-4 blocked */
    lw(&f, LAPIC_EOI, 0); lw(&f, LAPIC_EOI, 0);  /* wind down both ISRs */
    printf("ok: tpr_arbitration\n");
}

static void test_self_ipi(void) {
    fx_t f; fx_init(&f);
    ivt(&f.m, 0x43, PH_MARK2); load(&f, PH_MARK2, H_MARK2, sizeof H_MARK2);
    ivt(&f.m, 0x44, PH_MARK3); load(&f, PH_MARK3, H_MARK3, sizeof H_MARK3);
    load(&f, 0, T_STI_SPIN, sizeof T_STI_SPIN);
    lw(&f, LAPIC_SVR, 0x1FF);
    lw(&f, LAPIC_ICR_LO, (1u << 18) | 0x43);     /* shorthand: self */
    run_until(&f, 0x2103, 50);
    assert(mem_read(&f.m, 0x2103, 1) == 1);
    assert(lr(&f, isr_reg(0x43)) != 0);
    lw(&f, LAPIC_EOI, 0);
    lw(&f, LAPIC_ICR_HI, 0xFF000000u);           /* broadcast dest, shorthand 0 */
    lw(&f, LAPIC_ICR_LO, 0x44);
    run_until(&f, 0x2104, 50);
    assert(mem_read(&f.m, 0x2104, 1) == 1);
    assert(lr(&f, isr_reg(0x44)) != 0);
    lw(&f, LAPIC_EOI, 0);
    /* all-excluding-self on a single-vCPU box: nobody home */
    lw(&f, LAPIC_ICR_LO, (3u << 18) | 0x45);
    assert(lr(&f, irr_reg(0x45)) == 0);
    /* illegal vector raises ESR bit6, delivers nothing */
    lw(&f, LAPIC_ICR_LO, (1u << 18) | 0x02);
    assert(lr(&f, LAPIC_ESR) == 0x40);
    lw(&f, LAPIC_ESR, 0);
    assert(lr(&f, LAPIC_ESR) == 0);
    printf("ok: self_ipi\n");
}

/* ---------------------------------------------------------- long mode */

/* guest-side MMIO programming + IDT-gate delivery, all in 64-bit code.
 * 64-bit payload: programs SVR/DCR/LVT/TMICT itself, then sti; jmp $.
 * Proves guest-side MMIO writes and guest-side MMIO EOI from the handler. */
static const uint8_t MAIN64[] = {
    0xB8, 0xF0,0x00,0xE0,0xFE,         /* mov eax, 0xFEE000F0 (SVR)  */
    0xC7, 0x00, 0xFF,0x01,0x00,0x00,   /* mov dword [rax], 0x1FF     */
    0xB8, 0xE0,0x03,0xE0,0xFE,         /* mov eax, 0xFEE003E0 (DCR)  */
    0xC7, 0x00, 0x08,0x00,0x00,0x00,   /* mov dword [rax], 0x8 (/1)  */
    0xB8, 0x20,0x03,0xE0,0xFE,         /* mov eax, 0xFEE00320 (LVT)  */
    0xC7, 0x00, 0x40,0x00,0x00,0x00,   /* mov dword [rax], 0x40      */
    0xB8, 0x80,0x03,0xE0,0xFE,         /* mov eax, 0xFEE00380 (TMICT)*/
    0xC7, 0x00, 0x96,0x00,0x00,0x00,   /* mov dword [rax], 150       */
    0xFB,                              /* sti                        */
    0xEB, 0xFE,                        /* spin: jmp $                */
};
static const uint8_t HANDLER64[] = {
    0xB8, 0x00,0x23,0x00,0x00,         /* mov eax, 0x2300            */
    0xC6, 0x00, 0x01,                  /* mov byte [rax], 1          */
    0xB8, 0xB0,0x00,0xE0,0xFE,         /* mov eax, 0xFEE000B0 (EOI)  */
    0xC7, 0x00, 0x00,0x00,0x00,0x00,   /* mov dword [rax], 0         */
    0x48, 0xCF,                        /* iretq                      */
};

#define PML4_A 0x8000
#define PDPT_A 0x9000
#define PD_A   0xA000
#define PD3_A  0xC000   /* 4th GB: where the LAPIC window lives */
#define H64_A  0xB000
#define MAIN64_A 0x6000
#define GDT_A  0x0800
#define IDT_A  0x1000
#define TSS_A  0x1800
#define IST_STK 0x4000  /* handler runs on this (top), guest sits at 0x3000 */

static void test_timer_long_mode_impl(fx_t *f) {
    /* identity-mapped first 2MB, one 64-bit code segment (selector 0x08) */
    mem_write(&f->m, PML4_A, 8, PDPT_A | 3);
    mem_write(&f->m, PDPT_A, 8, PD_A | 3);
    mem_write(&f->m, PDPT_A + 3*8, 8, PD3_A | 3);
    mem_write(&f->m, PD_A, 8, 0x83);          /* 2MB page, present|rw|ps */
    mem_write(&f->m, PD3_A + 503*8, 8, 0xFEE00083ULL); /* 2MB @ 0xFEE00000 */
    static const uint8_t CODE64_DESC[8] = { 0xFF,0xFF,0,0,0,0x9A,0xAF,0 };
    memcpy(f->m.ram + GDT_A + 8, CODE64_DESC, 8);
    /* 64-bit interrupt gate for vector 0x40 -> H64_A */
    uint64_t h = H64_A;
    mem_write(&f->m, IDT_A + 0x40*16 + 0, 2, h & 0xFFFF);
    mem_write(&f->m, IDT_A + 0x40*16 + 2, 2, 0x08);
    mem_write(&f->m, IDT_A + 0x40*16 + 4, 1, 0x01);   /* IST=1 */
    mem_write(&f->m, IDT_A + 0x40*16 + 5, 1, 0x8E);   /* P|int-gate */
    mem_write(&f->m, IDT_A + 0x40*16 + 6, 2, (h >> 16) & 0xFFFF);
    mem_write(&f->m, IDT_A + 0x40*16 + 8, 4, (h >> 32) & 0xFFFFFFFFu);
    mem_write(&f->m, IDT_A + 0x40*16 + 12, 4, 0);
    /* TSS (64-bit, present) at GDT selector 0x10, descriptor = 16 bytes;
     * TSS body at TSS_A with IST1 stack at IST_STK. */
    mem_write(&f->m, TSS_A + 4, 4, IST_STK);            /* ist1 low 32 */
    uint32_t tb = TSS_A;
    mem_write(&f->m, GDT_A + 0x10 + 0, 2, 0x0067);      /* limit */
    mem_write(&f->m, GDT_A + 0x10 + 2, 2, tb & 0xFFFF);
    mem_write(&f->m, GDT_A + 0x10 + 4, 1, (tb >> 16) & 0xFF);
    mem_write(&f->m, GDT_A + 0x10 + 5, 1, 0x89);        /* P|type=64-bit TSS */
    mem_write(&f->m, GDT_A + 0x10 + 6, 1, 0x00);
    mem_write(&f->m, GDT_A + 0x10 + 7, 1, (tb >> 24) & 0xFF);
    mem_write(&f->m, GDT_A + 0x10 + 8, 4, 0);           /* base 63:32=0 */
    mem_write(&f->m, GDT_A + 0x10 + 12, 4, 0);          /* reserved */

    memcpy(f->m.ram + MAIN64_A, MAIN64, sizeof MAIN64);
    memcpy(f->m.ram + H64_A, HANDLER64, sizeof HANDLER64);

    cpu_t *c = &f->m.cpu;
    c->cr4 = 0x20;                       /* PAE */
    c->cr3 = PML4_A;
    c->efer = 0x500;                     /* LME|LMA */
    c->cr0 = 0x80000011ULL;              /* PG|PE|ET */
    c->seg[SEG_CS].sel = 0x08; c->seg[SEG_CS].base = 0;
    c->seg[SEG_CS].limit = 0xFFFFFFFFu;  c->seg[SEG_CS].l = 1; c->seg[SEG_CS].d_b = 0;
    c->seg[SEG_SS].sel = 0; c->seg[SEG_SS].base = 0;
    c->seg[SEG_SS].limit = 0xFFFFFFFFu; c->seg[SEG_SS].d_b = 1;
    c->gdtr_base = GDT_A; c->gdtr_limit = 0x20;   /* null, code64, tss(2) */
    c->idtr_base = IDT_A; c->idtr_limit = 0x4FF;
    c->gpr[RSP] = 0x5000;
    c->rip = MAIN64_A;

    /* run until the handler has BOTH marked and issued its (guest-side)
     * EOI -- checking only the marker would stop mid-handler */
    for (int i = 0; i < 2000 && !(mem_read(&f->m, 0x2300, 1) && lr(f, isr_reg(0x40)) == 0); i++)
        (void)cpu_step(c);
    assert(mem_read(&f->m, 0x2300, 1) == 1);          /* 64-bit handler ran */
    assert(c->seg[SEG_CS].l == 1);                    /* still in long mode */
    assert(lr(f, isr_reg(0x40)) == 0);                /* guest-side EOI done */
    assert(lr(f, LAPIC_TMCCUR) == 0);                 /* one-shot consumed */
}

static void test_timer_long_mode_fx(void) {
    fx_t f; fx_init(&f);
    test_timer_long_mode_impl(&f);
}

/* --------------------------------------------------- MSR + HLT wake */

static void test_msr_apic_base(void) {
    fx_t f; fx_init(&f);
    int found = 0;
    uint64_t v = cpu_get_msr(&f.m.cpu, LAPIC_MSR_APICBASE, &found);
    assert(found && v == LAPIC_MSR_DEFAULT);
    /* guest RDMSR: mov ecx,0x1B; rdmsr; mov [0x2400],eax; jmp $ */
    static const uint8_t T_RDMSR[] = {
        0xB9, 0x1B,0x00,0x00,0x00,
        0x0F, 0x32,
        0x66, 0xA3, 0x00,0x24,
        0xEB, 0xFE };
    load(&f, 0, T_RDMSR, sizeof T_RDMSR);
    f.m.cpu.rip = 0;
    for (int i = 0; i < 16; i++) (void)cpu_step(&f.m.cpu);
    assert(mem_read(&f.m, 0x2400, 4) == (uint32_t)LAPIC_MSR_DEFAULT);
    printf("ok: msr_apic_base\n");
}

static void test_hlt_wake_via_ipi_and_latched_irr(void) {
    fx_t f; fx_init(&f);
    ivt(&f.m, 0x40, PH_MARK); load(&f, PH_MARK, H_MARK, sizeof H_MARK);
    /* hlt; mov byte[0x2101],1; sti; jmp $  -- IF=0 on entry (reset state) */
    static const uint8_t T_HLT[] = {
        0xF4,
        0xC6, 0x06, 0x01,0x21, 0x01,
        0xFB, 0xEB, 0xFE };
    load(&f, 0, T_HLT, sizeof T_HLT);
    lw(&f, LAPIC_SVR, 0x1FF);
    f.m.cpu.rip = 0;
    for (int i = 0; i < 100 && !f.m.cpu.halted; i++) (void)cpu_step(&f.m.cpu);
    assert(f.m.cpu.halted);
    assert(mem_read(&f.m, 0x2101, 1) == 0);
    /* (The D6 virtual timebase freezes under HLT -- so the wake source
     * here is a self-IPI, not the virtual-time LAPIC timer. Same class
     * of semantics, no virtual time needed.) */
    lw(&f, LAPIC_ICR_LO, (1u << 18) | 0x40);     /* self-IPI vector 0x40 */
    for (int i = 0; i < 100 && mem_read(&f.m, 0x2101, 1) == 0; i++)
        (void)cpu_step(&f.m.cpu);
    assert(mem_read(&f.m, 0x2101, 1) == 1);      /* woke, resumed past HLT */
    assert(mem_read(&f.m, 0x2100, 1) == 0);      /* vector NOT delivered (IF=0) */
    assert(lr(&f, irr_reg(0x40)) == 1);          /* ...still latched in IRR */
    run_until(&f, 0x2100, 50);
    assert(mem_read(&f.m, 0x2100, 1) == 1);      /* STI delivers the latch */
    lw(&f, LAPIC_EOI, 0);
    printf("ok: hlt_wake_via_ipi_and_latched_irr\n");
}

int main(void) {
    test_mmio_registers();
    test_svr_gates_delivery_not_latching();
    test_timer_oneshot_real_mode();
    test_timer_periodic_real_mode();
    test_tpr_arbitration();
    test_self_ipi();
    test_timer_long_mode_fx();  printf("ok: timer_long_mode\n");
    test_msr_apic_base();
    test_hlt_wake_via_ipi_and_latched_irr();
    printf("test_lapic: all vectors passed\n");
    return 0;
}
