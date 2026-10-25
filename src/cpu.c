// cpu.c -- x86 16/32/64-bit interpreter core (real/protected/long mode,
// segmentation, 4-level paging, exceptions). Scoped to the integer ISA
// subset needed by firmware/bootloader-style code, built on a fully
// generic ModRM/SIB decoder so it is straightforward to extend further.
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <stdlib.h>
#include "cpu.h"
#include <math.h>
#include <stdint.h>
#include "machine.h"
#include <fenv.h>
#include <math.h>
#include "platform.h"
/* USB U3: device-cadence hook (devices_tick) -- installed by devices_init.
 * Narrow unit harnesses link cpu.c without devices.c and leave it NULL. */
void (*cpu_devices_tick)(machine_t *m);
/* TOOLING T1/T2: instruction-boundary hook (src/dbg.c); see cpu.h. */
int (*cpu_dbg_boundary)(machine_t *m);
/* TOOLING T2: watchpoint access hook (src/dbg.c); see cpu.h. */
void (*cpu_dbg_access)(machine_t *m, uint64_t lin, int size, int write);

/* USB U3: instruction-cadence divider for devices_tick (deterministic). */
static uint32_t s_devtick;
uint32_t cpu_devtick_get(void) { return s_devtick; }       /* TOOLING T4 */
void     cpu_devtick_set(uint32_t v) { s_devtick = v; }

/* RFLAGS bits a guest may control via POPF/IRET (IOPL/NT/RF/AC/ID included;
 * VM/VIF/VIP and reserved bits excluded; bit 1 is architecturally 1). */
#define RFLAGS_WRITABLE 0x00257FD5ull

/* ============================= misc helpers ============================= */

static void faultf(cpu_t *c, const char *fmt, ...) {
    va_list ap; va_start(ap, fmt);
    vsnprintf(c->fault_msg, sizeof c->fault_msg, fmt, ap);
    va_end(ap);
    c->fault = 1;
    c->fault_rip = c->rip;
}

int cpu_addr_size(cpu_t *c) {
    int lma = (int)((c->efer >> 10) & 1);
    if (lma && c->seg[SEG_CS].l) return 64;
    if (c->cr0 & 1) return c->seg[SEG_CS].d_b ? 32 : 16;
    return 16;
}
/* NOTE: unlike cpu_addr_size() (which is bit-width: 16/32/64, matching how
 * decode_modrm's addressing-mode tables are keyed), this returns a *byte*
 * count (2/4), matching how every opsize-consuming instruction handler in
 * cpu_step() below treats it (fetch_imm(d,size), rm_read/rm_write(...,size),
 * reg_mask(size) etc. all expect bytes, not bits). */
int cpu_op_size_default(cpu_t *c) { return cpu_addr_size(c) == 16 ? 2 : 4; }

const char *cpu_mode_name(cpu_t *c) {
    int lma = (int)((c->efer >> 10) & 1);
    if (lma && c->seg[SEG_CS].l) return "long64";
    if (lma) return "compat32";
    if (c->cr0 & 1) return c->seg[SEG_CS].d_b ? "prot32" : "prot16";
    return "real16";
}

uint64_t cpu_get_msr(cpu_t *c, uint32_t idx, int *found) {
    for (int i = 0; i < c->msr_count; i++) if (c->msr[i].idx == idx) { if (found) *found = 1; return c->msr[i].val; }
    if (found) *found = 0;
    return 0;
}
void cpu_set_msr(cpu_t *c, uint32_t idx, uint64_t val) {
    for (int i = 0; i < c->msr_count; i++) if (c->msr[i].idx == idx) { c->msr[i].val = val; return; }
    if (c->msr_count < MAX_MSR) { c->msr[c->msr_count].idx = idx; c->msr[c->msr_count].val = val; c->msr_count++; }
}

/* ======================= paging (4-level, long mode) ===================== */
static void raise_exception(machine_t *m, int vector, int has_err, uint32_t err);
static int cpl_now(const cpu_t *c) { return c->seg[SEG_CS].sel & 3; }

/* ==================== guest-bring-up diagnostics (A6) =====================
 *
 * A6 (audit fix): this core is documented as "firmware-agnostic -- it contains
 * no hacks tied to a specific ROM" (docs/architecture.md, "What this is").
 * The K4
 * bring-up probes below broke that promise in a way that shipped in every
 * release build:
 *
 *   * cpu_step() compared RIP against three literal AuraLite-OS kernel
 *     addresses (0xffffffff801907be / ...7d3 / 0xffffffff80106f85) once per
 *     instruction;
 *   * write_mem_v(), i.e. EVERY guest store, range-compared the linear
 *     address against 0x7fffefffa000..0x7fffefffc000 and against a GS window,
 *     and formatted log lines;
 *   * the SYSCALL/SYSRET arms opened and wrote "/tmp/k4-probe.txt" -- a fixed
 *     host path, from inside the interpreter, with the FILE* never closed
 *     (an fd leak per machine), on an assumption (a writable /tmp) the
 *     emulator has no right to make;
 *   * raise_exception() and fetch8()/translate() consulted getenv().
 *
 * None of it was reachable unless EMU_DBG_U=1, but the constants, the
 * comparisons and the /tmp string were in the shipped binary -- and the three
 * RIPs are stale the moment that kernel is rebuilt.
 *
 * The facility is kept, because it is genuinely useful while bringing up a new
 * guest, but it is now a COMPILE-TIME option:
 *
 *     make DEBUG_HOOKS=1        (adds -DAURA_DEBUG_HOOKS=1)
 *
 * With AURA_DEBUG_HOOKS=0 (the default) dbg_u_on can never become 1, so every
 * `if (dbg_u_on > 0)` site below folds away and the release binary carries
 * neither the guest addresses nor the /tmp path; `strings x86emu |
 * grep k4-probe` is the regression vector (tests/test_cli_inputs.py).
 *
 * The lazy getenv() resolution itself is deliberately NOT gated: dbg_u_on
 * flips -1 -> 0 on the first fetch, and that transition is what gates the P3
 * predecode (PERF P3) and is carried in snapshots.  Pinning it at compile time
 * would change decode_hits by one on the first instruction and move every
 * recorded --stats receipt.
 */
#ifndef AURA_DEBUG_HOOKS
#define AURA_DEBUG_HOOKS 0
#endif

static int dbg_u_on = -1;  /* K4: EMU_DBG_U=1 knob, resolved lazily (fetch8) */
/* TOOLING T4: whether that lazy lookup has run is internal path state --
 * while it is -1 the P3 predecode is bypassed -- so a snapshot carries it
 * (measured: without this, a restored run lost one decode hit). */
#if AURA_DEBUG_HOOKS
static void dbg_u_rips_load(void);
#endif
int  cpu_dbg_knob_resolved(void) { return dbg_u_on >= 0; }
void cpu_dbg_knob_resolve(void) {
    if (dbg_u_on < 0) { const char *e = getenv("EMU_DBG_U"); dbg_u_on = (e && e[0] == '1'); }
#if AURA_DEBUG_HOOKS
    if (dbg_u_on > 0) dbg_u_rips_load();   /* A6: guest watch list, once */
#endif
}

#if AURA_DEBUG_HOOKS
/* A6: the guest-specific watch list, named and in one place instead of three
 * literals buried in cpu_step().  These are AuraLite-OS kernel addresses and
 * are meaningful only for that build -- override with EMU_DBG_U_RIPS
 * (comma-separated hex) when the kernel moves. */
static const uint64_t dbg_u_rips_default[] = {
    0xffffffff801907beull,   /* syscall-entry stub stores (syscall path A)   */
    0xffffffff801907d3ull,   /* syscall-entry stub stores (syscall path B)   */
    0xffffffff80106f85ull,   /* the ring-3 trampoline's cpu_local write      */
};
#define DBG_U_RIPS_MAX 16
static uint64_t dbg_u_rips[DBG_U_RIPS_MAX];
static int dbg_u_nrips;

static void dbg_u_rips_load(void) {
    dbg_u_nrips = (int)(sizeof dbg_u_rips_default / sizeof dbg_u_rips_default[0]);
    if (dbg_u_nrips > DBG_U_RIPS_MAX) dbg_u_nrips = DBG_U_RIPS_MAX;
    for (int i = 0; i < dbg_u_nrips; i++) dbg_u_rips[i] = dbg_u_rips_default[i];
    const char *e = getenv("EMU_DBG_U_RIPS");
    if (!e || !*e) return;
    int n = 0;
    const char *p = e;
    while (*p && n < DBG_U_RIPS_MAX) {
        char *end = NULL;
        unsigned long long v = strtoull(p, &end, 0);
        if (end == p) break;                  /* not a number: stop at it */
        dbg_u_rips[n++] = (uint64_t)v;
        p = end;
        if (*p == ',') p++;
        else break;
    }
    if (n) dbg_u_nrips = n;                   /* an explicit list replaces the default */
}

/* The per-instruction hook cpu_step() calls.  False in the default build.
 * Both the lazily-resolved knob and the watch list are settled here, because
 * cpu_step() runs BEFORE fetch8() resolves EMU_DBG_U for the first
 * instruction -- without this the first match of a run could not be seen. */
static inline int dbg_u_watch_rip(const cpu_t *c) {
    if (dbg_u_on < 0) cpu_dbg_knob_resolve();
    if (dbg_u_on <= 0) return 0;
    if (!dbg_u_nrips) dbg_u_rips_load();
    for (int i = 0; i < dbg_u_nrips; i++)
        if (c->rip == dbg_u_rips[i]) return 1;
    return 0;
}

static int dbg_u_events;
static int dbg_read_probe = 0;      /* K4: first-N read/write syscall probe */
static int syscall_ret_probe = -1;  /* id of the in-flight probed syscall */
static FILE *probe_fp;              /* $EMU_DBG_U_FILE, unbuffered -- survives kills */

/* A6: the probe file is a diagnostic sink, so it must be (a) named by the
 * operator, (b) opened only when the probe fires, and (c) closable.  The
 * default keeps the historical /tmp path when EMU_DBG_U=1 is asked for. */
static FILE *dbg_u_probe_file(void) {
    if (!probe_fp) {
        const char *path = getenv("EMU_DBG_U_FILE");
        if (!path || !*path) path = "/tmp/k4-probe.txt";
        probe_fp = fopen(path, "w");
        if (probe_fp) setvbuf(probe_fp, NULL, _IONBF, 0);
    }
    return probe_fp;
}

void cpu_dbg_hooks_close(void) {
    if (probe_fp) { fclose(probe_fp); probe_fp = NULL; }
}
#else
#define dbg_u_events 0
#define dbg_u_watch_rip(c) 0
void cpu_dbg_hooks_close(void) { }
#endif

/* K4: raise a page fault with the architected error code.
 * mode: 0 = data read, 1 = data write, 2 = instruction fetch. */
static void page_fault(machine_t *m, uint64_t vaddr, int mode, int present_chain, int nx_hit) {
    cpu_t *c = &m->cpu;
    c->cr2 = vaddr; /* K2: the AuraLite isr14 dispatches on CR2 */
    uint32_t err = 0;
    if (present_chain) err |= 1u;             /* P: level-all-present */
    if (mode == 1) err |= 2u;                 /* W/R */
    if (cpl_now(c) == 3) err |= 4u;           /* U/S */
    if (nx_hit) err |= 16u; /* I/D: fetch-protection fault -- NX
                             * (call sites gate on EFER.NXE) or SMEP (CF1:
                             * the SDM sets I/D for SMEP regardless) */
    /* K4: page faults are rare in a healthy boot but their context rip is the
     * entire delta between "mystery SIGSEGV" and a one-line verdict -- keep
     * it always-on (measured: the init-shell null-deref chased below). */
    mlog(&m->log, "[cpu] #PF ctx rip=%llx cr3=%llx cpl=%d rsp=%llx",
         (unsigned long long)m->cpu.rip, (unsigned long long)m->cpu.cr3,
         m->cpu.seg[SEG_CS].sel & 3, (unsigned long long)m->cpu.gpr[RSP]);
    mlog(&m->log, "[cpu] #PF v=0x%llx mode=%d err=0x%x", (unsigned long long)vaddr, mode, err);
    /* K8 debug guard: a #PF raised while a previous exception delivery is
     * in flight is architecturally the start of a double-fault sequence.
     * Catch the recursion the host would otherwise die of (native stack
     * overflow at CPL3->0 frame push on a broken chain), name it, halt. */
    {
        static int in_dlv;
        static uint64_t pend_cr2;
        if (in_dlv) {
            mlog(&m->log, "[cpu] #DF-CHAIN: #PF raised while delivering an "
                 "exception (new cr2=%llx, in-flight cr2=%llx, cr3=%llx "
                 "rip=%llx rsp=%llx cpl=%d)",
                 (unsigned long long)vaddr, (unsigned long long)pend_cr2,
                 (unsigned long long)m->cpu.cr3,
                 (unsigned long long)m->cpu.rip,
                 (unsigned long long)m->cpu.gpr[RSP],
                 m->cpu.seg[SEG_CS].sel & 3);
            snprintf(c->fault_msg, sizeof c->fault_msg, "K8 df-chain");
            c->fault = 1; return;
        }
        in_dlv = 1; pend_cr2 = vaddr;
        raise_exception(m, 14, 1, err);
        in_dlv = 0;
    }
}

/* K4: the walk enforces U/S, R/W (with CR0.WP for CPL0), NX (with EFER.NXE)
 * and the CR4.SMEP/SMAP supervisor guards. Effective U/S and R/W are the AND
 * of every level; NX is the OR. On any failure the fault is raised HERE and
 * 0 is returned -- callers must return without re-raising. */
#define T_READ 0
#define T_WRITE 1
#define T_EXEC 2
/* A cached walk may only have read side-effect-free RAM-backed page-table
 * entries.  The low ROM alias is inside RAM_SIZE and must be excluded;
 * disabled A20 changes the addresses of page-table reads, so take the old
 * walk instead.  The physical bus gives RAM priority over registered MMIO
 * in this range (src/mem.c), including caller-registered test MMIO. */
static int perf_tlb_plain_table(machine_t *m, uint64_t phys) {
    return m->chipset.a20 && phys <= (uint64_t)RAM_SIZE - 8 &&
           !(phys >= ROM_ALIAS_LOW_BASE && phys < ROM_ALIAS_LOW_BASE + ROM_SIZE);
}
static int translate(machine_t *m, uint64_t vaddr, uint64_t *out_phys, int mode) {
    cpu_t *c = &m->cpu;
    if (!(c->cr0 & (1ULL<<31))) { *out_phys = vaddr; return 1; } /* paging disabled */
    int user_cpl = (cpl_now(c) == 3) && !c->desc_sv;   /* K4: implicit descriptor reads are supervisor */
    /* PERF P2: only SUCCESSFUL walks are cached.  Key the whole security
     * context: CR3 (ASID), CR0.WP, CR4.SMEP/SMAP, EFER.NXE, RFLAGS.AC,
     * effective CPL including descriptor-supervisor override, and access
     * mode.  Whole control regs in the key are conservative; no access
     * should ever hit a translation with weaker permissions.  Cache is
     * shared across SMP contexts, never copied on a vCPU slot switch. */
    uint64_t page = vaddr >> 12;
    uint64_t slot = (page ^ (page >> 9) ^ (c->cr3 >> 12)) & (PERF_TLB_SLOTS - 1);
    perf_tlb_entry_t *hit = &m->perf_tlb[slot];
    uint8_t ac = (uint8_t)((c->rflags >> 18) & 1);
    uint8_t cpl = (uint8_t)cpl_now(c);
    uint8_t desc_sv = (uint8_t)(c->desc_sv != 0);
    if (m->chipset.a20 && hit->valid &&
        hit->generation == m->perf_tlb_generation &&
        hit->linear_page == page && hit->cr3 == c->cr3 &&
        hit->cr0 == c->cr0 && hit->cr4 == c->cr4 && hit->efer == c->efer &&
        hit->ac == ac && hit->cpl == cpl && hit->desc_sv == desc_sv &&
        hit->mode == mode) {
        ++m->perf_tlb_hits;
        *out_phys = hit->phys_page | (vaddr & 0xFFFULL);
        return 1;
    }
    ++m->perf_tlb_misses;
    int cacheable = m->chipset.a20;
    uint64_t cr3 = c->cr3 & ~0xFFFULL;
    int idx4 = (int)((vaddr >> 39) & 0x1FF);
    int idx3 = (int)((vaddr >> 30) & 0x1FF);
    int idx2 = (int)((vaddr >> 21) & 0x1FF);
    int idx1 = (int)((vaddr >> 12) & 0x1FF);
    int eff_us = 1, eff_w = 1, nx_any = 0;

    cacheable &= perf_tlb_plain_table(m, cr3 + (uint64_t)idx4*8);
    uint64_t pml4e = mem_read(m, cr3 + (uint64_t)idx4*8, 8);
    if (!(pml4e & 1)) { page_fault(m, vaddr, mode, 0, 0); return 0; }
    eff_us &= (int)((pml4e >> 2) & 1); eff_w &= (int)((pml4e >> 1) & 1); nx_any |= (int)(pml4e >> 63);
    uint64_t pdpt = pml4e & 0x000FFFFFFFFFF000ULL;

    cacheable &= perf_tlb_plain_table(m, pdpt + (uint64_t)idx3*8);
    uint64_t pdpte = mem_read(m, pdpt + (uint64_t)idx3*8, 8);
    if (!(pdpte & 1)) { page_fault(m, vaddr, mode, 0, 0); return 0; }
    eff_us &= (int)((pdpte >> 2) & 1); eff_w &= (int)((pdpte >> 1) & 1); nx_any |= (int)(pdpte >> 63);
    if (pdpte & (1ULL<<7)) { *out_phys = (pdpte & 0xFFFFFC0000000ULL) | (vaddr & 0x3FFFFFFFULL); goto check; }
    {
        uint64_t pd = pdpte & 0x000FFFFFFFFFF000ULL;
        cacheable &= perf_tlb_plain_table(m, pd + (uint64_t)idx2*8);
        uint64_t pde = mem_read(m, pd + (uint64_t)idx2*8, 8);
        if (!(pde & 1)) { page_fault(m, vaddr, mode, 0, 0); return 0; }
        eff_us &= (int)((pde >> 2) & 1); eff_w &= (int)((pde >> 1) & 1); nx_any |= (int)(pde >> 63);
        if (pde & (1ULL<<7)) { *out_phys = (pde & 0x000FFFFFFFE00000ULL) | (vaddr & 0x1FFFFFULL); goto check; }
        {
            uint64_t pt = pde & 0x000FFFFFFFFFF000ULL;
            cacheable &= perf_tlb_plain_table(m, pt + (uint64_t)idx1*8);
            uint64_t pte = mem_read(m, pt + (uint64_t)idx1*8, 8);
            if (!(pte & 1)) { page_fault(m, vaddr, mode, 0, 0); return 0; }
            eff_us &= (int)((pte >> 2) & 1); eff_w &= (int)((pte >> 1) & 1); nx_any |= (int)(pte >> 63);
            *out_phys = (pte & 0x000FFFFFFFFFF000ULL) | (vaddr & 0xFFFULL);
        }
    }
check:
    /* whole chain was present from here on: P=1 */
    if (!eff_us) {
        if (user_cpl) { page_fault(m, vaddr, mode, 1, 0); return 0; }
    } else if (!user_cpl) {
        /* CPU_FEATURES CF1: SMEP/SMAP constrain a SUPERVISOR access to
         * a USER page (eff U/S=1) -- a supervisor fetch (CR4.SMEP,
         * err P|I/D) or a supervisor data access with EFLAGS.AC=0
         * (CR4.SMAP). user_cpl already folds in K4's desc_sv rule, so
         * implicit descriptor accesses are treated exactly like
         * explicit kernel ones (the QEMU kernel-MMU-index shape);
         * page-walk reads themselves are physical and never reach
         * here. Pre-CF1 both guards sat inside the !eff_us branch --
         * inverted since the original upstream code, invisible only
         * because CPUID advertised neither feature, so no consumer
         * ever set the CR4 bits (the CF0 harness table, plan 1.3). */
        if ((c->cr4 & (1ULL<<20)) && mode == T_EXEC) { page_fault(m, vaddr, mode, 1, 1); return 0; } /* SMEP */
        if ((c->cr4 & (1ULL<<21)) && mode != T_EXEC &&
            !(c->rflags & (1ULL<<18)))          { page_fault(m, vaddr, mode, 1, 0); return 0; } /* SMAP, AC=0 */
    }
    if (mode == T_EXEC && nx_any && (c->efer & (1ULL<<11))) { page_fault(m, vaddr, mode, 1, 1); return 0; }
    if (mode == T_WRITE && !eff_w && (user_cpl || (c->cr0 & (1ULL<<16)))) { page_fault(m, vaddr, mode, 1, 0); return 0; }
    if (cacheable) {
        hit->generation = m->perf_tlb_generation;
        hit->linear_page = page;
        hit->phys_page = *out_phys & ~0xFFFULL;
        hit->cr3 = c->cr3; hit->cr0 = c->cr0;
        hit->cr4 = c->cr4; hit->efer = c->efer;
        hit->ac = ac; hit->cpl = cpl; hit->desc_sv = desc_sv;
        hit->mode = (uint8_t)mode;
        hit->valid = 1;
    }
    return 1;
}

static void raise_exception(machine_t *m, int vector, int has_err, uint32_t err);

/* TOOLING T2: the watchpoint call sits out of line (cold), and read_mem_v
 * carries an `inline` hint. Measured: the hook's extra bytes pushed
 * read_mem_v over GCC's -O2 auto-inline threshold, rm_read/do_pop/
 * read_descriptor/raise_exception started calling it, and the paired
 * bench showed ~3% on six of seven tapes. With the hint the inlining map
 * is the pre-T2 one again (always_inline over-inlined into cpu_step and
 * was slower) and every paired CI contains 1.0. */
__attribute__((noinline, cold))
static int dbg_boundary_cold(machine_t *m) {
    return (cpu_dbg_boundary && !m->cpu.fault) ? cpu_dbg_boundary(m) : 0;
}
__attribute__((noinline, cold))
static void dbg_access_cold(machine_t *m, uint64_t vaddr, int size, int write) {
    if (cpu_dbg_access) cpu_dbg_access(m, vaddr, size, write);
}
#define DBG_ACCESS(m, va, sz, w) \
    do { if (__builtin_expect(cpu_dbg_access != NULL, 0)) dbg_access_cold((m), (va), (sz), (w)); } while (0)

static inline uint64_t read_mem_v(machine_t *m, uint64_t vaddr, int size) {
    uint64_t phys;
    if (!translate(m, vaddr, &phys, T_READ))  /* fault already raised (K4) */
        return 0;
    DBG_ACCESS(m, vaddr, size, 0);   /* TOOLING T2 watch */
    return mem_read(m, phys, size);
}
static void write_mem_v(machine_t *m, uint64_t vaddr, int size, uint64_t val) {
    uint64_t phys;
    if (!translate(m, vaddr, &phys, T_WRITE))  /* fault already raised (K4) */
        return;
#if AURA_DEBUG_HOOKS
    /* A6: guest-specific store probes -- compile-time optional, so the
     * release path carries neither the literal windows nor the formatting. */
    if (dbg_u_on > 0 && vaddr >= 0x7fffefffa000ull && vaddr < 0x7fffefffc000ull)
        mlog(&m->log, "[dbg-u] ustack W va=%llx sz=%d val=%llx cpl=%d rip=%llx",
             (unsigned long long)vaddr, size, (unsigned long long)val,
             m->cpu.seg[SEG_CS].sel & 3, (unsigned long long)m->cpu.rip);
    /* CL_SYS_RIP/RFLAGS/RSP slots live at +72/+80/+88 inside the kernel
     * cpu_local (gs.base at kernel time). Watch who rewrites them. */
    if (dbg_u_on > 0 && m->cpu.seg[SEG_GS].base != 0 &&
        vaddr >= m->cpu.seg[SEG_GS].base + 64 && vaddr < m->cpu.seg[SEG_GS].base + 96 &&
        (m->cpu.seg[SEG_CS].sel & 3) == 0)
        mlog(&m->log, "[dbg-u] cpulocal W va=GS+%llx val=%llx rip=%llx",
             (unsigned long long)(vaddr - m->cpu.seg[SEG_GS].base),
             (unsigned long long)val, (unsigned long long)m->cpu.rip);
#endif
    DBG_ACCESS(m, vaddr, size, 1);   /* TOOLING T2 watch */
    mem_write(m, phys, size, val);
}

/* ============================ GDT / segments ============================ */
typedef struct { uint64_t base; uint32_t limit; uint8_t type, s, dpl, present, l, db, g; } desc_t;

static void read_descriptor(machine_t *m, uint64_t table_base, uint16_t sel, desc_t *d) {
    uint32_t idx = sel >> 3;
    uint64_t addr = table_base + (uint64_t)idx * 8;
    /* K2: descriptor-table addresses are LINEAR (GDTR/IDTR/TR hold linear
     * bases: SDM vol.3A s.2.4.1), so fetches go through translation.
     * mem_read (physical) only coincides in real mode / flat tables, which
     * is why every pre-long-mode stage appeared to work; a higher-half
     * guest GDT read as physical went to open bus (measured: kmain's
     * `mov %eax,%ss` loaded base=0xFFFFFFFF from descriptor bytes all-FF). */
    m->cpu.desc_sv++;                       /* K4: implicit supervisor read */
    uint64_t lo = read_mem_v(m, addr, 4);
    uint64_t hi = read_mem_v(m, addr + 4, 4);
    m->cpu.desc_sv--;
    uint64_t limit = (lo & 0xFFFF) | (hi & 0xF0000);
    uint64_t base  = ((lo >> 16) & 0xFFFF) | ((hi & 0xFF) << 16) | ((hi >> 24) << 24);
    uint8_t access = (uint8_t)((hi >> 8) & 0xFF);
    uint8_t flags  = (uint8_t)((hi >> 20) & 0xF);
    d->base = base; d->limit = (uint32_t)limit;
    d->type = access & 0xF; d->s = (access>>4)&1; d->dpl=(access>>5)&3; d->present=(access>>7)&1;
    d->l=(flags>>1)&1; d->db=(flags>>2)&1; d->g=(flags>>3)&1;
    if (d->g) d->limit = (uint32_t)((d->limit << 12) | 0xFFF);
}

static void load_seg_from_desc(cpu_t *c, int segidx, uint16_t sel, desc_t *d) {
    c->seg[segidx].sel = sel;
    c->seg[segidx].base = d->base;
    c->seg[segidx].limit = d->limit;
    c->seg[segidx].present = d->present;
    c->seg[segidx].type = d->type;
    c->seg[segidx].d_b = d->db;
    c->seg[segidx].l = (segidx==SEG_CS) ? d->l : 0;
}

static void far_load_cs(machine_t *m, uint16_t sel, uint64_t new_rip) {
    cpu_t *c = &m->cpu;
    if (!(c->cr0 & 1)) {
        c->seg[SEG_CS].sel = sel;
        c->seg[SEG_CS].base = (uint64_t)sel << 4;
        c->seg[SEG_CS].limit = 0xFFFF;
        c->seg[SEG_CS].d_b = 0; c->seg[SEG_CS].l = 0;
        c->rip = new_rip;
        return;
    }
    desc_t d; read_descriptor(m, c->gdtr_base, sel, &d);
    load_seg_from_desc(c, SEG_CS, sel, &d);
    c->rip = new_rip;
}

/* ============================== exceptions =============================== */
static const char *vecname(int v){
    switch(v){case 0:return "#DE";case 6:return "#UD";case 8:return "#DF";
        case 13:return "#GP";case 14:return "#PF";default:return "#??";}
}
/* H6: locate the 64-bit TSS for IST stack switching.
 * K2: the architected TSS pointer is the one LTR caches in tr_base --
 * the old "selector 0x10 convention" fallback broke the first time a real
 * GDT used slot 0x10 for a normal data segment (measured on AuraLite's
 * idx2 data descriptor: it computed a bogus TSS base 0xffff00000000,
 * turning every IST check into a spurious nested #PF). Pre-LTR fixtures
 * still get the 0x10 probe, but only behind a type check. */
static uint64_t tss_base_from_gdt(machine_t *m, cpu_t *c) {
    if (c->tr_base || c->tr_limit)      /* LTR points at the live TSS */
        return c->tr_base;
    m->cpu.desc_sv++;                   /* K4: TSS probe is also an implicit read */
    uint64_t lo = read_mem_v(m, c->gdtr_base + 0x10, 8);   /* linear (K2) */
    if (!((lo >> 47) & 1)) { m->cpu.desc_sv--; return 0; } /* present bit */
    uint8_t acc = (uint8_t)((lo >> 40) & 0xFF);
    if (acc & 0x10) { m->cpu.desc_sv--; return 0; } /* S=1: code/data, not a TSS gate */
    uint8_t t = acc & 0xF;
    if (t != 0x9 && t != 0xB) { m->cpu.desc_sv--; return 0; } /* 64-bit TSS: available / busy */
    uint64_t hi32 = read_mem_v(m, c->gdtr_base + 0x18, 4);
    m->cpu.desc_sv--;
    return ((lo >> 16) & 0xFFFFFFULL) | (((lo >> 56) & 0xFFULL) << 24) |
           ((uint64_t)hi32 << 32);
}

static void raise_exception(machine_t *m, int vector, int has_err, uint32_t err) {
    cpu_t *c = &m->cpu;
    c->event_count++;   /* TOOLING T1: tooling-only, see cpu.h */
    /* K4: the whole delivery window -- gate fetch, TSS read, frame pushes --
     * happens at supervisor privilege on real hardware, even when the event
     * struck in ring 3. Suspend the RPL for the duration so the U/S
     * enforcer sees the architected context; a ring-changing delivery
     * reloads CS itself below, a same-ring one restores it at the end. */
    uint16_t presuspend_cs = c->seg[SEG_CS].sel;
    int delivery_from_cpl = presuspend_cs & 3;   /* captured pre-suspend */
    if (delivery_from_cpl != 0) c->seg[SEG_CS].sel = (uint16_t)(presuspend_cs & ~3u);
    if (c->in_exception) {
        mlog(&m->log, "[cpu] exception while delivering an exception -> double/triple fault, halting");
        faultf(c, "double fault (vector %d while handling another)", vector);
        c->halted = 1;
        return;
    }
    c->in_exception = 1;

    /* ---- real mode: vectors dispatch through the IVT at physical 0 ----
     * (C0 ledger #13: a real-mode fault used to halt the machine even when
     * the guest had a perfectly good vector table). Hardware pushes
     * FLAGS,CS,IP as three 16-bit words and clears IF/TF. */
    if (!(c->cr0 & 1)) {
        uint16_t off = (uint16_t)mem_read(m, (uint64_t)vector * 4, 2);
        uint16_t cs  = (uint16_t)mem_read(m, (uint64_t)vector * 4 + 2, 2);
        uint32_t sp = (uint32_t)c->gpr[RSP];
        sp -= 2; write_mem_v(m, c->seg[SEG_SS].base + (sp & 0xFFFF), 2, c->rflags);
        sp -= 2; write_mem_v(m, c->seg[SEG_SS].base + (sp & 0xFFFF), 2, c->seg[SEG_CS].sel);
        sp -= 2; write_mem_v(m, c->seg[SEG_SS].base + (sp & 0xFFFF), 2, c->rip);
        c->gpr[RSP] = (c->gpr[RSP] & 0xFFFFFFFFFFFF0000ull) | (sp & 0xFFFF);
        c->seg[SEG_CS].sel = cs;
        c->seg[SEG_CS].base = (uint64_t)cs << 4;
        c->seg[SEG_CS].d_b = 0; c->seg[SEG_CS].l = 0;
        c->rip = off;
        c->rflags &= ~(FLAG_IF | FLAG_TF);
        c->exception_taken = 1;
        /* IVT dispatch receipts: measured 9381 INT16 lines during a stock
         * SeaBIOS keyboard spin-up alone -- opt-in via AURALITE_IVT_LOG=1. */
#if AURA_DEBUG_HOOKS
        { static int ivt_log = -1;
          if (ivt_log < 0) { const char *e = getenv("AURALITE_IVT_LOG"); ivt_log = (e && e[0] == '1'); }
          if (ivt_log)
              mlog(&m->log, "[cpu] real-mode vector %d (%s) -> %04x:%04x via IVT", vector, vecname(vector), cs, off); }
#endif
        c->in_exception = 0;
        return;
    }

    /* ---- protected / long mode: dispatch through the IDT ---- */
    if (c->idtr_limit == 0) {
        mlog(&m->log, "[cpu] %s (vector %d) with NO IDT loaded -> triple fault / reset-worthy condition", vecname(vector), vector);
        faultf(c, "%s with no IDT loaded", vecname(vector));
        c->halted = 1;
        return;
    }
    /* K2: IDTR holds a linear base -- read the gate through translation */
    uint64_t gate_addr = c->idtr_base + (uint64_t)vector * 16;
    uint16_t off0 = (uint16_t)read_mem_v(m, gate_addr+0, 2);
    uint16_t sel  = (uint16_t)read_mem_v(m, gate_addr+2, 2);
    uint16_t off1 = (uint16_t)read_mem_v(m, gate_addr+6, 2);
    uint32_t off2 = (uint32_t)read_mem_v(m, gate_addr+8, 4);
    uint8_t  type_attr = (uint8_t)read_mem_v(m, gate_addr+5, 1);
    if (!(type_attr & 0x80)) {
        mlog(&m->log, "[cpu] vector %d: IDT gate not present -> triple fault", vector);
        c->halted = 1; faultf(c, "%s with not-present IDT gate", vecname(vector));
        return;
    }
    uint64_t handler = (uint64_t)off0 | ((uint64_t)off1<<16) | ((uint64_t)off2<<32);
    int is64 = ((c->efer>>10)&1) && c->seg[SEG_CS].l;
    int stacksz = is64 ? 8 : (c->seg[SEG_CS].d_b ? 4 : 2);
    uint64_t rsp = c->gpr[RSP];
    int from_cpl = delivery_from_cpl;   /* K4: not the suspended pseudo-CPL */
    uint16_t old_ss_sel = c->seg[SEG_SS].sel;
    uint16_t old_cs_sel = presuspend_cs;
    uint64_t old_rsp = c->gpr[RSP];
    /* CHIPSET H6: a 64-bit IDT gate with nonzero IST switches the handler
     * onto the matching TSS IST stack. Without this, a delivery that
     * lands while RSP still points at a register window (mid-EOI is the
     * classic case) would push the IRET frame over that window and the
     * next dereference corrupts the machine -- measured in the H6
     * long-mode vector (read: #PF loop) before IST existed. */
    int ist = 0;
    if (is64)
        /* K4: gate_addr is LINEAR (IDTR holds a linear base, K2) -- a raw
         * physical mem_read here consulted RAM at the higher-half VMA and,
         * for a higher-half IDT, returned garbage whose low 3 bits read as
         * ist=7. Delivery from CPL3 then tried TSS.IST7 (=0) instead of
         * TSS.RSP0, skipped the stack switch and pushed the frame onto the
         * USER stack: measured IRQ32-from-ring-3 storm ending in a kernel
         * #PF at 0x801053d5 against the boot CR3. */
        ist = (int)read_mem_v(m, gate_addr + 4, 1) & 7;
    /* K4: a delivery taken at CPL3 switches to the target ring's stack:
     * IST wins when the gate names one, else TSS.RSP0. The outer SS:RSP
     * then rides above the frame so IRETQ can restore it. */
    int ring_change = is64 && from_cpl != (sel & 3) && (sel & 3) == 0;
    if (ist || ring_change) {
        uint64_t tss = c->tr_base ? c->tr_base : tss_base_from_gdt(m, c);
        uint64_t ns = 0;
        if (ist)
            /* K4: TSS64 layout (SDM vol.3A fig.8-11): RSP0 @4, then RSP1/RSP2,
             * 8 reserved bytes, IST1 @0x24 -- so ISTn = tss + 0x24 + (n-1)*8.
             * The old `tss + 4 + (n-1)*8` read IST1 out of the RSP0 slot
             * (nil early on: no stack switch at all). AuraLite's FIX_R1 arms
             * #DF -> IST1; the first real #DF delivery would have run the
             * #DF handler on the *interrupting* stack. */
            ns = tss ? read_mem_v(m, tss + 0x24 + (uint64_t)(ist - 1) * 8, 8) : 0; /* linear (K2) */
        else if (ring_change)
            ns = tss ? read_mem_v(m, tss + 4, 8) : 0;                          /* TSS.RSP0 */
#if AURA_DEBUG_HOOKS
        if (dbg_u_on > 0 && ring_change)
            mlog(&m->log, "[dbg-u] ringchg vec=%d from_cpl=%d ist=%d tss=%llx ns=%llx old_rsp=%llx cr3=%llx",
                 vector, from_cpl, ist, (unsigned long long)tss, (unsigned long long)ns,
                 (unsigned long long)c->gpr[RSP], (unsigned long long)c->cr3);
#endif
        if (ns) rsp = ns;
    }
    if (ring_change) {
        /* adopt the new ring NOW so the frame pushes translate as CPL0
         * writes to supervisor pages -- the same interim state hardware
         * enters before the first push (K4; measured twice against the
         * U/S enforcer). SS is reloaded with the rpl-0 mate of the gate
         * selector; CS follows at far_load_cs below. */
        c->seg[SEG_SS].sel = (uint16_t)((sel + 8) & 0xFFFC);
        c->seg[SEG_SS].base = 0;
        c->seg[SEG_CS].sel = sel;   /* CPL drops here; base/rip fixed below */
    }
    /* Hardware pushes (highest address first): [SS, RSP,] RFLAGS, CS, RIP,
     * and the error code LAST (lowest address). The baseline pushed the
     * error code first, which would have made any future IRET pop the
     * wrong slots (C0 ledger #12). */
    if (ring_change) {
        rsp -= stacksz; write_mem_v(m, rsp, stacksz, old_ss_sel);
        rsp -= stacksz; write_mem_v(m, rsp, stacksz, old_rsp);
    }
    rsp -= stacksz; write_mem_v(m, rsp, stacksz, c->rflags);
    rsp -= stacksz; write_mem_v(m, rsp, stacksz, old_cs_sel); /* K4: pre-switch CS */
    rsp -= stacksz; write_mem_v(m, rsp, stacksz, c->rip);
    if (has_err) { rsp -= stacksz; write_mem_v(m, rsp, stacksz, err); }
    c->gpr[RSP] = rsp;
    far_load_cs(m, sel, handler);
    if (!ring_change)         /* same-ring delivery: bring the RPL back */
        c->seg[SEG_CS].sel = (uint16_t)(presuspend_cs | (sel & 3));
    /* CF1: gate delivery also clears EFLAGS.AC (the pushed image keeps
     * the old bit for IRET to restore), so a handler always starts
     * with SMAP shut -- a STAC window interrupted mid-copy must not
     * leak open-door state into arbitrary handler code. */
    c->rflags &= ~(FLAG_IF | (1ULL<<18));
    c->exception_taken = 1;
    /* IRQ vectors (32+) fire per timer tick in the idle loop and would drown
     * the serial history in the logring -- exceptions stay always-on. (K4:
     * measured, the fix4/fix5 rings were 4096/4096 vector-32 lines.) */
    if (dbg_u_on > 0 || vector < 32)
        mlog(&m->log, "[cpu] exception %s(%d) delivered -> CS:RIP=%04x:%llx", vecname(vector), vector, sel, (unsigned long long)handler);
    c->in_exception = 0;
}

