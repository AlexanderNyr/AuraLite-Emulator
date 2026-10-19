/* tests/test_fpu.c -- x87 FPU vector suite (phase F1).
 *
 * The F0 receipts measured the pre-F1 surface: FLD1/FLDZ decoded as FLDCW
 * and wrote AX into the control word (defect D1), FNINIT/FNCLEX were
 * silent no-ops (D2), FNSTSW AX and every data op died #UD (D3), and
 * FXSAVE stored a hardcoded x87 block with MXCSR in the MASK slot (D5,
 * found by this phase against the host CPU). Every vector below pins the
 * F1 contract bit-exactly: 80-bit constants and m80 stores compare
 * against the host-measured patterns, rounding compares against host
 * casts under the matching fenv direction, and the FXSAVE image is the
 * byte-exact host layout (fxprobe2 receipt).
 *
 * Negative control for the F1 dispatch: reverting the D8-DF routing to
 * the old "#UD x87 data op" stubs reddens v_fld_constants,
 * v_fld1_no_fcw_clobber and the FNINIT/FNSTSW vectors (see FPU_PLAN.md).
 */
#include <assert.h>
#include <fenv.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include "machine.h"
#include "harness.h"

static int fails;
#define CHECK(cond, ...) do { \
    if (!(cond)) { fails++; fprintf(stderr, "FAIL %s:%d ", __func__, __LINE__); \
                   fprintf(stderr, __VA_ARGS__); fputc('\n', stderr); } \
} while (0)

/* ---- the host-measured 80-bit patterns (const_probe receipt) ---- */
static const uint8_t K_FLD1 [10] = {0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x80,0xFF,0x3F};
static const uint8_t K_FLDZ [10] = {0,0,0,0,0,0,0,0,0,0};
static const uint8_t K_L2T  [10] = {0xFE,0x8A,0x1B,0xCD,0x4B,0x78,0x9A,0xD4,0x00,0x40};
static const uint8_t K_L2E  [10] = {0xBC,0xF0,0x17,0x5C,0x29,0x3B,0xAA,0xB8,0xFF,0x3F};
static const uint8_t K_PI   [10] = {0x35,0xC2,0x68,0x21,0xA2,0xDA,0x0F,0xC9,0x00,0x40};
static const uint8_t K_LG2  [10] = {0x99,0xF7,0xCF,0xFB,0x84,0x9A,0x20,0x9A,0xFD,0x3F};
static const uint8_t K_LN2  [10] = {0xAC,0x79,0xCF,0xD1,0xF7,0x17,0x72,0xB1,0xFE,0x3F};

static void to80(long double v, uint8_t b[10]) {
    uint8_t t[16]; memset(t, 0, 16); memcpy(t, &v, 16); memcpy(b, t, 10);
}
static int eq80(long double v, const uint8_t want[10]) {
    uint8_t b[10]; to80(v, b);
    return memcmp(b, want, 10) == 0;
}
static void dump80(const char *what, long double v) {
    uint8_t b[10]; to80(v, b);
    fprintf(stderr, "  %s = %02X%02X %02X%02X %02X%02X %02X%02X %02X%02X\n", what,
            b[9],b[8],b[7],b[6],b[5],b[4],b[3],b[2],b[1],b[0]);
}

static machine_t g;

/* host oracle: round a double to float under the given fenv direction.
 * volatile + noinline keep the conversion inside the fesetround window
 * (without -frounding-math the compiler may legally reorder it). */
__attribute__((noinline))
static uint32_t host_r32(double d, int dir) {
    volatile double v = d;
    volatile float f;
    uint32_t b;
    fesetround(dir);
    f = (float)v;
    memcpy(&b, (void *)&f, 4);
    fesetround(FE_TONEAREST);
    return b;
}

/* run a fragment to the trailing HLT; returns 0 normally, -1 on fault */
static int run(const uint8_t *p, size_t n, int max_steps) {
    setup_machine(&g);
    run_program(&g, p, n, 0, max_steps ? max_steps : 200);
    return g.cpu.fault ? -1 : 0;
}
static void w16(uint64_t a, uint16_t v) { mem_write(&g, a, 2, v); }
static void w32(uint64_t a, uint32_t v) { mem_write(&g, a, 4, v); }
static uint16_t r16(uint64_t a) { return (uint16_t)mem_read(&g, a, 2); }
static uint32_t r32(uint64_t a) { return (uint32_t)mem_read(&g, a, 4); }

/* ============ D1 regression: register forms are NOT FLDCW ============ */
static void v_fld1_no_fcw_clobber(void) {
    /* pre-F1, D9 E8 decoded as FLDCW /5 in register form: FCW <- AX=0 */
    static const uint8_t p[] = { 0xB8, 0x00, 0x00,   /* mov ax,0 */
                                 0xD9, 0xE8,         /* FLD1 */
                                 0xF4 };
    CHECK(run(p, sizeof p, 8) == 0, "fld1 must not fault");
    CHECK(g.cpu.fcw == 0x037F, "FCW=%04x (clobbered by FLD1)", g.cpu.fcw);
    CHECK(eq80(g.cpu.fst[g.cpu.ftop], K_FLD1), "ST0 != 1.0");
    if (!eq80(g.cpu.fst[g.cpu.ftop], K_FLD1)) dump80("ST0", g.cpu.fst[g.cpu.ftop]);
    CHECK(g.cpu.ftop == 7 && g.cpu.fsw == 0x3800, "FSW=%04x ftop=%d", g.cpu.fsw, g.cpu.ftop);
    CHECK(((g.cpu.ftw >> 14) & 3u) == 0, "tag of phys7=%d", (g.cpu.ftw >> 14) & 3);

    static const uint8_t pz[] = { 0xB8, 0x34, 0x12, 0xD9, 0xEE, 0xF4 }; /* FLDZ */
    CHECK(run(pz, sizeof pz, 8) == 0, "fldz must not fault");
    CHECK(g.cpu.fcw == 0x037F, "FCW=%04x (clobbered by FLDZ)", g.cpu.fcw);
    CHECK(eq80(g.cpu.fst[g.cpu.ftop], K_FLDZ), "ST0 != 0");
    CHECK(((g.cpu.ftw >> 14) & 3u) == 1, "zero tag expected, got %d",
          (g.cpu.ftw >> 14) & 3);
}

/* ============ constants are bit-exact ============ */
static void v_fld_constants(void) {
    static const struct { uint8_t op1; const uint8_t *bits; const char *n; } t[] = {
        {0xE8, K_FLD1, "1"},   {0xE9, K_L2T, "l2t"}, {0xEA, K_L2E, "l2e"},
        {0xEB, K_PI,  "pi"},   {0xEC, K_LG2, "lg2"}, {0xED, K_LN2, "ln2"},
        {0xEE, K_FLDZ, "0"},
    };
    for (unsigned i = 0; i < sizeof t / sizeof t[0]; i++) {
        uint8_t p[] = { 0xD9, t[i].op1, 0xF4 };
        CHECK(run(p, sizeof p, 8) == 0, "%s faults", t[i].n);
        if (!eq80(g.cpu.fst[g.cpu.ftop], t[i].bits)) {
            dump80(t[i].n, g.cpu.fst[g.cpu.ftop]);
            CHECK(0, "constant %s bit pattern", t[i].n);
        }
    }
}

/* ============ m32/m64/m80 loads and stores round-trip ============ */
static void v_fld_fstp_mem(void) {
    /* FLD m32 / FSTP m32 through [bx] */
    static const uint8_t p32[] = {
        0xBB, 0x00, 0x30,             /* mov bx,0x3000 */
        0xD9, 0x07,                   /* fld dword [bx] */
        0xD9, 0x1F,                   /* fstp dword [bx] */
        0xF4 };
    setup_machine(&g);
    w32(0x3000, 0x3FC00000u);   /* 1.5f */
    run_program(&g, p32, sizeof p32, 0, 12);
    CHECK(!g.cpu.fault, "m32 roundtrip faults");
    CHECK(r32(0x3000) == 0x3FC00000u, "m32 bits=%08x", r32(0x3000));  /* 1.5 */
    CHECK(g.cpu.ftop == 0, "stack must be empty again");

    /* m80 preserves NaN payloads bit-exactly (no quieting) */
    static const uint8_t nan80[10] = {0x01,0x02,0x03,0x04,0x05,0x06,0x07,0x80,0xFF,0x7F};
    static const uint8_t p80[] = {
        0xBB, 0x00, 0x30,
        0xDB, 0x2F,                   /* fld tword [bx] */
        0xBB, 0x10, 0x30,
        0xDB, 0x3F,                   /* fstp tword [bx] */
        0xF4 };
    setup_machine(&g);
    for (int i = 0; i < 10; i++) mem_write(&g, 0x3000 + i, 1, nan80[i]);
    run_program(&g, p80, sizeof p80, 0, 16);
    CHECK(!g.cpu.fault, "m80 NaN roundtrip faults");
    for (int i = 0; i < 10; i++)
        CHECK((uint8_t)mem_read(&g, 0x3010 + i, 1) == nan80[i], "m80 byte %d", i);

    /* m64 */
    static const uint8_t p64[] = {
        0xBB, 0x00, 0x30,
        0xDD, 0x07,                   /* fld qword [bx] */
        0xDD, 0x1F,                   /* fstp qword [bx] */
        0xF4 };
    setup_machine(&g);
    mem_write(&g, 0x3000, 8, 0x4000000000000000ull);  /* 2.0 */
    run_program(&g, p64, sizeof p64, 0, 16);
    CHECK(!g.cpu.fault && mem_read(&g, 0x3000, 8) == 0x4000000000000000ull,
          "m64 roundtrip");
}

/* ============ FCW.RC drives architected rounding (host-matched) ============ */
static void v_rounding_rc(void) {
    volatile long double third = 1.0L / 3.0L;
    double dthird = (double)third;
    uint32_t b_near  = host_r32(dthird, FE_TONEAREST);
    uint32_t b_trunc = host_r32(dthird, FE_TOWARDZERO);
    uint32_t b_up    = host_r32(dthird, FE_UPWARD);
    /* double 1/3 sits past the float halfway point in the down direction:
     * nearest rounds UP and matches RC=up; truncation is the odd one out. */
    CHECK(b_up != b_trunc, "host oracle degenerate (up==trunc)");
    CHECK(b_near == b_up, "double 1/3 should round up to nearest");
    uint64_t d64; memcpy(&d64, &dthird, 8);

    /* RC=trunc (FCW 0x0F7F): FSTP m32 of a DOUBLE 1/3 (not a float!) */
    static const uint8_t p[] = {
        0xBB, 0x00, 0x30,
        0xDD, 0x07,                   /* fld qword [bx] = 1/3 (double) */
        0xBB, 0x10, 0x30,
        0xD9, 0x2F,                   /* fldcw [bx] (RC=trunc) */
        0xBB, 0x00, 0x30,
        0xD9, 0x1F,                   /* fstp dword [bx] */
        0xF4 };
    setup_machine(&g);
    mem_write(&g, 0x3000, 8, d64);
    w16(0x3010, 0x0F7F);
    run_program(&g, p, sizeof p, 0, 16);
    CHECK(!g.cpu.fault, "rc=trunc faults");
    CHECK(r32(0x3000) == b_trunc, "trunc store %08x want %08x", r32(0x3000), b_trunc);
    CHECK((g.cpu.fsw & 0x20u) == 0x20u, "PE (bit5) must be set after inexact store");

    /* RC=up (FCW 0x0B7F) */
    static const uint8_t pu[] = {
        0xBB, 0x00, 0x30, 0xDD, 0x07,
        0xBB, 0x10, 0x30, 0xD9, 0x2F,
        0xBB, 0x00, 0x30, 0xD9, 0x1F,
        0xF4 };
    setup_machine(&g);
    mem_write(&g, 0x3000, 8, d64);
    w16(0x3010, 0x0B7F);
    run_program(&g, pu, sizeof pu, 0, 16);
    CHECK(r32(0x3000) == b_up, "up store %08x want %08x", r32(0x3000), b_up);
    CHECK(b_up != b_trunc, "RC must be visible in the store");

    /* RC=nearest default */
    static const uint8_t pn[] = {
        0xBB, 0x00, 0x30, 0xDD, 0x07, 0xD9, 0x1F, 0xF4 };
    setup_machine(&g);
    mem_write(&g, 0x3000, 8, d64);
    run_program(&g, pn, sizeof pn, 0, 16);
    CHECK(r32(0x3000) == b_near, "near store %08x want %08x", r32(0x3000), b_near);
}

/* ============ FNSTCW/FLDCW/FNSTSW AX ============ */
static void v_cw_sw(void) {
    static const uint8_t p[] = {
        0xBB, 0x00, 0x30,
        0xD9, 0x3F,                   /* fnstcw [bx] */
        0xD9, 0xE8,                   /* fld1 -> TOP=7 */
        0xDF, 0xE0,                   /* fnstsw ax */
        0xF4 };
    CHECK(run(p, sizeof p, 8) == 0, "cw/sw faults");
    CHECK(r16(0x3000) == 0x037F, "fnstcw=%04x", r16(0x3000));
    CHECK((g.cpu.gpr[RAX] & 0xFFFF) == 0x3800, "fnstsw ax=%04x", (unsigned)(g.cpu.gpr[RAX] & 0xFFFF));
}

