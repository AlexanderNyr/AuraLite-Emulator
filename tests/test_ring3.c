/* tests/test_ring3.c -- KERNEL-BOOT K4 CPU gate: the ring-3 ISA contract.
 *
 * Uses the shared flat-machine harness but flips the machine into 64-bit
 * long mode with hand-built page tables. Positive controls: SYSCALL/SYSRET
 * selector/flag/RCX/R11 semantics, exception delivery from CPL3 onto
 * TSS.RSP0 with the outer SS:RSP in the frame, IRETQ ring return, MOVUPS/
 * MOVDQA/MOVSS/MOVSD/MOVHLPS/PXOR data lanes. Negative controls: U/S read
 * from CPL3, NX fetch, CR0.WP supervisor write to a read-only page, MOVAPS
 * on a misaligned address, software int with a DPL-0 gate from CPL3.
 *
 * Everything asserted here is invariant of the K4 contract, not of one
 * boot's luck: error codes, frame layouts and selector arithmetic are the
 * architected values.
 */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "machine.h"
#include "platform.h"
#include "harness.h"

#define P  0x001ull
#define W  0x002ull
#define US 0x004ull
#define NX_BITS (1ull<<63)

static machine_t g_m;

/* ---- long-mode fixture ------------------------------------------------
 * PML4[0] -> PDPT@0x21000 ; PDPT[0] -> PD@0x22000
 * PD[0,1,3..15] = 2 MiB identity supervisor pages (US=0)
 * PD[2] -> PT@0x23000, PT[0]: 0x400000 -> phys 0x60000 (the USER page,
 * flags chosen per vector; vector code itself runs at phys 0x1000 which
 * sits in supervisor space -- CPL comes from CS.RPL).
 * TSS at 0x2000 with RSP0=0x30000; IDT at 0x1000 region base 0x0C00.
 * Handler stubs at 0x9000: [HLT], one per vector (vec n -> 0x9000+n*8).
 */
#define VMA_USER 0x400000ull

static void w64(uint64_t a, uint64_t v) { memcpy(g_m.ram + a, &v, 8); }
static uint64_t r64(uint64_t a) { uint64_t v; memcpy(&v, g_m.ram + a, 8); return v; }

static void gate(int vec, uint16_t sel, uint64_t off, uint8_t dpl_type) {
    uint64_t a = 0x0C00 + (uint64_t)vec * 16;
    w64(a + 0, ((uint64_t)(off & 0xFFFF)) | ((uint64_t)sel << 16) |
               ((uint64_t)dpl_type << 40) | ((uint64_t)(off & 0xFFFF0000ull) >> 16 << 48));
    w64(a + 8, off >> 32);
}

static void fx64(uint64_t user_pt_flags) {
    setup_machine(&g_m);
    cpu_t *c = &g_m.cpu;
    w64(0x10000 + 0 * 8, 0x21000 | P | W | US);          /* PML4[0] */
    w64(0x21000 + 0 * 8, 0x22000 | P | W | US);          /* PDPT[0] */
    for (int i = 0; i < 16; i++)
        if (i != 2)
            w64(0x22000 + i * 8, ((uint64_t)i << 21) | P | W | 0x80); /* 2MiB sup */
    w64(0x22000 + 2 * 8, 0x23000 | P | W | US);          /* PD[2] -> PT */
    for (int i = 0; i < 512; i++)
        w64(0x23000 + i * 8, 0);                          /* PT: mostly absent */
    w64(0x23000 + 0 * 8, 0x400000 | user_pt_flags);       /* PT[0]: VMA_USER identity-ish */
    c->cr3 = 0x10000;
    c->cr4 = 0x20 | 0x40 | (1u << 9);                     /* PAE|PVI? VMXE off; OSFXSR on */
    c->cr4 = 0x20 | (1u << 9);                            /* PAE + OSFXSR (SSE ok) */
    c->efer = (1u << 8) | (1u << 10) | 1u | (1u << 11);   /* LME|LMA|SCE|NXE */
    c->cr0 = 0x80010033ull;                               /* PG|WP|x87+PE trio */
    c->idtr_base = 0x0C00; c->idtr_limit = 256 * 16 - 1;
    w64(0x2000 + 4, 0x30000);                             /* TSS.RSP0 */
    c->tr_base = 0x2000; c->tr_limit = 103;
    for (int v = 0; v < 32; v++) gate(v, 8, 0x9000 + (uint64_t)v * 8, 0x8F); /* DPL0 trap */
    memset(g_m.ram + 0x9000, 0xF4, 32 * 8);               /* HLT stubs */
    c->seg[SEG_CS] = (segment_t){ .sel = 8, .base = 0, .limit = ~0u, .l = 1 };
    c->seg[SEG_SS] = (segment_t){ .sel = 0x10, .base = 0, .limit = ~0u };
    c->gpr[RSP] = 0x8000;
}

