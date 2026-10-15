// dbg.h -- TOOLING: the tooling core (trace, inspection, run control).
//
// Everything here is opt-in: nothing runs unless main() attached the core
// (m->dbg != NULL), and nothing here changes guest-visible state unless a
// user explicitly writes registers or memory.
//
// Inspection is side-effect-free by construction (TOOLING_PLAN rule 3):
// dbg_translate() mirrors cpu.c translate() -- 4-level walk, PS leaves,
// the same masks -- but raises no #PF, fills no P2 TLB entry and reads
// page tables only from RAM/ROM; dbg_read_phys() never calls an MMIO
// callback. Debugger and trace reads therefore cannot perturb a run.
//
// Run control (T2) is the only stop/resume machinery (TOOLING_PLAN rule
// 4): the REPL, the gdbstub (T3) and the monitor (T5) are front ends that
// receive stops and return resume actions; none of them calls cpu_step.
#ifndef DBG_H
#define DBG_H
#include <stdio.h>
#include <stdint.h>
#include <stddef.h>
#include "machine.h"

/* ---- side-effect-free inspection ------------------------------------- */
int    dbg_code_bits(const cpu_t *c);                 /* 16 / 32 / 64          */
int    dbg_translate(machine_t *m, const cpu_t *c, uint64_t lin, uint64_t *phys);
size_t dbg_read_phys(machine_t *m, uint64_t phys, void *buf, size_t n);
size_t dbg_read_lin(machine_t *m, const cpu_t *c, uint64_t lin, void *buf, size_t n);
/* RAM-only writes for debuggers; they flush the P2/P3 caches. */
size_t dbg_write_phys(machine_t *m, uint64_t phys, const void *buf, size_t n);
size_t dbg_write_lin(machine_t *m, const cpu_t *c, uint64_t lin, const void *buf, size_t n);
/* the context of vCPU i as tools see it (live m->cpu for the loaded one) */
cpu_t *dbg_vcpu(machine_t *m, int i);
int    dbg_nvcpus(machine_t *m);

/* ---- attach / detach --------------------------------------------------- */
struct dbg_state *dbg_attach(machine_t *m);
void dbg_detach(machine_t *m);

/* ---- T1: disassembly trace -------------------------------------------- */
typedef struct {
    uint64_t lines;         /* instruction lines written                    */
    uint64_t fall;          /* fall-through successors checked              */
    uint64_t branch;        /* direct-branch successors checked             */
    uint64_t mismatches;    /* successor RIP != what the decode predicts    */
    uint64_t undecodable;   /* executed bytes the disassembler rejects      */
    uint64_t unreadable;    /* code bytes not readable side-effect-free     */
} dbg_trace_stats_t;
int  dbg_trace_open(machine_t *m, const char *path, uint64_t from_vtime, uint64_t max_lines);
void dbg_trace_close(machine_t *m, int print_summary);
int  dbg_trace_stats(machine_t *m, dbg_trace_stats_t *out);

/* one-line disassembly at a linear address (no side effects) */
int  dbg_disas_at(machine_t *m, const cpu_t *c, uint64_t lin, int bits, uint64_t pc,
                  char *out, size_t outsz, int *len_out);

/* ---- T2: run control ---------------------------------------------------- */
typedef enum {
    DBG_STOP_NONE = 0, DBG_STOP_START, DBG_STOP_BREAK, DBG_STOP_STEP,
    DBG_STOP_WATCH, DBG_STOP_PAUSE, DBG_STOP_UNTIL, DBG_STOP_END
} dbg_stop_reason_t;

typedef enum {   /* what a front end asks for when it leaves a stop */
    DBG_ACT_NONE = 0, DBG_ACT_CONTINUE, DBG_ACT_STEP, DBG_ACT_DETACH, DBG_ACT_QUIT
} dbg_action_t;