/* =========================== register access ============================= */
static uint64_t reg_mask(int size){ return size==1?0xFFull:size==2?0xFFFFull:size==4?0xFFFFFFFFull:~0ull; }

static uint64_t get_reg(cpu_t *c, int idx, int size, int has_rex) {
    if (size == 1 && !has_rex && idx >= 4 && idx <= 7) return (c->gpr[idx-4] >> 8) & 0xFF;
    return c->gpr[idx] & reg_mask(size);
}
static void set_reg(cpu_t *c, int idx, int size, int has_rex, uint64_t val) {
    if (size == 1 && !has_rex && idx >= 4 && idx <= 7) {
        uint64_t base = c->gpr[idx-4];
        c->gpr[idx-4] = (base & ~0xFF00ull) | ((val & 0xFF) << 8);
        return;
    }
    if (size == 8) c->gpr[idx] = val;
    else if (size == 4) c->gpr[idx] = val & 0xFFFFFFFFull;
    else c->gpr[idx] = (c->gpr[idx] & ~reg_mask(size)) | (val & reg_mask(size));
}

/* ============================== flags ==================================== */
static int parity8(uint8_t v){ v ^= v>>4; v ^= v>>2; v ^= v>>1; return !(v&1); }

static void set_flags_logic(cpu_t *c, uint64_t res, int size) {
    uint64_t mm = reg_mask(size); res &= mm;
    c->rflags &= ~(FLAG_CF|FLAG_OF|FLAG_ZF|FLAG_SF|FLAG_PF);
    if (res == 0) c->rflags |= FLAG_ZF;
    if (res & (1ull << (size*8-1))) c->rflags |= FLAG_SF;
    if (parity8((uint8_t)res)) c->rflags |= FLAG_PF;
}
static void set_flags_add(cpu_t *c, uint64_t a, uint64_t b, uint64_t cin, uint64_t res, int size) {
    /* C10 fix: the carry-in contributed by ADC must participate in CF and AF;
     * otherwise 0 + 0xFFFF + 1 computes CF=0 and 0xF + 0 + 1 computes AF=0,
     * both wrong (caught by the table vector suite). */
    uint64_t mm = reg_mask(size), sbit = 1ull << (size*8-1);
    c->rflags &= ~(FLAG_CF|FLAG_OF|FLAG_ZF|FLAG_SF|FLAG_PF|FLAG_AF);
    if ((res & mm) == 0) c->rflags |= FLAG_ZF;
    if (res & sbit) c->rflags |= FLAG_SF;
    if (parity8((uint8_t)res)) c->rflags |= FLAG_PF;
    if (((a & mm) + (b & mm) + cin) > mm) c->rflags |= FLAG_CF;
    uint64_t sa=a&sbit, sb=b&sbit, sr=res&sbit;
    if (sa==sb && sr!=sa) c->rflags |= FLAG_OF;
    if (((a & 0xF) + (b & 0xF) + cin) & 0x10) c->rflags |= FLAG_AF;
}
static void set_flags_sub(cpu_t *c, uint64_t a, uint64_t b, uint64_t cin, uint64_t res, int size) {
    /* C10 fix: SBB's borrow-in participates in CF and AF (see set_flags_add). */
    uint64_t mm = reg_mask(size), sbit = 1ull << (size*8-1);
    c->rflags &= ~(FLAG_CF|FLAG_OF|FLAG_ZF|FLAG_SF|FLAG_PF|FLAG_AF);
    if ((res & mm) == 0) c->rflags |= FLAG_ZF;
    if (res & sbit) c->rflags |= FLAG_SF;
    if (parity8((uint8_t)res)) c->rflags |= FLAG_PF;
    if ((a & mm) < ((b & mm) + cin)) c->rflags |= FLAG_CF;
    uint64_t sa=a&sbit, sb=b&sbit, sr=res&sbit;
    if (sa!=sb && sr!=sa) c->rflags |= FLAG_OF;
    if ((a & 0xF) < ((b & 0xF) + cin)) c->rflags |= FLAG_AF;
}

/* ============================ instr decode ctx ============================ */
typedef struct {
    machine_t *m; cpu_t *c;
    uint64_t start_pc;  /* pc at first byte of instruction (after none consumed) */
    uint64_t pc;        /* fetch cursor, offset within CS */
    int addrsize, opsize;
    int has_rex, rex_w, rex_r, rex_x, rex_b;
    int seg_override;
    int rep;
} dctx_t;

/* PERF P3: the cache is a preDECODE cache for the prefix/opcode bytes only.
 * An actual ModRM decode reads current GPRs and may itself fetch SIB/disps,
 * so caching rm_t would freeze live effective addresses -- never do it.
 * Only store after a successful ordinary-RAM P2 EXEC walk, and only when
 * the entire parsed prefix/opcode stream is on ONE 4KiB page.  The P2 RAM-
 * store epoch invalidates self-modifying code, PTE changes and device DMA;
 * control changes also bump it and are in the key.  Debug fetch logging and
 * --trace take the original slow path. */
static size_t perf_decode_slot(cpu_t *c) {
    uint64_t page_mix = c->rip ^ (c->rip >> 9) ^ (c->cr3 >> 12);
    return (size_t)(page_mix & (PERF_DECODE_SLOTS - 1));
}
static int perf_decode_hit(machine_t *m, dctx_t *d, uint8_t *op) {
    cpu_t *c = d->c;
    if (dbg_u_on != 0 || c->trace || !m->chipset.a20 || !(c->cr0 & (1ULL<<31)))
        return 0;
    perf_decode_entry_t *e = &m->perf_decode[perf_decode_slot(c)];
    if (!e->valid || e->generation != m->perf_tlb_generation ||
        e->rip != c->rip || e->cs_base != c->seg[SEG_CS].base ||
        e->cs_sel != c->seg[SEG_CS].sel || e->cs_l != c->seg[SEG_CS].l ||
        e->cs_db != c->seg[SEG_CS].d_b || e->cr0 != c->cr0 ||
        e->cr3 != c->cr3 || e->cr4 != c->cr4 || e->efer != c->efer ||
        e->ac != ((c->rflags >> 18) & 1) ||
        e->desc_sv != (c->desc_sv != 0)) {
        ++m->perf_decode_misses;
        return 0;
    }
    ++m->perf_decode_hits;
    d->pc = e->next_pc;
    d->addrsize = e->addrsize; d->opsize = e->opsize;
    d->has_rex = e->has_rex;
    d->rex_w = e->rex_w; d->rex_r = e->rex_r;
    d->rex_x = e->rex_x; d->rex_b = e->rex_b;
    d->seg_override = e->seg_override; d->rep = e->rep;
    *op = e->opcode;
    return 1;
}
static void perf_decode_store(machine_t *m, dctx_t *d, uint8_t op) {
    cpu_t *c = d->c;
    if (dbg_u_on != 0 || c->trace || !m->chipset.a20 || !(c->cr0 & (1ULL<<31)))
        return;
    if (d->pc <= d->start_pc || d->pc - d->start_pc > 15) return;
    uint64_t first = c->seg[SEG_CS].base + d->start_pc;
    uint64_t last = c->seg[SEG_CS].base + d->pc - 1;
    if (last < first || (first >> 12) != (last >> 12)) return;
    /* A P2 EXEC entry only exists if EVERY table entry came from ordinary
     * RAM with A20 open.  If a side-effecting MMIO table participated in
     * the walk, this check fails and we keep fetching each byte live. */
    uint64_t page = first >> 12;
    uint64_t slot = (page ^ (page >> 9) ^ (c->cr3 >> 12)) & (PERF_TLB_SLOTS - 1);
    const perf_tlb_entry_t *t = &m->perf_tlb[slot];
    if (!t->valid || t->generation != m->perf_tlb_generation ||
        t->linear_page != page || t->cr3 != c->cr3 ||
        t->cr0 != c->cr0 || t->cr4 != c->cr4 || t->efer != c->efer ||
        t->ac != ((c->rflags >> 18) & 1) ||
        t->cpl != (c->seg[SEG_CS].sel & 3) ||
        t->desc_sv != (c->desc_sv != 0) || t->mode != T_EXEC ||
        t->phys_page > (uint64_t)RAM_SIZE - 4096 ||
        (t->phys_page >= ROM_ALIAS_LOW_BASE &&
         t->phys_page < ROM_ALIAS_LOW_BASE + ROM_SIZE)) return;
    perf_decode_entry_t *e = &m->perf_decode[perf_decode_slot(c)];
    e->generation = m->perf_tlb_generation;
    e->rip = c->rip; e->cs_base = c->seg[SEG_CS].base;
    e->next_pc = d->pc;
    e->cr0 = c->cr0; e->cr3 = c->cr3;
    e->cr4 = c->cr4; e->efer = c->efer;
    e->cs_sel = c->seg[SEG_CS].sel;
    e->cs_l = c->seg[SEG_CS].l; e->cs_db = c->seg[SEG_CS].d_b;
    e->ac = (uint8_t)((c->rflags >> 18) & 1);
    e->desc_sv = (uint8_t)(c->desc_sv != 0);
    e->addrsize = d->addrsize; e->opsize = d->opsize;
    e->has_rex = d->has_rex;
    e->rex_w = d->rex_w; e->rex_r = d->rex_r;
    e->rex_x = d->rex_x; e->rex_b = d->rex_b;
    e->seg_override = d->seg_override; e->rep = d->rep;
    e->opcode = op;
    e->valid = 1;
}

static uint64_t read_code_v(machine_t *m, uint64_t vaddr, int size) {
    uint64_t phys;
    if (!translate(m, vaddr, &phys, T_EXEC)) /* fault already raised */
        return 0;
    return mem_read(m, phys, size);
}
static uint8_t fetch8(dctx_t *d){
    if (dbg_u_on < 0) { const char *e = getenv("EMU_DBG_U"); dbg_u_on = (e && e[0] == '1'); }
    uint64_t la = d->c->seg[SEG_CS].base + d->pc;
#if AURA_DEBUG_HOOKS
    if (dbg_u_on && dbg_u_events < 200 && (d->c->seg[SEG_CS].sel & 3) == 3)
        { dbg_u_events++; mlog(&d->m->log, "[dbg-u] %03d uf la=%llx", dbg_u_events, (unsigned long long)la); }
#endif
    uint8_t v=(uint8_t)read_code_v(d->m, la, 1); d->pc+=1; return v; }
static uint16_t fetch16(dctx_t *d){ uint16_t v=(uint16_t)read_code_v(d->m, d->c->seg[SEG_CS].base+d->pc,2); d->pc+=2; return v; }
static uint32_t fetch32(dctx_t *d){ uint32_t v=(uint32_t)read_code_v(d->m, d->c->seg[SEG_CS].base+d->pc,4); d->pc+=4; return v; }
static uint64_t fetch64(dctx_t *d){ uint64_t v=read_code_v(d->m, d->c->seg[SEG_CS].base+d->pc,8); d->pc+=8; return v; }
static int8_t  fetch8s(dctx_t *d){ return (int8_t)fetch8(d); }
static int32_t fetch32s(dctx_t *d){ return (int32_t)fetch32(d); }
static uint64_t fetch_imm(dctx_t *d, int size) {
    if (size == 1)
        return fetch8(d);
    if (size == 2)
        return fetch16(d);
    if (size == 4)
        return fetch32(d);
    return fetch64(d);
}

static int64_t fetch_imm_signed_z(dctx_t *d, int size) {
    /* The immediate is at most 32 bits and is sign-extended by the caller. */
    if (size == 1)
        return fetch8s(d);
    if (size == 2)
        return (int16_t)fetch16(d);
    return fetch32s(d);
}

/* is_mem: 0 = register operand (reg=index), 1 = memory (addr/seg valid),
 * 2 = memory pending RIP-relative fixup. rsp_base marks an effective address
 * based on RSP/ESP, needed by POP r/m because that form computes its memory
 * destination after incrementing the stack pointer. reg_field is the ModRM.reg
 * bits (5:3), ALWAYS valid regardless of is_mem -- it is either a second
 * register operand or an opcode-extension, depending on the instruction. */
typedef struct { int is_mem; int reg; uint64_t addr; int seg; int reg_field; int rsp_base; } rm_t;
static uint64_t rm_ea(dctx_t *d, rm_t *rm);  /* lazy RIP-relative EA (K2) */

static const int tbl16_base[8] = {RBX,RBX,RBP,RBP,RSI,RDI,RBP,RBX};
static const int tbl16_idx[8]  = {RSI,RDI,RSI,RDI,-1,-1,-1,-1};

static rm_t decode_modrm(dctx_t *d, uint8_t modrm) {
    rm_t out; memset(&out,0,sizeof out); out.seg = d->seg_override;
    int mod=(modrm>>6)&3, rm=modrm&7;
    out.reg_field = ((modrm>>3)&7) | (d->rex_r<<3);
    if (mod==3) { out.is_mem=0; out.reg = rm | (d->rex_b<<3); return out; }
    out.is_mem = 1;
    if (d->addrsize == 16) {
        int32_t base;
        if (mod==0 && rm==6) base = fetch16(d);
        else {
            base = (int32_t)(d->c->gpr[tbl16_base[rm]] & 0xFFFF);
            if (tbl16_idx[rm] >= 0) base += (int32_t)(d->c->gpr[tbl16_idx[rm]] & 0xFFFF);
            if (mod==1) base += fetch8s(d); else if (mod==2) base += (int16_t)fetch16(d);
        }
        out.addr = (uint64_t)base & 0xFFFF;
        if (out.seg < 0) out.seg = (rm==2||rm==3||(rm==6 && mod!=0)) ? SEG_SS : SEG_DS;
        return out;
    }
    uint64_t base_val = 0; int basereg = rm | (d->rex_b<<3); int have_base = 1;
    int64_t disp = 0;
    if (rm == 4) {
        uint8_t sib = fetch8(d);
        int scale = 1 << ((sib>>6)&3);
        int idxf = (sib>>3)&7; int idx = idxf | (d->rex_x<<3);
        int bsef = sib&7; int bse = bsef | (d->rex_b<<3);
        uint64_t idxval = (idxf==4 && !d->rex_x) ? 0 : d->c->gpr[idx];
        if (bsef == 5 && mod == 0) { disp = fetch32s(d); have_base = 0; base_val = 0; }
        else base_val = d->c->gpr[bse];
        base_val += idxval * (uint64_t)scale;
        basereg = bse;
    } else if (rm == 5 && mod == 0) {
        disp = fetch32s(d);
        if (d->addrsize == 64) {
            /* RIP-relative: displacement is from the address of the END of the
             * instruction. We don't know total length yet at decode time for
             * all cases, so we record a pending fixup resolved by the caller
             * once the instruction length is final (see RIP_REL handling in
             * execute()). We approximate using pc at this point and let the
             * caller add any trailing immediate size. */
            out.addr = (uint64_t)(int64_t)disp; /* temp: relative part only */
            out.seg = SEG_DS; out.reg = -1;
            out.is_mem = 2; /* sentinel: needs RIP fixup */
            return out;
        } else { out.addr = (uint64_t)(uint32_t)disp; if (out.seg<0) out.seg=SEG_DS; return out; }
    } else {
        base_val = d->c->gpr[basereg];
    }
    if (mod==1) disp = fetch8s(d); else if (mod==2) disp = fetch32s(d);
    uint64_t eff = base_val + (uint64_t)disp;
    if (d->addrsize == 32) eff &= 0xFFFFFFFFull;
    out.addr = eff;
    out.rsp_base = have_base && basereg == RSP;
    if (out.seg < 0) out.seg = (have_base && ((basereg&7)==RSP || (basereg&7)==RBP)) ? SEG_SS : SEG_DS;
    return out;
}

static uint64_t rm_read(dctx_t *d, rm_t *rm, int size) {
    if (!rm->is_mem) return get_reg(d->c, rm->reg, size, d->has_rex);
    uint64_t base = (rm->seg>=0) ? d->c->seg[rm->seg].base : 0;
    return read_mem_v(d->m, base + rm_ea(d, rm), size);
}
static void rm_write(dctx_t *d, rm_t *rm, int size, uint64_t val) {
    if (!rm->is_mem) { set_reg(d->c, rm->reg, size, d->has_rex, val); return; }
    uint64_t base = (rm->seg>=0) ? d->c->seg[rm->seg].base : 0;
    write_mem_v(d->m, base + rm_ea(d, rm), size, val);
}
/* resolves a pending RIP-relative memory operand once the instruction length is final */
/* K2: RIP-relative fixup is LAZY. The anchor of a [rip+disp32] EA is the
 * first byte AFTER the whole instruction, but at decode time trailing
 * immediates have not been fetched yet, so an eager pc+disp here is short
 * by the immediate length. Measured: the kernel's `movq [rip+disp],imm32`
 * wrote the GDTR base field 4 bytes early; descriptor fetches then read
 * open-bus garbage (the all-FF CS/SS of the first K2 trace). rm->is_mem
 * stays 2 ("rip-rel pending") and rm_ea() resolves at USE time, when the
 * handler has fetched every immediate the encoding carries. Handlers that
 * would consume the operand BEFORE fetching their imm (IMUL r,r/m,imm)
 * fetch the immediate first instead. */
static void fixup_riprel(dctx_t *d, rm_t *rm) { (void)d; (void)rm; }
static uint64_t rm_ea(dctx_t *d, rm_t *rm) {
    return (rm->is_mem == 2) ? (uint64_t)((int64_t)rm->addr + (int64_t)d->pc)
                             : rm->addr;
}

/* ========================= push / pop (stack ops) ========================= */
/* The stack-unit size is a property of the INSTRUCTION (default 8 in long
 * mode, or 2 with a 0x66 prefix; operand size otherwise) and is decided by
 * every caller explicitly. Forcing 8 here broke any 16-bit stack op in long
 * mode (C0 ledger #11): "66 50" (push ax) wrote 8 bytes over the top of the
 * next stack slots. */
static void do_push(dctx_t *d, int size, uint64_t val) {
    cpu_t *c = d->c;
    c->gpr[RSP] -= (uint64_t)size;
    write_mem_v(d->m, c->seg[SEG_SS].base + c->gpr[RSP], size, val);
}
static uint64_t do_pop(dctx_t *d, int size) {
    cpu_t *c = d->c;
    uint64_t v = read_mem_v(d->m, c->seg[SEG_SS].base + c->gpr[RSP], size);
    c->gpr[RSP] += (uint64_t)size;
    return v;
}
/* stack unit for instructions whose size follows the operand-size override */
static int stack_unit(dctx_t *d) {
    return (cpu_addr_size(d->c)==64) ? (d->opsize==2 ? 2 : 8) : d->opsize;
}

/* =================== string ops: address-size plumbing ==================== */
/* MOVS/CMPS/SCAS/LODS/STOS/INS/OUTS (and LOOP/JCXZ) share: an address-size
 * dependent pointer width (SI/DI 16-bit, ESI/EDI 32, RSI/RDI 64), a count
 * register in the same width, a direction from DF, and a source segment that
 * may be overridden (the destination is always ES). Each width wraps at its
 * natural boundary inside the 64-bit GPR, leaving the upper bits intact. */
static uint64_t str_ptr(cpu_t *c, int reg, int as) {
    if (as==64) return c->gpr[reg];
    if (as==32) return (uint32_t)c->gpr[reg];
    return (uint16_t)c->gpr[reg];
}
static void str_set_ptr(cpu_t *c, int reg, int as, uint64_t v) {
    if (as==64) c->gpr[reg]=v;
    else if (as==32) c->gpr[reg]=(c->gpr[reg] & 0xFFFFFFFF00000000ull) | (uint32_t)v;
    else c->gpr[reg]=(c->gpr[reg] & 0xFFFFFFFFFFFF0000ull) | (uint16_t)v;
}
static void str_adv(cpu_t *c, int reg, int as, int size, int dir) {
    str_set_ptr(c, reg, as, str_ptr(c, reg, as) + (uint64_t)((int64_t)dir * size));
}
static uint64_t str_count(cpu_t *c, int as) { return str_ptr(c, RCX, as); }
static void str_dec_count(cpu_t *c, int as) { str_set_ptr(c, RCX, as, str_ptr(c, RCX, as) - 1); }

/* =========================== group-op ALU helper =========================== */
enum { ALU_ADD=0, ALU_OR=1, ALU_ADC=2, ALU_SBB=3, ALU_AND=4, ALU_SUB=5, ALU_XOR=6, ALU_CMP=7 };
static uint64_t alu_op(cpu_t *c, int op, uint64_t a, uint64_t b, int size, int *write_back) {
    uint64_t res=0; *write_back=1;
    switch(op){
        case ALU_ADD: res=a+b; set_flags_add(c,a,b,0,res,size); break;
        case ALU_OR:  res=a|b; set_flags_logic(c,res,size); break;
        case ALU_AND: res=a&b; set_flags_logic(c,res,size); break;
        case ALU_SUB: res=a-b; set_flags_sub(c,a,b,0,res,size); break;
        case ALU_XOR: res=a^b; set_flags_logic(c,res,size); break;
        case ALU_CMP: res=a-b; set_flags_sub(c,a,b,0,res,size); *write_back=0; break;
        case ALU_ADC: { uint64_t cin = (c->rflags&FLAG_CF)?1:0; res=a+b+cin; set_flags_add(c,a,b,cin,res,size); break; }
        case ALU_SBB: { uint64_t cin = (c->rflags&FLAG_CF)?1:0; res=a-b-cin; set_flags_sub(c,a,b,cin,res,size); break; }
    }
    return res;
}

/* ============================== CPUID ===================================== */
static void do_cpuid(machine_t *m) {
    cpu_t *c = &m->cpu;
    uint32_t leaf = (uint32_t)c->gpr[RAX];
    uint32_t a=0,b=0,cc=0,dd=0;
    platform_cpuid(m->plat, leaf, (uint32_t)c->gpr[RCX], &a,&b,&cc,&dd);
    /* COMPAT G6: leaf 1 EBX[31:24] reports this logical processor's initial
     * APIC ID (m->lapic.id[31:24]); 0 on the BSP and in UP, so single-vCPU
     * and BSP receipts stay bit-identical while APs identify themselves to
     * SeaBIOS smp_write_msr(). */
    if (leaf == 1) {
        b = (b & 0x00FFFFFFu) | (m->lapic.id & 0xFF000000u);
        /* UPKEEP U4 (SDM 10.4.3): with the local APIC globally disabled
         * (IA32_APIC_BASE[11]=0) the CPUID APIC feature flag reads 0 --
         * the processor looks like it has no on-chip APIC. */
        int f; uint64_t ab = cpu_get_msr(c, LAPIC_MSR_APICBASE, &f);
        if (f && !(ab & LAPIC_MSR_EN)) dd &= ~(1u << 9);
    }
    c->gpr[RAX] = a; c->gpr[RBX] = b; c->gpr[RCX] = cc; c->gpr[RDX] = dd;
}

/* ======================== condition evaluation =========================== */
/* Single source of truth for every cc-based instruction: short/near Jcc,
 * CMOVcc and SETcc. Fixes a whole bug class found at the C0 baseline: the
 * near-Jcc handler lacked opcodes 0F 80-83 entirely (#UD) and had no branches
 * for conditions 0x8-0xB, so JS/JNS/JP/JPE/JPO decoded but NEVER took. */
static int eval_cc(int cc, uint64_t f) {
    switch (cc & 0xF) {
    case 0x0: return (f & FLAG_OF) != 0;                                                   /* O   */
    case 0x1: return (f & FLAG_OF) == 0;                                                   /* NO  */
    case 0x2: return (f & FLAG_CF) != 0;                                                   /* B/C/NAE */
    case 0x3: return (f & FLAG_CF) == 0;                                                   /* AE/NB/NC */
    case 0x4: return (f & FLAG_ZF) != 0;                                                   /* E/Z */
    case 0x5: return (f & FLAG_ZF) == 0;                                                   /* NE/NZ */
    case 0x6: return (f & (FLAG_ZF|FLAG_CF)) != 0;                                         /* BE/NA */
    case 0x7: return (f & (FLAG_ZF|FLAG_CF)) == 0;                                         /* A/NBE */
    case 0x8: return (f & FLAG_SF) != 0;                                                   /* S   */
    case 0x9: return (f & FLAG_SF) == 0;                                                   /* NS  */
    case 0xA: return (f & FLAG_PF) != 0;                                                   /* P/PE */
    case 0xB: return (f & FLAG_PF) == 0;                                                   /* NP/PO */
    case 0xC: return ((f & FLAG_SF) != 0) != ((f & FLAG_OF) != 0);                         /* L/NGE */
    case 0xD: return ((f & FLAG_SF) != 0) == ((f & FLAG_OF) != 0);                         /* GE/NL */
    case 0xE: return (f & FLAG_ZF) || (((f & FLAG_SF) != 0) != ((f & FLAG_OF) != 0));      /* LE/NG */
    case 0xF: return !(f & FLAG_ZF) && (((f & FLAG_SF) != 0) == ((f & FLAG_OF) != 0));     /* G/NLE */
    }
    return 0;
}

/* ================================ reset =================================== */
void cpu_reset(cpu_t *c) {
    machine_t *mach = c->mach; /* preserved across reset */
    memset(c, 0, sizeof *c);
    c->mach = mach;
    c->seg[SEG_CS].sel = 0xF000; c->seg[SEG_CS].base = 0xFFFF0000ULL; c->seg[SEG_CS].limit=0xFFFF;
    c->seg[SEG_DS].limit = c->seg[SEG_ES].limit = c->seg[SEG_SS].limit = 0xFFFF;
    c->seg[SEG_FS].limit = c->seg[SEG_GS].limit = 0xFFFF;
    c->rip = 0xFFF0;
    c->rflags = 0x2;
    c->mxcsr = 0x1F80;   /* architected power-on value (K3) */
    c->fcw = 0x037F;       /* architected x87 init control word (K3) */
    c->ftw = 0xFFFF;       /* F1: all eight x87 registers empty at power-on */
    c->cr0 = 0x60000010ULL;
    c->halted = 0; c->fault = 0; c->intr_delay = 0;
    /* CHIPSET H6 + UPKEEP U4: IA32_APIC_BASE resets to base|EN, with the
     * BSP bit set only on the bootstrap processor (SDM 10.4.4: "set to
     * 1 for the processor selected as the BSP and set to 0 for the
     * remaining processors"; the xAPIC comes out of reset hardware-
     * enabled and software-disabled at SVR). The role comes from the
     * cpu's identity in its machine: context 0 is the BSP. */
    {
        int is_bsp = 1;
        if (mach && mach->n_vcpus > 1) {
            if (c == &mach->cpu) is_bsp = (mach->cur_vcpu == 0);
            else for (int i = 0; i < EM_MAX_VCPU; i++)
                     if (c == &mach->vcpu[i].c) { is_bsp = (i == 0); break; }
        }
        cpu_set_msr(c, LAPIC_MSR_APICBASE,
                    LAPIC_BASE | LAPIC_MSR_EN | (is_bsp ? LAPIC_MSR_BSP : 0));
        /* a reset of the live pair re-bases the MMIO window too */
        if (mach && c == &mach->cpu) lapic_sync_mmio(mach);
    }
    /* CPU_FEATURES CF2: IA32_PAT leaves reset at the SDM default
     * (cpu.h). Seeded here -- the RESET path -- only; cpu_init_reset
     * below preserves the whole sparse table across INIT# exactly as
     * silicon does, so an INIT keeps whatever PAT the guest set. */
    cpu_set_msr(c, MSR_IA32_PAT, MSR_IA32_PAT_RESET);
}

/* UPKEEP U5: the INIT# processor state (Intel SDM Vol.3A Table 9-1,
 * the INIT column; implementation-measured against QEMU do_cpu_init's
 * start_init_save..end_init_save region). Everything cpu_reset()
 * does, EXCEPT the state silicon keeps over an INIT:
 *   - x87 register BIT PATTERNS (the control file becomes the FINIT
 *     state -- tags empty, FCW=037F, FIP/FDP/FOP=0 -- while the ST
 *     bits underneath stay stale, exactly like FINIT itself),
 *   - the XMM file and MXCSR,
 *   - IA32_KERNEL_GS_BASE,
 *   - the software-configured MSRs, except the three the INIT column
 *     puts back at their reset 0: EFER, FS_BASE, GS_BASE.
 * Emulator bookkeeping that machine_reset preserves too (the
 * monotonic step counters and the debug trace flag) survives alike.
 * Used by both INIT sources: the LAPIC INIT IPI and the chipset
 * CF9 INIT#. */
void cpu_init_reset(cpu_t *c) {
    machine_t *mach = c->mach;
    msr_entry_t keep_msr[MAX_MSR];
    int keep_n = c->msr_count;
    memcpy(keep_msr, c->msr, sizeof keep_msr);
    long double keep_fst[8];
    memcpy(keep_fst, c->fst, sizeof keep_fst);
    uint64_t keep_xmm[16][2];
    memcpy(keep_xmm, c->xmm, sizeof keep_xmm);
    uint32_t keep_mxcsr = c->mxcsr;
    uint64_t keep_kgs = c->kgs_base;
    uint64_t keep_steps = c->executed_steps;
    uint64_t keep_instr = c->instr_count;
    int keep_trace = c->trace;

    cpu_reset(c);

    memcpy(c->msr, keep_msr, sizeof keep_msr);
    c->msr_count = keep_n;
    cpu_set_msr(c, 0xC0000080u, 0);        /* EFER: reset 0 */
    cpu_set_msr(c, 0xC0000100u, 0);        /* IA32_FS_BASE */
    cpu_set_msr(c, 0xC0000101u, 0);        /* IA32_GS_BASE */
    memcpy(c->fst, keep_fst, sizeof keep_fst);
    memcpy(c->xmm, keep_xmm, sizeof keep_xmm);
    c->mxcsr = keep_mxcsr;
    c->kgs_base = keep_kgs;
    c->executed_steps = keep_steps;
    c->instr_count = keep_instr;
    c->trace = keep_trace;
    /* cpu_reset synced the window to the reseeded default; the
     * preserved IA32_APIC_BASE may say otherwise -- resync. */
    if (mach && c == &mach->cpu) lapic_sync_mmio(mach);
}

/* ---------------------------------------------------------------- x87 (F1) --
 * The x87 data and control layer (phase F1). The register file lives in
 * fst[8] as host long double -- the ISA's own 80-bit extended format --
 * with ftop as FSW.TOP and a full 2-bit-per-register FTW. Conversions and
 * rounding run through <fenv.h>: fesetround() mirrors FCW.RC and the host
 * FP status flags fold back into FSW, so architected rounding and
 * exception bits come out of the host unit, not out of software
 * arithmetic. When the host long double is not the 80-bit format the two
 * representations would silently disagree, so ops raise #UD there instead
 * of pretending (the frozen "no data layer" boundary stays honest).
 *
 * FPU_PLAN.md phase split: this block owns D9/DB/DD/DF (loads, stores,
 * constants, control, env/save, FCMOV). The arithmetic escapes
 * D8/DA/DC/DE (incl. FCOM/FCOMPP) stay the deliberate #UD boundary of
 * phase F2. Escapes and WAIT honour CR0.TS -> #NM(7) and CR0.EM -> #UD.
 *
 * FXSAVE x87/MXCSR image (HOST-MEASURED, /tmp/fxprobe2 receipt, pinned by
 * test_fpu.c::v_fxsave_live_layout -- the pre-F1 code wrote the whole
 * prefix two bytes off and stored MXCSR into the MASK slot):
 *   0 FCW, 2 FSW, 4 FTW-abridged(1B), 6 FOP, 8 FIP, 12 FCS, 16 FDP,
 *   20 FDS, 24 MXCSR, 28 MXCSR_MASK, 32+16*i ST(i) logical order (low
 *   10 bytes = 80-bit value), 160+16*i XMMi.
 */

enum { X87_T_VALID = 0, X87_T_ZERO = 1, X87_T_SPECIAL = 2, X87_T_EMPTY = 3 };

/* HOST-MEASURED exception-bit order (/tmp/flagbits, defect D6: F1 had
 * these reversed): IE DE ZE OE UE PE -- the FCW mask bits mirror it. */
#define FSW_IE 0x0001u
#define FSW_DE 0x0002u
#define FSW_ZE 0x0004u
#define FSW_OE 0x0008u
#define FSW_UE 0x0010u
#define FSW_PE 0x0020u
#define FSW_SF 0x0040u
#define FSW_ES 0x0080u
#define FSW_C0 0x0100u
#define FSW_C1 0x0200u
#define FSW_C2 0x0400u
#define FSW_TOPM 0x3800u
#define FSW_C3 0x4000u
#define FSW_B  0x8000u
#define FCW_EXCM 0x003Fu   /* per-exception mask bits, same order as FSW */

/* 80-bit extended <-> host long double. On LDBL_MANT_DIG==64 hosts the
 * struct is 10 value bytes plus padding: a plain memcpy is bit-exact. */
static void x87_ld_to80(long double x, uint8_t b[10]) {
    uint8_t t[16]; memcpy(t, &x, 16); memcpy(b, t, 10);
}
static long double x87_ld_from80(const uint8_t b[10]) {
    uint8_t t[16] = {0}; long double x; memcpy(t, b, 10); memcpy(&x, t, 16); return x;
}
static int x87_host_ok(void) {
#if (defined(__x86_64__) || defined(__i386__)) && defined(__LDBL_MANT_DIG__) && defined(__LDBL_MAX_EXP__)
    return (__LDBL_MANT_DIG__ == 64 && __LDBL_MAX_EXP__ == 16384);
#else
    return 0;
#endif
}

static void x87_set_tag(cpu_t *c, int phys, int tag) {
    c->ftw = (uint16_t)((c->ftw & ~(3u << (phys * 2))) | ((unsigned)tag << (phys * 2)));
}
static int x87_get_tag(const cpu_t *c, int phys) { return (c->ftw >> (phys * 2)) & 3; }
static int x87_phys(const cpu_t *c, int sti) { return (c->ftop + sti) & 7; }

