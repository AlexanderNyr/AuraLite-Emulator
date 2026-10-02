/* tests/diff_fuzz.c -- C11 differential fuzzer: emulator vs the HOST CPU.
 *
 * Method: generate random flat programs built ONLY from instructions that
 * have IDENTICAL semantics in the two worlds being compared:
 *
 *   - host:   x86-64 Linux, executed natively under ptrace SINGLESTEP,
 *   - target: the emulator in 16-bit real mode (tests/harness.h machine).
 *
 * The subtlety: operand-size prefixes INVERT meaning between the two
 * (host64: default 32, 66 -> 16; real16: default 16, 66 -> 32), and the
 * short inc/dec opcodes 0x40-0x4F are REX prefixes in 64-bit mode. So every
 * generated instruction carries TWO encodings -- `.h` for the host64 world
 * and `.e` for the real16 world -- that are byte-different but
 * semantically the SAME 16-bit register-direct operation. Only mod=11
 * instructions are used: no memory, no branches, no stack (short inc/dec
 * become FF /0 /1 on the host side), no LOCK.
 *
 * After every instruction the GPRs (low 16 bits) and the architecturally
 * DEFINED flag subset for that opcode are compared; undefined flags (AF
 * after shifts/logic ops, OF after unknown shift counts, everything but
 * CF/OF after multiplies) are masked out per opcode class. That mask table
 * is the plan's "AF/undef allowlist".
 *
 * To keep CMOVcc/SETcc/ADC/SBB inputs deterministic despite undefined
 * output flags of earlier instructions, BOTH sides' arithmetic+DF flags
 * are force-synced to the same fresh random value before EVERY
 * instruction (the plan's per-instruction contract). Register values,
 * meanwhile, flow through the whole program unmodified.
 *
 * Part two (crash invariant): raw random byte streams run ONLY in the
 * emulator; each must end in a clean architectural fault or halt, never a
 * host-level crash.
 *
 * Usage: diff_fuzz [programs] [instr-per-program] [seed]
 *        defaults: 512 24 0xC11
 */
#define _GNU_SOURCE
#include <assert.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/ptrace.h>
#include <sys/types.h>
#include <sys/uio.h>
#include <sys/user.h>
#include <sys/wait.h>
#include <unistd.h>
#include "machine.h"
#include "platform.h"
#include "harness.h"

#ifndef __linux__
#error "diff_fuzz requires Linux ptrace"
#endif
#ifndef NT_PRSTATUS
#define NT_PRSTATUS 1
#endif

/* ------------------------------------------------------------- PRNG ----- */

static uint64_t RNG_STATE;
static uint64_t rnd(void) { /* xorshift64* */
    uint64_t x = RNG_STATE;
    x ^= x >> 12; x ^= x << 25; x ^= x >> 27;
    RNG_STATE = x;
    return x * 0x2545F4914F6CDD1DULL;
}
static uint8_t rnd8(void) { return (uint8_t)rnd(); }

/* ------------------------------------------------------- instruction set -- */

#define MASK_ALL   (FLAG_CF|FLAG_PF|FLAG_AF|FLAG_ZF|FLAG_SF|FLAG_OF)
#define MASK_NO_AF (MASK_ALL & ~FLAG_AF)          /* logic ops: AF undefined */
#define SYNC_MASK  (FLAG_CF|FLAG_PF|FLAG_AF|FLAG_ZF|FLAG_SF|FLAG_DF|FLAG_OF)

typedef struct {
    uint8_t h[8], e[8];     /* host64 / real16 encodings, same operation */
    int hlen, elen;
    uint64_t fmask;         /* architecturally-defined flags to compare */
} insn_t;


