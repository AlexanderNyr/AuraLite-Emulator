/* tests/test_cpu.c -- flat-machine ISA regression vectors.
 *
 * The second half of this file locks the repairs audited and measured in
 * docs/plans/CORE_PLAN.md (phases C1-C5). Every vector was observed RED
 * against the C0 baseline before its fix landed, so the suite fails if
 * any of those fixes is reverted. See CORE_PLAN.md for the baseline
 * measurements.
 */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "machine.h"
#include "platform.h"
#include "harness.h"

/* ------------------------------------------------------------------ tests */

static void test_mov_add_hlt(void) {
    machine_t m;
    setup_machine(&m);

    /* mov ax,123; add ax,10; hlt */
    const uint8_t program[] = {
        0xB8, 0x7B, 0x00,
        0x05, 0x0A, 0x00,
        0xF4
    };
    memcpy(m.ram, program, sizeof program);

    assert(cpu_step(&m.cpu) == 0);
    assert((m.cpu.gpr[RAX] & 0xFFFF) == 123);
    assert(cpu_step(&m.cpu) == 0);
    assert((m.cpu.gpr[RAX] & 0xFFFF) == 133);
    assert(cpu_step(&m.cpu) != 0);
    assert(m.cpu.halted);
    assert(!m.cpu.fault);
}

static void test_memory_round_trip(void) {
    machine_t m;
    setup_machine(&m);

    mem_write(&m, 0x1234, 4, 0xA1B2C3D4);
    assert(mem_read(&m, 0x1234, 4) == 0xA1B2C3D4);
}

static void test_platform_cpuid_profile(void) {
    const platform_t *p = platform_by_name("haswell");
    uint32_t a, b, c, d;
    platform_cpuid(p, 1, 0, &a, &b, &c, &d);
    assert(a == 0x000306C3u);
}

/* ---- C1 vectors: near Jcc, both sides of the branch ---------------------- */

/* Baseline: "#UD unsupported 0F opcode 0x0F 0x80". */
static void test_near_jcc_taken_of_family(void) {
    machine_t m;
    setup_machine(&m);
    /* mov al,0x7F; add al,1   -> OF=1
     * jo +6                   -> must jump over the 0x1111 block to 0x2222 */
    const uint8_t program[] = {
        0xB0, 0x7F,             /* mov al,0x7F */
        0x04, 0x01,             /* add al,1    */
        0x0F, 0x80, 0x06, 0, 0, /* jo near +6  */
        0x00,
        0xB8, 0x11, 0x11,       /* mov ax,0x1111 (not-taken) */
        0xF4,
        0x90, 0x90,
        0xB8, 0x22, 0x22,       /* mov ax,0x2222 (jo target) */
        0xF4
    };
    run_program(&m, program, sizeof program, 0, 16);
    assert(!m.cpu.fault);
    assert(m.cpu.halted);
    assert((m.cpu.gpr[RAX] & 0xFFFF) == 0x2222);
}

/* Baseline: JS near decoded but never branched (ax was 0x3333). */
static void test_near_jcc_taken_sf_family(void) {
    machine_t m;
    setup_machine(&m);
    /* mov al,0xFF; test al,al -> SF=1
     * js +6 -> must jump to 0x4444 */
    const uint8_t program[] = {
        0xB0, 0xFF,             /* mov al,0xFF */
        0x84, 0xC0,             /* test al,al  */
        0x0F, 0x88, 0x06, 0, 0, /* js near +6  */
        0x00,
        0xB8, 0x33, 0x33,       /* mov ax,0x3333 (not-taken) */
        0xF4,
        0x90, 0x90,
        0xB8, 0x44, 0x44,       /* mov ax,0x4444 (js target) */
        0xF4
    };
    run_program(&m, program, sizeof program, 0, 16);
    assert(!m.cpu.fault);
    assert(m.cpu.halted);
    assert((m.cpu.gpr[RAX] & 0xFFFF) == 0x4444);
}

/* Parity family on the not-taken side: mov al,3 -> PF=1, JNP must fall through. */
static void test_near_jcc_not_taken_pf_family(void) {
    machine_t m;
    setup_machine(&m);
    const uint8_t program[] = {
        0xB0, 0x03,             /* mov al,3 (two bits set -> PF=1) */
        0x84, 0xC0,             /* test al,al */
        0x0F, 0x8B, 0x06, 0, 0, /* jnp near +6 (must NOT jump) */
        0x00,
        0xB8, 0x55, 0x55,       /* mov ax,0x5555 (fall-through) */
        0xF4,
        0x90, 0x90,
        0xB8, 0x66, 0x66,       /* mov ax,0x6666 (jnp target) */
        0xF4
    };
    run_program(&m, program, sizeof program, 0, 16);
    assert(!m.cpu.fault);
    assert((m.cpu.gpr[RAX] & 0xFFFF) == 0x5555);
}

/* ---- C2 vectors: shifts, rotates, count masks ---------------------------- */

static void test_rotates_8bit(void) {
    machine_t m;
    setup_machine(&m);
    /* mov al,0x81; rol al,1 -> 0x03, CF=1 */
    const uint8_t prog_rol[] = { 0xB0, 0x81, 0xD0, 0xC0, 0xF4 };
    run_program(&m, prog_rol, sizeof prog_rol, 0, 16);
    assert(!m.cpu.fault);
    assert((m.cpu.gpr[RAX] & 0xFF) == 0x03);
    assert((m.cpu.rflags & FLAG_CF) != 0);

    setup_machine(&m);
    /* mov al,0x80; rcl al,1 -> 0x00, CF=1 (bit leaves through carry) */
    const uint8_t prog_rcl[] = { 0xB0, 0x80, 0xD0, 0xD0, 0xF4 };
    run_program(&m, prog_rcl, sizeof prog_rcl, 0, 16);
    assert(!m.cpu.fault);
    assert((m.cpu.gpr[RAX] & 0xFF) == 0x00);
    assert((m.cpu.rflags & FLAG_CF) != 0);

    setup_machine(&m);
    /* mov ax,0x8001; rol ax,1 -> 0x0003, CF=1 */
    const uint8_t prog_rol16[] = { 0xB8, 0x01, 0x80, 0xD1, 0xC0, 0xF4 };
    run_program(&m, prog_rol16, sizeof prog_rol16, 0, 16);
    assert(!m.cpu.fault);
    assert((m.cpu.gpr[RAX] & 0xFFFF) == 0x0003);
    assert((m.cpu.rflags & FLAG_CF) != 0);
}

static void test_shift_count_mask(void) {
    machine_t m;
    setup_machine(&m);
    /* mov ax,1; mov cl,33; shl ax,cl -> count masked to 5 bits (33&31=1) -> 2 */
    const uint8_t prog_shl[] = { 0xB8, 0x01, 0x00, 0xB1, 0x21, 0xD3, 0xE0, 0xF4 };
    run_program(&m, prog_shl, sizeof prog_shl, 0, 16);
    assert(!m.cpu.fault);
    assert((m.cpu.gpr[RAX] & 0xFFFF) == 2);

    setup_machine(&m);
    /* mov ax,0xFFFF; mov cl,17; shr ax,cl -> count 17 >= 16 -> 0 (test-locked
     * deterministic choice for the architecturally undefined range) */
    const uint8_t prog_shr[] = { 0xB8, 0xFF, 0xFF, 0xB1, 0x11, 0xD3, 0xE8, 0xF4 };
    run_program(&m, prog_shr, sizeof prog_shr, 0, 16);
    assert(!m.cpu.fault);
    assert((m.cpu.gpr[RAX] & 0xFFFF) == 0);

    setup_machine(&m);
    /* mov ax,0xC000; mov cl,15; sar ax,cl -> 0xFFFF sign-filled, CF=bit14=1 */
    const uint8_t prog_sar[] = { 0xB8, 0x00, 0xC0, 0xB1, 0x0F, 0xD3, 0xF8, 0xF4 };
    run_program(&m, prog_sar, sizeof prog_sar, 0, 16);
    assert(!m.cpu.fault);
    assert((m.cpu.gpr[RAX] & 0xFFFF) == 0xFFFF);
    assert((m.cpu.rflags & FLAG_CF) != 0);
}

/* SHRD/SHLD: immediate and CL counts, operand-size override, flags, and
 * the zero-count no-op. The first case is the exact 32-bit form from the
 * G1 diagnostic's next fault (shrd eax,edx,cl). */