/* class of a value for the FTW (empty is set by the stack op itself) */
static int x87_class(long double v) {
    uint8_t b[10]; x87_ld_to80(v, b);
    uint16_t se = (uint16_t)(b[8] | ((uint16_t)b[9] << 8));
    uint64_t sig = 0; memcpy(&sig, b, 8);
    unsigned exp = se & 0x7FFFu;
    if (exp == 0 && sig == 0) return X87_T_ZERO;
    if (exp == 0x7FFFu) return X87_T_SPECIAL;
    if (exp == 0) return X87_T_SPECIAL;          /* denormal: "special" tag */
    (void)v;
    return X87_T_VALID;
}
/* FXAM classification codes (C3,C2,C0): see SDM FXAM */
static void x87_fxam_code(long double v, int empty, unsigned *c3, unsigned *c2, unsigned *c0, unsigned *c1) {
    uint8_t b[10]; x87_ld_to80(v, b);
    uint16_t se = (uint16_t)(b[8] | ((uint16_t)b[9] << 8));
    uint64_t sig = 0; memcpy(&sig, b, 8);
    unsigned exp = se & 0x7FFFu;
    *c1 = (se >> 15) & 1u;
    if (empty)          { *c3 = 1; *c2 = 1; *c0 = 1; return; }
    if (exp == 0x7FFFu) { if (sig & 0x7FFFFFFFFFFFFFFFull) { *c3 = 0; *c2 = 1; *c0 = 1; }
                          else                              { *c3 = 1; *c2 = 1; *c0 = 0; } return; }
    if (exp == 0)       { *c3 = (sig == 0); *c2 = 0; *c0 = 0; return; } /* zero vs denormal */
                        { *c3 = 0; *c2 = 1; *c0 = 0; }
}

static void x87_sync_top(cpu_t *c) {
    c->fsw = (uint16_t)((c->fsw & (uint16_t)~FSW_TOPM) | (((unsigned)c->ftop & 7u) << 11));
}
static void x87_set_c(cpu_t *c, unsigned c0, unsigned c1, unsigned c2, unsigned c3) {
    c->fsw &= (uint16_t)~(FSW_C0 | FSW_C1 | FSW_C2 | FSW_C3);
    if (c0) c->fsw |= FSW_C0;
    if (c1) c->fsw |= FSW_C1;
    if (c2) c->fsw |= FSW_C2;
    if (c3) c->fsw |= FSW_C3;
}

/* F4: every fst write goes through x87_w. The slots are long doubles --
 * 10 data bytes plus 6 padding bytes -- and only the raw cpu_t dump (the
 * snapshot's machine.lo/machine.hi) ever sees the padding; FNSAVE and
 * FXSAVE emit explicit 10-byte (+zeroed reserved) forms. Measured defect
 * (F4 lanes): a computed long double (FBLD's accumulator) assigned to a
 * slot carried run-dependent stack garbage into the padding, so two
 * processes saving the same state wrote different snapshot files.
 * Normalizing at every write keeps the raw dump byte-stable. */
static void x87_w(cpu_t *c, int p, long double v) {
    c->fst[p] = v;
    memset((char *)&c->fst[p] + 10, 0, 6);
}

/* UPKEEP U3: the gdbstub bridge declared in cpu.h.  All ST(i) traffic is
 * logical (x87_phys rotation).  Both converters are no-ops on a host whose
 * long double is not the 80-bit format -- the memcpy in x87_ld_from80
 * would overflow a narrower slot, and the stub has already reported the
 * registers unavailable there.  The write normalizes padding (x87_w, so
 * snapshots stay byte-stable) and re-classifies the destination tag from
 * the value: an empty register written through RSP becomes valid/zero/
 * special, never a valid-bytes-but-tagged-empty state no instruction
 * sequence could consume. */
int cpu_x87_host_supported(void) { return x87_host_ok(); }
void cpu_x87_st_to80(const cpu_t *c, int sti, uint8_t b[10]) {
    if (!x87_host_ok()) { memset(b, 0, 10); return; }
    x87_ld_to80(c->fst[x87_phys(c, sti)], b);
}
void cpu_x87_st_from80(cpu_t *c, int sti, const uint8_t b[10]) {
    if (!x87_host_ok()) return;
    int p = x87_phys(c, sti);
    long double v = x87_ld_from80(b);
    x87_w(c, p, v);
    x87_set_tag(c, p, x87_class(v));
}
void cpu_x87_set_fsw(cpu_t *c, uint16_t v) {
    c->fsw = v;
    c->ftop = (v >> 11) & 7;          /* x87_load_env's invariant */
}

/* push: stack overflow overwrites the last register and raises IE|SF|C1 */
static void x87_push(cpu_t *c, long double v, uint16_t *raised) {
    int dst = (c->ftop - 1) & 7;
    if (x87_get_tag(c, dst) != X87_T_EMPTY) {
        *raised |= FSW_IE | FSW_SF; c->fsw |= FSW_C1;
    }
    c->ftop = dst;
    x87_w(c, c->ftop, v);
    x87_set_tag(c, c->ftop, x87_class(v));
    x87_sync_top(c);
}
/* pop: underflow is popping an ALREADY-empty ST(0) (legal pops that empty
 * the stack are not faults); raises IE|SF with C1=0 */
static void x87_pop(cpu_t *c, uint16_t *raised) {
    int was_empty = x87_get_tag(c, c->ftop) == X87_T_EMPTY;
    x87_set_tag(c, c->ftop, X87_T_EMPTY);
    c->ftop = (c->ftop + 1) & 7;
    x87_sync_top(c);
    if (was_empty) {
        *raised |= FSW_IE | FSW_SF; c->fsw &= (uint16_t)~FSW_C1;
    }
}
static int x87_st_empty(const cpu_t *c, int sti) {
    return x87_get_tag(c, x87_phys(c, sti)) == X87_T_EMPTY;
}
/* underflow of an ST(i) reference: IE|SF, C1=0 */
static int x87_check_src(cpu_t *c, int sti, uint16_t *raised) {
    if (!x87_st_empty(c, sti)) return 1;
    *raised |= FSW_IE | FSW_SF; c->fsw &= (uint16_t)~FSW_C1;
    return 0;
}

static int x87_rc(const cpu_t *c) { return (c->fcw >> 10) & 3; }
static void x87_set_round(const cpu_t *c) {
    static const int map[4] = { FE_TONEAREST, FE_DOWNWARD, FE_UPWARD, FE_TOWARDZERO };
    fesetround(map[x87_rc(c)]);
}
static void x87_get_fenv(uint16_t *raised) {
    int f = fetestexcept(FE_ALL_EXCEPT);
    if (f & FE_INEXACT)   *raised |= FSW_PE;
    if (f & FE_UNDERFLOW) *raised |= FSW_UE;
    if (f & FE_OVERFLOW)  *raised |= FSW_OE;
    if (f & FE_DIVBYZERO) *raised |= FSW_ZE;
    if (f & FE_INVALID)   *raised |= FSW_IE;
}

/* ---- FXSAVE abridged tag: one bit per PHYSICAL register, 1 = non-empty ---- */
static uint8_t x87_ftw_abridged(const cpu_t *c) {
    uint8_t r = 0;
    for (int p = 0; p < 8; p++)
        if (x87_get_tag(c, p) != X87_T_EMPTY) r |= (uint8_t)(1u << p);
    return r;
}

/* ---- conversion core: memory <-> ST ---- */
static long double x87_from_m32(uint32_t bits) {
    float f; memcpy(&f, &bits, 4);
    return (long double)f;
}
static long double x87_from_m64(uint64_t bits) {
    double d; memcpy(&d, &bits, 8);
    return (long double)d;
}
static uint32_t x87_to_m32(long double v) {
    /* the host x87 store honors the current rounding direction */
    volatile long double t = v;
    float f = (float)t;
    uint32_t bits; memcpy(&bits, &f, 4);
    return bits;
}
static uint64_t x87_to_m64(long double v) {
    volatile long double t = v;
    double d = (double)t;
    uint64_t bits; memcpy(&bits, &d, 8);
    return bits;
}
/* integer stores: round per RC via rintl (the raw C cast truncates), then
 * range-check; out-of-range/NaN stores the architected indefinite and IE */
static long double x87_round_to_int(long double v) {
    volatile long double t = v;
    return rintl(t);
}
static int x87_to_int32(long double v, int32_t *out, uint16_t *raised) {
    long double r = x87_round_to_int(v);
    if (isnan(r)) { *raised |= FSW_IE; *out = (int32_t)0x80000000u; return 1; }
    if (r >= 2147483648.0L || r < -2147483648.0L) {
        *raised |= FSW_IE; *out = (int32_t)0x80000000u; return 1;
    }
    *out = (int32_t)r;
    if (r != (long double)v) *raised |= FSW_PE;
    return 1;
}
static int x87_to_int64(long double v, int64_t *out, uint16_t *raised) {
    long double r = x87_round_to_int(v);
    long double lim = 9223372036854775808.0L;
    if (isnan(r)) { *raised |= FSW_IE; *out = (int64_t)0x8000000000000000ull; return 1; }
    if (r >= lim || r < -lim) {
        *raised |= FSW_IE; *out = (int64_t)0x8000000000000000ull; return 1;
    }
    *out = (int64_t)r;
    if (r != (long double)v) *raised |= FSW_PE;
    return 1;
}
static int x87_to_int16(long double v, int16_t *out, uint16_t *raised) {
    long double r = x87_round_to_int(v);
    if (isnan(r)) { *raised |= FSW_IE; *out = (int16_t)0x8000u; return 1; }
    if (r >= 32768.0L || r < -32768.0L) {
        *raised |= FSW_IE; *out = (int16_t)0x8000u; return 1;
    }
    *out = (int16_t)r;
    if (r != (long double)v) *raised |= FSW_PE;
    return 1;
}
/* packed BCD (10 bytes): 18 digits + sign nibble (F3 receipts: bcdprobe,
 * transprobe3/5). FBLD is a pure weighted nibble sum -- nibble values
 * A-F count as 10-15 with NO invalid-digit exception, sign = bit 7 of
 * byte 9 (low nibble ignored). FBSTP rounds to an integer per FCW.RC
 * (ties-to-even), sets PE iff the value changed and C1 iff the magnitude
 * rounded UP, keeps the source sign even for a zero result, and on
 * NaN/Inf/|result| >= 1e18 stores the QNaN-indefinite bit pattern (mem
 * 00 x7 C0 FF FF) with IE. Denormal sources do NOT raise DE (measured). */
static long double x87_from_bcd(const uint8_t b[10]) {
    long double v = 0, mul = 1;
    for (int i = 0; i < 9; i++) {
        v += (long double)(b[i] & 0xF) * mul; mul *= 10;
        v += (long double)(b[i] >> 4) * mul; mul *= 10;
    }
    if (b[9] & 0x80) v = -v;
    return v;
}
static void x87_to_bcd(long double v, uint8_t b[10], uint16_t *raised, unsigned *c1) {
    memset(b, 0, 10);
    long double r = x87_round_to_int(v);   /* rintl under the current RC */
    if (isnan(r) || isinf(r) || fabsl(r) >= 1.0e18L) {
        b[7] = 0xC0; b[8] = 0xFF; b[9] = 0xFF;   /* QNaN-indefinite */
        *raised |= FSW_IE;
        *c1 = 0;
        return;
    }
    if (r != v) {
        *raised |= FSW_PE;
        *c1 = (fabsl(r) > fabsl(v)) ? 1u : 0u;
    } else {
        *c1 = 0;
    }
    uint64_t n = (uint64_t)fabsl(r);   /* |r| < 1e18 fits */
    for (int i = 0; i < 9; i++) {
        b[i] = (uint8_t)((n % 10u) | ((n / 10u % 10u) << 4));
        n /= 100u;
    }
    b[9] = signbit(v) ? 0x80u : 0x00u;
}

/* ---- environment image (HOST-MEASURED layouts, envprobe2 receipt) ----
 * 16-bit form (14B): CW@0 SW@2 TW@4 FIP16@6 FCS@8 FDP16@10 FDS@12
 * 32-bit form (28B): CW@0 rsv(FFFF)@2 SW@4 rsv@6 TW@8 rsv@10 FIP32@12
 *                    FCS@16 FOP@18 FDP32@20 FDS@24 rsv@26
 * FNSAVE appends ST(0)..ST(7) in LOGICAL order, 10 bytes each. */
static void x87_save_env(cpu_t *c, uint64_t ea, int wide) {
    machine_t *m = c->mach;
    if (!wide) {
        write_mem_v(m, ea +  0, 2, c->fcw);
        write_mem_v(m, ea +  2, 2, c->fsw);
        write_mem_v(m, ea +  4, 2, c->ftw);
        write_mem_v(m, ea +  6, 2, (uint16_t)c->fip);
        write_mem_v(m, ea +  8, 2, c->fcs);
        write_mem_v(m, ea + 10, 2, (uint16_t)c->fdp);
        write_mem_v(m, ea + 12, 2, c->fds);
    } else {
        write_mem_v(m, ea +  0, 2, c->fcw);
        write_mem_v(m, ea +  2, 2, 0xFFFF);
        write_mem_v(m, ea +  4, 2, c->fsw);
        write_mem_v(m, ea +  6, 2, 0xFFFF);
        write_mem_v(m, ea +  8, 2, c->ftw);
        write_mem_v(m, ea + 10, 2, 0xFFFF);
        write_mem_v(m, ea + 12, 4, (uint32_t)c->fip);
        write_mem_v(m, ea + 16, 2, c->fcs);
        write_mem_v(m, ea + 18, 2, c->fop);
        write_mem_v(m, ea + 20, 4, (uint32_t)c->fdp);
        write_mem_v(m, ea + 24, 2, c->fds);
        write_mem_v(m, ea + 26, 2, 0xFFFF);
    }
}
static void x87_load_env(cpu_t *c, uint64_t ea, int wide) {
    machine_t *m = c->mach;
    if (!wide) {
        c->fcw = (uint16_t)read_mem_v(m, ea +  0, 2);
        c->fsw = (uint16_t)read_mem_v(m, ea +  2, 2);
        c->ftw = (uint16_t)read_mem_v(m, ea +  4, 2);
        c->fip = (uint16_t)read_mem_v(m, ea +  6, 2);
        c->fcs = (uint16_t)read_mem_v(m, ea +  8, 2);
        c->fdp = (uint16_t)read_mem_v(m, ea + 10, 2);
        c->fds = (uint16_t)read_mem_v(m, ea + 12, 2);
        c->fop = 0;
    } else {
        c->fcw = (uint16_t)read_mem_v(m, ea +  0, 2);
        c->fsw = (uint16_t)read_mem_v(m, ea +  4, 2);
        c->ftw = (uint16_t)read_mem_v(m, ea +  8, 2);
        c->fip = read_mem_v(m, ea + 12, 4);
        c->fcs = (uint16_t)read_mem_v(m, ea + 16, 2);
        c->fop = (uint16_t)read_mem_v(m, ea + 18, 2);
        c->fdp = read_mem_v(m, ea + 20, 4);
        c->fds = (uint16_t)read_mem_v(m, ea + 24, 2);
    }
    c->ftop = (c->fsw >> 11) & 7;
}
static void x87_save_regs(cpu_t *c, uint64_t ea) {   /* logical ST(0)..ST(7) */
    uint8_t b[10];
    for (int i = 0; i < 8; i++) {
        x87_ld_to80(c->fst[x87_phys(c, i)], b);
        for (int j = 0; j < 10; j++) write_mem_v(c->mach, ea + (uint64_t)i * 10 + (uint64_t)j, 1, b[j]);
    }
}
static void x87_load_regs(cpu_t *c, uint64_t ea) {
    uint8_t b[10];
    for (int i = 0; i < 8; i++) {
        for (int j = 0; j < 10; j++) b[j] = (uint8_t)read_mem_v(c->mach, ea + (uint64_t)i * 10 + (uint64_t)j, 1);
        x87_w(c, x87_phys(c, i), x87_ld_from80(b));
    }
}

static void x87_reset(cpu_t *c) {   /* FNINIT */
    c->fcw = 0x037F;
    c->fsw = 0;
    c->ftw = 0xFFFF;
    c->ftop = 0;
    c->fop = 0; c->fcs = 0; c->fds = 0; c->fip = 0; c->fdp = 0;
    for (int i = 0; i < 8; i++) x87_w(c, i, 0.0L);
}

/* EA of an x87 memory operand */
static uint64_t x87_ea(dctx_t *d, rm_t *rm) {
    uint64_t base = (rm->seg >= 0) ? d->c->seg[rm->seg].base : 0;
    return base + rm_ea(d, rm);
}

/* ---------------------------------------------------------------- x87 F2 --
 * Arithmetic and compare (phase F2). Every numeric op runs on the host
 * x87 itself -- the same 80-bit extended unit the ISA specifies -- via
 * volatile long double expressions (add/sub/mul/div) or raw instruction
 * bytes (fsqrt/fprem/fprem1/frndint/fscale/fxtract have no C spelling).
 * Result bits and NaN patterns therefore come out of silicon, and the
 * <fenv.h> mapping from F1 carries the exception flags back into FSW.
 *
 * Operand-direction receipts (raw-byte probes, /tmp/rawdir):
 *   D8 reg: OP ST(0),ST(i)   -> st0 = st0 OP sti (4=SUB st0-sti, 5=SUBR)
 *   DC reg: OP ST(i),ST(0)   -> sti = sti OP st0 (E0+i=FSUBR st0-sti)
 *   DE reg: OPP ST(i),ST(0)  -> same as DC then pop
 *   D8/DC/DA/DE memory forms all use the NON-reversed /4-/7 map
 *   (DC /4 = FSUB m64, /5 = FSUBR -- measured, matching ndisasm).
 *
 * FSW exception bits (HOST-MEASURED, /tmp/flagbits -- defect D6: the F1
 * map had these reversed):
 *   IE=0x0001 DE=0x0002 ZE=0x0004 OE=0x0008 UE=0x0010 PE=0x0020
 */

enum { XA_ADD, XA_MUL, XA_SUB, XA_SUBR, XA_DIV, XA_DIVR };

static int x87_is_denorm(long double v) {
    uint8_t b[10]; x87_ld_to80(v, b);
    uint16_t se = (uint16_t)(b[8] | ((uint16_t)b[9] << 8));
    uint64_t sig = 0; memcpy(&sig, b, 8);
    return (se & 0x7FFFu) == 0 && sig != 0;
}
static int x87_is_snan(long double v) {
    uint8_t b[10]; x87_ld_to80(v, b);
    uint16_t se = (uint16_t)(b[8] | ((uint16_t)b[9] << 8));
    uint64_t sig = 0; memcpy(&sig, b, 8);
    return (se & 0x7FFFu) == 0x7FFFu && (sig & 0x7FFFFFFFFFFFFFFFull) != 0
        && ((sig >> 62) & 1) == 0;   /* quiet bit clear (pseudo-NaN too) */
}

static long double x87_binop(cpu_t *c, int kind, long double a, long double b, uint16_t *raised) {
    if (x87_is_denorm(a) || x87_is_denorm(b)) *raised |= FSW_DE;
    volatile long double x = a, y = b, r = 0;
    x87_set_round(c);
    feclearexcept(FE_ALL_EXCEPT);
    switch (kind) {
    case XA_ADD:  r = x + y; break;
    case XA_MUL:  r = x * y; break;
    case XA_SUB:  r = x - y; break;
    case XA_SUBR: r = y - x; break;
    case XA_DIV:  r = x / y; break;
    case XA_DIVR: r = y / x; break;
    }
    x87_get_fenv(raised);
    return r;
}

#if defined(__x86_64__) || defined(__i386__)
static long double x87_u_sqrt(long double v, uint16_t *raised) {
    long double r;
    if (x87_is_denorm(v)) *raised |= FSW_DE;
    feclearexcept(FE_ALL_EXCEPT);
    __asm__ volatile("fldt %1; fsqrt; fstpt %0" : "=m"(r) : "m"(v));
    x87_get_fenv(raised);
    return r;
}
static long double x87_u_rndint(long double v, uint16_t *raised) {
    long double r;
    if (x87_is_denorm(v)) *raised |= FSW_DE;
    feclearexcept(FE_ALL_EXCEPT);
    __asm__ volatile("fldt %1; frndint; fstpt %0" : "=m"(r) : "m"(v));
    x87_get_fenv(raised);
    return r;
}
static long double x87_u_scale(long double a, long double b, uint16_t *raised) {
    long double r;
    if (x87_is_denorm(a) || x87_is_denorm(b)) *raised |= FSW_DE;
    feclearexcept(FE_ALL_EXCEPT);
    __asm__ volatile("fldt %2; fldt %1; fscale; fstp %%st(1); fstpt %0"
                 : "=m"(r) : "m"(a), "m"(b) : "st");
    x87_get_fenv(raised);
    return r;
}
static long double x87_u_prem(long double a, long double b, int ieee,
                              uint16_t *raised, uint16_t *sw_out) {
    long double r; uint16_t sw;
    if (x87_is_denorm(a) || x87_is_denorm(b)) *raised |= FSW_DE;
    feclearexcept(FE_ALL_EXCEPT);
    if (ieee)
        __asm__ volatile("fldt %3; fldt %2; fprem1; fnstsw %1; fstp %%st(1); fstpt %0"
                     : "=m"(r), "=a"(sw) : "m"(a), "m"(b) : "st", "memory");
    else
        __asm__ volatile("fldt %3; fldt %2; fprem; fnstsw %1; fstp %%st(1); fstpt %0"
                     : "=m"(r), "=a"(sw) : "m"(a), "m"(b) : "st", "memory");
    x87_get_fenv(raised);
    if (sw_out) *sw_out = sw;
    return r;
}
static void x87_u_xtract(long double v, long double *sig, long double *exp,
                         uint16_t *raised) {
    if (x87_is_denorm(v)) *raised |= FSW_DE;
    feclearexcept(FE_ALL_EXCEPT);
    /* HOST-MEASURED (/tmp/rawdir2): after FXTRACT, ST0 = significand and
     * ST1 = the unbiased exponent as a real. */
    __asm__ volatile("fldt %2; fxtract; fstpt %0; fstpt %1"
                 : "=m"(*sig), "=m"(*exp) : "m"(v) : "st");
    x87_get_fenv(raised);
}
#else
/* non-x86 hosts never get here: x87_host_ok() gates every escape first */
static long double x87_u_sqrt(long double v, uint16_t *r) { (void)v; (void)r; return 0; }
static long double x87_u_rndint(long double v, uint16_t *r) { (void)v; (void)r; return 0; }
static long double x87_u_scale(long double a, long double b, uint16_t *r) { (void)a; (void)b; (void)r; return 0; }
static long double x87_u_prem(long double a, long double b, int i, uint16_t *r, uint16_t *s)
{ (void)a; (void)b; (void)i; (void)r; (void)s; return 0; }
static void x87_u_xtract(long double v, long double *s, long double *e, uint16_t *r)
{ (void)v; *s = 0; *e = 0; (void)r; }
#endif

/* ---------------------------------------------------------------- x87 F3 --
 * Transcendentals (phase F3) -- host silicon again: the real fsin/fcos/
 * fsincos/fptan/fpatan/f2xm1/fyl2x/fyl2xp1 encodings run against the host
 * unit, so payload digits, NaN folding, and the odd corners (FYL2XP1 with
 * ST0 <= -1 returns ST0 for finite nonzero ST1 and ST0*ST1 for ST1 of
 * 0/inf; FYL2X with ST0=0, ST1=+-inf gives -+inf WITHOUT ZE) all come out
 * of the very silicon the ISA describes. Receipts: /tmp/transprobe{,2..7}.
 *
 * C-flag policy (transprobe6/7): all eight WRITE C1 (the round-up
 * indicator); the trig four (FSIN/FCOS/FPTAN/FSINCOS) also WRITE C2
 * (1 = range reduction incomplete, |x| >= 2^63). C0/C3 -- and C2 on the
 * other four -- are undefined on silicon and measured PRESERVED there,
 * so the engine preserves them too and never copies unmeasured host bits.
 */

enum { XT_2XM1, XT_L2X, XT_L2XP1, XT_PATAN, XT_SIN, XT_COS, XT_SINCOS, XT_TAN };

static long double x87_indefinite(void) {
    uint8_t b[10] = {0,0,0,0,0,0,0,0xC0,0xFF,0xFF};
    return x87_ld_from80(b);
}
static void x87_c1(cpu_t *c, uint16_t psw) {
    c->fsw &= (uint16_t)~FSW_C1;
    if (psw & 0x0200u) c->fsw |= FSW_C1;
}
static void x87_c12(cpu_t *c, uint16_t psw) {
    x87_c1(c, psw);
    c->fsw &= (uint16_t)~FSW_C2;
    if (psw & 0x0400u) c->fsw |= FSW_C2;
}

#if defined(__x86_64__) || defined(__i386__)
/* single-result op; x = ST0, y = ST1 (the log2/atan2 family) */
static long double x87_t_pair(int kind, long double x, long double y,
                              uint16_t *raised, uint16_t *psw) {
    long double r;
    if (x87_is_denorm(x) || x87_is_denorm(y)) *raised |= FSW_DE;
    feclearexcept(FE_ALL_EXCEPT);
    switch (kind) {
    case XT_2XM1:
        __asm__ volatile("fldt %2; .byte 0xD9,0xF0; fnstsw %1; fstpt %0"
                     : "=m"(r), "=a"(*psw) : "m"(x) : "st", "memory");
        break;
    case XT_L2X:
        __asm__ volatile("fldt %3; fldt %2; .byte 0xD9,0xF1; fnstsw %1; fstpt %0"
                     : "=m"(r), "=a"(*psw) : "m"(x), "m"(y) : "st", "memory");
        break;
    case XT_L2XP1:
        __asm__ volatile("fldt %3; fldt %2; .byte 0xD9,0xF9; fnstsw %1; fstpt %0"
                     : "=m"(r), "=a"(*psw) : "m"(x), "m"(y) : "st", "memory");
        break;
    case XT_PATAN:
        __asm__ volatile("fldt %3; fldt %2; .byte 0xD9,0xF3; fnstsw %1; fstpt %0"
                     : "=m"(r), "=a"(*psw) : "m"(x), "m"(y) : "st", "memory");
        break;
    default: /* XT_SIN / XT_COS */
        if (kind == XT_SIN)
            __asm__ volatile("fldt %2; .byte 0xD9,0xFE; fnstsw %1; fstpt %0"
                         : "=m"(r), "=a"(*psw) : "m"(x) : "st", "memory");
        else
            __asm__ volatile("fldt %2; .byte 0xD9,0xFF; fnstsw %1; fstpt %0"
                         : "=m"(r), "=a"(*psw) : "m"(x) : "st", "memory");
        break;
    }
    x87_get_fenv(raised);
    return r;
}
/* push-pair op: after the op ST0 = r0, ST1 = r1 (FPTAN: 1.0 then tan;
 * FSINCOS: cos then sin; NaN/inf inputs give the special in BOTH slots) */
static void x87_t_pushpair(int kind, long double x, long double *r0, long double *r1,
                           uint16_t *raised, uint16_t *psw) {
    if (x87_is_denorm(x)) *raised |= FSW_DE;
    feclearexcept(FE_ALL_EXCEPT);
    if (kind == XT_TAN)
        __asm__ volatile("fldt %3; .byte 0xD9,0xF2; fnstsw %1; fstpt %0; fstpt %2"
                     : "=m"(*r0), "=a"(*psw), "=m"(*r1) : "m"(x) : "st", "memory");
    else
        __asm__ volatile("fldt %3; .byte 0xD9,0xFB; fnstsw %1; fstpt %0; fstpt %2"
                     : "=m"(*r0), "=a"(*psw), "=m"(*r1) : "m"(x) : "st", "memory");
    x87_get_fenv(raised);
}
#else
static long double x87_t_pair(int kind, long double x, long double y, uint16_t *r, uint16_t *p)
{ (void)kind; (void)x; (void)y; (void)r; (void)p; return 0; }
static void x87_t_pushpair(int kind, long double x, long double *a, long double *b,
                           uint16_t *r, uint16_t *p)
{ (void)kind; (void)x; *a = 0; *b = 0; (void)r; (void)p; }
#endif

/* compare result codes */
enum { XCP_GT, XCP_LT, XCP_EQ, XCP_UNORD };

static int x87_cmp_vals(long double a, long double b, int quiet, uint16_t *raised) {
    if (x87_is_denorm(a) || x87_is_denorm(b)) *raised |= FSW_DE;
    if (isnan(a) || isnan(b)) {
        if (!quiet || x87_is_snan(a) || x87_is_snan(b)) *raised |= FSW_IE;
        return XCP_UNORD;
    }
    if (a > b) return XCP_GT;
    if (a < b) return XCP_LT;
    return XCP_EQ;
}
static void x87_com_cflags(cpu_t *c, int code) {
    switch (code) {
    case XCP_GT:    x87_set_c(c, 0, 0, 0, 0); break;
    case XCP_LT:    x87_set_c(c, 1, 0, 0, 0); break;
    case XCP_EQ:    x87_set_c(c, 0, 0, 0, 1); break;
    default:        x87_set_c(c, 1, 0, 1, 1); break;
    }
}
static void x87_com_eflags(cpu_t *c, int code) {
    c->rflags &= ~(FLAG_CF | FLAG_PF | FLAG_ZF | FLAG_OF | FLAG_SF | FLAG_AF);
    if (code == XCP_LT || code == XCP_UNORD) c->rflags |= FLAG_CF;
    if (code == XCP_EQ || code == XCP_UNORD) c->rflags |= FLAG_ZF;
    if (code == XCP_UNORD) c->rflags |= FLAG_PF;
}

/* the /r group map: D8/mem/DA/DE use it plain; DC/DE-register forms are
 * named with the reversed sub/divr pair (measured /tmp/rawdir receipts) */
static int x87_grp_kind(int grp) {
    switch (grp) {
    case 0: return XA_ADD;
    case 1: return XA_MUL;
    case 4: return XA_SUB;
    case 5: return XA_SUBR;
    case 6: return XA_DIV;
    default: return XA_DIVR;
    }
}
static int x87_grp_kind_rev(int grp) {
    switch (grp) {
    case 0: return XA_ADD;
    case 1: return XA_MUL;
    case 4: return XA_SUBR;
    case 5: return XA_SUB;
    case 6: return XA_DIVR;
    default: return XA_DIV;
    }
}

/* binary op into a destination register slot (D8: dst=ST0; DC/DE: dst=STi) */
static void x87_apply_binop(cpu_t *c, int kind, int dst_sti, long double b,
                            int pop, uint16_t *raised) {
    long double a = c->fst[x87_phys(c, dst_sti)];
    long double r = x87_binop(c, kind, a, b, raised);
    x87_w(c, x87_phys(c, dst_sti), r);
    x87_set_tag(c, x87_phys(c, dst_sti), x87_class(r));
    if (pop) x87_pop(c, raised);
}

/* ---------------------------------------------------------------------------
 * The escape dispatcher. modrm is fetched here so the reg/memory split is
 * exact (the pre-F1 D9 path decoded register forms as memory -- measured
 * FLD1 wrote AX into FCW, defect D1 of the F0 receipts).
 * ------------------------------------------------------------------------- */
