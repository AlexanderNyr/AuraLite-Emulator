// cpu.c -- x86 16/32/64-bit interpreter core (real/protected/long mode,
// segmentation, 4-level paging, exceptions). Scoped to the integer ISA
// subset needed by firmware/bootloader-style code, built on a fully
// generic ModRM/SIB decoder so it is straightforward to extend further.
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <stdlib.h>
#include "cpu.h"
#include "machine.h"
#include "platform.h"

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
static int translate(machine_t *m, uint64_t vaddr, uint64_t *out_phys, int for_write) {
    (void)for_write;
    cpu_t *c = &m->cpu;
    if (!(c->cr0 & (1ULL<<31))) { *out_phys = vaddr; return 1; } /* paging disabled */
    uint64_t cr3 = c->cr3 & ~0xFFFULL;
    int idx4 = (int)((vaddr >> 39) & 0x1FF);
    int idx3 = (int)((vaddr >> 30) & 0x1FF);
    int idx2 = (int)((vaddr >> 21) & 0x1FF);
    int idx1 = (int)((vaddr >> 12) & 0x1FF);

    uint64_t pml4e = mem_read(m, cr3 + (uint64_t)idx4*8, 8);
    if (!(pml4e & 1)) return 0;
    uint64_t pdpt = pml4e & 0x000FFFFFFFFFF000ULL;

    uint64_t pdpte = mem_read(m, pdpt + (uint64_t)idx3*8, 8);
    if (!(pdpte & 1)) return 0;
    if (pdpte & (1ULL<<7)) { *out_phys = (pdpte & 0xFFFFFC0000000ULL) | (vaddr & 0x3FFFFFFFULL); return 1; }
    uint64_t pd = pdpte & 0x000FFFFFFFFFF000ULL;

    uint64_t pde = mem_read(m, pd + (uint64_t)idx2*8, 8);
    if (!(pde & 1)) return 0;
    if (pde & (1ULL<<7)) { *out_phys = (pde & 0x000FFFFFFFE00000ULL) | (vaddr & 0x1FFFFFULL); return 1; }
    uint64_t pt = pde & 0x000FFFFFFFFFF000ULL;

    uint64_t pte = mem_read(m, pt + (uint64_t)idx1*8, 8);
    if (!(pte & 1)) return 0;
    *out_phys = (pte & 0x000FFFFFFFFFF000ULL) | (vaddr & 0xFFFULL);
    return 1;
}

static void raise_exception(machine_t *m, int vector, int has_err, uint32_t err);

static uint64_t read_mem_v(machine_t *m, uint64_t vaddr, int size) {
    uint64_t phys;
    if (!translate(m, vaddr, &phys, 0)) {
        mlog(&m->log, "[cpu] #PF reading v=0x%llx (page not present)", (unsigned long long)vaddr);
        raise_exception(m, 14, 1, 0);
        return 0;
    }
    return mem_read(m, phys, size);
}
static void write_mem_v(machine_t *m, uint64_t vaddr, int size, uint64_t val) {
    uint64_t phys;
    if (!translate(m, vaddr, &phys, 1)) {
        mlog(&m->log, "[cpu] #PF writing v=0x%llx (page not present)", (unsigned long long)vaddr);
        raise_exception(m, 14, 1, 2);
        return;
    }
    mem_write(m, phys, size, val);
}

/* ============================ GDT / segments ============================ */
typedef struct { uint64_t base; uint32_t limit; uint8_t type, s, dpl, present, l, db, g; } desc_t;