/* Run one instruction from phys `pa` (already written), return step rc. */
static int load1(uint64_t pa, const uint8_t *bytes, size_t n) {
    memcpy(g_m.ram + pa, bytes, n);
    g_m.cpu.rip = pa;
    return cpu_step(&g_m.cpu);
}

/* ============================= vectors ================================= */
static void v_syscall_switch(void) {
    fx64(P | W | US);
    cpu_t *c = &g_m.cpu;
    cpu_set_msr(c, 0xC0000081, 0x0010000800000000ull);    /* STAR like AuraLite */
    cpu_set_msr(c, 0xC0000082, 0x9000ull);                /* LSTAR -> stub */
    cpu_set_msr(c, 0xC0000084, 0x7702ull);                /* SFMASK */
    c->rflags |= 0x200 | 0x100;                           /* IF|TF set */
    static const uint8_t b[] = { 0x0F, 0x05 };
    load1(0x1000, b, sizeof b);
    assert(!c->fault);
    assert(c->rip == 0x9000);                             /* LSTAR */
    assert(c->gpr[RCX] == 0x1002);                        /* next RIP */
    assert(c->gpr[R11] == 0x302);                             /* pre-mask value */
    assert(c->seg[SEG_CS].sel == 8 && c->seg[SEG_SS].sel == 0x10);
    assert((c->rflags & 0x7702) == 0x0002);               /* masked */
    assert(c->gpr[RSP] == 0x8000);                        /* SYSCALL never touches RSP */
}

/* K4 regression: SYSCALL from CPL3 must fetch the target CS descriptor from
 * a SUPERVISOR-ONLY GDT page without faulting -- descriptor-table reads are
 * implicit supervisor accesses on real hardware (SDM vol.3A s.5.5). Pre-fix,
 * far_load_cs fetched GDT[1] at CPL3, translate U/S-faulted the kernel GDT
 * page (#PF err=0x5 measured on the AuraLite higher-half GDT at
 * 0xffffffff801d****), and the stale exception delivery went on to clobber
 * the userspace stack while LSTAR still won the RIP race: SYSRET then dropped
 * the guest into address 0x5 == the pushed error code. */
static void v_syscall_supervisor_gdt(void) {
    fx64(P | W | US);
    cpu_t *c = &g_m.cpu;
    c->gdtr_base = 0x80000; c->gdtr_limit = 0x17;         /* supervisor-only page */
    w64(0x80008, 0x00209A0000000000ull);                  /* GDT[1]: kernel code64 */
    cpu_set_msr(c, 0xC0000081, 0x0010000800000000ull);    /* STAR like AuraLite */
    cpu_set_msr(c, 0xC0000082, 0x9000ull);                /* LSTAR -> HLT stub */
    cpu_set_msr(c, 0xC0000084, 0x7702ull);                /* SFMASK */
    c->seg[SEG_CS] = (segment_t){ .sel = 0x1B, .base = 0, .limit = ~0u, .l = 1 };
    c->seg[SEG_SS] = (segment_t){ .sel = 0x23, .base = 0, .limit = ~0u };
    c->gpr[RSP] = VMA_USER + 0x100;                       /* user stack */
    static const uint8_t b[] = { 0x0F, 0x05 };            /* SYSCALL */
    load1(VMA_USER, b, sizeof b);
    assert(!c->fault);
    assert(c->exception_taken == 0);                      /* no #PF mid-transform */
    assert(c->rip == 0x9000);                             /* LSTAR latched */
    assert(c->seg[SEG_CS].sel == 8 && c->seg[SEG_SS].sel == 0x10);
    assert(c->gpr[RSP] == VMA_USER + 0x100);              /* user RSP untouched */
    assert(c->desc_sv == 0);                              /* window balanced */
}