/* ============ FNINIT resets the whole FPU ============ */
static void v_fninit(void) {
    static const uint8_t p[] = {
        0xD9, 0xE8,                   /* fld1 */
        0xD9, 0xE8,                   /* fld1 again */
        0xD9, 0xE4,                   /* ftst -> C3 */
        0xDB, 0xE3,                   /* fninit */
        0xF4 };
    CHECK(run(p, sizeof p, 16) == 0, "fninit faults");
    CHECK(g.cpu.fcw == 0x037F, "fcw=%04x", g.cpu.fcw);
    CHECK(g.cpu.fsw == 0, "fsw=%04x", g.cpu.fsw);
    CHECK(g.cpu.ftw == 0xFFFF, "ftw=%04x", g.cpu.ftw);
    CHECK(g.cpu.ftop == 0, "ftop=%d", g.cpu.ftop);
    CHECK(g.cpu.fip == 0 && g.cpu.fdp == 0 && g.cpu.fop == 0, "tracked pointers");
}

/* ============ FNCLEX clears exc/SF/ES, keeps C flags and TOP ============ */
static void v_fnclex(void) {
    /* empty-stack FSTP sets IE|SF with C1=0, TOP=0 */
    static const uint8_t q[] = {
        0xBB, 0x00, 0x30,
        0xD9, 0x1F,                   /* fstp dword [bx] on EMPTY stack */
        0xDB, 0xE2,                   /* fnclex */
        0xF4 };
    CHECK(run(q, sizeof q, 8) == 0, "masked underflow must not #MF");
    CHECK((g.cpu.fsw & 0xFFu) == 0, "IE|SF sticky, fsw=%04x", g.cpu.fsw);
    CHECK(!(g.cpu.fsw & 0x0200u), "C1 must be 0 after underflow");
}

/* ============ FXCH/FFREE/FINCSTP/FDECSTP/ST(i) forms ============ */
static void v_stack_ops(void) {
    static const uint8_t p[] = {
        0xD9, 0xE8,                   /* fld1        ST0=1 */
        0xD9, 0xEE,                   /* fldz        ST0=0, ST1=1 */
        0xD9, 0xC9,                   /* fxch st(1)  ST0=1, ST1=0 */
        0xD9, 0xF6,                   /* fincstp     TOP wraps, tags kept */
        0xD9, 0xF7,                   /* fdecstp */
        0xF4 };
    CHECK(run(p, sizeof p, 16) == 0, "stack ops fault");
    CHECK(eq80(g.cpu.fst[g.cpu.ftop], K_FLD1), "ST0 must be 1.0 after fxch");
    CHECK(eq80(g.cpu.fst[(g.cpu.ftop + 1) & 7], K_FLDZ), "ST1 must be 0.0");
    CHECK(g.cpu.ftop == 6, "ftop=%d", g.cpu.ftop);

    static const uint8_t q[] = {
        0xD9, 0xE8, 0xD9, 0xE8,       /* two values */
        0xDD, 0xC1,                   /* ffree st(1) */
        0xF4 };
    CHECK(run(q, sizeof q, 12) == 0, "ffree faults");
    CHECK(((g.cpu.ftw >> (((g.cpu.ftop + 1) & 7) * 2)) & 3) == 3, "st(1) not empty");

    static const uint8_t s[] = {
        0xD9, 0xE8, 0xD9, 0xEE,
        0xDD, 0xD9,                   /* fstp st(1): store into st1, pop */
        0xF4 };
    CHECK(run(s, sizeof s, 12) == 0, "fstp st(i) faults");
    CHECK(g.cpu.ftop == 7 && eq80(g.cpu.fst[7], K_FLDZ), "stored value in phys7, popped");
    CHECK(((g.cpu.ftw >> (6 * 2)) & 3) == 3, "old ST0 (phys6) must be empty now");
}

/* ============ stack overflow/underflow flags (masked => no #MF) ====== */
static void v_stack_faults(void) {
    uint8_t p[32]; int n = 0;
    for (int i = 0; i < 9; i++) { p[n++] = 0xD9; p[n++] = 0xE8; } /* fld1 x9 */
    p[n++] = 0xF4;
    CHECK(run(p, n, 32) == 0, "overflow must stay masked");
    CHECK((g.cpu.fsw & 0x241u) == 0x241u, "IE|SF|C1 expected, fsw=%04x", g.cpu.fsw);

    static const uint8_t q[] = {
        0xBB, 0x00, 0x30,
        0xD9, 0x1F,                   /* fstp on empty */
        0xF4 };
    CHECK(run(q, sizeof q, 8) == 0, "underflow must stay masked");
    CHECK((g.cpu.fsw & 0x241u) == 0x41u, "IE|SF with C1=0, fsw=%04x", g.cpu.fsw);
}

/* ============ FXAM / FTST / FCHS / FABS ============ */
static void v_status_ops(void) {
    struct { uint8_t b[10]; unsigned c3, c2, c0; const char *n; } t[] = {
        {{0,0,0,0,0,0,0,0,0,0},                    1, 0, 0, "zero"},
        {{0,0,0,0,0,0,0,0x80,0xFF,0x3F},           0, 1, 0, "normal"},
        {{0,0,0,0,0,0,0,0x80,0xFF,0x7F},           1, 1, 0, "inf"},
        {{1,0,0,0,0,0,0,0x80,0xFF,0x7F},           0, 1, 1, "nan"},
        {{1,0,0,0,0,0,0,0x00,0x00,0x00},           0, 0, 0, "denorm"},
    };
    for (unsigned i = 0; i < sizeof t / sizeof t[0]; i++) {
        uint8_t p[] = {
            0xBB, 0x00, 0x30,
            0xDB, 0x2F,               /* fld tword [bx] */
            0xD9, 0xE5,               /* fxam */
            0xF4 };
        setup_machine(&g);
        for (int j = 0; j < 10; j++) mem_write(&g, 0x3000 + j, 1, t[i].b[j]);
        run_program(&g, p, sizeof p, 0, 12);
        CHECK(!g.cpu.fault, "fxam %s faults", t[i].n);
        unsigned c0 = (g.cpu.fsw >> 8) & 1, c2 = (g.cpu.fsw >> 10) & 1, c3 = (g.cpu.fsw >> 14) & 1;
        CHECK(c0 == t[i].c0 && c2 == t[i].c2 && c3 == t[i].c3,
              "fxam %s: C0=%u C2=%u C3=%u want %u %u %u", t[i].n, c0, c2, c3,
              t[i].c0, t[i].c2, t[i].c3);
    }
    /* empty: C3=C2=C0=1, no exception */
    static const uint8_t pe[] = { 0xD9, 0xE5, 0xF4 };
    CHECK(run(pe, sizeof pe, 8) == 0, "fxam empty faults");
    CHECK((g.cpu.fsw & 0x4500u) == 0x4500u, "empty code, fsw=%04x", g.cpu.fsw);

    /* FTST: 0 -> C3, 1 -> clear, -1 -> C0 */
    static const uint8_t p0[] = { 0xD9, 0xEE, 0xD9, 0xE4, 0xF4 };
    CHECK(run(p0, sizeof p0, 8) == 0, "ftst 0");
    CHECK((g.cpu.fsw & 0x4500u) == 0x4000u, "ftst(0) C3, fsw=%04x", g.cpu.fsw);
    static const uint8_t pm[] = { 0xD9, 0xE8, 0xD9, 0xE0, 0xD9, 0xE4, 0xF4 }; /* 1; fchs; ftst */
    CHECK(run(pm, sizeof pm, 8) == 0, "ftst -1");
    CHECK((g.cpu.fsw & 0x4500u) == 0x0100u, "ftst(-1) C0, fsw=%04x", g.cpu.fsw);

    /* FABS/FCHS round-trip the sign */
    static const uint8_t ps[] = { 0xD9, 0xE8, 0xD9, 0xE0, 0xD9, 0xE1, 0xF4 }; /* 1;fchs;fabs */
    CHECK(run(ps, sizeof ps, 8) == 0, "fchs/fabs");
    CHECK(eq80(g.cpu.fst[g.cpu.ftop], K_FLD1), "sign round-trip");
}

/* ============ FILD/FIST/FISTTP integer conversions ============ */
static void v_int_conv(void) {
    /* FILD m32 -7 -> FISTP m32 exact (no PE) */
    static const uint8_t p[] = {
        0xBB, 0x00, 0x30,
        0xDB, 0x07,                   /* fild dword [bx] */
        0xDB, 0x1F,                   /* fistp dword [bx] */
        0xF4 };
    setup_machine(&g);
    w32(0x3000, (uint32_t)-7);
    run_program(&g, p, sizeof p, 0, 12);
    CHECK(!g.cpu.fault, "fild/fistp faults");
    CHECK((int32_t)r32(0x3000) == -7, "got %d", (int32_t)r32(0x3000));
    CHECK((g.cpu.fsw & 0x20u) == 0, "exact store must not set PE");

    /* 2.75: FISTP with RC=trunc -> 2 + PE; RC=up -> 3 + PE */
    static const uint8_t q[] = {
        0xBB, 0x00, 0x30,
        0xD9, 0x07,                   /* fld dword [bx] = 2.75 */
        0xBB, 0x10, 0x30,
        0xD9, 0x2F,                   /* fldcw RC=trunc (0x0F7F) */
        0xBB, 0x00, 0x30,
        0xDB, 0x1F,                   /* fistp dword [bx] */
        0xF4 };
    setup_machine(&g);
    w32(0x3000, 0x40300000u);   /* 2.75f */
    w16(0x3010, 0x0F7F);
    run_program(&g, q, sizeof q, 0, 16);
    CHECK((int32_t)r32(0x3000) == 2 && (g.cpu.fsw & 0x20u), "trunc: %d fsw=%04x",
          (int32_t)r32(0x3000), g.cpu.fsw);

    static const uint8_t qu[] = {
        0xBB, 0x00, 0x30, 0xD9, 0x07,
        0xBB, 0x10, 0x30, 0xD9, 0x2F,  /* fldcw RC=up (0x0B7F) */
        0xBB, 0x00, 0x30, 0xDB, 0x1F,
        0xF4 };
    setup_machine(&g);
    mem_write(&g, 0x3000, 4, 0x40300000u);
    w16(0x3010, 0x0B7F);
    run_program(&g, qu, sizeof qu, 0, 16);
    CHECK((int32_t)r32(0x3000) == 3, "up: %d", (int32_t)r32(0x3000));

    /* FISTTP ignores RC: RC=up still truncates to 2 */
    static const uint8_t qt[] = {
        0xBB, 0x00, 0x30, 0xD9, 0x07,
        0xBB, 0x10, 0x30, 0xD9, 0x2F,
        0xBB, 0x00, 0x30, 0xDB, 0x0F,  /* fisttp dword [bx] */
        0xF4 };
    setup_machine(&g);
    mem_write(&g, 0x3000, 4, 0x40300000u);
    w16(0x3010, 0x0B7F);
    run_program(&g, qt, sizeof qt, 0, 16);
    CHECK((int32_t)r32(0x3000) == 2, "fisttp must truncate: %d", (int32_t)r32(0x3000));

    /* m16 and m64 lanes */
    static const uint8_t q16[] = {
        0xBB, 0x00, 0x30,
        0xDF, 0x07,                   /* fild word [bx] */
        0xDF, 0x1F,                   /* fistp word [bx] */
        0xF4 };
    setup_machine(&g);
    w16(0x3000, (uint16_t)-300);
    run_program(&g, q16, sizeof q16, 0, 12);
    CHECK((int16_t)r16(0x3000) == -300, "m16: %d", (int16_t)r16(0x3000));

    static const uint8_t q64[] = {
        0xBB, 0x00, 0x30,
        0xDF, 0x2F,                   /* fild qword [bx] */
        0xDF, 0x3F,                   /* fistp qword [bx] */
        0xF4 };
    setup_machine(&g);
    mem_write(&g, 0x3000, 8, (uint64_t)(int64_t)-9000000000ll);
    run_program(&g, q64, sizeof q64, 0, 12);
    CHECK((int64_t)mem_read(&g, 0x3000, 8) == -9000000000ll, "m64");
}

/* ============ packed BCD: FBLD/FBSTP ============ */
static void v_bcd(void) {
    /* 1234.0: bytes 34 12 00 .. 00 (sign=0) */
    static const uint8_t p[] = {
        0xBB, 0x00, 0x30,
        0xDF, 0x27,                   /* fbld tword [bx] */
        0xBB, 0x10, 0x30,
        0xDF, 0x37,                   /* fbstp tword [bx] */
        0xF4 };
    setup_machine(&g);
    mem_write(&g, 0x3000, 1, 0x34);
    mem_write(&g, 0x3001, 1, 0x12);
    run_program(&g, p, sizeof p, 0, 16);
    CHECK(!g.cpu.fault, "bcd faults");
    CHECK((uint8_t)mem_read(&g, 0x3010, 1) == 0x34 && (uint8_t)mem_read(&g, 0x3011, 1) == 0x12,
          "bcd digits %02x %02x", (uint8_t)mem_read(&g, 0x3010, 1), (uint8_t)mem_read(&g, 0x3011, 1));
    CHECK((uint8_t)mem_read(&g, 0x3019, 1) == 0x00, "sign byte");
    CHECK(g.cpu.ftop == 0, "fbstp must pop");
}