static insn_t gen_insn(void) {
    insn_t in; in.hlen = in.elen = 0; in.fmask = 0;
    unsigned kind = (unsigned)(rnd() % 17);
    uint8_t r_rm = (uint8_t)(rnd() % 8), r_reg = (uint8_t)(rnd() % 8);
    switch (kind) {
    case 0: case 1: case 2: case 3: { /* ALU r16,r16 : 01/09/11/19/21/29/31 + cmp 39 */
        static const uint8_t OPS[8] = { 0x01, 0x09, 0x11, 0x19, 0x21, 0x29, 0x31, 0x39 };
        uint8_t opc = OPS[rnd() % 8];
        in.fmask = (opc == 0x09 || opc == 0x21 || opc == 0x31) ? MASK_NO_AF : MASK_ALL;
        { int hn = 0, en = 0;
          in.h[hn++] = 0x66; in.h[hn++] = opc; in.h[hn++] = (uint8_t)(0xC0 | (r_reg << 3) | r_rm);
          in.e[en++] = opc; in.e[en++] = (uint8_t)(0xC0 | (r_reg << 3) | r_rm);
          in.hlen = hn; in.elen = en; }
        break; }
    case 4: case 5: { /* group1 r16,imm8 : 83 /x ib */
        uint8_t ext = (uint8_t)(rnd() % 8), imm = rnd8();
        in.fmask = (ext == 1 || ext == 4 || ext == 6) ? MASK_NO_AF : MASK_ALL;
        { int hn = 0, en = 0;
          in.h[hn++] = 0x66; in.h[hn++] = 0x83; in.h[hn++] = (uint8_t)(0xC0 | (ext << 3) | r_rm); in.h[hn++] = imm;
          in.e[en++] = 0x83; in.e[en++] = (uint8_t)(0xC0 | (ext << 3) | r_rm); in.e[en++] = imm;
          in.hlen = hn; in.elen = en; }
        break; }
    case 6: case 7: { /* shift/rotate: D1 (by 1), C1 imm8, D3 (by CL) */
        uint8_t ext = (uint8_t)(rnd() % 8);
        uint8_t form = (uint8_t)(rnd() % 3);
        int is_rot = ext <= 3;
        if (form == 2) { /* by CL: count unknown statically */
            in.fmask = is_rot ? (uint64_t)FLAG_CF : (MASK_ALL & ~FLAG_AF & ~FLAG_OF);
            { int hn = 0, en = 0;
              in.h[hn++] = 0x66; in.h[hn++] = 0xD3; in.h[hn++] = (uint8_t)(0xC0 | (ext << 3) | r_rm);
              in.e[en++] = 0xD3; in.e[en++] = (uint8_t)(0xC0 | (ext << 3) | r_rm);
              in.hlen = hn; in.elen = en; }
        } else {
            uint8_t count = form ? (uint8_t)(1 + rnd() % 8) : 1;
            uint8_t opc = form ? 0xC1 : 0xD1;
            int of_ok = is_rot ? 0 : (count == 1);
            in.fmask = is_rot ? (uint64_t)FLAG_CF
                              : ((MASK_ALL & ~FLAG_AF) & (of_ok ? ~0ull : ~FLAG_OF));
            { int hn = 0, en = 0;
              in.h[hn++] = 0x66; in.h[hn++] = opc; in.h[hn++] = (uint8_t)(0xC0 | (ext << 3) | r_rm); if (form) in.h[hn++] = count;
              in.e[en++] = opc; in.e[en++] = (uint8_t)(0xC0 | (ext << 3) | r_rm); if (form) in.e[en++] = count;
              in.hlen = hn; in.elen = en; }
        }
        break; }
    case 8: { /* F7 group: /2 not /3 neg /4 mul /5 imul */
        uint8_t ext = (uint8_t)(2 + rnd() % 4);
        in.fmask = ext == 2 ? 0 : ext == 3 ? MASK_ALL : (FLAG_CF | FLAG_OF);
        { int hn = 0, en = 0;
          in.h[hn++] = 0x66; in.h[hn++] = 0xF7; in.h[hn++] = (uint8_t)(0xC0 | (ext << 3) | r_rm);
          in.e[en++] = 0xF7; in.e[en++] = (uint8_t)(0xC0 | (ext << 3) | r_rm);
          in.hlen = hn; in.elen = en; }
        break; }
    case 9: { /* imul r16,r16 / imul r16,r16,imm8 */
        uint8_t three = (uint8_t)(rnd() & 1);
        in.fmask = FLAG_CF | FLAG_OF;
        { int hn = 0, en = 0;
          if (three) {
              in.h[hn++] = 0x66; in.h[hn++] = 0x6B; in.h[hn++] = (uint8_t)(0xC0 | (r_reg << 3) | r_rm); in.h[hn++] = rnd8();
              in.e[en++] = 0x6B; in.e[en++] = (uint8_t)(0xC0 | (r_reg << 3) | r_rm); in.e[en++] = in.h[hn - 1];
          } else {
              in.h[hn++] = 0x66; in.h[hn++] = 0x0F; in.h[hn++] = 0xAF; in.h[hn++] = (uint8_t)(0xC0 | (r_reg << 3) | r_rm);
              in.e[en++] = 0x0F; in.e[en++] = 0xAF; in.e[en++] = (uint8_t)(0xC0 | (r_reg << 3) | r_rm);
          }
          in.hlen = hn; in.elen = en; }
        break; }
    case 10: { /* inc/dec r/m16 via FF (short 0x4x forms are REX on host64) */
        uint8_t ext = (uint8_t)(rnd() & 1); /* /0 inc /1 dec */
        in.fmask = MASK_ALL & ~FLAG_CF;
        { int hn = 0, en = 0;
          in.h[hn++] = 0x66; in.h[hn++] = 0xFF; in.h[hn++] = (uint8_t)(0xC0 | (ext << 3) | r_rm);
          in.e[en++] = 0xFF; in.e[en++] = (uint8_t)(0xC0 | (ext << 3) | r_rm);
          in.hlen = hn; in.elen = en; }
        break; }
    case 11: { /* xchg / xadd / cmpxchg r16 */
        uint8_t sel = (uint8_t)(rnd() % 3);
        { int hn = 0, en = 0;
          if (sel == 0) { in.fmask = 0;
              in.h[hn++] = 0x66; in.h[hn++] = 0x87; in.h[hn++] = (uint8_t)(0xC0 | (r_reg << 3) | r_rm);
              in.e[en++] = 0x87; in.e[en++] = (uint8_t)(0xC0 | (r_reg << 3) | r_rm);
          } else if (sel == 1) { in.fmask = MASK_ALL;
              in.h[hn++] = 0x66; in.h[hn++] = 0x0F; in.h[hn++] = 0xC1; in.h[hn++] = (uint8_t)(0xC0 | (r_reg << 3) | r_rm);
              in.e[en++] = 0x0F; in.e[en++] = 0xC1; in.e[en++] = (uint8_t)(0xC0 | (r_reg << 3) | r_rm);
          } else { in.fmask = MASK_ALL;
              in.h[hn++] = 0x66; in.h[hn++] = 0x0F; in.h[hn++] = 0xB1; in.h[hn++] = (uint8_t)(0xC0 | (r_reg << 3) | r_rm);
              in.e[en++] = 0x0F; in.e[en++] = 0xB1; in.e[en++] = (uint8_t)(0xC0 | (r_reg << 3) | r_rm);
          }
          in.hlen = hn; in.elen = en; }
        break; }
    case 12: { /* cmovcc r16,r16 */
        { int hn = 0, en = 0;
          uint8_t cc = (uint8_t)(0x40 | (rnd() % 16));
          in.fmask = 0;
          in.h[hn++] = 0x66; in.h[hn++] = 0x0F; in.h[hn++] = cc; in.h[hn++] = (uint8_t)(0xC0 | (r_reg << 3) | r_rm);
          in.e[en++] = 0x0F; in.e[en++] = cc; in.e[en++] = (uint8_t)(0xC0 | (r_reg << 3) | r_rm);
          in.hlen = hn; in.elen = en; }
        break; }
    case 13: { /* setcc r8, 16-bit-neutral encoding */
        { int hn = 0, en = 0;
          uint8_t cc = (uint8_t)(0x90 | (rnd() % 16));
          in.fmask = 0;
          in.h[hn++] = 0x0F; in.h[hn++] = cc; in.h[hn++] = (uint8_t)(0xC0 | r_rm);
          in.e[en++] = 0x0F; in.e[en++] = cc; in.e[en++] = (uint8_t)(0xC0 | r_rm);
          in.hlen = hn; in.elen = en; }
        break; }
    case 14: { /* movsx/movzx r16, r8/r16 */
        static const uint8_t OP2[4] = { 0xB6, 0xB7, 0xBE, 0xBF };
        { int hn = 0, en = 0;
          uint8_t o = OP2[rnd() % 4];
          in.fmask = 0;
          in.h[hn++] = 0x66; in.h[hn++] = 0x0F; in.h[hn++] = o; in.h[hn++] = (uint8_t)(0xC0 | (r_reg << 3) | r_rm);
          in.e[en++] = 0x0F; in.e[en++] = o; in.e[en++] = (uint8_t)(0xC0 | (r_reg << 3) | r_rm);
          in.hlen = hn; in.elen = en; }
        break; }
    case 15: { /* flag controls + nops (identical bytes in both worlds) */
        uint8_t sel = (uint8_t)(rnd() % 8);
        { int hn = 0, en = 0;
          switch (sel) {
          case 0: in.fmask = FLAG_CF; in.h[hn++] = 0xF5; break;         /* cmc */
          case 1: in.fmask = FLAG_CF; in.h[hn++] = 0xF8; break;         /* clc */
          case 2: in.fmask = FLAG_CF; in.h[hn++] = 0xF9; break;         /* stc */
          case 3: in.fmask = FLAG_DF; in.h[hn++] = 0xFC; break;         /* cld */
          case 4: in.fmask = FLAG_DF; in.h[hn++] = 0xFD; break;         /* std */
          case 5: in.fmask = 0;       in.h[hn++] = 0x90; break;         /* nop */
          case 6: in.fmask = 0;       in.h[hn++] = 0xF3; in.h[hn++] = 0x90; break; /* pause */
          default: in.fmask = 0;      in.h[hn++] = 0x0F; in.h[hn++] = 0xAE;
                   in.h[hn++] = (uint8_t)(rnd() & 1 ? 0xF0 : 0xF8); break;        /* m/sfence */
          }
          memcpy(in.e, in.h, (size_t)hn); en = hn;
          in.hlen = hn; in.elen = en; }
        break; }
    default: { /* test / mov r16,r16 / mov r16,imm16 */
        uint8_t sel = (uint8_t)(rnd() % 3);
        { int hn = 0, en = 0;
          if (sel == 0) { in.fmask = MASK_NO_AF;
              in.h[hn++] = 0x66; in.h[hn++] = 0x85; in.h[hn++] = (uint8_t)(0xC0 | (r_reg << 3) | r_rm);
              in.e[en++] = 0x85; in.e[en++] = (uint8_t)(0xC0 | (r_reg << 3) | r_rm);
          } else if (sel == 1) { in.fmask = 0;
              in.h[hn++] = 0x66; in.h[hn++] = 0x89; in.h[hn++] = (uint8_t)(0xC0 | (r_reg << 3) | r_rm);
              in.e[en++] = 0x89; in.e[en++] = (uint8_t)(0xC0 | (r_reg << 3) | r_rm);
          } else { in.fmask = 0;
              uint8_t lo = rnd8(), hi = rnd8();
              in.h[hn++] = 0x66; in.h[hn++] = (uint8_t)(0xB8 + r_reg); in.h[hn++] = lo; in.h[hn++] = hi;
              in.e[en++] = (uint8_t)(0xB8 + r_reg); in.e[en++] = lo; in.e[en++] = hi;
          }
          in.hlen = hn; in.elen = en; }
        break; }
    }
    return in;
}