typedef struct {
    dbg_stop_reason_t reason;
    int vcpu;
    uint64_t lin;            /* CS.base + RIP of the stopping vCPU          */
    int bp_id;               /* DBG_STOP_BREAK                              */
    uint64_t watch_addr;     /* DBG_STOP_WATCH: the accessed linear address */
    int watch_kind;          /* 'r' / 'w' / 'a'                             */
    int watch_id;
    uint64_t watch_old, watch_new;
    int watch_size;
    int end_rc;              /* DBG_STOP_END: cpu_step result               */
    const char *end_why;
} dbg_stop_t;

/* Front ends (REPL, gdbstub, monitor) register with the core. */
typedef struct dbg_frontend {
    const char *name;
    void *ctx;
    /* a stop happened: announce it (print banner / send stop reply) */
    void (*on_stop)(machine_t *m, struct dbg_frontend *fe, const dbg_stop_t *st);
    /* wait for work while stopped: return an fd to poll (or -1 for a
     * frontend that reads synchronously, e.g. a script file) */
    int  (*stop_fd)(machine_t *m, struct dbg_frontend *fe);
    /* service input while stopped; may set a resume action via dbg_resume */
    void (*service)(machine_t *m, struct dbg_frontend *fe);
    /* periodic poll while running (async stop requests, monitor I/O) */
    void (*poll_running)(machine_t *m, struct dbg_frontend *fe);
    /* the run is over (after the final DBG_STOP_END service loop) */
    void (*on_detach)(machine_t *m, struct dbg_frontend *fe);
    int  wants_end_stop;     /* keep serving after the run ended            */
} dbg_frontend_t;

int  dbg_add_frontend(machine_t *m, dbg_frontend_t *fe);
void dbg_remove_frontend(machine_t *m, dbg_frontend_t *fe);
int  dbg_frontends(machine_t *m);

/* Called by main()'s run loop. */
void dbg_run_start(machine_t *m, int stop_at_start);
void dbg_post_step(machine_t *m);           /* after every cpu_step         */
void dbg_run_end(machine_t *m, int step_result);
void dbg_request_pause(machine_t *m);       /* async (signal-safe flag)     */

/* resume from inside a stop (front ends call this). steps == 0 and
 * vcpu < 0 keep what dbg_exec() parsed during this stop (stepi N, vcpu). */
void dbg_resume(machine_t *m, dbg_action_t act, uint64_t steps, int vcpu);

/* breakpoints / watchpoints (linear addresses) */
int  dbg_break_add(machine_t *m, uint64_t lin, int temp);       /* id or -1 */
int  dbg_break_del(machine_t *m, int id);                        /* 0 ok     */
int  dbg_break_del_addr(machine_t *m, uint64_t lin);
int  dbg_watch_add(machine_t *m, uint64_t lin, int len, int kind);
int  dbg_watch_del(machine_t *m, int id);
int  dbg_watch_del_addr(machine_t *m, uint64_t lin, int len, int kind);
void dbg_set_until(machine_t *m, uint64_t vtime);

/* the REPL command interpreter (also gdb `monitor` and the T5 monitor) */
int  dbg_exec(machine_t *m, const char *line, FILE *out);  /* DBG_ACT_*   */
int  dbg_selected_vcpu(machine_t *m);
const dbg_stop_t *dbg_last_stop(machine_t *m);

/* REPL front end: stdin (interactive) or a script file */
int  dbg_repl_attach(machine_t *m, FILE *in, int interactive, FILE *out);

/* TOOLING T4: savevm/loadvm reach src/snapshot.c through this hook (set
 * by main; NULL in narrow harnesses). load != 0 replaces the machine. */
extern int (*dbg_snap_hook)(machine_t *m, const char *path, int load);

/* symbols (ELF symtab of --kernel / --symbols) */
int  dbg_symbols_load(machine_t *m, const char *elf_path);
int  dbg_symbol_lookup(machine_t *m, const char *name, uint64_t *addr);
const char *dbg_symbolize(machine_t *m, uint64_t addr, uint64_t *off);

/* register access by name (64-bit value; sub-registers handled) */
int  dbg_reg_get(machine_t *m, int vcpu, const char *name, uint64_t *v);
int  dbg_reg_set(machine_t *m, int vcpu, const char *name, uint64_t v);
int  dbg_eval(machine_t *m, const char *expr, uint64_t *v, const char **err);

#endif