/* ============ FCMOVcc ============ */
static void v_fcmov(void) {
    static const uint8_t p[] = {
        0xD9, 0xEE,                   /* fldz   ST0=0, ST1=1 */
        0xD9, 0xE8,                   /* fld1 */
        0xF9,                         /* stc */
        0xDA, 0xC1,                   /* fcmovb st(0),st(1) -> ST0=0 */
        0xF4 };
    CHECK(run(p, sizeof p, 12) == 0, "fcmovb faults");
    CHECK(eq80(g.cpu.fst[g.cpu.ftop], K_FLDZ), "fcmovb must move");

    static const uint8_t q[] = {
        0xD9, 0xEE, 0xD9, 0xE8,
        0xF8,                         /* clc */
        0xDA, 0xC1,                   /* fcmovb with CF=0: no move */
        0xF4 };
    CHECK(run(q, sizeof q, 12) == 0, "fcmovb-neg");
    CHECK(eq80(g.cpu.fst[g.cpu.ftop], K_FLD1), "fcmovb must NOT move");

    static const uint8_t s[] = {
        0xD9, 0xEE, 0xD9, 0xE8,
        0xDB, 0xC9,                   /* fcmovne st(0),st(1); ZF=0 -> moves */
        0xF4 };
    CHECK(run(s, sizeof s, 12) == 0, "fcmovne");
    CHECK(eq80(g.cpu.fst[g.cpu.ftop], K_FLDZ), "fcmovne must move");
}

/* ============ CR0.TS -> #NM(7) delivered through the IVT ============ */
static void ivt_set(int vec, uint16_t off, uint16_t cs) {
    w16((uint64_t)vec * 4, off);
    w16((uint64_t)vec * 4 + 2, cs);
}
static void v_nm_trap(void) {
    static const uint8_t p[] = { 0xD9, 0xE8, 0xF4 };       /* fld1 */
    static const uint8_t handler[] = { 0xB8, 0x34, 0x12, 0xF4 }; /* mov ax,0x1234; hlt */

    setup_machine(&g);
    memcpy(g.ram + 0x2000, handler, sizeof handler);
    ivt_set(7, 0x2000, 0);
    g.cpu.cr0 |= 0x8u;                                       /* CR0.TS */
    run_program(&g, p, sizeof p, 0, 16);
    CHECK((g.cpu.gpr[RAX] & 0xFFFF) == 0x1234, "#NM handler did not run");
    CHECK(g.cpu.fcw == 0x037F, "escape must not execute under TS");
    /* fault semantics: the pushed frame carries the faulting IP */
    CHECK((g.cpu.gpr[RSP] & 0xFFFF) == 0x8000 - 6, "frame size");
    CHECK(r16(0x8000 - 6) == 0, "frame IP must point at the escape");

    /* WAIT (9B) under TS raises #NM too */
    static const uint8_t pw[] = { 0x9B, 0xF4 };
    setup_machine(&g);
    memcpy(g.ram + 0x2000, handler, sizeof handler);
    ivt_set(7, 0x2000, 0);
    g.cpu.cr0 |= 0x8u;
    run_program(&g, pw, sizeof pw, 0, 16);
    CHECK((g.cpu.gpr[RAX] & 0xFFFF) == 0x1234, "WAIT #NM handler did not run");
}

/* ============ CR0.EM -> #UD (the project's hard-stop convention) ===== */
static void v_em_ud(void) {
    static const uint8_t p[] = { 0xD9, 0xE8, 0xF4 };
    setup_machine(&g);
    g.cpu.cr0 |= 0x4u;
    run_program(&g, p, sizeof p, 0, 8);
    CHECK(g.cpu.fault != 0, "EM=1 must #UD");
}

/* ============ unmasked IE -> #MF(16) delivered; masked stays quiet === */
static void v_mf_trap(void) {
    static const uint8_t handler[] = { 0xB8, 0x77, 0x55, 0xF4 };
    static const uint8_t p[] = {
        0xBB, 0x00, 0x30,
        0xD9, 0x2F,                   /* fldcw [bx]=0x037E (IM cleared) */
        0xD9, 0x1F,                   /* fstp dword [bx] on EMPTY stack */
        0xF4 };
    setup_machine(&g);
    memcpy(g.ram + 0x2800, handler, sizeof handler);
    ivt_set(16, 0x2800, 0);
    w16(0x3000, 0x037E);   /* clear IM (bit0): the D6 map -- IE is bit0 */
    run_program(&g, p, sizeof p, 0, 16);
    CHECK((g.cpu.gpr[RAX] & 0xFFFF) == 0x5577, "#MF handler did not run");
    CHECK((g.cpu.gpr[RSP] & 0xFFFF) == 0x8000 - 6, "frame");
    CHECK(r16(0x8000 - 6) == 5, "frame IP=%d must be the FSTP at offset 5",
          r16(0x8000 - 6));
}

/* ============ FXSAVE image = host layout (fxprobe2), FXRSTOR live ==== */
static void v_fxsave_live_layout(void) {
    /* 16-bit addressing: mod=0 rm=6 carries the disp16 directly */
    static const uint8_t q[] = {
        0xD9, 0xE8, 0xD9, 0xEB,
        0x0F, 0xAE, 0x06, 0x00, 0x40, /* fxsave [0x4000] */
        0xDB, 0xE3,                   /* fninit (clobbers) */
        0x0F, 0xAE, 0x0E, 0x00, 0x40, /* fxrstor [0x4000] */
        0xF4 };
    CHECK(run(q, sizeof q, 20) == 0, "fxsave/fxrstor faults");
    CHECK(r16(0x4000) == 0x037F, "FCW@0=%04x", r16(0x4000));
    CHECK(r16(0x4002) == 0x3000, "FSW@2=%04x (TOP=6)", r16(0x4002));
    CHECK((uint8_t)r32(0x4004) == 0xC0, "FTW@4=%02x (phys6+7)", (uint8_t)r32(0x4004));
    CHECK(r32(0x4018) == 0x1F80, "MXCSR@24=%08x", r32(0x4018));
    CHECK(r32(0x401C) == 0x0000FFBF, "MASK@28=%08x", r32(0x401C));
    uint8_t st0[10], st1[10];
    for (int i = 0; i < 10; i++) {
        st0[i] = (uint8_t)mem_read(&g, 0x4020 + i, 1);       /* ST0 = pi at 32 */
        st1[i] = (uint8_t)mem_read(&g, 0x4030 + i, 1);       /* ST1 = 1 at 48 */
    }
    CHECK(memcmp(st0, K_PI, 10) == 0, "ST0@32 must be pi");
    CHECK(memcmp(st1, K_FLD1, 10) == 0, "ST1@48 must be 1.0");
    /* and FXRSTOR actually restored it */
    CHECK(g.cpu.ftop == 6, "restored ftop=%d", g.cpu.ftop);
    CHECK(eq80(g.cpu.fst[6], K_PI), "restored ST0");
    CHECK(eq80(g.cpu.fst[7], K_FLD1), "restored ST1");
}

/* ============ FNSAVE/FRSTOR: env + logical regs, save resets ======== */
static void v_fnsave_frstor(void) {
    static const uint8_t p[] = {
        0xD9, 0xE8,                   /* fld1 */
        0xBB, 0x00, 0x30,
        0xDD, 0x37,                   /* fnsave [bx] (94 bytes) */
        0xF4 };
    CHECK(run(p, sizeof p, 12) == 0, "fnsave faults");
    CHECK(r16(0x3000) == 0x037F && r16(0x3002) == 0x3800 && r16(0x3004) == 0x3FFF,
          "env words %04x %04x %04x", r16(0x3000), r16(0x3002), r16(0x3004));
    uint8_t reg0[10];
    for (int i = 0; i < 10; i++) reg0[i] = (uint8_t)mem_read(&g, 0x300E + i, 1);
    CHECK(memcmp(reg0, K_FLD1, 10) == 0, "logical ST0 at +14 must be 1.0");
    /* FNSAVE leaves the FPU initialized */
    CHECK(g.cpu.fcw == 0x037F && g.cpu.fsw == 0 && g.cpu.ftw == 0xFFFF, "post-save reset");

    /* FRSTOR brings the saved state back */
    static const uint8_t q[] = {
        0xD9, 0xEE,                   /* fldz (clobbers) */
        0xBB, 0x00, 0x30,
        0xDD, 0x27,                   /* frstor [bx] */
        0xF4 };
    setup_machine(&g);
    /* build the 94-byte image by hand: TOP=7, one register = 1.0 */
    w16(0x3000, 0x037F); w16(0x3002, 0x3800); w16(0x3004, 0x3FFF);
    for (int i = 0; i < 10; i++) mem_write(&g, 0x300E + i, 1, K_FLD1[i]);
    run_program(&g, q, sizeof q, 0, 12);
    CHECK(g.cpu.fsw == 0x3800 && g.cpu.ftop == 7, "restored env, fsw=%04x", g.cpu.fsw);
    CHECK(eq80(g.cpu.fst[7], K_FLD1), "restored register");
}

/* ============ FNSTENV/FLDENV: the 14-byte and 28-byte layouts ======= */
static void v_env(void) {
    static const uint8_t p[] = {
        0xD9, 0xE8,
        0xBB, 0x00, 0x30,
        0xD9, 0x37,                   /* fnstenv [bx] (14-byte form) */
        0xF4 };
    CHECK(run(p, sizeof p, 12) == 0, "fnstenv faults");
    CHECK(r16(0x3000) == 0x037F, "CW@0");
    CHECK(r16(0x3002) == 0x3800, "SW@2=%04x", r16(0x3002));
    CHECK(r16(0x3004) == 0x3FFF, "TW@4=%04x", r16(0x3004));

    /* 32-bit form with the 66 prefix: HOST-MEASURED padding (envprobe2) */
    static const uint8_t q[] = {
        0xD9, 0xE8,
        0xBB, 0x00, 0x30,
        0x66, 0xD9, 0x37,             /* fnstenv32 [bx] */
        0xF4 };
    CHECK(run(q, sizeof q, 12) == 0, "fnstenv32 faults");
    CHECK(r16(0x3000) == 0x037F, "32: CW@0");
    CHECK(r16(0x3002) == 0xFFFF, "32: rsv@2=%04x", r16(0x3002));
    CHECK(r16(0x3004) == 0x3800, "32: SW@4=%04x", r16(0x3004));
    CHECK(r16(0x3008) == 0x3FFF, "32: TW@8=%04x", r16(0x3008));
    CHECK(r16(0x300A) == 0xFFFF, "32: rsv@10");

    /* FLDENV restores TOP/FTW from the image */
    static const uint8_t s[] = {
        0xBB, 0x00, 0x31,
        0xD9, 0x27,                   /* fldenv [bx] */
        0xF4 };
    setup_machine(&g);
    w16(0x3100, 0x027F); w16(0x3102, 0x1800); w16(0x3104, 0xFFFF); /* TOP=3, empty */
    run_program(&g, s, sizeof s, 0, 12);
    CHECK(g.cpu.fcw == 0x027F, "restored fcw=%04x", g.cpu.fcw);
    CHECK(g.cpu.ftop == 3 && g.cpu.ftw == 0xFFFF, "restored ftop/ftw");
}

/* ============ the F3 transcendental boundary stays a pinned #UD ===== */
static void v_transcendental_boundary(void) {
    /* F3 landed the transcendentals: the old #UD pins fell here. The
     * architectural reserved holes (D9 D1-D7 etc.) keep their #UD and are
     * pinned by the frozen table suite ("x87 d9 hole stays #UD"). */
    static const uint8_t f2xm1[] = { 0xD9, 0xF0, 0xF4 };  /* F2XM1 */
    CHECK(run(f2xm1, sizeof f2xm1, 8) == 0, "F2XM1 computes since F3");
    CHECK(eq80(g.cpu.fst[g.cpu.ftop], K_FLDZ), "F2XM1(0) != 0");
}

/* =================================================================== */
/* F2 vectors: arithmetic and compare (raw-byte direction receipts)    */
/* =================================================================== */

static const uint8_t K_INDEF[10] = {0x00,0x00,0x00,0x00,0x00,0x00,0x00,0xC0,0xFF,0xFF};

static unsigned fsw_c0(void) { return (g.cpu.fsw >> 8) & 1u; }
static unsigned fsw_c1(void) { return (g.cpu.fsw >> 9) & 1u; }
static unsigned fsw_c2(void) { return (g.cpu.fsw >> 10) & 1u; }
static unsigned fsw_c3(void) { return (g.cpu.fsw >> 14) & 1u; }

/* push order for ST0=a, ST1=b: fld b then fld a. The m64 slots hold
 * plain doubles (the values in use are exact in double). */
static void setup_ab(const uint8_t *op, size_t n, long double a, long double b) {
    setup_machine(&g);
    { double da = (double)a, db = (double)b;
      uint64_t ua = 0, ub = 0;
      memcpy(&ua, &da, 8); memcpy(&ub, &db, 8);
      mem_write(&g, 0x3000, 8, ub);   /* b, loaded first */
      mem_write(&g, 0x3008, 8, ua); } /* a */
    run_program(&g, op, n, 0, 32);
}
/* m80 slots for extended-precision sources */
static void put80(uint64_t at, long double v) {
    uint8_t b[16]; memset(b, 0, 16); memcpy(b, &v, 16);
    for (int i = 0; i < 10; i++) mem_write(&g, at + i, 1, b[i]);
}