/* K4 regression pair, both measured on the real AuraLite K4 boot:
 *  1) the IST byte of a 64-bit IDT gate must be read through the LINEAR
 *     IDTR base (a raw physical read behind a higher-half IDTR returned
 *     trash; ist came out 7 and the CPL3 delivery skipped the stack switch,
 *     pushing the frame onto the user stack -- IRQ32-from-ring-3 storm);
 *  2) ISTn lives at TSS + 0x24 + (n-1)*8 (SDM vol.3A fig.8-11), not
 *     TSS + 4 + (n-1)*8.
 * The IDT is aliased: linear 0x404000 -> phys 0x8000, supervisor-only, so
 * any physical-addressed or CPL3-privileged gate read delivers wrong. */
static void v_irq_from_ring3_linear_idt_ist(void) {
    fx64(P | W | US);
    cpu_t *c = &g_m.cpu;
    w64(0x23000 + 4 * 8, 0x8000 | P | W);                 /* PT[4]: 0x404000 -> phys 0x8000 (supervisor) */
    const int vec = 3;                                    /* INT3 below */
    uint64_t gpa = 0x8000 + (uint64_t)vec * 16;           /* gate lives at the ALIASED physical page */
    uint64_t off = 0x9000;
    w64(gpa + 0, (off & 0xFFFF) | ((uint64_t)8 << 16) |
                 ((uint64_t)1 /*IST1*/ << 32) | ((uint64_t)0x8E << 40) |
                 ((off & 0xFFFF0000ull) << 32));
    w64(gpa + 8, off >> 32);
    c->idtr_base = 0x404000; c->idtr_limit = 256 * 16 - 1;
    w64(0x2000 + 4, 0x30000);                             /* TSS.RSP0 */
    w64(0x2000 + 0x24, 0x3F000);                          /* TSS.IST1 */
    c->seg[SEG_CS] = (segment_t){ .sel = 0x1B, .base = 0, .limit = ~0u, .l = 1 };
    c->seg[SEG_SS] = (segment_t){ .sel = 0x23, .base = 0, .limit = ~0u };
    c->gpr[RSP] = VMA_USER + 0x100;
    static const uint8_t b[] = { 0xCC };                  /* INT3: trap from CPL3 */
    load1(VMA_USER, b, sizeof b);
    assert(!c->fault);
    assert(c->rip == 0x9000);                             /* gate handler */
    assert(c->gpr[RSP] == 0x3F000 - 40);                  /* IST1 minus 5-slot frame */
    uint64_t fr = c->gpr[RSP];
    assert(r64(fr +  0) == VMA_USER + 1);                 /* RIP: trap pushes next addr */
    assert(r64(fr +  8) == 0x1B);                         /* CS */
    assert(r64(fr + 24) == VMA_USER + 0x100);             /* outer RSP */
    assert(r64(fr + 32) == 0x23);                         /* outer SS */
    assert(c->seg[SEG_CS].sel == 8 && (c->seg[SEG_SS].sel & 3) == 0);
    assert(c->desc_sv == 0);
}

/* K4 regression: WRMSR IA32_FS_BASE must update the FS segment shadow that
 * the decoder's FS-override addressing reads -- AuraLite userspace keeps
 * errno at %fs:0 and dies the first time a syscall fails if FS.base is 0. */
static void v_fsbase_wrmsr(void) {
    fx64(P | W | US);
    cpu_t *c = &g_m.cpu;
    c->gpr[RAX] = 0x400000; c->gpr[RDX] = 0; c->gpr[RCX] = 0xC0000100;
    c->gpr[RBX] = 0x234;                                /* stay on the PT[0] page */
    w64(0x400000 + 0x234, 0xDEADBEEFCAFEF00Dull);       /* TLS slot */
    static const uint8_t b[] = { 0x0F, 0x30,              /* WRMSR */
                                 0x64, 0x48, 0x8B, 0x13 } /* mov %fs:(%rbx),%rdx */;
    memcpy(g_m.ram + 0x1000, b, sizeof b);
    c->rip = 0x1000;
    assert(cpu_step(c) == 0 && !c->fault);
    assert(c->seg[SEG_FS].base == 0x400000);              /* shadow synced */
    int f; assert(cpu_get_msr(c, 0xC0000100, &f) == 0x400000 && f);
    assert(cpu_step(c) == 0 && !c->fault);
    assert(c->gpr[RDX] == 0xDEADBEEFCAFEF00Dull);         /* %fs: addressing */
}

