// dbg.c -- TOOLING: tooling core. T1: side-effect-free inspection and the
// disassembly trace with its CPU-vs-disassembler self-check. T2: run
// control (breakpoints, watchpoints, steps, pause), symbols, expressions
// and the REPL front end.
#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <signal.h>
#include <poll.h>
#include "dbg.h"
#include "cpu.h"
#include "disasm.h"

typedef struct {
    int valid;
    uint64_t rip, next, target;
    uint16_t cs;
    int chk;
    uint64_t events, resets;
} trace_prev_t;

#define MAX_BP 64
#define MAX_WP 16
#define MAX_FE 4
typedef struct { int id; uint64_t lin; int temp; int enabled; int internal; uint64_t hits; } bp_t;
typedef struct { int id; uint64_t lin; int len; int kind; uint64_t hits; } wp_t;
typedef struct { uint64_t addr, size; char *name; } sym_t;

struct dbg_state {
    machine_t *m;
    /* ---- T2 run control ---- */
    dbg_frontend_t *fe[MAX_FE];
    int nfe;
    bp_t bp[MAX_BP];
    int nbp, next_id;
    wp_t wp[MAX_WP];
    int nwp;
    dbg_action_t pending;
    uint64_t pend_steps;
    int pend_vcpu;
    int stopped, run_ended;
    int step_vcpu;
    uint64_t step_left;
    uint64_t until_vtime;
    int skip_once, skip_vcpu;
    uint64_t skip_lin;
    int ignore_once[EM_MAX_VCPU];
    int whit, whit_wp;
    dbg_stop_t wstop;
    volatile sig_atomic_t pause_req;
    uint32_t poll_ctr;
    int sel_vcpu;
    dbg_stop_t last;
    sym_t *syms;
    size_t nsyms;
    /* ---- T4 snapshot ops from the REPL ---- */
    int cur_site;            /* SITE_* of the stop being served          */
    int snap_op;             /* 1 = savevm, 2 = loadvm, pending          */
    char snap_path[512];
    /* ---- T1 trace ---- */
    FILE *tfp;
    char *tpath;
    char *tbuf;
    uint64_t tfrom, tmax;
    int tarmed, tdone;
    dbg_trace_stats_t ts;
    trace_prev_t prev[EM_MAX_VCPU];
};

/* ===================================================== inspection ===== */
int dbg_code_bits(const cpu_t *c) {
    int lma = (int)((c->efer >> 10) & 1);
    if (lma && c->seg[SEG_CS].l) return 64;
    if (c->cr0 & 1) return c->seg[SEG_CS].d_b ? 32 : 16;
    return 16;
}

size_t dbg_read_phys(machine_t *m, uint64_t phys, void *buf, size_t n) {
    uint8_t *b = buf;
    size_t i = 0;
    for (; i < n; i++)
        if (!mem_debug_peek_byte(m, phys + i, &b[i])) break;
    return i;
}

static int phys_q(machine_t *m, uint64_t phys, uint64_t *v) {
    uint8_t b[8];
    if (dbg_read_phys(m, phys, b, 8) != 8) return 0;
    uint64_t x = 0;
    for (int i = 7; i >= 0; i--) x = (x << 8) | b[i];
    *v = x;
    return 1;
}

/* Mirrors cpu.c translate(): same 4-level walk, PS leaves and masks, no
 * permission checks (a debugger sees supervisor pages), no faults, no TLB. */
int dbg_translate(machine_t *m, const cpu_t *c, uint64_t lin, uint64_t *phys) {
    if (!(c->cr0 & (1ULL << 31))) { *phys = lin; return 1; }
    uint64_t cr3 = c->cr3 & ~0xFFFULL, e;
    int i4 = (int)((lin >> 39) & 0x1FF), i3 = (int)((lin >> 30) & 0x1FF);
    int i2 = (int)((lin >> 21) & 0x1FF), i1 = (int)((lin >> 12) & 0x1FF);
    if (!phys_q(m, cr3 + (uint64_t)i4 * 8, &e) || !(e & 1)) return 0;
    uint64_t pdpt = e & 0x000FFFFFFFFFF000ULL;
    if (!phys_q(m, pdpt + (uint64_t)i3 * 8, &e) || !(e & 1)) return 0;
    if (e & (1ULL << 7)) { *phys = (e & 0xFFFFFC0000000ULL) | (lin & 0x3FFFFFFFULL); return 1; }
    uint64_t pd = e & 0x000FFFFFFFFFF000ULL;
    if (!phys_q(m, pd + (uint64_t)i2 * 8, &e) || !(e & 1)) return 0;
    if (e & (1ULL << 7)) { *phys = (e & 0x000FFFFFFFE00000ULL) | (lin & 0x1FFFFFULL); return 1; }
    uint64_t pt = e & 0x000FFFFFFFFFF000ULL;
    if (!phys_q(m, pt + (uint64_t)i1 * 8, &e) || !(e & 1)) return 0;
    *phys = (e & 0x000FFFFFFFFFF000ULL) | (lin & 0xFFFULL);
    return 1;
}

size_t dbg_read_lin(machine_t *m, const cpu_t *c, uint64_t lin, void *buf, size_t n) {
    uint8_t *b = buf;
    size_t i = 0;
    uint64_t page = ~0ULL, pbase = 0;
    for (; i < n; i++) {
        uint64_t a = lin + i;
        if ((a >> 12) != page) {
            uint64_t p;
            if (!dbg_translate(m, c, a & ~0xFFFULL, &p)) break;
            page = a >> 12; pbase = p & ~0xFFFULL;
        }
        if (!mem_debug_peek_byte(m, pbase | (a & 0xFFF), &b[i])) break;
    }
    return i;
}

size_t dbg_write_phys(machine_t *m, uint64_t phys, const void *buf, size_t n) {
    const uint8_t *b = buf;
    size_t i = 0;
    for (; i < n; i++) {
        uint64_t a = phys + i;
        if (!m->chipset.a20) a &= ~0x100000ULL;
        if (a >= ROM_ALIAS_LOW_BASE && a < ROM_ALIAS_LOW_BASE + ROM_SIZE) break;
        if (a >= RAM_SIZE) break;
        m->ram[a] = b[i];
    }
    if (i) perf_tlb_flush(m);   /* P2 TLB and P3 predecode share this epoch */
    return i;
}

size_t dbg_write_lin(machine_t *m, const cpu_t *c, uint64_t lin, const void *buf, size_t n) {
    const uint8_t *b = buf;
    size_t i = 0;
    for (; i < n; i++) {
        uint64_t p;
        if (!dbg_translate(m, c, lin + i, &p)) break;
        if (dbg_write_phys(m, p, &b[i], 1) != 1) break;
    }
    return i;
}

cpu_t *dbg_vcpu(machine_t *m, int i) {
    if (m->n_vcpus <= 1 || i == m->cur_vcpu || i < 0 || i >= EM_MAX_VCPU) return &m->cpu;
    return &m->vcpu[i].c;
}

int dbg_disas_at(machine_t *m, const cpu_t *c, uint64_t lin, int bits, uint64_t pc,
                 char *out, size_t outsz, int *len_out) {
    uint8_t b[16];
    size_t n = dbg_read_lin(m, c, lin, b, 15);
    if (!n) { snprintf(out, outsz, "<unreadable>"); if (len_out) *len_out = 0; return 0; }
    dis_insn_t in;
    dis_decode(b, n, bits, pc, &in);
    snprintf(out, outsz, "%s", in.text);
    if (len_out) *len_out = in.len;
    return in.valid;
}

/* ====================================================== the trace ===== */
static void trace_step(struct dbg_state *s, machine_t *m) {
    cpu_t *c = &m->cpu;
    int v = (m->cur_vcpu >= 0 && m->cur_vcpu < EM_MAX_VCPU) ? m->cur_vcpu : 0;
    trace_prev_t *p = &s->prev[v];
    if (!s->tarmed) {
        if (m->vtime_instr < s->tfrom) return;
        s->tarmed = 1;
    }
    /* self-check of this vCPU's previous instruction: no event, no reset and
     * the same CS in between, so the successor RIP is fully predicted */
    if (p->valid && c->event_count == p->events && m->chipset.resets == p->resets &&
        c->seg[SEG_CS].sel == p->cs) {
        int ok = 1;
        if (p->chk == DIS_CHK_FALL) { s->ts.fall++; ok = (c->rip == p->next); }
        else if (p->chk == DIS_CHK_COND) { s->ts.branch++; ok = (c->rip == p->next || c->rip == p->target); }
        else if (p->chk == DIS_CHK_TARGET) { s->ts.branch++; ok = (c->rip == p->target); }
        if (!ok) {
            s->ts.mismatches++;
            fprintf(s->tfp, "! vcpu%d successor of %04x:%016llx: decode predicts %016llx (target %016llx), cpu at %016llx\n",
                    v, p->cs, (unsigned long long)p->rip, (unsigned long long)p->next,
                    (unsigned long long)p->target, (unsigned long long)c->rip);
        }
    }
    uint8_t b[16];
    uint64_t lin = c->seg[SEG_CS].base + c->rip;
    size_t n = dbg_read_lin(m, c, lin, b, 15);
    int bits = dbg_code_bits(c);
    char hex[34]; size_t h = 0;
    dis_insn_t in;
    memset(&in, 0, sizeof in);
    if (n == 0) {
        s->ts.unreadable++;
        snprintf(in.text, sizeof in.text, "<unreadable>");
        p->valid = 0;
    } else {
        dis_decode(b, n, bits, c->rip, &in);
        if (!in.valid) {
            if (in.truncated) s->ts.unreadable++; else s->ts.undecodable++;
        }
        for (int i = 0; i < in.len && h + 2 < sizeof hex; i++) h += (size_t)snprintf(hex + h, sizeof hex - h, "%02x", b[i]);
        p->valid = in.valid;
        p->rip = c->rip; p->next = c->rip + (uint64_t)in.len; p->target = in.target;
        p->cs = c->seg[SEG_CS].sel; p->chk = in.chk;
        p->events = c->event_count; p->resets = m->chipset.resets;
    }
    hex[h] = 0;
    fprintf(s->tfp, "%d %llu %s %04x:%016llx %016llx %s %s\n", v,
            (unsigned long long)m->vtime_instr, cpu_mode_name(c), c->seg[SEG_CS].sel,
            (unsigned long long)c->rip, (unsigned long long)lin, h ? hex : "-", in.text);
    s->ts.lines++;
    if (s->tmax && s->ts.lines >= s->tmax) s->tdone = 1;
}

