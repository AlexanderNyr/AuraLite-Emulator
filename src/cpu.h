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
    uint64_t kgs_base;
    uint16_t fcw;    /* K3: x87 control word tracked for FLDCW/FNSTCW;
                          * no x87 data model behind it (deliberate). */
    uint32_t mxcsr;    /* K3: SSE control/status; FXSAVE/FXRSTOR/LDMXCSR/
                        * STMXCSR route here. */
    uint64_t xmm[16][2]; /* K4: the media data file exists now -- the
                        * measured ring-3 lane (userspace memset/memcpy and
                        * the AuraLite context switch) runs MOVAPS/UPS, MOVDQA/
                        * U, scalar moves and PXOR through it. SIMD arithmetic
                        * is still deliberately out of scope. */   /* IA32_KERNEL_GS_BASE (0xC0000102) -- the SWAPGS
                          * partner of seg[SEG_GS].base which shadows
                          * IA32_GS_BASE (0xC0000101). K3: per-CPU data in
                          * long mode flows through these MSRs (measured:
                          * AuraLite's cpu_local/scheduler init). */
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
    int trace;               /* if non-zero, print each instruction */

    machine_t *mach;         /* back-reference to owning machine (memory/io) */
} cpu_t;

void cpu_reset(cpu_t *c);
/* executes exactly one instruction; returns 0 normally, -1 on fault/halt-without-progress */
int  cpu_step(cpu_t *c);

uint64_t cpu_get_msr(cpu_t *c, uint32_t idx, int *found);
void     cpu_set_msr(cpu_t *c, uint32_t idx, uint64_t val);

const char *cpu_mode_name(cpu_t *c);
int cpu_addr_size(cpu_t *c);
int cpu_op_size_default(cpu_t *c);

#endif