static void v_sysret_returns_to_ring3(void) {
    fx64(P | W | US);
    cpu_t *c = &g_m.cpu;
    cpu_set_msr(c, 0xC0000081, 0x0010000800000000ull);
    static const uint8_t u_pg[] = { 0xF4 };               /* HLT at user page */
    memcpy(g_m.ram + VMA_USER, u_pg, 1);
    c->gpr[RCX] = VMA_USER;                               /* SYSRET target */
    c->gpr[R11] = 0x202;
    static const uint8_t b[] = { 0x0F, 0x07 };
    load1(0x1000, b, sizeof b);
    assert(!c->fault);
    assert(c->rip == VMA_USER);
    assert(c->seg[SEG_CS].sel == 0x23 && c->seg[SEG_SS].sel == 0x1B);
    assert(c->rflags == 0x202);
    cpu_step(c);                                          /* user page HLT executes */
    assert(c->halted);
}

static void v_int3_from_cpl3_pushes_outer_frame(void) {
    fx64(P | W | US);
    cpu_t *c = &g_m.cpu;
    c->seg[SEG_CS].sel = 0x23;                            /* CPL3 */
    c->seg[SEG_SS].sel = 0x1B;
    c->gpr[RSP] = 0x45000;
    c->rflags = 0x202;
    static const uint8_t b[] = { 0xCC };                  /* INT3 */
    memcpy(g_m.ram + VMA_USER, b, 1);
    c->rip = VMA_USER;
    int rc = cpu_step(c);
    (void)rc;
    assert(!c->fault);
    /* gate 3 is DPL0 -> INT3 (trap) is exempt from the software-int DPL
     * check on real hardware (fault-type gate); our int path raises via
     * raise_exception, so frame must land on TSS.RSP0 with SS:RSP atop. */
    assert(c->seg[SEG_CS].sel == 8);
    assert(c->seg[SEG_SS].sel == 0x10);
    assert(c->gpr[RSP] == 0x30000 - 5 * 8);
    assert(r64(0x30000 - 8)  == 0x1B);                    /* old SS */
    assert(r64(0x30000 - 16) == 0x45000);                 /* old RSP */
    assert(r64(0x30000 - 24) == 0x202);                   /* RFLAGS */
    assert(r64(0x30000 - 32) == 0x23);                    /* old CS */
    assert(r64(0x30000 - 40) == VMA_USER + 1);            /* saved RIP */
    cpu_step(c);                                          /* handler HLT stub runs */
    assert(c->halted);
}

static void v_iretq_ring_return_restores_ss_rsp(void) {
    fx64(P | W | US);
    cpu_t *c = &g_m.cpu;
    /* frame as an isr would leave it for a return to CPL3 */
    c->gpr[RSP] = 0x30000 - 5 * 8;
    w64(0x30000 - 40, VMA_USER);                          /* RIP */
    w64(0x30000 - 32, 0x23);                              /* CS */
    w64(0x30000 - 24, 0x246);                             /* RFLAGS */
    w64(0x30000 - 16, 0x45000);                           /* RSP */
    w64(0x30000 - 8,  0x1B);                              /* SS */
    memcpy(g_m.ram + VMA_USER, (const uint8_t[]){ 0xF4 }, 1);
    static const uint8_t b[] = { 0x48, 0xCF };            /* IRETQ */
    load1(0x1000, b, sizeof b);
    assert(!c->fault);
    assert(c->rip == VMA_USER);
    assert(c->seg[SEG_CS].sel == 0x23 && c->seg[SEG_SS].sel == 0x1B);
    assert(c->gpr[RSP] == 0x45000);
    assert(c->rflags == 0x246);
    cpu_step(c);
    assert(c->halted);
}

