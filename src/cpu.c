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
#include "platform.h"

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
static int dbg_u_on = -1;  /* K4: EMU_DBG_U=1 knob, resolved lazily (fetch8) */
static int dbg_u_events;
static int dbg_read_probe = 0;      /* K4: first-N read/write syscall probe */
static int syscall_ret_probe = -1;  /* id of the in-flight probed syscall */
static FILE *probe_fp;              /* /tmp/k4-probe.txt, unbuffered -- survives kills */

/* K4: raise a page fault with the architected error code.
 * mode: 0 = data read, 1 = data write, 2 = instruction fetch. */
static void page_fault(machine_t *m, uint64_t vaddr, int mode, int present_chain, int nx_hit) {
    cpu_t *c = &m->cpu;
    c->cr2 = vaddr; /* K2: the AuraLite isr14 dispatches on CR2 */
    uint32_t err = 0;
    if (present_chain) err |= 1u;             /* P: level-all-present */
    if (mode == 1) err |= 2u;                 /* W/R */
    if (cpl_now(c) == 3) err |= 4u;           /* U/S */
    if (nx_hit && (c->efer & (1ULL<<11))) err |= 16u; /* I/D (needs EFER.NXE) */
    /* K4: page faults are rare in a healthy boot but their context rip is the
     * entire delta between "mystery SIGSEGV" and a one-line verdict -- keep
     * it always-on (measured: the init-shell null-deref chased below). */
    mlog(&m->log, "[cpu] #PF ctx rip=%llx cr3=%llx cpl=%d rsp=%llx",
         (unsigned long long)m->cpu.rip, (unsigned long long)m->cpu.cr3,
         m->cpu.seg[SEG_CS].sel & 3, (unsigned long long)m->cpu.gpr[RSP]);
    mlog(&m->log, "[cpu] #PF v=0x%llx mode=%d err=0x%x", (unsigned long long)vaddr, mode, err);
    raise_exception(m, 14, 1, err);
}

/* K4: the walk enforces U/S, R/W (with CR0.WP for CPL0), NX (with EFER.NXE)
 * and the CR4.SMEP/SMAP supervisor guards. Effective U/S and R/W are the AND
 * of every level; NX is the OR. On any failure the fault is raised HERE and
 * 0 is returned -- callers must return without re-raising. */