static int run_control_boundary(machine_t *m);
int (*dbg_snap_hook)(machine_t *m, const char *path, int load);

static void trace_forget(struct dbg_state *s) {   /* after loadvm: no stale successor */
    memset(s->prev, 0, sizeof s->prev);
}

static int dbg_boundary(machine_t *m) {
    struct dbg_state *s = m->dbg;
    if (s->tfp && !s->tdone) trace_step(s, m);
    if (s->nfe) return run_control_boundary(m);
    return 0;
}

/* ================================================== attach/detach ===== */
struct dbg_state *dbg_attach(machine_t *m) {
    if (m->dbg) return m->dbg;
    struct dbg_state *s = calloc(1, sizeof *s);
    if (!s) return NULL;
    s->m = m;
    m->dbg = s;
    cpu_dbg_boundary = dbg_boundary;
    return s;
}

void dbg_detach(machine_t *m) {
    struct dbg_state *s = m->dbg;
    if (!s) return;
    while (s->nfe) dbg_remove_frontend(m, s->fe[s->nfe - 1]);
    dbg_trace_close(m, 0);
    for (size_t i = 0; i < s->nsyms; i++) free(s->syms[i].name);
    free(s->syms);
    cpu_dbg_access = NULL;
    free(s);
    m->dbg = NULL;
}

int dbg_trace_open(machine_t *m, const char *path, uint64_t from_vtime, uint64_t max_lines) {
    struct dbg_state *s = dbg_attach(m);
    if (!s) return -1;
    FILE *f = fopen(path, "w");
    if (!f) return -1;
    s->tbuf = malloc(1 << 20);
    if (s->tbuf) setvbuf(f, s->tbuf, _IOFBF, 1 << 20);
    s->tfp = f;
    size_t pl = strlen(path) + 1;
    s->tpath = malloc(pl);
    if (s->tpath) memcpy(s->tpath, path, pl);
    s->tfrom = from_vtime; s->tmax = max_lines;
    s->tarmed = 0; s->tdone = 0;
    memset(&s->ts, 0, sizeof s->ts);
    memset(s->prev, 0, sizeof s->prev);
    fprintf(f, "# x86emu trace v1: vcpu vtime mode cs:rip linear bytes disassembly (TOOLING T1)\n");
    return 0;
}

int dbg_trace_stats(machine_t *m, dbg_trace_stats_t *out) {
    if (!m->dbg) return -1;
    *out = m->dbg->ts;
    return 0;
}

void dbg_trace_close(machine_t *m, int print_summary) {
    struct dbg_state *s = m->dbg;
    if (!s || !s->tfp) return;
    fprintf(s->tfp, "# end: lines=%llu fall=%llu branch=%llu mismatches=%llu undecodable=%llu unreadable=%llu\n",
            (unsigned long long)s->ts.lines, (unsigned long long)s->ts.fall,
            (unsigned long long)s->ts.branch, (unsigned long long)s->ts.mismatches,
            (unsigned long long)s->ts.undecodable, (unsigned long long)s->ts.unreadable);
    fclose(s->tfp);
    s->tfp = NULL;
    free(s->tbuf); s->tbuf = NULL;
    if (print_summary)
        fprintf(stderr, "[trace] lines=%llu fall=%llu branch=%llu mismatches=%llu undecodable=%llu unreadable=%llu file=%s\n",
                (unsigned long long)s->ts.lines, (unsigned long long)s->ts.fall,
                (unsigned long long)s->ts.branch, (unsigned long long)s->ts.mismatches,
                (unsigned long long)s->ts.undecodable, (unsigned long long)s->ts.unreadable,
                s->tpath ? s->tpath : "?");
    free(s->tpath); s->tpath = NULL;
}

/* ================================================== T2: run control ===== */
/* Stop sites. BOUNDARY: inside cpu_step, after interrupt delivery, before
 * the fetch -- the instruction at CS:RIP has not run. POST: between two
 * cpu_step calls (watchpoints, pause requests, run start). Resuming from a
 * POST stop must not count the next boundary as a step, because that
 * boundary is still in front of the instruction the user is looking at. */
enum { SITE_BOUNDARY = 0, SITE_POST = 1 };

static uint64_t cur_lin(machine_t *m, int v) {
    cpu_t *c = dbg_vcpu(m, v);
    return c->seg[SEG_CS].base + c->rip;
}

int dbg_nvcpus(machine_t *m) { return m->n_vcpus > 1 ? m->n_vcpus : 1; }

static int cur_vcpu_idx(machine_t *m) {
    return (m->n_vcpus > 1 && m->cur_vcpu >= 0 && m->cur_vcpu < EM_MAX_VCPU) ? m->cur_vcpu : 0;
}

static void drop_internal_bps(struct dbg_state *s) {
    int j = 0;
    for (int i = 0; i < s->nbp; i++) if (!s->bp[i].internal) s->bp[j++] = s->bp[i];
    s->nbp = j;
}

static void stop_loop(machine_t *m) {
    struct dbg_state *s = m->dbg;
    while (s->pending == DBG_ACT_NONE && s->nfe > 0) {
        dbg_frontend_t *fes[MAX_FE];
        int nf = s->nfe;
        memcpy(fes, s->fe, sizeof(fes[0]) * (size_t)nf);
        struct pollfd pfd[MAX_FE];
        dbg_frontend_t *pfe[MAX_FE];
        int np = 0;
        for (int i = 0; i < nf && s->pending == DBG_ACT_NONE; i++) {
            int fd = fes[i]->stop_fd ? fes[i]->stop_fd(m, fes[i]) : -1;
            if (fd < 0) { if (fes[i]->service) fes[i]->service(m, fes[i]); }
            else {
                pfd[np].fd = fd;
                pfd[np].events = fes[i]->stop_events ? (short)fes[i]->stop_events(m, fes[i]) : POLLIN;
                if (!pfd[np].events) pfd[np].events = POLLIN;
                pfd[np].revents = 0;
                pfe[np++] = fes[i];
            }
        }
        if (s->pending != DBG_ACT_NONE || np == 0) continue;
        int r = poll(pfd, (nfds_t)np, 200);
        if (r <= 0) continue;
        for (int i = 0; i < np && s->pending == DBG_ACT_NONE; i++)
            if (pfd[i].revents) pfe[i]->service(m, pfe[i]);
    }
}

/* Enter a stop: announce, serve the front ends, apply the resume action.
 * Returns 1 when the current cpu_step must be abandoned (quit). */
static int enter_stop(machine_t *m, dbg_stop_t *st, int site) {
    struct dbg_state *s = m->dbg;
    s->last = *st;
    s->sel_vcpu = st->vcpu;
    s->stopped = 1;
    s->step_left = 0;
    drop_internal_bps(s);
    s->pending = DBG_ACT_NONE;
    s->pend_steps = 0;
    s->pend_vcpu = -1;
    s->cur_site = site;
    dbg_frontend_t *fes[MAX_FE];
    int nf = s->nfe;
    memcpy(fes, s->fe, sizeof(fes[0]) * (size_t)nf);
    for (int i = 0; i < nf; i++) if (fes[i]->on_stop) fes[i]->on_stop(m, fes[i], st);
    stop_loop(m);
    s->stopped = 0;
    dbg_action_t act = s->pending;
    s->pending = DBG_ACT_NONE;
    if (act == DBG_ACT_NONE) act = DBG_ACT_CONTINUE;     /* every front end left */
    if (act == DBG_ACT_QUIT) { m->stop_requested = 1; return 1; }
    if (act == DBG_ACT_STEP) {
        s->step_vcpu = s->pend_vcpu >= 0 ? s->pend_vcpu : s->sel_vcpu;
        s->step_left = s->pend_steps ? s->pend_steps : 1;
    }
    /* the resuming vCPU's first boundary at its (possibly edited) RIP does
     * not re-trigger the breakpoint it is standing on */
    s->skip_once = 1;
    s->skip_vcpu = st->vcpu;
    s->skip_lin = cur_lin(m, st->vcpu);
    if (site == SITE_POST && st->vcpu >= 0 && st->vcpu < EM_MAX_VCPU) s->ignore_once[st->vcpu] = 1;
    if (s->nfe == 0) {           /* nothing left to stop for: plain run */
        s->nbp = 0; s->nwp = 0; cpu_dbg_access = NULL;
        s->step_left = 0; s->until_vtime = 0;
    }
    return 0;
}

static int run_control_boundary(machine_t *m) {
    struct dbg_state *s = m->dbg;
    int v = cur_vcpu_idx(m);
    cpu_t *c = &m->cpu;
    uint64_t lin = c->seg[SEG_CS].base + c->rip;
    int ignore = s->ignore_once[v];
    s->ignore_once[v] = 0;
    dbg_stop_t st;
    memset(&st, 0, sizeof st);
    st.vcpu = v; st.lin = lin;
    int skip = s->skip_once && s->skip_vcpu == v && s->skip_lin == lin;
    if (s->skip_once && s->skip_vcpu == v) s->skip_once = 0;
    if (!skip) {
        for (int i = 0; i < s->nbp; i++) {
            bp_t *b = &s->bp[i];
            if (!b->enabled || b->lin != lin) continue;
            b->hits++;
            if (!b->internal) { st.reason = DBG_STOP_BREAK; st.bp_id = b->id; }
            else st.reason = DBG_STOP_STEP;      /* nexti landed */
            if (b->temp) { s->bp[i] = s->bp[--s->nbp]; }
            break;
        }
    }
    if (!st.reason && !ignore && s->step_left && v == s->step_vcpu) {
        if (--s->step_left == 0) st.reason = DBG_STOP_STEP;
    }
    if (!st.reason && s->until_vtime && m->vtime_instr >= s->until_vtime) {
        st.reason = DBG_STOP_UNTIL;
        s->until_vtime = 0;
    }
    if (!st.reason) return 0;
    return enter_stop(m, &st, SITE_BOUNDARY);
}