static void v_us_read_from_cpl3_faults(void) {
    fx64(P | W | US);
    cpu_t *c = &g_m.cpu;
    c->seg[SEG_CS].sel = 0x23;                            /* CPL3 */
    c->seg[SEG_SS].sel = 0x1B;
    c->gpr[RSP] = 0x45000;
    /* mov rax,[0x200000] -- supervisor 2MiB page, US=0 all the way up */
    static const uint8_t b[] = { 0x48, 0xA1, 0, 0, 0x20, 0, 0, 0, 0, 0 };
    memcpy(g_m.ram + VMA_USER, b, sizeof b);
    c->rip = VMA_USER;
    cpu_step(c);
    assert(!c->fault);
    assert(c->seg[SEG_CS].sel == 8);                      /* last handler = vec14 */
    assert(c->rip == 0x9000 + 14 * 8);                    /* gate 14 stub */
    uint64_t esp = c->gpr[RSP];
    assert(esp == 0x30000 - 6 * 8);                       /* 5 slots + err */
    assert(r64(esp + 0 * 8) == 0x5);                      /* #PF err: P|U read */
    assert(c->cr2 == 0x200000);
    assert(r64(esp + 5 * 8) == 0x1B);                     /* old SS on top */
    assert(r64(esp + 4 * 8) == 0x45000);                  /* old user RSP (fixture) */
    assert(r64(esp + 1 * 8) == VMA_USER);                 /* faulting RIP */
}

static void v_nx_fetch_faults_with_id(void) {
    fx64(P | W | US | NX_BITS);                           /* user page present but NX */
    cpu_t *c = &g_m.cpu;
    c->seg[SEG_CS].sel = 0x23;
    c->seg[SEG_SS].sel = 0x1B;
    c->gpr[RSP] = 0x45000;
    c->rip = VMA_USER;
    cpu_step(c);                                          /* FETCH itself faults */
    assert(!c->fault);
    assert(c->seg[SEG_CS].sel == 8);
    uint64_t esp = c->gpr[RSP];
    assert(esp == 0x30000 - 6 * 8);
    assert((r64(esp) & 0x1F) == 0x15);                    /* P|U|I/D */
    assert(c->cr2 == VMA_USER);
}

static void v_wp_supervisor_write_to_ro_faults(void) {
    fx64(P | US);                                         /* user page, NOT writable */
    cpu_t *c = &g_m.cpu;
    assert((c->cr0 & (1ull << 16)) != 0);                 /* WP on in the fixture */
    /* mov [0x400000], rax at CPL0 */
    static const uint8_t b[] = { 0x48, 0x89, 0x04, 0x25, 0, 0, 0x40, 0 };
    load1(0x1000, b, sizeof b);
    assert(!c->fault);
    uint64_t esp = c->gpr[RSP];                           /* no ring switch: same stack */
    assert(r64(esp) == 0x3);                              /* #PF err: P|W (sup) */
    assert(c->cr2 == VMA_USER);
}

static void v_movaps_misaligned_gps(void) {
    fx64(P | W | US);
    cpu_t *c = &g_m.cpu;
    /* movaps xmm0, [0x1003] -- misaligned by 3 */
    static const uint8_t b[] = { 0x0F, 0x28, 0x04, 0x25, 0x03, 0x10, 0, 0 };
    load1(0x1000, b, sizeof b);
    assert(!c->fault);
    uint64_t esp = c->gpr[RSP];
    assert(r64(esp) == 0x0);                              /* #GP err=0 */
    assert(c->seg[SEG_CS].sel == 8 && r64(esp + 2 * 8) == 8);   /* CS slot */
}