#define T_READ 0
#define T_WRITE 1
#define T_EXEC 2
static int translate(machine_t *m, uint64_t vaddr, uint64_t *out_phys, int mode) {
    cpu_t *c = &m->cpu;
    if (!(c->cr0 & (1ULL<<31))) { *out_phys = vaddr; return 1; } /* paging disabled */
    int user_cpl = (cpl_now(c) == 3) && !c->desc_sv;   /* K4: implicit descriptor reads are supervisor */
    uint64_t cr3 = c->cr3 & ~0xFFFULL;
    int idx4 = (int)((vaddr >> 39) & 0x1FF);
    int idx3 = (int)((vaddr >> 30) & 0x1FF);
    int idx2 = (int)((vaddr >> 21) & 0x1FF);
    int idx1 = (int)((vaddr >> 12) & 0x1FF);
    int eff_us = 1, eff_w = 1, nx_any = 0;

    uint64_t pml4e = mem_read(m, cr3 + (uint64_t)idx4*8, 8);
    if (!(pml4e & 1)) { page_fault(m, vaddr, mode, 0, 0); return 0; }
    eff_us &= (int)((pml4e >> 2) & 1); eff_w &= (int)((pml4e >> 1) & 1); nx_any |= (int)(pml4e >> 63);
    uint64_t pdpt = pml4e & 0x000FFFFFFFFFF000ULL;

    uint64_t pdpte = mem_read(m, pdpt + (uint64_t)idx3*8, 8);
    if (!(pdpte & 1)) { page_fault(m, vaddr, mode, 0, 0); return 0; }
    eff_us &= (int)((pdpte >> 2) & 1); eff_w &= (int)((pdpte >> 1) & 1); nx_any |= (int)(pdpte >> 63);
    if (pdpte & (1ULL<<7)) { *out_phys = (pdpte & 0xFFFFFC0000000ULL) | (vaddr & 0x3FFFFFFFULL); goto check; }
    {
        uint64_t pd = pdpte & 0x000FFFFFFFFFF000ULL;
        uint64_t pde = mem_read(m, pd + (uint64_t)idx2*8, 8);
        if (!(pde & 1)) { page_fault(m, vaddr, mode, 0, 0); return 0; }
        eff_us &= (int)((pde >> 2) & 1); eff_w &= (int)((pde >> 1) & 1); nx_any |= (int)(pde >> 63);
        if (pde & (1ULL<<7)) { *out_phys = (pde & 0x000FFFFFFFE00000ULL) | (vaddr & 0x1FFFFFULL); goto check; }
        {
            uint64_t pt = pde & 0x000FFFFFFFFFF000ULL;
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
        if ((c->cr4 & (1ULL<<20)) && mode == T_EXEC) { page_fault(m, vaddr, mode, 1, 1); return 0; } /* SMEP */
        if ((c->cr4 & (1ULL<<21)) && mode != T_EXEC &&
            !(c->rflags & (1ULL<<18)))          { page_fault(m, vaddr, mode, 1, 0); return 0; } /* SMAP, AC=0 */
    }
    if (mode == T_EXEC && nx_any && (c->efer & (1ULL<<11))) { page_fault(m, vaddr, mode, 1, 1); return 0; }
    if (mode == T_WRITE && !eff_w && (user_cpl || (c->cr0 & (1ULL<<16)))) { page_fault(m, vaddr, mode, 1, 0); return 0; }
    return 1;
}

static void raise_exception(machine_t *m, int vector, int has_err, uint32_t err);

static uint64_t read_mem_v(machine_t *m, uint64_t vaddr, int size) {
    uint64_t phys;
    if (!translate(m, vaddr, &phys, T_READ))  /* fault already raised (K4) */
        return 0;
    return mem_read(m, phys, size);
}
static void write_mem_v(machine_t *m, uint64_t vaddr, int size, uint64_t val) {
    uint64_t phys;
    if (!translate(m, vaddr, &phys, T_WRITE))  /* fault already raised (K4) */
        return;
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
        mlog(&m->log, "[cpu] real-mode vector %d (%s) -> %04x:%04x via IVT", vector, vecname(vector), cs, off);
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
        if (dbg_u_on > 0 && ring_change)
            mlog(&m->log, "[dbg-u] ringchg vec=%d from_cpl=%d ist=%d tss=%llx ns=%llx old_rsp=%llx cr3=%llx",
                 vector, from_cpl, ist, (unsigned long long)tss, (unsigned long long)ns,
                 (unsigned long long)c->gpr[RSP], (unsigned long long)c->cr3);
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
    c->rflags &= ~FLAG_IF;
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

static uint64_t read_code_v(machine_t *m, uint64_t vaddr, int size) {
    uint64_t phys;
    if (!translate(m, vaddr, &phys, T_EXEC)) /* fault already raised */
        return 0;
    return mem_read(m, phys, size);
}
static uint8_t fetch8(dctx_t *d){
    if (dbg_u_on < 0) { const char *e = getenv("EMU_DBG_U"); dbg_u_on = (e && e[0] == '1'); }
    uint64_t la = d->c->seg[SEG_CS].base + d->pc;
    if (dbg_u_on && dbg_u_events < 200 && (d->c->seg[SEG_CS].sel & 3) == 3)
        { dbg_u_events++; mlog(&d->m->log, "[dbg-u] %03d uf la=%llx", dbg_u_events, (unsigned long long)la); }
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
 * 2 = memory pending RIP-relative fixup. reg_field is the ModRM.reg bits
 * (5:3), ALWAYS valid regardless of is_mem -- it is either a second
 * register operand or an opcode-extension, depending on the instruction. */
typedef struct { int is_mem; int reg; uint64_t addr; int seg; int reg_field; } rm_t;
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
    c->cr0 = 0x60000010ULL;
    c->halted = 0; c->fault = 0; c->intr_delay = 0;
    /* CHIPSET H6: IA32_APIC_BASE resets to base|EN|BSP (the xAPIC comes
     * out of reset hardware-enabled and software-disabled at SVR). */
    cpu_set_msr(c, LAPIC_MSR_APICBASE, LAPIC_MSR_DEFAULT);
}

/* ============================== main stepper ============================== */
int cpu_step(cpu_t *c) {
    machine_t *m = c->mach;
    /* K4 dbg-u: point watches on the syscall-entry stub stores */
    if (dbg_u_on > 0 && (c->rip == 0xffffffff801907beull ||
                         c->rip == 0xffffffff801907d3ull ||
                         c->rip == 0xffffffff80106f85ull))
        mlog(&m->log, "[dbg-u] watch rip=%llx rsp=%llx rcx=%llx gs=%llx kgs=%llx",
             (unsigned long long)c->rip, (unsigned long long)c->gpr[RSP],
             (unsigned long long)c->gpr[RCX], (unsigned long long)c->seg[SEG_GS].base,
             (unsigned long long)c->kgs_base);

    /* ---- CHIPSET H5: a requested system reset (KBC 0xFE / output-port
     * bit0 / port 0x92 bit0 / port 0xCF9) takes effect at the next
     * instruction boundary, modeling reset-line propagation and keeping
     * a reset out of the middle of an executing instruction. */
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
                        /* K3: virtual time must not freeze while the CPU idles in HLT
             * -- PIT/LAPIC/RTC all derive from instr_count (K6: vtime). Charge
             * a time quantum (it counts toward --max-instr like retired work)
             * and wake on whatever arrives.  */
            c->instr_count += 512;
            m->vtime_instr += 512;   /* K6: master clock advances too */
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

    c->exception_taken = 0;
    dctx_t d; memset(&d,0,sizeof d);
    d.m = m; d.c = c; d.pc = c->rip; d.seg_override = -1;
    d.start_pc = c->rip;
    int default_as = cpu_addr_size(c);
    d.addrsize = default_as;
    d.opsize = cpu_op_size_default(c);

    uint8_t op;
    for (;;) {
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
        d.pc = d.start_pc; /* restart decode of this instruction cleanly */
        /* re-skip prefixes */
        for (;;) {
            uint8_t b = fetch8(&d);
            if (b==0x66||b==0x67||b==0xF0||b==0xF2||b==0xF3||b==0x2E||b==0x36||b==0x3E||b==0x26||b==0x64||b==0x65) continue;
            if (default_as==64 && b>=0x40 && b<=0x4F) continue;
            op = b; break;
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
                /* unbuffered FILE, not the ring or a pipe: storm wraps wrap
                 * the ring, pipe buffers never flush before a kill (both
                 * measured). Live-readable mid-run. */
                if (!probe_fp) { probe_fp = fopen("/tmp/k4-probe.txt", "w"); setvbuf(probe_fp, NULL, _IONBF, 0); }
                fprintf(probe_fp, "sysread0#%d buf=%llx n=%lld rip=%llx cr3=%llx\n",
                        syscall_ret_probe, (unsigned long long)c->gpr[RSI],
                        (long long)c->gpr[RDX], (unsigned long long)c->rip,
                        (unsigned long long)c->cr3);
            }
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
            if (dbg_u_on > 0)
                mlog(&m->log, "[dbg-u] SYSCALL pre-farload rsp=%llx kcs=%x lstar=%llx",
                     (unsigned long long)c->gpr[RSP], kcs, (unsigned long long)lstar);
            far_load_cs(m, kcs, lstar);
            if (dbg_u_on > 0)
                mlog(&m->log, "[dbg-u] SYSCALL post-farload rsp=%llx rip=%llx",
                     (unsigned long long)c->gpr[RSP], (unsigned long long)c->rip);
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
            if (dbg_u_on > 0 && syscall_ret_probe >= 0) {
                fprintf(probe_fp, "sysret0#%d rax=%lld rip=%llx bytes=",
                        syscall_ret_probe, (long long)c->gpr[RAX],
                        (unsigned long long)c->gpr[RCX]);
                for (int i = 0; i < 16 && (long long)c->gpr[RAX] > 0; i++)
                    fprintf(probe_fp, "%02llx", (unsigned long long)read_mem_v(m, c->gpr[RSI] + i, 1));
                fputc('\n', probe_fp);
                syscall_ret_probe = -1;
            }
            if (dbg_u_on)
                mlog(&m->log, "[dbg-u] SYSRET to rip=%llx rflags=%llx rsp=%llx [rsp]=%llx",
                     (unsigned long long)c->gpr[RCX], (unsigned long long)c->gpr[R11],
                     (unsigned long long)c->gpr[RSP],
                     (unsigned long long)read_mem_v(m, c->gpr[RSP], 8));
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
        case 0x31: { /* RDTSC: virtual time = retired instructions x per-platform
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
            if (ext == 0) { /* FXSAVE m512: powered-on x87 state + our live
                               MXCSR + live XMM file (K4: the scheduler's
                               context images must round-trip the values
                               userspace actually computed). */
                write_mem_v(m, addr +  0, 2, 0x037F);             /* FCW */
                write_mem_v(m, addr +  2, 2, 0);                  /* FSW */
                write_mem_v(m, addr +  4, 2, 0);                  /* FTW */
                write_mem_v(m, addr +  6, 2, 0);                  /* RSVD */
                write_mem_v(m, addr +  8, 2, 0);                  /* FOP */
                write_mem_v(m, addr + 10, 1, 0); write_mem_v(m, addr + 11, 1, 0);
                write_mem_v(m, addr + 12, 4, 0);                  /* FIP */
                write_mem_v(m, addr + 16, 2, 0);                  /* FCS */
                write_mem_v(m, addr + 18, 2, 0);
                write_mem_v(m, addr + 20, 4, 0);                  /* FDP */
                write_mem_v(m, addr + 24, 2, 0);                  /* FDS */
                write_mem_v(m, addr + 26, 2, 0);
                write_mem_v(m, addr + 28, 4, c->mxcsr);           /* MXCSR */
                write_mem_v(m, addr + 32, 4, 0x0000FFBF);         /* MXCSR_MASK */
                for (int off = 32 + 4; off < 512; off += 4)
                    write_mem_v(m, addr + (uint64_t)off, 4, 0);
                for (int i = 0; i < 16; i++) {
                    write_mem_v(m, addr + 160 + (uint64_t)i * 16,     8, c->xmm[i][0]);
                    write_mem_v(m, addr + 160 + (uint64_t)i * 16 + 8, 8, c->xmm[i][1]);
                }
                break; }
            { /* ext == 1: FXRSTOR -- absorb the MXCSR (masked like the
                 hardware does) and the live XMM file (K4). */
                uint32_t nx = (uint32_t)read_mem_v(m, addr + 28, 4);
                c->mxcsr = (c->mxcsr & ~0x0000FFFFu) | (nx & 0x0000FFFFu);
                for (int i = 0; i < 16; i++) {
                    c->xmm[i][0] = read_mem_v(m, addr + 160 + (uint64_t)i * 16,     8);
                    c->xmm[i][1] = read_mem_v(m, addr + 160 + (uint64_t)i * 16 + 8, 8);
                }
                break; } }
        case 0x30: { uint32_t idx=(uint32_t)c->gpr[RCX]; uint64_t val=((uint64_t)(uint32_t)c->gpr[RDX]<<32)|(uint32_t)c->gpr[RAX];
                     cpu_set_msr(c, idx, val);
                     if (idx==0xC0000080) c->efer = val;
                     /* K3: GS-base MSRs are the architected long-mode
                      * segment base for GS addressing -- keep the decoder's
                      * seg[] shadow in sync (measured: AuraLite's
                      * cpu_local_init wires its per-CPU block through
                      * IA32_GS_BASE). */
                     /* K4: FS.base rides the same MSR lane (IA32_FS_BASE) --
                      * AuraLite's ARCH_SET_FS target write_fs_base() goes
                      * through write_msr, and the userspace-libc errno slot
                      * lives at %fs:0. Measured: init shell SIGSEGV'd on
                      * `mov %fs:0x0,%rdx` at 0x40008561 with FS.base stuck 0
                      * (null read, err=0x4) right after the banner. */
                     else if (idx==0xC0000100) c->seg[SEG_FS].base = val;
                     else if (idx==0xC0000101) c->seg[SEG_GS].base = val;
                     else if (idx==0xC0000102) c->kgs_base = val;
                     break; }
        case 0x32: { uint32_t idx=(uint32_t)c->gpr[RCX]; int found; uint64_t val=cpu_get_msr(c, idx, &found);
                     if (idx==0xC0000080) val = c->efer;
                     if (!found && idx!=0xC0000080) val = 0;
                     c->gpr[RAX] = (uint32_t)val; c->gpr[RDX] = (uint32_t)(val>>32);
                     break; }
        case 0x20: { rm_t rm=MODRM(); int crn=rm.reg_field&7; uint64_t v = crn==0?c->cr0:crn==2?c->cr2:crn==3?c->cr3:crn==4?c->cr4:0;
                     /* MOV r32/64, CRn is always register-to-register (mod==3 enforced by spec) */
                     set_reg(c, rm.reg, (default_as==64)?8:4, 1, v); break; }
        case 0x22: { rm_t rm=MODRM(); int crn=rm.reg_field&7;
                     /* MOV CRn, reg reads the architected width: 64 bits in
                      * long mode, 32 elsewhere (the baseline always read 8
                      * bytes, C0 ledger #10). */
                     uint64_t v = rm_read(&d,&rm,(default_as==64)?8:4) & ((default_as==64)?~0ull:0xFFFFFFFFull);
                     if (crn==0) { int entering_pg = ((v>>31)&1) && !((c->cr0>>31)&1); c->cr0=v;
                                   if (entering_pg && (c->efer & (1ULL<<8))) c->efer |= (1ULL<<10); /* set LMA */
                                   if (!((v>>31)&1)) c->efer &= ~(1ULL<<10);
                     }
                     else if (crn==2) c->cr2=v; else if (crn==3) c->cr3=v; else if (crn==4) c->cr4=v;
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
                     if (modrm == 0xCB) { /* STAC (K4): SMAP door open */
                         c->rflags |= (1u<<18);
                     } else if (modrm == 0xCA) { /* CLAC (K4): door shut */
                         c->rflags &= ~(1u<<18);
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
                                c->cr0 = nc; }
                         break;
                     }
                     if (ext==7) { /* INVLPG: the operand is a page HINT, never a
                                      memory read -- real silicon does not fault on
                                      an unmapped address (SDM: no TLB entry, no
                                      operation). We keep no TLB model, so INVLPG
                                      is a pure no-op. K2: measured -- AuraLite's
                                      paging_unmap() INVLPG faulted through our
                                      eager linear read below and re-entered the
                                      kernel's own #PF handler mid-self-test. */
                         break; }
                     if (ext==5) { faultf(c, "#UD 0F 01 /5 reserved"); break; }
                     uint64_t base = (rm.seg>=0)? d.c->seg[rm.seg].base:0; uint64_t addr=base+rm_ea(&d,&rm);
                     if (ext==0 || ext==1) { /* SGDT / SIDT */
                         uint16_t lim = (ext==0) ? c->gdtr_limit : c->idtr_limit;
                         uint64_t gb  = (ext==0) ? c->gdtr_base  : c->idtr_base;
                         write_mem_v(m, addr, 2, lim);
                         if (cpu_addr_size(c)==64) write_mem_v(m, addr+2, 8, gb);
                         else { write_mem_v(m, addr+2, 4, gb); write_mem_v(m, addr+6, 2, 0); }
                         break;
                     }
                     uint16_t lim = (uint16_t)read_mem_v(m, addr, 2);
                     uint64_t gbase = read_mem_v(m, addr+2, (cpu_addr_size(c)==64)?8:4);
                     if (ext==2) { c->gdtr_limit=lim; c->gdtr_base=gbase; }
                     else if (ext==3) { c->idtr_limit=lim; c->idtr_base=gbase; }
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
    case 0xDB: { /* x87 control subset (K3): FNINIT (DB E3) and FNCLEX
                    (DB E2) are deliberate no-ops -- this machine has no x87
                    data model (measured: AuraLite's context_switch issues
                    fninit+ldmxcsr per switch). Every other encoding stays
                    #UD: that is the honest silence boundary for x87 data
                    ops, not accidental absence. */
        uint8_t modrm = fetch8(&d);
        if (modrm == 0xE3 /* FNINIT */ || modrm == 0xE2 /* FNCLEX */) break;
        faultf(c, "#UD x87 data op out of scope (K3)"); break; }
    case 0xD9: { /* x87 control words only (K3): FLDCW /5 m16 stores into a
                    tracked control word, FNSTCW /7 m16 reads it back. Data
                    opcodes (#UD) fail loudly rather than pretend. */
        uint8_t modrm = fetch8(&d); int ext=(modrm>>3)&7, mod=modrm>>6;
        (void)mod;
        rm_t rm = decode_modrm(&d, modrm); fixup_riprel(&d,&rm);
        if (ext==5) { c->fcw=(uint16_t)rm_read(&d,&rm,2); break; }
        if (ext==7) { rm_write(&d,&rm,2,c->fcw); break; }
        faultf(c, "#UD x87 data op out of scope (K3)"); break; }
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

    default:
        faultf(c, "#UD unsupported opcode 0x%02x at rip=%08llx (mode %s)", op, (unsigned long long)d.start_pc, cpu_mode_name(c));
        break;
    }

done:
    if (!c->fault && !c->exception_taken) c->rip = d.pc;
    c->instr_count++;
    m->vtime_instr++;   /* K6: the machine-wide master clock */
    pit_tick(m);   /* CHIPSET H2: virtual-time advance; no-op until pit_init */
    rtc_tick(m);   /* CHIPSET H3: same D6 timebase; update-ended IRQ8 here */
    lapic_tick(m); /* CHIPSET H6: LAPIC timer on the same virtual TSC */
    if (c->fault) { mlog(&m->log, "[cpu] FAULT: %s", c->fault_msg); return -1; }
    if (c->halted) return (c->rflags & FLAG_IF) ? 0 : -1;   /* K3: sti;hlt stays
        alive -- the sleep is serviced by the top-of-step branch next round */
    return 0;
}
