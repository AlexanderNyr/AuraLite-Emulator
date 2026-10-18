/* tests/test_dbg.c -- TOOLING T2: run-control core vectors.
 *
 * A scripted front end drives the core exactly like the REPL does (one
 * command per service call, resume on the command's action) and records
 * every stop structurally, so the vectors assert exact stop RIPs and exact
 * executed-instruction counts rather than transcript text.
 *
 * Program (real mode, CS=0, IF=0 so the final HLT ends the run):
 *   00 B9 05 00      mov cx,5
 *   03 89 0E 00 02   mov [0x200],cx        <- watch target
 *   07 E8 16 00      call 0x20
 *   0A 49            dec cx
 *   0B 75 F6         jnz 0x03
 *   0D F4            hlt
 *   20 42            inc dx
 *   21 8B 1E 00 02   mov bx,[0x200]        <- read-watch target
 *   25 C3            ret
 * 1 + 5 x 7 + 1 = 37 instructions (each loop: store, call, inc, load,
 * ret, dec, jnz).
 */
#include <stdio.h>
#include <string.h>
#include <assert.h>
#include "harness.h"
#include "cpu.h"
#include "dbg.h"

static machine_t g_m;
static int fails;
#define EXPECT(cond, ...) do { if (!(cond)) { printf("FAIL " __VA_ARGS__); printf("\n"); fails++; } } while (0)

static const uint8_t prog_lo[] = { 0xB9, 0x05, 0x00, 0x89, 0x0E, 0x00, 0x02, 0xE8, 0x16, 0x00,
                                   0x49, 0x75, 0xF6, 0xF4 };
static const uint8_t prog_fn[] = { 0x42, 0x8B, 0x1E, 0x00, 0x02, 0xC3 };

typedef struct {
    dbg_frontend_t fe;
    const char *const *cmds;
    int ncmds, next;
    dbg_stop_t st[64];
    uint64_t rip[64], steps[64];
    int nst;
    FILE *out;
} tfe_t;

static void t_on_stop(machine_t *m, dbg_frontend_t *fe, const dbg_stop_t *st) {
    tfe_t *t = fe->ctx;
    if (t->nst < 64) {
        t->st[t->nst] = *st;
        t->rip[t->nst] = m->cpu.rip;
        t->steps[t->nst] = m->cpu.executed_steps;
        t->nst++;
    }
}
static int t_fd(machine_t *m, dbg_frontend_t *fe) { (void)m; (void)fe; return -1; }
static void t_service(machine_t *m, dbg_frontend_t *fe) {
    tfe_t *t = fe->ctx;
    if (t->next >= t->ncmds) { dbg_resume(m, DBG_ACT_DETACH, 0, -1); dbg_remove_frontend(m, fe); return; }
    int act = dbg_exec(m, t->cmds[t->next++], t->out);
    if (act != DBG_ACT_NONE) dbg_resume(m, (dbg_action_t)act, 0, -1);
}

static void load(void) {
    setup_machine(&g_m);
    memcpy(g_m.ram, prog_lo, sizeof prog_lo);
    memcpy(g_m.ram + 0x20, prog_fn, sizeof prog_fn);
}

/* the run loop main() uses, minus devices */
static int run(int max) {
    int rc = 0;
    for (int i = 0; i < max && !g_m.stop_requested; i++) {
        rc = cpu_step(&g_m.cpu);
        if (rc != 0) break;
        g_m.run_steps++;
        if (g_m.dbg) dbg_post_step(&g_m);
    }
    return rc;
}

static void attach(tfe_t *t, const char *const *cmds, int n) {
    memset(t, 0, sizeof *t);
    t->cmds = cmds; t->ncmds = n;
    t->out = fopen("/dev/null", "w");
    t->fe.name = "test"; t->fe.ctx = t;
    t->fe.on_stop = t_on_stop; t->fe.stop_fd = t_fd; t->fe.service = t_service;
    t->fe.wants_end_stop = 1;
    assert(dbg_add_frontend(&g_m, &t->fe) == 0);
}

static void finish(tfe_t *t, int rc) {
    dbg_run_end(&g_m, rc);
    dbg_detach(&g_m);
    fclose(t->out);
}

/* reference: the same program without any tool attached */
static uint64_t ref_steps;
static uint64_t ref_gpr[16];
static void reference(void) {
    load();
    run(1000);
    ref_steps = g_m.cpu.executed_steps;
    memcpy(ref_gpr, g_m.cpu.gpr, sizeof ref_gpr);
}