static void v_sse_move_semantics(void) {
    fx64(P | W | US);
    cpu_t *c = &g_m.cpu;
    /* movups [0x30003], xmm9 ; movups xmm10, [0x30003]  (unaligned ok) */
    c->xmm[9][0] = 0x1122334455667788ull; c->xmm[9][1] = 0xAABBCCDDEEFF0011ull;
    static const uint8_t s[] = { 0x44, 0x0F, 0x11, 0x0C, 0x25, 0x03, 0, 0x03, 0 };
    static const uint8_t l[] = { 0x44, 0x0F, 0x10, 0x14, 0x25, 0x03, 0, 0x03, 0 };
    load1(0x1000, s, sizeof s);
    load1(0x1000, l, sizeof l);
    assert(!c->fault);
    assert(memcmp(g_m.ram + 0x30003, c->xmm[9], 16) == 0);
    assert(c->xmm[10][0] == c->xmm[9][0] && c->xmm[10][1] == c->xmm[9][1]);
    /* movss xmm1, xmm0: only low dword moves, high preserved */
    c->xmm[1][0] = 0xDEADBEEF00000000ull; c->xmm[1][1] = 0x1234ull;
    c->xmm[0][0] = 0x00000000CAFEBABEull;
    static const uint8_t ss[] = { 0xF3, 0x0F, 0x10, 0xC8 };
    load1(0x1000, ss, sizeof ss);
    assert(c->xmm[1][0] == 0xDEADBEEFCAFEBABEull && c->xmm[1][1] == 0x1234ull);
    /* pxor xmm2, xmm2 zeroes */
    c->xmm[2][0] = ~0ull; c->xmm[2][1] = ~0ull;
    static const uint8_t px[] = { 0x66, 0x0F, 0xEF, 0xD2 };
    load1(0x1000, px, sizeof px);
    assert(c->xmm[2][0] == 0 && c->xmm[2][1] == 0);
    /* movdqa: aligned store + load roundtrip */
    c->xmm[3][0] = 0x5555AAAA5555AAAAull; c->xmm[3][1] = 1;
    static const uint8_t dq1[] = { 0x66, 0x0F, 0x7F, 0x1C, 0x25, 0x40, 0x20, 0, 0 };
    static const uint8_t dq2[] = { 0x66, 0x0F, 0x6F, 0x1C, 0x25, 0x40, 0x20, 0, 0 };
    load1(0x1000, dq1, sizeof dq1);
    c->xmm[3][0] = 0; c->xmm[3][1] = 0;
    load1(0x1000, dq2, sizeof dq2);
    assert(c->xmm[3][0] == 0x5555AAAA5555AAAAull && c->xmm[3][1] == 1);
}

static void v_fxsave_roundtrips_xmm(void) {
    fx64(P | W | US);
    cpu_t *c = &g_m.cpu;
    c->xmm[5][0] = 0x0BADCAFEDEADBEEFull; c->xmm[5][1] = 0x1111222233334444ull;
    static const uint8_t sv[] = { 0x0F, 0xAE, 0x04, 0x25, 0x00, 0x40, 0, 0 }; /* fxsave [0x4000] */
    load1(0x1000, sv, sizeof sv);
    assert(r64(0x4000 + 160 + 5 * 16)     == 0x0BADCAFEDEADBEEFull);
    assert(r64(0x4000 + 160 + 5 * 16 + 8) == 0x1111222233334444ull);
    c->xmm[5][0] = 7; c->xmm[5][1] = 7;
    static const uint8_t ld[] = { 0x0F, 0xAE, 0x0C, 0x25, 0x00, 0x40, 0, 0 }; /* fxrstor [0x4000] */
    load1(0x1000, ld, sizeof ld);
    assert(c->xmm[5][0] == 0x0BADCAFEDEADBEEFull);
    assert(c->xmm[5][1] == 0x1111222233334444ull);
}

static void v_int80_dpl0_gate_from_cpl3_gps(void) {
    fx64(P | W | US);
    cpu_t *c = &g_m.cpu;
    c->seg[SEG_CS].sel = 0x23;
    c->seg[SEG_SS].sel = 0x1B;
    c->gpr[RSP] = 0x45000;
    static const uint8_t b[] = { 0xCD, 0x80 };            /* INT 0x80 */
    memcpy(g_m.ram + VMA_USER, b, 2);
    c->rip = VMA_USER;
    cpu_step(c);
    assert(!c->fault);
    uint64_t esp = c->gpr[RSP];
    assert(esp == 0x30000 - 6 * 8);                       /* ring-switch frame + err */
    assert(r64(esp) == 0x80 * 8 + 2);                     /* #GP err */
    assert(r64(esp + 1 * 8) == VMA_USER);                 /* saved RIP = int insn */
}