static void test_shld_shrd(void) {
    machine_t m;
    setup_machine(&m);
    const uint8_t shrd_cl[] = {
        0x66,0xB8,0x67,0x45,0x23,0x81,       /* mov eax,0x81234567 */
        0x66,0xBA,0xEF,0xCD,0xAB,0x89,       /* mov edx,0x89ABCDEF */
        0xB1,0x21,                            /* mov cl,33 -> masked to 1 */
        0x66,0x0F,0xAD,0xD0,                  /* shrd eax,edx,cl */
        0xF4
    };
    run_program(&m, shrd_cl, sizeof shrd_cl, 0, 16);
    assert(!m.cpu.fault && m.cpu.halted);
    assert(m.cpu.gpr[RAX] == 0xC091A2B3u);
    assert((m.cpu.rflags & (FLAG_CF|FLAG_OF|FLAG_SF|FLAG_ZF|FLAG_PF)) ==
           (FLAG_CF|FLAG_SF));

    setup_machine(&m);
    const uint8_t shld_imm[] = {
        0x66,0xB8,0x67,0x45,0x23,0x81,
        0x66,0xBA,0xEF,0xCD,0xAB,0x89,
        0x66,0x0F,0xA4,0xD0,0x01,              /* shld eax,edx,1 */
        0xF4
    };
    run_program(&m, shld_imm, sizeof shld_imm, 0, 16);
    assert(!m.cpu.fault && m.cpu.halted);
    assert(m.cpu.gpr[RAX] == 0x02468ACFu);
    assert((m.cpu.rflags & (FLAG_CF|FLAG_OF|FLAG_SF|FLAG_ZF|FLAG_PF)) ==
           (FLAG_CF|FLAG_OF|FLAG_PF));

    setup_machine(&m);
    const uint8_t shrd16[] = {
        0xB8,0x34,0x12,                       /* mov ax,0x1234 */
        0xBA,0xCD,0xAB,                       /* mov dx,0xABCD */
        0x0F,0xAC,0xD0,0x04,                  /* shrd ax,dx,4 */
        0xF4
    };
    run_program(&m, shrd16, sizeof shrd16, 0, 16);
    assert(!m.cpu.fault && m.cpu.halted);
    assert((m.cpu.gpr[RAX] & 0xFFFF) == 0xD123);

    setup_machine(&m);
    m.cpu.gpr[RDX] = 0x89ABCDEFu;
    mem_write(&m, 0x2000, 4, 0x81234567u);
    const uint8_t shrd_mem[] = {
        0x66,0x0F,0xAC,0x16,0x00,0x20,0x01, /* shrd dword [0x2000],edx,1 */
        0xF4
    };
    run_program(&m, shrd_mem, sizeof shrd_mem, 0, 16);
    assert(!m.cpu.fault && m.cpu.halted);
    assert(mem_read(&m, 0x2000, 4) == 0xC091A2B3u);

    setup_machine(&m);
    m.cpu.gpr[RAX] = 0x81234567u;
    m.cpu.gpr[RDX] = 0x89ABCDEFu;
    m.cpu.gpr[RCX] = 0;
    const uint64_t original_flags = 0x202u | FLAG_CF | FLAG_PF | FLAG_AF | FLAG_ZF | FLAG_OF;
    m.cpu.rflags = original_flags;
    const uint8_t shrd_zero[] = { 0x0F,0xAD,0xD0,0xF4 };
    run_program(&m, shrd_zero, sizeof shrd_zero, 0, 4);
    assert(!m.cpu.fault && m.cpu.halted);
    assert(m.cpu.gpr[RAX] == 0x81234567u);
    assert(m.cpu.rflags == original_flags);
}

/* ---- C3 vectors: MUL/IMUL/DIV/IDIV --------------------------------------- */

static void test_mul_imul(void) {
    machine_t m;
    setup_machine(&m);
    /* mov ax,0x1234; mov bx,0x100; mul bx -> DX:AX = 0012:3400, CF=OF=1 */
    const uint8_t prog_mul[] = { 0xB8, 0x34, 0x12, 0xBB, 0x00, 0x01, 0xF7, 0xE3, 0xF4 };
    run_program(&m, prog_mul, sizeof prog_mul, 0, 16);
    assert(!m.cpu.fault);
    assert((m.cpu.gpr[RDX] & 0xFFFF) == 0x0012);
    assert((m.cpu.gpr[RAX] & 0xFFFF) == 0x3400);
    assert((m.cpu.rflags & FLAG_CF) && (m.cpu.rflags & FLAG_OF));

    setup_machine(&m);
    /* mov ax,-2; mov bx,3; imul bx -> DX:AX = FFFF:FFFA, CF=OF=0 (upper half
     * is exactly the sign extension of the lower half) */
    const uint8_t prog_imul[] = { 0xB8, 0xFE, 0xFF, 0xBB, 0x03, 0x00, 0xF7, 0xEB, 0xF4 };
    run_program(&m, prog_imul, sizeof prog_imul, 0, 16);
    assert(!m.cpu.fault);
    assert((m.cpu.gpr[RDX] & 0xFFFF) == 0xFFFF);
    assert((m.cpu.gpr[RAX] & 0xFFFF) == 0xFFFA);
    assert(!(m.cpu.rflags & FLAG_CF) && !(m.cpu.rflags & FLAG_OF));
}

static void test_div_idiv(void) {
    machine_t m;
    setup_machine(&m);
    /* dx=0; ax=100; bx=7; div bx -> ax=14, dx=2 */
    const uint8_t prog_div[] = { 0xBA, 0x00, 0x00, 0xB8, 0x64, 0x00,
                                 0xBB, 0x07, 0x00, 0xF7, 0xF3, 0xF4 };
    run_program(&m, prog_div, sizeof prog_div, 0, 16);
    assert(!m.cpu.fault);
    assert((m.cpu.gpr[RAX] & 0xFFFF) == 14);
    assert((m.cpu.gpr[RDX] & 0xFFFF) == 2);

    setup_machine(&m);
    /* dx:ax = -10; bx=3; idiv bx -> q=-3, r=-1 (truncation toward zero) */
    const uint8_t prog_idiv[] = { 0xBA, 0xFF, 0xFF, 0xB8, 0xF6, 0xFF,
                                  0xBB, 0x03, 0x00, 0xF7, 0xFB, 0xF4 };
    run_program(&m, prog_idiv, sizeof prog_idiv, 0, 16);
    assert(!m.cpu.fault);
    assert((m.cpu.gpr[RAX] & 0xFFFF) == 0xFFFD);
    assert((m.cpu.gpr[RDX] & 0xFFFF) == 0xFFFF);
}

/* ---- C4 vectors: PUSH imm16 keeps the instruction stream in sync --------- */

/* Baseline: bx was 0x0000 (the decoder ate "BB 22" of the next instruction
 * as immediate bytes). */
static void test_push_imm16_stream_sync(void) {
    machine_t m;
    setup_machine(&m);
    /* mov ax,0x1111; push 0x1234; mov bx,0x2222; hlt */
    const uint8_t program[] = { 0xB8, 0x11, 0x11, 0x68, 0x34, 0x12,
                                0xBB, 0x22, 0x22, 0xF4 };
    run_program(&m, program, sizeof program, 0, 16);
    assert(!m.cpu.fault);
    assert(m.cpu.halted);
    assert((m.cpu.gpr[RBX] & 0xFFFF) == 0x2222);
    assert(mem_read(&m, 0x7FFE, 2) == 0x1234);
    assert((m.cpu.gpr[RSP] & 0xFFFF) == 0x7FFE);
}

/* The pinned SeaBIOS ROM reaches POP r/m with both address-size and
 * operand-size overrides while running in real mode. Include that encoding,
 * plus the special POP [ESP] post-increment destination rule. */