static void dbg_access(machine_t *m, uint64_t lin, int size, int write) {
    struct dbg_state *s = m->dbg;
    if (!s || s->whit || !s->nwp) return;
    for (int i = 0; i < s->nwp; i++) {
        wp_t *w = &s->wp[i];
        if (lin >= w->lin + (uint64_t)w->len || lin + (uint64_t)size <= w->lin) continue;
        if (w->kind == 'w' && !write) continue;
        if (w->kind == 'r' && write) continue;
        w->hits++;
        memset(&s->wstop, 0, sizeof s->wstop);
        s->wstop.reason = DBG_STOP_WATCH;
        s->wstop.vcpu = cur_vcpu_idx(m);
        s->wstop.lin = m->cpu.seg[SEG_CS].base + m->cpu.rip;   /* the accessing instruction */
        s->wstop.watch_addr = lin;
        s->wstop.watch_kind = write ? 'w' : 'r';
        s->wstop.watch_id = w->id;
        s->wstop.watch_size = w->len > 8 ? 8 : w->len;
        uint64_t old = 0;
        dbg_read_lin(m, &m->cpu, w->lin, &old, (size_t)s->wstop.watch_size);
        s->wstop.watch_old = old;
        s->whit_wp = i;
        s->whit = 1;
        return;
    }
}

static int do_snap(machine_t *m, struct dbg_state *s, int op, const char *path) {
    int rc = dbg_snap_hook ? dbg_snap_hook(m, path, op == 2) : -1;
    if (op == 2 && rc == 0) trace_forget(s);
    return rc;
}

void dbg_post_step(machine_t *m) {
    struct dbg_state *s = m->dbg;
    if (!s || !s->nfe) return;
    if (s->snap_op) {          /* a savevm/loadvm deferred from a boundary stop */
        int op = s->snap_op;
        s->snap_op = 0;
        int rc = do_snap(m, s, op, s->snap_path);
        fprintf(stderr, "[dbg] %s %s %s at vtime %llu\n", op == 2 ? "loadvm" : "savevm", s->snap_path,
                rc == 0 ? "done" : "FAILED", (unsigned long long)m->vtime_instr);
        dbg_stop_t st;
        memset(&st, 0, sizeof st);
        st.reason = DBG_STOP_STEP; st.vcpu = cur_vcpu_idx(m); st.lin = cur_lin(m, st.vcpu);
        enter_stop(m, &st, SITE_POST);
        return;
    }
    if (s->whit) {
        s->whit = 0;
        if (s->whit_wp < s->nwp) {
            uint64_t nv = 0;
            dbg_read_lin(m, &m->cpu, s->wp[s->whit_wp].lin, &nv, (size_t)s->wstop.watch_size);
            s->wstop.watch_new = nv;
        }
        dbg_stop_t st = s->wstop;
        enter_stop(m, &st, SITE_POST);
        return;
    }
    if (s->pause_req) {
        s->pause_req = 0;
        dbg_stop_t st;
        memset(&st, 0, sizeof st);
        st.reason = DBG_STOP_PAUSE; st.vcpu = cur_vcpu_idx(m); st.lin = cur_lin(m, st.vcpu);
        enter_stop(m, &st, SITE_POST);
        return;
    }
    if ((++s->poll_ctr & 0x3FFu) == 0) {
        dbg_frontend_t *fes[MAX_FE];
        int nf = s->nfe;
        memcpy(fes, s->fe, sizeof(fes[0]) * (size_t)nf);
        for (int i = 0; i < nf; i++) if (fes[i]->poll_running) fes[i]->poll_running(m, fes[i]);
    }
}

void dbg_request_pause(machine_t *m) { if (m->dbg) m->dbg->pause_req = 1; }

void dbg_run_start(machine_t *m, int stop_at_start) {
    struct dbg_state *s = m->dbg;
    if (!s || !s->nfe || !stop_at_start) return;
    dbg_stop_t st;
    memset(&st, 0, sizeof st);
    st.reason = DBG_STOP_START; st.vcpu = cur_vcpu_idx(m); st.lin = cur_lin(m, st.vcpu);
    enter_stop(m, &st, SITE_POST);
}

void dbg_run_end(machine_t *m, int step_result) {
    struct dbg_state *s = m->dbg;
    if (!s || !s->nfe) return;
    s->run_ended = 1;
    int want = 0;
    for (int i = 0; i < s->nfe; i++) want |= s->fe[i]->wants_end_stop;
    if (want && !m->stop_requested) {
        dbg_stop_t st;
        memset(&st, 0, sizeof st);
        st.reason = DBG_STOP_END; st.vcpu = 0; st.lin = cur_lin(m, 0);
        st.end_rc = step_result;
        cpu_t *c = dbg_vcpu(m, 0);
        st.end_why = c->fault ? "cpu fault" : c->halted ? "halted" : step_result ? "stopped" : "instruction cap";
        s->last = st;
        s->stopped = 1;
        s->pending = DBG_ACT_NONE;
        dbg_frontend_t *fes[MAX_FE];
        int nf = s->nfe;
        memcpy(fes, s->fe, sizeof(fes[0]) * (size_t)nf);
        for (int i = 0; i < nf; i++) if (fes[i]->on_stop) fes[i]->on_stop(m, fes[i], &st);
        stop_loop(m);
        s->stopped = 0;
    }
    while (s->nfe) {
        dbg_frontend_t *fe = s->fe[s->nfe - 1];
        dbg_remove_frontend(m, fe);
    }
}

/* steps == 0 / vcpu < 0 keep what dbg_exec() parsed for this stop */
void dbg_resume(machine_t *m, dbg_action_t act, uint64_t steps, int vcpu) {
    struct dbg_state *s = m->dbg;
    if (!s) return;
    s->pending = act;
    if (steps) s->pend_steps = steps;
    if (vcpu >= 0 && vcpu < dbg_nvcpus(m)) s->pend_vcpu = vcpu;
}

int dbg_add_frontend(machine_t *m, dbg_frontend_t *fe) {
    struct dbg_state *s = dbg_attach(m);
    if (!s || s->nfe >= MAX_FE) return -1;
    s->fe[s->nfe++] = fe;
    return 0;
}

void dbg_remove_frontend(machine_t *m, dbg_frontend_t *fe) {
    struct dbg_state *s = m->dbg;
    if (!s) return;
    for (int i = 0; i < s->nfe; i++) {
        if (s->fe[i] != fe) continue;
        memmove(&s->fe[i], &s->fe[i + 1], sizeof(s->fe[0]) * (size_t)(s->nfe - i - 1));
        s->nfe--;
        if (fe->on_detach) fe->on_detach(m, fe);
        break;
    }
    if (s->nfe == 0) { s->nbp = 0; s->nwp = 0; cpu_dbg_access = NULL; s->step_left = 0; s->until_vtime = 0; }
}

int dbg_frontends(machine_t *m) { return m->dbg ? m->dbg->nfe : 0; }

int dbg_has_frontend(machine_t *m, const char *name) {
    if (!m->dbg || !name) return 0;
    for (int i = 0; i < m->dbg->nfe; i++)
        if (m->dbg->fe[i]->name && !strcmp(m->dbg->fe[i]->name, name)) return 1;
    return 0;
}

/* ----------------------------------------------- breakpoints/watch ---- */
int dbg_break_add(machine_t *m, uint64_t lin, int temp) {
    struct dbg_state *s = dbg_attach(m);
    if (!s || s->nbp >= MAX_BP) return -1;
    for (int i = 0; i < s->nbp; i++)
        if (s->bp[i].lin == lin && !s->bp[i].internal && !s->bp[i].temp && !temp) return s->bp[i].id;
    bp_t *b = &s->bp[s->nbp++];
    memset(b, 0, sizeof *b);
    b->id = ++s->next_id; b->lin = lin; b->temp = temp; b->enabled = 1;
    return b->id;
}
static int break_add_internal(struct dbg_state *s, uint64_t lin) {
    if (s->nbp >= MAX_BP) return -1;
    bp_t *b = &s->bp[s->nbp++];
    memset(b, 0, sizeof *b);
    b->id = 0; b->lin = lin; b->temp = 1; b->enabled = 1; b->internal = 1;
    return 0;
}
int dbg_break_del(machine_t *m, int id) {
    struct dbg_state *s = m->dbg;
    if (!s) return -1;
    for (int i = 0; i < s->nbp; i++)
        if (s->bp[i].id == id && !s->bp[i].internal) { s->bp[i] = s->bp[--s->nbp]; return 0; }
    return -1;
}
int dbg_break_del_addr(machine_t *m, uint64_t lin) {
    struct dbg_state *s = m->dbg;
    if (!s) return -1;
    for (int i = 0; i < s->nbp; i++)
        if (s->bp[i].lin == lin && !s->bp[i].internal) { s->bp[i] = s->bp[--s->nbp]; return 0; }
    return -1;
}
int dbg_watch_add(machine_t *m, uint64_t lin, int len, int kind) {
    struct dbg_state *s = dbg_attach(m);
    if (!s || s->nwp >= MAX_WP || len <= 0 || len > 4096) return -1;
    if (kind != 'r' && kind != 'w' && kind != 'a') return -1;
    wp_t *w = &s->wp[s->nwp++];
    memset(w, 0, sizeof *w);
    w->id = ++s->next_id; w->lin = lin; w->len = len; w->kind = kind;
    cpu_dbg_access = dbg_access;
    return w->id;
}
int dbg_watch_del(machine_t *m, int id) {
    struct dbg_state *s = m->dbg;
    if (!s) return -1;
    for (int i = 0; i < s->nwp; i++)
        if (s->wp[i].id == id) {
            s->wp[i] = s->wp[--s->nwp];
            if (!s->nwp) cpu_dbg_access = NULL;
            return 0;
        }
    return -1;
}
int dbg_watch_del_addr(machine_t *m, uint64_t lin, int len, int kind) {
    struct dbg_state *s = m->dbg;
    if (!s) return -1;
    for (int i = 0; i < s->nwp; i++)
        if (s->wp[i].lin == lin && s->wp[i].len == len && s->wp[i].kind == kind) {
            s->wp[i] = s->wp[--s->nwp];
            if (!s->nwp) cpu_dbg_access = NULL;
            return 0;
        }
    return -1;
}
void dbg_set_until(machine_t *m, uint64_t vtime) { if (dbg_attach(m)) m->dbg->until_vtime = vtime; }
int dbg_selected_vcpu(machine_t *m) { return m->dbg ? m->dbg->sel_vcpu : 0; }
const dbg_stop_t *dbg_last_stop(machine_t *m) { return m->dbg ? &m->dbg->last : NULL; }