static void x87_exec(machine_t *m, cpu_t *c, dctx_t *d, uint8_t op) {
    uint8_t modrm = fetch8(d);
    int mod = modrm >> 6, ext = (modrm >> 3) & 7, regi = modrm & 7;
    uint16_t raised = 0;
    int track = 1;   /* FIP/FOP bookkeeping: env/status/control ops stay out */
    rm_t rm; memset(&rm, 0, sizeof rm);

    /* CR0.EM / CR0.TS gates: every escape (and WAIT) sees these first. */
    if (c->cr0 & 0x8u) {            /* CR0.TS -> #NM(7), a delivered fault */
        raise_exception(m, 7, 0, 0);
        return;
    }
    if (c->cr0 & 0x4u) {            /* CR0.EM -> #UD */
        faultf(c, "#UD x87 (CR0.EM=1)");
        return;
    }
    if (!x87_host_ok()) {
        faultf(c, "#UD x87 needs host 80-bit long double");
        return;
    }

    int is_mem = (mod != 3);
    uint16_t fop = (uint16_t)(((op & 7) << 8) | modrm);
    uint64_t ea = 0;
    if (is_mem) {
        rm = decode_modrm(d, modrm); fixup_riprel(d, &rm);
        ea = x87_ea(d, &rm);
    }

    switch (op) {
    /* ---------------- D9: loads/stores/env/status/constants ------------- */
    case 0xD9:
        switch (is_mem ? ext + 8 : modrm) {
        case 0 + 8: { /* FLD m32 (converting: SNaN -> IE + quiet) */
            uint32_t b = (uint32_t)read_mem_v(m, ea, 4);
            feclearexcept(FE_ALL_EXCEPT);
            long double v = x87_from_m32(b);
            x87_get_fenv(&raised);
            x87_push(c, v, &raised); break; }
        case 1 + 8:   /* (reserved) */
            faultf(c, "#UD x87 D9 /1"); return;
        case 2 + 8: { /* FST m32 */
            if (!x87_check_src(c, 0, &raised)) break;
            x87_set_round(c); feclearexcept(FE_ALL_EXCEPT);
            write_mem_v(m, ea, 4, x87_to_m32(c->fst[x87_phys(c, 0)]));
            x87_get_fenv(&raised); break; }
        case 3 + 8: { /* FSTP m32 */
            if (!x87_check_src(c, 0, &raised)) { x87_pop(c, &raised); break; }
            x87_set_round(c); feclearexcept(FE_ALL_EXCEPT);
            write_mem_v(m, ea, 4, x87_to_m32(c->fst[x87_phys(c, 0)]));
            x87_get_fenv(&raised); x87_pop(c, &raised);
            break; }
        case 4 + 8:   /* FLDENV */
                track = 0;
            x87_load_env(c, ea, d->opsize != 2); break;
        case 5 + 8:   /* FLDCW */
                track = 0;
            c->fcw = (uint16_t)read_mem_v(m, ea, 2); break;
        case 6 + 8:   /* FNSTENV */
                track = 0;
            x87_save_env(c, ea, d->opsize != 2); break;
        case 7 + 8:   /* FNSTCW */
                track = 0;
            write_mem_v(m, ea, 2, c->fcw); break;
        case 0xC0: case 0xC1: case 0xC2: case 0xC3:
        case 0xC4: case 0xC5: case 0xC6: case 0xC7: { /* FLD ST(i) */
            if (!x87_check_src(c, regi, &raised)) break;
            x87_push(c, c->fst[x87_phys(c, regi)], &raised); break; }
        case 0xC8: case 0xC9: case 0xCA: case 0xCB:
        case 0xCC: case 0xCD: case 0xCE: case 0xCF: { /* FXCH ST(i) */
            int p0 = x87_phys(c, 0), pi = x87_phys(c, regi);
            long double t = c->fst[p0]; int tg = x87_get_tag(c, p0);
            x87_w(c, p0, c->fst[pi]); x87_set_tag(c, p0, x87_get_tag(c, pi));
            x87_w(c, pi, t); x87_set_tag(c, pi, tg);
            c->fsw &= (uint16_t)~FSW_C1; break; }
        case 0xD0:  /* FNOP */
            break;
        case 0xF6:  /* FINCSTP: TOP advances, tags stay, C1=0 */
            c->ftop = (c->ftop + 1) & 7;
            x87_sync_top(c); c->fsw &= (uint16_t)~FSW_C1; break;
        case 0xF7:  /* FDECSTP */
            c->ftop = (c->ftop - 1) & 7;
            x87_sync_top(c); c->fsw &= (uint16_t)~FSW_C1; break;
        case 0xE0:  /* FCHS */
            if (!x87_check_src(c, 0, &raised)) break;
            x87_w(c, x87_phys(c, 0), -c->fst[x87_phys(c, 0)]); break;
        case 0xE1:  /* FABS */
            if (!x87_check_src(c, 0, &raised)) break;
            x87_w(c, x87_phys(c, 0), fabsl(c->fst[x87_phys(c, 0)])); break;
        case 0xE4: { /* FTST: compare ST(0) with 0.0 -> C flags */
            if (!x87_check_src(c, 0, &raised)) break;
            long double v = c->fst[x87_phys(c, 0)];
            uint8_t b[10]; x87_ld_to80(v, b);
            uint16_t se = (uint16_t)(b[8] | ((uint16_t)b[9] << 8));
            uint64_t sig = 0; memcpy(&sig, b, 8);
            if (isnan(v)) {
                x87_set_c(c, 1, 0, 1, 1);
                raised |= FSW_IE;
            } else if (v == 0.0L) x87_set_c(c, 0, 0, 0, 1);
            else if (signbit(v))  x87_set_c(c, 1, 0, 0, 0);
            else                  x87_set_c(c, 0, 0, 0, 0);
            (void)se; (void)sig; break; }
        case 0xE5: { /* FXAM */
            unsigned c3, c2, c0, c1;
            int empty = x87_st_empty(c, 0);
            x87_fxam_code(c->fst[x87_phys(c, 0)], empty, &c3, &c2, &c0, &c1);
            x87_set_c(c, c0, c1, c2, c3); break; }
        case 0xE8: case 0xE9: case 0xEA: case 0xEB:
        case 0xEC: case 0xED: case 0xEE: {  /* FLD constants (host-measured bits) */
            static const uint16_t k[7][5] = {
                {0x3FFF,0x8000,0x0000,0x0000,0x0000},   /* 1  */
                {0x4000,0xD49A,0x784B,0xCD1B,0x8AFE},   /* log2(10) */
                {0x3FFF,0xB8AA,0x3B29,0x5C17,0xF0BC},   /* log2(e) */
                {0x4000,0xC90F,0xDAA2,0x2168,0xC235},   /* pi */
                {0x3FFD,0x9A20,0x9A84,0xFBCF,0xF799},   /* log10(2) */
                {0x3FFE,0xB172,0x17F7,0xD1CF,0x79AC},   /* ln(2) */
                {0x0000,0x0000,0x0000,0x0000,0x0000},   /* 0  */
            };
            const uint16_t *w = k[modrm - 0xE8];
            uint8_t b[10];
            /* rows are display-order words (se, sig-hi..sig-lo): se lands
             * in bytes 8-9, the last row in bytes 0-1 (little-endian). */
            for (int i = 0; i < 5; i++) {
                b[8 - i * 2] = (uint8_t)w[i];
                b[9 - i * 2] = (uint8_t)(w[i] >> 8);
            }
            x87_push(c, x87_ld_from80(b), &raised); break; }
        default: {
            /* F2 unaries + F3 transcendentals live here; the D1-D7/E2/E3/
             * E6/E7/EF register holes stay reserved (#UD, pinned in the
             * table suite). */
            uint16_t psw = 0;
            long double v;
            x87_set_round(c);
            switch (modrm) {
            case 0xF4: { /* FXTRACT: push significand on top, exponent below */
                if (!x87_check_src(c, 0, &raised)) break;
                long double sig, ex;
                x87_u_xtract(c->fst[x87_phys(c, 0)], &sig, &ex, &raised);
                int dst = (c->ftop - 1) & 7;
                if (x87_get_tag(c, dst) != X87_T_EMPTY) {
                    raised |= FSW_IE | FSW_SF; c->fsw |= FSW_C1;
                }
                c->ftop = dst;
                x87_w(c, dst, sig);
                x87_set_tag(c, dst, x87_class(sig));
                x87_w(c, (dst + 1) & 7, ex);
                x87_set_tag(c, (dst + 1) & 7, x87_class(ex));
                x87_sync_top(c);
                break; }
            case 0xF5: case 0xF8: { /* FPREM1 / FPREM */
                if (!x87_check_src(c, 0, &raised)) break;
                if (!x87_check_src(c, 1, &raised)) break;
                v = x87_u_prem(c->fst[x87_phys(c, 0)], c->fst[x87_phys(c, 1)],
                               modrm == 0xF5, &raised, &psw);
                x87_w(c, x87_phys(c, 0), v);
                x87_set_tag(c, x87_phys(c, 0), x87_class(v));
                /* the instruction SW carries C0=Q2 C3=Q1 C1=Q0 C2=partial */
                x87_set_c(c, (psw >> 8) & 1u, (psw >> 9) & 1u,
                          (psw >> 10) & 1u, (psw >> 14) & 1u);
                break; }
            case 0xFA: /* FSQRT */
                if (!x87_check_src(c, 0, &raised)) break;
                v = x87_u_sqrt(c->fst[x87_phys(c, 0)], &raised);
                x87_w(c, x87_phys(c, 0), v);
                x87_set_tag(c, x87_phys(c, 0), x87_class(v));
                break;
            case 0xFC: /* FRNDINT */
                if (!x87_check_src(c, 0, &raised)) break;
                v = x87_u_rndint(c->fst[x87_phys(c, 0)], &raised);
                x87_w(c, x87_phys(c, 0), v);
                x87_set_tag(c, x87_phys(c, 0), x87_class(v));
                break;
            case 0xFD: /* FSCALE */
                if (!x87_check_src(c, 0, &raised)) break;
                if (!x87_check_src(c, 1, &raised)) break;
                v = x87_u_scale(c->fst[x87_phys(c, 0)], c->fst[x87_phys(c, 1)], &raised);
                x87_w(c, x87_phys(c, 0), v);
                x87_set_tag(c, x87_phys(c, 0), x87_class(v));
                break;
            case 0xF0: { /* F2XM1: ST0 = 2^ST0 - 1 (|x|>1: source + PE quirk) */
                if (!x87_check_src(c, 0, &raised)) break;
                v = x87_t_pair(XT_2XM1, c->fst[x87_phys(c, 0)], 0.0L, &raised, &psw);
                x87_w(c, x87_phys(c, 0), v);
                x87_set_tag(c, x87_phys(c, 0), x87_class(v));
                x87_c1(c, psw);
                break; }
            case 0xF1: case 0xF9: case 0xF3: {
                /* FYL2X: ST1 = ST1*log2(ST0); FYL2XP1: ST1 = ST1*log2(1+ST0);
                 * FPATAN: ST1 = atan2(ST1, ST0) -- then pop (result at ST0). */
                if (!x87_check_src(c, 0, &raised)) break;
                if (!x87_check_src(c, 1, &raised)) break;
                int kind = (modrm == 0xF1) ? XT_L2X : (modrm == 0xF9) ? XT_L2XP1 : XT_PATAN;
                v = x87_t_pair(kind, c->fst[x87_phys(c, 0)], c->fst[x87_phys(c, 1)],
                               &raised, &psw);
                x87_w(c, x87_phys(c, 1), v);
                x87_set_tag(c, x87_phys(c, 1), x87_class(v));
                x87_pop(c, &raised);
                x87_c1(c, psw);
                break; }
            case 0xFE: case 0xFF: { /* FSIN / FCOS */
                if (!x87_check_src(c, 0, &raised)) break;
                v = c->fst[x87_phys(c, 0)];
                if (!isnan(v) && !isinf(v) && fabsl(v) >= 0x1p63L) {
                    /* host receipt: |x| >= 2^63 -> source unchanged, C2=1,
                     * C1=0, quiet (transprobe2 boundary) */
                    c->fsw &= (uint16_t)~(FSW_C1 | FSW_C2);
                    c->fsw |= FSW_C2;
                    break;
                }
                v = x87_t_pair(modrm == 0xFE ? XT_SIN : XT_COS, v, 0.0L, &raised, &psw);
                x87_w(c, x87_phys(c, 0), v);
                x87_set_tag(c, x87_phys(c, 0), x87_class(v));
                x87_c12(c, psw);
                break; }
            case 0xF2: case 0xFB: { /* FPTAN: [1.0, tan]; FSINCOS: [cos, sin] */
                if (!x87_check_src(c, 0, &raised)) break;
                v = c->fst[x87_phys(c, 0)];
                if (!isnan(v) && !isinf(v) && fabsl(v) >= 0x1p63L) {
                    /* huge: no push at all, source unchanged, C2=1, C1=0 */
                    c->fsw &= (uint16_t)~(FSW_C1 | FSW_C2);
                    c->fsw |= FSW_C2;
                    break;
                }
                long double r0, r1;
                x87_t_pushpair(modrm == 0xF2 ? XT_TAN : XT_SINCOS, v,
                               &r0, &r1, &raised, &psw);
                int dst = (c->ftop - 1) & 7;
                if (x87_get_tag(c, dst) != X87_T_EMPTY) {
                    /* stack fault (transprobe3): IE|SF|C1=1 and BOTH the
                     * old ST0 and the push slot become indefinite */
                    raised |= FSW_IE | FSW_SF;
                    c->fsw |= FSW_C1;
                    r0 = x87_indefinite();
                    r1 = x87_indefinite();
                } else {
                    x87_c12(c, psw);
                }
                c->ftop = dst;
                x87_w(c, dst, r0);
                x87_set_tag(c, dst, x87_class(r0));
                x87_w(c, (dst + 1) & 7, r1);
                x87_set_tag(c, (dst + 1) & 7, x87_class(r1));
                x87_sync_top(c);
                break; }
            default:
                faultf(c, "#UD x87 D9 reg form out of scope (%02x)", modrm); return;
            }
            break; }
        }
        break;

    /* ---------------- DB: int/m80 stores, FCMOV, control -------------- */
    case 0xDB:
        if (is_mem) {
            switch (ext) {
            case 0: { /* FILD m32 */
                int32_t v = (int32_t)read_mem_v(m, ea, 4);
                x87_push(c, (long double)v, &raised); break; }
            case 1: { /* FISTTP m32 (truncates regardless of RC) */
                if (!x87_check_src(c, 0, &raised)) { x87_pop(c, &raised); break; }
                fesetround(FE_TOWARDZERO); feclearexcept(FE_ALL_EXCEPT);
                int32_t out; x87_to_int32(c->fst[x87_phys(c, 0)], &out, &raised);
                write_mem_v(m, ea, 4, (uint32_t)out);
                x87_set_round(c); x87_pop(c, &raised); break; }
            case 2: { /* FIST m32 */
                if (!x87_check_src(c, 0, &raised)) break;
                x87_set_round(c); feclearexcept(FE_ALL_EXCEPT);
                int32_t out; x87_to_int32(c->fst[x87_phys(c, 0)], &out, &raised);
                write_mem_v(m, ea, 4, (uint32_t)out); break; }
            case 3: { /* FISTP m32 */
                if (!x87_check_src(c, 0, &raised)) { x87_pop(c, &raised); break; }
                x87_set_round(c); feclearexcept(FE_ALL_EXCEPT);
                int32_t out; x87_to_int32(c->fst[x87_phys(c, 0)], &out, &raised);
                write_mem_v(m, ea, 4, (uint32_t)out);
                x87_pop(c, &raised); break; }
            case 5: { /* FLD m80 */
                uint8_t b[10];
                for (int j = 0; j < 10; j++) b[j] = (uint8_t)read_mem_v(m, ea + (uint64_t)j, 1);
                x87_push(c, x87_ld_from80(b), &raised); break; }
            case 7: { /* FSTP m80 */
                if (!x87_check_src(c, 0, &raised)) { x87_pop(c, &raised); break; }
                uint8_t b[10]; x87_ld_to80(c->fst[x87_phys(c, 0)], b);
                for (int j = 0; j < 10; j++) write_mem_v(m, ea + (uint64_t)j, 1, b[j]);
                x87_pop(c, &raised); break; }
            default:  /* /4, /6 are reserved in memory form */
                faultf(c, "#UD x87 DB /%d", ext); return;
            }
        } else {
            switch (modrm) {
            case 0xC0: case 0xC1: case 0xC2: case 0xC3:
            case 0xC4: case 0xC5: case 0xC6: case 0xC7: /* FCMOVNB ST(0),ST(i) */
            case 0xC8: case 0xC9: case 0xCA: case 0xCB:
            case 0xCC: case 0xCD: case 0xCE: case 0xCF: /* FCMOVNE */
            case 0xD0: case 0xD1: case 0xD2: case 0xD3:
            case 0xD4: case 0xD5: case 0xD6: case 0xD7: /* FCMOVNBE */
            case 0xD8: case 0xD9: case 0xDA: case 0xDB:
            case 0xDC: case 0xDD: case 0xDE: case 0xDF: { /* FCMOVNU */
                int take = 0;
                switch ((modrm >> 3) & 3) {
                case 0: take = !(c->rflags & FLAG_CF); break;      /* NB */
                case 1: take = !(c->rflags & FLAG_ZF); break;      /* NE */
                case 2: take = !(c->rflags & FLAG_CF) && !(c->rflags & FLAG_ZF); break;
                case 3: take = !(c->rflags & FLAG_PF); break;      /* NU */
                }
                if (!x87_check_src(c, 0, &raised)) break;
                if (!x87_check_src(c, regi, &raised)) break;
                if (take) {
                    x87_w(c, x87_phys(c, 0), c->fst[x87_phys(c, regi)]);
                    x87_set_tag(c, x87_phys(c, 0), x87_get_tag(c, x87_phys(c, regi)));
                }
                break; }
            case 0xE0: case 0xE1: case 0xE4: /* FNENI/FNDISI/FSETPM: legacy no-ops */
                break;
            case 0xE2: /* FNCLEX */
                track = 0;
                c->fsw &= (uint16_t)~(FSW_PE | FSW_UE | FSW_OE | FSW_ZE | FSW_DE |
                                      FSW_IE | FSW_SF | FSW_ES | FSW_B);
                break;
            case 0xE3: /* FNINIT */
                track = 0;
                x87_reset(c); break;
            case 0xE8: case 0xE9: case 0xEA: case 0xEB:
            case 0xEC: case 0xED: case 0xEE: case 0xEF: /* FUCOMI ST(i) */
            case 0xF0: case 0xF1: case 0xF2: case 0xF3:
            case 0xF4: case 0xF5: case 0xF6: case 0xF7: /* FCOMI ST(i) */
                if (!x87_check_src(c, 0, &raised)) break;
                if (!x87_check_src(c, regi, &raised)) break;
                x87_com_eflags(c, x87_cmp_vals(c->fst[x87_phys(c, 0)],
                        c->fst[x87_phys(c, regi)], (modrm & 0xF8) == 0xE8, &raised));
                break;
            default:   /* the reserved holes */
                faultf(c, "#UD x87 DB reg form out of scope (%02x)", modrm); return;
            }
        }
        break;

    /* ---------------- DD: m64 loads/stores, save/restore, FFREE ------- */
    case 0xDD:
        if (is_mem) {
            switch (ext) {
            case 0: { /* FLD m64 (converting: SNaN -> IE + quiet) */
                uint64_t b = read_mem_v(m, ea, 8);
                feclearexcept(FE_ALL_EXCEPT);
                long double v = x87_from_m64(b);
                x87_get_fenv(&raised);
                x87_push(c, v, &raised); break; }
            case 1: { /* FISTTP m64 */
                if (!x87_check_src(c, 0, &raised)) { x87_pop(c, &raised); break; }
                fesetround(FE_TOWARDZERO); feclearexcept(FE_ALL_EXCEPT);
                int64_t out; x87_to_int64(c->fst[x87_phys(c, 0)], &out, &raised);
                write_mem_v(m, ea, 8, (uint64_t)out);
                x87_set_round(c); x87_pop(c, &raised); break; }
            case 2: { /* FST m64 */
                if (!x87_check_src(c, 0, &raised)) break;
                x87_set_round(c); feclearexcept(FE_ALL_EXCEPT);
                write_mem_v(m, ea, 8, x87_to_m64(c->fst[x87_phys(c, 0)]));
                x87_get_fenv(&raised); break; }
            case 3: { /* FSTP m64 */
                if (!x87_check_src(c, 0, &raised)) { x87_pop(c, &raised); break; }
                x87_set_round(c); feclearexcept(FE_ALL_EXCEPT);
                write_mem_v(m, ea, 8, x87_to_m64(c->fst[x87_phys(c, 0)]));
                x87_get_fenv(&raised); x87_pop(c, &raised);
                break; }
            case 4:   /* FRSTOR */
                track = 0;
                x87_load_env(c, ea, d->opsize != 2);
                x87_load_regs(c, ea + (d->opsize != 2 ? 28u : 14u));
                break;
            case 6:   /* FNSAVE */
                track = 0;
                x87_save_env(c, ea, d->opsize != 2);
                x87_save_regs(c, ea + (d->opsize != 2 ? 28u : 14u));
                x87_reset(c);
                break;
            case 7:   /* FNSTSW m16 */
                track = 0;
                write_mem_v(m, ea, 2, c->fsw); break;
            default:  /* /5 reserved */
                faultf(c, "#UD x87 DD /%d", ext); return;
            }
        } else {
            switch (modrm) {
            case 0xC0: case 0xC1: case 0xC2: case 0xC3:
            case 0xC4: case 0xC5: case 0xC6: case 0xC7: /* FFREE ST(i) */
                /* ST(i) is STACK-relative here (host-measured: FFREE ST(1)
                 * with TOP=6 frees phys7, not phys1) */
                x87_set_tag(c, x87_phys(c, regi), X87_T_EMPTY); break;
            case 0xD0: case 0xD1: case 0xD2: case 0xD3:
            case 0xD4: case 0xD5: case 0xD6: case 0xD7: /* FST ST(i) */
                if (!x87_check_src(c, 0, &raised)) break;
                x87_w(c, x87_phys(c, regi), c->fst[x87_phys(c, 0)]);
                x87_set_tag(c, x87_phys(c, regi), x87_get_tag(c, x87_phys(c, 0)));
                break;
            case 0xD8: case 0xD9: case 0xDA: case 0xDB:
            case 0xDC: case 0xDD: case 0xDE: case 0xDF: /* FSTP ST(i) */
                if (!x87_check_src(c, 0, &raised)) { x87_pop(c, &raised); break; }
                x87_w(c, x87_phys(c, regi), c->fst[x87_phys(c, 0)]);
                x87_set_tag(c, x87_phys(c, regi), x87_get_tag(c, x87_phys(c, 0)));
                x87_pop(c, &raised);
                break;
            case 0xE0: case 0xE1: case 0xE2: case 0xE3:
            case 0xE4: case 0xE5: case 0xE6: case 0xE7: /* FUCOM ST(i) */
                if (!x87_check_src(c, 0, &raised)) break;
                if (!x87_check_src(c, regi, &raised)) break;
                x87_com_cflags(c, x87_cmp_vals(c->fst[x87_phys(c, 0)],
                        c->fst[x87_phys(c, regi)], 1, &raised));
                break;
            case 0xE8: case 0xE9: case 0xEA: case 0xEB:
            case 0xEC: case 0xED: case 0xEE: case 0xEF: /* FUCOMP ST(i) */
                if (!x87_check_src(c, 0, &raised)) { x87_pop(c, &raised); break; }
                if (!x87_check_src(c, regi, &raised)) { x87_pop(c, &raised); break; }
                x87_com_cflags(c, x87_cmp_vals(c->fst[x87_phys(c, 0)],
                        c->fst[x87_phys(c, regi)], 1, &raised));
                x87_pop(c, &raised);
                break;
            default:  /* the reserved holes */
                faultf(c, "#UD x87 DD reg form out of scope (%02x)", modrm); return;
            }
        }
        break;

    /* ---------------- DF: m16/m64 ints, BCD, FNSTSW AX, FFREEP -------- */
    case 0xDF:
        if (is_mem) {
            switch (ext) {
            case 0: { /* FILD m16 */
                int16_t v = (int16_t)read_mem_v(m, ea, 2);
                x87_push(c, (long double)v, &raised); break; }
            case 1: { /* FISTTP m16 */
                if (!x87_check_src(c, 0, &raised)) { x87_pop(c, &raised); break; }
                fesetround(FE_TOWARDZERO); feclearexcept(FE_ALL_EXCEPT);
                int16_t out; x87_to_int16(c->fst[x87_phys(c, 0)], &out, &raised);
                write_mem_v(m, ea, 2, (uint16_t)out);
                x87_set_round(c); x87_pop(c, &raised); break; }
            case 2: { /* FIST m16 */
                if (!x87_check_src(c, 0, &raised)) break;
                x87_set_round(c); feclearexcept(FE_ALL_EXCEPT);
                int16_t out; x87_to_int16(c->fst[x87_phys(c, 0)], &out, &raised);
                write_mem_v(m, ea, 2, (uint16_t)out); break; }
            case 3: { /* FISTP m16 */
                if (!x87_check_src(c, 0, &raised)) { x87_pop(c, &raised); break; }
                x87_set_round(c); feclearexcept(FE_ALL_EXCEPT);
                int16_t out; x87_to_int16(c->fst[x87_phys(c, 0)], &out, &raised);
                write_mem_v(m, ea, 2, (uint16_t)out);
                x87_pop(c, &raised); break; }
            case 4: { /* FBLD m80 BCD */
                uint8_t b[10];
                for (int j = 0; j < 10; j++) b[j] = (uint8_t)read_mem_v(m, ea + (uint64_t)j, 1);
                x87_push(c, x87_from_bcd(b), &raised); break; }
            case 5: { /* FILD m64 */
                int64_t v = (int64_t)read_mem_v(m, ea, 8);
                x87_push(c, (long double)v, &raised); break; }
            case 6: { /* FBSTP m80 BCD (F3: rounds per RC, C1/PE/IE live) */
                if (!x87_check_src(c, 0, &raised)) { x87_pop(c, &raised); break; }
                uint8_t b[10]; unsigned c1 = 0;
                x87_set_round(c);
                x87_to_bcd(c->fst[x87_phys(c, 0)], b, &raised, &c1);
                for (int j = 0; j < 10; j++) write_mem_v(m, ea + (uint64_t)j, 1, b[j]);
                if (c1) c->fsw |= FSW_C1; else c->fsw &= (uint16_t)~FSW_C1;
                x87_pop(c, &raised); break; }
            case 7: { /* FISTP m64 */
                if (!x87_check_src(c, 0, &raised)) { x87_pop(c, &raised); break; }
                x87_set_round(c); feclearexcept(FE_ALL_EXCEPT);
                int64_t out; x87_to_int64(c->fst[x87_phys(c, 0)], &out, &raised);
                write_mem_v(m, ea, 8, (uint64_t)out);
                x87_pop(c, &raised); break; }
            }
        } else if (modrm == 0xE0) { /* FNSTSW AX */
            track = 0;
            set_reg(c, RAX, 2, 0, c->fsw);
        } else if ((modrm & 0xF8) == 0xC0) { /* FFREEP ST(i): free + pop */
            x87_set_tag(c, x87_phys(c, regi), X87_T_EMPTY);
            x87_pop(c, &raised);
        } else if ((modrm & 0xF8) == 0xE8 || (modrm & 0xF8) == 0xF0) {
            /* FUCOMIP / FCOMIP ST(i) */
            if (!x87_check_src(c, 0, &raised)) { x87_pop(c, &raised); break; }
            if (!x87_check_src(c, regi, &raised)) { x87_pop(c, &raised); break; }
            x87_com_eflags(c, x87_cmp_vals(c->fst[x87_phys(c, 0)],
                    c->fst[x87_phys(c, regi)], (modrm & 0xF8) == 0xE8, &raised));
            x87_pop(c, &raised);
        } else {
            faultf(c, "#UD x87 DF reg form out of scope (%02x)", modrm); return;
        }
        break;

    /* ---------------- DA: FCMOVcc up front; rest is F2 arithmetic ----- */
    case 0xDA:
        if (!is_mem && (modrm & 0xF8) >= 0xC0 && (modrm & 0xF8) <= 0xD8) {
            int take = 0;
            switch ((modrm >> 3) & 3) {
            case 0: take =  (c->rflags & FLAG_CF) != 0; break;                    /* B  */
            case 1: take =  (c->rflags & FLAG_ZF) != 0; break;                    /* E  */
            case 2: take =  (c->rflags & (FLAG_CF | FLAG_ZF)) != 0; break;        /* BE */
            case 3: take =  (c->rflags & FLAG_PF) != 0; break;                    /* U  */
            }
            if (!x87_check_src(c, 0, &raised)) break;
            if (!x87_check_src(c, regi, &raised)) break;
            if (take) {
                x87_w(c, x87_phys(c, 0), c->fst[x87_phys(c, regi)]);
                x87_set_tag(c, x87_phys(c, 0), x87_get_tag(c, x87_phys(c, regi)));
            }
            break;
        }
        if (is_mem) {
            long double src = (long double)(int32_t)(uint32_t)read_mem_v(m, ea, 4);
            switch (ext) {
            case 2: case 3:  /* FICOM/FICOMP m32int */
                if (!x87_check_src(c, 0, &raised)) break;
                x87_com_cflags(c, x87_cmp_vals(c->fst[x87_phys(c, 0)], src, 0, &raised));
                if (ext == 3) x87_pop(c, &raised);
                break;
            default:
                if (!x87_check_src(c, 0, &raised)) break;
                x87_apply_binop(c, x87_grp_kind(ext), 0, src, 0, &raised);
                break;
            }
            break;
        }
        if (modrm == 0xE9) { /* FUCOMPP */
            if (!x87_check_src(c, 0, &raised)) { x87_pop(c, &raised); x87_pop(c, &raised); break; }
            if (!x87_check_src(c, 1, &raised)) { x87_pop(c, &raised); x87_pop(c, &raised); break; }
            x87_com_cflags(c, x87_cmp_vals(c->fst[x87_phys(c, 0)],
                                          c->fst[x87_phys(c, 1)], 1, &raised));
            x87_pop(c, &raised); x87_pop(c, &raised);
            break;
        }
        faultf(c, "#UD x87 DA reg form out of scope (%02x)", modrm);
        return;

    /* ---------------- D8: m32/ST(i) -> ST0 arithmetic, FCOM ---------- */
    case 0xD8: {
        long double src = 0;
        if (is_mem) src = x87_from_m32((uint32_t)read_mem_v(m, ea, 4));
        int grp = is_mem ? ext : (modrm >> 3) & 7;
        if (grp == 2 || grp == 3) { /* FCOM/FCOMP */
            if (!x87_check_src(c, 0, &raised)) break;
            if (!is_mem && !x87_check_src(c, regi, &raised)) break;
            x87_com_cflags(c, x87_cmp_vals(c->fst[x87_phys(c, 0)],
                    is_mem ? src : c->fst[x87_phys(c, regi)], 0, &raised));
            if (grp == 3) x87_pop(c, &raised);
            break;
        }
        if (!x87_check_src(c, 0, &raised)) break;
        if (!is_mem && !x87_check_src(c, regi, &raised)) break;
        x87_apply_binop(c, x87_grp_kind(grp), 0,
                        is_mem ? src : c->fst[x87_phys(c, regi)], 0, &raised);
        break; }

    /* ---------------- DC: m64/ST(i),ST(0) arithmetic ----------------- */
    case 0xDC: {
        long double src = 0;
        if (is_mem) src = x87_from_m64(read_mem_v(m, ea, 8));
        int grp = is_mem ? ext : (modrm >> 3) & 7;
        if (!is_mem && (grp == 2 || grp == 3)) {
            faultf(c, "#UD x87 DC reg form out of scope (%02x)", modrm); return;
        }
        if (grp == 2 || grp == 3) { /* FCOM/FCOMP m64 */
            if (!x87_check_src(c, 0, &raised)) break;
            x87_com_cflags(c, x87_cmp_vals(c->fst[x87_phys(c, 0)], src, 0, &raised));
            if (grp == 3) x87_pop(c, &raised);
            break;
        }
        if (!x87_check_src(c, 0, &raised)) break;
        if (is_mem) {
            x87_apply_binop(c, x87_grp_kind(grp), 0, src, 0, &raised);
        } else {
            if (!x87_check_src(c, regi, &raised)) break;
            x87_apply_binop(c, x87_grp_kind_rev(grp), regi,
                            c->fst[x87_phys(c, 0)], 0, &raised);
        }
        break; }

    /* ---------------- DE: m16 int arithmetic, FCOMPP, +P forms -------- */
    case 0xDE: {
        if (is_mem) {
            long double src = (long double)(int16_t)(uint16_t)read_mem_v(m, ea, 2);
            int grp = ext;
            if (grp == 2 || grp == 3) { /* FICOM/FICOMP m16int */
                if (!x87_check_src(c, 0, &raised)) break;
                x87_com_cflags(c, x87_cmp_vals(c->fst[x87_phys(c, 0)], src, 0, &raised));
                if (grp == 3) x87_pop(c, &raised);
                break;
            }
            if (!x87_check_src(c, 0, &raised)) break;
            x87_apply_binop(c, x87_grp_kind(grp), 0, src, 0, &raised);
            break;
        }
        if (modrm == 0xD9) { /* FCOMPP */
            if (!x87_check_src(c, 0, &raised)) { x87_pop(c, &raised); x87_pop(c, &raised); break; }
            if (!x87_check_src(c, 1, &raised)) { x87_pop(c, &raised); x87_pop(c, &raised); break; }
            x87_com_cflags(c, x87_cmp_vals(c->fst[x87_phys(c, 0)],
                                          c->fst[x87_phys(c, 1)], 0, &raised));
            x87_pop(c, &raised); x87_pop(c, &raised);
            break;
        }
        if ((modrm & 0xF8) == 0xC0 || (modrm & 0xF8) == 0xC8 ||
            (modrm & 0xF8) == 0xE0 || (modrm & 0xF8) == 0xE8 ||
            (modrm & 0xF8) == 0xF0 || (modrm & 0xF8) == 0xF8) {
            int grp = (modrm >> 3) & 7;
            if (!x87_check_src(c, 0, &raised)) { x87_pop(c, &raised); break; }
            if (!x87_check_src(c, regi, &raised)) { x87_pop(c, &raised); break; }
            x87_apply_binop(c, x87_grp_kind_rev(grp), regi,
                            c->fst[x87_phys(c, 0)], 1, &raised);
            break;
        }
        faultf(c, "#UD x87 DE reg form out of scope (%02x)", modrm);
        return; }

    default:
        faultf(c, "#UD x87 escape out of scope");
        return;
    }

    if (track) {   /* FIP/FOP/FDP: last executed x87 op and its operand */
        c->fip = d->start_pc;
        c->fcs = c->seg[SEG_CS].sel;
        c->fop = fop;
        if (is_mem) {
            c->fdp = ea;
            c->fds = (rm.seg >= 0) ? c->seg[rm.seg].sel : c->seg[SEG_DS].sel;
        }
    }
    /* exception bookkeeping: sticky bits first, then #MF when unmasked.
     * Only the six FCW-maskable bits (PE..IE) can raise; SF/C1 ride along
     * as sticky state and must NOT gate the #MF (measured in F0 testing). */
    if (raised) {
        c->fsw |= raised;
        if ((raised & FCW_EXCM) & (uint16_t)~(c->fcw & FCW_EXCM)) {
            c->fsw |= FSW_ES;
            raise_exception(m, 16, 0, 0);   /* #MF: delivered fault */
        }
    }
}