static void test_pop_rm(void) {
    machine_t m;
    setup_machine(&m);
    m.cpu.gpr[RAX] = 0x2000;          /* EAX base for [EAX+0x1c] */
    m.cpu.gpr[RSP] = 0x7000;
    mem_write(&m, 0x7000, 4, 0x89ABCDEFu);
    const uint8_t seabios_form[] = { 0x67,0x66,0x8F,0x40,0x1C,0xF4 };
    run_program(&m, seabios_form, sizeof seabios_form, 0, 8);
    assert(!m.cpu.fault && m.cpu.halted);
    assert(m.cpu.gpr[RSP] == 0x7004);
    assert(mem_read(&m, 0x201C, 4) == 0x89ABCDEFu);

    setup_machine(&m);
    m.cpu.gpr[RSP] = 0x7000;
    mem_write(&m, 0x7000, 4, 0xDEADBEEFu);
    const uint8_t pop_esp_memory[] = { 0x67,0x66,0x8F,0x04,0x24,0xF4 };
    run_program(&m, pop_esp_memory, sizeof pop_esp_memory, 0, 8);
    assert(!m.cpu.fault && m.cpu.halted);
    assert(m.cpu.gpr[RSP] == 0x7004);
    assert(mem_read(&m, 0x7000, 4) == 0xDEADBEEFu);
    assert(mem_read(&m, 0x7004, 4) == 0xDEADBEEFu);

    setup_machine(&m);
    m.cpu.gpr[RSP] = 0x7000;
    mem_write(&m, 0x7000, 2, 0xBEEFu);
    const uint8_t pop_word_esp_memory[] = { 0x67,0x8F,0x04,0x24,0xF4 };
    run_program(&m, pop_word_esp_memory, sizeof pop_word_esp_memory, 0, 8);
    assert(!m.cpu.fault && m.cpu.halted);
    assert(m.cpu.gpr[RSP] == 0x7002);
    assert(mem_read(&m, 0x7002, 2) == 0xBEEFu);

    setup_machine(&m);
    const uint8_t invalid_group[] = { 0x8F,0xC8 };
    memcpy(m.ram, invalid_group, sizeof invalid_group);
    assert(cpu_step(&m.cpu) != 0);
    assert(m.cpu.fault);
}

static void setup_long_mode_stack_test(machine_t *m, uint8_t opcode) {
    setup_machine(m);
    /* Identity-map the first 2 MiB with one large page so the illegal
     * PUSHA/POPA encoding can be fetched in a valid long-mode state. */
    mem_write(m, 0x1000, 8, 0x2003);
    mem_write(m, 0x2000, 8, 0x3003);
    mem_write(m, 0x3000, 8, 0x0083);  /* present, writable, 2-MiB page at 0 */
    m->cpu.cr3 = 0x1000;
    m->cpu.cr4 = 0x20;               /* PAE */
    m->cpu.efer = 0x500;             /* LME | LMA */
    m->cpu.cr0 = 0x80000011u;        /* PG | ET | PE */
    m->cpu.seg[SEG_CS].sel = 0x08;
    m->cpu.seg[SEG_CS].base = 0;
    m->cpu.seg[SEG_CS].limit = 0xFFFFFFFFu;
    m->cpu.seg[SEG_CS].l = 1;
    m->cpu.seg[SEG_CS].d_b = 0;
    m->cpu.seg[SEG_SS].base = 0;
    m->cpu.seg[SEG_SS].limit = 0xFFFFFFFFu;
    m->cpu.rip = 0;
    m->ram[0] = opcode;
    perf_tlb_flush(m);
}

static void test_pusha_popa(void) {
    machine_t m;
    setup_machine(&m);
    m.cpu.gpr[RAX] = 0x1111;
    m.cpu.gpr[RCX] = 0x2222;
    m.cpu.gpr[RDX] = 0x3333;
    m.cpu.gpr[RBX] = 0x4444;
    m.cpu.gpr[RBP] = 0x5555;
    m.cpu.gpr[RSI] = 0x6666;
    m.cpu.gpr[RDI] = 0x7777;
    m.cpu.gpr[RSP] = 0x8000;
    uint64_t flags = m.cpu.rflags;
    const uint8_t pusha16[] = { 0x60, 0xF4 };
    run_program(&m, pusha16, sizeof pusha16, 0, 4);
    assert(!m.cpu.fault && m.cpu.halted);
    assert(m.cpu.gpr[RSP] == 0x7FF0);
    const uint16_t expected16[] = {
        0x7777, 0x6666, 0x5555, 0x8000, 0x4444, 0x3333, 0x2222, 0x1111
    };
    for (size_t i=0; i<sizeof(expected16)/sizeof(expected16[0]); i++)
        assert(mem_read(&m, 0x7FF0 + i*2, 2) == expected16[i]);
    assert(m.cpu.rflags == flags);  /* PUSHA does not change flags */

    setup_machine(&m);
    m.cpu.gpr[RAX] = 0xAABBCCDDEEFF0001ull;
    m.cpu.gpr[RBX] = 0x1122334455660002ull;
    m.cpu.gpr[RCX] = 0x2233445566770003ull;
    m.cpu.gpr[RDX] = 0x3344556677880004ull;
    m.cpu.gpr[RBP] = 0x4455667788990005ull;
    m.cpu.gpr[RSI] = 0x5566778899AA0006ull;
    m.cpu.gpr[RDI] = 0x66778899AABB0007ull;
    m.cpu.gpr[RSP] = 0x7000;
    const uint64_t old_ax = m.cpu.gpr[RAX], old_bx = m.cpu.gpr[RBX];
    const uint64_t old_cx = m.cpu.gpr[RCX], old_dx = m.cpu.gpr[RDX];
    const uint64_t old_bp = m.cpu.gpr[RBP], old_si = m.cpu.gpr[RSI];
    const uint64_t old_di = m.cpu.gpr[RDI];
    const uint16_t popa16_data[] = {
        0x7777, 0x6666, 0x5555, 0xDEAD, 0x4444, 0x3333, 0x2222, 0x1111
    };
    for (size_t i=0; i<sizeof(popa16_data)/sizeof(popa16_data[0]); i++)
        mem_write(&m, 0x7000 + i*2, 2, popa16_data[i]);
    flags = m.cpu.rflags;
    const uint8_t popa16[] = { 0x61, 0xF4 };
    run_program(&m, popa16, sizeof popa16, 0, 4);
    assert(!m.cpu.fault && m.cpu.halted);
    assert(m.cpu.gpr[RSP] == 0x7010); /* saved SP slot was discarded */
    assert((m.cpu.gpr[RDI] & 0xFFFF) == 0x7777 && (m.cpu.gpr[RDI] >> 16) == (old_di >> 16));
    assert((m.cpu.gpr[RSI] & 0xFFFF) == 0x6666 && (m.cpu.gpr[RSI] >> 16) == (old_si >> 16));
    assert((m.cpu.gpr[RBP] & 0xFFFF) == 0x5555 && (m.cpu.gpr[RBP] >> 16) == (old_bp >> 16));
    assert((m.cpu.gpr[RBX] & 0xFFFF) == 0x4444 && (m.cpu.gpr[RBX] >> 16) == (old_bx >> 16));
    assert((m.cpu.gpr[RDX] & 0xFFFF) == 0x3333 && (m.cpu.gpr[RDX] >> 16) == (old_dx >> 16));
    assert((m.cpu.gpr[RCX] & 0xFFFF) == 0x2222 && (m.cpu.gpr[RCX] >> 16) == (old_cx >> 16));
    assert((m.cpu.gpr[RAX] & 0xFFFF) == 0x1111 && (m.cpu.gpr[RAX] >> 16) == (old_ax >> 16));
    assert(m.cpu.rflags == flags);  /* POPA does not change flags */

    setup_machine(&m);
    m.cpu.gpr[RAX] = 0x11111111;
    m.cpu.gpr[RCX] = 0x22222222;
    m.cpu.gpr[RDX] = 0x33333333;
    m.cpu.gpr[RBX] = 0x44444444;
    m.cpu.gpr[RBP] = 0x55555555;
    m.cpu.gpr[RSI] = 0x66666666;
    m.cpu.gpr[RDI] = 0x77777777;
    m.cpu.gpr[RSP] = 0x9000;
    const uint8_t pushad32[] = { 0x66, 0x60, 0xF4 };
    run_program(&m, pushad32, sizeof pushad32, 0, 4);
    assert(!m.cpu.fault && m.cpu.halted);
    assert(m.cpu.gpr[RSP] == 0x8FE0);
    const uint32_t expected32[] = {
        0x77777777, 0x66666666, 0x55555555, 0x00009000,
        0x44444444, 0x33333333, 0x22222222, 0x11111111
    };
    for (size_t i=0; i<sizeof(expected32)/sizeof(expected32[0]); i++)
        assert(mem_read(&m, 0x8FE0 + i*4, 4) == expected32[i]);

    setup_machine(&m);
    m.cpu.gpr[RSP] = 0x7000;
    const uint32_t popad32_data[] = {
        0x77777777, 0x66666666, 0x55555555, 0xDEADBEEF,
        0x44444444, 0x33333333, 0x22222222, 0x11111111
    };
    for (size_t i=0; i<sizeof(popad32_data)/sizeof(popad32_data[0]); i++)
        mem_write(&m, 0x7000 + i*4, 4, popad32_data[i]);
    const uint8_t popad32[] = { 0x66, 0x61, 0xF4 };
    run_program(&m, popad32, sizeof popad32, 0, 4);
    assert(!m.cpu.fault && m.cpu.halted);
    assert(m.cpu.gpr[RSP] == 0x7020);
    assert(m.cpu.gpr[RDI] == 0x77777777 && m.cpu.gpr[RSI] == 0x66666666);
    assert(m.cpu.gpr[RBP] == 0x55555555 && m.cpu.gpr[RBX] == 0x44444444);
    assert(m.cpu.gpr[RDX] == 0x33333333 && m.cpu.gpr[RCX] == 0x22222222);
    assert(m.cpu.gpr[RAX] == 0x11111111);

    setup_long_mode_stack_test(&m, 0x60);
    assert(cpu_step(&m.cpu) != 0 && m.cpu.fault);
    setup_long_mode_stack_test(&m, 0x61);
    assert(cpu_step(&m.cpu) != 0 && m.cpu.fault);
}