static void vectors_run_control(void) {
    static const char *const cmds[] = {
        "break 0x0a",            /* stop 0: START */
        "continue",              /* -> stop 1: BREAK at 0x0a after 6 */
        "continue",              /* -> stop 2: BREAK at 0x0a after 13 (skip-once) */
        "stepi 3",               /* -> stop 3: STEP at 0x07 after 16 */
        "delete",
        "nexti",                 /* -> stop 4: STEP at 0x0a after 20 (callee ran) */
        "watch w 0x200 2",
        "continue",              /* -> stop 5: WATCH after the store: rip 0x07, 23 */
        "stepi 1",               /* -> stop 6: STEP from a between-instruction stop: */
                                 /*    exactly one instruction (the call): 0x20, 24 */
        "delete",
        "rwatch 0x200 2",
        "continue",              /* -> stop 7: WATCH (r) after the load: rip 0x25, 26 */
        "delete",
        "until +3",              /* -> stop 8: UNTIL after 29 (jnz taken: rip 0x03) */
        "set $rax = 0x1122334455667788",
        "set $eax = 0x5",        /* 32-bit write zero-extends */
        "set $rax = 0",
        "continue",              /* -> stop 9: END (hlt, IF=0) after 37 */
    };
    load();
    tfe_t t;
    attach(&t, cmds, (int)(sizeof cmds / sizeof cmds[0]));
    dbg_run_start(&g_m, 1);
    int rc = run(1000);
    uint64_t rax_after = g_m.cpu.gpr[RAX];
    finish(&t, rc);
    static const struct { dbg_stop_reason_t r; uint64_t rip, steps; } want[] = {
        { DBG_STOP_START, 0x00, 0 },  { DBG_STOP_BREAK, 0x0a, 6 },  { DBG_STOP_BREAK, 0x0a, 13 },
        { DBG_STOP_STEP, 0x07, 16 },  { DBG_STOP_STEP, 0x0a, 20 },  { DBG_STOP_WATCH, 0x07, 23 },
        { DBG_STOP_STEP, 0x20, 24 },  { DBG_STOP_WATCH, 0x25, 26 }, { DBG_STOP_UNTIL, 0x03, 29 },
        { DBG_STOP_END, 0x0e, 37 },
    };
    EXPECT(t.nst == 10, "run control: %d stops, want 10", t.nst);
    for (int i = 0; i < t.nst && i < 10; i++)
        EXPECT(t.st[i].reason == want[i].r && t.rip[i] == want[i].rip && t.steps[i] == want[i].steps,
               "stop %d: reason %d rip %llx steps %llu, want reason %d rip %llx steps %llu", i,
               t.st[i].reason, (unsigned long long)t.rip[i], (unsigned long long)t.steps[i],
               want[i].r, (unsigned long long)want[i].rip, (unsigned long long)want[i].steps);
    EXPECT(t.st[5].watch_kind == 'w' && t.st[5].watch_addr == 0x200 && t.st[5].watch_old == 3 &&
           t.st[5].watch_new == 2, "write watch: kind %c addr %llx old %llx new %llx", t.st[5].watch_kind,
           (unsigned long long)t.st[5].watch_addr, (unsigned long long)t.st[5].watch_old,
           (unsigned long long)t.st[5].watch_new);
    EXPECT(t.st[7].watch_kind == 'r' && t.st[7].watch_addr == 0x200, "read watch");
    EXPECT(rax_after == 0, "set $rax");
    /* non-perturbation: identical instruction count and registers */
    EXPECT(g_m.cpu.executed_steps == ref_steps, "steps %llu vs reference %llu",
           (unsigned long long)g_m.cpu.executed_steps, (unsigned long long)ref_steps);
    EXPECT(!memcmp(g_m.cpu.gpr, ref_gpr, sizeof ref_gpr), "registers differ from the reference run");
    printf("ok run control: 10 exact stops (start, break x2, stepi 3, nexti over a call, write watch\n"
           "   old=3 new=2, stepi 1 from a between-instruction stop, read watch, until +3, end);\n"
           "   final state == plain run\n");
}

