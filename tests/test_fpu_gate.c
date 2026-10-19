/* tests/test_fpu_gate.c -- FPU F4: integration gates (plan: F4 row).
 *
 * Two lanes, both live here because both close the plan's F4:
 *
 *  1. --kernel= freestanding payload receipts pair (determinism x2):
 *     the kloader core the CLI --kernel= flag drives boots
 *     tests/fpu_payload.elf twice on fresh machines; the COM1 receipts
 *     must be byte-identical between the runs AND equal the pinned
 *     receipt content (constants, arithmetic, the FSIN host payload,
 *     FBSTP/FBLD bytes, FSW bits -- the F0-F3 vectors, now produced
 *     inside a real guest).
 *
 *  2. snapshot byte-stability of the x87 file: an x87 tour (the surface
 *     test_fpu.c pins) runs to a fixed step count on fresh machines and
 *     is snap_save'd; the snapshot files must be byte-identical. The
 *     fst slots' 6 padding bytes ride the raw cpu_t dump inside
 *     machine_rec -- that garbage is the trap this lane exists to pin.
 *     save -> load -> save must be a fixed point on the same file.
 */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "machine.h"
#include "cpu.h"
#include "platform.h"
#include "kloader.h"
#include "snapshot.h"
#include "harness.h"

#define PAYLOAD "tests/fpu_payload.elf"

static int fails;
#define EXPECT(cond, ...) do { if (!(cond)) { printf("FAIL " __VA_ARGS__); printf("\n"); fails++; } } while (0)

/* COM1 byte capture (the test's own sink; devices.c's line-logger is not
 * wired in the kload lane) */
static char   g_ser[256];
static size_t g_ser_n;

static uint32_t ser_read(void *ctx, uint16_t port, int size) {
    (void)ctx; (void)port; (void)size;
    return 0x20;
}
static void ser_write(void *ctx, uint16_t port, int size, uint32_t val) {
    (void)ctx; (void)size;
    if (port == 0x3F8 && g_ser_n < sizeof(g_ser) - 1)
        g_ser[g_ser_n++] = (char)val;
}

static machine_t g_m;
static void fx_init(void) {
    setup_machine(&g_m);
    g_m.fb_w = 800; g_m.fb_h = 600;   /* what devices_init_common() sets */
    io_register(&g_m, 0x3F8, 1, ser_read, ser_write, NULL, "COM1-capture");
    g_ser_n = 0;
}

/* the pinned receipt content, measured host-side (F0/F3 protocol) and
 * reproduced by the guest payload: FNINIT-anchored, so only the payload
 * bytes move between hosts -- the FSW pins assume x87 arithmetic */
static const char GOLDEN[] =
    "F4R0=3800\n"
    "F4R1=0000000000000080ff3f\n"
    "F4R2=35c26821a2da0fc90040\n"
    "F4R3=447f2f58a24377f5fd3f\n"
    "F4R4=3a20\n"
    "F4R5=34120000000000000000\n"
    "F4R6=00000000000000800040\n"
    "F4R7=000000000000409a0940\n";

static void run_payload(char *out, size_t n) {
    fx_init();
    assert(kload_boot(&g_m, PAYLOAD, NULL) == 0);
    machine_t *m = &g_m;
    for (int i = 0; i < 20000 && !m->cpu.halted && !m->cpu.fault; i++)
        cpu_step(&m->cpu);
    assert(!m->cpu.fault);
    assert(m->cpu.halted);
    g_ser[g_ser_n] = 0;
    assert(g_ser_n < n);
    memcpy(out, g_ser, g_ser_n + 1);
}

/* ---- lane 2: the x87 tour program (real mode at physical 0) ----
 * constants -> arith -> the F2/F3 surface -> compares/stack ops ->
 * env save/restore -> HLT, leaving the fst array full of computed
 * values so the snapshot's raw cpu_t dump carries them. */
static const uint8_t tour[] = {
    0xDB, 0xE3,          /* fninit         */
    0xD9, 0xE8,          /* fld1           */
    0xD9, 0xEB,          /* fldpi          */
    0xD9, 0xE9,          /* fldl2t         */
    0xD9, 0xEA,          /* fldl2e         */
    0xD9, 0xEC,          /* fldlg2         */
    0xD9, 0xED,          /* fldln2         */
    0xD9, 0xEE,          /* fldz           */
    0xD8, 0xC1,          /* fadd st0,st1   */
    0xD8, 0xE1,          /* fsub st0,st1   */
    0xD8, 0xC9,          /* fmul st0,st1   */
    0xD8, 0xF1,          /* fdiv st0,st1   */
    0xD9, 0xFB,          /* fsincos        */
    0xD9, 0xFE,          /* fsin           */
    0xD9, 0xFF,          /* fcos           */
    0xD9, 0xF2,          /* fptan          */
    0xD9, 0xF3,          /* fpatan         */
    0xD9, 0xFA,          /* fsqrt          */
    0xD9, 0xF8,          /* fprem          */
    0xD9, 0xFC,          /* frndint        */
    0xD9, 0xF0,          /* f2xm1          */
    0xD9, 0xF1,          /* fyl2x          */
    0xD9, 0xF9,          /* fyl2xp1        */
    0xD9, 0xC9,          /* fxch st1       */
    0xDF, 0xE0,          /* fnstsw ax      */
    0xBB, 0x00, 0x31,    /* mov bx,0x3100  */
    0xDB, 0x2F,          /* fld m80 [bx]   */
    0xDB, 0x3F,          /* fstp m80 [bx]  */
    0xDF, 0x27,          /* fbld [bx]      */
    0xDF, 0x37,          /* fbstp [bx]     */
    0xD9, 0x37,          /* fnstenv [bx]   */
    0xDD, 0x37,          /* fnsave [bx]    */
    0xDD, 0x27,          /* frstor [bx]    */
    0x0F, 0xAE, 0x07,    /* fxsave [bx]    */
    0x0F, 0xAE, 0x0F,    /* fxrstor [bx]   */
    0xF4                 /* hlt            */
};