static void test_smsw_sgdt(void) {
    machine_t m;
    setup_machine(&m);
    /* smsw ax -> CR0 low 16 = 0x0010 (test machine reset value) */
    const uint8_t prog_smsw[] = { 0x0F, 0x01, 0xE0, 0xF4 };
    run_program(&m, prog_smsw, sizeof prog_smsw, 0, 16);
    assert(!m.cpu.fault);
    assert((m.cpu.gpr[RAX] & 0xFFFF) == 0x0010);

    setup_machine(&m);
    /* pointer at 0x300: limit=0xF, base=0x500 */
    m.ram[0x300]=0x0F; m.ram[0x301]=0; m.ram[0x302]=0x00; m.ram[0x303]=0x05;
    /* lgdt [0x300]; sgdt [0x2000]; hlt */
    const uint8_t prog_sgdt[] = { 0x0F, 0x01, 0x16, 0x00, 0x03,
                                  0x0F, 0x01, 0x06, 0x00, 0x20, 0xF4 };
    mem_write(&m, 0x2006, 2, 0xA55A);  /* the six-byte pseudo-descriptor ends here */
    run_program(&m, prog_sgdt, sizeof prog_sgdt, 0, 16);
    assert(!m.cpu.fault);
    assert(mem_read(&m, 0x2000, 2) == 0x000F);
    assert(mem_read(&m, 0x2002, 4) == 0x500);
    assert(mem_read(&m, 0x2006, 2) == 0xA55A);

    setup_machine(&m);
    m.cpu.gdtr_limit = 0x1234;
    m.cpu.gdtr_base = 0x89ABCDEFu;
    m.cpu.gpr[RSP] = 0x7000;
    mem_write(&m, 0x7008, 2, 0xA55A);
    /* SeaBIOS call32_prep: data32 SGDT [esp+2] in real mode. */
    const uint8_t seabios_sgdt[] = {
        0x67,0x66,0x0F,0x01,0x44,0x24,0x02,0xF4
    };
    run_program(&m, seabios_sgdt, sizeof seabios_sgdt, 0, 16);
    assert(!m.cpu.fault && m.cpu.halted);
    assert(mem_read(&m, 0x7002, 2) == 0x1234);
    assert(mem_read(&m, 0x7004, 4) == 0x89ABCDEFu);
    assert(mem_read(&m, 0x7008, 2) == 0xA55A);
}

/* ---- C5 vectors: PUSHF/POPF, INT/IRET round trip, #DE through the IVT ---- */

static void test_pushf_popf(void) {
    machine_t m;
    setup_machine(&m);
    /* zero -> popf -> pushf -> pop ax: only the architectural bit1 survives */
    const uint8_t prog[] = { 0xB8, 0x00, 0x00, 0x50, 0x9D, 0x9C, 0x58, 0xF4 };
    run_program(&m, prog, sizeof prog, 0, 16);
    assert(!m.cpu.fault);
    assert((m.cpu.gpr[RAX] & 0xFFFF) == 0x2);

    setup_machine(&m);
    /* 0x40 (ZF) survives the round trip */
    const uint8_t prog_zf[] = { 0xB8, 0x40, 0x00, 0x50, 0x9D, 0x9C, 0x58, 0xF4 };
    run_program(&m, prog_zf, sizeof prog_zf, 0, 16);
    assert(!m.cpu.fault);
    assert((m.cpu.gpr[RAX] & 0xFFFF) == 0x42);
    assert((m.cpu.rflags & FLAG_ZF) != 0);
}

static void test_int_iret_roundtrip(void) {
    machine_t m;
    setup_machine(&m);
    /* IVT[0x80] = 0000:0060; handler: mov ax,0xBEEF; iret */
    m.ram[0x200] = 0x60;
    const uint8_t handler[] = { 0xB8, 0xEF, 0xBE, 0xCF };
    memcpy(m.ram + 0x60, handler, sizeof handler);
    /* int 0x80; mov bx,0xCAFE; hlt */
    const uint8_t program[] = { 0xCD, 0x80, 0xBB, 0xFE, 0xCA, 0xF4 };
    run_program(&m, program, sizeof program, 0, 16);
    assert(!m.cpu.fault);
    assert(m.cpu.halted);
    assert((m.cpu.gpr[RAX] & 0xFFFF) == 0xBEEF);  /* handler ran */
    assert((m.cpu.gpr[RBX] & 0xFFFF) == 0xCAFE);  /* resumed after INT */
    assert((m.cpu.gpr[RSP] & 0xFFFF) == 0x8000);  /* frame fully unwound */
}

static void test_divide_error_reaches_ivt(void) {
    machine_t m;
    setup_machine(&m);
    /* IVT[0] = 0000:0040; handler: mov ax,0xDEAD; hlt */
    m.ram[0x00] = 0x40;
    const uint8_t handler[] = { 0xB8, 0xAD, 0xDE, 0xF4 };
    memcpy(m.ram + 0x40, handler, sizeof handler);
    /* mov cl,0; mov ax,1; div cl; hlt (fallback, must not be reached) */
    const uint8_t program[] = { 0xB1, 0x00, 0xB8, 0x01, 0x00, 0xF6, 0xF1, 0xF4 };
    run_program(&m, program, sizeof program, 0x20, 16);
    assert(!m.cpu.fault);              /* baseline halted the *machine* here */
    assert(m.cpu.halted);
    assert((m.cpu.gpr[RAX] & 0xFFFF) == 0xDEAD);
}

/* ---- C7 vectors: string ops with REP/REPE/REPNE semantics ---------------- */

static uint8_t g_outs_capture[8];
static int g_outs_n;
static uint32_t tport_read(void *ctx, uint16_t port, int size) {
    (void)ctx; (void)port; (void)size;
    return 0x5A;
}
static void tport_write(void *ctx, uint16_t port, int size, uint32_t val) {
    (void)ctx; (void)port; (void)size;
    if (g_outs_n < 8) g_outs_capture[g_outs_n++] = (uint8_t)val;
}

/* rep movsb copies forward and leaves count zeroed */
static void test_rep_movsb(void) {
    machine_t m;
    setup_machine(&m);
    memcpy(m.ram + 0x1000, "HELLO", 5);
    /* mov si,0x1000; mov di,0x2000; mov cx,5; cld; rep movsb; hlt */
    const uint8_t program[] = { 0xBE, 0x00, 0x10, 0xBF, 0x00, 0x20,
                                0xB9, 0x05, 0x00, 0xFC, 0xF3, 0xA4, 0xF4 };
    run_program(&m, program, sizeof program, 0, 32);
    assert(!m.cpu.fault);
    assert(memcmp(m.ram + 0x2000, "HELLO", 5) == 0);
    assert((m.cpu.gpr[RSI] & 0xFFFF) == 0x1005);
    assert((m.cpu.gpr[RDI] & 0xFFFF) == 0x2005);
    assert((m.cpu.gpr[RCX] & 0xFFFF) == 0);
}

/* repne scasb stops right after the matching byte, ZF=1 */
static void test_repne_scasb(void) {
    machine_t m;
    setup_machine(&m);
    memcpy(m.ram + 0x1000, "ABCD", 4);
    /* mov al,'C'; mov di,0x1000; mov cx,4; cld; repne scasb; hlt */
    const uint8_t program[] = { 0xB0, 0x43, 0xBF, 0x00, 0x10,
                                0xB9, 0x04, 0x00, 0xFC, 0xF2, 0xAE, 0xF4 };
    run_program(&m, program, sizeof program, 0, 32);
    assert(!m.cpu.fault);
    assert((m.cpu.gpr[RDI] & 0xFFFF) == 0x1003);
    assert((m.cpu.gpr[RCX] & 0xFFFF) == 1);
    assert((m.cpu.rflags & FLAG_ZF) != 0);
}