static long double x87_test_denorm(void) {
    uint8_t b[16]; memset(b, 0, 16); b[0] = 1;
    long double v; memcpy(&v, b, 16); return v;
}
static long double x87_test_qnan(void) {
    uint8_t b[16]; memset(b, 0, 16); b[7] = 0xC0; b[8] = 0xFF; b[9] = 0x7F;
    long double v; memcpy(&v, b, 16); return v;
}
static long double x87_test_snan(void) {
    uint8_t b[16]; memset(b, 0, 16); b[0] = 1; b[7] = 0x80; b[8] = 0xFF; b[9] = 0x7F;
    long double v; memcpy(&v, b, 16); return v;
}


static void v_add_sub_dir(void) {
    /* D8 reg: dest ST0 -- (a=2,b=5): FSUB -> -3, FSUBR -> +3 */
    static const uint8_t sub[] = {
        0xBB, 0x00, 0x30, 0xDD, 0x07, 0xDD, 0x47, 0x08, /* fld b; fld a */
        0xD8, 0xE1,                                     /* fsub st0,st1 */
        0xF4 };
    setup_ab(sub, sizeof sub, 2.0L, 5.0L);
    CHECK(!g.cpu.fault, "D8 E1 faults");
    CHECK(g.cpu.fst[g.cpu.ftop] == (2.0L - 5.0L), "D8 E1 FSUB st0 = st0-st1");
    CHECK(g.cpu.fst[(g.cpu.ftop + 1) & 7] == 5.0L, "ST1 kept");

    static const uint8_t subr[] = {
        0xBB, 0x00, 0x30, 0xDD, 0x07, 0xDD, 0x47, 0x08,
        0xD8, 0xE9,                                     /* fsubr st0,st1 */
        0xF4 };
    setup_ab(subr, sizeof subr, 2.0L, 5.0L);
    CHECK(g.cpu.fst[g.cpu.ftop] == (5.0L - 2.0L), "D8 E9 FSUBR st0 = st1-st0");

    /* DC reg: dest ST(i) -- FSUBR st1,st0 -> st1 = st0-st1 = -3 */
    static const uint8_t dce1[] = {
        0xBB, 0x00, 0x30, 0xDD, 0x07, 0xDD, 0x47, 0x08,
        0xDC, 0xE1,                                     /* fsubr st1,st0 */
        0xF4 };
    setup_ab(dce1, sizeof dce1, 2.0L, 5.0L);
    CHECK(g.cpu.fst[(g.cpu.ftop + 1) & 7] == (2.0L - 5.0L), "DC E1 FSUBR st1 = st0-st1");
    CHECK(g.cpu.fst[g.cpu.ftop] == 2.0L, "ST0 kept");

    static const uint8_t dce9[] = {
        0xBB, 0x00, 0x30, 0xDD, 0x07, 0xDD, 0x47, 0x08,
        0xDC, 0xE9,                                     /* fsub st1,st0 */
        0xF4 };
    setup_ab(dce9, sizeof dce9, 2.0L, 5.0L);
    CHECK(g.cpu.fst[(g.cpu.ftop + 1) & 7] == (5.0L - 2.0L), "DC E9 FSUB st1 = st1-st0");

    /* DE +P: FSUBRP st1,st0 -> result -3 after the pop */
    static const uint8_t dee1[] = {
        0xBB, 0x00, 0x30, 0xDD, 0x07, 0xDD, 0x47, 0x08,
        0xDE, 0xE1,                                     /* fsubrp st1,st0 */
        0xF4 };
    setup_ab(dee1, sizeof dee1, 2.0L, 5.0L);
    CHECK(g.cpu.fst[g.cpu.ftop] == (2.0L - 5.0L), "DE E1 FSUBRP result");
    CHECK(g.cpu.ftop == 7, "FSUBRP must pop once, ftop=%d", g.cpu.ftop);

    static const uint8_t dee9[] = {
        0xBB, 0x00, 0x30, 0xDD, 0x07, 0xDD, 0x47, 0x08,
        0xDE, 0xE9,                                     /* fsubp st1,st0 */
        0xF4 };
    setup_ab(dee9, sizeof dee9, 2.0L, 5.0L);
    CHECK(g.cpu.fst[g.cpu.ftop] == (5.0L - 2.0L), "DE E9 FSUBP result");

    /* D8 C1 FADD st0,st1 -> 7 in ST0 */
    static const uint8_t add[] = {
        0xBB, 0x00, 0x30, 0xDD, 0x07, 0xDD, 0x47, 0x08,
        0xD8, 0xC1, 0xF4 };
    setup_ab(add, sizeof add, 2.0L, 5.0L);
    CHECK(g.cpu.fst[g.cpu.ftop] == 7.0L, "D8 C1 FADD");
}

static void v_mul_div_dir(void) {
    static const uint8_t mul[] = {
        0xBB, 0x00, 0x30, 0xDD, 0x07, 0xDD, 0x47, 0x08,
        0xD8, 0xC9, 0xF4 };                             /* fmul st0,st1 */
    setup_ab(mul, sizeof mul, 2.0L, 5.0L);
    CHECK(g.cpu.fst[g.cpu.ftop] == 10.0L, "D8 C9 FMUL");

    static const uint8_t div[] = {
        0xBB, 0x00, 0x30, 0xDD, 0x07, 0xDD, 0x47, 0x08,
        0xD8, 0xF1, 0xF4 };                             /* fdiv st0,st1: 2/5 */
    setup_ab(div, sizeof div, 2.0L, 5.0L);
    CHECK(g.cpu.fst[g.cpu.ftop] == 2.0L / 5.0L, "D8 F1 FDIV st0=st0/st1");

    static const uint8_t divr[] = {
        0xBB, 0x00, 0x30, 0xDD, 0x07, 0xDD, 0x47, 0x08,
        0xD8, 0xF9, 0xF4 };                             /* fdivr st0,st1: 5/2 */
    setup_ab(divr, sizeof divr, 2.0L, 5.0L);
    CHECK(g.cpu.fst[g.cpu.ftop] == 5.0L / 2.0L, "D8 F9 FDIVR st0=st1/st0");

    /* DC F9 FDIV st1,st0: st1 = st1/st0 = 5/2 */
    static const uint8_t dcf9[] = {
        0xBB, 0x00, 0x30, 0xDD, 0x07, 0xDD, 0x47, 0x08,
        0xDC, 0xF9, 0xF4 };
    setup_ab(dcf9, sizeof dcf9, 2.0L, 5.0L);
    CHECK(g.cpu.fst[(g.cpu.ftop + 1) & 7] == 5.0L / 2.0L, "DC F9 FDIV st1=st1/st0");

    /* FIADD m32int: st0 = 2 + 5 */
    static const uint8_t fiadd[] = {
        0xBB, 0x00, 0x30,
        0xD9, 0x07,             /* fld dword [bx] = 2.0 */
        0xDA, 0x07,             /* fiadd dword [bx+0?] -- needs int 5 at [bx+8] */
        0xF4 };
    setup_machine(&g);
    mem_write(&g, 0x3000, 4, 0x40000000u);   /* 2.0f */
    mem_write(&g, 0x3004, 4, 5);             /* int 5 */
    static const uint8_t fiadd2[] = {
        0xBB, 0x00, 0x30,
        0xD9, 0x07,
        0x83, 0xC3, 0x04,       /* add bx,4 */
        0xDA, 0x07,             /* fiadd dword [bx] */
        0xF4 };
    (void)fiadd;
    run_program(&g, fiadd2, sizeof fiadd2, 0, 16);
    CHECK(g.cpu.fst[g.cpu.ftop] == 7.0L, "DA /0 FIADD m32");
}

static void v_div_exceptions(void) {
    /* 1/0 -> +inf and ZE (bit2); 0/0 -> real indefinite and IE (bit0) */
    static const uint8_t dz[] = {
        0xBB, 0x00, 0x30, 0xDD, 0x07, 0xDD, 0x47, 0x08,
        0xD8, 0xF1, 0xF4 };   /* fdiv st0,st1 = 1/0 */
    setup_ab(dz, sizeof dz, 1.0L, 0.0L);
    CHECK(!g.cpu.fault, "divzero faults");
    CHECK(g.cpu.fst[g.cpu.ftop] == __builtin_infl(), "1/0 -> +inf");
    CHECK((g.cpu.fsw & 0x04u) == 0x04u, "ZE (bit2), fsw=%04x", g.cpu.fsw);

    setup_ab(dz, sizeof dz, 0.0L, 0.0L);
    CHECK(eq80(g.cpu.fst[g.cpu.ftop], K_INDEF), "0/0 -> real indefinite");
    CHECK((g.cpu.fsw & 0x01u) == 0x01u, "IE (bit0), fsw=%04x", g.cpu.fsw);

    /* inf - inf -> indefinite + IE */
    static const uint8_t ii[] = {
        0xBB, 0x00, 0x30, 0xDD, 0x07, 0xDD, 0x47, 0x08,
        0xD8, 0xE1, 0xF4 };   /* fsub st0,st1 */
    setup_machine(&g);
    mem_write(&g, 0x3000, 8, 0x7FF0000000000000ull);  /* +inf double */
    mem_write(&g, 0x3008, 8, 0x7FF0000000000000ull);
    run_program(&g, ii, sizeof ii, 0, 16);
    CHECK(eq80(g.cpu.fst[g.cpu.ftop], K_INDEF), "inf-inf -> indefinite");
    CHECK((g.cpu.fsw & 0x01u), "inf-inf -> IE");

    /* unmasked ZE -> #MF(16) */
    static const uint8_t handler[] = { 0xB8, 0x66, 0x44, 0xF4 };
    setup_machine(&g);
    memcpy(g.ram + 0x2800, handler, sizeof handler);
    ivt_set(16, 0x2800, 0);
    w16(0x3000, 0x037B);
    mem_write(&g, 0x3008, 8, 0);                        /* b = 0.0 */
    mem_write(&g, 0x3010, 8, 0x3FF0000000000000ull);    /* a = 1.0 */
    static const uint8_t dzm2[] = {
        0xBB, 0x00, 0x30,
        0xD9, 0x2F,
        0xDD, 0x47, 0x08,       /* fld qword [bx+8] = 0 */
        0xDD, 0x47, 0x10,       /* fld qword [bx+16] = 1 */
        0xD8, 0xF1,             /* fdiv st0,st1 = 1/0 */
        0xF4 };
    run_program(&g, dzm2, sizeof dzm2, 0, 16);
    CHECK((g.cpu.gpr[RAX] & 0xFFFF) == 0x4466, "#MF handler ran");
}

static void v_sqrt_rndint(void) {
    static const uint8_t sq[] = {
        0xBB, 0x00, 0x30, 0xDD, 0x47, 0x08,
        0xD9, 0xFA, 0xF4 };    /* fsqrt [a] */
    setup_ab(sq, sizeof sq, 4.0L, 0.0L);
    CHECK(g.cpu.fst[g.cpu.ftop] == 2.0L, "fsqrt 4 = 2");

    static const uint8_t sqn[] = {
        0xBB, 0x00, 0x30, 0xDD, 0x47, 0x08,
        0xD9, 0xFA, 0xF4 };
    setup_ab(sqn, sizeof sqn, -1.0L, 0.0L);
    CHECK(isnan(g.cpu.fst[g.cpu.ftop]), "sqrt(-1) = NaN");
    CHECK((g.cpu.fsw & 0x01u), "sqrt(-1) -> IE");

    /* sqrt(-0) = -0, no IE */
    setup_ab(sqn, sizeof sqn, -0.0L, 0.0L);
    CHECK(signbit(g.cpu.fst[g.cpu.ftop]) && g.cpu.fst[g.cpu.ftop] == 0.0L,
          "sqrt(-0) = -0");
    CHECK((g.cpu.fsw & 0x01u) == 0, "sqrt(-0) quiet");

    /* FRNDINT 2.5 -> 2 (nearest-even) + PE; RC=up -> 3 + PE */
    static const uint8_t ri[] = {
        0xBB, 0x00, 0x30, 0xDD, 0x47, 0x08,
        0xD9, 0xFC, 0xF4 };
    setup_ab(ri, sizeof ri, 2.5L, 0.0L);
    CHECK(g.cpu.fst[g.cpu.ftop] == 2.0L, "frndint 2.5 -> 2 (even)");
    CHECK((g.cpu.fsw & 0x20u), "frndint inexact -> PE");

    static const uint8_t riu[] = {
        0xBB, 0x00, 0x30,
        0xD9, 0x2F,             /* fldcw RC=up (0x0B7F) */
        0xDD, 0x47, 0x08,
        0xD9, 0xFC, 0xF4 };
    setup_machine(&g);
    w16(0x3000, 0x0B7F);
    mem_write(&g, 0x3008, 8, 0x4004000000000000ull);   /* 2.5 double */
    run_program(&g, riu, sizeof riu, 0, 16);
    CHECK(g.cpu.fst[g.cpu.ftop] == 3.0L, "frndint RC=up -> 3");
}