/* ----------------------------------------------------------- symbols --- */
typedef struct {
    unsigned char e_ident[16]; uint16_t e_type, e_machine; uint32_t e_version;
    uint64_t e_entry, e_phoff, e_shoff; uint32_t e_flags;
    uint16_t e_ehsize, e_phentsize, e_phnum, e_shentsize, e_shnum, e_shstrndx;
} elf64_ehdr_t;
typedef struct {
    uint32_t sh_name, sh_type; uint64_t sh_flags, sh_addr, sh_offset, sh_size;
    uint32_t sh_link, sh_info; uint64_t sh_addralign, sh_entsize;
} elf64_shdr_t;
typedef struct {
    uint32_t st_name; unsigned char st_info, st_other; uint16_t st_shndx;
    uint64_t st_value, st_size;
} elf64_sym_t;

static int sym_cmp(const void *a, const void *b) {
    const sym_t *x = a, *y = b;
    return x->addr < y->addr ? -1 : x->addr > y->addr ? 1 : 0;
}

int dbg_symbols_load(machine_t *m, const char *path) {
    struct dbg_state *s = dbg_attach(m);
    if (!s) return -1;
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (n < (long)sizeof(elf64_ehdr_t)) { fclose(f); return -1; }
    uint8_t *img = malloc((size_t)n);
    if (!img || fread(img, 1, (size_t)n, f) != (size_t)n) { free(img); fclose(f); return -1; }
    fclose(f);
    elf64_ehdr_t eh;
    memcpy(&eh, img, sizeof eh);
    int added = 0;
    if (memcmp(eh.e_ident, "\x7f" "ELF", 4) || eh.e_ident[4] != 2 ||
        eh.e_shentsize != sizeof(elf64_shdr_t) ||
        eh.e_shoff + (uint64_t)eh.e_shnum * sizeof(elf64_shdr_t) > (uint64_t)n) { free(img); return -1; }
    for (int i = 0; i < eh.e_shnum; i++) {
        elf64_shdr_t sh;
        memcpy(&sh, img + eh.e_shoff + (uint64_t)i * sizeof sh, sizeof sh);
        if (sh.sh_type != 2 /* SHT_SYMTAB */ || sh.sh_link >= eh.e_shnum) continue;
        elf64_shdr_t st;
        memcpy(&st, img + eh.e_shoff + (uint64_t)sh.sh_link * sizeof st, sizeof st);
        if (sh.sh_offset + sh.sh_size > (uint64_t)n || st.sh_offset + st.sh_size > (uint64_t)n) continue;
        size_t cnt = (size_t)(sh.sh_size / sizeof(elf64_sym_t));
        sym_t *ns = realloc(s->syms, sizeof(sym_t) * (s->nsyms + cnt));
        if (!ns) break;
        s->syms = ns;
        for (size_t k = 0; k < cnt; k++) {
            elf64_sym_t sy;
            memcpy(&sy, img + sh.sh_offset + k * sizeof sy, sizeof sy);
            int type = sy.st_info & 15;
            if (!sy.st_value || sy.st_name >= st.sh_size || (type != 0 && type != 1 && type != 2)) continue;
            const char *nm = (const char *)img + st.sh_offset + sy.st_name;
            size_t L = strnlen(nm, (size_t)(st.sh_size - sy.st_name));
            if (!L || L >= (size_t)(st.sh_size - sy.st_name)) continue;
            if (nm[0] == '.' || nm[0] == '$') continue;
            char *cp = malloc(L + 1);
            if (!cp) break;
            memcpy(cp, nm, L); cp[L] = 0;
            s->syms[s->nsyms].addr = sy.st_value;
            s->syms[s->nsyms].size = sy.st_size;
            s->syms[s->nsyms].name = cp;
            s->nsyms++; added++;
        }
    }
    free(img);
    qsort(s->syms, s->nsyms, sizeof(sym_t), sym_cmp);
    return added;
}

int dbg_symbol_lookup(machine_t *m, const char *name, uint64_t *addr) {
    struct dbg_state *s = m->dbg;
    if (!s) return 0;
    for (size_t i = 0; i < s->nsyms; i++)
        if (!strcmp(s->syms[i].name, name)) { *addr = s->syms[i].addr; return 1; }
    return 0;
}

const char *dbg_symbolize(machine_t *m, uint64_t addr, uint64_t *off) {
    struct dbg_state *s = m->dbg;
    if (!s || !s->nsyms) return NULL;
    size_t lo = 0, hi = s->nsyms;
    while (lo < hi) { size_t mid = (lo + hi) / 2; if (s->syms[mid].addr <= addr) lo = mid + 1; else hi = mid; }
    if (!lo) return NULL;
    const sym_t *y = &s->syms[lo - 1];
    uint64_t d = addr - y->addr;
    if (y->size ? d >= y->size : d >= 0x10000) return NULL;
    if (off) *off = d;
    return y->name;
}

/* ----------------------------------------------------------- registers --- */
static const char *const gpr64n[16] = {"rax","rcx","rdx","rbx","rsp","rbp","rsi","rdi",
                                       "r8","r9","r10","r11","r12","r13","r14","r15"};
static const char *const gpr32n[16] = {"eax","ecx","edx","ebx","esp","ebp","esi","edi",
                                       "r8d","r9d","r10d","r11d","r12d","r13d","r14d","r15d"};
static const char *const gpr16n[16] = {"ax","cx","dx","bx","sp","bp","si","di",
                                       "r8w","r9w","r10w","r11w","r12w","r13w","r14w","r15w"};
static const char *const gpr8n[8]   = {"al","cl","dl","bl","spl","bpl","sil","dil"};
static const char *const segn[6]    = {"es","cs","ss","ds","fs","gs"};

int dbg_reg_get(machine_t *m, int vcpu, const char *name, uint64_t *v) {
    cpu_t *c = dbg_vcpu(m, vcpu);
    if (*name == '$') name++;
    for (int i = 0; i < 16; i++) {
        if (!strcmp(name, gpr64n[i])) { *v = c->gpr[i]; return 1; }
        if (!strcmp(name, gpr32n[i])) { *v = c->gpr[i] & 0xFFFFFFFFu; return 1; }
        if (!strcmp(name, gpr16n[i])) { *v = c->gpr[i] & 0xFFFFu; return 1; }
    }
    for (int i = 0; i < 8; i++) if (!strcmp(name, gpr8n[i])) { *v = c->gpr[i] & 0xFFu; return 1; }
    for (int i = 0; i < 6; i++) if (!strcmp(name, segn[i])) { *v = c->seg[i].sel; return 1; }
    if (!strcmp(name, "rip") || !strcmp(name, "pc")) { *v = c->rip; return 1; }
    if (!strcmp(name, "eip")) { *v = c->rip & 0xFFFFFFFFu; return 1; }
    if (!strcmp(name, "ip")) { *v = c->rip & 0xFFFFu; return 1; }
    if (!strcmp(name, "rflags") || !strcmp(name, "eflags") || !strcmp(name, "flags")) { *v = c->rflags; return 1; }
    if (!strcmp(name, "cr0")) { *v = c->cr0; return 1; }
    if (!strcmp(name, "cr2")) { *v = c->cr2; return 1; }
    if (!strcmp(name, "cr3")) { *v = c->cr3; return 1; }
    if (!strcmp(name, "cr4")) { *v = c->cr4; return 1; }
    if (!strcmp(name, "efer")) { *v = c->efer; return 1; }
    if (!strcmp(name, "fs_base")) { *v = c->seg[SEG_FS].base; return 1; }
    if (!strcmp(name, "gs_base")) { *v = c->seg[SEG_GS].base; return 1; }
    if (!strcmp(name, "k_gs_base") || !strcmp(name, "kgs_base")) { *v = c->kgs_base; return 1; }
    if (!strcmp(name, "cs_base")) { *v = c->seg[SEG_CS].base; return 1; }
    if (!strcmp(name, "lin")) { *v = c->seg[SEG_CS].base + c->rip; return 1; }
    if (!strcmp(name, "vtime")) { *v = m->vtime_instr; return 1; }
    if (!strcmp(name, "steps")) { *v = c->executed_steps; return 1; }
    if (!strcmp(name, "mxcsr")) { *v = c->mxcsr; return 1; }
    return 0;
}

int dbg_reg_set(machine_t *m, int vcpu, const char *name, uint64_t v) {
    cpu_t *c = dbg_vcpu(m, vcpu);
    if (*name == '$') name++;
    for (int i = 0; i < 16; i++) {
        if (!strcmp(name, gpr64n[i])) { c->gpr[i] = v; return 1; }
        if (!strcmp(name, gpr32n[i])) { c->gpr[i] = v & 0xFFFFFFFFu; return 1; }   /* zero-extends, as a 32-bit write does */
        if (!strcmp(name, gpr16n[i])) { c->gpr[i] = (c->gpr[i] & ~0xFFFFull) | (v & 0xFFFF); return 1; }
    }
    for (int i = 0; i < 8; i++)
        if (!strcmp(name, gpr8n[i])) { c->gpr[i] = (c->gpr[i] & ~0xFFull) | (v & 0xFF); return 1; }
    if (!strcmp(name, "rip") || !strcmp(name, "pc")) { c->rip = v; return 1; }
    if (!strcmp(name, "eip")) { c->rip = v & 0xFFFFFFFFu; return 1; }
    if (!strcmp(name, "rflags") || !strcmp(name, "eflags")) { c->rflags = (v & 0x00257FD5ull) | 2; return 1; }
    return 0;
}