/* ============================== main stepper ============================== */
int cpu_step(cpu_t *c) {
    machine_t *m = c->mach;
    /* A6: the per-instruction probe is a compile-time option now; in the
     * default build this folds to nothing (measured: the release binary no
     * longer contains the three guest RIP literals). */
    if (AURA_DEBUG_HOOKS && dbg_u_watch_rip(c))
        mlog(&m->log, "[dbg-u] watch rip=%llx rsp=%llx rcx=%llx gs=%llx kgs=%llx",
             (unsigned long long)c->rip, (unsigned long long)c->gpr[RSP],
             (unsigned long long)c->gpr[RCX], (unsigned long long)c->seg[SEG_GS].base,
             (unsigned long long)c->kgs_base);

    /* ---- USB U3: the EHCI async schedule advances with controller time.
     * Real EHCI walks it continuously while ASE=1; sampled here at a
     * deterministic instruction cadence so guest poll loops (which spin
     * on their own qTD array, measured) complete without MMIO kicks. */
    if ((++s_devtick & 0xFFu) == 0 && cpu_devices_tick) cpu_devices_tick(m);

    /* ---- CHIPSET H5/U5: a requested system reset (KBC 0xFE /
     * output-port bit0 / port 0x92 bit0 / port 0xCF9 hard) or CPU
     * INIT# (port 0xCF9 soft) takes effect at the next instruction
     * boundary, modeling reset-line propagation and keeping the
     * event out of the middle of an executing instruction. */
    if (m->chipset.reset_pending == RESET_KIND_CPU_INIT) {
        machine_cpu_init(m);       /* clears the request itself */
        return 0;
    }
    if (m->chipset.reset_pending) {
        machine_reset(m);          /* clears the request via chipset_init */
        return 0;
    }

    /* ---- CHIPSET H0: hardware INTR sampling at the instruction boundary ----
     * A pending unmasked PIC line wakes HLT even with IF=0 (matching
     * silicon); the actual vector delivery still requires IF=1 and the
     * post-STI / post-MOV-SS one-instruction shadow to have expired. */
    if (c->halted) {
        /* K6: the 8259 INTR line is a board signal wired to the BSP only;
         * an AP wakes exclusively on its own LAPIC.  n_vcpus==1 keeps
         * cur_vcpu at 0 forever, so this gate is identity in UP. */
        if ((m->cur_vcpu == 0 && pic_pending(m)) ||
            lapic_deliverable(m) >= 0) c->halted = 0;
        else if (c->rflags & FLAG_IF) {
            /* K8: an hlt-ed vcpu whose peer(s) have runnable work must not
             * drag the shared clock: on real hardware a halted sibling
             * consumes ZERO issue slots and the running cpu retires its
             * instructions at full rate.  The K3 +512 quantum stays ONLY
             * for the all-halted case (UP included), where it is the sole
             * way virtual time flows at all.  Measured failure this fixes:
             * with --smp=2, one vcpu's busy 5.8M-instruction fb scroll
             * stretched to 6.5 s of PIT wall time while the other sat in
             * hlt (each busy instruction paid for the sibling's 512-vtime
             * quantum), inflating 2-CPU boot-to-shell to ~1031 virtual
             * seconds; with the yield below it returns to scroll-cost only.
             * Charging nothing here is safe because the wait's wake sources
             * (PIT/LAPIC/RTC) all tick off the GLOBAL vtime that the busy
             * peer keeps advancing; this vcpu's LAPIC timer catches up on
             * its next slot by construction (per-context delta accumulator). */
            int peer_busy = 0;
            for (int i = 0; i < m->n_vcpus && i < EM_MAX_VCPU; i++) {
                if (i == m->cur_vcpu) continue;
                if (m->vcpu[i].state != VCPU_RUN) continue;   /* parked: no clock drag either way */
                if (!m->vcpu[i].c.halted) { peer_busy = 1; break; }
            }
            if (!peer_busy) {
                        /* K3: virtual time must not freeze while the CPU idles in HLT
             * -- PIT/LAPIC/RTC all derive from instr_count (K6: vtime). Charge
             * a time quantum (it counts toward --max-instr like executed work)
             * and wake on whatever arrives.  */
                c->instr_count += 512;
                m->vtime_instr += 512;   /* K6: master clock advances too */
            }
            /* K8: the timer SETTLE calls are time bookkeeping, not wall-clock
             * advancement -- they must run on every idle slot, peer-busy
             * ones included.  pit/rtc are global and idempotent at delta 0;
             * lapic_tick computes THIS context's timer from the global vtime
             * via its per-context delta accumulator, so the halted vcpu's
             * LAPIC watchdog keeps ticking at true wall rate while a peer
             * works (measured red: without this, /tests/smpstress deadlocked
             * -- the AP's TMCCUR froze mid-tick, its scheduler tick never
             * fired, and a fork child sat unpicked on the AP runqueue while
             * the parent waited forever). */
            pit_tick(m);
            rtc_tick(m);
            lapic_tick(m);
            if ((m->cur_vcpu == 0 && pic_pending(m)) ||
                lapic_deliverable(m) >= 0) c->halted = 0;
            return 0;   /* woke or still waiting; the run stays alive */
        } else return -1;  /* hlt+cli: only NMI/SMI/RESET could wake -> real halt */
    }
    if (c->fault) return -1;
    if (c->intr_delay) c->intr_delay--;
    else if (c->rflags & FLAG_IF) {
        /* CHIPSET H6: the LAPIC arbitrates first; with the LAPIC soft-
         * disabled (or nothing deliverable) the 8259 INTR path is
         * byte-for-byte the H0/H1 design (plan D7). The CPU only issues
         * INTA while INTR is asserted, so the spurious vector
         * pic_intack() returns on an empty PIC (H1) is never delivered
         * from the step loop -- the silicon race it models (line
         * deasserted between INTR and INTA) is below our instruction-
         * boundary time resolution. */
        int vec = lapic_deliverable(m);
        if (vec >= 0) {
            lapic_intack(m);
            raise_exception(m, vec, 0, 0);
        } else if (m->cur_vcpu == 0 && pic_pending(m)) {
            /* K6: 8259 INTR -> BSP only; LAPIC-first arbitration otherwise
             * byte-for-byte the H0/H1/H6 design. */
            int pvec = pic_intack(m);
            if (pvec >= 0) raise_exception(m, pvec, 0, 0);
        }
    }

    /* TOOLING T1/T2: the instruction boundary as tools see it -- any
     * hardware interrupt has been delivered, nothing of the next
     * instruction has been fetched. One pointer test when no tool is
     * attached (m->dbg == NULL); the call itself lives out of line so it
     * does not cost the untooled path register spills (measured). */
    if (__builtin_expect(m->dbg != NULL, 0)) {
        if (dbg_boundary_cold(m)) return 0;
    }

    c->exception_taken = 0;
    dctx_t d; memset(&d,0,sizeof d);
    d.m = m; d.c = c; d.pc = c->rip; d.seg_override = -1;
    d.start_pc = c->rip;
    int default_as = cpu_addr_size(c);
    d.addrsize = default_as;
    d.opsize = cpu_op_size_default(c);

    uint8_t op;
    int predecoded = perf_decode_hit(m, &d, &op);
    if (!predecoded) for (;;) {
        op = fetch8(&d);
        if (op == 0x66) { d.opsize = (default_as==16) ? 4 : 2; continue; } /* toggles byte-count between 2 and 4 */
        if (op == 0x67) { d.addrsize = (default_as==32)?16:(default_as==16?32:default_as); continue; }
        if (op == 0xF0) continue;
        if (op == 0xF2) { d.rep = 0xF2; continue; }
        if (op == 0xF3) { d.rep = 0xF3; continue; }
        if (op == 0x2E) { d.seg_override = SEG_CS; continue; }
        if (op == 0x36) { d.seg_override = SEG_SS; continue; }
        if (op == 0x3E) { d.seg_override = SEG_DS; continue; }
        if (op == 0x26) { d.seg_override = SEG_ES; continue; }
        if (op == 0x64) { d.seg_override = SEG_FS; continue; }
        if (op == 0x65) { d.seg_override = SEG_GS; continue; }
        if (default_as == 64 && op >= 0x40 && op <= 0x4F) {
            d.has_rex = 1; d.rex_w=(op>>3)&1; d.rex_r=(op>>2)&1; d.rex_x=(op>>1)&1; d.rex_b=op&1;
            continue;
        }
        break;
    }
    /* K4: a fault raised while FETCHING (not-present/NX/SMEP via the new
     * execute-mode translate) aborts the instruction right here -- without
     * this early-exit the decoder would consume fault-zero bytes and
     * double-fault the machine (measured while building the NX row). The
     * same guard covers the nested 0F opcode fetch below. */
    if (c->fault || c->exception_taken) goto done;
    if (d.has_rex && d.rex_w) d.opsize = 8; /* REX.W -> 64-bit operand size = 8 bytes */
    if (!predecoded) perf_decode_store(m, &d, op);
    int osz = d.opsize;

    if (c->trace) {
        mlog(&m->log, "[trace] %s rip=%08llx op=%02x", cpu_mode_name(c), (unsigned long long)d.start_pc, op);
    }


    #define MODRM() decode_modrm(&d, fetch8(&d))
    #define FIXUP(rm) fixup_riprel(&d, &(rm))

    switch (op) {
    case 0x90: case 0x91: case 0x92: case 0x93: case 0x94: case 0x95: case 0x96: case 0x97: {
        /* XCHG rAX,r -- 0x90 is the degenerate rax<->rax form (= NOP);
         * F3 90 is PAUSE, also a NOP on an in-order single-threaded core. */
        if (op == 0x90) break;
        int r = (op-0x90) | (d.has_rex ? (d.rex_b<<3) : 0);
        uint64_t a = get_reg(c,RAX,osz,d.has_rex), b = get_reg(c,r,osz,d.has_rex);
        set_reg(c,RAX,osz,d.has_rex,b);
        set_reg(c,r,osz,d.has_rex,a);
        break; }
    case 0xF4: c->halted = 1; break; /* HLT */
    /* CMC/CLC/STC were missing entirely (caught by the C10 table suite). */
    case 0xF5: c->rflags ^= FLAG_CF; break;  /* CMC */
    case 0xF8: c->rflags &= ~FLAG_CF; break; /* CLC */
    case 0xF9: c->rflags |= FLAG_CF; break;  /* STC */
    case 0xFA: c->rflags &= ~FLAG_IF; break; /* CLI */
    case 0xFB: c->rflags |= FLAG_IF; c->intr_delay = 1; break;  /* STI (INTR held off one more instruction) */
    case 0xFC: c->rflags &= ~FLAG_DF; break; /* CLD */
    case 0xFD: c->rflags |= FLAG_DF; break;  /* STD */

    /* ---- ALU r/m,r and r,r/m forms: 00-3D range for ADD/OR/ADC/SBB/AND/SUB/XOR/CMP ---- */
    case 0x00: case 0x08: case 0x10: case 0x18: case 0x20: case 0x28: case 0x30: case 0x38: { /* r/m8,r8 */
        int aluop = op>>3; rm_t rm = MODRM(); FIXUP(rm);
        uint64_t a=rm_read(&d,&rm,1), b=get_reg(c,rm.reg_field,1,d.has_rex); int wb;
        uint64_t r=alu_op(c,aluop,a,b,1,&wb); if (wb) rm_write(&d,&rm,1,r);
        break; }
    case 0x01: case 0x09: case 0x11: case 0x19: case 0x21: case 0x29: case 0x31: case 0x39: { /* r/m,r */
        int aluop = op>>3; rm_t rm = MODRM(); FIXUP(rm);
        uint64_t a=rm_read(&d,&rm,osz), b=get_reg(c,rm.reg_field,osz,d.has_rex); int wb;
        uint64_t r=alu_op(c,aluop,a,b,osz,&wb); if (wb) rm_write(&d,&rm,osz,r);
        break; }
    case 0x02: case 0x0A: case 0x12: case 0x1A: case 0x22: case 0x2A: case 0x32: case 0x3A: { /* r8,r/m8 */
        int aluop = op>>3; rm_t rm = MODRM(); FIXUP(rm);
        uint64_t a=get_reg(c,rm.reg_field,1,d.has_rex), b=rm_read(&d,&rm,1); int wb;
        uint64_t r=alu_op(c,aluop,a,b,1,&wb); if (wb) set_reg(c,rm.reg_field,1,d.has_rex,r);
        break; }
    case 0x03: case 0x0B: case 0x13: case 0x1B: case 0x23: case 0x2B: case 0x33: case 0x3B: { /* r,r/m */
        int aluop = op>>3; rm_t rm = MODRM(); FIXUP(rm);
        uint64_t a=get_reg(c,rm.reg_field,osz,d.has_rex), b=rm_read(&d,&rm,osz); int wb;
        uint64_t r=alu_op(c,aluop,a,b,osz,&wb); if (wb) set_reg(c,rm.reg_field,osz,d.has_rex,r);
        break; }
    case 0x04: case 0x0C: case 0x14: case 0x1C: case 0x24: case 0x2C: case 0x34: case 0x3C: { /* AL,imm8 */
        int aluop = op>>3; uint64_t imm = fetch8(&d); int wb;
        uint64_t r=alu_op(c,aluop,get_reg(c,RAX,1,d.has_rex),imm,1,&wb); if (wb) set_reg(c,RAX,1,d.has_rex,r);
        break; }
    case 0x05: case 0x0D: case 0x15: case 0x1D: case 0x25: case 0x2D: case 0x35: case 0x3D: { /* eAX,imm */
        int aluop = op>>3; int isz = osz==2?2:4; uint64_t imm=(uint64_t)(int64_t)fetch_imm_signed_z(&d,isz); int wb;
        uint64_t r=alu_op(c,aluop,get_reg(c,RAX,osz,d.has_rex),imm,osz,&wb); if (wb) set_reg(c,RAX,osz,d.has_rex,r);
        break; }

    default: goto slow_decode;
    }
    goto done;

slow_decode:
    /* second pass for opcodes needing their modrm "reg" field as an opcode
     * extension (group1/2/3/5), 0F-prefixed map, and control transfer /
     * misc instructions. Implemented as its own switch for clarity. */
    switch (op) {
    case 0x80: case 0x81: case 0x83: { /* group1 imm */
        /* must re-fetch: we already consumed the opcode in the first switch's
         * MODRM() call for 0x80, which fetched but discarded -- to keep this
         * simple & correct we instead decode group1 fully here. */
        goto group1;
    }
    default: break;
    }
    goto really_slow;

group1: {
        /* The original group1 path re-fetches the opcode from start_pc.
         * For a P3 hit only the proven RAM prefix/opcode bytes were already
         * decoded, so preserve the saved cursor; live ModRM/SIB/displacement
         * and immediate fetches below are NEVER reused.  On the slow path,
         * including ROM/MMIO/debug, keep the old re-fetch exactly intact. */
        if (!predecoded) {
            d.pc = d.start_pc;
            for (;;) {
                uint8_t b = fetch8(&d);
                if (b==0x66||b==0x67||b==0xF0||b==0xF2||b==0xF3||b==0x2E||b==0x36||b==0x3E||b==0x26||b==0x64||b==0x65) continue;
                if (default_as==64 && b>=0x40 && b<=0x4F) continue;
                op = b; break;
            }
        }
        int size = (op==0x80) ? 1 : osz;
        rm_t rm = MODRM();
        FIXUP(rm);
        int aluop = (rm.reg_field>>0)&7; /* reg field (already masked to 0-7 by decode, ignoring rex.r which group1 doesn't use for the opcode-extension) */
        aluop &= 7;
        uint64_t imm;
        if (op==0x81) imm = (uint64_t)(int64_t)fetch_imm_signed_z(&d, size==2?2:4);
        else imm = (uint64_t)(int64_t)fetch8s(&d); /* 0x80 and 0x83 use imm8 (sign-extended for 0x83) */
        uint64_t a = rm_read(&d,&rm,size); int wb;
        uint64_t r = alu_op(c, aluop, a, imm, size, &wb);
        if (wb) rm_write(&d,&rm,size,r);
        goto done;
    }

really_slow:
    switch (op) {
    /* ---- TEST ---- */
    case 0x84: { rm_t rm=MODRM(); FIXUP(rm); uint64_t a=rm_read(&d,&rm,1), b=get_reg(c,rm.reg_field,1,d.has_rex); set_flags_logic(c,a&b,1); break; }
    case 0x85: { rm_t rm=MODRM(); FIXUP(rm); uint64_t a=rm_read(&d,&rm,osz), b=get_reg(c,rm.reg_field,osz,d.has_rex); set_flags_logic(c,a&b,osz); break; }
    case 0xA8: { uint64_t imm=fetch8(&d); set_flags_logic(c, get_reg(c,RAX,1,d.has_rex)&imm, 1); break; }
    case 0xA9: { uint64_t imm=(uint64_t)(int64_t)fetch_imm_signed_z(&d, osz==2?2:4); set_flags_logic(c, get_reg(c,RAX,osz,d.has_rex)&imm, osz); break; }

    /* ---- MOV AL/eAX,moffs and moffs,AL/eAX (absolute addr, no ModRM) ---- */
    case 0xA0: case 0xA1: case 0xA2: case 0xA3: {
        uint64_t off = (d.addrsize==16) ? fetch16(&d) : (d.addrsize==32 ? fetch32(&d) : fetch64(&d));
        int sz = (op==0xA0 || op==0xA2) ? 1 : osz;
        uint64_t base = (d.seg_override>=0) ? c->seg[d.seg_override].base : c->seg[SEG_DS].base;
        if (op==0xA0 || op==0xA1) set_reg(c, RAX, sz, d.has_rex, read_mem_v(m, base+off, sz));
        else write_mem_v(m, base+off, sz, get_reg(c, RAX, sz, d.has_rex));
        break; }

    /* ---- MOV ---- */
    case 0x88: { rm_t rm=MODRM(); FIXUP(rm); rm_write(&d,&rm,1,get_reg(c,rm.reg_field,1,d.has_rex)); break; }
    case 0x89: { rm_t rm=MODRM(); FIXUP(rm); rm_write(&d,&rm,osz,get_reg(c,rm.reg_field,osz,d.has_rex)); break; }
    case 0x8A: { rm_t rm=MODRM(); FIXUP(rm); set_reg(c,rm.reg_field,1,d.has_rex, rm_read(&d,&rm,1)); break; }
    case 0x8B: { rm_t rm=MODRM(); FIXUP(rm); set_reg(c,rm.reg_field,osz,d.has_rex, rm_read(&d,&rm,osz)); break; }
    case 0x8F: { /* POP r/m: only group /0 is defined */
        rm_t rm = MODRM(); FIXUP(rm);
        int ext = rm.reg_field & 7;
        if (ext != 0) { faultf(c, "#UD POP r/m group ext=%d", ext); break; }
        int sz = stack_unit(&d);
        uint64_t value = do_pop(&d, sz);
        if (rm.is_mem && rm.rsp_base) {
            /* POP computes an ESP/RSP-based destination after the pop. The
             * ModRM decoder cached the old base, so advance that EA using
             * the active address width before storing the value. */
            rm.addr += (uint64_t)sz;
            if (d.addrsize == 32) rm.addr &= 0xFFFFFFFFull;
        }
        rm_write(&d, &rm, sz, value);
        break; }
    /* ---- XCHG r/m,r (C8) ---- */
    case 0x86: case 0x87: {
        int size = (op==0x86)?1:osz; rm_t rm=MODRM(); FIXUP(rm);
        uint64_t a = rm_read(&d,&rm,size), b = get_reg(c,rm.reg_field,size,d.has_rex);
        rm_write(&d,&rm,size,b);
        set_reg(c,rm.reg_field,size,d.has_rex,a);
        break; }
    /* ---- CBW/CWDE/CDQE (0x98), CWD/CDQ/CQO (0x99) ---- */
    case 0x98: {
        if (osz==2)      set_reg(c,RAX,2,d.has_rex, (uint64_t)(int64_t)(int16_t)(int8_t)c->gpr[RAX]);
        else if (osz==4) c->gpr[RAX] = (uint32_t)(int32_t)(int16_t)c->gpr[RAX];
        else             c->gpr[RAX] = (uint64_t)(int64_t)(int32_t)c->gpr[RAX];
        break; }
    case 0x99: {
        if (osz==2)      set_reg(c,RDX,2,d.has_rex, (c->gpr[RAX]&0x8000)?0xFFFF:0);
        else if (osz==4) c->gpr[RDX] = (c->gpr[RAX]&0x80000000)?0xFFFFFFFFull:0;
        else             c->gpr[RDX] = (c->gpr[RAX]>>63)?~0ull:0;
        break; }
    case 0x8D: { rm_t rm=MODRM(); FIXUP(rm); /* LEA: address itself (no segment base) is the value */
        set_reg(c, rm.reg_field, osz, d.has_rex, rm_ea(&d, &rm)); break; }
    case 0x8E: { rm_t rm=MODRM(); FIXUP(rm); int segidx = rm.reg_field & 7;
        if (segidx == SEG_SS) c->intr_delay = 1;
        uint16_t sel = (uint16_t)rm_read(&d,&rm,2);
        if (!(c->cr0&1)) { c->seg[segidx].sel = sel; c->seg[segidx].base = (uint64_t)sel<<4; c->seg[segidx].limit=0xFFFF; }
        else { desc_t dsc; read_descriptor(m, c->gdtr_base, sel, &dsc); load_seg_from_desc(c, segidx, sel, &dsc); if (segidx==SEG_CS) {} }
        break; }
    /* ---- MOV r/m16, Sreg (0x8C): the missing half of segment moves ---- */
    case 0x8C: { rm_t rm=MODRM(); FIXUP(rm); int segidx = rm.reg_field & 7;
        if (segidx > 5) { faultf(c, "#UD mov Sreg index %d", segidx); break; }
        uint64_t sv = c->seg[segidx].sel;
        if (!rm.is_mem && default_as==64) set_reg(c, rm.reg, 8, 1, sv); /* reg form zero-extends to 64 */
        else if (!rm.is_mem) set_reg(c, rm.reg, 2, d.has_rex, sv);
        else rm_write(&d,&rm,2,sv);
        break; }
    case 0xC6: { rm_t rm=MODRM(); FIXUP(rm); uint8_t imm=fetch8(&d); rm_write(&d,&rm,1,imm); break; }
    case 0xC7: { rm_t rm=MODRM(); FIXUP(rm); int isz= osz==2?2:4; uint64_t imm=fetch_imm(&d,isz);
        if (osz==8) imm = (uint64_t)(int64_t)(int32_t)imm; /* sign-extend to 64 */
        rm_write(&d,&rm,osz,imm); break; }
    case 0xB0: case 0xB1: case 0xB2: case 0xB3: case 0xB4: case 0xB5: case 0xB6: case 0xB7: {
        int r = (op-0xB0) | (d.has_rex? (d.rex_b<<3):0); uint8_t imm=fetch8(&d); set_reg(c,r,1,d.has_rex,imm); break; }
    case 0xB8: case 0xB9: case 0xBA: case 0xBB: case 0xBC: case 0xBD: case 0xBE: case 0xBF: {
        int r = (op-0xB8) | (d.has_rex? (d.rex_b<<3):0);
        if (osz==8) { uint64_t imm=fetch64(&d); set_reg(c,r,8,d.has_rex,imm); }
        else { uint64_t imm=fetch_imm(&d, osz==2?2:4); set_reg(c,r,osz,d.has_rex,imm); }
        break; }

    /* ---- MOVZX ---- */
    case 0x0F: {
        uint8_t op2 = fetch8(&d);
        if (c->fault || c->exception_taken) goto done;   /* K4 fetch abort */
        switch (op2) {
        case 0x05: { /* SYSCALL (K4): ring-3 -> kernel fast call. RCX/R11 take
                      * the return RIP/RFLAGS, CS:EIP load from STAR/LSTAR,
                      * SFMASK masks RFLAGS. Requires EFER.SCE. */
            if (!(c->efer & 1ull) || default_as != 64) {
                faultf(c, "#UD SYSCALL (EFER.SCE=%d, mode-%d)",
                       (int)(c->efer & 1), default_as);
                break;
            }
#if AURA_DEBUG_HOOKS
            if (dbg_u_on > 0)
                mlog(&m->log, "[dbg-u] SYSCALL entry rip=%llx rsp=%llx [rsp]=%llx [rsp+8]=%llx rcx=%llx r11=%llx rax=%llx rdi=%llx",
                     (unsigned long long)c->rip, (unsigned long long)c->gpr[RSP],
                     (unsigned long long)read_mem_v(m, c->gpr[RSP], 8),
                     (unsigned long long)read_mem_v(m, c->gpr[RSP] + 8, 8),
                     (unsigned long long)c->gpr[RCX], (unsigned long long)c->gpr[R11],
                     (unsigned long long)c->gpr[RAX], (unsigned long long)c->gpr[RDI]);
            /* K4: first-N sys_read/sys_write probe for the shell prompt-spin
             * chase -- entry args at SYSCALL, result at SYSRET (tagged). */
            /* only read(0,...) -- the shell stdin lane; writes are noise here */
            if (dbg_u_on > 0 && c->gpr[RAX] == 0 && c->gpr[RDI] == 0 &&
                dbg_read_probe < 30) {
                syscall_ret_probe = dbg_read_probe++;
                /* unbuffered FILE, not the ring or a pipe: a storm wraps the
                 * ring and pipe buffers never flush before a kill (both
                 * measured).  A6: the sink is operator-named and closable. */
                FILE *pf = dbg_u_probe_file();
                if (pf)
                    fprintf(pf, "sysread0#%d buf=%llx n=%lld rip=%llx cr3=%llx\n",
                            syscall_ret_probe, (unsigned long long)c->gpr[RSI],
                            (long long)c->gpr[RDX], (unsigned long long)c->rip,
                            (unsigned long long)c->cr3);
            }
#endif
            int f1, f2, f3;
            uint64_t star   = cpu_get_msr(c, 0xC0000081, &f1);
            uint64_t lstar  = cpu_get_msr(c, 0xC0000082, &f2);
            uint64_t sfmask = cpu_get_msr(c, 0xC0000084, &f3);
            if (!f1 || !f2) { faultf(c, "#UD SYSCALL without STAR/LSTAR MSRs"); break; }
            c->gpr[RCX] = d.pc;
            c->gpr[R11] = c->rflags;
            c->rflags = (c->rflags & ~sfmask) | 0x2;
            uint16_t kcs = (uint16_t)((star >> 32) & 0xFFFC);
            c->seg[SEG_SS].sel = (uint16_t)(kcs + 8);
            c->seg[SEG_SS].base = 0;
#if AURA_DEBUG_HOOKS
            if (dbg_u_on > 0)
                mlog(&m->log, "[dbg-u] SYSCALL pre-farload rsp=%llx kcs=%x lstar=%llx",
                     (unsigned long long)c->gpr[RSP], kcs, (unsigned long long)lstar);
#endif
            far_load_cs(m, kcs, lstar);
#if AURA_DEBUG_HOOKS
            if (dbg_u_on > 0)
                mlog(&m->log, "[dbg-u] SYSCALL post-farload rsp=%llx rip=%llx",
                     (unsigned long long)c->gpr[RSP], (unsigned long long)c->rip);
#endif
            d.pc = c->rip;
            break; }
        case 0x07: { /* SYSRET (K4): CS=STAR[63:48]+16|3, SS=+8|3,
                      * RIP=RCX, RFLAGS=R11. Ring-0 only. */
            if (cpl_now(c) != 0 || default_as != 64) {
                faultf(c, "#GP SYSRET from CPL%d", cpl_now(&m->cpu));
                break;
            }
            int f1, f3;
            uint64_t star = cpu_get_msr(c, 0xC0000081, &f1);
            (void)f3;
            uint16_t ubase = (uint16_t)(star >> 48);
#if AURA_DEBUG_HOOKS
            if (dbg_u_on > 0 && syscall_ret_probe >= 0) {
                FILE *pf = dbg_u_probe_file();
                if (pf) {
                    fprintf(pf, "sysret0#%d rax=%lld rip=%llx bytes=",
                            syscall_ret_probe, (long long)c->gpr[RAX],
                            (unsigned long long)c->gpr[RCX]);
                    for (int i = 0; i < 16 && (long long)c->gpr[RAX] > 0; i++)
                        fprintf(pf, "%02llx", (unsigned long long)read_mem_v(m, c->gpr[RSI] + i, 1));
                    fputc('\n', pf);
                }
                syscall_ret_probe = -1;
            }
            if (dbg_u_on)
                mlog(&m->log, "[dbg-u] SYSRET to rip=%llx rflags=%llx rsp=%llx [rsp]=%llx",
                     (unsigned long long)c->gpr[RCX], (unsigned long long)c->gpr[R11],
                     (unsigned long long)c->gpr[RSP],
                     (unsigned long long)read_mem_v(m, c->gpr[RSP], 8));
#endif
            c->seg[SEG_SS].sel = (uint16_t)((ubase + 8) | 3);
            c->seg[SEG_SS].base = 0;
            c->rflags = (c->gpr[R11] & RFLAGS_WRITABLE) | 0x2;
            far_load_cs(m, (uint16_t)((ubase + 16) | 3), c->gpr[RCX]);
            d.pc = c->rip;
            break; }
        /* ---- K4 SSE data lanes: the measured ring-3 userspace runs its
         * memset/memcpy and compiler prologues through these. MOVUPS ==
         * MOVUPD; MOVAPS/MOVDQA fault #GP(0) on a misaligned memory
         * operand. Scalar moves merge/zero exactly per the manual.
         * SIMD arithmetic is still out of scope on purpose. ---- */
        case 0x10: case 0x11: case 0x28: case 0x29: {
            rm_t rm = MODRM(); FIXUP(rm);
            int load = (op2 == 0x10 || op2 == 0x28);
            int scalar = (d.rep == 0xF2 || d.rep == 0xF3) && (op2 == 0x10 || op2 == 0x11);
            int scalar64 = scalar && d.rep == 0xF2;
            int aligned_req = (op2 == 0x28 || op2 == 0x29);
            if (load) {
                if (rm.is_mem) {
                    uint64_t ea = (rm.seg>=0 ? c->seg[rm.seg].base : 0) + rm_ea(&d,&rm);
                    if (aligned_req && (ea & 15)) { c->rip = d.start_pc; raise_exception(m, 13, 1, 0); break; }
                    uint64_t lo = read_mem_v(m, ea, 8);
                    c->xmm[rm.reg_field][0] = scalar && !scalar64 ? (lo & 0xFFFFFFFFull) : lo;
                    c->xmm[rm.reg_field][1] = scalar ? 0 : read_mem_v(m, ea + 8, 8);
                } else {
                    uint64_t slo = c->xmm[rm.reg][0], shi = c->xmm[rm.reg][1];
                    if (!scalar) { c->xmm[rm.reg_field][0] = slo; c->xmm[rm.reg_field][1] = shi; }
                    else if (scalar64) c->xmm[rm.reg_field][0] = slo;                /* high preserved */
                    else { c->xmm[rm.reg_field][0] = (c->xmm[rm.reg_field][0] & ~0xFFFFFFFFull) | (slo & 0xFFFFFFFFull); }
                }
            } else {
                uint64_t lo = c->xmm[rm.reg_field][0], hi = c->xmm[rm.reg_field][1];
                if (rm.is_mem) {
                    uint64_t ea = (rm.seg>=0 ? c->seg[rm.seg].base : 0) + rm_ea(&d,&rm);
                    if (aligned_req && (ea & 15)) { c->rip = d.start_pc; raise_exception(m, 13, 1, 0); break; }
                    if (scalar) {
                        if (scalar64) write_mem_v(m, ea, 8, lo);
                        else          write_mem_v(m, ea, 4, lo & 0xFFFFFFFFull);
                    } else {
                        write_mem_v(m, ea, 8, lo);
                        write_mem_v(m, ea + 8, 8, hi);
                    }
                } else {
                    if (!scalar) { c->xmm[rm.reg][0] = lo; c->xmm[rm.reg][1] = hi; }
                    else if (scalar64) c->xmm[rm.reg][0] = lo;
                    else c->xmm[rm.reg][0] = (c->xmm[rm.reg][0] & ~0xFFFFFFFFull) | (lo & 0xFFFFFFFFull);
                }
            }
            break; }
        case 0x12: case 0x13: case 0x16: case 0x17: { /* MOVL/HPS (+MOVHLPS) */
            rm_t rm = MODRM(); FIXUP(rm);
            int is_h = (op2 == 0x16 || op2 == 0x17), is_st = (op2 == 0x13 || op2 == 0x17);
            if (!rm.is_mem && (op2 == 0x12)) {          /* MOVHLPS: lo = src.hi */
                c->xmm[rm.reg_field][0] = c->xmm[rm.reg][1];
            } else if (!rm.is_mem && op2 == 0x16) {     /* MOVLHPS: hi = src.lo */
                c->xmm[rm.reg_field][1] = c->xmm[rm.reg][0];
            } else if (rm.is_mem) {
                uint64_t ea = (rm.seg>=0 ? c->seg[rm.seg].base : 0) + rm_ea(&d,&rm);
                if (!is_st) c->xmm[rm.reg_field][is_h ? 1 : 0] = read_mem_v(m, ea, 8);
                else        write_mem_v(m, ea, 8, c->xmm[rm.reg_field][is_h ? 1 : 0]);
            } else {
                faultf(c, "#UD scalar/prefixed 0F %02x reg-form out of K4 scope", op2);
            }
            break; }
        case 0x6E: { /* MOVD/MOVQ r/m -> xmm (66 prefix; REX.W = 64-bit) */
            if (d.opsize != 2) { faultf(c, "#UD 0F 6E without 66 prefix (MMX out of scope)"); break; }
            rm_t rm = MODRM(); FIXUP(rm);
            uint64_t v = rm_read(&d,&rm, d.rex_w ? 8 : 4);
            c->xmm[rm.reg_field][0] = v;
            c->xmm[rm.reg_field][1] = 0;
            break; }
        case 0x6F: case 0x7F: { /* MOVDQA (66) / MOVDQU (F3) 128b */
            if (d.opsize != 2 && d.rep != 0xF3) { faultf(c, "#UD 0F %02x without 66/F3 prefix (MMX out of scope)", op2); break; }
            rm_t rm = MODRM(); FIXUP(rm);
            int load = (op2 == 0x6F);
            if (rm.is_mem) {
                uint64_t ea = (rm.seg>=0 ? c->seg[rm.seg].base : 0) + rm_ea(&d,&rm);
                if (d.opsize == 2 && (ea & 15)) { c->rip = d.start_pc; raise_exception(m, 13, 1, 0); break; }
                if (load) {
                    c->xmm[rm.reg_field][0] = read_mem_v(m, ea, 8);
                    c->xmm[rm.reg_field][1] = read_mem_v(m, ea + 8, 8);
                } else {
                    write_mem_v(m, ea, 8, c->xmm[rm.reg_field][0]);
                    write_mem_v(m, ea + 8, 8, c->xmm[rm.reg_field][1]);
                }
            } else {
                int src = load ? rm.reg : rm.reg_field;
                int dst = load ? rm.reg_field : rm.reg;
                c->xmm[dst][0] = c->xmm[src][0];
                c->xmm[dst][1] = c->xmm[src][1];
            }
            break; }
        case 0x7E: { /* MOVD/MOVQ xmm -> r/m (66) | MOVQ xmm/m64 -> xmm (F3) */
            if (d.rep == 0xF3) {
                rm_t rm = MODRM(); FIXUP(rm);
                uint64_t lo = rm.is_mem
                    ? read_mem_v(m, (rm.seg>=0 ? c->seg[rm.seg].base : 0) + rm_ea(&d,&rm), 8)
                    : c->xmm[rm.reg][0];
                c->xmm[rm.reg_field][0] = lo;
                c->xmm[rm.reg_field][1] = 0;
            } else if (d.opsize == 2) {
                rm_t rm = MODRM(); FIXUP(rm);
                rm_write(&d,&rm, d.rex_w ? 8 : 4, c->xmm[rm.reg_field][0]);
            } else {
                faultf(c, "#UD 0F 7E without 66/F3 prefix (MMX out of scope)");
            }
            break; }
        case 0x6C: case 0x6D: { /* PUNPCKL/HQDQ (66): interleave quads */
            if (d.opsize != 2 || d.rep) { faultf(c, "#UD 0F %02x without 66", op2); break; }
            rm_t rm = MODRM(); FIXUP(rm);
            uint64_t s0, s1 = 0;
            if (rm.is_mem) {
                uint64_t ea = (rm.seg>=0 ? c->seg[rm.seg].base : 0) + rm_ea(&d,&rm);
                s0 = read_mem_v(m, ea, 8);
                s1 = read_mem_v(m, ea + 8, 8);
            } else { s0 = c->xmm[rm.reg][0]; s1 = c->xmm[rm.reg][1]; }
            if (op2 == 0x6C) { c->xmm[rm.reg_field][1] = s0; }            /* {dst.lo, src.lo} */
            else { uint64_t keep = c->xmm[rm.reg_field][1];
                   c->xmm[rm.reg_field][0] = keep; c->xmm[rm.reg_field][1] = s1; }
            break; }
        case 0xD6: { /* MOVQ xmm -> xmm/m64 (66) */
            if (d.opsize != 2) { faultf(c, "#UD 0F D6 without 66 prefix"); break; }
            rm_t rm = MODRM(); FIXUP(rm);
            if (rm.is_mem)
                write_mem_v(m, (rm.seg>=0 ? c->seg[rm.seg].base : 0) + rm_ea(&d,&rm), 8, c->xmm[rm.reg_field][0]);
            else { c->xmm[rm.reg][0] = c->xmm[rm.reg_field][0]; c->xmm[rm.reg][1] = 0; }
            break; }
        /* ---- K4 scalar-FP subset: double-precision scalar moves through
         * the xmm low quadword, hit by the measured 3D/GUI demo code.
         * IEEE semantics come free from the host's C double; MXCSR flags
         * and exception reporting stay approximated on purpose. ---- */
        case 0x2A: { /* CVTSI2SD (F2) / CVTSI2SS (F3): int r/m -> scalar */
            if (d.rep != 0xF2 && d.rep != 0xF3) { faultf(c, "#UD 0F 2A without F2/F3 (CVTPI out of scope)"); break; }
            rm_t rm = MODRM(); FIXUP(rm);
            int64_t iv = (int64_t)rm_read(&d,&rm, d.rex_w ? 8 : 4);
            if (!d.rex_w) iv = (int32_t)iv;
            if (d.rep == 0xF2) { double v = (double)iv; uint64_t u; memcpy(&u, &v, 8); c->xmm[rm.reg_field][0] = u; }
            else { float v = (float)iv; uint32_t u; memcpy(&u, &v, 4); c->xmm[rm.reg_field][0] = (c->xmm[rm.reg_field][0] & ~0xFFFFFFFFull) | u; }
            break; }
        case 0x2C: case 0x2D: { /* CVTTSD2SI / CVTSD2SI (F2) + SS variants (F3) */
            if (d.rep != 0xF2 && d.rep != 0xF3) { faultf(c, "#UD 0F %02x without F2/F3", op2); break; }
            rm_t rm = MODRM(); FIXUP(rm);
            uint64_t lo = rm.is_mem ? read_mem_v(m, (rm.seg>=0 ? c->seg[rm.seg].base : 0) + rm_ea(&d,&rm), 8)
                                    : c->xmm[rm.reg][0];
            int64_t ov;
            if (d.rep == 0xF2) {
                double v; memcpy(&v, &lo, 8);
                if (v != v || v >= (double)(d.rex_w ? INT64_MAX : INT32_MAX) || v <= (double)(d.rex_w ? INT64_MIN : INT32_MIN))
                    ov = (int64_t)0x8000000000000000ull;
                else if (op2 == 0x2C) ov = (int64_t)v;
                else { double r = nearbyint(v); ov = (int64_t)r; }
            } else {
                float v; memcpy(&v, &lo, 4);
                if (v != v || v >= (double)(d.rex_w ? INT64_MAX : INT32_MAX) || v <= (double)(d.rex_w ? INT64_MIN : INT32_MIN))
                    ov = (int64_t)0x8000000000000000ull;
                else if (op2 == 0x2C) ov = (int64_t)v;
                else ov = (int64_t)lrintf(v);
            }
            if (!d.rex_w) { ov = (int32_t)ov; }
            set_reg(c, rm.reg_field, d.rex_w ? 8 : 4, d.has_rex, (uint64_t)ov);
            break; }
        case 0x5A: { /* CVTSD2SS (F2) / CVTSS2SD (F3): scalar precision hops;
                       * CVTPS2PD (np) / CVTPD2PS (66): packed hops. */
            rm_t rm = MODRM(); FIXUP(rm);
            uint64_t slo, shi = 0;
            if (rm.is_mem) {
                uint64_t ea = (rm.seg>=0 ? c->seg[rm.seg].base : 0) + rm_ea(&d,&rm);
                slo = read_mem_v(m, ea, 8);
                if (d.rep == 0 || (d.opsize == 2 && d.rep == 0)) shi = read_mem_v(m, ea + 8, 8);
            } else { slo = c->xmm[rm.reg][0]; shi = c->xmm[rm.reg][1]; }
            if (d.rep == 0xF2) {                        /* sd -> ss */
                double v; memcpy(&v, &slo, 8); float f = (float)v; uint32_t u;
                memcpy(&u, &f, 4);
                c->xmm[rm.reg_field][0] = (c->xmm[rm.reg_field][0] & ~0xFFFFFFFFull) | u;
            } else if (d.rep == 0xF3) {                 /* ss -> sd */
                float f; memcpy(&f, &slo, 4); double v = (double)f; uint64_t u;
                memcpy(&u, &v, 8);
                c->xmm[rm.reg_field][0] = u;
            } else if (d.rep == 0 && d.opsize == 2) {   /* CVTPD2PS */
                uint32_t out[4] = { 0, 0, 0, 0 };
                for (int i = 0; i < 2; i++) {
                    double v; memcpy(&v, i ? &shi : &slo, 8);
                    float f = (float)v; memcpy(&out[i], &f, 4);
                }
                c->xmm[rm.reg_field][0] = (uint64_t)out[0] | ((uint64_t)out[1] << 32);
                c->xmm[rm.reg_field][1] = (uint64_t)out[2] | ((uint64_t)out[3] << 32);
            } else if (d.rep == 0) {                    /* CVTPS2PD */
                for (int i = 0; i < 2; i++) {
                    float f; memcpy(&f, (uint8_t*)&slo + i * 4, 4);
                    double v = (double)f; uint64_t u;
                    memcpy(&u, &v, 8);
                    c->xmm[rm.reg_field][i] = u;
                }
            } else { faultf(c, "#UD 0F 5A prefix mix out of K4 scope"); }
            break; }
        case 0x5B: { /* CVTDQ2PS (np) / CVTPS2DQ (66) / CVTTPS2DQ (F3): 4-lane int<->float */
            if (d.rep == 0xF2) { faultf(c, "#UD 0F 5B F2 reserved"); break; }
            rm_t rm = MODRM(); FIXUP(rm);
            uint64_t sq[2] = { 0, 0 };
            if (rm.is_mem) {
                uint64_t ea = (rm.seg>=0 ? c->seg[rm.seg].base : 0) + rm_ea(&d,&rm);
                sq[0] = read_mem_v(m, ea, 8);
                sq[1] = read_mem_v(m, ea + 8, 8);
            } else { sq[0] = c->xmm[rm.reg][0]; sq[1] = c->xmm[rm.reg][1]; }
            uint64_t dcq[2];
            for (int i = 0; i < 4; i++) {
                uint32_t lane; memcpy(&lane, (uint8_t*)&sq[i/2] + (i%2)*4, 4);
                if (d.rep == 0) {                       /* dq -> ps */
                    float f = (float)(int32_t)lane;
                    memcpy((uint8_t*)&dcq[i/2] + (i%2)*4, &f, 4);
                } else if (d.opsize == 2) {             /* ps -> dq (RNE) */
                    float f; memcpy(&f, &lane, 4);
                    int32_t r = (f != f || f > 2147483647.0f || f < -2147483648.0f)
                        ? (int32_t)0x80000000 : (int32_t)nearbyintf(f);
                    memcpy((uint8_t*)&dcq[i/2] + (i%2)*4, &r, 4);
                } else {                                /* tps -> dq (trunc) */
                    float f; memcpy(&f, &lane, 4);
                    int32_t r = (f != f || f >= 2147483648.0f || f < -2147483648.0f)
                        ? (int32_t)0x80000000 : (int32_t)f;
                    memcpy((uint8_t*)&dcq[i/2] + (i%2)*4, &r, 4);
                }
            }
            c->xmm[rm.reg_field][0] = dcq[0];
            c->xmm[rm.reg_field][1] = dcq[1];
            break; }
        case 0x14: case 0x15: { /* UNPCKL/UNPCKH: PS (np) / PD (66) */
            rm_t rm = MODRM(); FIXUP(rm);
            uint64_t slo, shi = 0;
            if (rm.is_mem) {
                uint64_t ea = (rm.seg>=0 ? c->seg[rm.seg].base : 0) + rm_ea(&d,&rm);
                slo = read_mem_v(m, ea, 8);
                shi = read_mem_v(m, ea + 8, 8);
            } else { slo = c->xmm[rm.reg][0]; shi = c->xmm[rm.reg][1]; }
            if (d.opsize == 2) {                        /* PD variants: 64b lanes */
                if (op2 == 0x14) { c->xmm[rm.reg_field][0] = c->xmm[rm.reg_field][0]; c->xmm[rm.reg_field][1] = slo; }
                else             { c->xmm[rm.reg_field][0] = c->xmm[rm.reg_field][1]; c->xmm[rm.reg_field][1] = shi; }
            } else {                                    /* PS: interleave 32b lanes */
                uint32_t dq[4], bq[4];
                memcpy(dq, c->xmm[rm.reg_field], 16);
                memcpy(bq, (uint64_t[]){ slo, shi }, 16);
                uint32_t out[4];
                if (op2 == 0x14) { out[0]=dq[0]; out[1]=bq[0]; out[2]=dq[1]; out[3]=bq[1]; }
                else             { out[0]=dq[2]; out[1]=bq[2]; out[2]=dq[3]; out[3]=bq[3]; }
                memcpy(c->xmm[rm.reg_field], out, 16);
            }
            break; }
        case 0xC2: { /* CMPPS/PD/SS/SD with 8 predicates (imm8).
                      * Measured first in the 3D demo (FALSE/LT/LE/NEQ). */
            rm_t rm = MODRM(); FIXUP(rm);
            int packed = (d.rep == 0);
            uint64_t slo, shi = 0;
            if (rm.is_mem) {
                uint64_t ea = (rm.seg>=0 ? c->seg[rm.seg].base : 0) + rm_ea(&d,&rm);
                slo = read_mem_v(m, ea, 8);
                if (packed) shi = read_mem_v(m, ea + 8, 8);
            } else { slo = c->xmm[rm.reg][0]; shi = c->xmm[rm.reg][1]; }
            uint8_t pred = fetch8(&d);
            if (pred > 7) { faultf(c, "#GP CMPPS predicate %u out of range", pred); break; }
            if (packed && d.opsize == 2) {           /* CMPPD */
                for (int q = 0; q < 2; q++) {
                    uint64_t sq = q ? shi : slo;
                    double a, b; memcpy(&a, &c->xmm[rm.reg_field][q], 8); memcpy(&b, &sq, 8);
                    int un = (a != a) || (b != b);
                    int t = 0;
                    switch (pred) {
                        case 0: t = !un && a == b; break;
                        case 1: t = !un && a < b;  break;
                        case 2: t = !un && a <= b; break;
                        case 3: t = un; break;
                        case 4: t = un || a != b; break;
                        case 5: t = un || a >= b; break;
                        case 6: t = un || a > b;  break;
                        case 7: t = !un; break;
                    }
                    c->xmm[rm.reg_field][q] = t ? ~0ull : 0;
                }
            } else if (packed) {                     /* CMPPS */
                uint64_t dq[2] = { c->xmm[rm.reg_field][0], c->xmm[rm.reg_field][1] };
                uint64_t sq2[2] = { slo, shi };
                for (int i = 0; i < 4; i++) {
                    float a, b; memcpy(&a, (uint8_t*)&dq[i/2] + (i%2)*4, 4); memcpy(&b, (uint8_t*)&sq2[i/2] + (i%2)*4, 4);
                    int un = (a != a) || (b != b);
                    int t = 0;
                    switch (pred) {
                        case 0: t = !un && a == b; break;
                        case 1: t = !un && a < b;  break;
                        case 2: t = !un && a <= b; break;
                        case 3: t = un; break;
                        case 4: t = un || a != b; break;
                        case 5: t = un || a >= b; break;
                        case 6: t = un || a > b;  break;
                        case 7: t = !un; break;
                    }
                    uint32_t v = t ? ~0u : 0;
                    memcpy((uint8_t*)&dq[i/2] + (i%2)*4, &v, 4);
                }
                c->xmm[rm.reg_field][0] = dq[0];
                c->xmm[rm.reg_field][1] = dq[1];
            } else if (d.rep == 0xF2) {              /* CMPSD */
                double a, b; memcpy(&a, &c->xmm[rm.reg_field][0], 8); memcpy(&b, &slo, 8);
                int un = (a != a) || (b != b);
                int t = 0;
                switch (pred) {
                    case 0: t = !un && a == b; break;
                    case 1: t = !un && a < b;  break;
                    case 2: t = !un && a <= b; break;
                    case 3: t = un; break;
                    case 4: t = un || a != b; break;
                    case 5: t = un || a >= b; break;
                    case 6: t = un || a > b;  break;
                    case 7: t = !un; break;
                }
                c->xmm[rm.reg_field][0] = t ? ~0ull : 0;   /* high quad kept */
            } else {                                 /* CMPSS */
                float a, b; memcpy(&a, &c->xmm[rm.reg_field][0], 4); memcpy(&b, &slo, 4);
                int un = (a != a) || (b != b);
                int t = 0;
                switch (pred) {
                    case 0: t = !un && a == b; break;
                    case 1: t = !un && a < b;  break;
                    case 2: t = !un && a <= b; break;
                    case 3: t = un; break;
                    case 4: t = un || a != b; break;
                    case 5: t = un || a >= b; break;
                    case 6: t = un || a > b;  break;
                    case 7: t = !un; break;
                }
                uint32_t v = t ? ~0u : 0;
                c->xmm[rm.reg_field][0] = (c->xmm[rm.reg_field][0] & ~0xFFFFFFFFull) | v;
            }
            break; }
        case 0xC6: { /* SHUFPS (np) / SHUFPD (66) with imm8 lane selector */
            rm_t rm = MODRM(); FIXUP(rm);
            uint64_t slo, shi = 0;
            if (rm.is_mem) {
                uint64_t ea = (rm.seg>=0 ? c->seg[rm.seg].base : 0) + rm_ea(&d,&rm);
                slo = read_mem_v(m, ea, 8);
                shi = read_mem_v(m, ea + 8, 8);
            } else { slo = c->xmm[rm.reg][0]; shi = c->xmm[rm.reg][1]; }
            uint8_t imm = fetch8(&d);
            if (d.rep) { faultf(c, "#UD SHUFPS with REP prefix"); break; }
            if (d.opsize == 2) {
                uint64_t a0 = c->xmm[rm.reg_field][0], a1 = c->xmm[rm.reg_field][1];
                c->xmm[rm.reg_field][0] = (imm & 1) ? a1 : a0;
                c->xmm[rm.reg_field][1] = (imm & 2) ? shi : slo;
            } else {
                uint32_t dq[4], bq[4];
                memcpy(dq, c->xmm[rm.reg_field], 16);
                memcpy(bq, (uint64_t[]){ slo, shi }, 16);
                uint32_t out[4];
                out[0] = dq[imm & 3]; out[1] = dq[(imm >> 2) & 3];
                out[2] = bq[(imm >> 4) & 3]; out[3] = bq[(imm >> 6) & 3];
                memcpy(c->xmm[rm.reg_field], out, 16);
            }
            break; }
        case 0x70: { /* PSHUFD (66) imm8 dword shuffle */
            if (d.opsize != 2 || d.rep) { faultf(c, "#UD 0F 70 without 66 (PSHUFLW/HW out of scope)"); break; }
            rm_t rm = MODRM(); FIXUP(rm);
            uint64_t sq[2] = { 0, 0 };
            if (rm.is_mem) {
                uint64_t ea = (rm.seg>=0 ? c->seg[rm.seg].base : 0) + rm_ea(&d,&rm);
                sq[0] = read_mem_v(m, ea, 8);
                sq[1] = read_mem_v(m, ea + 8, 8);
            } else { sq[0] = c->xmm[rm.reg][0]; sq[1] = c->xmm[rm.reg][1]; }
            uint8_t imm = fetch8(&d);
            uint32_t bq[4];
            memcpy(bq, sq, 16);
            uint32_t out[4];
            for (int i = 0; i < 4; i++) out[i] = bq[(imm >> (i * 2)) & 3];
            memcpy(c->xmm[rm.reg_field], out, 16);
            break; }
        case 0x50: { /* MOVMSKPS (np) / MOVMSKPD (66): sign bits -> GP */
            if (d.rep) { faultf(c, "#UD 0F 50 with REP prefix"); break; }
            rm_t rm = MODRM(); FIXUP(rm);
            uint64_t sq[2] = { rm.is_mem ? 0 : c->xmm[rm.reg][0], rm.is_mem ? 0 : c->xmm[rm.reg][1] };
            if (rm.is_mem) {
                uint64_t ea = (rm.seg>=0 ? c->seg[rm.seg].base : 0) + rm_ea(&d,&rm);
                sq[0] = read_mem_v(m, ea, 8);
                sq[1] = read_mem_v(m, ea + 8, 8);
            }
            uint32_t mask = 0;
            if (d.opsize == 2) {
                mask = (uint32_t)((sq[0] >> 63) | ((sq[1] >> 63) << 1));
            } else {
                for (int i = 0; i < 4; i++) {
                    uint32_t lane; memcpy(&lane, (uint8_t*)&sq[i/2] + (i%2)*4, 4);
                    mask |= ((lane >> 31) & 1u) << i;
                }
            }
            set_reg(c, rm.reg_field, 8, d.has_rex, mask);
            break; }
        case 0x54: case 0x55: case 0x56: { /* ANDPS/ANDNPS/ORPS (+PD via 66) */
            if (d.rep) { faultf(c, "#UD 0F %02x with REP prefix", op2); break; }
            rm_t rm = MODRM(); FIXUP(rm);
            uint64_t slo, shi;
            if (rm.is_mem) {
                uint64_t ea = (rm.seg>=0 ? c->seg[rm.seg].base : 0) + rm_ea(&d,&rm);
                slo = read_mem_v(m, ea, 8);
                shi = read_mem_v(m, ea + 8, 8);
            } else { slo = c->xmm[rm.reg][0]; shi = c->xmm[rm.reg][1]; }
            if (op2 == 0x54) { c->xmm[rm.reg_field][0] &= slo; c->xmm[rm.reg_field][1] &= shi; }
            if (op2 == 0x55) { c->xmm[rm.reg_field][0] = ~c->xmm[rm.reg_field][0] & slo;
                               c->xmm[rm.reg_field][1] = ~c->xmm[rm.reg_field][1] & shi; }
            if (op2 == 0x56) { c->xmm[rm.reg_field][0] |= slo; c->xmm[rm.reg_field][1] |= shi; }
            break; }
        case 0x71: case 0x72: { /* PSRLW/D (66,/2) PSRAW/D (/4) PSLLW/D (/6), imm8 */
            if (d.opsize != 2 || d.rep) { faultf(c, "#UD 0F %02x without 66 (MMX out of scope)", op2); break; }
            uint8_t modrm = fetch8(&d);
            int ext = (modrm >> 3) & 7;
            if ((modrm & 0xC0) != 0xC0) { faultf(c, "#UD 0F %02x needs mod=3", op2); break; }
            int r = modrm & 7;
            uint8_t imm = fetch8(&d);
            int elemsz = (op2 == 0x71) ? 2 : 4;
            if (imm >= elemsz * 8) imm = elemsz * 8;   /* out-of-range shifts zero (sat) */
            for (int q = 0; q < 2; q++) {
                uint64_t v = c->xmm[r][q], out = 0;
                int lanes = 8 / elemsz;
                for (int i = 0; i < lanes; i++) {
                    uint64_t lane = (v >> (i * elemsz * 8)) & ((1ull << (elemsz * 8)) - 1);
                    uint64_t res = 0;
                    if (ext == 2) res = lane >> imm;                        /* PSRL */
                    else if (ext == 4) {                                    /* PSRA */
                        uint64_t sign = lane >> (elemsz * 8 - 1);
                        res = (int64_t)(int32_t)(lane << (32 - elemsz * 8)) >> imm >> (32 - elemsz * 8);
                        (void)sign;
                    } else if (ext == 6) res = (lane << imm) & ((1ull << (elemsz * 8)) - 1);
                    else { faultf(c, "#UD 0F %02x /%d", op2, ext); goto done_shift; }
                    out |= (res & ((1ull << (elemsz * 8)) - 1)) << (i * elemsz * 8);
                }
                c->xmm[r][q] = out;
            }
        done_shift: ;
            break; }
        case 0x60: case 0x61: case 0x62: { /* PUNPCKLBW/LWD/LDQ (66) low interleave */
            if (d.opsize != 2 || d.rep) { faultf(c, "#UD 0F %02x without 66", op2); break; }
            rm_t rm = MODRM(); FIXUP(rm);
            uint64_t b;
            if (rm.is_mem)
                b = read_mem_v(m, (rm.seg>=0 ? c->seg[rm.seg].base : 0) + rm_ea(&d,&rm), 8);
            else
                b = c->xmm[rm.reg][0];
            int esz = (op2 == 0x60) ? 1 : (op2 == 0x61 ? 2 : 4);
            uint64_t a = c->xmm[rm.reg_field][0];
            int n = 8 / esz;
            /* interleaved result: [a0,b0,a1,b1,...] across the two quads */
            {
                uint64_t mask = (1ull << (esz * 8)) - 1;
                uint64_t res0 = 0, res1 = 0;
                int idx[16];
                for (int i = 0; i < n; i++) { idx[2*i] = i; idx[2*i+1] = -1-i; }
                for (int pos = 0; pos < 2 * n; pos++) {
                    uint64_t lane = (idx[pos] >= 0)
                        ? ((a >> (idx[pos] * esz * 8)) & mask)
                        : ((b >> ((-1 - idx[pos]) * esz * 8)) & mask);
                    if (pos < n) res0 |= lane << (pos * esz * 8);
                    else         res1 |= lane << ((pos - n) * esz * 8);
                }
                c->xmm[rm.reg_field][0] = res0;
                c->xmm[rm.reg_field][1] = res1;
            }
            break; }
        case 0x64: case 0x65: case 0x66: { /* PCMPGTB/W/D (66): signed greater-than */
            if (d.opsize != 2 || d.rep) { faultf(c, "#UD 0F %02x without 66 (MMX out of scope)", op2); break; }
            rm_t rm = MODRM(); FIXUP(rm);
            uint64_t bq[2];
            if (rm.is_mem) {
                uint64_t ea = (rm.seg>=0 ? c->seg[rm.seg].base : 0) + rm_ea(&d,&rm);
                bq[0] = read_mem_v(m, ea, 8);
                bq[1] = read_mem_v(m, ea + 8, 8);
            } else { bq[0] = c->xmm[rm.reg][0]; bq[1] = c->xmm[rm.reg][1]; }
            int esz = (op2 == 0x64) ? 1 : (op2 == 0x65 ? 2 : 4);
            int n = 16 / esz;
            uint64_t mask = (esz == 1) ? 0xFFull : (esz == 2 ? 0xFFFFull : 0xFFFFFFFFull);
            uint8_t out[16] = { 0 };
            for (int i = 0; i < n; i++) {
                int q = i / (8 / esz), pos = i % (8 / esz);
                int64_t la = (int64_t)((c->xmm[rm.reg_field][q] >> (pos * esz * 8)) & mask);
                int64_t lb = (int64_t)((bq[q] >> (pos * esz * 8)) & mask);
                if (esz == 1) { la = (int8_t)la; lb = (int8_t)lb; }
                else if (esz == 2) { la = (int16_t)la; lb = (int16_t)lb; }
                else { la = (int32_t)la; lb = (int32_t)lb; }
                if (la > lb) memset(out + i * esz, 0xFF, (size_t)esz);
            }
            memcpy(c->xmm[rm.reg_field], out, 16);
            break; }
        case 0x74: case 0x75: case 0x76: { /* PCMPEQB/W/D (66) */
            if (d.opsize != 2 || d.rep) { faultf(c, "#UD 0F %02x without 66", op2); break; }
            rm_t rm = MODRM(); FIXUP(rm);
            uint64_t bq[2];
            if (rm.is_mem) {
                uint64_t ea = (rm.seg>=0 ? c->seg[rm.seg].base : 0) + rm_ea(&d,&rm);
                bq[0] = read_mem_v(m, ea, 8);
                bq[1] = read_mem_v(m, ea + 8, 8);
            } else { bq[0] = c->xmm[rm.reg][0]; bq[1] = c->xmm[rm.reg][1]; }
            int esz = (op2 == 0x74) ? 1 : (op2 == 0x75 ? 2 : 4);
            int n = 16 / esz;
            uint64_t mask = (esz == 1) ? 0xFFull : (esz == 2 ? 0xFFFFull : 0xFFFFFFFFull);
            uint8_t out[16] = { 0 };
            for (int i = 0; i < n; i++) {
                uint64_t la = (c->xmm[rm.reg_field][i/ (8/esz) ] >> ((i % (8/esz)) * esz * 8)) & mask;
                uint64_t lb = (bq[i / (8/esz)] >> ((i % (8/esz)) * esz * 8)) & mask;
                if (la == lb) memset(out + i * esz, 0xFF, (size_t)esz);
            }
            memcpy(c->xmm[rm.reg_field], out, 16);
            break; }
        case 0xD7: { /* PMOVMSKB (66): byte sign bits -> GP */
            if (d.opsize != 2 || d.rep) { faultf(c, "#UD 0F D7 without 66"); break; }
            rm_t rm = MODRM(); FIXUP(rm);
            const uint8_t *raw = (const uint8_t *)c->xmm[rm.reg];
            uint32_t mask = 0;
            for (int i = 0; i < 16; i++) mask |= ((raw[i] >> 7) & 1u) << i;
            set_reg(c, rm.reg_field, 8, d.has_rex, mask);
            break; }
        case 0x73: { /* PSLLDQ (66,/2) / PSRLDQ (66,/3) byte shifts */
            if (d.opsize != 2 || d.rep) { faultf(c, "#UD 0F 73 without 66"); break; }
            uint8_t modrm = fetch8(&d);
            int ext = (modrm >> 3) & 7;
            if ((modrm & 0xC0) != 0xC0) { faultf(c, "#UD 0F 73 needs mod=3"); break; }
            int r = modrm & 7;
            uint8_t imm = fetch8(&d);
            int n = imm > 16 ? 16 : imm;
            if (ext == 2) {                             /* PSLLDQ */
                uint64_t all[2] = { c->xmm[r][0], c->xmm[r][1] };
                uint8_t *p = (uint8_t *)all;
                uint8_t out[16] = { 0 };
                memcpy(out + n, p, (size_t)(16 - n));
                memcpy(all, out, 16);
                c->xmm[r][0] = all[0]; c->xmm[r][1] = all[1];
            } else if (ext == 3) {                      /* PSRLDQ */
                uint64_t all[2] = { c->xmm[r][0], c->xmm[r][1] };
                uint8_t *p = (uint8_t *)all;
                uint8_t out[16] = { 0 };
                memcpy(out, p + n, (size_t)(16 - n));
                memcpy(all, out, 16);
                c->xmm[r][0] = all[0]; c->xmm[r][1] = all[1];
            } else faultf(c, "#UD 0F 73 /%d (byte shifts only)", ext);
            break; }
        case 0x58: case 0x59: case 0x5C: case 0x5D: case 0x5E: case 0x5F: {
            /* SSE FP arithmetic: scalar (F2/F3) + packed (np/66) -- the
             * packed lanes arrived with the measured 3D/GUI demo code
             * (KERNEL-BOOT K4; IEEE via host C, MXCSR flags approximated). */
            rm_t rm = MODRM(); FIXUP(rm);
            uint64_t slo, shi = 0;
            if (rm.is_mem) {
                uint64_t ea = (rm.seg>=0 ? c->seg[rm.seg].base : 0) + rm_ea(&d,&rm);
                slo = read_mem_v(m, ea, 8);
                if (!(d.rep == 0xF2 || d.rep == 0xF3)) shi = read_mem_v(m, ea + 8, 8);
            } else { slo = c->xmm[rm.reg][0]; shi = c->xmm[rm.reg][1]; }
            if (d.rep == 0 || (d.opsize == 2 && d.rep == 0)) {
                if (d.opsize == 2) {             /* packed PD: 2 x double */
                    for (int q = 0; q < 2; q++) {
                        uint64_t sq = q ? shi : slo;
                        double a, b; memcpy(&a, &c->xmm[rm.reg_field][q], 8); memcpy(&b, &sq, 8);
                        double r = 0;
                        switch (op2) {
                            case 0x58: r = a + b; break;
                            case 0x59: r = a * b; break;
                            case 0x5C: r = a - b; break;
                            case 0x5D: r = (a < b) ? a : b; break;
                            case 0x5E: r = a / b; break;
                            case 0x5F: r = (a > b) ? a : b; break;
                        }
                        memcpy(&c->xmm[rm.reg_field][q], &r, 8);
                    }
                } else {                         /* packed PS: 4 x float */
                    uint64_t dq[2] = { c->xmm[rm.reg_field][0], c->xmm[rm.reg_field][1] };
                    uint64_t sq[2] = { slo, shi };
                    for (int i = 0; i < 4; i++) {
                        float a, b; memcpy(&a, (uint8_t*)&dq[i/2] + (i%2)*4, 4); memcpy(&b, (uint8_t*)&sq[i/2] + (i%2)*4, 4);
                        float r = 0;
                        switch (op2) {
                            case 0x58: r = a + b; break;
                            case 0x59: r = a * b; break;
                            case 0x5C: r = a - b; break;
                            case 0x5D: r = (a < b) ? a : b; break;
                            case 0x5E: r = a / b; break;
                            case 0x5F: r = (a > b) ? a : b; break;
                        }
                        memcpy((uint8_t*)&dq[i/2] + (i%2)*4, &r, 4);
                    }
                    c->xmm[rm.reg_field][0] = dq[0];
                    c->xmm[rm.reg_field][1] = dq[1];
                }
                break;
            }
            if (d.rep != 0xF2 && d.rep != 0xF3) { faultf(c, "#UD packed 0F %02x arithmetic out of K4 scope", op2); break; }
            uint64_t lo = slo;
            if (d.rep == 0xF2) {
                double a, b; memcpy(&a, &c->xmm[rm.reg_field][0], 8); memcpy(&b, &lo, 8);
                double r = 0;
                switch (op2) {
                    case 0x58: r = a + b; break;
                    case 0x59: r = a * b; break;
                    case 0x5C: r = a - b; break;
                    case 0x5D: r = (a < b) ? a : b; break;
                    case 0x5E: r = a / b; break;
                    case 0x5F: r = (a > b) ? a : b; break;
                }
                memcpy(&c->xmm[rm.reg_field][0], &r, 8);
            } else {
                float a, b; memcpy(&a, &c->xmm[rm.reg_field][0], 4); memcpy(&b, &lo, 4);
                float r = 0;
                switch (op2) {
                    case 0x58: r = a + b; break;
                    case 0x59: r = a * b; break;
                    case 0x5C: r = a - b; break;
                    case 0x5D: r = (a < b) ? a : b; break;
                    case 0x5E: r = a / b; break;
                    case 0x5F: r = (a > b) ? a : b; break;
                }
                uint32_t u; memcpy(&u, &r, 4);
                c->xmm[rm.reg_field][0] = (c->xmm[rm.reg_field][0] & ~0xFFFFFFFFull) | u;
            }
            break; }
        case 0x2E: case 0x2F: { /* UCOMISS (none) / UCOMISD (66): low-scalar cmp */
            rm_t rm = MODRM(); FIXUP(rm);
            uint64_t lo = rm.is_mem ? read_mem_v(m, (rm.seg>=0 ? c->seg[rm.seg].base : 0) + rm_ea(&d,&rm), 8)
                                    : c->xmm[rm.reg][0];
            uint64_t zf, pf, cf;
            if (d.opsize == 2) {
                double a, b; memcpy(&a, &c->xmm[rm.reg_field][0], 8); memcpy(&b, &lo, 8);
                if (a != a || b != b) { zf = pf = cf = 1; }
                else if (a == b) { zf = 1; pf = cf = 0; }
                else if (a < b)  { cf = 1; zf = pf = 0; }
                else             { zf = pf = cf = 0; }
            } else {
                float a, b; memcpy(&a, &c->xmm[rm.reg_field][0], 4); memcpy(&b, &lo, 4);
                if (a != a || b != b) { zf = pf = cf = 1; }
                else if (a == b) { zf = 1; pf = cf = 0; }
                else if (a < b)  { cf = 1; zf = pf = 0; }
                else             { zf = pf = cf = 0; }
            }
            c->rflags &= ~(FLAG_ZF|FLAG_PF|FLAG_CF|FLAG_OF|FLAG_SF|FLAG_AF);
            c->rflags |= (zf ? FLAG_ZF : 0) | (pf ? FLAG_PF : 0) | (cf ? FLAG_CF : 0);
            break; }
        case 0xDB: case 0xDF: case 0xEB: {
            /* PAND/PANDN/POR integer logic (66 prefix; pxor kept separate) */
            if (d.opsize != 2 || d.rep) { faultf(c, "#UD 0F %02x without 66 (MMX out of scope)", op2); break; }
            rm_t rm = MODRM(); FIXUP(rm);
            uint64_t s0, s1;
            if (rm.is_mem) {
                uint64_t ea = (rm.seg>=0 ? c->seg[rm.seg].base : 0) + rm_ea(&d,&rm);
                s0 = read_mem_v(m, ea, 8);
                s1 = read_mem_v(m, ea + 8, 8);
            } else { s0 = c->xmm[rm.reg][0]; s1 = c->xmm[rm.reg][1]; }
            if (op2 == 0xDB) { c->xmm[rm.reg_field][0] &= s0; c->xmm[rm.reg_field][1] &= s1; }
            if (op2 == 0xDF) { c->xmm[rm.reg_field][0] = ~c->xmm[rm.reg_field][0] & s0;
                               c->xmm[rm.reg_field][1] = ~c->xmm[rm.reg_field][1] & s1; }
            if (op2 == 0xEB) { c->xmm[rm.reg_field][0] |= s0; c->xmm[rm.reg_field][1] |= s1; }
            break; }
        case 0xD4: case 0xF8: case 0xF9: case 0xFA: case 0xFB: case 0xFC: case 0xFD: case 0xFE: {
            /* PADDQ/PSUB{B,W,D,Q}/PADD{B,W,D} integer lanes (66 prefix) */
            if (d.opsize != 2 || d.rep) { faultf(c, "#UD 0F %02x without 66 (MMX out of scope)", op2); break; }
            rm_t rm = MODRM(); FIXUP(rm);
            uint64_t s0, s1;
            if (rm.is_mem) {
                uint64_t ea = (rm.seg>=0 ? c->seg[rm.seg].base : 0) + rm_ea(&d,&rm);
                s0 = read_mem_v(m, ea, 8);
                s1 = read_mem_v(m, ea + 8, 8);
            } else { s0 = c->xmm[rm.reg][0]; s1 = c->xmm[rm.reg][1]; }
            int esz = (op2 == 0xD4 || op2 == 0xFB) ? 8
                     : (op2 == 0xFA || op2 == 0xFE) ? 4
                     : (op2 == 0xF9 || op2 == 0xFD) ? 2 : 1;
            int sub = (op2 == 0xF8 || op2 == 0xF9 || op2 == 0xFA || op2 == 0xFB);
            for (int q = 0; q < 2; q++) {
                uint64_t a = c->xmm[rm.reg_field][q];
                uint64_t b = q ? s1 : s0;
                uint64_t out = 0;
                /* K6 fix: elements PER QWORD are 8/esz (was 16/esz -- the
                 * 128-bit count iterated over each 64-bit half, overrunning
                 * the operand into UB shift exponents for esz < 8; measured
                 * as a UBSan "shift exponent 64/112" stop on the guest's
                 * software-SSE 3D demo path).  The lane mask is written so
                 * the shift count never reaches 64, cmov-evaluation safe. */
                for (int i = 0; i < 8 / esz; i++) {
                    uint64_t mask = ~0ull >> (64 - esz * 8);
                    uint64_t la = (a >> (i * esz * 8)) & mask;
                    uint64_t lb = (b >> (i * esz * 8)) & mask;
                    uint64_t r = sub ? (la - lb) : (la + lb);
                    out |= (r & mask) << (i * esz * 8);
                }
                c->xmm[rm.reg_field][q] = out;
            }
            break; }
        case 0xEF: { /* PXOR xmm (66 prefix; the MMX view stays out of scope) */
            if (d.opsize != 2 || d.rep) { faultf(c, "#UD 0F EF without 66 prefix (MMX out of scope)"); break; }
            rm_t rm = MODRM(); FIXUP(rm);
            uint64_t lo, hi;
            if (rm.is_mem) {
                uint64_t ea = (rm.seg>=0 ? c->seg[rm.seg].base : 0) + rm_ea(&d,&rm);
                lo = read_mem_v(m, ea, 8); hi = read_mem_v(m, ea + 8, 8);
            } else { lo = c->xmm[rm.reg][0]; hi = c->xmm[rm.reg][1]; }
            c->xmm[rm.reg_field][0] ^= lo;
            c->xmm[rm.reg_field][1] ^= hi;
            break; }
        case 0x57: { /* XORPS / XORPD: full 128-bit xor */
            rm_t rm = MODRM(); FIXUP(rm);
            uint64_t lo, hi;
            if (rm.is_mem) {
                uint64_t ea = (rm.seg>=0 ? c->seg[rm.seg].base : 0) + rm_ea(&d,&rm);
                lo = read_mem_v(m, ea, 8); hi = read_mem_v(m, ea + 8, 8);
            } else { lo = c->xmm[rm.reg][0]; hi = c->xmm[rm.reg][1]; }
            c->xmm[rm.reg_field][0] ^= lo;
            c->xmm[rm.reg_field][1] ^= hi;
            break; }
        case 0xB6: { rm_t rm=MODRM(); FIXUP(rm); uint64_t v=rm_read(&d,&rm,1); set_reg(c,rm.reg_field,osz,d.has_rex,v); break; }
        case 0xB7: { rm_t rm=MODRM(); FIXUP(rm); uint64_t v=rm_read(&d,&rm,2); set_reg(c,rm.reg_field,osz,d.has_rex,v); break; }
        /* ---- SHLD/SHRD r/m,r,imm8 or CL ---- */
        case 0xA4: case 0xA5: case 0xAC: case 0xAD: {
            int size = osz;
            int bits = size * 8;
            int is_right = op2 == 0xAC || op2 == 0xAD;
            rm_t rm = MODRM();
            uint8_t raw_count = (op2 == 0xA4 || op2 == 0xAC)
                              ? fetch8(&d) : (uint8_t)get_reg(c, RCX, 1, d.has_rex);
            FIXUP(rm);  /* immediate is part of a RIP-relative instruction */
            if (c->fault || c->exception_taken) goto done;
            uint8_t count = raw_count & (size == 8 ? 63 : 31);
            uint64_t mask = reg_mask(size);
            uint64_t dst = rm_read(&d, &rm, size) & mask;
            if (c->fault || c->exception_taken) goto done;
            if (!count) break;  /* zero count preserves destination and flags */
            uint64_t src = get_reg(c, rm.reg_field, size, d.has_rex) & mask;

            /* Treat the operands as a 2*width concatenation. This avoids
             * undefined C shifts and also gives deterministic silicon-like
             * behavior for the architecturally undefined 16-bit count>15
             * range (the architectural count mask is still applied above). */
            __uint128_t pair = is_right
                ? (((__uint128_t)src << bits) | dst)
                : (((__uint128_t)dst << bits) | src);
            uint64_t result = is_right
                ? (uint64_t)(pair >> count) & mask
                : (uint64_t)((pair << count) >> bits) & mask;
            uint64_t cf;
            if (is_right) {
                cf = count <= bits ? (dst >> (count - 1)) & 1
                                   : (src >> (count - bits - 1)) & 1;
            } else {
                cf = count <= bits ? (dst >> (bits - count)) & 1
                                   : (src >> (2 * bits - count)) & 1;
            }
            set_flags_logic(c, result, size);  /* SF/ZF/PF, AF undefined */
            if (cf) c->rflags |= FLAG_CF;
            if (count == 1) {
                uint64_t msb = 1ULL << (bits - 1);
                int overflow = is_right ? (((dst ^ result) & msb) != 0)
                                        : ((((result & msb) != 0) ^ (cf != 0)) != 0);
                if (overflow) c->rflags |= FLAG_OF;
            }
            rm_write(&d, &rm, size, result);
            break; }
        /* ---- MOVSX (sign-extended counterpart of MOVZX) ---- */
        case 0xBE: case 0xBF: {
            int from = (op2==0xBE)?1:2; rm_t rm=MODRM(); FIXUP(rm);
            uint64_t v = rm_read(&d,&rm,from);
            int64_t sv = (from==1) ? (int8_t)v : (int16_t)v;
            set_reg(c, rm.reg_field, osz, d.has_rex, (uint64_t)sv);
            break; }
        /* ---- IMUL r, r/m (two-operand) -- caught missing by the C10 table
           suite. CF=OF=1 iff the full product does not fit the destination
           width; SF/ZF/AF/PF are undefined, we leave them. ---- */
        case 0xAF: {
            rm_t rm=MODRM(); FIXUP(rm);
            uint64_t a = get_reg(c, rm.reg_field, osz, d.has_rex);
            uint64_t b = rm_read(&d,&rm,osz);
            uint64_t res; int fits;
            if (osz==2)      { int32_t p = (int32_t)((int16_t)a * (int16_t)b); res = (uint16_t)p; fits = (p == (int32_t)(int16_t)p); }
            else if (osz==4) { int64_t p = (int64_t)(int32_t)a * (int32_t)b; res = (uint32_t)p; fits = (p == (int64_t)(int32_t)p); }
            else             { __int128 p = (__int128)(int64_t)a * (int64_t)b; res = (uint64_t)p; fits = (p == (__int128)(int64_t)p); }
            set_reg(c, rm.reg_field, osz, d.has_rex, res);
            c->rflags &= ~(FLAG_CF|FLAG_OF);
            if (!fits) c->rflags |= FLAG_CF|FLAG_OF;
            break; }
        /* ---- CMOVcc r, r/m (consumes the C1 eval_cc) ---- */
        case 0x40: case 0x41: case 0x42: case 0x43: case 0x44: case 0x45: case 0x46: case 0x47:
        case 0x48: case 0x49: case 0x4A: case 0x4B: case 0x4C: case 0x4D: case 0x4E: case 0x4F: {
            rm_t rm=MODRM(); FIXUP(rm);
            uint64_t v = rm_read(&d,&rm,osz);   /* memory operand is read even when cc is false */
            if (eval_cc(op2 & 0xF, c->rflags)) set_reg(c, rm.reg_field, osz, d.has_rex, v);
            break; }
        /* ---- SETcc r/m8,1-byte store ---- */
        case 0x90: case 0x91: case 0x92: case 0x93: case 0x94: case 0x95: case 0x96: case 0x97:
        case 0x98: case 0x99: case 0x9A: case 0x9B: case 0x9C: case 0x9D: case 0x9E: case 0x9F: {
            rm_t rm=MODRM(); FIXUP(rm);
            rm_write(&d,&rm,1, eval_cc(op2 & 0xF, c->rflags) ? 1 : 0);
            break; }
        /* ---- CMPXCHG r/m,r (accumulator compare; LOCK-free: atomicity is a no-op) ---- */
        case 0xB0: case 0xB1: {
            int size = (op2==0xB0)?1:osz; rm_t rm=MODRM(); FIXUP(rm);
            uint64_t acc = get_reg(c,RAX,size,d.has_rex);
            uint64_t dst = rm_read(&d,&rm,size);
            uint64_t src = get_reg(c,rm.reg_field,size,d.has_rex);
            set_flags_sub(c,acc,dst,0,acc-dst,size);
            if ((acc & reg_mask(size)) == (dst & reg_mask(size))) rm_write(&d,&rm,size,src);
            else set_reg(c,RAX,size,d.has_rex,dst);
            break; }
        /* ---- XADD r/m,r ---- */
        case 0xC0: case 0xC1: {
            int size = (op2==0xC0)?1:osz; rm_t rm=MODRM(); FIXUP(rm);
            uint64_t dst = rm_read(&d,&rm,size), src = get_reg(c,rm.reg_field,size,d.has_rex);
            uint64_t sum = dst + src;
            set_flags_add(c,dst,src,0,sum,size);
            set_reg(c,rm.reg_field,size,d.has_rex,dst);
            rm_write(&d,&rm,size,sum);
            break; }
        /* ---- BSWAP r32/r64 (16-bit operand size is per-CPU undefined; we swap 32) ---- */
        case 0xC8: case 0xC9: case 0xCA: case 0xCB: case 0xCC: case 0xCD: case 0xCE: case 0xCF: {
            int r = (op2-0xC8) | (d.has_rex ? (d.rex_b<<3) : 0);
            if (osz==8) {
                uint64_t v = c->gpr[r], s = 0;
                for (int i = 0; i < 8; i++) s |= ((v >> (8*i)) & 0xFFull) << (8*(7-i));
                c->gpr[r] = s;
            } else {
                uint32_t v = (uint32_t)c->gpr[r];
                c->gpr[r] = (uint32_t)(((v&0xFF)<<24) | ((v&0xFF00)<<8) | ((v>>8)&0xFF00) | ((v>>24)&0xFF));
            }
            break; }
        case 0xA2: do_cpuid(m); break;
        case 0x18: case 0x19: case 0x1A: case 0x1B:
        case 0x1C: case 0x1D: case 0x1E: case 0x1F: {
            /* Hint block (PREFETCHh / multibyte NOP 0F 19-1F): the modrm
             * operand is decoded -- including SIB/disp32, RIP-relative --
             * but NEVER accessed: the SDM is explicit these encodings
             * cannot fault. Measured need: clang sprinkles 0F 1F /0
             * alignment NOPs through -O2 code (first K2 trace item). */
            uint8_t modrm = fetch8(&d);
            rm_t rm = decode_modrm(&d, modrm);
            fixup_riprel(&d, &rm);
            (void)rm;
            break; }
        /* ---- BT/BTS/BTR/BTC bit-string family (K2: measured, kernel flag
         *      tests/toggles) and group 0F BA /4..7 imm8 forms. CF := old
         *      bit; other arithmetic flags are architecturally undefined and
         *      deliberately left untouched (fuzz masks them per class). ---- */
        case 0xA3: case 0xAB: case 0xB3: case 0xBB: {
            int opid = (op2==0xAB)?1 : (op2==0xB3)?2 : (op2==0xBB)?3 : 0; /* 0 BT 1 TS 2 TR 3 TC */
            int bits = osz*8;
            rm_t rm=MODRM(); FIXUP(rm);
            uint64_t raw_sel = get_reg(c, rm.reg_field, osz, d.has_rex);
            int64_t selv = (osz==8) ? (int64_t)raw_sel
                         : (osz==4) ? (int64_t)(int32_t)raw_sel
                                    : (int64_t)(int16_t)raw_sel;
            uint64_t elem;
            rm_t access = rm;
            uint64_t ib;
            if (rm.is_mem) {
                /* signed bit-string addressing: floor((sel)/bits) elements */
                int64_t eoff = selv >> (bits==64?6 : bits==32?5 : 4);
                access.addr = rm_ea(&d,&rm) + eoff * (bits/8);
                access.is_mem = 1;   /* resolved (lazily) above: resolve-once */
                elem = rm_read(&d,&access,osz);
                ib = (uint64_t)selv & (uint64_t)(bits-1);
            } else {
                elem = rm_read(&d,&access,osz);
                ib = (uint64_t)selv & (uint64_t)(bits-1);
            }
            uint64_t oldb = (elem >> ib) & 1;
            c->rflags = (c->rflags & ~FLAG_CF) | oldb;
            if (opid) {
                if (opid==1) elem |=  (1ULL << ib);
                if (opid==2) elem &= ~(1ULL << ib);
                if (opid==3) elem ^=  (1ULL << ib);
                rm_write(&d,&access,osz,elem);
            }
            break; }
        case 0xBA: {
            int bits = osz*8;
            rm_t rm=MODRM(); FIXUP(rm); int ext = rm.reg_field & 7;
            uint8_t imm = fetch8(&d);
            if (ext < 4) { faultf(c, "#UD unsupported 0F opcode 0x0F 0xBA /%d at rip=%08llx", ext, (unsigned long long)d.start_pc); break; }
            uint64_t ib = (uint64_t)imm & (uint64_t)(bits-1);
            uint64_t elem = rm_read(&d,&rm,osz);
            uint64_t oldb = (elem >> ib) & 1;
            c->rflags = (c->rflags & ~FLAG_CF) | oldb;
            if (ext==5) elem |=  (1ULL << ib);
            if (ext==6) elem &= ~(1ULL << ib);
            if (ext==7) elem ^=  (1ULL << ib);
            if (ext!=4) rm_write(&d,&rm,osz,elem);
            break; }
        case 0x09: { /* WBINVD: no data-cache model, so coherent no-op. */
            if ((c->cr0 & 1) && cpl_now(c) != 0) {
                raise_exception(m, 13, 1, 0); /* #GP(0): privileged instruction */
                break;
            }
            /* Guest writes are immediately visible; there is no cache to flush. */
            break; }
        case 0x31: { /* RDTSC: virtual time = executed instruction attempts x per-platform
                        ratio (platform_t::tsc_per_instr) — deterministic */
            uint64_t t = m->vtime_instr * (uint64_t)m->plat->tsc_per_instr;
            /* K6: TSCs are machine-synchronized (as on silicon); with one
             * vcpu vtime_instr == instr_count bit-for-bit (UP identity). */
            c->gpr[RAX] = (uint32_t)t; c->gpr[RDX] = (uint32_t)(t >> 32);
            break; }
        case 0xAE: { /* group 15: LFENCE/MFENCE/SFENCE (mod=11, /5 /6 /7) are
                        NOPs here -- a single-threaded in-order core has
                        nothing to order. Memory forms (K3, measured: the
                        AuraLite FPU init issued FXSAVE at +426M):
                        /0 FXSAVE m512, /1 FXRSTOR m512, /2 LDMXCSR,
                        /3 STMXCSR, /7 CLFLUSH (no cache model -> no-op). */
            uint8_t modrm = fetch8(&d);
            int mod = modrm >> 6, ext = (modrm >> 3) & 7;
            if (mod == 3) {
                if (!(ext == 5 || ext == 6 || ext == 7))
                    faultf(c, "#UD unsupported 0F opcode 0x0F 0xAE /%d mod%d at rip=%08llx (mode %s)",
                           ext, mod, (unsigned long long)d.start_pc, cpu_mode_name(c));
                break;
            }
            if (ext == 7) break;                 /* CLFLUSH: no caches to flush */
            if (ext > 3) { /* XSAVE/XRSTOR/XSAVEOPT are CPUID-gated on real
                              silicon and we don't advertise XSAVE, so #UD is
                              the architecturally honest answer. */
                faultf(c, "#UD XSAVE family out of scope (CPUID.XSAVE not set)");
                break;
            }
            rm_t rm = decode_modrm(&d, modrm); fixup_riprel(&d,&rm);
            uint64_t base2 = (rm.seg>=0)? d.c->seg[rm.seg].base:0;
            uint64_t addr = base2 + rm_ea(&d, &rm);
            if (ext == 2) { c->mxcsr = (uint32_t)read_mem_v(m, addr, 4) & 0x0000FFFFu; break; }
            if (ext == 3) { write_mem_v(m, addr, 4, c->mxcsr); break; }
            if (addr & 15) { raise_exception(m, 13, 1, 0); break; }  /* FXSAVE aligns */
            if (ext == 0) { /* FXSAVE m512: LIVE x87 state + live MXCSR +
                               live XMM file (K4 round-trip pin). Layout is
                               HOST-MEASURED (fxprobe2 receipt, pinned by
                               test_fpu.c::v_fxsave_live_layout): the F0
                               audit found the old image two bytes off with
                               MXCSR sitting in the MASK slot -- */
                write_mem_v(m, addr +  0, 2, c->fcw);
                write_mem_v(m, addr +  2, 2, c->fsw);
                write_mem_v(m, addr +  4, 1, x87_ftw_abridged(c)); /* FTW 1B */
                write_mem_v(m, addr +  5, 1, 0);
                write_mem_v(m, addr +  6, 2, c->fop);
                write_mem_v(m, addr +  8, 4, (uint32_t)c->fip);
                write_mem_v(m, addr + 12, 2, c->fcs);
                write_mem_v(m, addr + 14, 2, 0);
                write_mem_v(m, addr + 16, 4, (uint32_t)c->fdp);
                write_mem_v(m, addr + 20, 2, c->fds);
                write_mem_v(m, addr + 22, 2, 0);
                write_mem_v(m, addr + 24, 4, c->mxcsr);           /* MXCSR@24 */
                write_mem_v(m, addr + 28, 4, 0x0000FFBF);         /* MASK@28 */
                for (int i = 0; i < 8; i++) {      /* ST(i): logical order */
                    uint8_t b[10];
                    x87_ld_to80(c->fst[x87_phys(c, i)], b);
                    for (int j = 0; j < 10; j++)
                        write_mem_v(m, addr + 32 + (uint64_t)i * 16 + (uint64_t)j, 1, b[j]);
                    for (int j = 10; j < 16; j++)
                        write_mem_v(m, addr + 32 + (uint64_t)i * 16 + (uint64_t)j, 1, 0);
                }
                for (int off = 160; off < 512; off += 4)
                    write_mem_v(m, addr + (uint64_t)off, 4, 0);
                for (int i = 0; i < 16; i++) {
                    write_mem_v(m, addr + 160 + (uint64_t)i * 16,     8, c->xmm[i][0]);
                    write_mem_v(m, addr + 160 + (uint64_t)i * 16 + 8, 8, c->xmm[i][1]);
                }
                break; }
            { /* ext == 1: FXRSTOR -- the live x87 block, MXCSR (masked
                 like the hardware does) and the live XMM file (K4). */
                c->fcw = (uint16_t)read_mem_v(m, addr +  0, 2);
                c->fsw = (uint16_t)read_mem_v(m, addr +  2, 2);
                c->ftop = (c->fsw >> 11) & 7;
                c->fop = (uint16_t)read_mem_v(m, addr +  6, 2);
                c->fip = read_mem_v(m, addr +  8, 4);
                c->fcs = (uint16_t)read_mem_v(m, addr + 12, 2);
                c->fdp = read_mem_v(m, addr + 16, 4);
                c->fds = (uint16_t)read_mem_v(m, addr + 20, 2);
                {   uint8_t aw = (uint8_t)read_mem_v(m, addr + 4, 1);
                    for (int i = 0; i < 8; i++) {
                        uint8_t b[10];
                        for (int j = 0; j < 10; j++)
                            b[j] = (uint8_t)read_mem_v(m, addr + 32 + (uint64_t)i * 16 + (uint64_t)j, 1);
                        int phys = x87_phys(c, i);
                        x87_w(c, phys, x87_ld_from80(b));
                        x87_set_tag(c, phys, (aw & (1u << phys))
                                    ? x87_class(c->fst[phys]) : X87_T_EMPTY);
                    }
                }
                uint32_t nx = (uint32_t)read_mem_v(m, addr + 24, 4);
                c->mxcsr = (c->mxcsr & ~0x0000FFFFu) | (nx & 0x0000FFFFu);
                for (int i = 0; i < 16; i++) {
                    c->xmm[i][0] = read_mem_v(m, addr + 160 + (uint64_t)i * 16,     8);
                    c->xmm[i][1] = read_mem_v(m, addr + 160 + (uint64_t)i * 16 + 8, 8);
                }
                break; } }
        case 0x30: { /* CF4: WRMSR is CPL0-only (#GP(0)), twin of the
                      * MOV CRn gate; unchecked before, census-clean. */
                     if (cpl_now(c)) { c->rip = d.start_pc; raise_exception(m, 13, 1, 0); break; }
                     uint32_t idx=(uint32_t)c->gpr[RCX]; uint64_t val=((uint64_t)(uint32_t)c->gpr[RDX]<<32)|(uint32_t)c->gpr[RAX];
                     /* UPKEEP U4: IA32_APIC_BASE belongs to the LAPIC model
                      * (validation -> BSP preservation -> window sync);
                      * a rejected write stores nothing and raises no #GP
                      * -- unknown MSR *indices* stay permissive (the CF4
                      * NO-GO record at the RDMSR case), so the rejection
                      * is logged instead. */
                     if (idx==LAPIC_MSR_APICBASE) lapic_apicbase_write(m, c, val);
                     else if (idx==0xC0000080) {
                         /* CF4: EFER is the one MSR whose *bits* are
                          * strict, because they steer the walk (LME/NXE)
                          * and the mode itself. Defined write bits are
                          * only those with advertised CPUID twins --
                          * SCE(0), LME(8), NXE(11) -- plus LMA(10), a
                          * read-only output: it is weighed for the
                          * reserved check, then overridden by the live
                          * value, so the canonical RDMSR->OR->WRMSR can
                          * neither #GP nor desync the mode. Anything else
                          * (SVME/FFXSR/TCE-class bits, high garbage) is
                          * #GP(0). Census: every lane writes inside
                          * {SCE,LME,LMA,NXE}. Table copy and c->efer get
                          * the same value -- one EFER, never two. */
                         if (val & ~0xD01ull) { c->rip = d.start_pc; raise_exception(m, 13, 1, 0); break; }
                         uint64_t nv = (val & ~(1ULL<<10)) | (c->efer & (1ULL<<10));
                         cpu_set_msr(c, idx, nv); c->efer = nv; perf_tlb_flush(m);
                     }
                     else {
                         cpu_set_msr(c, idx, val);
                         /* CF4: the 65th distinct index used to vanish
                          * without a trace. The table keeps its 64 slots
                          * (snapshot layout; census counts zero drops in
                          * every lane), but a lost write is now said out
                          * loud. */
                         int f2; (void)cpu_get_msr(c, idx, &f2);
                         if (!f2) mlog(&m->log, "[cpu] WRMSR %08x dropped: sparse MSR table full (%d slots)", idx, MAX_MSR);
                         /* K3/K4: FS/GS/KGS bases ride this lane -- keep
                          * the decoder's seg[] shadows in sync (AuraLite
                          * wires per-CPU through IA32_GS_BASE and TLS
                          * through IA32_FS_BASE). */
                         if (idx==0xC0000100) c->seg[SEG_FS].base = val;
                         else if (idx==0xC0000101) c->seg[SEG_GS].base = val;
                         else if (idx==0xC0000102) c->kgs_base = val;
                     }
                     break; }
        case 0x32: { /* CF4: CPL0-only (#GP(0)), twin of the WRMSR gate.
                      * Unknown indices keep reading 0 with no fault --
                      * this phase's measured NO-GO: the whitelist+#GP
                      * option was census-tested, and the sample
                      * firmware's reset-stage CAR code RDMSRs
                      * IA32_MTRR_DEF_TYPE (2FFh) unconditionally before
                      * any IDT exists (then RMWs it and the variable
                      * MTRRs through this very table); a #GP there is a
                      * bricked primary workload, and every other
                      * consumer probes by CPUID before touching an MSR.
                      * Full record: CPU_FEATURES_PLAN CF4. */
                     if (cpl_now(c)) { c->rip = d.start_pc; raise_exception(m, 13, 1, 0); break; }
                     uint32_t idx=(uint32_t)c->gpr[RCX]; int found; uint64_t val=cpu_get_msr(c, idx, &found);
                     if (idx==0xC0000080) val = c->efer;
                     if (!found && idx!=0xC0000080) val = 0;
                     c->gpr[RAX] = (uint32_t)val; c->gpr[RDX] = (uint32_t)(val>>32);
                     break; }
        case 0x20: { /* CPU_FEATURES CF4: MOV to/from CRn is CPL0-only
                      * on silicon (#GP(0)); neither direction had a CPL
                      * check at all before this phase -- found while
                      * censusing this site for the reserved-bit masks,
                      * and census-clean in every lane. (The RDMSR/WRMSR
                      * gate landed at their cases for the same reason.) */
                     if (cpl_now(c)) { c->rip = d.start_pc; raise_exception(m, 13, 1, 0); break; }
                     rm_t rm=MODRM(); int crn=rm.reg_field&7; uint64_t v = crn==0?c->cr0:crn==2?c->cr2:crn==3?c->cr3:crn==4?c->cr4:0;
                     /* MOV r32/64, CRn is always register-to-register (mod==3 enforced by spec) */
                     set_reg(c, rm.reg, (default_as==64)?8:4, 1, v); break; }
        case 0x22: { if (cpl_now(c)) { c->rip = d.start_pc; raise_exception(m, 13, 1, 0); break; }
                     rm_t rm=MODRM(); int crn=rm.reg_field&7;
                     /* MOV CRn, reg reads the architected width: 64 bits in
                      * long mode, 32 elsewhere (the baseline always read 8
                      * bytes, C0 ledger #10). */
                     uint64_t v = rm_read(&d,&rm,(default_as==64)?8:4) & ((default_as==64)?~0ull:0xFFFFFFFFull);
                     if (crn==0) {
                                   /* CF4 (SDM MOV-CR; every clause was
                                    * census-measured at zero across
                                    * firmware/SeaBIOS/suites before being
                                    * enforced): CR0[63:32] are reserved,
                                    * PG requires PE, NW requires CD, and
                                    * PG cannot be cleared while this
                                    * sub-mode is 64-bit (64-bit list) or
                                    * under PCIDE in compat mode (the CF3
                                    * deferral). Low reserved bits stay
                                    * store-verbatim: silicon "ignores"
                                    * them and nothing here consumes them. */
                                   if ((v >> 32) ||
                                       (((v>>31)&1) && !(v&1)) ||
                                       (((v>>29)&1) && !((v>>30)&1)) ||
                                       (!((v>>31)&1) && ((c->cr0>>31)&1) &&
                                        (default_as==64 || ((c->cr4>>17)&1))))
                                       { c->rip = d.start_pc; raise_exception(m, 13, 1, 0); break; }
                                   int entering_pg = ((v>>31)&1) && !((c->cr0>>31)&1); c->cr0=v;
                                   if (entering_pg && (c->efer & (1ULL<<8))) c->efer |= (1ULL<<10); /* set LMA */
                                   if (!((v>>31)&1)) c->efer &= ~(1ULL<<10);
                     }
                     else if (crn==2) c->cr2=v;
                     else if (crn==3) {
                         /* CPU_FEATURES CF3 (SDM MOV-CR description): with
                          * CR4.PCIDE=1, bit 63 of the source is the
                          * no-invalidate hint -- weighed, never stored
                          * (MOV-from-CR3 reads it back 0). With PCIDE=0
                          * the same bit sits in the CR3[63:MAXPHYADDR]
                          * reserved band -> #GP(0). The perf flush below
                          * runs either way: this TLB is flush-always (P2)
                          * and the architecture forbids demanding
                          * retention -- the hint only permits it. */
                         if ((v >> 63) & 1) {
                             if (!((c->cr4 >> 17) & 1)) { c->rip = d.start_pc; raise_exception(m, 13, 1, 0); break; }
                             v &= ~(1ULL << 63);
                         }
                         /* CF4: the rest of the reserved band. This
                          * machine advertises a 40-bit physical address
                          * (80000008h), so CR3[62:40] set -> #GP(0),
                          * completing the SDM CR3[63:MAXPHYADDR] rule
                          * (bit 63 handled above). Census-zero. */
                         if (v & 0x7FFFFF0000000000ull) { c->rip = d.start_pc; raise_exception(m, 13, 1, 0); break; }
                         c->cr3 = v; /* PCID rides [11:0]; the walk masks it */
                     }
                     else if (crn==4) {
                         /* CPU_FEATURES CF3 (SDM MOV-CR exceptions):
                          * CR4.PCIDE may hold 1 only in IA-32e mode
                          * (EFER.LMA) -- outside it even writing 1 is
                          * #GP(0) -- and the 0->1 edge additionally
                          * requires CR3[11:0]==000H (those bits become
                          * the current PCID at the flip). */
                         /* CF4: the reserved mask -- QEMU's rule with
                          * our advertisement: the structural base set
                          * (VME..OSXMMEXCPT, bits 0-10, allowed whether
                          * or not a feature bit names them) plus exactly
                          * the gated bits whose CPUID twins we set:
                          * PCIDE(17), SMEP(20), SMAP(21). LA57, OSXSAVE,
                          * FSGSBASE, PKE, VMXE/SMXE, bits 15/19 and
                          * everything from 22 up are reserved here
                          * because we do not advertise them; storage
                          * must not pretend otherwise. The CF3 PCIDE
                          * edges above still apply within the mask.
                          * Census: no lane writes outside it. */
                         if (v & ~0x3207FFull) { c->rip = d.start_pc; raise_exception(m, 13, 1, 0); break; }
                         if (((v >> 17) & 1) && !(c->efer & (1ULL << 10))) { c->rip = d.start_pc; raise_exception(m, 13, 1, 0); break; }
                         if (((v >> 17) & 1) && !((c->cr4 >> 17) & 1) && (c->cr3 & 0xFFF)) { c->rip = d.start_pc; raise_exception(m, 13, 1, 0); break; }
                         c->cr4 = v;
                     }
                     if (crn==0 || crn==3 || crn==4) perf_tlb_flush(m);
                     break; }
        case 0x00: { uint8_t modrm = fetch8(&d); int ext=(modrm>>3)&7;
                     rm_t rm = decode_modrm(&d, modrm); fixup_riprel(&d,&rm);
                     if (ext==3) {          /* LTR r/m16 (H6): cache TR, set busy */
                         uint16_t sel = (uint16_t)rm_read(&d,&rm,2) & ~7u;
                         /* K2: GDT reads/writes are linear-addressed */
                         /* K4: descriptor fetch + the busy-bit store are
                          * CPU-internal supervisor accesses (desc_sv) */
                         m->cpu.desc_sv++;
                         uint64_t lo = read_mem_v(m, c->gdtr_base + sel, 8);
                         uint64_t hi32 = read_mem_v(m, c->gdtr_base + sel + 8, 4);
                         c->tr_base = ((lo >> 16) & 0xFFFFFFULL) |
                                      (((lo >> 56) & 0xFFULL) << 24) | (hi32 << 32);
                         c->tr_limit = (uint16_t)(lo & 0xFFFF);
                         write_mem_v(m, c->gdtr_base + sel + 5, 1,
                                   (uint8_t)((((lo >> 40) & 0xFFu)) | 0x2)); /* busy */
                         m->cpu.desc_sv--;
                     } else if (ext==1) {   /* STR: single fixed TSS, raw selector stored as 0 */
                         rm_write(&d,&rm,2,0);
                     } else {
                         faultf(c, "#UD unsupported 0F opcode 0x0F 0x00 /%d", ext);
                     }
                     break; }
        case 0x01: { uint8_t modrm = fetch8(&d); int ext=(modrm>>3)&7;
                     /* CPU_FEATURES CF1: STAC/CLAC are the SMAP door
                      * (K4), made honest on both sides: (a) they now
                      * END the case -- pre-CF1 execution fell through
                      * into decode_modrm (register form: addr=0) and
                      * the ext==1 SIDT store at linear address 0, so
                      * every STAC/CLAC in history would have
                      * overwritten the guest's first bytes with the
                      * IDT pseudo-descriptor (never fired in a
                      * measured lane: guests gate STAC on the SMAP
                      * CPUID bit we had never advertised);
                      * (b) CPL>0 takes the architected #UD, delivered
                      * like the other core exceptions (the WBINVD
                      * #GP(0) site is the local idiom). The CPUID
                      * prong of the #UD is satisfied by construction:
                      * the SMAP leaf-7 bit is unconditionally on. */
                     if (modrm == 0xCB) { /* STAC: SMAP door open */
                         if (cpl_now(c) != 0) raise_exception(m, 6, 0, 0);
                         else c->rflags |= (1u<<18);
                         break;
                     } else if (modrm == 0xCA) { /* CLAC: door shut */
                         if (cpl_now(c) != 0) raise_exception(m, 6, 0, 0);
                         else c->rflags &= ~(1u<<18);
                         break;
                     } else if (modrm == 0xF9) { /* RDTSCP (CF4, D6): before
                         this phase 0F 01 F9 decoded into the ext==7 arm
                         below and silently executed as an INVLPG flush --
                         worse than absent. Semantics twin RDTSC's
                         exactly (same deterministic vtime formula, and
                         neither carries a CR4.TSD gate in this core);
                         ECX takes IA32_TSC_AUX, plain sparse storage
                         that reads 0 until a guest WRMSRs it. */
                         uint64_t t = m->vtime_instr * (uint64_t)m->plat->tsc_per_instr;
                         int f9; uint64_t aux = cpu_get_msr(c, 0xC0000103, &f9); (void)f9;
                         c->gpr[RAX] = (uint32_t)t; c->gpr[RDX] = (uint32_t)(t >> 32);
                         c->gpr[RCX] = (uint32_t)aux;
                         break;
                     } else if (modrm == 0xF8) { /* SWAPGS (0F 01 /7 mod=3 exact):
                         exchanges GS base <-> IA32_KERNEL_GS_BASE. The
                         AuraLite syscall/ISR stubs gate it by frame CS RPL,
                         so kernel-mode entries never execute it (K4 brings
                         the ring-3 rows); keep the long-mode-only guard
                         honest. */
                         if (default_as != 64) { faultf(c, "#UD SWAPGS requires 64-bit mode (K3)"); break; }
                         uint64_t t = c->seg[SEG_GS].base;
                         c->seg[SEG_GS].base = c->kgs_base; c->kgs_base = t;
                         cpu_set_msr(c, 0xC0000101, c->seg[SEG_GS].base);
                         cpu_set_msr(c, 0xC0000102, c->kgs_base);
                         break; }
                     rm_t rm = decode_modrm(&d, modrm); fixup_riprel(&d,&rm);
                     if (ext==4 || ext==6) { /* SMSW / LMSW */
                         if (ext==4) { uint16_t v = (uint16_t)(c->cr0 & 0xFFFF);
                                       if (!rm.is_mem && default_as==64) set_reg(c, rm.reg, 8, 1, v);
                                       else rm_write(&d,&rm,2,v); }
                         else { uint64_t v = rm_read(&d,&rm,2);
                                uint64_t nc = (c->cr0 & 0xFFFFFFFFFFFF0000ull) | (v & 0xFFFF);
                                if ((c->cr0 & 1) && !(v & 1)) nc |= 1; /* PE is sticky under LMSW */
                                c->cr0 = nc; perf_tlb_flush(m); }
                         break;
                     }
                     if (ext==7) { /* INVLPG: the operand is a page HINT, not
                                      a memory read -- never fault on an unmapped
                                      operand.  P2 flushes the shared TLB in
                                      O(1); a full flush is conservative but
                                      avoids mis-keying large-page aliases.
                                      K2 measured an eager read causing a
                                      recursive fault in paging_unmap().
                                      CF3: PCID narrows its architected
                                      scope to the current PCID -- the same
                                      full flush covers that too. */
                         perf_tlb_flush(m);
                         break; }
                     if (ext==5) { faultf(c, "#UD 0F 01 /5 reserved"); break; }
                     uint64_t base = (rm.seg>=0)? d.c->seg[rm.seg].base:0; uint64_t addr=base+rm_ea(&d,&rm);
                     if (ext==0 || ext==1) { /* SGDT / SIDT */
                         uint16_t lim = (ext==0) ? c->gdtr_limit : c->idtr_limit;
                         uint64_t gb  = (ext==0) ? c->gdtr_base  : c->idtr_base;
                         write_mem_v(m, addr, 2, lim);
                         /* SGDT/SIDT write a 10-byte pseudo-descriptor only
                          * in 64-bit mode. In real/protected 16- or 32-bit
                          * mode the 16-bit limit plus 32-bit base is exactly
                          * six bytes; padding a write through addr+7 can
                          * corrupt the next stack slot (SeaBIOS call32_prep
                          * places a local SGDT object directly before EBX). */
                         if (cpu_addr_size(c)==64) write_mem_v(m, addr+2, 8, gb);
                         else write_mem_v(m, addr+2, 4, gb);
                         break;
                     }
                     uint16_t lim = (uint16_t)read_mem_v(m, addr, 2);
                     uint64_t gbase = read_mem_v(m, addr+2, (cpu_addr_size(c)==64)?8:4);
                     if (ext==2) { c->gdtr_limit=lim; c->gdtr_base=gbase; }
                     else if (ext==3) { c->idtr_limit=lim; c->idtr_base=gbase; }
                     break; }
        case 0xB8: { /* POPCNT (F3 0F B8) -- CPU_FEATURES CF4 (D6):
                      * advertised only as of this phase. Bare 0F B8 is
                      * the Itanium JMPE relic, not an x86 instruction:
                      * the house #UD. Flags: ZF by (count==0), the
                      * arithmetic five all cleared (SDM definition,
                      * not the BSF-style preserve). */
            if (d.rep != 0xF3) { faultf(c, "#UD 0F B8 without F3 (JMPE is not x86)"); break; }
            rm_t rm=MODRM(); FIXUP(rm);
            uint64_t src = rm_read(&d,&rm,osz);
            unsigned n = (unsigned)__builtin_popcountll(src);
            set_reg(c, rm.reg_field, osz, d.has_rex, (uint64_t)n);
            c->rflags &= ~(FLAG_CF | FLAG_PF | FLAG_AF | FLAG_SF | FLAG_OF);
            if (n) c->rflags &= ~(uint64_t)FLAG_ZF; else c->rflags |= FLAG_ZF;
            break; }
        case 0xBC: case 0xBD: { /* BSF / BSR -- bit scans in either
             * direction. src==0: ZF=1 and the destination is PRESERVED:
             * the SDM calls it "undefined", but both host vendors keep the
             * old register and the C11 differential fuzzer runs against
             * host silicon. Other flags are likewise left alone. K3:
             * measured in the AuraLite tmpfs self-test (0F BC at +426M). */
            rm_t rm=MODRM(); FIXUP(rm);
            uint64_t src = rm_read(&d,&rm,osz);
            if (!src) { c->rflags |= FLAG_ZF; break; }
            c->rflags &= ~(uint64_t)FLAG_ZF;
            int idx = 0; uint64_t t = src;
            if (op2 == 0xBC) { while (!(t & 1)) { t >>= 1; idx++; } }
            else             { while (t >>= 1) idx++; }
            set_reg(c, rm.reg_field, osz, d.has_rex, (uint64_t)idx);
            break; }
        case 0x80: case 0x81: case 0x82: case 0x83:
        case 0x84: case 0x85: case 0x86: case 0x87: case 0x88: case 0x89: case 0x8A: case 0x8B:
        case 0x8C: case 0x8D: case 0x8E: case 0x8F: { /* Jcc near rel16/rel32 */
            /* 0F 80-83 previously fell through to the #UD default, and the old
             * condition switch had no 0x8-0xB branches (near JS/JNS/JP/JPE/JPO
             * decoded but never branched). Honour operand size: rel16 in
             * 16-bit mode, rel32 otherwise. */
            int32_t rel = (osz==2) ? (int16_t)fetch16(&d) : fetch32s(&d);
            if (eval_cc(op2 & 0xF, c->rflags)) d.pc = (uint64_t)((int64_t)d.pc + rel);
            break; }
        case 0x38: { /* three-byte escape. Only 66 0F 38 82 (INVPCID)
                      * is decoded; the rest of the map keeps the house
                      * unsupported-opcode halt. */
            uint8_t op3 = fetch8(&d);
            if (c->fault || c->exception_taken) goto done;   /* K4 fetch abort */
            if (op3 != 0x82) { faultf(c, "#UD unsupported 0F 38 opcode 0x%02x at rip=%08llx", op3, (unsigned long long)d.start_pc); break; }
            /* CPU_FEATURES CF3: INVPCID r64/r32, m128. The P2 TLB is
             * flush-always and RAM-store-generation-keyed, so no type
             * can retain anything a narrower invalidation would --
             * and the SDM text for every type explicitly permits
             * invalidating more. The complete honest model here is
             * therefore the legality surface + one conservative
             * flush (the INVLPG one). 66 is opcode-defining and is
             * sensed exactly as at the MOVDQA site (d.opsize==2; in
             * a 16-bit-default mode 66 lands on 4) -- REX.W would
             * clobber opsize to 8 and is rejected by the same test,
             * matching the SSE family's treatment; real encoders
             * never emit it (the operand width comes from the mode:
             * 64-bit in 64-bit mode, 32 otherwise regardless of
             * CS.D). LOCK, swallowed decoder-wide since forever,
             * stays swallowed (the STAC precedent). */
            if (!(d.opsize == 2 || (default_as == 16 && d.opsize == 4))) { c->rip = d.start_pc; raise_exception(m, 6, 0, 0); break; }
            rm_t rm = MODRM(); FIXUP(rm);
            if (!rm.is_mem) { c->rip = d.start_pc; raise_exception(m, 6, 0, 0); break; } /* m128 operand only */
            if (cpl_now(c) != 0) { c->rip = d.start_pc; raise_exception(m, 13, 1, 0); break; } /* CPL0 only */
            uint64_t itype = get_reg(c, rm.reg_field, (default_as == 64) ? 8 : 4, d.has_rex);
            if (itype > 3) { c->rip = d.start_pc; raise_exception(m, 13, 1, 0); break; }
            uint64_t ea = (rm.seg >= 0 ? c->seg[rm.seg].base : 0) + rm_ea(&d, &rm);
            uint64_t dlo = read_mem_v(m, ea, 8);        /* a faulting descriptor read is the architected #PF */
            uint64_t dhi = read_mem_v(m, ea + 8, 8);
            if (dlo & ~0xFFFull) { c->rip = d.start_pc; raise_exception(m, 13, 1, 0); break; } /* DESC[63:12] reserved */
            if (!((c->cr4 >> 17) & 1) && itype < 2 && (dlo & 0xFFF)) { c->rip = d.start_pc; raise_exception(m, 13, 1, 0); break; } /* PCIDE=0: types 0/1 name PCID 000H only */
            if (itype == 0 && (c->efer & (1ULL << 10)) &&
                (uint64_t)((int64_t)(dhi << 16) >> 16) != dhi) { c->rip = d.start_pc; raise_exception(m, 13, 1, 0); break; } /* type 0 address must be canonical (48-bit VA) */
            perf_tlb_flush(m);
            break; }
        default:
            faultf(c, "#UD unsupported 0F opcode 0x0F 0x%02x at rip=%08llx", op2, (unsigned long long)d.start_pc);
            break;
        }
        break; }

    /* ---- INC/DEC (32/16-bit short forms; invalid encoding space in 64-bit mode) ---- */
    case 0x40: case 0x41: case 0x42: case 0x43: case 0x44: case 0x45: case 0x46: case 0x47: {
        int r=op-0x40; uint64_t a=get_reg(c,r,osz,0), res=a+1; set_reg(c,r,osz,0,res);
        uint64_t save_cf=c->rflags&FLAG_CF; set_flags_add(c,a,1,0,res,osz); c->rflags=(c->rflags&~FLAG_CF)|save_cf; break; }
    case 0x48: case 0x49: case 0x4A: case 0x4B: case 0x4C: case 0x4D: case 0x4E: case 0x4F: {
        int r=op-0x48; uint64_t a=get_reg(c,r,osz,0), res=a-1; set_reg(c,r,osz,0,res);
        uint64_t save_cf=c->rflags&FLAG_CF; set_flags_sub(c,a,1,0,res,osz); c->rflags=(c->rflags&~FLAG_CF)|save_cf; break; }

    /* ---- PUSH/POP reg ---- */
    case 0x50: case 0x51: case 0x52: case 0x53: case 0x54: case 0x55: case 0x56: case 0x57: {
        int r=(op-0x50)|(d.has_rex?(d.rex_b<<3):0); int sz = stack_unit(&d);
        do_push(&d, sz, get_reg(c,r,sz,d.has_rex)); break; }
    case 0x58: case 0x59: case 0x5A: case 0x5B: case 0x5C: case 0x5D: case 0x5E: case 0x5F: {
        int r=(op-0x58)|(d.has_rex?(d.rex_b<<3):0); int sz = stack_unit(&d);
        set_reg(c,r,sz,d.has_rex, do_pop(&d, sz)); break; }
    case 0x60: { /* PUSHA/PUSHAD: legacy only; saved SP/ESP is pre-decrement */
        if (cpu_addr_size(c)==64) { faultf(c, "#UD PUSHA in 64-bit mode"); break; }
        int sz = stack_unit(&d);
        static const int regs[] = { RAX, RCX, RDX, RBX, RSP, RBP, RSI, RDI };
        uint64_t original_sp = get_reg(c, RSP, sz, 0);
        for (size_t i=0; i<sizeof(regs)/sizeof(regs[0]); i++) {
            uint64_t value = regs[i]==RSP ? original_sp : get_reg(c, regs[i], sz, 0);
            do_push(&d, sz, value);
        }
        break; }
    case 0x61: { /* POPA/POPAD discards the saved SP/ESP slot */
        if (cpu_addr_size(c)==64) { faultf(c, "#UD POPA in 64-bit mode"); break; }
        int sz = stack_unit(&d);
        static const int regs[] = { RDI, RSI, RBP, RSP, RBX, RDX, RCX, RAX };
        for (size_t i=0; i<sizeof(regs)/sizeof(regs[0]); i++) {
            uint64_t value = do_pop(&d, sz);
            if (regs[i] != RSP) set_reg(c, regs[i], sz, 0, value);
        }
        break; }
    /* PUSH imm: the immediate width follows the operand size (imm16 when
     * osz==2 — the baseline always fetched imm32 and decoded 2 bytes past
     * the instruction, C0 ledger #8), then sign-extends into the stack unit. */
    case 0x68: { int psz = stack_unit(&d);
        int64_t imm = (d.opsize==2) ? (int16_t)fetch16(&d) : fetch32s(&d);
        do_push(&d, psz, (uint64_t)imm); break; }
    case 0x6A: { int psz = stack_unit(&d);
        uint64_t imm=(uint64_t)(int64_t)fetch8s(&d); do_push(&d,psz,imm); break; }

    /* ---- IMUL r, r/m, imm (three-operand) -- caught missing by the C10
       table suite; same CF/OF semantics as the two-operand form. ---- */
    case 0xD8: case 0xD9: case 0xDA: case 0xDB:
    case 0xDC: case 0xDD: case 0xDE: case 0xDF: {
        /* x87 escapes (F1): one decoder owns the reg/mem split -- the
         * pre-F1 D9 path decoded register forms as memory and FLDCW'ed AX
         * into the control word on FLD1 (F0 receipt defect D1). Data and
         * control live in x87_exec; D8/DA/DC/DE stay the deliberate F2
         * arithmetic boundary. */
        x87_exec(m, c, &d, op);
        break; }
    case 0x63: { /* MOVSXD r64, r/m32 in long mode (REX.W); without REX.W it
                    degrades to a 32-bit (osz4) or 16-bit (osz2) plain move
                    per the Intel table. In legacy modes the same byte is
                    ARPL -- deliberately out of scope (firmware never uses it;
                    a #UD row pins the documented boundary). K2: measured at
                    the AuraLite kernel's post-banner diagnostics path. */
        if (default_as != 64) { faultf(c, "#UD ARPL out of scope (legacy modes)"); break; }
        rm_t rm=MODRM(); FIXUP(rm);
        if (osz==8) { uint64_t v = (uint64_t)(int64_t)(int32_t)rm_read(&d,&rm,4);
                      set_reg(c, rm.reg_field, 8, d.has_rex, v); }
        else if (osz==4) { set_reg(c, rm.reg_field, 4, d.has_rex, rm_read(&d,&rm,4)); }
        else             { set_reg(c, rm.reg_field, 2, d.has_rex, rm_read(&d,&rm,2)); }
        break; }
    case 0x6B: case 0x69: {
        rm_t rm=MODRM(); FIXUP(rm);
        /* K2 lazy rip-rel: the immediate must be in d->pc before any rm
         * use (rm_ea anchors at instruction end) -> fetch order swapped. */
        int64_t imm = (op==0x6B) ? (int64_t)fetch8s(&d)
                      : (d.opsize==2) ? (int16_t)fetch16(&d) : fetch32s(&d);
        uint64_t b = rm_read(&d,&rm,osz);
        uint64_t res; int fits;
        if (osz==2)      { int32_t p = (int32_t)((int16_t)b * (int16_t)imm); res = (uint16_t)p; fits = (p == (int32_t)(int16_t)p); }
        else if (osz==4) { int64_t p = (int64_t)(int32_t)b * (int32_t)imm;   res = (uint32_t)p; fits = (p == (int64_t)(int32_t)p); }
        else             { __int128 p = (__int128)(int64_t)b * imm;          res = (uint64_t)p; fits = (p == (__int128)(int64_t)p); }
        set_reg(c, rm.reg_field, osz, d.has_rex, res);
        c->rflags &= ~(FLAG_CF|FLAG_OF);
        if (!fits) c->rflags |= FLAG_CF|FLAG_OF;
        break; }

    /* ---- shift/rotate group2: ROL/ROR/RCL/RCR/SHL/SHR/SAR, all forms ----
     * Baseline defects fixed here (C0 ledger #4-#6): the 8-bit forms
     * C0/D0/D2 raised #UD; /0-/3 (ROL/ROR/RCL/RCR) silently returned the
     * operand unchanged; the count was masked to 31 even for 64-bit
     * operands; CF/OF-where-defined semantics were absent. Counts >= the
     * operand width are "undefined" in the manual; we implement the
     * deterministic behaviour real silicon exhibits and lock it with tests. */
    case 0xC0: case 0xC1: case 0xD0: case 0xD1: case 0xD2: case 0xD3: {
        int sz = (op & 1) ? osz : 1;            /* even opcode = 8-bit, odd = full operand size */
        rm_t rm=MODRM(); FIXUP(rm); int ext=rm.reg_field&7;
        uint8_t raw = (op==0xC0||op==0xC1) ? fetch8(&d) : (op==0xD0||op==0xD1) ? 1 : (uint8_t)c->gpr[RCX];
        int bits = sz*8;
        uint8_t cnt = raw & (sz==8 ? 63 : 31);  /* hardware masks the count to 5/6 bits */
        uint64_t mask = reg_mask(sz);
        uint64_t v = rm_read(&d,&rm,sz) & mask;
        uint64_t res = v;
        switch (ext) {
        case 0: case 1: { /* ROL / ROR: rotates; SZP untouched; CF = wrapped bit */
            int n = cnt % bits;
            if (n) res = (ext==0) ? (((v << n) | (v >> (bits - n))) & mask)
                                  : (((v >> n) | (v << (bits - n))) & mask);
            if (cnt) {
                if (ext==0) { if (res & 1) c->rflags |= FLAG_CF; else c->rflags &= ~FLAG_CF; }
                else        { if ((res >> (bits-1)) & 1) c->rflags |= FLAG_CF; else c->rflags &= ~FLAG_CF; }
                if (cnt == 1) {
                    int a = (int)((res >> (bits-1)) & 1);
                    int b = (ext==0) ? (int)(res & 1) : (int)((res >> (bits-2)) & 1);
                    if (a ^ b) c->rflags |= FLAG_OF; else c->rflags &= ~FLAG_OF;
                }
            }
            break; }
        case 2: case 3: { /* RCL / RCR through CF: exact (bits+1)-bit rotation on a 128-bit scaffold */
            int total = bits + 1;
            int n = cnt % total;
            if (cnt && n) {
                __uint128_t wide = (__uint128_t)v | ((c->rflags & FLAG_CF) ? ((__uint128_t)1 << bits) : 0);
                __uint128_t wmask = (((__uint128_t)1) << total) - 1;
                if (ext==2) wide = ((wide << n) | (wide >> (total - n))) & wmask;
                else        wide = ((wide >> n) | (wide << (total - n))) & wmask;
                res = (uint64_t)wide & mask;
                if ((wide >> bits) & 1) c->rflags |= FLAG_CF; else c->rflags &= ~FLAG_CF;
                if (cnt == 1) {
                    int msb = (int)((res >> (bits-1)) & 1);
                    int other = (ext==2) ? (int)((c->rflags & FLAG_CF) ? 1 : 0)
                                         : (int)((res >> (bits-2)) & 1);
                    if (msb ^ other) c->rflags |= FLAG_OF; else c->rflags &= ~FLAG_OF;
                }
            }
            break; }
        case 4: case 6: /* SHL (SAL aliases SHL) */
        case 5: case 7: { /* SHR / SAR */
            if (cnt) {
                if (ext != 7) {
                    res = (cnt < bits) ? ((ext & 1) ? v >> cnt : (v << cnt) & mask) : 0;
                    set_flags_logic(c, res, sz);
                    uint64_t lastbit = 0;
                    if (ext == 4 || ext == 6) lastbit = (cnt <= bits) ? ((v >> (bits - cnt)) & 1) : 0;
                    else                      lastbit = (cnt <= bits) ? ((v >> (cnt - 1)) & 1) : 0;
                    if (lastbit) c->rflags |= FLAG_CF; else c->rflags &= ~FLAG_CF;
                    if (cnt == 1) {
                        if (ext == 5) { /* SHR: OF = original MSB */
                            if ((v >> (bits-1)) & 1) c->rflags |= FLAG_OF; else c->rflags &= ~FLAG_OF;
                        } else { /* SHL: OF = MSB(result) ^ CF */
                            if (((res >> (bits-1)) & 1) ^ lastbit) c->rflags |= FLAG_OF; else c->rflags &= ~FLAG_OF;
                        }
                    }
                } else { /* SAR: sign-extended, OF cleared on count==1 */
                    int64_t sv = (int64_t)(v << (64 - bits)) >> (64 - bits);
                    int sh = (cnt >= bits) ? bits - 1 : cnt;
                    res = (uint64_t)(sv >> sh) & mask;
                    set_flags_logic(c, res, sz);
                    int cb = (cnt - 1) >= bits ? bits - 1 : cnt - 1;
                    if ((v >> cb) & 1) c->rflags |= FLAG_CF; else c->rflags &= ~FLAG_CF;
                    if (cnt == 1) c->rflags &= ~FLAG_OF;
                }
            }
            break; }
        }
        rm_write(&d,&rm,sz,res);
        break; }

    /* ---- group3 (TEST/NOT/NEG/MUL/IMUL/DIV/IDIV) ----
     * Baseline defect (C0 ledger #7): /4-/7 raised "#UD group3 ext=N".
     * Multiply results land in rDX:rAX; CF/OF set iff the upper half is
     * non-zero (MUL) or not the sign-extension of the lower half (IMUL).
     * Division raises #DE (vector 0) on divide-by-zero and quotient
     * overflow; the INT_MIN/-1 C UB cases are guarded BEFORE dividing.
     * Flags other than CF/OF after MUL/IMUL and all flags after DIV are
     * architecturally undefined and are left untouched (deterministic). */
    case 0xF6: case 0xF7: {
        int size = (op==0xF6)?1:osz; rm_t rm=MODRM(); FIXUP(rm); int ext=rm.reg_field&7;
        if (ext==0 || ext==1) { uint64_t imm = (size==1)? fetch8(&d) : fetch_imm(&d, size==2?2:4);
            set_flags_logic(c, rm_read(&d,&rm,size) & imm, size); }
        else if (ext==2) { rm_write(&d,&rm,size, ~rm_read(&d,&rm,size)); }
        else if (ext==3) { uint64_t v=rm_read(&d,&rm,size); uint64_t res=(uint64_t)(-(int64_t)v); set_flags_sub(c,0,v,0,res,size); rm_write(&d,&rm,size,res); }
        else if (ext==4 || ext==5) { /* MUL / IMUL */
            uint64_t b = rm_read(&d,&rm,size); int carry;
            if (size==1) {
                uint16_t r = (ext==4) ? (uint16_t)((uint16_t)(uint8_t)c->gpr[RAX] * (uint16_t)(uint8_t)b)
                                      : (uint16_t)((int16_t)(int8_t)c->gpr[RAX] * (int16_t)(int8_t)b);
                c->gpr[RAX] = (c->gpr[RAX] & ~0xFFFFull) | r;
                carry = (ext==4) ? (r >> 8) != 0 : ((int16_t)(int8_t)(r & 0xFF) != (int16_t)r);
            } else if (size==2) {
                uint32_t r = (ext==4) ? ((uint32_t)(uint16_t)c->gpr[RAX] * (uint16_t)b)
                                      : (uint32_t)((int32_t)(int16_t)c->gpr[RAX] * (int32_t)(int16_t)b);
                set_reg(c, RAX, 2, 0, r);
                set_reg(c, RDX, 2, 0, r >> 16);
                carry = (ext==4) ? (r >> 16) != 0 : ((int32_t)(int16_t)(r & 0xFFFF) != (int32_t)r);
            } else if (size==4) {
                uint64_t r = (ext==4) ? ((uint64_t)(uint32_t)c->gpr[RAX] * (uint32_t)b)
                                      : (uint64_t)((int64_t)(int32_t)c->gpr[RAX] * (int64_t)(int32_t)b);
                set_reg(c, RAX, 4, 0, (uint32_t)r);
                set_reg(c, RDX, 4, 0, (uint32_t)(r >> 32));
                carry = (ext==4) ? (r >> 32) != 0 : ((int64_t)(int32_t)(uint32_t)r != (int64_t)r);
            } else {
                __uint128_t r = (ext==4) ? ((__uint128_t)c->gpr[RAX] * (__uint128_t)b)
                                         : (__uint128_t)((__int128_t)c->gpr[RAX] * (__int128_t)b);
                c->gpr[RAX] = (uint64_t)r;
                c->gpr[RDX] = (uint64_t)(r >> 64);
                carry = (ext==4) ? (r >> 64) != 0
                                 : ((__int128_t)(int64_t)(uint64_t)r != (__int128_t)r);
            }
            if (carry) c->rflags |= FLAG_CF|FLAG_OF; else c->rflags &= ~(FLAG_CF|FLAG_OF);
        }
        else { /* ext==6 DIV / ext==7 IDIV */
            uint64_t dv = rm_read(&d,&rm,size) & reg_mask(size);
            if (dv == 0) { mlog(&m->log, "[cpu] #DE: division by zero at rip=%08llx", (unsigned long long)d.start_pc); raise_exception(m, 0, 0, 0); break; }
            if (size==1) {
                uint16_t dd = (uint16_t)c->gpr[RAX];
                uint16_t q, r;
                if (ext==6) { q = dd / dv; r = dd % dv; if (q > 0xFF) goto div_overflow; }
                else { int32_t sq = (int16_t)dd / (int8_t)dv, sr = (int16_t)dd % (int8_t)dv;
                       if (sq > 127 || sq < -128) goto div_overflow;
                       q = (uint16_t)(int16_t)sq; r = (uint16_t)(int16_t)sr; }
                c->gpr[RAX] = (c->gpr[RAX] & ~0xFFFFull) | (uint16_t)(((r & 0xFF) << 8) | (q & 0xFF));
            } else if (size==2) {
                uint32_t dd = ((uint32_t)(uint16_t)c->gpr[RDX] << 16) | (uint16_t)c->gpr[RAX];
                if (ext==6) { uint32_t q = dd / dv, r = dd % dv; if (q > 0xFFFF) goto div_overflow;
                              set_reg(c, RAX, 2, 0, q); set_reg(c, RDX, 2, 0, r); }
                else { int64_t sq = (int64_t)(int32_t)dd / (int64_t)(int16_t)dv, sr = (int64_t)(int32_t)dd % (int64_t)(int16_t)dv; /* 64-bit arm: INT32_MIN/-1 UB guarded */
                       if (sq > 32767 || sq < -32768) goto div_overflow;
                       set_reg(c, RAX, 2, 0, (uint16_t)(int16_t)sq); set_reg(c, RDX, 2, 0, (uint16_t)(int16_t)sr); }
            } else if (size==4) {
                uint64_t dd = ((uint64_t)(uint32_t)c->gpr[RDX] << 32) | (uint32_t)c->gpr[RAX];
                if (ext==6) { uint64_t q = dd / dv, r = dd % dv; if (q > 0xFFFFFFFFull) goto div_overflow;
                              set_reg(c, RAX, 4, 0, (uint32_t)q); set_reg(c, RDX, 4, 0, (uint32_t)r); }
                else { __int128 sq = (__int128)(int64_t)dd / (__int128)(int32_t)dv;
                       __int128 sr = (__int128)(int64_t)dd % (__int128)(int32_t)dv; /* 128-bit: INT64_MIN/-1 UB guarded */
                       if (sq > INT32_MAX || sq < INT32_MIN) goto div_overflow;
                       set_reg(c, RAX, 4, 0, (uint32_t)(int32_t)sq); set_reg(c, RDX, 4, 0, (uint32_t)(int32_t)sr); }
            } else {
                if (ext==6) { __uint128_t dd = ((__uint128_t)c->gpr[RDX] << 64) | c->gpr[RAX];
                              __uint128_t q = dd / dv, r = dd % dv; if ((q >> 64) != 0) goto div_overflow;
                              c->gpr[RAX] = (uint64_t)q; c->gpr[RDX] = (uint64_t)r; }
                else { __int128 dd = (__int128)(((__uint128_t)c->gpr[RDX] << 64) | c->gpr[RAX]);
                       if (dd == (__int128)((__uint128_t)1 << 127) && (int64_t)dv == -1) goto div_overflow; /* INT128_MIN/-1 */
                       __int128 sq = dd / (int64_t)dv, sr = dd % (int64_t)dv;
                       if (sq > INT64_MAX || sq < INT64_MIN) goto div_overflow;
                       c->gpr[RAX] = (uint64_t)(int64_t)sq; c->gpr[RDX] = (uint64_t)(int64_t)sr; }
            }
            break;
        div_overflow:
            mlog(&m->log, "[cpu] #DE: divide overflow at rip=%08llx", (unsigned long long)d.start_pc);
            raise_exception(m, 0, 0, 0);
            break;
        }
        break; }

    /* ---- group5 (INC/DEC/CALL/JMP/PUSH r/m) ---- */
    case 0xFE: { rm_t rm=MODRM(); FIXUP(rm); int ext=rm.reg_field&7; uint64_t a=rm_read(&d,&rm,1);
        if (ext==0) { uint64_t r=a+1; set_flags_add(c,a,1,0,r,1); rm_write(&d,&rm,1,r);} else { uint64_t r=a-1; set_flags_sub(c,a,1,0,r,1); rm_write(&d,&rm,1,r);} break; }
    case 0xFF: { rm_t rm=MODRM(); FIXUP(rm); int ext=rm.reg_field&7;
        if (ext==0 || ext==1) { uint64_t a=rm_read(&d,&rm,osz);
            if (ext==0) { uint64_t r=a+1; set_flags_add(c,a,1,0,r,osz); rm_write(&d,&rm,osz,r);} else { uint64_t r=a-1; set_flags_sub(c,a,1,0,r,osz); rm_write(&d,&rm,osz,r);} }
        else if (ext==2) { uint64_t target=rm_read(&d,&rm,(default_as==64)?8:osz); do_push(&d,(default_as==64)?8:osz, d.pc); d.pc=target; }
        else if (ext==4) { uint64_t target=rm_read(&d,&rm,(default_as==64)?8:osz); d.pc=target; }
        else if (ext==6) { int sz=stack_unit(&d); do_push(&d,sz, rm_read(&d,&rm,sz)); }
        else if (ext==3 || ext==5) { /* far call/jmp through memory: m16:16/32/64 */
            if (!rm.is_mem) { faultf(c, "#UD far jmp/call with register operand"); break; }
            int offsz = (osz==2)?2:(default_as==64?8:4);
            uint64_t base = (rm.seg>=0) ? d.c->seg[rm.seg].base : 0;
            uint64_t off = read_mem_v(d.m, base + rm_ea(&d,&rm), offsz);
            uint16_t sel = (uint16_t)read_mem_v(d.m, base + rm_ea(&d,&rm) + offsz, 2);
            if (ext==3) { int sz=(default_as==64)?8:osz; do_push(&d, sz, c->seg[SEG_CS].sel); do_push(&d, sz, d.pc); }
            far_load_cs(m, sel, off);
            d.pc = c->rip; /* far_load_cs sets c->rip directly; keep decode cursor in sync */
        }
        else faultf(c,"#UD group5 ext=%d", ext);
        break; }

    /* ---- JMP/CALL near rel ---- */
    case 0xE9: { int32_t rel = (osz==2)? (int16_t)fetch16(&d) : fetch32s(&d); d.pc = (uint64_t)((int64_t)d.pc + rel); break; }
    case 0xEB: { int8_t rel = fetch8s(&d); d.pc = (uint64_t)((int64_t)d.pc + rel); break; }
    case 0xE8: { int32_t rel = (osz==2)? (int16_t)fetch16(&d) : fetch32s(&d);
        int sz=(default_as==64)?8:osz; do_push(&d, sz, d.pc); d.pc = (uint64_t)((int64_t)d.pc + rel); break; }
    case 0xC3: { int sz=(default_as==64)?8:osz; d.pc = do_pop(&d, sz); break; }
    case 0xC2: { int sz=(default_as==64)?8:osz; uint64_t target=do_pop(&d,sz); uint16_t n=fetch16(&d); c->gpr[RSP]+=n; d.pc=target; break; }

    /* ---- LOOP/LOOPE/LOOPNE/JCXZ (count register follows the ADDRESS size) ---- */
    case 0xE0: case 0xE1: case 0xE2: {
        int8_t rel = fetch8s(&d);
        uint64_t n = str_ptr(c, RCX, d.addrsize) - 1;
        str_set_ptr(c, RCX, d.addrsize, n);
        int take = (n != 0);
        if (op==0xE1) take = take &&  (c->rflags & FLAG_ZF);   /* LOOPE */
        if (op==0xE0) take = take && !(c->rflags & FLAG_ZF);   /* LOOPNE */
        if (take) d.pc = (uint64_t)((int64_t)d.pc + rel);
        break; }
    case 0xE3: {
        int8_t rel = fetch8s(&d);
        if (str_ptr(c, RCX, d.addrsize) == 0) d.pc = (uint64_t)((int64_t)d.pc + rel);
        break; }

    /* ---- short Jcc (0x70-0x7F) ---- */
    case 0x70: case 0x71: case 0x72: case 0x73: case 0x74: case 0x75: case 0x76: case 0x77:
    case 0x78: case 0x79: case 0x7A: case 0x7B: case 0x7C: case 0x7D: case 0x7E: case 0x7F: {
        int8_t rel = fetch8s(&d);
        if (eval_cc(op & 0xF, c->rflags)) d.pc = (uint64_t)((int64_t)d.pc + rel);
        break; }

    /* ---- PUSHF / POPF ---- */
    case 0x9C: { int psz = stack_unit(&d); do_push(&d, psz, c->rflags | 0x2); break; }
    case 0x9D: { int psz = stack_unit(&d); uint64_t f = do_pop(&d, psz);
        c->rflags = (f & RFLAGS_WRITABLE) | 0x2; break; }

    /* ---- PUSH/POP Sreg (06/07/0E/16/17/1E/1F) -- caught missing by the C10
     * table suite ("push cs" raised #UD). There is no POP CS. ---- */
    case 0x06: case 0x0E: case 0x16: case 0x1E: {
        int s = (op==0x06)?SEG_ES : (op==0x0E)?SEG_CS : (op==0x16)?SEG_SS : SEG_DS;
        do_push(&d, stack_unit(&d), c->seg[s].sel);
        break; }
    case 0x07: case 0x17: case 0x1F: {
        int s = (op==0x07)?SEG_ES : (op==0x17)?SEG_SS : SEG_DS;
        if (s == SEG_SS) c->intr_delay = 1;
        uint16_t sel = (uint16_t)do_pop(&d, stack_unit(&d));
        if (!(c->cr0&1)) { c->seg[s].sel = sel; c->seg[s].base = (uint64_t)sel<<4; c->seg[s].limit = 0xFFFF; }
        else { desc_t dsc; read_descriptor(m, c->gdtr_base, sel, &dsc); load_seg_from_desc(c, s, sel, &dsc); }
        break; }

    /* ---- CALLF direct ptr16:16/32 (invalid in 64-bit mode) ---- */
    case 0x9A: {
        if (default_as==64) { faultf(c, "#UD callf (9A) in 64-bit mode"); break; }
        uint64_t off = (osz==2) ? fetch16(&d) : fetch32(&d);
        uint16_t sel = fetch16(&d);
        do_push(&d, osz, c->seg[SEG_CS].sel);
        do_push(&d, osz, d.pc);
        far_load_cs(m, sel, off);
        d.pc = c->rip;
        break; }

    /* ---- RETF (+imm16) ---- */
    case 0xCA: case 0xCB: {
        int ssz = stack_unit(&d);
        uint64_t off = do_pop(&d, ssz);
        uint16_t sel = (uint16_t)do_pop(&d, ssz);
        if (op==0xCA) c->gpr[RSP] += fetch16(&d);
        far_load_cs(m, sel, off);
        d.pc = c->rip;
        break; }

    /* ---- software interrupts: traps push the NEXT instruction as the
     * return RIP (that's what separates a trap from a fault) ---- */
    case 0xCC: { c->rip = d.pc; raise_exception(m, 3, 0, 0); break; }
    case 0xCD: { uint8_t vec = fetch8(&d);
        if (cpl_now(c) == 3 && c->idtr_limit != 0) {
            /* K4: the gate attribute read is an implicit (ring-0) access --
             * do it under the same suspended-CPL window raise_exception
             * uses, or a supervisor IDT page would fault the check itself. */
            uint16_t ps = c->seg[SEG_CS].sel;
            c->seg[SEG_CS].sel = (uint16_t)(ps & ~3u);
            uint8_t ga = (uint8_t)read_mem_v(m, c->idtr_base + (uint64_t)vec * 16 + 5, 1);
            c->seg[SEG_CS].sel = ps;
            if (((ga >> 5) & 3) < 3) {           /* software-int DPL check */
                c->rip = d.start_pc;
                raise_exception(m, 13, 1, (uint32_t)(vec * 8 + 2));
                break;
            }
        }
        c->rip = d.pc; raise_exception(m, vec, 0, 0); break; }
    case 0xCE: { if (c->rflags & FLAG_OF) { c->rip = d.pc; raise_exception(m, 4, 0, 0); } break; }

    /* ---- IRET/IRETQ: pops RIP, CS, RFLAGS in the order C5's frame lays
     * them down. K4: in 64-bit mode a return whose CS has a different RPL
     * is a ring switch -- RSP and SS are popped too, and the restored RSP
     * is the outer-ring one. ---- */
    case 0xCF: {
        int ssz = stack_unit(&d);
        uint64_t off = do_pop(&d, ssz);
        uint16_t sel = (uint16_t)do_pop(&d, ssz);
        uint64_t fl  = do_pop(&d, ssz);
        if (ssz == 8 && (sel & 3) != cpl_now(c)) {
            uint64_t nsp = do_pop(&d, 8);
            uint16_t nss = (uint16_t)do_pop(&d, 8);
            c->seg[SEG_SS].sel = nss;
            c->seg[SEG_SS].base = 0;
            c->gpr[RSP] = nsp;
        }
        c->rflags = (fl & RFLAGS_WRITABLE) | 0x2;
        far_load_cs(m, sel, off);
        d.pc = c->rip;
        break; }

    /* ---- far jmp direct ptr16:16/32 ---- */
    case 0xEA: {
        uint64_t off = (osz==2) ? fetch16(&d) : fetch32(&d);
        uint16_t sel = fetch16(&d);
        far_load_cs(m, sel, off);
        d.pc = c->rip; /* far_load_cs sets c->rip directly; keep decode cursor in sync */
        break; }

    /* ---- IN/OUT ---- */
    case 0xE4: { uint8_t port=fetch8(&d); set_reg(c,RAX,1,d.has_rex, io_read(m, port, 1)); break; }
    case 0xE5: { uint8_t port=fetch8(&d); int sz= osz==2?2:4; set_reg(c,RAX,sz,d.has_rex, io_read(m, port, sz)); break; }
    case 0xE6: { uint8_t port=fetch8(&d); io_write(m, port, 1, (uint32_t)get_reg(c,RAX,1,d.has_rex)); break; }
    case 0xE7: { uint8_t port=fetch8(&d); int sz=osz==2?2:4; io_write(m, port, sz, (uint32_t)get_reg(c,RAX,sz,d.has_rex)); break; }
    case 0xEC: { uint16_t port=(uint16_t)c->gpr[RDX]; set_reg(c,RAX,1,d.has_rex, io_read(m, port, 1)); break; }
    case 0xED: { uint16_t port=(uint16_t)c->gpr[RDX]; int sz=osz==2?2:4; set_reg(c,RAX,sz,d.has_rex, io_read(m, port, sz)); break; }
    case 0xEE: { uint16_t port=(uint16_t)c->gpr[RDX]; io_write(m, port, 1, (uint32_t)get_reg(c,RAX,1,d.has_rex)); break; }
    case 0xEF: { uint16_t port=(uint16_t)c->gpr[RDX]; int sz=osz==2?2:4; io_write(m, port, sz, (uint32_t)get_reg(c,RAX,sz,d.has_rex)); break; }

    /* ---- string ops (C7): MOVS/CMPS/SCAS/LODS/STOS + INS/OUTS ----
     * REP(F3) on MOVS/STOS/LODS/INS/OUTS is a plain count loop; on CMPS/SCAS,
     * REPE(F3) stops when ZF clears and REPNE(F2) stops when ZF sets. A count
     * of zero performs NO memory/IO access at all. On a mid-loop exception the
     * pointers/count already carry the in-flight progress and RIP still names
     * the instruction start, so an IRET back re-enters the loop — the same
     * contract real hardware gives. */
    case 0xA4: case 0xA5: { /* MOVS */
        int size = (op==0xA4)?1:osz;
        int src_seg = (d.seg_override>=0) ? d.seg_override : SEG_DS;
        int dir = (c->rflags & FLAG_DF) ? -1 : 1;
        uint64_t cnt = d.rep ? str_count(c,d.addrsize) : 1;
        while (cnt--) {
            uint64_t v = read_mem_v(m, c->seg[src_seg].base + str_ptr(c,RSI,d.addrsize), size);
            if (c->fault || c->exception_taken) goto done;
            write_mem_v(m, c->seg[SEG_ES].base + str_ptr(c,RDI,d.addrsize), size, v);
            if (c->fault || c->exception_taken) goto done;
            str_adv(c,RSI,d.addrsize,size,dir); str_adv(c,RDI,d.addrsize,size,dir);
            if (d.rep) str_dec_count(c,d.addrsize);
        }
        break; }
    case 0xA6: case 0xA7: { /* CMPS */
        int size = (op==0xA6)?1:osz;
        int src_seg = (d.seg_override>=0) ? d.seg_override : SEG_DS;
        int dir = (c->rflags & FLAG_DF) ? -1 : 1;
        if (!d.rep) {
            uint64_t a=read_mem_v(m, c->seg[src_seg].base + str_ptr(c,RSI,d.addrsize), size);
            if (c->fault || c->exception_taken) goto done;
            uint64_t b=read_mem_v(m, c->seg[SEG_ES].base + str_ptr(c,RDI,d.addrsize), size);
            if (c->fault || c->exception_taken) goto done;
            int wb; alu_op(c,ALU_CMP,a,b,size,&wb);
            str_adv(c,RSI,d.addrsize,size,dir); str_adv(c,RDI,d.addrsize,size,dir);
        } else {
            while (str_count(c,d.addrsize) != 0) {
                uint64_t a=read_mem_v(m, c->seg[src_seg].base + str_ptr(c,RSI,d.addrsize), size);
                if (c->fault || c->exception_taken) goto done;
                uint64_t b=read_mem_v(m, c->seg[SEG_ES].base + str_ptr(c,RDI,d.addrsize), size);
                if (c->fault || c->exception_taken) goto done;
                int wb; alu_op(c,ALU_CMP,a,b,size,&wb);
                str_adv(c,RSI,d.addrsize,size,dir); str_adv(c,RDI,d.addrsize,size,dir);
                str_dec_count(c,d.addrsize);
                if (d.rep==0xF3 && !(c->rflags & FLAG_ZF)) break;
                if (d.rep==0xF2 &&  (c->rflags & FLAG_ZF)) break;
            }
        }
        break; }
    case 0xAA: case 0xAB: { /* STOS */
        int size = (op==0xAA)?1:osz;
        int dir = (c->rflags & FLAG_DF) ? -1 : 1;
        uint64_t cnt = d.rep ? str_count(c,d.addrsize) : 1;
        while (cnt--) {
            write_mem_v(m, c->seg[SEG_ES].base + str_ptr(c,RDI,d.addrsize), size, get_reg(c,RAX,size,d.has_rex));
            if (c->fault || c->exception_taken) goto done;
            str_adv(c,RDI,d.addrsize,size,dir);
            if (d.rep) str_dec_count(c,d.addrsize);
        }
        break; }
    case 0xAC: case 0xAD: { /* LODS */
        int size = (op==0xAC)?1:osz;
        int src_seg = (d.seg_override>=0) ? d.seg_override : SEG_DS;
        int dir = (c->rflags & FLAG_DF) ? -1 : 1;
        uint64_t cnt = d.rep ? str_count(c,d.addrsize) : 1;
        while (cnt--) {
            uint64_t v = read_mem_v(m, c->seg[src_seg].base + str_ptr(c,RSI,d.addrsize), size);
            if (c->fault || c->exception_taken) goto done;
            set_reg(c,RAX,size,d.has_rex,v);
            str_adv(c,RSI,d.addrsize,size,dir);
            if (d.rep) str_dec_count(c,d.addrsize);
        }
        break; }
    case 0xAE: case 0xAF: { /* SCAS */
        int size = (op==0xAE)?1:osz;
        int dir = (c->rflags & FLAG_DF) ? -1 : 1;
        if (!d.rep) {
            uint64_t b=read_mem_v(m, c->seg[SEG_ES].base + str_ptr(c,RDI,d.addrsize), size);
            if (c->fault || c->exception_taken) goto done;
            uint64_t a=get_reg(c,RAX,size,d.has_rex);
            int wb; alu_op(c,ALU_CMP,a,b,size,&wb);
            str_adv(c,RDI,d.addrsize,size,dir);
        } else {
            while (str_count(c,d.addrsize) != 0) {
                uint64_t b=read_mem_v(m, c->seg[SEG_ES].base + str_ptr(c,RDI,d.addrsize), size);
                if (c->fault || c->exception_taken) goto done;
                uint64_t a=get_reg(c,RAX,size,d.has_rex);
                int wb; alu_op(c,ALU_CMP,a,b,size,&wb);
                str_adv(c,RDI,d.addrsize,size,dir);
                str_dec_count(c,d.addrsize);
                if (d.rep==0xF3 && !(c->rflags & FLAG_ZF)) break;
                if (d.rep==0xF2 &&  (c->rflags & FLAG_ZF)) break;
            }
        }
        break; }
    case 0x6C: case 0x6D: { /* INS: port DX -> ES:DI */
        int size = (op==0x6C)?1:(osz==2?2:4);
        int dir = (c->rflags & FLAG_DF) ? -1 : 1;
        uint16_t port = (uint16_t)c->gpr[RDX];
        uint64_t cnt = d.rep ? str_count(c,d.addrsize) : 1;
        while (cnt--) {
            uint32_t v = io_read(m, port, size);
            write_mem_v(m, c->seg[SEG_ES].base + str_ptr(c,RDI,d.addrsize), size, v);
            if (c->fault || c->exception_taken) goto done;
            str_adv(c,RDI,d.addrsize,size,dir);
            if (d.rep) str_dec_count(c,d.addrsize);
        }
        break; }
    case 0x6E: case 0x6F: { /* OUTS: DS:SI (overridable) -> port DX */
        int size = (op==0x6E)?1:(osz==2?2:4);
        int src_seg = (d.seg_override>=0) ? d.seg_override : SEG_DS;
        int dir = (c->rflags & FLAG_DF) ? -1 : 1;
        uint16_t port = (uint16_t)c->gpr[RDX];
        uint64_t cnt = d.rep ? str_count(c,d.addrsize) : 1;
        while (cnt--) {
            uint64_t v = read_mem_v(m, c->seg[src_seg].base + str_ptr(c,RSI,d.addrsize), size);
            if (c->fault || c->exception_taken) goto done;
            io_write(m, port, size, (uint32_t)v);
            str_adv(c,RSI,d.addrsize,size,dir);
            if (d.rep) str_dec_count(c,d.addrsize);
        }
        break; }

    case 0x9B: /* WAIT/FWAIT: the x87 sync point. Under CR0.TS it raises #NM
                * like any escape (F1); no pending unmasked x87 exception can
                * exist between completed ops here. */
        if (c->cr0 & 0x8u) raise_exception(m, 7, 0, 0);
        break;

    default:
        faultf(c, "#UD unsupported opcode 0x%02x at rip=%08llx (mode %s)", op, (unsigned long long)d.start_pc, cpu_mode_name(c));
        break;
    }

done:
    if (!c->fault && !c->exception_taken) c->rip = d.pc;
    c->instr_count++;
    c->executed_steps++;  /* PERF P0: executed attempts only, including faults.
                           * Idle slots do not reach done and do not count. */
    m->vtime_instr++;   /* K6: the machine-wide master clock */
    pit_tick(m);   /* CHIPSET H2: virtual-time advance; no-op until pit_init */
    rtc_tick(m);   /* CHIPSET H3: same D6 timebase; update-ended IRQ8 here */
    lapic_tick(m); /* CHIPSET H6: LAPIC timer on the same virtual TSC */
    if (c->fault) { mlog(&m->log, "[cpu] FAULT: %s", c->fault_msg); return -1; }
    if (c->halted) return (c->rflags & FLAG_IF) ? 0 : -1;   /* K3: sti;hlt stays
        alive -- the sleep is serviced by the top-of-step branch next round */
    return 0;
}