static void v_prem_flags(void) {
    /* FPREM 17 mod 3: q=5 (101b) -> 2.0, C0=Q2=1 C1=Q0=1 C3=Q1=0 C2=0 */
    static const uint8_t pm[] = {
        0xBB, 0x00, 0x30, 0xDD, 0x07, 0xDD, 0x47, 0x08,
        0xD9, 0xF8, 0xF4 };
    setup_ab(pm, sizeof pm, 17.0L, 3.0L);
    CHECK(g.cpu.fst[g.cpu.ftop] == 2.0L, "fprem 17 mod 3 = 2");
    CHECK(fsw_c0() == 1 && fsw_c1() == 1 && fsw_c3() == 0 && fsw_c2() == 0,
          "fprem flags C0=%u C1=%u C2=%u C3=%u", fsw_c0(), fsw_c1(), fsw_c2(), fsw_c3());

    /* FPREM1 17 mod 3: q=6 (110b) -> -1.0, C0=Q2=1 C3=Q1=1 C1=Q0=0 */
    static const uint8_t pm1[] = {
        0xBB, 0x00, 0x30, 0xDD, 0x07, 0xDD, 0x47, 0x08,
        0xD9, 0xF5, 0xF4 };
    setup_ab(pm1, sizeof pm1, 17.0L, 3.0L);
    CHECK(g.cpu.fst[g.cpu.ftop] == -1.0L, "fprem1 17 mod 3 = -1");
    CHECK(fsw_c0() == 1 && fsw_c1() == 0 && fsw_c3() == 1 && fsw_c2() == 0,
          "fprem1 flags C0=%u C1=%u C2=%u C3=%u", fsw_c0(), fsw_c1(), fsw_c2(), fsw_c3());
}

static void v_scale_xtract(void) {
    /* FSCALE: st0=1.5, st1=3.7 -> 1.5 * 2^3 = 12 */
    static const uint8_t sc[] = {
        0xBB, 0x00, 0x30, 0xDD, 0x07, 0xDD, 0x47, 0x08,
        0xD9, 0xFD, 0xF4 };
    setup_ab(sc, sizeof sc, 1.5L, 3.7L);
    CHECK(g.cpu.fst[g.cpu.ftop] == 12.0L, "fscale 1.5*2^3");

    /* FXTRACT 12: ST0 = significand 1.5, ST1 = exponent 3 (host receipt) */
    static const uint8_t xt[] = {
        0xBB, 0x00, 0x30, 0xDD, 0x47, 0x08,
        0xD9, 0xF4, 0xF4 };
    setup_ab(xt, sizeof xt, 12.0L, 0.0L);
    CHECK(g.cpu.fst[g.cpu.ftop] == 1.5L, "fxtract ST0 = significand");
    CHECK(g.cpu.fst[(g.cpu.ftop + 1) & 7] == 3.0L, "fxtract ST1 = exponent");
    CHECK(g.cpu.ftop == 6, "fxtract pushes (one load -> ftop 7->6), ftop=%d",
          g.cpu.ftop);
}

static void v_com_matrix(void) {
    static const uint8_t com[] = {
        0xBB, 0x00, 0x30, 0xDD, 0x07, 0xDD, 0x47, 0x08,
        0xD8, 0xD1, 0xF4 };     /* fcom st1 */
    setup_ab(com, sizeof com, 2.0L, 5.0L);
    CHECK(fsw_c0() == 1 && fsw_c2() == 0 && fsw_c3() == 0, "fcom 2<5: C0");
    CHECK((g.cpu.fsw & 0x01u) == 0, "clean compare has no IE");

    setup_ab(com, sizeof com, 5.0L, 2.0L);
    CHECK(fsw_c0() == 0 && fsw_c2() == 0 && fsw_c3() == 0, "fcom 5>2");
    setup_ab(com, sizeof com, 5.0L, 5.0L);
    CHECK(fsw_c3() == 1 && fsw_c0() == 0, "fcom 5==5: C3");

    /* QNaN/SNaN probes use m80 loads: an m80 FLD is a pure move and keeps
     * a signaling NaN signaling (host receipt), so FUCOM's SNaN rule is
     * actually testable. */
    { /* full programs (a follow-up run after HLT would never start) */
      static const uint8_t p_fcom_q[] = {
          0xBB, 0x00, 0x30,
          0xDB, 0x2F, 0x83, 0xC3, 0x10, 0xDB, 0x2F,
          0xD8, 0xD1, 0xF4 };                          /* fcom st1 */
      static const uint8_t p_fucom_q[] = {
          0xBB, 0x00, 0x30,
          0xDB, 0x2F, 0x83, 0xC3, 0x10, 0xDB, 0x2F,
          0xDD, 0xE1, 0xF4 };                          /* fucom st1 */
      setup_machine(&g);
      put80(0x3000, 5.0L); put80(0x3010, x87_test_qnan());
      run_program(&g, p_fcom_q, sizeof p_fcom_q, 0, 16);
      CHECK(fsw_c0() == 1 && fsw_c2() == 1 && fsw_c3() == 1, "fcom qnan unordered");
      CHECK((g.cpu.fsw & 0x01u), "fcom qnan -> IE");

      setup_machine(&g);
      put80(0x3000, 5.0L); put80(0x3010, x87_test_qnan());
      run_program(&g, p_fucom_q, sizeof p_fucom_q, 0, 16);
      CHECK(fsw_c0() == 1 && fsw_c2() == 1 && fsw_c3() == 1, "fucom qnan unordered");
      CHECK((g.cpu.fsw & 0x01u) == 0, "fucom qnan stays quiet");

      /* SNaN survives an m80 load (host receipt) and still trips FUCOM */
      setup_machine(&g);
      put80(0x3000, 5.0L); put80(0x3010, x87_test_snan());
      run_program(&g, p_fucom_q, sizeof p_fucom_q, 0, 16);
      CHECK((g.cpu.fsw & 0x01u), "fucom snan -> IE"); }

    /* FCOMP pops; FCOMPP pops twice */
    static const uint8_t fcomp[] = {
        0xBB, 0x00, 0x30, 0xDD, 0x07, 0xDD, 0x47, 0x08,
        0xD8, 0xD9, 0xF4 };     /* fcomp st1 */
    setup_ab(fcomp, sizeof fcomp, 5.0L, 2.0L);
    CHECK(g.cpu.ftop == 7, "fcomp pops once");

    static const uint8_t fcompp[] = {
        0xBB, 0x00, 0x30, 0xDD, 0x07, 0xDD, 0x47, 0x08,
        0xDE, 0xD9, 0xF4 };     /* fcompp */
    setup_ab(fcompp, sizeof fcompp, 5.0L, 2.0L);
    CHECK(g.cpu.ftop == 0, "fcompp pops twice, ftop=%d", g.cpu.ftop);
    CHECK(fsw_c3() == 0 && fsw_c0() == 0, "fcompp 5>2 flags");
}

static void v_comi_eflags(void) {
    static const uint8_t fcomi[] = {
        0xBB, 0x00, 0x30, 0xDD, 0x07, 0xDD, 0x47, 0x08,
        0xDB, 0xF1, 0xF4 };     /* fcomi st0,st1 */
    setup_ab(fcomi, sizeof fcomi, 2.0L, 5.0L);
    CHECK((g.cpu.rflags & FLAG_CF) != 0, "fcomi 2<5 -> CF");
    CHECK((g.cpu.rflags & (FLAG_ZF | FLAG_PF)) == 0, "fcomi 2<5 clear ZF/PF");

    setup_ab(fcomi, sizeof fcomi, 5.0L, 5.0L);
    CHECK((g.cpu.rflags & FLAG_ZF) != 0 && !(g.cpu.rflags & FLAG_CF), "fcomi eq -> ZF");

    { static const uint8_t p_fcomi_q[] = {
          0xBB, 0x00, 0x30,
          0xDB, 0x2F, 0x83, 0xC3, 0x10, 0xDB, 0x2F,
          0xDB, 0xF1, 0xF4 };                          /* fcomi st1 */
      static const uint8_t p_fucomi_q[] = {
          0xBB, 0x00, 0x30,
          0xDB, 0x2F, 0x83, 0xC3, 0x10, 0xDB, 0x2F,
          0xDB, 0xE9, 0xF4 };                          /* fucomi st1 */
      setup_machine(&g);
      put80(0x3000, 5.0L); put80(0x3010, x87_test_qnan());
      run_program(&g, p_fcomi_q, sizeof p_fcomi_q, 0, 16);
      CHECK((g.cpu.rflags & (FLAG_CF | FLAG_PF | FLAG_ZF)) == (FLAG_CF | FLAG_PF | FLAG_ZF),
            "fcomi qnan unordered");
      CHECK((g.cpu.fsw & 0x01u), "fcomi qnan -> IE");

      setup_machine(&g);
      put80(0x3000, 5.0L); put80(0x3010, x87_test_qnan());
      run_program(&g, p_fucomi_q, sizeof p_fucomi_q, 0, 16);
      CHECK((g.cpu.rflags & (FLAG_CF | FLAG_PF | FLAG_ZF)) == (FLAG_CF | FLAG_PF | FLAG_ZF),
            "fucomi qnan unordered");
      CHECK((g.cpu.fsw & 0x01u) == 0, "fucomi qnan stays quiet"); }

    /* FCOMIP pops */
    static const uint8_t fcomip[] = {
        0xBB, 0x00, 0x30, 0xDD, 0x07, 0xDD, 0x47, 0x08,
        0xDF, 0xF1, 0xF4 };
    setup_ab(fcomip, sizeof fcomip, 5.0L, 2.0L);
    CHECK(g.cpu.ftop == 7, "fcomip pops");
    CHECK((g.cpu.rflags & FLAG_CF) == 0, "fcomip 5>2");
}

static void v_ficom_int(void) {
    static const uint8_t fi[] = {
        0xBB, 0x00, 0x30,
        0xD9, 0x07,             /* fld dword [bx] = 2.0 */
        0x83, 0xC3, 0x04,
        0xDA, 0x17,             /* ficom dword [bx] = int 5 */
        0xF4 };
    setup_machine(&g);
    mem_write(&g, 0x3000, 4, 0x40000000u);
    mem_write(&g, 0x3004, 4, 5);
    run_program(&g, fi, sizeof fi, 0, 16);
    CHECK(fsw_c0() == 1, "ficom 2 < 5 -> C0");
    CHECK(g.cpu.ftop == 7, "ficom does not pop");

    static const uint8_t ficomp[] = {
        0xBB, 0x00, 0x30,
        0xD9, 0x07,
        0x83, 0xC3, 0x04,
        0xDA, 0x1F,             /* ficomp dword [bx] */
        0xF4 };
    setup_machine(&g);
    mem_write(&g, 0x3000, 4, 0x40000000u);
    mem_write(&g, 0x3004, 4, 5);
    run_program(&g, ficomp, sizeof ficomp, 0, 16);
    CHECK(g.cpu.ftop == 0, "ficmp pops");
}


static void v_denorm_de(void) {
    /* denormal OPERAND (80-bit form) sets DE (bit1); unmasking DM -> #MF */
    static const uint8_t da[] = {
        0xBB, 0x00, 0x30,
        0xDB, 0x2F,             /* fld tword [bx] = denormal */
        0x83, 0xC3, 0x10,
        0xDB, 0x2F,             /* fld tword [bx] = 1.0 */
        0xD8, 0xC1, 0xF4 };     /* fadd st0,st1 */
    setup_machine(&g);
    put80(0x3000, x87_test_denorm());
    put80(0x3010, 1.0L);
    run_program(&g, da, sizeof da, 0, 16);
    CHECK((g.cpu.fsw & 0x02u) == 0x02u, "DE (bit1) on denormal operand, fsw=%04x",
          g.cpu.fsw);
    CHECK((g.cpu.fsw & 0x01u) == 0, "denormal is not invalid");

    /* DM cleared (FCW bit1) -> #MF delivered */
    static const uint8_t handler[] = { 0xB8, 0x22, 0x11, 0xF4 };
    static const uint8_t dam[] = {
        0xBB, 0x00, 0x30,
        0xD9, 0x2F,             /* fldcw [bx]=0x037D */
        0xDB, 0x6F, 0x10,       /* fld tword [bx+16] = denormal */
        0xDB, 0x6F, 0x20,       /* fld tword [bx+32] = 1.0 */
        0xD8, 0xC1,
        0xF4 };
    setup_machine(&g);
    memcpy(g.ram + 0x2800, handler, sizeof handler);
    ivt_set(16, 0x2800, 0);
    w16(0x3000, 0x037D);        /* DM (bit1) cleared */
    put80(0x3010, x87_test_denorm());
    put80(0x3020, 1.0L);
    run_program(&g, dam, sizeof dam, 0, 16);
    CHECK((g.cpu.gpr[RAX] & 0xFFFF) == 0x1122, "#MF handler ran for unmasked DM");
}