/* --------------------------------------------------------- expressions --- */
typedef struct { machine_t *m; const char *p; const char *err; } ev_t;
static void ev_ws(ev_t *e) { while (*e->p == ' ' || *e->p == '\t') e->p++; }
static uint64_t ev_sum(ev_t *e);
static uint64_t ev_atom(ev_t *e) {
    ev_ws(e);
    if (*e->p == '(') {
        e->p++;
        uint64_t v = ev_sum(e);
        ev_ws(e);
        if (*e->p == ')') e->p++; else e->err = "missing ')'";
        return v;
    }
    if (*e->p == '-') { e->p++; return (uint64_t)(-(int64_t)ev_atom(e)); }
    if (*e->p == '*') {   /* memory deref: *ADDR reads 8 bytes */
        e->p++;
        uint64_t a = ev_atom(e), v = 0;
        if (dbg_read_lin(e->m, dbg_vcpu(e->m, dbg_selected_vcpu(e->m)), a, &v, 8) != 8) e->err = "unreadable memory";
        return v;
    }
    if (*e->p == '$') {
        char nm[32]; size_t k = 0;
        e->p++;
        while ((isalnum((unsigned char)*e->p) || *e->p == '_') && k < sizeof nm - 1) nm[k++] = *e->p++;
        nm[k] = 0;
        uint64_t v;
        if (!dbg_reg_get(e->m, dbg_selected_vcpu(e->m), nm, &v)) { e->err = "unknown register"; return 0; }
        return v;
    }
    if (isdigit((unsigned char)*e->p)) {
        char *end;
        uint64_t v = strtoull(e->p, &end, 0);
        e->p = end;
        return v;
    }
    if (isalpha((unsigned char)*e->p) || *e->p == '_' || *e->p == '.') {
        char nm[128]; size_t k = 0;
        while ((isalnum((unsigned char)*e->p) || *e->p == '_' || *e->p == '.') && k < sizeof nm - 1) nm[k++] = *e->p++;
        nm[k] = 0;
        uint64_t v;
        if (dbg_symbol_lookup(e->m, nm, &v)) return v;
        if (dbg_reg_get(e->m, dbg_selected_vcpu(e->m), nm, &v)) return v;
        e->err = "unknown symbol";
        return 0;
    }
    e->err = "bad expression";
    return 0;
}
static uint64_t ev_prod(ev_t *e) {
    uint64_t v = ev_atom(e);
    for (;;) {
        ev_ws(e);
        if (*e->p == '*') { e->p++; v *= ev_atom(e); }
        else return v;
    }
}
static uint64_t ev_sum(ev_t *e) {
    uint64_t v = ev_prod(e);
    for (;;) {
        ev_ws(e);
        if (*e->p == '+') { e->p++; v += ev_prod(e); }
        else if (*e->p == '-') { e->p++; v -= ev_prod(e); }
        else return v;
    }
}
int dbg_eval(machine_t *m, const char *expr, uint64_t *v, const char **err) {
    ev_t e = { m, expr, NULL };
    ev_ws(&e);
    if (!*e.p) { if (err) *err = "missing expression"; return 0; }
    *v = ev_sum(&e);
    ev_ws(&e);
    if (!e.err && *e.p) e.err = "trailing characters";
    if (err) *err = e.err;
    return e.err == NULL;
}

/* ====================================================== T2: the REPL ===== */
static void print_flags(FILE *o, uint64_t f) {
    static const struct { int bit; const char *n; } fl[] = {
        {0,"CF"},{2,"PF"},{4,"AF"},{6,"ZF"},{7,"SF"},{8,"TF"},{9,"IF"},{10,"DF"},{11,"OF"} };
    fprintf(o, "[");
    for (size_t i = 0; i < sizeof fl / sizeof fl[0]; i++) if (f & (1ull << fl[i].bit)) fprintf(o, " %s", fl[i].n);
    fprintf(o, " ]");
}

static void print_where(machine_t *m, FILE *o, int v) {
    cpu_t *c = dbg_vcpu(m, v);
    uint64_t lin = c->seg[SEG_CS].base + c->rip, off = 0;
    char txt[160]; int len = 0;
    dbg_disas_at(m, c, lin, dbg_code_bits(c), c->rip, txt, sizeof txt, &len);
    const char *sym = dbg_symbolize(m, lin, &off);
    fprintf(o, "=> %04x:%016llx", c->seg[SEG_CS].sel, (unsigned long long)c->rip);
    if (sym) fprintf(o, " <%s+0x%llx>", sym, (unsigned long long)off);
    fprintf(o, "  %s\n", txt);
}

static void cmd_regs(machine_t *m, FILE *o, int v) {
    cpu_t *c = dbg_vcpu(m, v);
    for (int i = 0; i < 16; i++)
        fprintf(o, "%-3s 0x%016llx%s", gpr64n[i], (unsigned long long)c->gpr[i], (i % 4 == 3) ? "\n" : "  ");
    fprintf(o, "rip 0x%016llx  rflags 0x%016llx ", (unsigned long long)c->rip, (unsigned long long)c->rflags);
    print_flags(o, c->rflags);
    /* A1: print the LINEAR address next to the raw RIP.  In real mode the
     * two differ by the CS base (0xFFFF0000 for the reset vector), so
     * "x/i $rip" reads RAM instead of the ROM at the reset vector and shows
     * zeros -- measured, and the reason the disassembly looked empty.  The
     * linear form is what every other address in the debugger means, so
     * show it right where the user is already looking. */
    fprintf(o, "\nlin 0x%016llx  (cs.base + rip; $lin in expressions, "
               "x/i with no address uses it)", (unsigned long long)(c->seg[SEG_CS].base + c->rip));
    fprintf(o, "\ncs %04x ss %04x ds %04x es %04x fs %04x gs %04x  mode %s cpl %d vcpu %d\n",
            c->seg[SEG_CS].sel, c->seg[SEG_SS].sel, c->seg[SEG_DS].sel, c->seg[SEG_ES].sel,
            c->seg[SEG_FS].sel, c->seg[SEG_GS].sel, cpu_mode_name(c), c->seg[SEG_CS].sel & 3, v);
}

static void cmd_sregs(machine_t *m, FILE *o, int v) {
    cpu_t *c = dbg_vcpu(m, v);
    for (int i = 0; i < 6; i++)
        fprintf(o, "%s sel=%04x base=%016llx limit=%08x type=%02x d_b=%d l=%d p=%d\n", segn[i],
                c->seg[i].sel, (unsigned long long)c->seg[i].base, c->seg[i].limit, c->seg[i].type,
                c->seg[i].d_b, c->seg[i].l, c->seg[i].present);
    /* D1: TR/LDTR now carry the selectors the loads recorded (STR/SLDT
     * report them, and VERR/VERW/LAR/LSL with TI=1 read through LDTR), so
     * the state dump has to show them or the debugger is lying about what
     * the guest's next descriptor access will use. */
    fprintf(o, "gdtr base=%016llx limit=%04x\nidtr base=%016llx limit=%04x\n"
               "tr   sel=%04x base=%016llx limit=%04x\n"
               "ldtr sel=%04x base=%016llx limit=%04x\n",
            (unsigned long long)c->gdtr_base, c->gdtr_limit, (unsigned long long)c->idtr_base, c->idtr_limit,
            c->tr_sel, (unsigned long long)c->tr_base, c->tr_limit,
            c->ldtr_sel, (unsigned long long)c->ldtr_base, c->ldtr_limit);
    fprintf(o, "kernel_gs_base=%016llx\n", (unsigned long long)c->kgs_base);
}

static void cmd_cr(machine_t *m, FILE *o, int v) {
    cpu_t *c = dbg_vcpu(m, v);
    fprintf(o, "cr0=%016llx [%s%s%s%s%s ]\n", (unsigned long long)c->cr0, (c->cr0 & 1) ? " PE" : "",
            (c->cr0 & (1ull << 16)) ? " WP" : "", (c->cr0 & (1ull << 31)) ? " PG" : "",
            (c->cr0 & (1ull << 30)) ? " CD" : "", (c->cr0 & (1ull << 29)) ? " NW" : "");
    fprintf(o, "cr2=%016llx\ncr3=%016llx\n", (unsigned long long)c->cr2, (unsigned long long)c->cr3);
    fprintf(o, "cr4=%016llx [%s%s%s%s%s ]\n", (unsigned long long)c->cr4, (c->cr4 & (1ull << 5)) ? " PAE" : "",
            (c->cr4 & (1ull << 7)) ? " PGE" : "", (c->cr4 & (1ull << 9)) ? " OSFXSR" : "",
            (c->cr4 & (1ull << 20)) ? " SMEP" : "", (c->cr4 & (1ull << 21)) ? " SMAP" : "");
    fprintf(o, "efer=%016llx [%s%s%s%s ]\n", (unsigned long long)c->efer, (c->efer & 1) ? " SCE" : "",
            (c->efer & (1ull << 8)) ? " LME" : "", (c->efer & (1ull << 10)) ? " LMA" : "",
            (c->efer & (1ull << 11)) ? " NXE" : "");
    for (int i = 0; i < c->msr_count && i < MAX_MSR; i++)
        fprintf(o, "msr %08x = %016llx\n", c->msr[i].idx, (unsigned long long)c->msr[i].val);
}

static void cmd_xmm(machine_t *m, FILE *o, int v) {
    cpu_t *c = dbg_vcpu(m, v);
    for (int i = 0; i < 16; i++)
        fprintf(o, "xmm%-2d %016llx%016llx\n", i, (unsigned long long)c->xmm[i][1], (unsigned long long)c->xmm[i][0]);
    fprintf(o, "mxcsr %08x  fcw %04x\n", c->mxcsr, c->fcw);
}