/* ------------------------------------------------------ host execution -- */

typedef struct { uint64_t gpr[8]; uint64_t flags; } state_t;

static uint8_t *CODE;
static const size_t CODE_SZ = 0x10000;

static int host_run(const uint8_t *prog, size_t proglen, int steps,
                    const state_t *init, const uint64_t *syncs, state_t *states) {
    memcpy(CODE, prog, proglen);
    CODE[proglen] = 0xCC; /* int3: host-side terminator (userspace #BP frame
                             switches to the kernel stack, so the guest RSP
                             value is irrelevant here) */

    pid_t pid = fork();
    if (pid == 0) {
        ptrace(PTRACE_TRACEME, 0, 0, 0);
        raise(SIGSTOP);
        __asm__ volatile("jmp *%0\n" :: "r"((uint64_t)CODE) : "memory");
        __builtin_unreachable();
    }
    int ws;
    if (waitpid(pid, &ws, 0) < 0 || !WIFSTOPPED(ws)) goto dead;

    struct user_regs_struct r;
    struct iovec io = { .iov_base = &r, .iov_len = sizeof r };
    ptrace(PTRACE_GETREGSET, pid, (void *)NT_PRSTATUS, &io);
    r.rip = (uint64_t)CODE; /* SIGSTOP lands in libc; force IP into our page */
    r.rax = init->gpr[RAX]; r.rcx = init->gpr[RCX]; r.rdx = init->gpr[RDX];
    r.rbx = init->gpr[RBX]; r.rsp = init->gpr[RSP];
    r.rbp = init->gpr[RBP]; r.rsi = init->gpr[RSI]; r.rdi = init->gpr[RDI];
    r.eflags = init->flags | 0x2;
    ptrace(PTRACE_SETREGSET, pid, (void *)NT_PRSTATUS, &io);

    for (int i = 0; i < steps; i++) {
        /* per-instruction flag sync: both sides start with identical, fully
         * specified flag state (keeps cmovcc/setcc/adc inputs deterministic
         * even when an earlier opcode left some flags undefined) */
        r.eflags = (r.eflags & ~SYNC_MASK) | syncs[i] | 0x2;
        ptrace(PTRACE_SETREGSET, pid, (void *)NT_PRSTATUS, &io);
        if (ptrace(PTRACE_SINGLESTEP, pid, 0, 0) != 0) goto dead;
        if (waitpid(pid, &ws, 0) < 0 || !WIFSTOPPED(ws)) goto dead;
        ptrace(PTRACE_GETREGSET, pid, (void *)NT_PRSTATUS, &io);
        if (r.rip < (uint64_t)CODE || r.rip >= (uint64_t)CODE + CODE_SZ) goto dead;
        states[i].gpr[RAX] = r.rax; states[i].gpr[RCX] = r.rcx;
        states[i].gpr[RDX] = r.rdx; states[i].gpr[RBX] = r.rbx;
        states[i].gpr[RSP] = r.rsp; states[i].gpr[RBP] = r.rbp;
        states[i].gpr[RSI] = r.rsi; states[i].gpr[RDI] = r.rdi;
        states[i].flags = r.eflags;
    }
    kill(pid, SIGKILL);
    waitpid(pid, &ws, 0);
    return 0;
dead:
    kill(pid, SIGKILL);
    waitpid(pid, &ws, 0);
    return -1;
}

