// disasm.h -- TOOLING T1: a pure x86 disassembler (16/32/64-bit).
//
// Output syntax is NASM as printed by `ndisasm` (lowercase mnemonics, 0x
// hex, "byte +0x1" sign-extended imm8, "[rel X]" RIP-relative targets,
// "jmp short"/"jz near" branch forms), so the nasm toolchain the project
// already depends on is the differential oracle (tests/disasm_diff.py).
// Coverage: the one-byte map, x87, the 0F map with its 66/F2/F3 SSE..SSE4
// forms, MMX, and the common 0F38/0F3A opcodes. VEX/EVEX/XOP/3DNow! are
// reported as undecoded ("db 0xNN", len 1, valid 0).
//
// dis_decode() has no machine dependency: callers hand it the bytes, the
// code size (16/32/64), and the address the bytes execute at (RIP/IP, i.e.
// the CS-relative offset), which is only used to print direct branch
// targets and RIP-relative operands the way ndisasm -o does.
#ifndef DISASM_H
#define DISASM_H
#include <stdint.h>
#include <stddef.h>

/* How the next instruction pointer relates to this instruction when it
 * completes without an exception/interrupt (T1 trace self-check). */
enum {
    DIS_CHK_NONE = 0,   /* indirect/far/ret/int/sys*: not checkable       */
    DIS_CHK_FALL,       /* next = pc + len                                */
    DIS_CHK_COND,       /* next = pc + len or target (jcc/loop/jcxz)      */
    DIS_CHK_TARGET      /* next = target (direct jmp/call)                */
};

typedef struct {
    int len;            /* bytes consumed (1..15); 0 only when avail == 0  */
    int valid;          /* 0 = undecoded: text "db 0xNN", len 1            */
    int truncated;      /* the bytes ran out before the instruction ended */
    int chk;            /* DIS_CHK_*                                       */
    uint64_t target;    /* direct branch target for CHK_COND/CHK_TARGET    */
    char text[160];
} dis_insn_t;

/* Decode one instruction. Returns out->len. */
int dis_decode(const uint8_t *buf, size_t avail, int bits, uint64_t pc,
               dis_insn_t *out);

#endif