static void cmd_vcpus(machine_t *m, FILE *o) {
    for (int i = 0; i < dbg_nvcpus(m); i++) {
        cpu_t *c = dbg_vcpu(m, i);
        const char *st = m->n_vcpus <= 1 ? "run" :
            m->vcpu[i].state == VCPU_RUN ? "run" : m->vcpu[i].state == VCPU_OFF ? "off" : "wait-sipi";
        fprintf(o, "%c vcpu%d %-9s %-8s %04x:%016llx halted=%d steps=%llu%s\n",
                i == dbg_selected_vcpu(m) ? '*' : ' ', i, st, cpu_mode_name(c), c->seg[SEG_CS].sel,
                (unsigned long long)c->rip, c->halted, (unsigned long long)c->executed_steps,
                c->fault ? " FAULT" : "");
    }
}

static void cmd_mmio(machine_t *m, FILE *o) {
    for (mmio_region_t *r = m->mmio_list; r; r = r->next)
        fprintf(o, "%016llx-%016llx %s%s\n", (unsigned long long)r->base,
                (unsigned long long)(r->base + r->size - 1), r->name ? r->name : "?", r->enabled ? "" : " (disabled)");
}

static void cmd_info_break(machine_t *m, FILE *o) {
    struct dbg_state *s = m->dbg;
    int any = 0;
    for (int i = 0; i < s->nbp; i++) {
        bp_t *b = &s->bp[i];
        if (b->internal) continue;
        uint64_t off = 0; const char *sym = dbg_symbolize(m, b->lin, &off);
        fprintf(o, "break %d %s%016llx%s hits=%llu", b->id, b->temp ? "(temp) " : "", (unsigned long long)b->lin,
                b->enabled ? "" : " (disabled)", (unsigned long long)b->hits);
        if (sym) fprintf(o, " <%s+0x%llx>", sym, (unsigned long long)off);
        fprintf(o, "\n");
        any = 1;
    }
    for (int i = 0; i < s->nwp; i++) {
        wp_t *w = &s->wp[i];
        fprintf(o, "watch %d %c %016llx len=%d hits=%llu\n", w->id, w->kind, (unsigned long long)w->lin, w->len,
                (unsigned long long)w->hits);
        any = 1;
    }
    if (!any) fprintf(o, "no breakpoints or watchpoints\n");
}

/* x/FMT ADDR: FMT = [count][b|h|w|g][x|d|u|c|i|s] (gdb-style) */
static void cmd_examine(machine_t *m, FILE *o, const char *fmt, const char *expr) {
    int count = 1, size = 4; char f = 'x';
    const char *p = fmt;
    if (isdigit((unsigned char)*p)) { count = atoi(p); while (isdigit((unsigned char)*p)) p++; }
    for (; *p; p++) {
        if (*p == 'b') size = 1; else if (*p == 'h') size = 2; else if (*p == 'w') size = 4; else if (*p == 'g') size = 8;
        else if (strchr("xduics", *p)) f = *p;
    }
    if (count < 1) count = 1;
    if (count > 4096) count = 4096;
    uint64_t a; const char *err;
    int v = dbg_selected_vcpu(m);
    cpu_t *c = dbg_vcpu(m, v);
    if (!expr || !*expr) a = (f == 'i') ? c->seg[SEG_CS].base + c->rip : 0;
    else if (!dbg_eval(m, expr, &a, &err)) { fprintf(o, "error: %s\n", err); return; }
    if (f == 'i') {
        int bits = dbg_code_bits(c);
        uint64_t pc = a - c->seg[SEG_CS].base;
        for (int i = 0; i < count; i++) {
            uint8_t b[16];
            size_t n = dbg_read_lin(m, c, a, b, 15);
            if (!n) { fprintf(o, "   %016llx  <unreadable>\n", (unsigned long long)a); break; }
            dis_insn_t in;
            dis_decode(b, n, bits, pc, &in);
            char hex[40]; size_t h = 0;
            for (int k = 0; k < in.len && h + 2 < sizeof hex; k++) h += (size_t)snprintf(hex + h, sizeof hex - h, "%02x", b[k]);
            uint64_t off = 0; const char *sym = dbg_symbolize(m, a, &off);
            int here = (a == c->seg[SEG_CS].base + c->rip);
            fprintf(o, "%s %016llx", here ? "=>" : "  ", (unsigned long long)a);
            if (sym) fprintf(o, " <%s+0x%llx>", sym, (unsigned long long)off);
            fprintf(o, "  %-20s %s", hex, in.text);
            if ((in.chk == DIS_CHK_TARGET || in.chk == DIS_CHK_COND)) {
                const char *ts = dbg_symbolize(m, c->seg[SEG_CS].base + in.target, &off);
                if (ts) fprintf(o, "  ; <%s+0x%llx>", ts, (unsigned long long)off);
            }
            fprintf(o, "\n");
            a += (uint64_t)in.len; pc += (uint64_t)in.len;
        }
        return;
    }
    if (f == 's') {
        char sbuf[512]; size_t n = dbg_read_lin(m, c, a, sbuf, sizeof sbuf - 1);
        size_t k = 0;
        while (k < n && sbuf[k]) k++;
        sbuf[k] = 0;
        fprintf(o, "%016llx \"%s\"\n", (unsigned long long)a, sbuf);
        return;
    }
    int per = (size == 1) ? 16 : (size == 2) ? 8 : (size == 4) ? 4 : 2;
    for (int i = 0; i < count; i++) {
        if (i % per == 0) fprintf(o, "%s%016llx:", i ? "\n" : "", (unsigned long long)(a + (uint64_t)i * (uint64_t)size));
        uint64_t val = 0;
        if (dbg_read_lin(m, c, a + (uint64_t)i * (uint64_t)size, &val, (size_t)size) != (size_t)size) {
            fprintf(o, " <unreadable>");
            break;
        }
        if (f == 'x') fprintf(o, " 0x%0*llx", size * 2, (unsigned long long)val);
        else if (f == 'd') {
            int64_t sv = size == 1 ? (int8_t)val : size == 2 ? (int16_t)val : size == 4 ? (int32_t)val : (int64_t)val;
            fprintf(o, " %lld", (long long)sv);
        } else if (f == 'u') fprintf(o, " %llu", (unsigned long long)val);
        else if (f == 'c') fprintf(o, " '%c'", (val >= 32 && val < 127) ? (int)val : '.');
    }
    fprintf(o, "\n");
}

static void cmd_xlate(machine_t *m, FILE *o, const char *expr) {
    uint64_t a; const char *err;
    if (!dbg_eval(m, expr, &a, &err)) { fprintf(o, "error: %s\n", err); return; }
    cpu_t *c = dbg_vcpu(m, dbg_selected_vcpu(m));
    if (!(c->cr0 & (1ull << 31))) { fprintf(o, "paging off: linear %016llx = physical\n", (unsigned long long)a); return; }
    uint64_t base = c->cr3 & ~0xFFFull;
    static const char *const lvl[4] = {"pml4e", "pdpte", "pde", "pte"};
    for (int L = 0; L < 4; L++) {
        int idx = (int)((a >> (39 - 9 * L)) & 0x1FF);
        uint64_t e = 0;
        if (dbg_read_phys(m, base + (uint64_t)idx * 8, &e, 8) != 8) { fprintf(o, "%s @%016llx unreadable\n", lvl[L], (unsigned long long)(base + (uint64_t)idx * 8)); return; }
        fprintf(o, "%s[%3d] @%016llx = %016llx %s%s%s%s%s\n", lvl[L], idx, (unsigned long long)(base + (uint64_t)idx * 8),
                (unsigned long long)e, (e & 1) ? "P" : "-", (e & 2) ? "W" : "-", (e & 4) ? "U" : "-",
                (L > 0 && L < 3 && (e & 0x80)) ? " PS" : "", (e >> 63) ? " NX" : "");
        if (!(e & 1)) { fprintf(o, "not present\n"); return; }
        if (L > 0 && L < 3 && (e & 0x80)) break;
        base = e & 0x000FFFFFFFFFF000ULL;
    }
    uint64_t p;
    if (dbg_translate(m, c, a, &p)) fprintf(o, "linear %016llx -> physical %016llx\n", (unsigned long long)a, (unsigned long long)p);
}

static void cmd_bt(machine_t *m, FILE *o, int max) {
    cpu_t *c = dbg_vcpu(m, dbg_selected_vcpu(m));
    int bits = dbg_code_bits(c), w = bits == 64 ? 8 : 4;
    uint64_t pc = c->seg[SEG_CS].base + c->rip, fp = c->gpr[RBP], off = 0;
    const char *sym = dbg_symbolize(m, pc, &off);
    fprintf(o, "#0  %016llx", (unsigned long long)pc);
    if (sym) fprintf(o, " <%s+0x%llx>", sym, (unsigned long long)off);
    fprintf(o, "\n");
    for (int i = 1; i < max && fp; i++) {
        uint64_t nfp = 0, ret = 0;
        if (dbg_read_lin(m, c, c->seg[SEG_SS].base + fp, &nfp, (size_t)w) != (size_t)w ||
            dbg_read_lin(m, c, c->seg[SEG_SS].base + fp + (uint64_t)w, &ret, (size_t)w) != (size_t)w) break;
        if (!ret) break;
        sym = dbg_symbolize(m, ret, &off);
        fprintf(o, "#%-2d %016llx", i, (unsigned long long)ret);
        if (sym) fprintf(o, " <%s+0x%llx>", sym, (unsigned long long)off);
        fprintf(o, "\n");
        if (nfp <= fp) break;   /* frames must grow toward higher addresses */
        fp = nfp;
    }
}

static void cmd_log(machine_t *m, FILE *o, int n) {
    if (n <= 0) n = 20;
    int start = m->log.count < n ? 0 : m->log.count - n;
    for (int i = start; i < m->log.count; i++) {
        int idx = (m->log.head - m->log.count + i + LOG_RING) % LOG_RING;
        fprintf(o, "%s\n", m->log.lines[idx]);
    }
}