/* a breakpoint on every instruction address must not perturb the run */
static void vectors_bp_everywhere(void) {
    static const char *const cmds[] = {
        "break 0x00", "break 0x03", "break 0x07", "break 0x0a", "break 0x0b", "break 0x0d",
        "break 0x20", "break 0x21", "break 0x25",
        "c","c","c","c","c","c","c","c","c","c","c","c","c","c","c","c","c","c","c","c",
        "c","c","c","c","c","c","c","c","c","c","c","c","c","c","c","c","c","c","c","c",
    };
    load();
    tfe_t t;
    attach(&t, cmds, (int)(sizeof cmds / sizeof cmds[0]));
    dbg_run_start(&g_m, 1);
    int rc = run(1000);
    int nbreak = 0;
    for (int i = 0; i < t.nst; i++) nbreak += t.st[i].reason == DBG_STOP_BREAK;
    finish(&t, rc);
    /* stop 0 is START at 0x00 where bp 1 sits: the first boundary is skipped
     * (skip-once), so 36 of the 37 instructions stop */
    EXPECT(nbreak == 36, "breakpoint stops %d, want 36", nbreak);
    EXPECT(g_m.cpu.executed_steps == ref_steps && !memcmp(g_m.cpu.gpr, ref_gpr, sizeof ref_gpr),
           "a breakpoint on every address perturbed the run");
    printf("ok breakpoints on every address: %d stops, run identical to the plain run\n", nbreak);
}

static void vectors_quit_pause_eval(void) {
    /* quit at a boundary abandons the step: nothing more executes */
    static const char *const q[] = { "break 0x0a", "continue", "quit" };
    load();
    tfe_t t;
    attach(&t, q, 3);
    dbg_run_start(&g_m, 1);
    run(1000);
    EXPECT(g_m.stop_requested == 1 && g_m.cpu.executed_steps == 6 && g_m.cpu.rip == 0x0a,
           "quit: stop_requested %d steps %llu rip %llx", g_m.stop_requested,
           (unsigned long long)g_m.cpu.executed_steps, (unsigned long long)g_m.cpu.rip);
    finish(&t, 0);

    /* an async pause request stops at the next post-step site */
    static const char *const pz[] = { "continue" };
    load();
    attach(&t, pz, 1);
    for (int i = 0; i < 3; i++) { cpu_step(&g_m.cpu); dbg_post_step(&g_m); }
    dbg_request_pause(&g_m);
    cpu_step(&g_m.cpu); dbg_post_step(&g_m);
    EXPECT(t.nst == 1 && t.st[0].reason == DBG_STOP_PAUSE && t.steps[0] == 4, "pause: %d stops", t.nst);
    finish(&t, 0);

    /* expressions and registers */
    load();
    dbg_attach(&g_m);
    g_m.cpu.gpr[RCX] = 0x40; g_m.cpu.gpr[RSP] = 0x8000;
    uint64_t v; const char *err;
    EXPECT(dbg_eval(&g_m, "0x10+2*3", &v, &err) && v == 0x16, "eval 0x10+2*3 = %llx", (unsigned long long)v);
    EXPECT(dbg_eval(&g_m, "(1+2)*3", &v, &err) && v == 9, "eval (1+2)*3");
    EXPECT(dbg_eval(&g_m, "$rcx+$rsp-8", &v, &err) && v == 0x8038, "eval regs");
    EXPECT(dbg_eval(&g_m, "-1", &v, &err) && v == ~0ULL, "eval -1");
    g_m.ram[0x8000] = 0xEF; g_m.ram[0x8001] = 0xBE;
    EXPECT(dbg_eval(&g_m, "*$rsp", &v, &err) && (v & 0xFFFF) == 0xBEEF, "eval *$rsp");
    EXPECT(!dbg_eval(&g_m, "nosuchsym", &v, &err) && err, "unknown symbol must fail");
    EXPECT(!dbg_eval(&g_m, "1+", &v, &err), "dangling operator must fail");
    g_m.cpu.gpr[RAX] = 0xFFFFFFFFFFFFFFFFull;
    dbg_reg_set(&g_m, 0, "ax", 0x1234);
    EXPECT(g_m.cpu.gpr[RAX] == 0xFFFFFFFFFFFF1234ull, "set ax keeps the upper bits");
    dbg_reg_set(&g_m, 0, "eax", 0x5);
    EXPECT(g_m.cpu.gpr[RAX] == 5, "set eax zero-extends");
    EXPECT(dbg_reg_get(&g_m, 0, "lin", &v) && v == 0, "lin = CS.base + RIP");
    dbg_detach(&g_m);
    printf("ok quit abandons the step, pause stops at the next boundary, expressions, registers\n");
}

