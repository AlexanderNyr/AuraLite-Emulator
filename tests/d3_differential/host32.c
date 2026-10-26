/* d3_differential/host32.c -- host reference for BOUND / LES / LDS.
 *
 * Prints one canonical line per case; emu.c prints the same lines from the
 * emulated core in 32-bit protected mode, so verification is `diff`.
 */
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <signal.h>
#include <setjmp.h>

static sigjmp_buf jb;
static void on_sig(int s) { (void)s; siglongjmp(jb, 1); }

extern void ref_bound32(int32_t idx, const int32_t *bounds);
extern void ref_bound16(int16_t idx, const int16_t *bounds);
extern void ref_les32(const void *farptr);
extern void ref_lds32(const void *farptr);
extern uint32_t g_eax;
extern uint16_t g_es, g_ds2;
extern uint16_t read_es(void);
extern uint16_t read_ds(void);
extern uint16_t g_saved_ds;

typedef struct { uint32_t off; uint16_t sel; } farptr_t;

int main(void) {
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = on_sig;
    sigaction(SIGSEGV, &sa, NULL);
    sigaction(SIGBUS, &sa, NULL);

    static int32_t b32[2];
    static int16_t b16[2];
    static const int32_t IDX32[] = { -2147483647 - 1, -101, -100, -11, -5, -1, 0,
                                     1, 5, 10, 11, 100, 2147483647 };
    /* windows chosen to exercise both edges of the signed compare */
    static const int32_t WIN[][2] = { {0, 10},     {-5, 5}, {-10, -5},
                                      {5, 5},      {10, 0},
                                      {-2147483647 - 1, 2147483647} };
    /* BOUND: measured, but NOT used as a reference.  This sandbox's 32-bit
     * BOUND is broken -- every attempt, in range or not, dies with
     * SEGV_MAPERR at the instruction -- so the emulated BOUND is pinned by
     * the SDM rule plus tests/test_table.c vectors instead (see README). */
    int bound_probe_faults = 0;
    b32[0] = 0; b32[1] = 10;
    if (sigsetjmp(jb, 1) == 0) ref_bound32(5, b32); else bound_probe_faults = 1;
    if (bound_probe_faults) fprintf(stderr, "note: host BOUND unusable here\n");
    (void)b16; (void)IDX32; (void)WIN;

    /* LES/LDS: a flat data selector (ours), the null selector, a selector
     * past the GDT limit, and a code selector.  The host can only accept a
     * real data descriptor, which is exactly the boundary being pinned. */
    uint16_t ds = read_ds();              /* 0x2b on i386 Linux, but measured */
    g_saved_ds = ds;
    uint16_t cs = read_es();              /* ES == DS; kept for the code case */
    (void)cs;
    static const uint16_t SELS[] = { 0x0023 /*code*/, 0x0000 /*null*/,
                                     0x00F8 /*past GDT*/, 0xFFFF };
    static farptr_t fp;
    for (size_t i = 0; i <= sizeof SELS / sizeof *SELS; i++) {
        uint16_t sel = (i == 0) ? ds : SELS[i - 1];
        fp.off = 0x11223344u; fp.sel = sel;
        g_eax = 0;
        volatile int fault = 0;
        if (sigsetjmp(jb, 1) == 0) ref_les32(&fp);
        else fault = 1;
        printf("les    sel=%04x -> fault=%d eax=%08x es=%s\n",
               (unsigned)sel, fault, (unsigned)g_eax,
               fault ? "----" : "ok");   /* the register is stale after a
                                          * fault; the emulator prints the
                                          * same placeholder */
    }
    for (size_t i = 0; i <= sizeof SELS / sizeof *SELS; i++) {
        uint16_t sel = (i == 0) ? ds : SELS[i - 1];
        fp.off = 0x55667788u; fp.sel = sel;
        g_eax = 0;
        volatile int fault = 0;
        if (sigsetjmp(jb, 1) == 0) ref_lds32(&fp);
        else fault = 1;
        printf("lds    sel=%04x -> fault=%d eax=%08x ds=%s\n",
               (unsigned)sel, fault, (unsigned)g_eax, fault ? "----" : "ok");
    }
    return 0;
}