static const char *const help_text =
    "run control: c|continue  s|si|stepi [N]  n|ni|nexti [N]  until VTIME|+N  detach  q|quit\n"
    "breakpoints: b|break ADDR  tb|tbreak ADDR  d|delete [ID..]  disable ID  enable ID\n"
    "watchpoints: w|watch [w|r|a] ADDR [LEN]      (linear; stop after the access)\n"
    "inspect:     r|regs  info regs|sregs|cr|xmm|vcpu|break|mmio|where  x/FMT ADDR  x/Ni ADDR\n"
    "             p|print EXPR  xlate ADDR  bt [N]  log [N]  sym ADDR  where\n"
    "modify:      set $REG = EXPR   poke ADDR VALUE [SIZE]   (RAM only)\n"
    "snapshots:   savevm FILE   loadvm FILE   (one vCPU; taken between instructions)\n"
    "             vcpu N\n"
    "ADDR/EXPR:   numbers, $reg, symbols (--symbols/--kernel ELF), + - * ( ), *ADDR (8-byte load)\n"
    "             $rip = the raw instruction pointer; $lin = its LINEAR address\n"
    "             (cs.base + rip).  In real mode they differ (reset vector:\n"
    "             $rip=0xfff0, $lin=0xfffffff0), so `x/i` with NO address uses\n"
    "             $lin, while `x/i $rip` reads the raw IP -- use $lin there.\n";

static char *trim(char *s) {
    while (*s == ' ' || *s == '\t') s++;
    size_t L = strlen(s);
    while (L && (s[L-1] == '\n' || s[L-1] == '\r' || s[L-1] == ' ' || s[L-1] == '\t')) s[--L] = 0;
    return s;
}

int dbg_exec(machine_t *m, const char *line_in, FILE *o) {
    struct dbg_state *s = dbg_attach(m);
    if (!s) return DBG_ACT_NONE;
    char buf[1024];
    snprintf(buf, sizeof buf, "%s", line_in);
    char *line = trim(buf);
    if (!*line || *line == '#') return DBG_ACT_NONE;
    char cmd[64]; size_t k = 0;
    const char *p = line;
    while (*p && *p != ' ' && *p != '\t' && k < sizeof cmd - 1) cmd[k++] = *p++;
    cmd[k] = 0;
    char *args = trim((char *)p);
    int v = s->sel_vcpu;
    uint64_t val; const char *err;
    #define IS(x) (!strcmp(cmd, x))
    if (IS("help") || IS("h") || IS("?")) { fputs(help_text, o); return DBG_ACT_NONE; }
    if (IS("c") || IS("cont") || IS("continue")) return DBG_ACT_CONTINUE;
    if (IS("s") || IS("si") || IS("stepi") || IS("step")) {
        uint64_t n = 1;
        if (*args && !dbg_eval(m, args, &n, &err)) { fprintf(o, "error: %s\n", err); return DBG_ACT_NONE; }
        s->pend_steps = n ? n : 1; s->pend_vcpu = v;
        return DBG_ACT_STEP;
    }
    if (IS("n") || IS("ni") || IS("nexti") || IS("next")) {
        cpu_t *c = dbg_vcpu(m, v);
        uint64_t lin = c->seg[SEG_CS].base + c->rip;
        uint8_t b[16]; size_t n = dbg_read_lin(m, c, lin, b, 15);
        dis_insn_t in; memset(&in, 0, sizeof in);
        if (n) dis_decode(b, n, dbg_code_bits(c), c->rip, &in);
        int is_call = in.valid && (!strncmp(in.text, "call", 4) || strstr(in.text, " call") ||
                                   !strncmp(in.text, "int ", 4) || !strcmp(in.text, "int3"));
        if (is_call && !*args) {
            break_add_internal(s, lin + (uint64_t)in.len);
            return DBG_ACT_CONTINUE;
        }
        uint64_t cnt = 1;
        if (*args && !dbg_eval(m, args, &cnt, &err)) { fprintf(o, "error: %s\n", err); return DBG_ACT_NONE; }
        s->pend_steps = cnt ? cnt : 1; s->pend_vcpu = v;
        return DBG_ACT_STEP;
    }
    if (IS("until") || IS("u")) {
        if (!*args) { fprintf(o, "usage: until VTIME | until +N\n"); return DBG_ACT_NONE; }
        int rel = (*args == '+');
        if (!dbg_eval(m, args + rel, &val, &err)) { fprintf(o, "error: %s\n", err); return DBG_ACT_NONE; }
        s->until_vtime = rel ? m->vtime_instr + val : val;
        if (s->until_vtime <= m->vtime_instr) { fprintf(o, "vtime %llu already reached\n", (unsigned long long)m->vtime_instr); s->until_vtime = 0; return DBG_ACT_NONE; }
        return DBG_ACT_CONTINUE;
    }
    if (IS("detach")) return DBG_ACT_DETACH;
    if (IS("q") || IS("quit") || IS("kill")) return DBG_ACT_QUIT;
    if (IS("b") || IS("break") || IS("tb") || IS("tbreak")) {
        const char *a = *args == '*' ? args + 1 : args;
        if (!*a) { cpu_t *c = dbg_vcpu(m, v); val = c->seg[SEG_CS].base + c->rip; }
        else if (!dbg_eval(m, a, &val, &err)) { fprintf(o, "error: %s\n", err); return DBG_ACT_NONE; }
        int id = dbg_break_add(m, val, cmd[0] == 't');
        if (id < 0) fprintf(o, "error: too many breakpoints\n");
        else {
            uint64_t off = 0; const char *sym = dbg_symbolize(m, val, &off);
            fprintf(o, "%sbreakpoint %d at 0x%016llx", cmd[0] == 't' ? "temporary " : "", id, (unsigned long long)val);
            if (sym) fprintf(o, " <%s+0x%llx>", sym, (unsigned long long)off);
            fprintf(o, "\n");
        }
        return DBG_ACT_NONE;
    }
    if (IS("d") || IS("delete")) {
        if (!*args) { s->nbp = 0; s->nwp = 0; cpu_dbg_access = NULL; fprintf(o, "deleted all breakpoints and watchpoints\n"); return DBG_ACT_NONE; }
        char *save, *tok = strtok_r(args, " \t", &save);
        while (tok) {
            int id = atoi(tok);
            if (dbg_break_del(m, id) && dbg_watch_del(m, id)) fprintf(o, "no breakpoint %d\n", id);
            tok = strtok_r(NULL, " \t", &save);
        }
        return DBG_ACT_NONE;
    }
    if (IS("disable") || IS("enable")) {
        int id = atoi(args), found = 0;
        for (int i = 0; i < s->nbp; i++) if (s->bp[i].id == id && !s->bp[i].internal) { s->bp[i].enabled = IS("enable"); found = 1; }
        if (!found) fprintf(o, "no breakpoint %d\n", id);
        return DBG_ACT_NONE;
    }
    if (IS("w") || IS("watch") || IS("rwatch") || IS("awatch")) {
        int kind = IS("rwatch") ? 'r' : IS("awatch") ? 'a' : 'w';
        char *a = args;
        if ((a[0] == 'r' || a[0] == 'w' || a[0] == 'a') && (a[1] == ' ' || a[1] == '\t')) { kind = a[0]; a = trim(a + 1); }
        char *sp = strrchr(a, ' ');
        int len = 1;
        char ab[256]; snprintf(ab, sizeof ab, "%s", a);
        if (sp) {
            uint64_t l2;
            char *ls = ab + (sp - a);
            *ls = 0;
            if (dbg_eval(m, ls + 1, &l2, &err)) len = (int)l2;
            else { *ls = ' '; }
        }
        if (!dbg_eval(m, *ab == '*' ? ab + 1 : ab, &val, &err)) { fprintf(o, "error: %s\n", err); return DBG_ACT_NONE; }
        int id = dbg_watch_add(m, val, len, kind);
        if (id < 0) fprintf(o, "error: cannot add watchpoint\n");
        else fprintf(o, "watchpoint %d (%c) at 0x%016llx len %d\n", id, kind, (unsigned long long)val, len);
        return DBG_ACT_NONE;
    }
    if (IS("r") || IS("regs") || IS("registers")) { cmd_regs(m, o, v); return DBG_ACT_NONE; }
    if (IS("where")) { print_where(m, o, v); return DBG_ACT_NONE; }
    if (IS("i") || IS("info")) {
        if (!strncmp(args, "r", 1) && strncmp(args, "reg", 3) != 0 && args[1] == 0) { cmd_regs(m, o, v); return DBG_ACT_NONE; }
        if (!strcmp(args, "regs") || !strcmp(args, "registers") || !strcmp(args, "r")) cmd_regs(m, o, v);
        else if (!strcmp(args, "sregs") || !strcmp(args, "seg")) cmd_sregs(m, o, v);
        else if (!strcmp(args, "cr") || !strcmp(args, "control")) cmd_cr(m, o, v);
        else if (!strcmp(args, "xmm") || !strcmp(args, "sse")) cmd_xmm(m, o, v);
        else if (!strcmp(args, "vcpu") || !strcmp(args, "vcpus") || !strcmp(args, "threads")) cmd_vcpus(m, o);
        else if (!strncmp(args, "b", 1) || !strncmp(args, "w", 1)) cmd_info_break(m, o);
        else if (!strcmp(args, "mmio")) cmd_mmio(m, o);
        else if (!strcmp(args, "where") || !strcmp(args, "pc")) print_where(m, o, v);
        else if (!strncmp(args, "symbol", 6)) {
            const char *a = trim(args + 6);
            if (!dbg_eval(m, a, &val, &err)) fprintf(o, "error: %s\n", err);
            else { uint64_t off = 0; const char *sym = dbg_symbolize(m, val, &off);
                   if (sym) fprintf(o, "%s+0x%llx\n", sym, (unsigned long long)off); else fprintf(o, "no symbol\n"); }
        }
        else fprintf(o, "info: regs sregs cr xmm vcpu break mmio where symbol ADDR\n");
        return DBG_ACT_NONE;
    }
    if (cmd[0] == 'x' && (cmd[1] == 0 || cmd[1] == '/')) { cmd_examine(m, o, cmd[1] ? cmd + 2 : "", args); return DBG_ACT_NONE; }
    if (IS("p") || IS("print") || !strncmp(cmd, "p/", 2) || !strncmp(cmd, "print/", 6)) {
        if (!dbg_eval(m, args, &val, &err)) { fprintf(o, "error: %s\n", err); return DBG_ACT_NONE; }
        uint64_t off = 0; const char *sym = dbg_symbolize(m, val, &off);
        fprintf(o, "0x%llx (%lld)", (unsigned long long)val, (long long)val);
        if (sym) fprintf(o, " <%s+0x%llx>", sym, (unsigned long long)off);
        fprintf(o, "\n");
        return DBG_ACT_NONE;
    }
    if (IS("sym")) {
        if (!dbg_eval(m, args, &val, &err)) { fprintf(o, "error: %s\n", err); return DBG_ACT_NONE; }
        uint64_t off = 0; const char *sym = dbg_symbolize(m, val, &off);
        if (sym) fprintf(o, "%s+0x%llx\n", sym, (unsigned long long)off); else fprintf(o, "no symbol\n");
        return DBG_ACT_NONE;
    }
    if (IS("set")) {
        char *eq = strchr(args, '=');
        char rn[32];
        const char *ve;
        if (eq) {
            size_t L = (size_t)(eq - args);
            if (L >= sizeof rn) L = sizeof rn - 1;
            memcpy(rn, args, L); rn[L] = 0;
            ve = eq + 1;
        } else {
            char *sp = strchr(args, ' ');
            if (!sp) { fprintf(o, "usage: set $REG = EXPR\n"); return DBG_ACT_NONE; }
            size_t L = (size_t)(sp - args);
            if (L >= sizeof rn) L = sizeof rn - 1;
            memcpy(rn, args, L); rn[L] = 0;
            ve = sp + 1;
        }
        char *r = trim(rn);
        if (!dbg_eval(m, ve, &val, &err)) { fprintf(o, "error: %s\n", err); return DBG_ACT_NONE; }
        if (!dbg_reg_set(m, v, r, val)) fprintf(o, "error: register %s is not writable\n", r);
        else fprintf(o, "%s = 0x%llx\n", r, (unsigned long long)val);
        return DBG_ACT_NONE;
    }
    if (IS("poke")) {
        char a1[128] = "", a2[128] = "", a3[32] = "";
        if (sscanf(args, "%127s %127s %31s", a1, a2, a3) < 2) { fprintf(o, "usage: poke ADDR VALUE [SIZE]\n"); return DBG_ACT_NONE; }
        uint64_t addr, sz = 1;
        if (!dbg_eval(m, a1, &addr, &err) || !dbg_eval(m, a2, &val, &err) || (a3[0] && !dbg_eval(m, a3, &sz, &err))) {
            fprintf(o, "error: %s\n", err); return DBG_ACT_NONE;
        }
        if (sz != 1 && sz != 2 && sz != 4 && sz != 8) { fprintf(o, "error: size must be 1, 2, 4 or 8\n"); return DBG_ACT_NONE; }
        size_t n = dbg_write_lin(m, dbg_vcpu(m, v), addr, &val, (size_t)sz);
        if (n != sz) fprintf(o, "error: wrote %zu of %llu bytes (RAM only)\n", n, (unsigned long long)sz);
        else fprintf(o, "poked %llu byte(s) at 0x%llx\n", (unsigned long long)sz, (unsigned long long)addr);
        return DBG_ACT_NONE;
    }
    if (IS("savevm") || IS("loadvm")) {
        int op = IS("loadvm") ? 2 : 1;
        if (!*args) { fprintf(o, "usage: %s FILE\n", cmd); return DBG_ACT_NONE; }
        if (!dbg_snap_hook) { fprintf(o, "error: snapshots are not available here\n"); return DBG_ACT_NONE; }
        if (dbg_nvcpus(m) > 1) {
            fprintf(o, "error: %s inside a debug session supports one vCPU; use --snapshot-save/--snapshot-load\n", cmd);
            return DBG_ACT_NONE;
        }
        if (s->run_ended) { fprintf(o, "error: the run has ended\n"); return DBG_ACT_NONE; }
        snprintf(s->snap_path, sizeof s->snap_path, "%s", args);
        if (s->cur_site == SITE_POST) {
            int rc = do_snap(m, s, op, args);
            fprintf(o, "%s %s %s\n", cmd, args, rc == 0 ? "done" : "FAILED");
            if (op == 2 && rc == 0) print_where(m, o, s->sel_vcpu);
            return DBG_ACT_NONE;
        }
        /* a boundary stop is inside cpu_step (interrupt delivered, fetch
         * pending): a snapshot is only consistent between steps, so finish
         * this instruction first and stop again right after it */
        s->snap_op = op;
        fprintf(o, "%s deferred: completing the current instruction, then stopping between steps\n", cmd);
        return DBG_ACT_CONTINUE;
    }
    if (IS("xlate")) { cmd_xlate(m, o, args); return DBG_ACT_NONE; }
    if (IS("bt") || IS("backtrace")) { cmd_bt(m, o, *args ? atoi(args) : 16); return DBG_ACT_NONE; }
    if (IS("log")) { cmd_log(m, o, *args ? atoi(args) : 20); return DBG_ACT_NONE; }
    if (IS("vcpu") || IS("thread")) {
        if (!*args) { cmd_vcpus(m, o); return DBG_ACT_NONE; }
        int n = atoi(args);
        if (n < 0 || n >= dbg_nvcpus(m)) { fprintf(o, "error: no vcpu %d\n", n); return DBG_ACT_NONE; }
        s->sel_vcpu = n;
        fprintf(o, "vcpu %d selected\n", n);
        print_where(m, o, n);
        return DBG_ACT_NONE;
    }
    #undef IS
    fprintf(o, "unknown command '%s' (try help)\n", cmd);
    return DBG_ACT_NONE;
}