/* ============ D6 regression: the measured exception-bit order ======= */
static void v_d6_flag_map(void) {
    /* inexact -> PE bit5, invalid -> IE bit0, divzero -> ZE bit2,
     * overflow -> OE bit3, underflow -> UE bit4 (host /tmp/flagbits) */
    static const uint8_t add[] = {
        0xBB, 0x00, 0x30,
        0xDB, 0x2F,             /* fld tword [bx] = 1/7 */
        0x83, 0xC3, 0x10,
        0xDB, 0x2F,             /* fld tword [bx] = 1/3 */
        0xD8, 0xC1, 0xF4 };     /* add: inexact in extended */
    { volatile long double third = 1.0L / 3.0L, seventh = 1.0L / 7.0L;
      setup_machine(&g);
      put80(0x3000, seventh);
      put80(0x3010, third);
      run_program(&g, add, sizeof add, 0, 16); }
    CHECK((g.cpu.fsw & 0x20u) == 0x20u, "PE is bit5");
    CHECK((g.cpu.fsw & 0x21u) == 0x20u, "and not bit0");

    /* overflow: max-extended * max-extended (m80 sources) -> OE|PE */
    static const uint8_t mul[] = {
        0xBB, 0x00, 0x30,
        0xDB, 0x2F, 0x83, 0xC3, 0x10, 0xDB, 0x2F,
        0xD8, 0xC9, 0xF4 };
    { uint8_t huge[16]; memset(huge, 0, 16);
      for (int i = 0; i < 8; i++) huge[i] = 0xFF;
      huge[8] = 0xFE; huge[9] = 0x7F;
      long double h; memcpy(&h, huge, 16);
      setup_machine(&g);
      put80(0x3000, h); put80(0x3010, h);
      run_program(&g, mul, sizeof mul, 0, 16); }
    CHECK((g.cpu.fsw & 0x08u) == 0x08u, "OE is bit3");
}

/* =================================================================== */
/* F3 vectors: transcendentals + BCD (host-silicon receipts,            */
/* /tmp/transprobe{,2..7}: results, C-flag writes, push shape, corners) */
/* =================================================================== */

/* host-measured 80-bit payloads (transprobe2; mem order like K_* above) */
static const uint8_t K_SIN_HALF [10] = {0x44,0x7F,0x2F,0x58,0xA2,0x43,0x77,0xF5,0xFD,0x3F};
static const uint8_t K_COS_HALF [10] = {0xEE,0x7C,0xEA,0xDB,0x32,0x40,0xA9,0xE0,0xFE,0x3F};
static const uint8_t K_TAN_HALF [10] = {0x19,0x52,0x3A,0x9A,0xDF,0x7A,0xDA,0x8B,0xFE,0x3F};
static const uint8_t K_PI4     [10] = {0x35,0xC2,0x68,0x21,0xA2,0xDA,0x0F,0xC9,0xFE,0x3F};
static const uint8_t K_PI2     [10] = {0x35,0xC2,0x68,0x21,0xA2,0xDA,0x0F,0xC9,0xFF,0x3F};
static const uint8_t K_3PI4    [10] = {0xA8,0x91,0x0E,0x99,0xF9,0xE3,0xCB,0x96,0x00,0x40};
static const uint8_t K_2XM1_HALF[10] = {0x11,0x92,0x79,0xE7,0xCF,0xCC,0x13,0xD4,0xFD,0x3F};
static const uint8_t K_L2XP1   [10] = {0x6F,0xCB,0xC1,0xF9,0x56,0x27,0xA0,0xE0,0xFF,0x3F};
static const uint8_t K_Q1      [10] = {0x01,0x00,0x00,0x00,0x00,0x00,0x00,0xC0,0xFF,0x7F};

/* m80 store helpers: write ST0 back to memory at [bx] and halt */
static void put_val(uint64_t at, long double v) { put80(at, v); }
static long double mk_val(uint16_t se, uint64_t sig) {
    uint8_t b[16]; memset(b, 0, 16);
    memcpy(b, &sig, 8); b[8] = (uint8_t)se; b[9] = (uint8_t)(se >> 8);
    long double v; memcpy(&v, b, 16); return v;
}
/* 10-byte pattern compare against guest memory */
static int mem_cmp(uint64_t at, const uint8_t want[10]) {
    for (int i = 0; i < 10; i++)
        if ((uint8_t)mem_read(&g, at + (uint64_t)i, 1) != want[i]) return 0;
    return 1;
}

/* FSIN/FCOS/F2XM1 one-operand shape: fld [bx]; OP; fstp [bx]; hlt */
static void v_trans_results(void) {
    static const uint8_t sin_p[]  = {0xBB,0x00,0x30, 0xDB,0x2F, 0xD9,0xFE, 0xDB,0x3F, 0xF4};
    static const uint8_t cos_p[]  = {0xBB,0x00,0x30, 0xDB,0x2F, 0xD9,0xFF, 0xDB,0x3F, 0xF4};
    static const uint8_t x2m1_p[] = {0xBB,0x00,0x30, 0xDB,0x2F, 0xD9,0xF0, 0xDB,0x3F, 0xF4};
    /* fptan pair: [0x3000]=1.0 slot, tan stored to 0x3010 */
    static const uint8_t tan_p[] = {
        0xBB,0x00,0x30, 0xDB,0x2F, 0xD9,0xF2, 0xDB,0x3F,
        0xBB,0x10,0x30, 0xDB,0x3F, 0xF4};
    static const uint8_t sincos_p[] = {
        0xBB,0x00,0x30, 0xDB,0x2F, 0xD9,0xFB, 0xDB,0x3F,
        0xBB,0x10,0x30, 0xDB,0x3F, 0xF4};

    setup_machine(&g); put_val(0x3000, 0.5L);
    run_program(&g, sin_p, sizeof sin_p, 0, 16);
    CHECK(!g.cpu.fault, "fsin 0.5 fault");
    CHECK(mem_cmp(0x3000, K_SIN_HALF), "sin(0.5) payload");

    setup_machine(&g); put_val(0x3000, 0.5L);
    run_program(&g, cos_p, sizeof cos_p, 0, 16);
    CHECK(mem_cmp(0x3000, K_COS_HALF), "cos(0.5) payload");

    setup_machine(&g); put_val(0x3000, 0.5L);
    run_program(&g, x2m1_p, sizeof x2m1_p, 0, 16);
    CHECK(mem_cmp(0x3000, K_2XM1_HALF), "f2xm1(0.5) payload");
    CHECK((g.cpu.fsw & 0x020u) == 0x020u, "f2xm1(0.5) PE");
    CHECK(fsw_c1() == 0, "f2xm1 C1=0 (round-down receipt)");

    /* exact identities */
    setup_machine(&g); put_val(0x3000, 0.0L);
    run_program(&g, sin_p, sizeof sin_p, 0, 16);
    CHECK(mem_cmp(0x3000, K_FLDZ), "sin(0)=0");
    CHECK(g.cpu.fsw == 0, "sin(0) clean FSW=%04x", g.cpu.fsw);
    setup_machine(&g); put_val(0x3000, 0.0L);
    run_program(&g, cos_p, sizeof cos_p, 0, 16);
    CHECK(mem_cmp(0x3000, K_FLD1), "cos(0)=1");
    setup_machine(&g); put_val(0x3000, 1.0L);
    run_program(&g, x2m1_p, sizeof x2m1_p, 0, 16);
    CHECK(mem_cmp(0x3000, K_FLD1), "f2xm1(1)=1");
    CHECK((g.cpu.fsw & 0x020u) == 0x020u, "f2xm1(1) PE (host receipt)");
    setup_machine(&g); put_val(0x3000, -1.0L);
    run_program(&g, x2m1_p, sizeof x2m1_p, 0, 16);
    { uint8_t want[10] = {0,0,0,0,0,0,0,0x80,0xFE,0xBF};  /* -0.5 */
      CHECK(mem_cmp(0x3000, want), "f2xm1(-1)=-0.5"); }

    /* fptan(0.5): ST0=1.0, ST1=tan(0.5) */
    setup_machine(&g); put_val(0x3000, 0.5L);
    run_program(&g, tan_p, sizeof tan_p, 0, 16);
    CHECK(mem_cmp(0x3000, K_FLD1), "fptan top=1.0");
    CHECK(mem_cmp(0x3010, K_TAN_HALF), "fptan tan(0.5) payload");
    CHECK(fsw_c1() == 1 && (g.cpu.fsw & 0x020u), "fptan C1=1 PE");

    /* fsincos(0.5): ST0=cos, ST1=sin */
    setup_machine(&g); put_val(0x3000, 0.5L);
    run_program(&g, sincos_p, sizeof sincos_p, 0, 16);
    CHECK(mem_cmp(0x3000, K_COS_HALF), "fsincos cos");
    CHECK(mem_cmp(0x3010, K_SIN_HALF), "fsincos sin");

    /* fpatan(y=1,x=1) = pi/4 (rounded pi/4 mantissa == FLDPI's) */
    static const uint8_t atan_p[] = {
        0xBB,0x00,0x30, 0xDB,0x2F,              /* fld y */
        0x83,0xC3,0x10, 0xDB,0x2F,              /* fld x */
        0xD9,0xF3, 0xDB,0x3F, 0xF4};
    setup_machine(&g); put_val(0x3000, 1.0L); put_val(0x3010, 1.0L);
    run_program(&g, atan_p, sizeof atan_p, 0, 16);
    CHECK(mem_cmp(0x3010, K_PI4), "fpatan(1,1)=pi/4");

    /* fyl2x(y=3,x=2) = 3.0 exact-ish (3*log2(2)); fyl2xp1(y=3,x=0.5) */
    static const uint8_t l2x_p[] = {
        0xBB,0x00,0x30, 0xDB,0x2F,
        0x83,0xC3,0x10, 0xDB,0x2F,
        0xD9,0xF1, 0xDB,0x3F, 0xF4};
    static const uint8_t l2xp1_p[] = {
        0xBB,0x00,0x30, 0xDB,0x2F,
        0x83,0xC3,0x10, 0xDB,0x2F,
        0xD9,0xF9, 0xDB,0x3F, 0xF4};
    setup_machine(&g); put_val(0x3000, 3.0L); put_val(0x3010, 2.0L);
    run_program(&g, l2x_p, sizeof l2x_p, 0, 16);
    { long double want = 3.0L; uint8_t wb[10]; to80(want, wb);
      CHECK(mem_cmp(0x3010, wb), "fyl2x(3,2)=3"); }
    setup_machine(&g); put_val(0x3000, 3.0L); put_val(0x3010, 0.5L);
    run_program(&g, l2xp1_p, sizeof l2xp1_p, 0, 16);
    CHECK(mem_cmp(0x3010, K_L2XP1), "fyl2xp1(3,0.5) payload");
}

/* the |x| >= 2^63 boundary: source unchanged, C2=1, no push, quiet */
static void v_trans_huge_c2(void) {
    static const uint8_t sin_p[]  = {0xBB,0x00,0x30, 0xDB,0x2F, 0xD9,0xFE, 0xDB,0x3F, 0xF4};
    static const uint8_t tan_p[] = {0xBB,0x00,0x30, 0xDB,0x2F, 0xD9,0xF2, 0xF4};
    long double big = 0x1p63L;      /* 2^63 */
    long double near = 0x1p63L - 1024.0L;  /* still 2^63? no: 2^63-1024 */
    uint8_t bb[10]; to80(big, bb);

    setup_machine(&g); put_val(0x3000, big);
    run_program(&g, sin_p, sizeof sin_p, 0, 16);
    CHECK(mem_cmp(0x3000, bb), "sin(2^63) source unchanged");
    CHECK(fsw_c2() == 1 && fsw_c1() == 0, "sin(2^63) C2=1 C1=0");
    CHECK(g.cpu.fsw == 0x0400u, "sin(2^63) quiet FSW=%04x", g.cpu.fsw);

    setup_machine(&g); put_val(0x3000, near);
    run_program(&g, sin_p, sizeof sin_p, 0, 16);
    CHECK(fsw_c2() == 0, "sin(2^63-1024) reduces (C2=0)");
    CHECK(!mem_cmp(0x3000, bb), "sin(2^63-1024) computed");

    /* fptan huge: no push at all (the one fld stays at ftop=7) */
    setup_machine(&g); put_val(0x3000, big);
    run_program(&g, tan_p, sizeof tan_p, 0, 16);
    CHECK(g.cpu.ftop == 7, "fptan huge must not push (ftop=%d)", g.cpu.ftop);
    CHECK(g.cpu.fst[7] == big, "fptan huge source unchanged");
    CHECK(fsw_c2() == 1 && (g.cpu.fsw & 0xC7FFu) == 0x0400u, "fptan huge FSW=%04x", g.cpu.fsw);

    static const uint8_t sincos_p[] = {0xBB,0x00,0x30, 0xDB,0x2F, 0xD9,0xFB, 0xF4};
    setup_machine(&g); put_val(0x3000, big);
    run_program(&g, sincos_p, sizeof sincos_p, 0, 16);
    CHECK(g.cpu.ftop == 7, "fsincos huge must not push");
    CHECK(g.cpu.fst[7] == big, "fsincos huge unchanged");
}