/* ---------------------------------------------------------- emulator ---- */

static void emu_init_state(machine_t *m, const state_t *s) {
    for (int i = 0; i < 8; i++) m->cpu.gpr[i] = s->gpr[i] & 0xFFFF;
    m->cpu.rflags = (s->flags &
        (FLAG_CF|FLAG_PF|FLAG_AF|FLAG_ZF|FLAG_SF|FLAG_DF|FLAG_OF)) | 0x2;
}

static int compare_step(int insn_idx, const insn_t *in,
                        const cpu_t *cpu, const state_t *hs) {
    static const char *RN[8] = { "ax", "cx", "dx", "bx", "sp", "bp", "si", "di" };
    int bad = 0;
    for (int r = 0; r < 8; r++)
        if ((uint16_t)cpu->gpr[r] != (uint16_t)hs->gpr[r]) {
            fprintf(stderr, "%s inn %d: emu %04llx host %04llx\n", RN[r], insn_idx,
                    (unsigned long long)cpu->gpr[r] & 0xFFFF,
                    (unsigned long long)hs->gpr[r] & 0xFFFF);
            bad = 1;
        }
    uint64_t ef = cpu->rflags, hf = hs->flags;
    if (((ef ^ hf) & in->fmask) != 0) {
        fprintf(stderr, "flags inn %d: emu %04llx host %04llx (mask %llx)\n",
                insn_idx, (unsigned long long)ef, (unsigned long long)hf,
                (unsigned long long)in->fmask);
        bad = 1;
    }
    if (bad) {
        fprintf(stderr, "Divergence at instruction %d: host bytes", insn_idx);
        for (int i = 0; i < in->hlen; i++) fprintf(stderr, " %02x", in->h[i]);
        fprintf(stderr, " |emu bytes");
        for (int i = 0; i < in->elen; i++) fprintf(stderr, " %02x", in->e[i]);
        fprintf(stderr, "\n");
    }
    return bad;
}

