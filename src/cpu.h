// cpu.h -- Generic x86 (16/32/64-bit) interpreter core
#ifndef CPU_H
#define CPU_H
#include <stdint.h>
#include <stddef.h>

typedef struct machine machine_t; // fwd decl (defined in mem.h/machine.h)

enum { RAX=0,RCX=1,RDX=2,RBX=3,RSP=4,RBP=5,RSI=6,RDI=7,
       R8=8,R9=9,R10=10,R11=11,R12=12,R13=13,R14=14,R15=15 };
enum { SEG_ES=0,SEG_CS=1,SEG_SS=2,SEG_DS=3,SEG_FS=4,SEG_GS=5 };

/* RFLAGS bits */
#define FLAG_CF (1ULL<<0)
#define FLAG_PF (1ULL<<2)
#define FLAG_AF (1ULL<<4)
#define FLAG_ZF (1ULL<<6)
#define FLAG_SF (1ULL<<7)
#define FLAG_TF (1ULL<<8)
#define FLAG_IF (1ULL<<9)
#define FLAG_DF (1ULL<<10)
#define FLAG_OF (1ULL<<11)

typedef struct {
    uint16_t sel;
    uint64_t base;
    uint32_t limit;
    uint8_t  d_b;     /* default operand size bit (1=32bit,0=16bit) for CS/SS etc */
    uint8_t  l;       /* long-mode 64-bit code segment bit (CS only) */
    uint8_t  present;
    uint8_t  type;    /* raw access-rights byte, for diagnostics */
} segment_t;

#define MAX_MSR 64

typedef struct {
    uint32_t idx;
    uint64_t val;
} msr_entry_t;

typedef struct cpu {
    uint64_t gpr[16];
    uint64_t rip;
    uint64_t rflags;
    segment_t seg[6];
    uint64_t cr0, cr2, cr3, cr4;
    uint64_t efer;
    uint64_t kgs_base;   /* IA32_KERNEL_GS_BASE (0xC0000102) -- the SWAPGS
                          * partner of seg[SEG_GS].base which shadows
                          * IA32_GS_BASE (0xC0000101). K3: per-CPU data in
                          * long mode flows through these MSRs (measured:
                          * AuraLite's cpu_local/scheduler init). */
    uint16_t fcw;    /* K3: x87 control word tracked for FLDCW/FNSTCW;
                          * no x87 data model behind it (deliberate). */
    uint32_t mxcsr;    /* K3: SSE control/status; FXSAVE/FXRSTOR/LDMXCSR/
                        * STMXCSR route here. */
    uint64_t xmm[16][2]; /* K4: the media data file exists now -- the
                        * measured ring-3 lane (userspace memset/memcpy and
                        * the AuraLite context switch) runs MOVAPS/UPS, MOVDQA/
                        * U, scalar moves and PXOR through it. SIMD arithmetic
                        * is still deliberately out of scope. */
    uint64_t gdtr_base; uint16_t gdtr_limit;
    uint64_t idtr_base; uint16_t idtr_limit;
    uint64_t tr_base;  uint16_t tr_limit;     /* H6: cached by LTR (0F 00 /3) */

    msr_entry_t msr[MAX_MSR];
    int msr_count;

    int halted;
    int in_exception;        /* re-entrancy guard against exception-during-exception (double fault) */
    int desc_sv;             /* >0: descriptor-table read window -- CPU-internal
                              * (implicit) reads of GDT/LDT/IDT descriptors are
                              * supervisor accesses on real hardware even at CPL3
                              * (SDM vol.3A s.5.5): the GDT may live on
                              * supervisor-only pages and SYSCALL/far-control
                              * transfers must not U/S-fault fetching from it.
                              * translate() grants supervisor credit while set.
                              * (K4: measured -- far_load_cs inside our SYSCALL
                              * case fetched kernel GDT[1] at CPL3 from a
                              * supervisor page, took #PF err=0x5, and the stale
                              * delivery pushed 48B onto the user stack before
                              * LSTAR was latched -> userspace stack clobbered,
                              * RST returned to address 5.) */
    uint8_t intr_delay;      /* H0: INTR shadow after STI / MOV SS / POP SS (one instruction) */
    int exception_taken;     /* set when raise_exception() redirected RIP mid-instruction */
    int fault;              /* set on unrecoverable decode/exec error */
    char fault_msg[256];
    uint64_t fault_rip;

    uint64_t instr_count;
    /* PERF P0: instruction attempts actually EXECUTED by this context.  instr_count
     * cannot serve as a throughput numerator: the K3 idle quantum adds 512
     * to it per all-idle HLT slot WITHOUT retiring anything (cpu.c, the
     * `!peer_busy` arm), and K6 makes it the guest-visible vtime base, so
     * its semantics are frozen by contract.  This counter increments only
     * in cpu_step's done block, so it is 0 for a run that only idled
     * and equals instr_count for a run that never idled.  This counts
     * faulting attempts too, not architecturally retired instructions;
     * "MIPS" below means guest instruction attempts per wall second.
     * Reported by
     * --stats; no guest-visible state and no historical tape depends on
     * it. */
    uint64_t executed_steps;
    /* TOOLING T1: exception/interrupt deliveries on this context (every
     * raise_exception call). Tooling-only bookkeeping: no instruction reads
     * it, so it is not guest-visible. The trace self-check uses it to tell
     * "the next RIP came from an event" from "the next RIP is wrong". */
    uint64_t event_count;
    int trace;               /* if non-zero, print each instruction */

    machine_t *mach;         /* back-reference to owning machine (memory/io) */
} cpu_t;