/* repe cmpsb stops at the first mismatch, count and pointers mid-stride */
static void test_repe_cmpsb(void) {
    machine_t m;
    setup_machine(&m);
    memcpy(m.ram + 0x1000, "QWER", 4);
    memcpy(m.ram + 0x2000, "QWZR", 4);
    /* mov si,0x1000; mov di,0x2000; mov cx,4; cld; repe cmpsb; hlt */
    const uint8_t program[] = { 0xBE, 0x00, 0x10, 0xBF, 0x00, 0x20,
                                0xB9, 0x04, 0x00, 0xFC, 0xF3, 0xA6, 0xF4 };
    run_program(&m, program, sizeof program, 0, 32);
    assert(!m.cpu.fault);
    assert((m.cpu.gpr[RSI] & 0xFFFF) == 0x1003);
    assert((m.cpu.gpr[RDI] & 0xFFFF) == 0x2003);
    assert((m.cpu.gpr[RCX] & 0xFFFF) == 1);
    assert(!(m.cpu.rflags & FLAG_ZF));      /* 'E' != 'Z' */
    assert((m.cpu.rflags & FLAG_CF) != 0);  /* 'E' < 'Z'  */
}

/* rep with a zero count performs NO memory access at all */
static void test_rep_stosb_zero_count(void) {
    machine_t m;
    setup_machine(&m);
    m.ram[0x2000] = 0x7E; /* sentinel */
    /* mov di,0x2000; mov cx,0; mov al,0x55; cld; rep stosb; hlt */
    const uint8_t program[] = { 0xBF, 0x00, 0x20, 0xB9, 0x00, 0x00,
                                0xB0, 0x55, 0xFC, 0xF3, 0xAA, 0xF4 };
    run_program(&m, program, sizeof program, 0, 32);
    assert(!m.cpu.fault);
    assert(m.ram[0x2000] == 0x7E);
    assert((m.cpu.gpr[RCX] & 0xFFFF) == 0);
    assert((m.cpu.gpr[RDI] & 0xFFFF) == 0x2000);
}

/* lodsb honours DF in both directions */
static void test_lodsb_direction(void) {
    machine_t m;
    setup_machine(&m);
    m.ram[0x1000] = 'Z';
    const uint8_t fwd[] = { 0xBE, 0x00, 0x10, 0xFC, 0xAC, 0xF4 }; /* cld; lodsb */
    run_program(&m, fwd, sizeof fwd, 0, 16);
    assert(!m.cpu.fault);
    assert((m.cpu.gpr[RAX] & 0xFF) == 'Z');
    assert((m.cpu.gpr[RSI] & 0xFFFF) == 0x1001);

    setup_machine(&m);
    m.ram[0x1000] = 'Z';
    const uint8_t bwd[] = { 0xBE, 0x00, 0x10, 0xFD, 0xAC, 0xF4 }; /* std; lodsb */
    run_program(&m, bwd, sizeof bwd, 0, 16);
    assert(!m.cpu.fault);
    assert((m.cpu.gpr[RAX] & 0xFF) == 'Z');
    assert((m.cpu.gpr[RSI] & 0xFFFF) == 0x0FFF);
}

/* outsb/insb round trip through a test I/O port; rep outsb streams bytes */
static void test_ins_outs(void) {
    machine_t m;
    setup_machine(&m);
    io_register(&m, 0x99, 1, tport_read, tport_write, NULL, "testport");
    m.ram[0x1000] = 0x77;
    g_outs_n = 0;
    /* mov si,0x1000; mov dx,0x99; cld; outsb; hlt */
    const uint8_t prog_outs[] = { 0xBE, 0x00, 0x10, 0xBA, 0x99, 0x00, 0xFC, 0x6E, 0xF4 };
    run_program(&m, prog_outs, sizeof prog_outs, 0, 16);
    assert(!m.cpu.fault);
    assert(g_outs_n == 1 && g_outs_capture[0] == 0x77);
    assert((m.cpu.gpr[RSI] & 0xFFFF) == 0x1001);

    setup_machine(&m);
    io_register(&m, 0x99, 1, tport_read, tport_write, NULL, "testport");
    /* mov di,0x3000; mov dx,0x99; cld; insb; hlt */
    const uint8_t prog_ins[] = { 0xBF, 0x00, 0x30, 0xBA, 0x99, 0x00, 0xFC, 0x6C, 0xF4 };
    run_program(&m, prog_ins, sizeof prog_ins, 0, 16);
    assert(!m.cpu.fault);
    assert(m.ram[0x3000] == 0x5A);
    assert((m.cpu.gpr[RDI] & 0xFFFF) == 0x3001);

    setup_machine(&m);
    io_register(&m, 0x99, 1, tport_read, tport_write, NULL, "testport");
    memcpy(m.ram + 0x1000, "XYZ", 3);
    g_outs_n = 0;
    /* mov si,0x1000; mov cx,3; mov dx,0x99; cld; rep outsb; hlt */
    const uint8_t prog_repouts[] = { 0xBE, 0x00, 0x10, 0xB9, 0x03, 0x00,
                                     0xBA, 0x99, 0x00, 0xFC, 0xF3, 0x6E, 0xF4 };
    run_program(&m, prog_repouts, sizeof prog_repouts, 0, 32);
    assert(!m.cpu.fault);
    assert(g_outs_n == 3 && memcmp(g_outs_capture, "XYZ", 3) == 0);
    assert((m.cpu.gpr[RCX] & 0xFFFF) == 0);
}

/* ---- C8 vectors: XCHG/CMOVcc/SETcc/MOVSX/CBW/LOOP/BSWAP/XADD/CMPXCHG ------ */

static void test_xchg_forms(void) {
    machine_t m;
    setup_machine(&m);
    /* mov ax,0x1111; mov bx,0x2222; xchg ax,bx (0x87 D8); hlt */
    const uint8_t prog_rm[] = { 0xB8, 0x11, 0x11, 0xBB, 0x22, 0x22, 0x87, 0xD8, 0xF4 };
    run_program(&m, prog_rm, sizeof prog_rm, 0, 16);
    assert(!m.cpu.fault);
    assert((m.cpu.gpr[RAX] & 0xFFFF) == 0x2222);
    assert((m.cpu.gpr[RBX] & 0xFFFF) == 0x1111);

    setup_machine(&m);
    /* mov ax,0x1111; mov bx,0x2222; xchg ax,bx short (0x93); hlt */
    const uint8_t prog_short[] = { 0xB8, 0x11, 0x11, 0xBB, 0x22, 0x22, 0x93, 0xF4 };
    run_program(&m, prog_short, sizeof prog_short, 0, 16);
    assert(!m.cpu.fault);
    assert((m.cpu.gpr[RAX] & 0xFFFF) == 0x2222);
    assert((m.cpu.gpr[RBX] & 0xFFFF) == 0x1111);
}

static void test_cmovcc_setcc(void) {
    machine_t m;
    setup_machine(&m);
    /* mov bx,9; xor ax,ax; mov al,0x7F; add al,1 -> OF=1; cmovo ax,bx; seto cl */
    const uint8_t prog[] = { 0xBB, 0x09, 0x00,             /* mov bx,9 */
                             0x31, 0xC0,                   /* xor ax,ax (dest = 0) */
                             0xB0, 0x7F, 0x04, 0x01,       /* mov al,0x7F; add al,1 -> OF=1 */
                             0x0F, 0x40, 0xC3,             /* cmovo ax,bx */
                             0x0F, 0x90, 0xC1,             /* seto cl */
                             0xF4 };
    run_program(&m, prog, sizeof prog, 0, 16);
    assert(!m.cpu.fault);
    assert((m.cpu.gpr[RAX] & 0xFFFF) == 9);   /* cmovo taken */
    assert((m.cpu.gpr[RCX] & 0xFF) == 1);     /* seto stored 1 */

    setup_machine(&m);
    /* xor ax,ax; test ax,ax -> OF=0: cmovo NOT taken, setno stores 1 */
    const uint8_t prog2[] = { 0xBB, 0x09, 0x00,
                              0x31, 0xC0, 0x84, 0xC0,      /* xor ax,ax; test ax,ax */
                              0x0F, 0x40, 0xC3,            /* cmovo ax,bx (not taken) */
                              0x0F, 0x91, 0xC1,            /* setno cl */
                              0xF4 };
    run_program(&m, prog2, sizeof prog2, 0, 16);
    assert(!m.cpu.fault);
    assert((m.cpu.gpr[RAX] & 0xFFFF) == 0);
    assert((m.cpu.gpr[RCX] & 0xFF) == 1);
}