/* NaN/inf shapes (transprobe2): passthrough, quiet-on-SNaN, double-indef */
static void v_trans_nan_inf(void) {
    static const uint8_t sin_p[]  = {0xBB,0x00,0x30, 0xDB,0x2F, 0xD9,0xFE, 0xDB,0x3F, 0xF4};
    static const uint8_t tan_p[] = {
        0xBB,0x00,0x30, 0xDB,0x2F, 0xD9,0xF2, 0xDB,0x3F,
        0xBB,0x10,0x30, 0xDB,0x3F, 0xF4};
    long double inf  = mk_val(0x7FFF, 0x8000000000000000ull);
    long double qnan = mk_val(0x7FFF, 0xC000000000000000ull);
    long double snan = mk_val(0x7FFF, 0x8000000000000001ull);

    setup_machine(&g); put_val(0x3000, inf);
    run_program(&g, sin_p, sizeof sin_p, 0, 16);
    CHECK(mem_cmp(0x3000, K_INDEF), "sin(inf)=indefinite");
    CHECK(g.cpu.fsw & 0x001u, "sin(inf) IE");

    setup_machine(&g); put_val(0x3000, qnan);
    run_program(&g, sin_p, sizeof sin_p, 0, 16);
    CHECK(mem_cmp(0x3000, K_INDEF) == 0, "sin(qnan) not indefinite");
    { uint8_t qb[10]; to80(qnan, qb); CHECK(mem_cmp(0x3000, qb), "sin(qnan) passthrough bits"); }
    CHECK(g.cpu.fsw == 0, "sin(qnan) quiet FSW=%04x", g.cpu.fsw);

    setup_machine(&g); put_val(0x3000, snan);
    run_program(&g, sin_p, sizeof sin_p, 0, 16);
    CHECK(mem_cmp(0x3000, K_Q1), "sin(snan) quieted payload");
    CHECK(g.cpu.fsw & 0x001u, "sin(snan) IE");

    /* fptan(inf): BOTH slots indefinite, push happened */
    setup_machine(&g); put_val(0x3000, inf);
    run_program(&g, tan_p, sizeof tan_p, 0, 16);
    CHECK(mem_cmp(0x3000, K_INDEF), "fptan(inf) top indef");
    CHECK(mem_cmp(0x3010, K_INDEF), "fptan(inf) tan indef");
    CHECK(g.cpu.fsw & 0x001u, "fptan(inf) IE");

    setup_machine(&g); put_val(0x3000, qnan);
    run_program(&g, tan_p, sizeof tan_p, 0, 16);
    { uint8_t qb[10]; to80(qnan, qb);
      CHECK(mem_cmp(0x3000, qb), "fptan(qnan) top");
      CHECK(mem_cmp(0x3010, qb), "fptan(qnan) tan slot"); }
    CHECK(g.cpu.fsw == 0, "fptan(qnan) quiet");

    static const uint8_t sincos_p[] = {
        0xBB,0x00,0x30, 0xDB,0x2F, 0xD9,0xFB, 0xDB,0x3F,
        0xBB,0x10,0x30, 0xDB,0x3F, 0xF4};
    setup_machine(&g); put_val(0x3000, snan);
    run_program(&g, sincos_p, sizeof sincos_p, 0, 16);
    CHECK(mem_cmp(0x3000, K_Q1), "fsincos(snan) cos quieted");
    CHECK(mem_cmp(0x3010, K_Q1), "fsincos(snan) sin quieted");
    CHECK(g.cpu.fsw & 0x001u, "fsincos(snan) IE");
}

/* push shape + stack faults (transprobe3 full-stack receipts) */
static void v_trans_stack(void) {
    /* 7 canaries (2.0 at 0x3000) + arg 0.5 (at 0x3010) = full stack */
    static const uint8_t tan_full[] = {
        0xBB,0x00,0x30,
        0xDB,0x2F, 0xDB,0x2F, 0xDB,0x2F, 0xDB,0x2F,
        0xDB,0x2F, 0xDB,0x2F, 0xDB,0x2F,
        0x83,0xC3,0x10, 0xDB,0x2F,
        0xD9,0xF2, 0xF4};
    static const uint8_t sincos_full[] = {
        0xBB,0x00,0x30,
        0xDB,0x2F, 0xDB,0x2F, 0xDB,0x2F, 0xDB,0x2F,
        0xDB,0x2F, 0xDB,0x2F, 0xDB,0x2F,
        0x83,0xC3,0x10, 0xDB,0x2F,
        0xD9,0xFB, 0xF4};
    uint8_t two[10]; to80(2.0L, two);

    setup_machine(&g); put_val(0x3000, 2.0L); put_val(0x3010, 0.5L);
    run_program(&g, tan_full, sizeof tan_full, 0, 24);
    CHECK(!g.cpu.fault, "fptan full must not #MF (masked)");
    CHECK(g.cpu.ftop == 7, "fptan full pushes anyway (ftop=%d)", g.cpu.ftop);
    CHECK(eq80(g.cpu.fst[7], K_INDEF), "fptan full new ST0 indef");
    CHECK(eq80(g.cpu.fst[0], K_INDEF), "fptan full old ST0 indef");
    CHECK(g.cpu.fst[6] == 2.0L, "canary survives");
    CHECK((g.cpu.fsw & 0x041u) == 0x041u, "IE+SF set FSW=%04x", g.cpu.fsw);
    CHECK(fsw_c1() == 1, "overflow C1=1");

    setup_machine(&g); put_val(0x3000, 2.0L); put_val(0x3010, 0.5L);
    run_program(&g, sincos_full, sizeof sincos_full, 0, 24);
    CHECK(g.cpu.ftop == 7, "fsincos full pushes");
    CHECK(eq80(g.cpu.fst[7], K_INDEF), "fsincos full ST0 indef");
    CHECK(eq80(g.cpu.fst[0], K_INDEF), "fsincos full ST1 indef");
    CHECK((g.cpu.fsw & 0x041u) == 0x041u && fsw_c1() == 1, "fsincos full flags");

    /* underflow: op on empty ST0 -> IE|SF, no fault, stack untouched */
    static const uint8_t empty[] = { 0xD9, 0xFE, 0xF4 };
    CHECK(run(empty, sizeof empty, 8) == 0, "fsin empty no fault");
    CHECK((g.cpu.fsw & 0x041u) == 0x041u, "fsin empty IE+SF FSW=%04x", g.cpu.fsw);

    /* normal push shape: ftop moves by one */
    static const uint8_t tan_p[] = {
        0xBB,0x00,0x30, 0xDB,0x2F, 0xD9,0xF2, 0xF4};
    setup_machine(&g); put_val(0x3000, 0.5L);
    run_program(&g, tan_p, sizeof tan_p, 0, 16);
    CHECK(g.cpu.ftop == 6, "fptan normal pushes (ftop=%d)", g.cpu.ftop);
}

/* FYL2X zero/inf corners (transprobe2 clean receipts, incl. the
 * x=0,y=+-inf ZE quirk) */
static void v_trans_l2x_corner(void) {
    static const uint8_t l2x_p[] = {
        0xBB,0x00,0x30, 0xDB,0x2F,
        0x83,0xC3,0x10, 0xDB,0x2F,
        0xD9,0xF1, 0xDB,0x3F, 0xF4};
    long double inf  = mk_val(0x7FFF, 0x8000000000000000ull);
    uint8_t minf[10], pinf[10], indef[10];
    to80(-inf, minf); to80(inf, pinf); to80(0.0L, indef);
    memcpy(indef, K_INDEF, 10);

    /* (y=1, x=0) -> -inf + ZE */
    setup_machine(&g); put_val(0x3000, 1.0L); put_val(0x3010, 0.0L);
    run_program(&g, l2x_p, sizeof l2x_p, 0, 16);
    CHECK(mem_cmp(0x3010, minf), "fyl2x(1,0)=-inf");
    CHECK(g.cpu.fsw == 0x0004u, "fyl2x(1,0) ZE only FSW=%04x", g.cpu.fsw);

    /* (y=-1, x=0) -> +inf + ZE */
    setup_machine(&g); put_val(0x3000, -1.0L); put_val(0x3010, 0.0L);
    run_program(&g, l2x_p, sizeof l2x_p, 0, 16);
    CHECK(mem_cmp(0x3010, pinf), "fyl2x(-1,0)=+inf");
    CHECK(g.cpu.fsw == 0x0004u, "fyl2x(-1,0) ZE");

    /* (y=0, x=0) -> indefinite + IE */
    setup_machine(&g); put_val(0x3000, 0.0L); put_val(0x3010, 0.0L);
    run_program(&g, l2x_p, sizeof l2x_p, 0, 16);
    CHECK(mem_cmp(0x3010, indef), "fyl2x(0,0) indef");
    CHECK(g.cpu.fsw == 0x0001u, "fyl2x(0,0) IE");

    /* (y=+inf, x=0) -> -inf WITHOUT ZE (host quirk) */
    setup_machine(&g); put_val(0x3000, inf); put_val(0x3010, 0.0L);
    run_program(&g, l2x_p, sizeof l2x_p, 0, 16);
    CHECK(mem_cmp(0x3010, minf), "fyl2x(inf,0)=-inf");
    CHECK(g.cpu.fsw == 0, "fyl2x(inf,0) NO ZE (quirk) FSW=%04x", g.cpu.fsw);

    /* (y=1, x=-1) -> indefinite + IE (log of negative) */
    setup_machine(&g); put_val(0x3000, 1.0L); put_val(0x3010, -1.0L);
    run_program(&g, l2x_p, sizeof l2x_p, 0, 16);
    CHECK(mem_cmp(0x3010, indef), "fyl2x(1,-1) indef");
    CHECK(g.cpu.fsw == 0x0001u, "fyl2x(1,-1) IE");

    /* (y=0, x=+inf) -> indefinite + IE (0*inf) */
    setup_machine(&g); put_val(0x3000, 0.0L); put_val(0x3010, inf);
    run_program(&g, l2x_p, sizeof l2x_p, 0, 16);
    CHECK(mem_cmp(0x3010, indef), "fyl2x(0,inf) indef");
    CHECK(g.cpu.fsw == 0x0001u, "fyl2x(0,inf) IE");

    /* denorm operand -> DE */
    setup_machine(&g); put_val(0x3000, x87_test_denorm()); put_val(0x3010, 2.0L);
    run_program(&g, l2x_p, sizeof l2x_p, 0, 16);
    CHECK(g.cpu.fsw & 0x002u, "fyl2x denorm y -> DE");
}

/* FYL2XP1 domain + the x <= -1 host corner (transprobe4 receipts) */
static void v_trans_l2xp1_domain(void) {
    static const uint8_t l2xp1_p[] = {
        0xBB,0x00,0x30, 0xDB,0x2F,
        0x83,0xC3,0x10, 0xDB,0x2F,
        0xD9,0xF9, 0xDB,0x3F, 0xF4};
    long double inf  = mk_val(0x7FFF, 0x8000000000000000ull);
    uint8_t indef[10], one[10], mone[10], mtwo[10], mzero[10], minf[10];
    memcpy(indef, K_INDEF, 10);
    to80(1.0L, one); to80(-1.0L, mone); to80(-2.0L, mtwo);
    to80(-0.0L, mzero); to80(-inf, minf);

    /* normal: (y=1,x=1) -> 1.0 */
    setup_machine(&g); put_val(0x3000, 1.0L); put_val(0x3010, 1.0L);
    run_program(&g, l2xp1_p, sizeof l2xp1_p, 0, 16);
    CHECK(mem_cmp(0x3010, one), "fyl2xp1(1,1)=1");
    CHECK((g.cpu.fsw & 0x020u) == 0x020u, "fyl2xp1(1,1) PE");

    /* normal inexact: (y=1,x=-0.5) -> log2(0.5) = -1 */
    setup_machine(&g); put_val(0x3000, 1.0L); put_val(0x3010, -0.5L);
    run_program(&g, l2xp1_p, sizeof l2xp1_p, 0, 16);
    CHECK(mem_cmp(0x3010, mone), "fyl2xp1(1,-0.5)=-1");

    /* host corner x <= -1: (y=5,x=-1) -> x = -1.0 + PE */
    setup_machine(&g); put_val(0x3000, 5.0L); put_val(0x3010, -1.0L);
    run_program(&g, l2xp1_p, sizeof l2xp1_p, 0, 16);
    CHECK(mem_cmp(0x3010, mone), "fyl2xp1(5,-1)=-1 (host corner)");
    CHECK(g.cpu.fsw == 0x0020u, "corner flags FSW=%04x", g.cpu.fsw);

    /* (y=3,x=-2) -> -2.0 (NOT y*x) + PE */
    setup_machine(&g); put_val(0x3000, 3.0L); put_val(0x3010, -2.0L);
    run_program(&g, l2xp1_p, sizeof l2xp1_p, 0, 16);
    CHECK(mem_cmp(0x3010, mtwo), "fyl2xp1(3,-2)=-2 (host corner)");

    /* (y=0,x=-1) -> -0 (x*y fold) */
    setup_machine(&g); put_val(0x3000, 0.0L); put_val(0x3010, -1.0L);
    run_program(&g, l2xp1_p, sizeof l2xp1_p, 0, 16);
    CHECK(mem_cmp(0x3010, mzero), "fyl2xp1(0,-1)=-0");
    CHECK(g.cpu.fsw == 0, "fold quiet FSW=%04x", g.cpu.fsw);

    /* (y=+inf,x=-2) -> -inf */
    setup_machine(&g); put_val(0x3000, inf); put_val(0x3010, -2.0L);
    run_program(&g, l2xp1_p, sizeof l2xp1_p, 0, 16);
    CHECK(mem_cmp(0x3010, minf), "fyl2xp1(inf,-2)=-inf");

    /* (y=1,x=-inf) -> indefinite + IE */
    setup_machine(&g); put_val(0x3000, 1.0L); put_val(0x3010, -inf);
    run_program(&g, l2xp1_p, sizeof l2xp1_p, 0, 16);
    CHECK(mem_cmp(0x3010, indef), "fyl2xp1(1,-inf) indef");
    CHECK(g.cpu.fsw == 0x0001u, "fyl2xp1(1,-inf) IE");
}