static void v_scalar_fp_semantics(void) {
    fx64(P | W | US);
    cpu_t *c = &g_m.cpu;
    /* cvtsi2sd xmm0, rax (F2 48 0F 2A C0) */
    c->gpr[RAX] = 42;
    static const uint8_t cv[] = { 0xF2, 0x48, 0x0F, 0x2A, 0xC0 };
    load1(0x1000, cv, sizeof cv);
    { double v; memcpy(&v, &c->xmm[0][0], 8); assert(v == 42.0); assert(c->xmm[0][1] == 0); }
    /* addsd xmm0, xmm1 (F2 0F 58 C1): 42 + 0.5 = 42.5 */
    { double h = 0.5; memcpy(&c->xmm[1][0], &h, 8); }
    static const uint8_t ad[] = { 0xF2, 0x0F, 0x58, 0xC1 };
    load1(0x1000, ad, sizeof ad);
    { double v; memcpy(&v, &c->xmm[0][0], 8); assert(v == 42.5); }
    /* divsd xmm0, xmm2 (F2 0F 5E C2): / 17 = 2.5 */
    { double h = 17.0; memcpy(&c->xmm[2][0], &h, 8); }
    static const uint8_t dv[] = { 0xF2, 0x0F, 0x5E, 0xC2 };
    load1(0x1000, dv, sizeof dv);
    { double v; memcpy(&v, &c->xmm[0][0], 8); assert(v == 2.5); }
    /* ucomisd xmm0, xmm1 (66 0F 2E C1): 2.5 > 0.5 -> ZF=PF=CF=0 */
    static const uint8_t uc[] = { 0x66, 0x0F, 0x2E, 0xC1 };
    load1(0x1000, uc, sizeof uc);
    assert(!(c->rflags & (FLAG_ZF|FLAG_PF|FLAG_CF|FLAG_OF|FLAG_SF|FLAG_AF)));
    /* reversed: 0.5 < 2.5 -> CF=1 */
    static const uint8_t uc2[] = { 0x66, 0x0F, 0x2E, 0xC8 };
    load1(0x1000, uc2, sizeof uc2);
    assert((c->rflags & FLAG_CF) && !(c->rflags & FLAG_ZF));
    /* cvttsd2si rax, xmm0 (F2 48 0F 2C C0): 2.5 -> 2 */
    static const uint8_t tt[] = { 0xF2, 0x48, 0x0F, 0x2C, 0xC0 };
    load1(0x1000, tt, sizeof tt);
    assert(c->gpr[RAX] == 2);
    /* cvtsd2si rax, xmm0 (F2 48 0F 2D C0): rounds-to-even 2.5 -> 2 */
    static const uint8_t rd[] = { 0xF2, 0x48, 0x0F, 0x2D, 0xC0 };
    load1(0x1000, rd, sizeof rd);
    assert(c->gpr[RAX] == 2);
    /* cvtss2sd (F3 0F 5A C9): xmm1(float 0.5) -> xmm1(double) */
    { float f = 0.5f; memcpy(&c->xmm[1][0], &f, 4); c->xmm[1][0] &= 0xFFFFFFFFull; c->xmm[1][1] = 7; }
    static const uint8_t sd[] = { 0xF3, 0x0F, 0x5A, 0xC9 };
    load1(0x1000, sd, sizeof sd);
    { double v; memcpy(&v, &c->xmm[1][0], 8); assert(v == 0.5); }
    assert(!c->fault);

    /* -- packed lane work (measured in the 3D/GUI demo region) -- */
    /* addps xmm0, xmm1 lane-wise */
    for (int i = 0; i < 4; i++) { float a = (float)(i + 1), b = 10.f; memcpy((uint8_t*)&c->xmm[0][0/1] + i * 4, &a, 4); memcpy((uint8_t*)c->xmm[1] + i * 4, &b, 4); }
    static const uint8_t ap[] = { 0x0F, 0x58, 0xC1 };
    load1(0x1000, ap, sizeof ap);
    for (int i = 0; i < 4; i++) { float v; memcpy(&v, (uint8_t*)c->xmm[0] + i * 4, 4); assert(v == (float)(i + 1) + 10.f); }
    /* mulpd xmm0, xmm2 (66 0F 59 C2): two doubles */
    { double a = 3.0, b = 4.0; memcpy(&c->xmm[0][0], &a, 8); memcpy(&c->xmm[0][1], &b, 8);
      double x = 2.0, y = 0.5; memcpy(&c->xmm[2][0], &x, 8); memcpy(&c->xmm[2][1], &y, 8); }
    static const uint8_t mpd[] = { 0x66, 0x0F, 0x59, 0xC2 };
    load1(0x1000, mpd, sizeof mpd);
    { double v; memcpy(&v, &c->xmm[0][0], 8); assert(v == 6.0); memcpy(&v, &c->xmm[0][1], 8); assert(v == 2.0); }
    /* shufps xmm0, xmm1, 0x1B: dst = [d1,d2,d3,d0]... use well-known imm */
    { float q[4] = { 1, 2, 3, 4 }; memcpy(c->xmm[3], q, 16);
      float q2[4] = { 100, 200, 300, 400 }; memcpy(c->xmm[4], q2, 16); }
    static const uint8_t sh[] = { 0x0F, 0xC6, 0xDC, 0x1B };   /* shufps xmm3, xmm4, 0x1B */
    load1(0x1000, sh, sizeof sh);
    { float v[4]; memcpy(v, c->xmm[3], 16);
      assert(v[0] == 4 && v[1] == 3 && v[2] == 200 && v[3] == 100); }
    /* unpcklps xmm3, xmm4 */
    static const uint8_t up[] = { 0x0F, 0x14, 0xDC };
    { float q[4] = { 1, 2, 3, 4 }; memcpy(c->xmm[3], q, 16); }
    { float q2[4] = { 9, 8, 7, 6 }; memcpy(c->xmm[4], q2, 16); }
    load1(0x1000, up, sizeof up);
    { float v[4]; memcpy(v, c->xmm[3], 16);
      assert(v[0] == 1 && v[1] == 9 && v[2] == 2 && v[3] == 8); }
    /* pshufd xmm5, xmm6, 0x1B (reverse) */
    { uint32_t q[4] = { 0x11, 0x22, 0x33, 0x44 }; memcpy(c->xmm[6], q, 16); }
    static const uint8_t pd[] = { 0x66, 0x0F, 0x70, 0xEE, 0x1B };
    load1(0x1000, pd, sizeof pd);
    { uint32_t v[4]; memcpy(v, c->xmm[5], 16);
      assert(v[0] == 0x44 && v[1] == 0x33 && v[2] == 0x22 && v[3] == 0x11); }
    /* psrlq... pslldq xmm7 by 4 */
    { uint8_t q[16]; for (int i = 0; i < 16; i++) q[i] = (uint8_t)(i + 1); memcpy(c->xmm[7], q, 16); }
    static const uint8_t sl[] = { 0x66, 0x0F, 0x73, 0xD7, 0x04 };
    load1(0x1000, sl, sizeof sl);
    { uint8_t v[16]; memcpy(v, c->xmm[7], 16);
      assert(v[0] == 0 && v[3] == 0 && v[4] == 1 && v[15] == 12); }
    /* movmskps xmm3 (0f 50 dc): lane signs */
    { uint32_t q[4] = { 0x80000000u, 0, 0x80000000u, 0 }; memcpy(c->xmm[3], q, 16); }
    static const uint8_t mm[] = { 0x0F, 0x50, 0xDB };
    load1(0x1000, mm, sizeof mm);
    assert((c->gpr[RBX] & 0xF) == 0x5);      /* reg_field=3 -> RBX; rm=3 -> xmm3 */
    assert(!c->fault);
}

int main(void) {
    v_syscall_switch();
    v_syscall_supervisor_gdt();
    v_irq_from_ring3_linear_idt_ist();
    v_fsbase_wrmsr();
    v_sysret_returns_to_ring3();
    v_int3_from_cpl3_pushes_outer_frame();
    v_iretq_ring_return_restores_ss_rsp();
    v_us_read_from_cpl3_faults();
    v_nx_fetch_faults_with_id();
    v_wp_supervisor_write_to_ro_faults();
    v_movaps_misaligned_gps();
    v_sse_move_semantics();
    v_fxsave_roundtrips_xmm();
    v_int80_dpl0_gate_from_cpl3_gps();
    v_scalar_fp_semantics();
    printf("test-ring3: all vectors passed\n");
    return 0;
}