static void test_movsx_cbw_cwd(void) {
    machine_t m;
    setup_machine(&m);
    /* mov bl,0xFF; movsx ax,bl -> 0xFFFF */
    const uint8_t prog_sx[] = { 0xB3, 0xFF, 0x0F, 0xBE, 0xC3, 0xF4 };
    run_program(&m, prog_sx, sizeof prog_sx, 0, 16);
    assert(!m.cpu.fault);
    assert((m.cpu.gpr[RAX] & 0xFFFF) == 0xFFFF);

    setup_machine(&m);
    /* mov al,0xFF; cbw -> ax=0xFFFF; cwd -> dx=0xFFFF (ax bit15 set) */
    const uint8_t prog_cbw[] = { 0xB0, 0xFF, 0x98, 0x99, 0xF4 };
    run_program(&m, prog_cbw, sizeof prog_cbw, 0, 16);
    assert(!m.cpu.fault);
    assert((m.cpu.gpr[RAX] & 0xFFFF) == 0xFFFF);
    assert((m.cpu.gpr[RDX] & 0xFFFF) == 0xFFFF);
}

static void test_loop_jcxz(void) {
    machine_t m;
    setup_machine(&m);
    /* mov cx,3; inc ax ... loop back over inc until cx==0 -> ax==3 */
    const uint8_t prog_loop[] = { 0xB9, 0x03, 0x00, 0x40, 0xE2, 0xFD, 0xF4 };
    run_program(&m, prog_loop, sizeof prog_loop, 0, 32);
    assert(!m.cpu.fault);
    assert((m.cpu.gpr[RAX] & 0xFFFF) == 3);
    assert((m.cpu.gpr[RCX] & 0xFFFF) == 0);

    setup_machine(&m);
    /* mov cx,0; jcxz +6 -> skips the 0x1111 block to 0x2222 */
    const uint8_t prog_jcxz[] = { 0xB9, 0x00, 0x00, 0xE3, 0x06,
                                  0xB8, 0x11, 0x11, 0xF4, 0x90, 0x90,
                                  0xB8, 0x22, 0x22, 0xF4 };
    run_program(&m, prog_jcxz, sizeof prog_jcxz, 0, 16);
    assert(!m.cpu.fault);
    assert((m.cpu.gpr[RAX] & 0xFFFF) == 0x2222);
}

static void test_bswap_xadd_cmpxchg(void) {
    machine_t m;
    setup_machine(&m);
    /* mov eax,0x12345678; bswap eax -> 0x78563412 (0x66 forms in 16-bit mode) */
    const uint8_t prog_bswap[] = { 0x66, 0xB8, 0x78, 0x56, 0x34, 0x12,
                                   0x66, 0x0F, 0xC8, 0xF4 };
    run_program(&m, prog_bswap, sizeof prog_bswap, 0, 16);
    assert(!m.cpu.fault);
    assert((m.cpu.gpr[RAX] & 0xFFFFFFFF) == 0x78563412);

    setup_machine(&m);
    /* mov ax,5; mov bx,3; xadd ax,bx -> ax=8 (sum), bx=5 (old dst) */
    const uint8_t prog_xadd[] = { 0xB8, 0x05, 0x00, 0xBB, 0x03, 0x00,
                                  0x0F, 0xC1, 0xD8, 0xF4 };
    run_program(&m, prog_xadd, sizeof prog_xadd, 0, 16);
    assert(!m.cpu.fault);
    assert((m.cpu.gpr[RAX] & 0xFFFF) == 8);
    assert((m.cpu.gpr[RBX] & 0xFFFF) == 5);

    setup_machine(&m);
    /* equal: ax=0x2222,bx=0x2222,cx=0x9999; cmpxchg bx,cx -> ZF=1, bx=0x9999 */
    const uint8_t prog_eq[] = { 0xB8, 0x22, 0x22, 0xBB, 0x22, 0x22,
                                0xB9, 0x99, 0x99, 0x0F, 0xB1, 0xCB, 0xF4 };
    run_program(&m, prog_eq, sizeof prog_eq, 0, 16);
    assert(!m.cpu.fault);
    assert((m.cpu.rflags & FLAG_ZF) != 0);
    assert((m.cpu.gpr[RBX] & 0xFFFF) == 0x9999);

    setup_machine(&m);
    /* unequal: ax=0x1111,bx=0x2222,cx=0x9999; cmpxchg bx,cx -> ZF=0, ax=0x2222 */
    const uint8_t prog_ne[] = { 0xB8, 0x11, 0x11, 0xBB, 0x22, 0x22,
                                0xB9, 0x99, 0x99, 0x0F, 0xB1, 0xCB, 0xF4 };
    run_program(&m, prog_ne, sizeof prog_ne, 0, 16);
    assert(!m.cpu.fault);
    assert(!(m.cpu.rflags & FLAG_ZF));
    assert((m.cpu.gpr[RAX] & 0xFFFF) == 0x2222);
    assert((m.cpu.gpr[RBX] & 0xFFFF) == 0x2222);
}

/* ---- C9 vectors: CPUID leaves, RDTSC virtual time, PAUSE/fences ---------- */

static void test_cpuid_leaves(void) {
    machine_t m;
    setup_machine(&m);
    const uint8_t prog[] = {
        0x66, 0xB8, 0x02, 0x00, 0x00, 0x00,       /* mov eax,2 */
        0x0F, 0xA2,                               /* cpuid */
        0x66, 0xA3, 0x00, 0x20,                   /* mov [0x2000],eax */
        0x66, 0x89, 0x1E, 0x04, 0x20,             /* mov [0x2004],ebx */
        0x66, 0xB8, 0x04, 0x00, 0x00, 0x00,       /* mov eax,4 */
        0x66, 0xB9, 0x00, 0x00, 0x00, 0x00,       /* mov ecx,0 (subleaf 0) */
        0x0F, 0xA2,
        0x66, 0xA3, 0x08, 0x20,                   /* mov [0x2008],eax */
        0x66, 0x89, 0x1E, 0x0C, 0x20,             /* mov [0x200C],ebx */
        0x66, 0xB9, 0x09, 0x00, 0x00, 0x00,       /* mov ecx,9 (beyond table) */
        0x0F, 0xA2,
        0x66, 0xA3, 0x10, 0x20,                   /* mov [0x2010],eax */
        0x66, 0xB8, 0x07, 0x00, 0x00, 0x00,       /* mov eax,7 */
        0x66, 0xB9, 0x00, 0x00, 0x00, 0x00,       /* mov ecx,0 */
        0x0F, 0xA2,
        0x66, 0xA3, 0x14, 0x20,                   /* mov [0x2014],eax */
        0x66, 0x89, 0x1E, 0x18, 0x20,             /* mov [0x2018],ebx */
        0x66, 0xB8, 0x02, 0x00, 0x00, 0x80,       /* mov eax,0x80000002 */
        0x0F, 0xA2,
        0x66, 0xA3, 0x1C, 0x20,                   /* mov [0x201C],eax */
        0x66, 0xB8, 0x08, 0x00, 0x00, 0x80,       /* mov eax,0x80000008 */
        0x0F, 0xA2,
        0x66, 0xA3, 0x20, 0x20,                   /* mov [0x2020],eax */
        0x66, 0xB8, 0x01, 0x00, 0x00, 0x00,       /* mov eax,1 */
        0x0F, 0xA2,
        0x66, 0x89, 0x16, 0x24, 0x20,             /* mov [0x2024],edx */
        0x66, 0x89, 0x0E, 0x2C, 0x20,             /* mov [0x202C],ecx */
        0x66, 0xB8, 0x01, 0x00, 0x00, 0x80,       /* mov eax,0x80000001 */
        0x0F, 0xA2,
        0x66, 0x89, 0x16, 0x28, 0x20,             /* mov [0x2028],edx */
        0xF4 };
    run_program(&m, prog, sizeof prog, 0, 64);
    assert(!m.cpu.fault);
    assert(mem_read(&m, 0x2000, 4) == 0x0000FF01u);      /* leaf 2: "use leaf 4" */
    assert(mem_read(&m, 0x2004, 4) == 0);
    uint32_t l4a = (uint32_t)mem_read(&m, 0x2008, 4);
    assert((l4a & 0x1F) == 1);                            /* type = L1 data */
    assert(((l4a >> 5) & 7) == 1);                        /* level 1 */
    assert((mem_read(&m, 0x200C, 4) & 0xFFF) == 63);      /* 64B line - 1 */
    assert(mem_read(&m, 0x2010, 4) == 0);                 /* enum terminates */
    assert(mem_read(&m, 0x2014, 4) == 0);                 /* leaf 7: max subleaf */
    assert(mem_read(&m, 0x2018, 4) == ((1u << 9) | (1u << 7) | (1u << 10) | (1u << 20))); /* ERMS + SMEP + INVPCID (CF3) + SMAP (CF1) */
    assert(mem_read(&m, 0x201C, 4) == 0x61727541u);       /* brand "Aura" */
    assert(mem_read(&m, 0x2020, 4) == 0x00003028u);       /* 48-bit VA, 40-bit PA */
    /* CF2: leaf-1 EDX = FPU|TSC|MSR|CX8|APIC|CMOV|PAT|CLFLUSH|MMX|
     * FXSR|SSE|SSE2 (PAT and CLFLUSH are the CF2 additions, both
     * already true in the core) */
    assert(mem_read(&m, 0x2024, 4) == 0x07898331u);
    /* CF2+CF4: 80000001 EDX = LM|RDTSCP|1GBPAGES|NX|SYSCALL (the
     * first three long-implemented when CF2 published them; RDTSCP
     * was implemented at CF4 -- before that 0F 01 F9 was an
     * accidental INVLPG) */
    assert(mem_read(&m, 0x2028, 4) == 0x2C100800u);
    /* CF3+CF4: leaf-1 ECX = SSE3|PCID|POPCNT|HYPERVISOR (the PCID
     * bit arrived only with the CR4.PCIDE/CR3[63]/INVPCID legality
     * surface; POPCNT's decode arrived at CF4 -- the semantics
     * vectors live in tests/test_tlb.c + test_cf4_units below) */
    assert(mem_read(&m, 0x202C, 4) == 0x80820001u);

    /* brand string is exactly 48 bytes of text padded with spaces */
    const platform_t *p = platform_by_name("haswell");
    char brand[49];
    uint32_t a, b, c2, d2;
    for (uint32_t leaf = 0x80000002; leaf <= 0x80000004; leaf++) {
        platform_cpuid(p, leaf, 0, &a, &b, &c2, &d2);
        memcpy(brand + (leaf - 0x80000002) * 16 + 0, &a, 4);
        memcpy(brand + (leaf - 0x80000002) * 16 + 4, &b, 4);
        memcpy(brand + (leaf - 0x80000002) * 16 + 8, &c2, 4);
        memcpy(brand + (leaf - 0x80000002) * 16 + 12, &d2, 4);
    }
    brand[48] = 0;
    assert(strstr(brand, "AuraLite Virtual CPU") != NULL);
    assert(strstr(brand, "0x3C") != NULL);
    assert(strchr(brand, '\0') == brand + 48);
}