/* FPATAN = atan2(ST1,ST0): signed-zero quadrants + inf corners */
static void v_trans_fpatan_quadrants(void) {
    static const uint8_t atan_p[] = {
        0xBB,0x00,0x30, 0xDB,0x2F,
        0x83,0xC3,0x10, 0xDB,0x2F,
        0xD9,0xF3, 0xDB,0x3F, 0xF4};
    long double inf = mk_val(0x7FFF, 0x8000000000000000ull);
    uint8_t p0[10], m0[10], pi[10], mpi[10], pi2[10], pi4[10], mpi4[10], p34[10];
    to80(0.0L, p0); to80(-0.0L, m0);
    memcpy(pi, K_PI, 10); memcpy(pi4, K_PI4, 10); memcpy(pi2, K_PI2, 10);
    memcpy(p34, K_3PI4, 10);
    /* -pi etc: K_PI-family patterns with the sign bit set */
    memcpy(mpi, K_PI, 10); mpi[9] |= 0x80;
    memcpy(mpi4, K_PI4, 10); mpi4[9] |= 0x80;

#define ATAN_CASE(yval, xval, wantb, want_sw, label) do { \
    setup_machine(&g); put_val(0x3000, (yval)); put_val(0x3010, (xval)); \
    run_program(&g, atan_p, sizeof atan_p, 0, 16); \
    CHECK(mem_cmp(0x3010, (wantb)), label); \
    CHECK(g.cpu.fsw == (want_sw), label " FSW=%04x", g.cpu.fsw); \
} while (0)

    ATAN_CASE(0.0L,  0.0L,  p0,  0x0000u, "atan2(+0,+0)=+0");
    ATAN_CASE(-0.0L, 0.0L,  m0,  0x0000u, "atan2(-0,+0)=-0");
    ATAN_CASE(0.0L, -0.0L,  pi,  0x0220u, "atan2(+0,-0)=+pi");
    ATAN_CASE(-0.0L,-0.0L,  mpi, 0x0220u, "atan2(-0,-0)=-pi");
    ATAN_CASE(0.0L,  inf,   p0,  0x0000u, "atan2(+0,+inf)=+0");
    ATAN_CASE(0.0L, -inf,   pi,  0x0220u, "atan2(+0,-inf)=+pi");
    ATAN_CASE(inf,   inf,   pi4, 0x0220u, "atan2(+inf,+inf)=pi/4");
    ATAN_CASE(inf,  -inf,   p34, 0x0220u, "atan2(+inf,-inf)=3pi/4");
    ATAN_CASE(inf,   1.0L,  pi2, 0x0220u, "atan2(+inf,1)=pi/2");
#undef ATAN_CASE
}

/* F2XM1 domain edge: |x|>1 returns the SOURCE with PE; +-inf specials */
static void v_trans_f2xm1_domain(void) {
    static const uint8_t x2m1_p[] = {0xBB,0x00,0x30, 0xDB,0x2F, 0xD9,0xF0, 0xDB,0x3F, 0xF4};
    uint8_t src[10];
    long double inf = mk_val(0x7FFF, 0x8000000000000000ull);
    long double m15 = -1.5L;

    setup_machine(&g); put_val(0x3000, 1.5L); to80(1.5L, src);
    run_program(&g, x2m1_p, sizeof x2m1_p, 0, 16);
    CHECK(mem_cmp(0x3000, src), "f2xm1(1.5)=1.5 (source, host quirk)");
    CHECK(g.cpu.fsw == 0x0020u, "f2xm1(1.5) PE only FSW=%04x", g.cpu.fsw);

    setup_machine(&g); put_val(0x3000, 2.0L); to80(2.0L, src);
    run_program(&g, x2m1_p, sizeof x2m1_p, 0, 16);
    CHECK(mem_cmp(0x3000, src), "f2xm1(2)=2 (source)");
    CHECK(g.cpu.fsw == 0x0020u, "f2xm1(2) PE only");

    setup_machine(&g); put_val(0x3000, m15); to80(m15, src);
    run_program(&g, x2m1_p, sizeof x2m1_p, 0, 16);
    CHECK(mem_cmp(0x3000, src), "f2xm1(-1.5)=-1.5 (source)");

    setup_machine(&g); put_val(0x3000, inf);
    run_program(&g, x2m1_p, sizeof x2m1_p, 0, 16);
    to80(inf, src); CHECK(mem_cmp(0x3000, src), "f2xm1(+inf)=+inf");
    CHECK(g.cpu.fsw == 0, "f2xm1(+inf) quiet");

    setup_machine(&g); put_val(0x3000, -inf);
    run_program(&g, x2m1_p, sizeof x2m1_p, 0, 16);
    { uint8_t want[10] = {0,0,0,0,0,0,0,0x80,0xFF,0xBF};  /* -1.0 */
      CHECK(mem_cmp(0x3000, want), "f2xm1(-inf)=-1.0"); }
    CHECK(g.cpu.fsw == 0, "f2xm1(-inf) quiet");
}

/* C-flag write policy (transprobe6/7): the ops WRITE C1 (and the trig
 * four write C2); C0/C3 -- and C2 on the rest -- survive from before. */
static void v_trans_cflags(void) {
    /* preset C0=C2=C3=1 + IE via FTST(qnan), then fsin(0.5) */
    static const uint8_t ftst_sin[] = {
        0xBB,0x00,0x30,
        0xDB,0x2F,              /* fld qnan */
        0xD9,0xE4,              /* ftst -> C0=C2=C3=1, IE */
        0xDD,0xD8,              /* fstp st(0) */
        0x83,0xC3,0x10,
        0xDB,0x2F,              /* fld 0.5 */
        0xD9,0xFE,              /* fsin */
        0xDB,0x3F, 0xF4};
    setup_machine(&g);
    put_val(0x3000, mk_val(0x7FFF, 0xC000000000000000ull));
    put_val(0x3010, 0.5L);
    run_program(&g, ftst_sin, sizeof ftst_sin, 0, 16);
    CHECK(fsw_c0() == 1 && fsw_c3() == 1, "fsin preserves C0/C3");
    CHECK(fsw_c2() == 0, "fsin WRITES C2 (reduction complete)");
    CHECK(fsw_c1() == 1, "fsin WRITES C1=1 (rounded up)");
    CHECK((g.cpu.fsw & 0x021u) == 0x021u, "sticky IE + PE");

    /* same preset, then f2xm1(0.5): C2 must SURVIVE (not written) */
    static const uint8_t ftst_2xm1[] = {
        0xBB,0x00,0x30,
        0xDB,0x2F,
        0xD9,0xE4,
        0xDD,0xD8,
        0x83,0xC3,0x10,
        0xDB,0x2F,
        0xD9,0xF0,
        0xDB,0x3F, 0xF4};
    setup_machine(&g);
    put_val(0x3000, mk_val(0x7FFF, 0xC000000000000000ull));
    put_val(0x3010, 0.5L);
    run_program(&g, ftst_2xm1, sizeof ftst_2xm1, 0, 16);
    CHECK(fsw_c2() == 1, "f2xm1 preserves C2");
    CHECK(fsw_c0() == 1 && fsw_c3() == 1, "f2xm1 preserves C0/C3");
    CHECK(fsw_c1() == 0, "f2xm1 WRITES C1=0");
}

/* FBSTP rounding matrix + invalid forms (bcdprobe/transprobe3) */
static void v_bcd_rounding(void) {
    static const uint8_t st[] = {0xBB,0x00,0x30, 0xDB,0x2F, 0x83,0xC3,0x10, 0xDF,0x37, 0xF4};
    struct { const char *what; long double v; uint16_t rc; uint8_t dig[10]; int pe, c1; }
    T[] = {
        {"1.5 near",  1.5L,  0x037F, {0x02,0,0,0,0,0,0,0,0,0x00}, 1, 1},
        {"1.5 down",  1.5L,  0x077F, {0x01,0,0,0,0,0,0,0,0,0x00}, 1, 0},
        {"1.5 up",    1.5L,  0x0B7F, {0x02,0,0,0,0,0,0,0,0,0x00}, 1, 1},
        {"1.5 zero",  1.5L,  0x0F7F, {0x01,0,0,0,0,0,0,0,0,0x00}, 1, 0},
        {"2.5 near",  2.5L,  0x037F, {0x02,0,0,0,0,0,0,0,0,0x00}, 1, 0},
        {"-1.5 near", -1.5L, 0x037F, {0x02,0,0,0,0,0,0,0,0,0x80}, 1, 1},
        {"-1.5 down", -1.5L, 0x077F, {0x02,0,0,0,0,0,0,0,0,0x80}, 1, 1},
        {"-1.5 up",   -1.5L, 0x0B7F, {0x01,0,0,0,0,0,0,0,0,0x80}, 1, 0},
        {"-0.9 up",   -0.9L, 0x0B7F, {0x00,0,0,0,0,0,0,0,0,0x80}, 1, 0},
        {"1234",      1234.0L, 0x037F, {0x34,0x12,0,0,0,0,0,0,0,0x00}, 0, 0},
    };
    for (unsigned i = 0; i < sizeof T / sizeof T[0]; i++) {
        setup_machine(&g);
        g.cpu.fcw = T[i].rc;
        put_val(0x3000, T[i].v);
        run_program(&g, st, sizeof st, 0, 16);
        CHECK(!g.cpu.fault, "%s fault", T[i].what);
        CHECK(mem_cmp(0x3010, T[i].dig), "%s digits", T[i].what);
        CHECK(((g.cpu.fsw >> 5) & 1u) == (unsigned)T[i].pe, "%s PE FSW=%04x", T[i].what, g.cpu.fsw);
        CHECK(fsw_c1() == (unsigned)T[i].c1, "%s C1 FSW=%04x", T[i].what, g.cpu.fsw);
        CHECK(g.cpu.ftop == 0, "%s pops", T[i].what);
    }
}

static void v_bcd_invalid(void) {
    static const uint8_t st[] = {0xBB,0x00,0x30, 0xDB,0x2F, 0x83,0xC3,0x10, 0xDF,0x37, 0xF4};
    static const uint8_t ld[] = {0xBB,0x00,0x30, 0xDF,0x27, 0xF4};
    uint8_t indef[10]; memcpy(indef, K_INDEF, 10);

    /* value too big (rounds to >= 1e18): indefinite + IE, still pops */
    setup_machine(&g); put_val(0x3000, 1.0e18L);
    run_program(&g, st, sizeof st, 0, 16);
    CHECK(mem_cmp(0x3010, indef), "fbstp 1e18 indefinite");
    CHECK(g.cpu.fsw == 0x0001u, "fbstp 1e18 IE only FSW=%04x", g.cpu.fsw);
    CHECK(g.cpu.ftop == 0, "fbstp pops on invalid");

    setup_machine(&g); put_val(0x3000, mk_val(0x7FFF, 0xC000000000000000ull));
    run_program(&g, st, sizeof st, 0, 16);
    CHECK(mem_cmp(0x3010, indef), "fbstp qnan indefinite");
    CHECK(g.cpu.fsw == 0x0001u, "fbstp qnan IE");

    /* negative zero keeps its sign */
    setup_machine(&g); put_val(0x3000, -0.0L);
    run_program(&g, st, sizeof st, 0, 16);
    CHECK((uint8_t)mem_read(&g, 0x3019, 1) == 0x80u, "fbstp -0 sign");
    CHECK(g.cpu.fsw == 0, "fbstp -0 quiet");

    /* FBLD: nibble values count 10..15, sign = bit7 only */
    setup_machine(&g);
    mem_write(&g, 0x3000, 1, 0x0A);           /* digits: 10 */
    run_program(&g, ld, sizeof ld, 0, 16);
    CHECK(g.cpu.fst[g.cpu.ftop] == 10.0L, "fbld nibble A = 10");

    setup_machine(&g);
    mem_write(&g, 0x3000, 1, 0x01); mem_write(&g, 0x3009, 1, 0x84);  /* -1 */
    run_program(&g, ld, sizeof ld, 0, 16);
    CHECK(g.cpu.fst[g.cpu.ftop] == -1.0L, "fbld sign bit7 (0x84)");
}

int main(void) {
    v_fld1_no_fcw_clobber();
    v_fld_constants();
    v_fld_fstp_mem();
    v_rounding_rc();
    v_cw_sw();
    v_fninit();
    v_fnclex();
    v_stack_ops();
    v_stack_faults();
    v_status_ops();
    v_int_conv();
    v_bcd();
    v_fcmov();
    v_nm_trap();
    v_em_ud();
    v_mf_trap();
    v_fxsave_live_layout();
    v_fnsave_frstor();
    v_env();
    v_transcendental_boundary();
    v_add_sub_dir();
    v_mul_div_dir();
    v_div_exceptions();
    v_sqrt_rndint();
    v_prem_flags();
    v_scale_xtract();
    v_com_matrix();
    v_comi_eflags();
    v_ficom_int();
    v_denorm_de();
    v_d6_flag_map();
    v_trans_results();
    v_trans_huge_c2();
    v_trans_nan_inf();
    v_trans_stack();
    v_trans_l2x_corner();
    v_trans_l2xp1_domain();
    v_trans_fpatan_quadrants();
    v_trans_f2xm1_domain();
    v_trans_cflags();
    v_bcd_rounding();
    v_bcd_invalid();

    if (fails) { fprintf(stderr, "test-fpu: %d FAILURES\n", fails); return 1; }
    puts("test-fpu: ok");
    return 0;
}