/* A1: the debugger's LINEAR address.  $rip is the raw instruction pointer
 * and $lin is cs.base + rip; in real mode with a non-zero CS base the two
 * differ, and the confusion that caused was the reason $lin became a
 * first-class name and the `regs` listing grew a linear-address row.
 *
 * The vector pins three things:
 *   1. the register arithmetic ($lin == cs.base + rip, $rip unchanged),
 *   2. that `x/i` with NO address reads the LINEAR address (so it finds the
 *      code the CPU is actually executing), and
 *   3. that `x/i $rip` really is the raw IP -- a different byte, because
 *      the two addresses are different places in memory.
 */
static char *exec_capture(machine_t *m, const char *cmd) {
    FILE *f = tmpfile();
    if (!f) return NULL;
    dbg_exec(m, cmd, f);
    fflush(f);
    long n = ftell(f);
    if (n < 0) n = 0;
    char *buf = calloc(1, (size_t)n + 1);
    if (!buf) { fclose(f); return NULL; }
    rewind(f);
    if (n > 0 && fread(buf, 1, (size_t)n, f) != (size_t)n) { /* short read: keep what we got */ }
    buf[n] = 0;
    fclose(f);
    return buf;
}

static void vectors_linear_address(void) {
    load();
    dbg_attach(&g_m);
    uint64_t v; const char *err;

    /* 1. the arithmetic, on the reset vector's real-mode CS base */
    g_m.cpu.seg[SEG_CS].sel = 0xF000;
    g_m.cpu.seg[SEG_CS].base = 0xFFFF0000ull;
    g_m.cpu.rip = 0xFFF0;
    EXPECT(dbg_reg_get(&g_m, 0, "lin", &v) && v == 0xFFFFFFF0ull,
           "lin = cs.base + rip (got %llx)", (unsigned long long)v);
    EXPECT(dbg_eval(&g_m, "$lin", &v, &err) && v == 0xFFFFFFF0ull, "$lin evaluates to the linear address");
    EXPECT(dbg_eval(&g_m, "$rip", &v, &err) && v == 0xFFF0ull, "$rip stays the raw IP");

    /* `regs` shows the linear address next to the raw one */
    char *regs = exec_capture(&g_m, "regs");
    EXPECT(regs && strstr(regs, "lin 0x00000000fffffff0"), "regs prints the linear address");
    free(regs);

    /* 2/3. `x/i` with no address follows cs.base; `x/i $rip` does not.  A
     * CS base of 0x1000 makes the linear address land in RAM where the
     * vector can place a known byte, and the raw IP land on a different
     * known byte -- so a transcript that disagrees PROVES which address
     * was used instead of merely printing one. */
    g_m.cpu.seg[SEG_CS].sel = 0x0000;
    g_m.cpu.seg[SEG_CS].base = 0x1000ull;
    g_m.cpu.rip = 0x20;
    g_m.ram[0x1020] = 0x90;   /* nop  -- at cs.base + rip */
    g_m.ram[0x0020] = 0xF4;   /* hlt  -- at the raw rip */

    char *at_lin = exec_capture(&g_m, "x/i");
    char *at_dollar_lin = exec_capture(&g_m, "x/i $lin");
    char *at_rip = exec_capture(&g_m, "x/i $rip");
    EXPECT(at_lin && strstr(at_lin, "0000000000001020") && strstr(at_lin, "nop"),
           "x/i with no address disassembles the LINEAR address");
    EXPECT(at_dollar_lin && at_lin && strcmp(at_dollar_lin, at_lin) == 0,
           "x/i $lin agrees with the default x/i");
    EXPECT(at_rip && strstr(at_rip, "0000000000000020") && strstr(at_rip, "hlt"),
           "x/i $rip reads the raw IP (documented, distinct from $lin)");
    free(at_lin); free(at_dollar_lin); free(at_rip);

    dbg_detach(&g_m);
    printf("ok linear address: $lin = cs.base + rip, x/i defaults to it, $rip stays raw\n");
}

int main(void) {
    reference();
    vectors_run_control();
    vectors_bp_everywhere();
    vectors_quit_pause_eval();
    vectors_linear_address();
    if (fails) { printf("test-dbg: %d FAILED\n", fails); return 1; }
    puts("test-dbg: all vectors passed");
    return 0;
}