/* CPU_FEATURES CF2 (D2): IA32_PAT. The consumer's gate needs
 * CPUID.1:EDX.PAT AND the architectural reset default before its
 * read-modify-write (PA4 := WC). Both halves pinned: the bit lives
 * in test_cpuid_leaves; here the reset value, the guest's exact
 * RDMSR -> RMW -> WRMSR -> readback sequence, survival across an
 * INIT# (silicon preserves the table), and re-default at RESET.
 * Snapshot survival lives in tests/test_snapshot.c. */
static void test_pat_msr(void) {
    machine_t m;
    setup_machine(&m);
    int found = 0;
    assert(cpu_get_msr(&m.cpu, MSR_IA32_PAT, &found) == MSR_IA32_PAT_RESET && found);
    static const uint8_t prog[] = {
        0x66, 0xB9, 0x77, 0x02, 0x00, 0x00,   /* mov ecx,0x277        */
        0x0F, 0x32,                           /* rdmsr                */
        0x66, 0xA3, 0x00, 0x24,               /* mov [0x2400],eax     */
        0x66, 0x89, 0x16, 0x04, 0x24,         /* mov [0x2404],edx     */
        0x66, 0x81, 0xE2, 0xF8, 0xFF, 0xFF, 0xFF, /* and edx,~7 (PA4) */
        0x66, 0x83, 0xCA, 0x01,               /* or  edx,1  (WC)      */
        0x0F, 0x30,                           /* wrmsr                */
        0x0F, 0x32,                           /* rdmsr                */
        0x66, 0xA3, 0x08, 0x24,               /* mov [0x2408],eax     */
        0x66, 0x89, 0x16, 0x0C, 0x24,         /* mov [0x240C],edx     */
        0xF4 };                               /* hlt                  */
    run_program(&m, prog, sizeof prog, 0, 64);
    assert(!m.cpu.fault);
    assert(mem_read(&m, 0x2400, 4) == 0x00070406u);   /* defaults PA0..PA3 */
    assert(mem_read(&m, 0x2404, 4) == 0x00070406u);   /* defaults PA4..PA7 */
    assert(mem_read(&m, 0x2408, 4) == 0x00070406u);   /* low half untouched*/
    assert(mem_read(&m, 0x240C, 4) == 0x00070401u);   /* PA4 := WC stuck   */
    assert(cpu_get_msr(&m.cpu, MSR_IA32_PAT, &found) == 0x0007040100070406ULL && found);
    cpu_init_reset(&m.cpu);   /* INIT# preserves the MSR table (PAT too) */
    assert(cpu_get_msr(&m.cpu, MSR_IA32_PAT, &found) == 0x0007040100070406ULL && found);
    cpu_reset(&m.cpu);        /* RESET re-seeds the SDM default */
    assert(cpu_get_msr(&m.cpu, MSR_IA32_PAT, &found) == MSR_IA32_PAT_RESET && found);
}

/* CPU_FEATURES CF4 (D6 + the measured NO-GO): POPCNT and RDTSCP
 * graduated from grep verdicts to implementations; the unknown-MSR
 * lane stays permissive BY EVIDENCE -- the sample firmware RDMSRs
 * 0x2FF unconditionally in reset-stage CAR code, so strictness
 * would brick the primary workload -- and that permissiveness is
 * pinned here, including the now-logged 64-slot drop. The CR/EFER
 * #GP surface lives in tests/test_tlb.c::cf4_validate. */