/* ---- the REPL front end ---- */
typedef struct {
    dbg_frontend_t fe;
    FILE *in, *out;
    int interactive;
    int prompted;
} repl_t;

static const char *stop_name(dbg_stop_reason_t r) {
    switch (r) {
    case DBG_STOP_START: return "start";
    case DBG_STOP_BREAK: return "breakpoint";
    case DBG_STOP_STEP: return "step";
    case DBG_STOP_WATCH: return "watchpoint";
    case DBG_STOP_PAUSE: return "pause";
    case DBG_STOP_UNTIL: return "until";
    case DBG_STOP_END: return "run ended";
    default: return "?";
    }
}

static void repl_on_stop(machine_t *m, dbg_frontend_t *fe, const dbg_stop_t *st) {
    repl_t *r = fe->ctx;
    FILE *o = r->out;
    cpu_t *c = dbg_vcpu(m, st->vcpu);
    fprintf(o, "[dbg] stopped: %s", stop_name(st->reason));
    if (st->reason == DBG_STOP_BREAK) fprintf(o, " %d", st->bp_id);
    if (st->reason == DBG_STOP_WATCH)
        fprintf(o, " %d (%c) addr=0x%llx old=0x%llx new=0x%llx", st->watch_id, st->watch_kind,
                (unsigned long long)st->watch_addr, (unsigned long long)st->watch_old, (unsigned long long)st->watch_new);
    if (st->reason == DBG_STOP_END) fprintf(o, " (%s)", st->end_why ? st->end_why : "?");
    fprintf(o, " vcpu=%d vtime=%llu steps=%llu\n", st->vcpu, (unsigned long long)m->vtime_instr,
            (unsigned long long)c->executed_steps);
    print_where(m, o, st->vcpu);
    fflush(o);
    r->prompted = 0;
}

static int repl_stop_fd(machine_t *m, dbg_frontend_t *fe) {
    repl_t *r = fe->ctx;
    (void)m;
    if (!r->interactive) return -1;
    if (!r->prompted) { fprintf(r->out, "(x86dbg) "); fflush(r->out); r->prompted = 1; }
    return fileno(r->in);
}

static void repl_service(machine_t *m, dbg_frontend_t *fe) {
    repl_t *r = fe->ctx;
    char line[1024];
    r->prompted = 0;
    if (!fgets(line, sizeof line, r->in)) {
        fprintf(r->out, "[dbg] end of %s: detaching (the run continues)\n", r->interactive ? "input" : "script");
        dbg_resume(m, DBG_ACT_DETACH, 0, -1);
        dbg_remove_frontend(m, fe);
        return;
    }
    if (!r->interactive) { char *t = trim(line); if (*t) fprintf(r->out, "(x86dbg) %s\n", t); }
    int act = dbg_exec(m, line, r->out);
    fflush(r->out);
    if (act == DBG_ACT_DETACH) {
        fprintf(r->out, "[dbg] detached\n");
        dbg_resume(m, DBG_ACT_DETACH, 0, -1);
        dbg_remove_frontend(m, fe);
        return;
    }
    if (act != DBG_ACT_NONE) dbg_resume(m, (dbg_action_t)act, 0, -1);
}

static void repl_detach(machine_t *m, dbg_frontend_t *fe) {
    (void)m;
    repl_t *r = fe->ctx;
    if (!r->interactive && r->in) fclose(r->in);
    free(r);
}

int dbg_repl_attach(machine_t *m, FILE *in, int interactive, FILE *out) {
    repl_t *r = calloc(1, sizeof *r);
    if (!r) return -1;
    r->in = in; r->out = out; r->interactive = interactive;
    r->fe.name = "repl";
    r->fe.ctx = r;
    r->fe.on_stop = repl_on_stop;
    r->fe.stop_fd = repl_stop_fd;
    r->fe.service = repl_service;
    r->fe.on_detach = repl_detach;
    r->fe.wants_end_stop = 1;
    if (dbg_add_frontend(m, &r->fe) != 0) { free(r); return -1; }
    return 0;
}