static void boot_tour(machine_t *m) {
    setup_machine(m);
    cpu_devtick_set(0);   /* a fresh process starts the device cadence at 0 */
    memcpy(m->ram, tour, sizeof tour);
    m->cpu.gpr[RSP] = 0x8000;
}
static void run_tour(machine_t *m, uint64_t n) {
    while (m->run_steps < n) {
        if (cpu_step(&m->cpu) != 0) break;
        m->run_steps++;
    }
}

static int same_file(const char *a, const char *b) {
    FILE *x = fopen(a, "rb"), *y = fopen(b, "rb");
    int ok = x && y;
    while (ok) {
        int c = fgetc(x), d = fgetc(y);
        if (c != d) ok = 0;
        if (c == EOF || d == EOF) break;
    }
    if (x) fclose(x);
    if (y) fclose(y);
    return ok;
}
/* name the first bytes that differ (diagnostics for EXPECT) */
static void file_diff(const char *a, const char *b) {
    FILE *x = fopen(a, "rb"), *y = fopen(b, "rb");
    long off = 0;
    if (!x || !y) { printf("  (diff: missing file)\n"); if (x) fclose(x); if (y) fclose(y); return; }
    for (;;) {
        int c = fgetc(x), d = fgetc(y);
        if (c != d) {
            printf("  first difference at offset %ld (%s=%02x %s=%02x)\n",
                   off, a, c == EOF ? 0 : (unsigned)c, b, d == EOF ? 0 : (unsigned)d);
            break;
        }
        if (c == EOF) { printf("  (identical)\n"); break; }
        off++;
    }
    fclose(x); fclose(y);
}

static const snap_cfg_t CFG = { "Haswell", 0, 0 };
static const char *PA = "/tmp/fpu-gate-a.snap";
static const char *PB = "/tmp/fpu-gate-b.snap";
static const char *PC = "/tmp/fpu-gate-c.snap";

int main(void) {
    /* ================= lane 1: receipts pair (determinism x2) ========== */
    static char r1[256], r2[256];
    int f0;
    run_payload(r1, sizeof r1);
    run_payload(r2, sizeof r2);
    f0 = fails;
    EXPECT(strcmp(r1, r2) == 0,
           "payload receipts pair: two fresh-machine runs differ:\n  run1=%s\n  run2=%s", r1, r2);
    EXPECT(strcmp(r1, GOLDEN) == 0,
           "payload receipts content:\n  got=%s\n  want=%s", r1, GOLDEN);
    if (fails == f0)
        printf("ok --kernel= payload receipts: pair identical x2, content pinned\n");

    /* ================= lane 2: x87 file byte-stability ================= */
    {
        /* static: machine_t is ~2.8MB -- three of them overflow the stack */
        static machine_t A, B, C;
        f0 = fails;
        boot_tour(&A);
        run_tour(&A, 200);
        EXPECT(A.cpu.halted, "tour must reach HLT (steps=%llu fault=%d rip=%llx msg=%s)",
               (unsigned long long)A.run_steps, (int)A.cpu.fault,
               (unsigned long long)A.cpu.fault_rip, A.cpu.fault_msg);
        EXPECT(snap_save(&A, PA, &CFG) == 0, "snap_save A");

        boot_tour(&B);
        run_tour(&B, 200);
        EXPECT(snap_save(&B, PB, &CFG) == 0, "snap_save B");
        EXPECT(same_file(PA, PB),
               "x87 file byte-stability: fresh-run snapshots differ (fst padding?)");

        boot_tour(&C);
        EXPECT(snap_load(&C, PA, &CFG) == 0, "snap_load A into C");
        EXPECT(C.run_steps == A.run_steps, "load restores run_steps (%llu vs %llu)",
               (unsigned long long)C.run_steps, (unsigned long long)A.run_steps);
        /* the CLI's load+save lane executes nothing: --max-instr lands on the
         * restored run_steps budget and the parked/halted vCPU never steps
         * (t4_snapshot_lanes.sh's round trip). Do not call run_tour here --
         * even a no-op cpu_step on a halted machine advances the device
         * cadence (s_devtick), which the file pins. */
        EXPECT(snap_save(&C, PC, &CFG) == 0, "snap_save C");
        EXPECT(same_file(PA, PC), "x87 file: save -> load -> save fixed point");
        if (!same_file(PA, PC)) { printf("  resave differs from A:\n"); file_diff(PA, PC); }
        if (fails == f0)
            printf("ok snapshot x87 file: byte-identical across fresh runs; save->load->save fixed point\n");
        if (!fails) { remove(PA); remove(PB); remove(PC); }
    }

    if (fails) { printf("test-fpu-gate: %d FAILED\n", fails); return 1; }
    puts("test-fpu-gate: all vectors passed");
    return 0;
}