static void test_cf4_units(void) {
    machine_t m;
    setup_machine(&m);
    /* POPCNT: count, flags defined (arith five cleared, ZF tracks
     * a zero result) -- STC before each proves the clears bite. */
    static const uint8_t popc[] = {
        0x66, 0xB9, 0xF0, 0xF0, 0xF0, 0xF0,   /* mov ecx,0xF0F0F0F0 */
        0xF9,                                 /* stc                */
        0xF3, 0x66, 0x0F, 0xB8, 0xC1,         /* popcnt eax,ecx     */
        0x66, 0xA3, 0x00, 0x25,               /* mov [0x2500],eax   */
        0x9C, 0x58, 0xA3, 0x04, 0x25,         /* pushf; pop ax; mov [0x2504],ax */
        0x66, 0x31, 0xC9,                     /* xor ecx,ecx        */
        0xF9, 0xF3, 0x66, 0x0F, 0xB8, 0xC1,   /* stc; popcnt eax,ecx*/
        0x66, 0xA3, 0x08, 0x25,               /* mov [0x2508],eax   */
        0x9C, 0x58, 0xA3, 0x0C, 0x25,         /* flags -> [0x250C]  */
        0xF4 };
    run_program(&m, popc, sizeof popc, 0, 32);
    assert(!m.cpu.fault);
    assert(mem_read(&m, 0x2500, 4) == 16);
    assert((mem_read(&m, 0x2504, 2) & 0x8D5) == 0);          /* CF PF AF ZF SF OF */
    assert(mem_read(&m, 0x2508, 4) == 0);
    assert((mem_read(&m, 0x250C, 2) & 0x8D5) == 0x40);      /* only ZF */
    /* RDTSCP: RDTSC's twin + IA32_TSC_AUX in ECX. */
    setup_machine(&m);   /* run_program leaves the HLT'd cpu halted */
    static const uint8_t tcp[] = {
        0x0F, 0x31,                           /* rdtsc              */
        0x66, 0xA3, 0x10, 0x25,               /* [0x2510]=eax       */
        0x66, 0x89, 0x16, 0x14, 0x25,         /* [0x2514]=edx       */
        0x66, 0xB9, 0x03, 0x01, 0x00, 0xC0,   /* mov ecx,0xC0000103 */
        0x66, 0xB8, 0x01, 0x00, 0xCD, 0xAB,   /* mov eax,0xABCD0001 */
        0x66, 0x31, 0xD2,                     /* xor edx,edx        */
        0x0F, 0x30,                           /* wrmsr (TSC_AUX)    */
        0x0F, 0x01, 0xF9,                     /* rdtscp             */
        0x66, 0xA3, 0x18, 0x25,               /* [0x2518]=eax       */
        0x66, 0x89, 0x16, 0x1C, 0x25,         /* [0x251C]=edx       */
        0x66, 0x89, 0x0E, 0x20, 0x25,         /* [0x2520]=ecx       */
        0xF4 };
    run_program(&m, tcp, sizeof tcp, 0, 32);
    assert(!m.cpu.fault);
    uint64_t t1 = ((uint64_t)mem_read(&m, 0x2514, 4) << 32) | mem_read(&m, 0x2510, 4);
    uint64_t t2 = ((uint64_t)mem_read(&m, 0x251C, 4) << 32) | mem_read(&m, 0x2518, 4);
    assert(mem_read(&m, 0x2520, 4) == 0xABCD0001u);
    assert(t2 >= t1 && t2 - t1 < 1000000);
    /* The NO-GO, pinned: an unstored index reads 0 fault-free (the
     * firmware's reset-stage 0x2FF probe above all), and arbitrary
     * indices are plain fatal-free storage. */
    setup_machine(&m);
    static const uint8_t msrperm[] = {
        0x66, 0xB9, 0xFF, 0x02, 0x00, 0x00,   /* mov ecx,0x2FF      */
        0x0F, 0x32,                           /* rdmsr              */
        0x66, 0xA3, 0x28, 0x25,               /* [0x2528]=eax       */
        0x66, 0x89, 0x16, 0x2C, 0x25,         /* [0x252C]=edx       */
        0x66, 0xB9, 0x34, 0x12, 0x00, 0x00,   /* mov ecx,0x1234     */
        0x66, 0xB8, 0xEF, 0xBE, 0xAD, 0xDE,   /* mov eax,0xDEADBEEF */
        0x66, 0xBA, 0x78, 0x56, 0x34, 0x12,   /* mov edx,0x12345678 */
        0x0F, 0x30,                           /* wrmsr              */
        0x66, 0x31, 0xC0, 0x66, 0x31, 0xD2,   /* clobber eax/edx    */
        0x0F, 0x32,                           /* rdmsr              */
        0x66, 0xA3, 0x30, 0x25,
        0x66, 0x89, 0x16, 0x34, 0x25,
        0xF4 };
    run_program(&m, msrperm, sizeof msrperm, 0, 32);
    assert(!m.cpu.fault);
    assert(mem_read(&m, 0x2528, 4) == 0 && mem_read(&m, 0x252C, 4) == 0);
    assert(mem_read(&m, 0x2530, 4) == 0xDEADBEEFu);
    assert(mem_read(&m, 0x2534, 4) == 0x12345678u);
    /* The sparse table tops out at 64: first-written survives, the
     * far side of the table reads back 0, and every loss is logged
     * (it used to be silent). 80 fresh indices cross any seeding. */
    setup_machine(&m);
    static uint8_t big[8 * 80 + 64];
    int n = 0;
    big[n++] = 0x66; big[n++] = 0xB8; big[n++] = 1; big[n++] = 0; big[n++] = 0; big[n++] = 0; /* eax=1 */
    big[n++] = 0x66; big[n++] = 0x31; big[n++] = 0xD2;                                       /* edx=0 */
    for (int i = 0; i < 80; i++) {
        unsigned idx = 0x9000 + (unsigned)i;
        big[n++] = 0x66; big[n++] = 0xB9;
        big[n++] = (uint8_t)idx; big[n++] = (uint8_t)(idx >> 8); big[n++] = 0; big[n++] = 0;
        big[n++] = 0x0F; big[n++] = 0x30;
    }
    big[n++] = 0x66; big[n++] = 0xB9; big[n++] = 0x00; big[n++] = 0x90; big[n++] = 0; big[n++] = 0;
    big[n++] = 0x0F; big[n++] = 0x32; big[n++] = 0x66; big[n++] = 0xA3; big[n++] = 0x38; big[n++] = 0x25;
    big[n++] = 0x66; big[n++] = 0xB9; big[n++] = 0x4F; big[n++] = 0x90; big[n++] = 0; big[n++] = 0;
    big[n++] = 0x0F; big[n++] = 0x32; big[n++] = 0x66; big[n++] = 0xA3; big[n++] = 0x3C; big[n++] = 0x25;
    big[n++] = 0xF4;
    run_program(&m, big, (size_t)n, 0, 512);
    assert(!m.cpu.fault);
    assert(mem_read(&m, 0x2538, 4) == 1);   /* 0x9000 stored  */
    assert(mem_read(&m, 0x253C, 4) == 0);   /* 0x904F dropped */
    int logged = 0;
    for (int i = 0; i < m.log.count; i++) if (strstr(m.log.lines[i], "dropped")) logged = 1;
    assert(logged);
}

/* SeaBIOS executes WBINVD before setting the PAM shadow read-only. With no
 * modeled data cache, it must be a coherent no-op that still advances RIP. */
static void test_wbinvd_no_cache(void) {
    machine_t m;
    setup_machine(&m);
    m.cpu.cr0 |= 1;                    /* protected mode */
    m.cpu.seg[SEG_CS].sel = 0x08;
    m.cpu.seg[SEG_CS].base = 0;
    m.cpu.seg[SEG_CS].limit = 0xFFFFFFFFu;
    m.cpu.seg[SEG_CS].d_b = 1;         /* 32-bit code, CPL0 */
    const uint8_t prog[] = {
        0xC7,0x05,0x00,0x20,0x00,0x00,0x78,0x56,0x34,0x12, /* mov dword [2000],12345678 */
        0x0F,0x09,                                             /* wbinvd */
        0xA1,0x00,0x20,0x00,0x00,                           /* mov eax,[2000] */
        0xF4
    };
    run_program(&m, prog, sizeof prog, 0, 16);
    assert(!m.cpu.fault && m.cpu.halted);
    assert((uint32_t)m.cpu.gpr[RAX] == 0x12345678u);
    assert(mem_read(&m, 0x2000, 4) == 0x12345678u);
}

static void test_rdtsc_pause_fences(void) {
    machine_t m;
    setup_machine(&m);
    const uint8_t prog[] = {
        0x0F, 0x31,                               /* rdtsc (instr_count == 0) */
        0x66, 0xA3, 0x00, 0x20,                   /* mov [0x2000],eax */
        0x0F, 0x31,                               /* rdtsc: 2 retired */
        0x66, 0xA3, 0x04, 0x20,                   /* mov [0x2004],eax */
        0xF3, 0x90,                               /* pause (nop) */
        0x0F, 0xAE, 0xE8,                         /* lfence */
        0x0F, 0xAE, 0xF0,                         /* mfence */
        0x0F, 0xAE, 0xF8,                         /* sfence */
        0x0F, 0x31,                               /* rdtsc: 8 retired */
        0x66, 0xA3, 0x08, 0x20,                   /* mov [0x2008],eax */
        0xF4 };
    run_program(&m, prog, sizeof prog, 0, 64);
    assert(!m.cpu.fault);
    uint64_t r = m.plat->tsc_per_instr;
    assert(mem_read(&m, 0x2000, 4) == 0);          /* TSC at instr 0 */
    assert((uint64_t)mem_read(&m, 0x2004, 4) == 2 * r);
    assert((uint64_t)mem_read(&m, 0x2008, 4) == 8 * r);

    /* RDX holds the high half; run long enough to stay zero for the check */
    setup_machine(&m);
    const uint8_t prog2[] = { 0x0F, 0x31, 0xF4 };
    run_program(&m, prog2, sizeof prog2, 0, 4);
    assert(!m.cpu.fault);
    assert(m.cpu.gpr[RDX] == 0);
}

int main(void) {
    test_mov_add_hlt();
    test_memory_round_trip();
    test_platform_cpuid_profile();

    test_near_jcc_taken_of_family();
    test_near_jcc_taken_sf_family();
    test_near_jcc_not_taken_pf_family();
    test_rotates_8bit();
    test_shift_count_mask();
    test_shld_shrd();
    test_mul_imul();
    test_div_idiv();
    test_push_imm16_stream_sync();
    test_pop_rm();
    test_pusha_popa();
    test_smsw_sgdt();
    test_pushf_popf();
    test_int_iret_roundtrip();
    test_divide_error_reaches_ivt();

    test_rep_movsb();
    test_repne_scasb();
    test_repe_cmpsb();
    test_rep_stosb_zero_count();
    test_lodsb_direction();
    test_ins_outs();

    test_xchg_forms();
    test_cmovcc_setcc();
    test_movsx_cbw_cwd();
    test_loop_jcxz();
    test_bswap_xadd_cmpxchg();

    test_cpuid_leaves();
    test_pat_msr();
    test_cf4_units();
    test_wbinvd_no_cache();
    test_rdtsc_pause_fences();

    puts("unit tests: ok");
    return 0;
}