static void read_descriptor(machine_t *m, uint64_t table_base, uint16_t sel, desc_t *d) {
    uint32_t idx = sel >> 3;
    uint64_t addr = table_base + (uint64_t)idx * 8;
    uint64_t lo = mem_read(m, addr, 4);
    uint64_t hi = mem_read(m, addr + 4, 4);
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
static void raise_exception(machine_t *m, int vector, int has_err, uint32_t err) {
    cpu_t *c = &m->cpu;
    if (c->in_exception) {
        mlog(&m->log, "[cpu] exception while delivering an exception -> double/triple fault, halting");
        faultf(c, "double fault (vector %d while handling another)", vector);
        c->halted = 1;
        return;
    }
    c->in_exception = 1;
    if (c->idtr_limit == 0) {
        mlog(&m->log, "[cpu] %s (vector %d) with NO IDT loaded -> triple fault / reset-worthy condition", vecname(vector), vector);
        faultf(c, "%s with no IDT loaded", vecname(vector));
        c->halted = 1;
        return;
    }
    uint64_t gate_addr = c->idtr_base + (uint64_t)vector * 16;
    uint16_t off0 = (uint16_t)mem_read(m, gate_addr+0, 2);
    uint16_t sel  = (uint16_t)mem_read(m, gate_addr+2, 2);
    uint16_t off1 = (uint16_t)mem_read(m, gate_addr+6, 2);
    uint32_t off2 = (uint32_t)mem_read(m, gate_addr+8, 4);
    uint8_t  type_attr = (uint8_t)mem_read(m, gate_addr+5, 1);
    if (!(type_attr & 0x80)) {
        mlog(&m->log, "[cpu] vector %d: IDT gate not present -> triple fault", vector);
        c->halted = 1; faultf(c, "%s with not-present IDT gate", vecname(vector));
        return;
    }
    uint64_t handler = (uint64_t)off0 | ((uint64_t)off1<<16) | ((uint64_t)off2<<32);
    int is64 = ((c->efer>>10)&1) && c->seg[SEG_CS].l;
    int stacksz = is64 ? 8 : 4;
    uint64_t rsp = c->gpr[RSP];
    if (has_err) { rsp -= stacksz; write_mem_v(m, rsp, stacksz, err); }
    rsp -= stacksz; write_mem_v(m, rsp, stacksz, c->seg[SEG_CS].sel);
    rsp -= stacksz; write_mem_v(m, rsp, stacksz, c->rip);
    rsp -= stacksz; write_mem_v(m, rsp, stacksz, c->rflags);
    c->gpr[RSP] = rsp;
    far_load_cs(m, sel, handler);
    c->rflags &= ~FLAG_IF;
    c->exception_taken = 1;
    mlog(&m->log, "[cpu] exception %s delivered -> CS:RIP=%04x:%llx", vecname(vector), sel, (unsigned long long)handler);
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
static void set_flags_add(cpu_t *c, uint64_t a, uint64_t b, uint64_t res, int size) {
    uint64_t mm = reg_mask(size), sbit = 1ull << (size*8-1);
    c->rflags &= ~(FLAG_CF|FLAG_OF|FLAG_ZF|FLAG_SF|FLAG_PF|FLAG_AF);
    if ((res & mm) == 0) c->rflags |= FLAG_ZF;
    if (res & sbit) c->rflags |= FLAG_SF;
    if (parity8((uint8_t)res)) c->rflags |= FLAG_PF;
    if (((a & mm) + (b & mm)) > mm) c->rflags |= FLAG_CF;
    uint64_t sa=a&sbit, sb=b&sbit, sr=res&sbit;
    if (sa==sb && sr!=sa) c->rflags |= FLAG_OF;
    if (((a & 0xF) + (b & 0xF)) & 0x10) c->rflags |= FLAG_AF;
}
static void set_flags_sub(cpu_t *c, uint64_t a, uint64_t b, uint64_t res, int size) {
    uint64_t mm = reg_mask(size), sbit = 1ull << (size*8-1);
    c->rflags &= ~(FLAG_CF|FLAG_OF|FLAG_ZF|FLAG_SF|FLAG_PF|FLAG_AF);
    if ((res & mm) == 0) c->rflags |= FLAG_ZF;
    if (res & sbit) c->rflags |= FLAG_SF;
    if (parity8((uint8_t)res)) c->rflags |= FLAG_PF;
    if ((a & mm) < (b & mm)) c->rflags |= FLAG_CF;
    uint64_t sa=a&sbit, sb=b&sbit, sr=res&sbit;
    if (sa!=sb && sr!=sa) c->rflags |= FLAG_OF;
    if ((a & 0xF) < (b & 0xF)) c->rflags |= FLAG_AF;
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

static uint8_t fetch8(dctx_t *d){ uint8_t v=(uint8_t)read_mem_v(d->m, d->c->seg[SEG_CS].base+d->pc,1); d->pc+=1; return v; }
static uint16_t fetch16(dctx_t *d){ uint16_t v=(uint16_t)read_mem_v(d->m, d->c->seg[SEG_CS].base+d->pc,2); d->pc+=2; return v; }
static uint32_t fetch32(dctx_t *d){ uint32_t v=(uint32_t)read_mem_v(d->m, d->c->seg[SEG_CS].base+d->pc,4); d->pc+=4; return v; }
static uint64_t fetch64(dctx_t *d){ uint64_t v=read_mem_v(d->m, d->c->seg[SEG_CS].base+d->pc,8); d->pc+=8; return v; }
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
    return read_mem_v(d->m, base + rm->addr, size);
}
static void rm_write(dctx_t *d, rm_t *rm, int size, uint64_t val) {
    if (!rm->is_mem) { set_reg(d->c, rm->reg, size, d->has_rex, val); return; }
    uint64_t base = (rm->seg>=0) ? d->c->seg[rm->seg].base : 0;
    write_mem_v(d->m, base + rm->addr, size, val);
}
/* resolves a pending RIP-relative memory operand once the instruction length is final */
static void fixup_riprel(dctx_t *d, rm_t *rm) {
    if (rm->is_mem == 2) { rm->addr = (uint64_t)((int64_t)rm->addr + (int64_t)d->pc); rm->is_mem = 1; }
}

/* ========================= push / pop (stack ops) ========================= */
static void do_push(dctx_t *d, int size, uint64_t val) {
    cpu_t *c = d->c;
    int ssz = (cpu_addr_size(c)==64) ? 8 : size;
    c->gpr[RSP] -= ssz;
    write_mem_v(d->m, c->seg[SEG_SS].base + c->gpr[RSP], ssz, val);
}
static uint64_t do_pop(dctx_t *d, int size) {
    cpu_t *c = d->c;
    int ssz = (cpu_addr_size(c)==64) ? 8 : size;
    uint64_t v = read_mem_v(d->m, c->seg[SEG_SS].base + c->gpr[RSP], ssz);
    c->gpr[RSP] += ssz;
    return v;
}

/* =========================== group-op ALU helper =========================== */
enum { ALU_ADD=0, ALU_OR=1, ALU_ADC=2, ALU_SBB=3, ALU_AND=4, ALU_SUB=5, ALU_XOR=6, ALU_CMP=7 };
static uint64_t alu_op(cpu_t *c, int op, uint64_t a, uint64_t b, int size, int *write_back) {
    uint64_t res=0; *write_back=1;
    switch(op){
        case ALU_ADD: res=a+b; set_flags_add(c,a,b,res,size); break;
        case ALU_OR:  res=a|b; set_flags_logic(c,res,size); break;
        case ALU_AND: res=a&b; set_flags_logic(c,res,size); break;
        case ALU_SUB: res=a-b; set_flags_sub(c,a,b,res,size); break;
        case ALU_XOR: res=a^b; set_flags_logic(c,res,size); break;
        case ALU_CMP: res=a-b; set_flags_sub(c,a,b,res,size); *write_back=0; break;
        case ALU_ADC: res=a+b+(c->rflags&FLAG_CF?1:0); set_flags_add(c,a,b,res,size); break;
        case ALU_SBB: res=a-b-(c->rflags&FLAG_CF?1:0); set_flags_sub(c,a,b,res,size); break;
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
    c->cr0 = 0x60000010ULL;
    c->halted = 0; c->fault = 0;
}

/* ============================== main stepper ============================== */
int cpu_step(cpu_t *c) {
    machine_t *m = c->mach;
    if (c->fault || c->halted) return -1;

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
    if (d.has_rex && d.rex_w) d.opsize = 8; /* REX.W -> 64-bit operand size = 8 bytes */
    int osz = d.opsize;

    if (c->trace) {
        mlog(&m->log, "[trace] %s rip=%08llx op=%02x", cpu_mode_name(c), (unsigned long long)d.start_pc, op);
    }


    #define MODRM() decode_modrm(&d, fetch8(&d))
    #define FIXUP(rm) fixup_riprel(&d, &(rm))

    switch (op) {
    case 0x90: break; /* NOP */
    case 0xF4: c->halted = 1; break; /* HLT */
    case 0xFA: c->rflags &= ~FLAG_IF; break; /* CLI */
    case 0xFB: c->rflags |= FLAG_IF; break;  /* STI */
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
    case 0x8D: { rm_t rm=MODRM(); FIXUP(rm); /* LEA: address itself (no segment base) is the value */
        set_reg(c, rm.reg_field, osz, d.has_rex, rm.addr); break; }
    case 0x8E: { rm_t rm=MODRM(); FIXUP(rm); int segidx = rm.reg_field & 7;
        uint16_t sel = (uint16_t)rm_read(&d,&rm,2);
        if (!(c->cr0&1)) { c->seg[segidx].sel = sel; c->seg[segidx].base = (uint64_t)sel<<4; c->seg[segidx].limit=0xFFFF; }
        else { desc_t dsc; read_descriptor(m, c->gdtr_base, sel, &dsc); load_seg_from_desc(c, segidx, sel, &dsc); if (segidx==SEG_CS) {} }
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
        switch (op2) {
        case 0xB6: { rm_t rm=MODRM(); FIXUP(rm); uint64_t v=rm_read(&d,&rm,1); set_reg(c,rm.reg_field,osz,d.has_rex,v); break; }
        case 0xB7: { rm_t rm=MODRM(); FIXUP(rm); uint64_t v=rm_read(&d,&rm,2); set_reg(c,rm.reg_field,osz,d.has_rex,v); break; }
        case 0xA2: do_cpuid(m); break;
        case 0x30: { uint32_t idx=(uint32_t)c->gpr[RCX]; uint64_t val=((uint64_t)(uint32_t)c->gpr[RDX]<<32)|(uint32_t)c->gpr[RAX];
                     cpu_set_msr(c, idx, val);
                     if (idx==0xC0000080) c->efer = val;
                     break; }
        case 0x32: { uint32_t idx=(uint32_t)c->gpr[RCX]; int found; uint64_t val=cpu_get_msr(c, idx, &found);
                     if (idx==0xC0000080) val = c->efer;
                     if (!found && idx!=0xC0000080) val = 0;
                     c->gpr[RAX] = (uint32_t)val; c->gpr[RDX] = (uint32_t)(val>>32);
                     break; }
        case 0x20: { rm_t rm=MODRM(); int crn=rm.reg_field&7; uint64_t v = crn==0?c->cr0:crn==2?c->cr2:crn==3?c->cr3:crn==4?c->cr4:0;
                     /* MOV r32/64, CRn is always register-to-register (mod==3 enforced by spec) */
                     set_reg(c, rm.reg, (default_as==64)?8:4, 1, v); break; }
        case 0x22: { rm_t rm=MODRM(); int crn=rm.reg_field&7; uint64_t v = rm_read(&d,&rm,8);
                     if (crn==0) { int entering_pg = ((v>>31)&1) && !((c->cr0>>31)&1); c->cr0=v;
                                   if (entering_pg && (c->efer & (1ULL<<8))) c->efer |= (1ULL<<10); /* set LMA */
                                   if (!((v>>31)&1)) c->efer &= ~(1ULL<<10);
                     }
                     else if (crn==2) c->cr2=v; else if (crn==3) c->cr3=v; else if (crn==4) c->cr4=v;
                     break; }
        case 0x01: { uint8_t modrm = fetch8(&d); int ext=(modrm>>3)&7;
                     rm_t rm = decode_modrm(&d, modrm); fixup_riprel(&d,&rm);
                     uint64_t base = (rm.seg>=0)? d.c->seg[rm.seg].base:0; uint64_t addr=base+rm.addr;
                     uint16_t lim = (uint16_t)read_mem_v(m, addr, 2);
                     uint64_t gbase = read_mem_v(m, addr+2, (cpu_addr_size(c)==64)?8:4);
                     if (ext==2) { c->gdtr_limit=lim; c->gdtr_base=gbase; }
                     else if (ext==3) { c->idtr_limit=lim; c->idtr_base=gbase; }
                     break; }
        case 0x84: case 0x85: case 0x86: case 0x87: case 0x88: case 0x89: case 0x8A: case 0x8B:
        case 0x8C: case 0x8D: case 0x8E: case 0x8F: { /* Jcc near rel32 */
            int32_t rel = fetch32s(&d);
            int cc = op2 & 0xF; int take=0; uint64_t f=c->rflags;
            switch(cc){
                case 0x4: take = (f&FLAG_ZF)!=0; break;                 /* JE/JZ */
                case 0x5: take = (f&FLAG_ZF)==0; break;                 /* JNE/JNZ */
                case 0xC: take = ((f&FLAG_SF)!=0) != ((f&FLAG_OF)!=0); break; /* JL */
                case 0xD: take = ((f&FLAG_SF)!=0) == ((f&FLAG_OF)!=0); break; /* JGE */
                case 0xE: take = (f&FLAG_ZF) || (((f&FLAG_SF)!=0)!=((f&FLAG_OF)!=0)); break; /* JLE */
                case 0xF: take = !(f&FLAG_ZF) && (((f&FLAG_SF)!=0)==((f&FLAG_OF)!=0)); break; /* JG */
                case 0x2: take = (f&FLAG_CF)!=0; break;                 /* JB/JC */
                case 0x3: take = (f&FLAG_CF)==0; break;                 /* JAE/JNC */
                case 0x6: take = (f&FLAG_ZF)||(f&FLAG_CF); break;       /* JBE */
                case 0x7: take = !(f&FLAG_ZF) && !(f&FLAG_CF); break;   /* JA */
                default: take = 0; break;
            }
            if (take) d.pc = (uint64_t)((int64_t)d.pc + rel);
            break; }
        default:
            faultf(c, "#UD unsupported 0F opcode 0x0F 0x%02x at rip=%08llx", op2, (unsigned long long)d.start_pc);
            break;
        }
        break; }

    /* ---- INC/DEC (32/16-bit short forms; invalid encoding space in 64-bit mode) ---- */
    case 0x40: case 0x41: case 0x42: case 0x43: case 0x44: case 0x45: case 0x46: case 0x47: {
        int r=op-0x40; uint64_t a=get_reg(c,r,osz,0), res=a+1; set_reg(c,r,osz,0,res);
        uint64_t save_cf=c->rflags&FLAG_CF; set_flags_add(c,a,1,res,osz); c->rflags=(c->rflags&~FLAG_CF)|save_cf; break; }
    case 0x48: case 0x49: case 0x4A: case 0x4B: case 0x4C: case 0x4D: case 0x4E: case 0x4F: {
        int r=op-0x48; uint64_t a=get_reg(c,r,osz,0), res=a-1; set_reg(c,r,osz,0,res);
        uint64_t save_cf=c->rflags&FLAG_CF; set_flags_sub(c,a,1,res,osz); c->rflags=(c->rflags&~FLAG_CF)|save_cf; break; }

    /* ---- PUSH/POP reg ---- */
    case 0x50: case 0x51: case 0x52: case 0x53: case 0x54: case 0x55: case 0x56: case 0x57: {
        int r=(op-0x50)|(d.has_rex?(d.rex_b<<3):0); int sz = (default_as==64)?8:osz;
        do_push(&d, sz, get_reg(c,r,sz,d.has_rex)); break; }
    case 0x58: case 0x59: case 0x5A: case 0x5B: case 0x5C: case 0x5D: case 0x5E: case 0x5F: {
        int r=(op-0x58)|(d.has_rex?(d.rex_b<<3):0); int sz = (default_as==64)?8:osz;
        set_reg(c,r,sz,d.has_rex, do_pop(&d, sz)); break; }
    case 0x68: { int sz=(default_as==64)?8:osz; uint64_t imm=(uint64_t)(int64_t)fetch32s(&d); do_push(&d,sz,imm); break; }
    case 0x6A: { int sz=(default_as==64)?8:osz; uint64_t imm=(uint64_t)(int64_t)fetch8s(&d); do_push(&d,sz,imm); break; }

    /* ---- shift group2 ---- */
    case 0xC1: case 0xD1: case 0xD3: {
        rm_t rm=MODRM(); FIXUP(rm); int ext=rm.reg_field&7;
        uint8_t cnt = (op==0xC1)? fetch8(&d) : (op==0xD1? 1 : (uint8_t)(c->gpr[RCX]&0x1F));
        uint64_t v = rm_read(&d,&rm,osz); uint64_t res=v; uint64_t sbit=1ull<<(osz*8-1);
        cnt &= (osz==64)?63:31;
        switch(ext){
            case 4: res = (cnt< (uint64_t)osz*8) ? (v<<cnt) : 0; break; /* SHL */
            case 5: res = (cnt< (uint64_t)osz*8) ? (v & reg_mask(osz)) >> cnt : 0; break; /* SHR */
            case 7: { int64_t sv=(int64_t)(v<<(64-osz*8))>>(64-osz*8); res=(uint64_t)(sv>>(cnt<osz*8?cnt:osz*8-1)); break; } /* SAR */
            default: res = v; break;
        }
        if (cnt) { set_flags_logic(c,res,osz); if (ext==5||ext==4) { if (cnt<= (uint64_t)osz*8 && cnt>0) { uint64_t lastbit = (ext==4)? ((v>>(osz*8-cnt))&1) : ((v>>(cnt-1))&1); if(lastbit) c->rflags|=FLAG_CF; } } }
        (void)sbit;
        rm_write(&d,&rm,osz,res);
        break; }

    /* ---- group3 (TEST/NOT/NEG r/m,imm) ---- */
    case 0xF6: case 0xF7: {
        int size = (op==0xF6)?1:osz; rm_t rm=MODRM(); FIXUP(rm); int ext=rm.reg_field&7;
        if (ext==0 || ext==1) { uint64_t imm = (size==1)? fetch8(&d) : fetch_imm(&d, size==2?2:4);
            set_flags_logic(c, rm_read(&d,&rm,size) & imm, size); }
        else if (ext==2) { rm_write(&d,&rm,size, ~rm_read(&d,&rm,size)); }
        else if (ext==3) { uint64_t v=rm_read(&d,&rm,size); uint64_t res=(uint64_t)(-(int64_t)v); set_flags_sub(c,0,v,res,size); rm_write(&d,&rm,size,res); }
        else faultf(c, "#UD group3 ext=%d", ext);
        break; }

    /* ---- group5 (INC/DEC/CALL/JMP/PUSH r/m) ---- */
    case 0xFE: { rm_t rm=MODRM(); FIXUP(rm); int ext=rm.reg_field&7; uint64_t a=rm_read(&d,&rm,1);
        if (ext==0) { uint64_t r=a+1; set_flags_add(c,a,1,r,1); rm_write(&d,&rm,1,r);} else { uint64_t r=a-1; set_flags_sub(c,a,1,r,1); rm_write(&d,&rm,1,r);} break; }
    case 0xFF: { rm_t rm=MODRM(); FIXUP(rm); int ext=rm.reg_field&7;
        if (ext==0 || ext==1) { uint64_t a=rm_read(&d,&rm,osz);
            if (ext==0) { uint64_t r=a+1; set_flags_add(c,a,1,r,osz); rm_write(&d,&rm,osz,r);} else { uint64_t r=a-1; set_flags_sub(c,a,1,r,osz); rm_write(&d,&rm,osz,r);} }
        else if (ext==2) { uint64_t target=rm_read(&d,&rm,(default_as==64)?8:osz); do_push(&d,(default_as==64)?8:osz, d.pc); d.pc=target; }
        else if (ext==4) { uint64_t target=rm_read(&d,&rm,(default_as==64)?8:osz); d.pc=target; }
        else if (ext==6) { int sz=(default_as==64)?8:osz; do_push(&d,sz, rm_read(&d,&rm,sz)); }
        else if (ext==3 || ext==5) { /* far call/jmp through memory: m16:16/32/64 */
            if (!rm.is_mem) { faultf(c, "#UD far jmp/call with register operand"); break; }
            int offsz = (osz==2)?2:(default_as==64?8:4);
            uint64_t base = (rm.seg>=0) ? d.c->seg[rm.seg].base : 0;
            uint64_t off = read_mem_v(d.m, base + rm.addr, offsz);
            uint16_t sel = (uint16_t)read_mem_v(d.m, base + rm.addr + offsz, 2);
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

    /* ---- short Jcc (0x70-0x7F) ---- */
    case 0x70: case 0x71: case 0x72: case 0x73: case 0x74: case 0x75: case 0x76: case 0x77:
    case 0x78: case 0x79: case 0x7A: case 0x7B: case 0x7C: case 0x7D: case 0x7E: case 0x7F: {
        int8_t rel = fetch8s(&d); int cc = op & 0xF; int take=0; uint64_t f=c->rflags;
        switch(cc){
            case 0x4: take=(f&FLAG_ZF)!=0; break; case 0x5: take=(f&FLAG_ZF)==0; break;
            case 0xC: take=((f&FLAG_SF)!=0)!=((f&FLAG_OF)!=0); break; case 0xD: take=((f&FLAG_SF)!=0)==((f&FLAG_OF)!=0); break;
            case 0xE: take=(f&FLAG_ZF)||(((f&FLAG_SF)!=0)!=((f&FLAG_OF)!=0)); break;
            case 0xF: take=!(f&FLAG_ZF) && (((f&FLAG_SF)!=0)==((f&FLAG_OF)!=0)); break;
            case 0x2: take=(f&FLAG_CF)!=0; break; case 0x3: take=(f&FLAG_CF)==0; break;
            case 0x6: take=(f&FLAG_ZF)||(f&FLAG_CF); break; case 0x7: take=!(f&FLAG_ZF)&&!(f&FLAG_CF); break;
            case 0x0: take=(f&FLAG_OF)!=0; break; case 0x1: take=(f&FLAG_OF)==0; break;
            case 0x8: take=(f&FLAG_SF)!=0; break; case 0x9: take=(f&FLAG_SF)==0; break;
            case 0xA: take=(f&FLAG_PF)!=0; break; case 0xB: take=(f&FLAG_PF)==0; break;
            default: take=0;
        }
        if (take) d.pc = (uint64_t)((int64_t)d.pc + rel);
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

    /* ---- REP STOS ---- */
    case 0xAA: case 0xAB: {
        int size = (op==0xAA)?1:osz;
        int as = d.addrsize;
        uint64_t cnt = d.rep ? ( as==64? c->gpr[RCX] : as==32? (uint32_t)c->gpr[RCX] : (uint16_t)c->gpr[RCX] ) : 1;
        int dir = (c->rflags & FLAG_DF) ? -1 : 1;
        while (cnt--) {
            uint64_t addr = c->seg[SEG_ES].base + (as==64?c->gpr[RDI]: as==32?(uint32_t)c->gpr[RDI]:(uint16_t)c->gpr[RDI]);
            write_mem_v(m, addr, size, get_reg(c,RAX,size,d.has_rex));
            uint64_t ndi = c->gpr[RDI] + (uint64_t)(int64_t)(dir*size);
            if (as==64) c->gpr[RDI]=ndi; else if (as==32) c->gpr[RDI]=(uint32_t)ndi; else c->gpr[RDI]=(uint16_t)ndi;
            if (c->fault) break;
        }
        if (d.rep) { if (as==64) c->gpr[RCX]=0; else if (as==32) c->gpr[RCX] &= ~0xFFFFFFFFull; else c->gpr[RCX] &= ~0xFFFFull; }
        break; }

    default:
        faultf(c, "#UD unsupported opcode 0x%02x at rip=%08llx (mode %s)", op, (unsigned long long)d.start_pc, cpu_mode_name(c));
        break;
    }

done:
    if (!c->fault && !c->exception_taken) c->rip = d.pc;
    c->instr_count++;
    if (c->fault) { mlog(&m->log, "[cpu] FAULT: %s", c->fault_msg); return -1; }
    if (c->halted) return -1;
    return 0;
}