void cpu_reset(cpu_t *c);
/* Executes exactly one instruction; returns 0 normally, -1 on fault or on a
 * hlt+cli halt without progress.
 *
 * SMP INVARIANT (K6): `c` MUST be the machine's CURRENT context -- the value
 * in m->cpu, not a parked vcpu slot.  cpu_step() passes m->cpu to every
 * internal helper (translate, raise_exception, page_fault, descriptor reads),
 * and the run loop swaps a slot into m->cpu with vcpu_load() before stepping
 * it.  Calling cpu_step() on a slot copy would execute "one instruction" and
 * deliver any exception into the live BSP context instead, so the swap is not
 * an optimization but part of the interface.  Unit harnesses satisfy this by
 * stepping the machine's own m->cpu. */
int  cpu_step(cpu_t *c);

uint64_t cpu_get_msr(cpu_t *c, uint32_t idx, int *found);
void     cpu_set_msr(cpu_t *c, uint32_t idx, uint64_t val);

const char *cpu_mode_name(cpu_t *c);
int cpu_addr_size(cpu_t *c);
int cpu_op_size_default(cpu_t *c);

/* USB U3: device-cadence hook installed by devices_init.  Narrow unit
 * harnesses link cpu.c without devices.c and leave it NULL. */
extern void (*cpu_devices_tick)(machine_t *m);
/* TOOLING T1/T2: instruction-boundary hook, called by cpu_step after
 * interrupt delivery and before the fetch, only while m->dbg is set.
 * A non-zero return makes cpu_step return 0 WITHOUT executing (used by the
 * debugger to abandon a step for quit/loadvm). Installed by src/dbg.c;
 * narrow harnesses link cpu.c without it and leave it NULL. */
extern int (*cpu_dbg_boundary)(machine_t *m);
/* TOOLING T2: linear data-access hook for watchpoints, called by the
 * CPU's linear read/write helpers after a successful translation. Set by
 * src/dbg.c only while at least one watchpoint exists; NULL otherwise. */
extern void (*cpu_dbg_access)(machine_t *m, uint64_t lin, int size, int write);
/* TOOLING T4: the machine-wide device-cadence phase (s_devtick) is machine
 * state -- a snapshot must carry it or EHCI timing shifts after a load. */
uint32_t cpu_devtick_get(void);
void     cpu_devtick_set(uint32_t v);
int      cpu_dbg_knob_resolved(void);   /* lazy EMU_DBG_U lookup done?   */
void     cpu_dbg_knob_resolve(void);

#endif