/* ------------------------------------------------------- crash mode ----- */

static long crash_rounds(int programs, long *faults, long *halts) {
    long checked = 0; *faults = *halts = 0;
    for (int p = 0; p < programs; p++) {
        machine_t m;
        setup_machine(&m);
        uint8_t code[40];
        int n = 1 + (int)(rnd() % 31);
        for (int i = 0; i < n; i++) code[i] = rnd8();
        memcpy(m.ram, code, (size_t)n);
        for (int i = 0; i < 64; i++)
            if (cpu_step(&m.cpu) != 0) break;
        checked++;
        if (m.cpu.fault) (*faults)++;
        else if (m.cpu.halted) (*halts)++;
        /* the loop above never crashing IS the invariant */
    }
    return checked;
}

/* ---------------------------------------------------------------- main --- */

int main(int argc, char **argv) {
    int programs = argc > 1 ? atoi(argv[1]) : 512;
    int perprog  = argc > 2 ? atoi(argv[2]) : 24;
    RNG_STATE    = argc > 3 ? strtoull(argv[3], NULL, 0) : 0xC11ull;
    if (!RNG_STATE) RNG_STATE = 0xC11ull;

    CODE = mmap(NULL, CODE_SZ, PROT_READ | PROT_WRITE | PROT_EXEC,
                MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (CODE == MAP_FAILED) { perror("mmap"); return 2; }
    memset(CODE, 0xCC, CODE_SZ);

    long compared = 0;
    for (int p = 0; p < programs; p++) {
        state_t init = {0};
        for (int r = 0; r < 8; r++) init.gpr[r] = rnd() & 0xFFFF;
        init.flags = rnd() & (FLAG_CF|FLAG_PF|FLAG_AF|FLAG_ZF|FLAG_SF|FLAG_OF);

        insn_t seq[128];
        uint8_t hprog[1024], eprog[1024];
        size_t hlen = 0, elen = 0;
        assert(perprog <= 128);
        for (int i = 0; i < perprog; i++) {
            seq[i] = gen_insn();
            memcpy(hprog + hlen, seq[i].h, (size_t)seq[i].hlen); hlen += (size_t)seq[i].hlen;
            memcpy(eprog + elen, seq[i].e, (size_t)seq[i].elen); elen += (size_t)seq[i].elen;
        }

        uint64_t syncs[128];
        for (int i = 0; i < perprog; i++) syncs[i] = rnd() & SYNC_MASK;

        state_t *hst = calloc((size_t)perprog, sizeof *hst);
        if (host_run(hprog, hlen, perprog, &init, syncs, hst) != 0) {
            fprintf(stderr, "host runner failed at program %d\n", p);
            free(hst);
            return 2;
        }

        machine_t m;
        setup_machine(&m);
        emu_init_state(&m, &init);
        memcpy(m.ram, eprog, elen);
        m.cpu.rip = 0;

        for (int i = 0; i < perprog; i++) {
            m.cpu.rflags = (m.cpu.rflags & ~SYNC_MASK) | syncs[i] | 0x2;
            if (cpu_step(&m.cpu) != 0) {
                fprintf(stderr,
                        "emulator faulted on program %d instruction %d (%s),"
                        " host stream accepted -- bytes:",
                        p, i, m.cpu.fault ? m.cpu.fault_msg : "halt");
                for (int k = 0; k < seq[i].elen; k++) fprintf(stderr, " %02x", seq[i].e[k]);
                fprintf(stderr, "\n");
                free(hst);
                return 1;
            }
            if (compare_step(i, &seq[i], &m.cpu, &hst[i])) {
                fprintf(stderr, "program %d (0-based); rerun: ./diff_fuzz %d %d 0x%llx\n",
                        p, programs, perprog, (unsigned long long)RNG_STATE);
                free(hst);
                return 1;
            }
            compared++;
        }
        free(hst);
    }

    long faults, halts;
    long crashed = crash_rounds(programs / 4 > 0 ? programs / 4 : 1, &faults, &halts);
    printf("diff fuzz: %d programs, %ld instructions compared -- all states match\n",
           programs, compared);
    printf("crash invariant: %ld garbage streams, %ld clean faults, %ld halts, "
           "0 host-level crashes\n", crashed, faults, halts);
    return 0;
}
