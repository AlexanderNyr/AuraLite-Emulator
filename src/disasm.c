// disasm.c -- TOOLING T1: table-driven x86 disassembler, ndisasm syntax.
//
// Tables use a small operand-spec language (Intel SDM appendix A letters):
//   E/G/M/R  r/m, reg-field GPR, memory-only, register-only r/m
//   Z        GPR in the low three opcode bits (+REX.B)
//   I/Is     immediate / sign-extended imm8 ("byte +0x1")
//   J        relative branch, O moffs, Ap far pointer immediate
//   V/W/U    xmm reg field, xmm-or-mem r/m, xmm r/m register
//   P/Q/N    mmx reg field, mmx-or-mem r/m, mmx r/m register
//   Sw Cd Dd segment / control / debug register in the reg field
//   AL CL DX AX eAX rAX ES..GS 1 ST STi   fixed operands
// sizes: b w d q t(80) o(128) v(opsize) z(16/32) y(32/64) p(far ptr) c(CR)
// suffix '!' forces the size keyword, '~' suppresses it.
// Mnemonic "a/b/c" picks by operand size 16/32/64; "name*" appends
// w/d/q only when the operand size differs from the mode default.
#include <stdio.h>
#include <string.h>
#include <stdarg.h>
#include "disasm.h"

typedef struct ent {
    const char *mn;
    const char *ops;
    unsigned fl;
    const struct ent *sub;
} ent_t;

#define F_I64   0x0001u   /* invalid in 64-bit mode                      */
#define F_O64   0x0002u   /* valid only in 64-bit mode                   */
#define F_D64   0x0004u   /* default operand size 64 in 64-bit mode      */
#define F_F64   0x0008u   /* near branch: 64-bit in 64-bit mode          */
#define F_STR   0x0010u   /* string op: rep/seg/a16 print as prefixes    */
#define F_CMPS  0x0020u   /* repe/repne                                  */
#define F_JCC   0x0040u
#define F_JMP   0x0080u
#define F_CALL  0x0100u
#define F_NOCHK 0x0200u   /* control transfer that cannot be checked     */
#define F_GRP   0x0400u   /* sub[8] by ModRM.reg                         */
#define F_GRPM  0x0800u   /* sub[16]: [reg] memory forms, [8+reg] reg    */
#define F_SSE   0x1000u   /* sub[4]: none, 66, F3, F2                    */
#define F_MODRM 0x2000u   /* has ModRM even without an r/m operand       */
#define F_W     0x4000u   /* "a/b" mnemonic picks by REX.W, not opsize   */
#define F_NOREP 0x8000u   /* rep prefix is consumed silently             */

#define N {NULL, NULL, 0, NULL}
#define I(m, o) {m, o, 0, NULL}
#define IF(m, o, f) {m, o, f, NULL}
#define G(t) {NULL, NULL, F_GRP, t}
#define GM(t) {NULL, NULL, F_GRPM, t}
#define S(t) {NULL, NULL, F_SSE, t}

/* ------------------------------------------------------------ groups --- */
#define ALU8(o) { I("add",o), I("or",o), I("adc",o), I("sbb",o), I("and",o), I("sub",o), I("xor",o), I("cmp",o) }
static const ent_t g80[8] = ALU8("Eb,Ib");
static const ent_t g81[8] = ALU8("Ev,Iz");
static const ent_t g83[8] = ALU8("Ev,Is");
static const ent_t g8f[8] = { IF("pop","Ev",F_D64), N, N, N, N, N, N, N };
#define SH8(o) { I("rol",o), I("ror",o), I("rcl",o), I("rcr",o), I("shl",o), I("shr",o), I("sal",o), I("sar",o) }
static const ent_t gc0[8] = SH8("Eb,Ib!");
static const ent_t gc1[8] = SH8("Ev,Ib!");
static const ent_t gd0[8] = SH8("Eb,1");
static const ent_t gd1[8] = SH8("Ev,1");
static const ent_t gd2[8] = SH8("Eb,CL");
static const ent_t gd3[8] = SH8("Ev,CL");
static const ent_t gf6[8] = { I("test","Eb,Ib"), I("test","Eb,Ib"), I("not","Eb"), I("neg","Eb"),
                              I("mul","Eb"), I("imul","Eb"), I("div","Eb"), I("idiv","Eb") };
static const ent_t gf7[8] = { I("test","Ev,Iz"), I("test","Ev,Iz"), I("not","Ev"), I("neg","Ev"),
                              I("mul","Ev"), I("imul","Ev"), I("div","Ev"), I("idiv","Ev") };
static const ent_t gfe[8] = { I("inc","Eb"), I("dec","Eb"), N, N, N, N, N, N };
static const ent_t gff[8] = { I("inc","Ev"), I("dec","Ev"), IF("call","Ev%",F_F64|F_NOCHK), IF("call","Mp",F_NOCHK),
                              IF("jmp","Ev%",F_F64|F_NOCHK), IF("jmp","Mp",F_NOCHK), IF("push","Ev",F_D64), N };
static const ent_t gc6[8] = { I("mov","Eb,Ib"), N, N, N, N, N, N, N };
static const ent_t gc7[8] = { I("mov","Ev,Iz"), N, N, N, N, N, N, N };

/* 0F 00 */
static const ent_t g0f00[8] = { I("sldt","Ev^"), I("str","Ev^"), I("lldt","Ew~"), I("ltr","Ew~"),
                                I("verr","Ew~"), I("verw","Ew~"), N, N };
/* 0F 01 memory forms (register forms are decoded by full ModRM) */
static const ent_t g0f01m[8] = { I("sgdt","M"), I("sidt","M"), I("lgdt","M"), I("lidt","M"),
                                 I("smsw","Ew~"), N, I("lmsw","Ew~"), I("invlpg","M") };
/* 0F 0D */
static const ent_t g0f0d[16] = { I("prefetch","M"), I("prefetchw","M"), I("prefetchwt1","Mb!"), N,
                                 N, N, N, N,
                                 N, N, N, N, N, N, N, N };
/* 0F 18 */
static const ent_t g0f18[16] = { I("prefetchnta","Mb!"), I("prefetcht0","Mb!"), I("prefetcht1","Mb!"), I("prefetcht2","Mb!"),
                                 N, N, N, N, N, N, N, N, N, N, N, N };
/* 0F 71/72/73 (register forms only) */
static const ent_t g71n[8] = { N, N, I("psrlw","Nq,Ib"), N, I("psraw","Nq,Ib"), N, I("psllw","Nq,Ib"), N };
static const ent_t g71x[8] = { N, N, I("psrlw","Ux,Ib"), N, I("psraw","Ux,Ib"), N, I("psllw","Ux,Ib"), N };
static const ent_t g72n[8] = { N, N, I("psrld","Nq,Ib"), N, I("psrad","Nq,Ib"), N, I("pslld","Nq,Ib"), N };
static const ent_t g72x[8] = { N, N, I("psrld","Ux,Ib"), N, I("psrad","Ux,Ib"), N, I("pslld","Ux,Ib"), N };
static const ent_t g73n[8] = { N, N, I("psrlq","Nq,Ib"), N, N, N, I("psllq","Nq,Ib"), N };
static const ent_t g73x[8] = { N, N, I("psrlq","Ux,Ib"), I("psrldq","Ux,Ib"), N, N, I("psllq","Ux,Ib"), I("pslldq","Ux,Ib") };
/* 0F AE */
static const ent_t g0faem[8] = { I("fxsave","M"), I("fxrstor","M"), I("ldmxcsr","Md!"), I("stmxcsr","Md!"),
                                 I("xsave","M"), I("xrstor","M"), I("xsaveopt","M"), I("clflush","M") };
/* 0F BA */
static const ent_t g0fba[8] = { N, N, N, N, I("bt","Ev,Ib!"), I("bts","Ev,Ib!"), I("btr","Ev,Ib!"), I("btc","Ev,Ib!") };
/* 0F C7 memory forms */
static const ent_t g0fc7m[8] = { N, I("cmpxchg8b","Mq!"), N, I("xrstors","M"), I("xsavec","M"), I("xsaves","M"),
                                 I("vmptrld","M"), I("vmptrst","M") };

/* --------------------------------------------------------- one-byte --- */
#define ALU(m) I(m,"Eb,Gb"), I(m,"Ev,Gv"), I(m,"Gb,Eb"), I(m,"Gv,Ev"), I(m,"AL,Ib"), I(m,"rAX,Iz")
#define JCC8(m) IF(m, "Jb", F_JCC)
static const ent_t op1[256] = {
/*00*/ ALU("add"), IF("push","ES",F_I64), IF("pop","ES",F_I64),
/*08*/ ALU("or"),  IF("push","CS",F_I64), N,
/*10*/ ALU("adc"), IF("push","SS",F_I64), IF("pop","SS",F_I64),
/*18*/ ALU("sbb"), IF("push","DS",F_I64), IF("pop","DS",F_I64),
/*20*/ ALU("and"), N, IF("daa","",F_I64),
/*28*/ ALU("sub"), N, IF("das","",F_I64),
/*30*/ ALU("xor"), N, IF("aaa","",F_I64),
/*38*/ ALU("cmp"), N, IF("aas","",F_I64),
/*40*/ IF("inc","Zv",F_I64), IF("inc","Zv",F_I64), IF("inc","Zv",F_I64), IF("inc","Zv",F_I64),
       IF("inc","Zv",F_I64), IF("inc","Zv",F_I64), IF("inc","Zv",F_I64), IF("inc","Zv",F_I64),
/*48*/ IF("dec","Zv",F_I64), IF("dec","Zv",F_I64), IF("dec","Zv",F_I64), IF("dec","Zv",F_I64),
       IF("dec","Zv",F_I64), IF("dec","Zv",F_I64), IF("dec","Zv",F_I64), IF("dec","Zv",F_I64),
/*50*/ IF("push","Zv",F_D64), IF("push","Zv",F_D64), IF("push","Zv",F_D64), IF("push","Zv",F_D64),
       IF("push","Zv",F_D64), IF("push","Zv",F_D64), IF("push","Zv",F_D64), IF("push","Zv",F_D64),
/*58*/ IF("pop","Zv",F_D64), IF("pop","Zv",F_D64), IF("pop","Zv",F_D64), IF("pop","Zv",F_D64),
       IF("pop","Zv",F_D64), IF("pop","Zv",F_D64), IF("pop","Zv",F_D64), IF("pop","Zv",F_D64),
/*60*/ IF("pusha*","",F_I64), IF("popa*","",F_I64), IF("bound","Gv,M",F_I64), IF("arpl","Ew,Gw",F_I64),
       N, N, N, N,
/*68*/ IF("push","Iz!",F_D64), I("imul","Gv,Ev,Iz#"), IF("push","Is",F_D64), I("imul","Gv,Ev,Is"),
       IF("insb","",F_STR), IF("insw/insd/insd","",F_STR), IF("outsb","",F_STR), IF("outsw/outsd/outsd","",F_STR),
/*70*/ JCC8("jo"), JCC8("jno"), JCC8("jc"), JCC8("jnc"), JCC8("jz"), JCC8("jnz"), JCC8("jna"), JCC8("ja"),
/*78*/ JCC8("js"), JCC8("jns"), JCC8("jpe"), JCC8("jpo"), JCC8("jl"), JCC8("jnl"), JCC8("jng"), JCC8("jg"),
/*80*/ G(g80), G(g81), N, G(g83), I("test","Eb,Gb"), I("test","Ev,Gv"), I("xchg","Eb,Gb"), I("xchg","Ev,Gv"),
/*88*/ I("mov","Eb,Gb"), I("mov","Ev,Gv"), I("mov","Gb,Eb"), I("mov","Gv,Ev"),
       I("mov","Ev&,Sw"), I("lea","Gv,M"), I("mov","Sw,Ev&"), G(g8f),
/*90*/ I("nop",""), I("xchg","rAX,Zv"), I("xchg","rAX,Zv"), I("xchg","rAX,Zv"),
       I("xchg","rAX,Zv"), I("xchg","rAX,Zv"), I("xchg","rAX,Zv"), I("xchg","rAX,Zv"),
/*98*/ I("cbw/cwde/cdqe",""), I("cwd/cdq/cqo",""), IF("call","Ap",F_I64|F_NOCHK), I("wait",""),
       IF("pushf*","",F_D64), IF("popf*","",F_D64), I("sahf",""), I("lahf",""),
/*A0*/ I("mov","AL,Ob"), I("mov","rAX,Ov"), I("mov","Ob,AL"), I("mov","Ov,rAX"),
       IF("movsb","",F_STR), IF("movsw/movsd/movsq","",F_STR),
       IF("cmpsb","",F_STR|F_CMPS), IF("cmpsw/cmpsd/cmpsq","",F_STR|F_CMPS),
/*A8*/ I("test","AL,Ib"), I("test","rAX,Iz"), IF("stosb","",F_STR), IF("stosw/stosd/stosq","",F_STR),
       IF("lodsb","",F_STR), IF("lodsw/lodsd/lodsq","",F_STR),
       IF("scasb","",F_STR|F_CMPS), IF("scasw/scasd/scasq","",F_STR|F_CMPS),
/*B0*/ I("mov","Zb,Ib"), I("mov","Zb,Ib"), I("mov","Zb,Ib"), I("mov","Zb,Ib"),
       I("mov","Zb,Ib"), I("mov","Zb,Ib"), I("mov","Zb,Ib"), I("mov","Zb,Ib"),
/*B8*/ I("mov","Zv,Iv"), I("mov","Zv,Iv"), I("mov","Zv,Iv"), I("mov","Zv,Iv"),
       I("mov","Zv,Iv"), I("mov","Zv,Iv"), I("mov","Zv,Iv"), I("mov","Zv,Iv"),
/*C0*/ G(gc0), G(gc1), IF("retn*","Iw",F_D64|F_NOCHK), IF("ret*","",F_D64|F_NOCHK),
       IF("les","Gv,M",F_I64), IF("lds","Gv,M",F_I64), G(gc6), G(gc7),
/*C8*/ I("enter","Iw,Ib"), IF("leave","",F_D64), IF("retf*","Iw",F_NOCHK), IF("retf*","",F_NOCHK),
       IF("int3","",F_NOCHK), IF("int","Ib",F_NOCHK), IF("into","",F_I64|F_NOCHK), IF("iret*","",F_NOCHK),
/*D0*/ G(gd0), G(gd1), G(gd2), G(gd3), IF("aam","Ib",F_I64), IF("aad","Ib",F_I64), IF("salc","",F_I64), I("xlatb",""),
/*D8*/ N, N, N, N, N, N, N, N,   /* x87: decoded by dis_x87() */
/*E0*/ IF("loopne","Jb",F_JCC), IF("loope","Jb",F_JCC), IF("loop","Jb",F_JCC), IF("jcxz/jecxz/jrcxz","Jb",F_JCC),
       I("in","AL,Ib"), I("in","eAX,Ib"), I("out","Ib,AL"), I("out","Ib,eAX"),
/*E8*/ IF("call","Jz",F_F64|F_CALL), IF("jmp","Jz",F_F64|F_JMP), IF("jmp","Ap",F_I64|F_NOCHK), IF("jmp short","Jb",F_JMP),
       I("in","AL,DX"), I("in","eAX,DX"), I("out","DX,AL"), I("out","DX,eAX"),
/*F0*/ N, IF("int1","",F_NOCHK), N, N, I("hlt",""), I("cmc",""), G(gf6), G(gf7),
/*F8*/ I("clc",""), I("stc",""), I("cli",""), I("sti",""), I("cld",""), I("std",""), G(gfe), G(gff),
};

/* ----------------------------------------------------- 0F SSE quads --- */
/* index: [0] no prefix, [1] 66, [2] F3, [3] F2 */
#define Q4(a,b,c,d) { a, b, c, d }
#define MMXSSE(m) Q4(I(m,"Pq,Qq"), I(m,"Vx,Wx"), N, N)
static const ent_t sse0f[256][4] = {
    [0x10] = Q4(I("movups","Vx,Wx"), I("movupd","Vx,Wx"), I("movss","Vx,Wd"), I("movsd","Vx,Wq")),
    [0x11] = Q4(I("movups","Wx,Vx"), I("movupd","Wx,Vx"), I("movss","Wd,Vx"), I("movsd","Wq,Vx")),
    [0x12] = Q4(I("movlps","Vx,Mq!"), I("movlpd","Vx,Mq!"), I("movsldup","Vx,Wx"), I("movddup","Vx,Wq")),
    [0x13] = Q4(I("movlps","Mq!,Vx"), I("movlpd","Mq!,Vx"), N, N),
    [0x14] = Q4(I("unpcklps","Vx,Wx"), I("unpcklpd","Vx,Wx"), N, N),
    [0x15] = Q4(I("unpckhps","Vx,Wx"), I("unpckhpd","Vx,Wx"), N, N),
    [0x16] = Q4(I("movhps","Vx,Mq!"), I("movhpd","Vx,Mq!"), I("movshdup","Vx,Wx"), N),
    [0x17] = Q4(I("movhps","Mq!,Vx"), I("movhpd","Mq!,Vx"), N, N),
    [0x28] = Q4(I("movaps","Vx,Wx"), I("movapd","Vx,Wx"), N, N),
    [0x29] = Q4(I("movaps","Wx,Vx"), I("movapd","Wx,Vx"), N, N),
    [0x2A] = Q4(I("cvtpi2ps","Vx,Qq!"), I("cvtpi2pd","Vx,Qq!"), I("cvtsi2ss","Vx,Ey"), I("cvtsi2sd","Vx,Ey")),
    [0x2B] = Q4(I("movntps","Mx!,Vx"), I("movntpd","Mx!,Vx"), N, N),
    [0x2C] = Q4(I("cvttps2pi","Pq,Wq"), I("cvttpd2pi","Pq,Wx"), I("cvttss2si","Gy,Wd~"), I("cvttsd2si","Gy,Wq~")),
    [0x2D] = Q4(I("cvtps2pi","Pq,Wq"), I("cvtpd2pi","Pq,Wx"), I("cvtss2si","Gy,Wd~"), I("cvtsd2si","Gy,Wq~")),
    [0x2E] = Q4(I("ucomiss","Vx,Wd"), I("ucomisd","Vx,Wq"), N, N),
    [0x2F] = Q4(I("comiss","Vx,Wd"), I("comisd","Vx,Wq"), N, N),
    [0x50] = Q4(I("movmskps","Gy,Ux"), I("movmskpd","Gd,Ux"), N, N),
    [0x51] = Q4(I("sqrtps","Vx,Wx"), I("sqrtpd","Vx,Wx"), I("sqrtss","Vx,Wd"), I("sqrtsd","Vx,Wq")),
    [0x52] = Q4(I("rsqrtps","Vx,Wx"), N, I("rsqrtss","Vx,Wd"), N),
    [0x53] = Q4(I("rcpps","Vx,Wx"), N, I("rcpss","Vx,Wd"), N),
    [0x54] = Q4(I("andps","Vx,Wx"), I("andpd","Vx,Wx"), N, N),
    [0x55] = Q4(I("andnps","Vx,Wx"), I("andnpd","Vx,Wx"), N, N),
    [0x56] = Q4(I("orps","Vx,Wx"), I("orpd","Vx,Wx"), N, N),
    [0x57] = Q4(I("xorps","Vx,Wx"), I("xorpd","Vx,Wx"), N, N),
    [0x58] = Q4(I("addps","Vx,Wx"), I("addpd","Vx,Wx"), I("addss","Vx,Wd"), I("addsd","Vx,Wq")),
    [0x59] = Q4(I("mulps","Vx,Wx"), I("mulpd","Vx,Wx"), I("mulss","Vx,Wd"), I("mulsd","Vx,Wq")),
    [0x5A] = Q4(I("cvtps2pd","Vx,Wq~"), I("cvtpd2ps","Vx,Wx~"), I("cvtss2sd","Vx,Wd"), I("cvtsd2ss","Vx,Wq")),
    [0x5B] = Q4(I("cvtdq2ps","Vx,Wx"), I("cvtps2dq","Vx,Wx"), I("cvttps2dq","Vx,Wx"), N),
    [0x5C] = Q4(I("subps","Vx,Wx"), I("subpd","Vx,Wx"), I("subss","Vx,Wd"), I("subsd","Vx,Wq")),
    [0x5D] = Q4(I("minps","Vx,Wx"), I("minpd","Vx,Wx"), I("minss","Vx,Wd"), I("minsd","Vx,Wq")),
    [0x5E] = Q4(I("divps","Vx,Wx"), I("divpd","Vx,Wx"), I("divss","Vx,Wd"), I("divsd","Vx,Wq")),
    [0x5F] = Q4(I("maxps","Vx,Wx"), I("maxpd","Vx,Wx"), I("maxss","Vx,Wd"), I("maxsd","Vx,Wq")),
    [0x60] = MMXSSE("punpcklbw"), [0x61] = MMXSSE("punpcklwd"), [0x62] = MMXSSE("punpckldq"),
    [0x63] = MMXSSE("packsswb"),  [0x64] = MMXSSE("pcmpgtb"),   [0x65] = MMXSSE("pcmpgtw"),
    [0x66] = MMXSSE("pcmpgtd"),   [0x67] = MMXSSE("packuswb"),  [0x68] = MMXSSE("punpckhbw"),
    [0x69] = MMXSSE("punpckhwd"), [0x6A] = MMXSSE("punpckhdq"), [0x6B] = MMXSSE("packssdw"),
    [0x6C] = Q4(N, I("punpcklqdq","Vx,Wx"), N, N),
    [0x6D] = Q4(N, I("punpckhqdq","Vx,Wx"), N, N),
    [0x6E] = Q4(IF("movd/movq","Pq,Ey!",F_W), IF("movd/movq","Vx,Ey!",F_W), N, N),
    [0x6F] = Q4(I("movq","Pq,Qq"), I("movdqa","Vx,Wx"), I("movdqu","Vx,Wx"), N),
    [0x70] = Q4(I("pshufw","Pq,Qq,Ib"), I("pshufd","Vx,Wx,Ib"), I("pshufhw","Vx,Wx,Ib"), I("pshuflw","Vx,Wx,Ib")),
    [0x74] = MMXSSE("pcmpeqb"), [0x75] = MMXSSE("pcmpeqw"), [0x76] = MMXSSE("pcmpeqd"),
    [0x77] = Q4(I("emms",""), N, N, N),
    [0x7C] = Q4(N, I("haddpd","Vx,Wx"), N, I("haddps","Vx,Wx")),
    [0x7D] = Q4(N, I("hsubpd","Vx,Wx"), N, I("hsubps","Vx,Wx")),
    [0x7E] = Q4(IF("movd/movq","Ey!,Pq",F_W), IF("movd/movq","Ey!,Vx",F_W), I("movq","Vx,Wq~"), N),
    [0x7F] = Q4(I("movq","Qq,Pq"), I("movdqa","Wx,Vx"), I("movdqu","Wx,Vx"), N),
    [0xB8] = Q4(N, N, I("popcnt","Gv,Ev!"), N),
    [0xBC] = Q4(I("bsf","Gv,Ev"), N, I("tzcnt","Gv,Ev!"), N),
    [0xBD] = Q4(I("bsr","Gv,Ev"), N, I("lzcnt","Gv,Ev!"), N),
    [0xC2] = Q4(I("cmpps","Vx,Wx,Ib"), I("cmppd","Vx,Wx,Ib"), I("cmpss","Vx,Wd,Ib"), I("cmpsd","Vx,Wq,Ib")),
    [0xC3] = Q4(I("movnti","My,Gy"), N, N, N),
    [0xC4] = Q4(I("pinsrw","Pq,Ew,Ib"), I("pinsrw","Vx,Ew,Ib"), N, N),
    [0xC5] = Q4(I("pextrw","Gd,Nq,Ib"), I("pextrw","Gd,Ux,Ib"), N, N),
    [0xC6] = Q4(I("shufps","Vx,Wx,Ib!"), I("shufpd","Vx,Wx,Ib!"), N, N),
    [0xD0] = Q4(N, I("addsubpd","Vx,Wx"), N, I("addsubps","Vx,Wx")),
    [0xD1] = MMXSSE("psrlw"), [0xD2] = MMXSSE("psrld"), [0xD3] = MMXSSE("psrlq"),
    [0xD4] = MMXSSE("paddq"), [0xD5] = MMXSSE("pmullw"),
    [0xD6] = Q4(N, I("movq","Wq~,Vx"), I("movq2dq","Vx,Nq"), I("movdq2q","Pq,Ux")),
    [0xD7] = Q4(I("pmovmskb","Gd,Nq"), I("pmovmskb","Gd,Ux"), N, N),
    [0xD8] = MMXSSE("psubusb"), [0xD9] = MMXSSE("psubusw"), [0xDA] = MMXSSE("pminub"),
    [0xDB] = MMXSSE("pand"),    [0xDC] = MMXSSE("paddusb"), [0xDD] = MMXSSE("paddusw"),
    [0xDE] = MMXSSE("pmaxub"),  [0xDF] = MMXSSE("pandn"),
    [0xE0] = MMXSSE("pavgb"),   [0xE1] = MMXSSE("psraw"),   [0xE2] = MMXSSE("psrad"),
    [0xE3] = MMXSSE("pavgw"),   [0xE4] = MMXSSE("pmulhuw"), [0xE5] = MMXSSE("pmulhw"),
    [0xE6] = Q4(N, I("cvttpd2dq","Vx,Wx"), I("cvtdq2pd","Vx,Wq"), I("cvtpd2dq","Vx,Wx")),
    [0xE7] = Q4(I("movntq","Mq,Pq"), I("movntdq","Mx,Vx"), N, N),
    [0xE8] = MMXSSE("psubsb"),  [0xE9] = MMXSSE("psubsw"),  [0xEA] = MMXSSE("pminsw"),
    [0xEB] = MMXSSE("por"),     [0xEC] = MMXSSE("paddsb"),  [0xED] = MMXSSE("paddsw"),
    [0xEE] = MMXSSE("pmaxsw"),  [0xEF] = MMXSSE("pxor"),
    [0xF0] = Q4(N, N, N, I("lddqu","Vx,Mx")),
    [0xF1] = MMXSSE("psllw"),   [0xF2] = MMXSSE("pslld"),   [0xF3] = MMXSSE("psllq"),
    [0xF4] = MMXSSE("pmuludq"), [0xF5] = MMXSSE("pmaddwd"), [0xF6] = MMXSSE("psadbw"),
    [0xF7] = Q4(I("maskmovq","Pq,Nq"), I("maskmovdqu","Vx,Ux"), N, N),
    [0xF8] = MMXSSE("psubb"),   [0xF9] = MMXSSE("psubw"),   [0xFA] = MMXSSE("psubd"),
    [0xFB] = MMXSSE("psubq"),   [0xFC] = MMXSSE("paddb"),   [0xFD] = MMXSSE("paddw"),
    [0xFE] = MMXSSE("paddd"),
};

/* ------------------------------------------------------------ 0F map --- */
#define JCCZ(m) IF(m, "Jz", F_F64|F_JCC)
#define CMOV(m) I(m, "Gv,Ev")
#define SETC(m) I(m, "Eb~")
#define SQ {NULL, NULL, F_SSE, NULL}   /* resolved through sse0f[] */
static const ent_t op0f[256] = {
/*00*/ G(g0f00), N, I("lar","Gv,Ew"), I("lsl","Gv,Ew"), N, IF("syscall","",F_NOCHK), I("clts",""), IF("sysret","",F_NOCHK),
/*08*/ I("invd",""), I("wbinvd",""), N, IF("ud2","",F_NOCHK), N, GM(g0f0d), I("femms",""), N,
/*10*/ SQ, SQ, SQ, SQ, SQ, SQ, SQ, SQ,
/*18*/ GM(g0f18), N, N, N, N, N, N, N,          /* 19-1F: hint nops, decoded inline */
/*20*/ I("mov","Rc,Cd"), I("mov","Rc,Dd"), I("mov","Cd,Rc"), I("mov","Dd,Rc"), N, N, N, N,
/*28*/ SQ, SQ, SQ, SQ, SQ, SQ, SQ, SQ,
/*30*/ I("wrmsr",""), I("rdtsc",""), I("rdmsr",""), I("rdpmc",""), IF("sysenter","",F_NOCHK), IF("sysexit","",F_NOCHK), N, I("getsec",""),
/*38*/ N, N, N, N, N, N, N, N,
/*40*/ CMOV("cmovo"), CMOV("cmovno"), CMOV("cmovc"), CMOV("cmovnc"), CMOV("cmovz"), CMOV("cmovnz"), CMOV("cmovna"), CMOV("cmova"),
/*48*/ CMOV("cmovs"), CMOV("cmovns"), CMOV("cmovpe"), CMOV("cmovpo"), CMOV("cmovl"), CMOV("cmovnl"), CMOV("cmovng"), CMOV("cmovg"),
/*50*/ SQ, SQ, SQ, SQ, SQ, SQ, SQ, SQ,
/*58*/ SQ, SQ, SQ, SQ, SQ, SQ, SQ, SQ,
/*60*/ SQ, SQ, SQ, SQ, SQ, SQ, SQ, SQ,
/*68*/ SQ, SQ, SQ, SQ, SQ, SQ, SQ, SQ,
/*70*/ SQ, N, N, N, SQ, SQ, SQ, SQ,             /* 71-73: shift groups inline */
/*78*/ IF("vmread","Ey!,Gy",F_D64), IF("vmwrite","Gy,Ey!",F_D64), N, N, SQ, SQ, SQ, SQ,
/*80*/ JCCZ("jo"), JCCZ("jno"), JCCZ("jc"), JCCZ("jnc"), JCCZ("jz"), JCCZ("jnz"), JCCZ("jna"), JCCZ("ja"),
/*88*/ JCCZ("js"), JCCZ("jns"), JCCZ("jpe"), JCCZ("jpo"), JCCZ("jl"), JCCZ("jnl"), JCCZ("jng"), JCCZ("jg"),
/*90*/ SETC("seto"), SETC("setno"), SETC("setc"), SETC("setnc"), SETC("setz"), SETC("setnz"), SETC("setna"), SETC("seta"),
/*98*/ SETC("sets"), SETC("setns"), SETC("setpe"), SETC("setpo"), SETC("setl"), SETC("setnl"), SETC("setng"), SETC("setg"),
/*A0*/ IF("push","FS",F_D64), IF("pop","FS",F_D64), I("cpuid",""), I("bt","Ev,Gv"), I("shld","Ev,Gv,Ib"), I("shld","Ev,Gv,CL"), N, N,
/*A8*/ IF("push","GS",F_D64), IF("pop","GS",F_D64), IF("rsm","",F_NOCHK), I("bts","Ev,Gv"), I("shrd","Ev,Gv,Ib"), I("shrd","Ev,Gv,CL"), N, I("imul","Gv,Ev"),
/*B0*/ I("cmpxchg","Eb,Gb"), I("cmpxchg","Ev,Gv"), I("lss","Gv,M"), I("btr","Ev,Gv"), I("lfs","Gv,M"), I("lgs","Gv,M"), I("movzx","Gv,Eb!"), I("movzx","Gv,Ew!"),
/*B8*/ SQ, I("ud1","Gv,Ev!"), G(g0fba), I("btc","Ev,Gv"), SQ, SQ, I("movsx","Gv,Eb!"), I("movsx","Gv,Ew!"),
/*C0*/ I("xadd","Eb,Gb"), I("xadd","Ev,Gv"), SQ, SQ, SQ, SQ, SQ, N,
/*C8*/ I("bswap","Zv"), I("bswap","Zv"), I("bswap","Zv"), I("bswap","Zv"), I("bswap","Zv"), I("bswap","Zv"), I("bswap","Zv"), I("bswap","Zv"),
/*D0*/ SQ, SQ, SQ, SQ, SQ, SQ, SQ, SQ,
/*D8*/ SQ, SQ, SQ, SQ, SQ, SQ, SQ, SQ,
/*E0*/ SQ, SQ, SQ, SQ, SQ, SQ, SQ, SQ,
/*E8*/ SQ, SQ, SQ, SQ, SQ, SQ, SQ, SQ,
/*F0*/ SQ, SQ, SQ, SQ, SQ, SQ, SQ, SQ,
/*F8*/ SQ, SQ, SQ, SQ, SQ, SQ, SQ, N,
};

/* ---------------------------------------------------------- 0F 38 / 3A --- */
#define SS3(m) Q4(I(m,"Pq,Qq"), I(m,"Vx,Wx"), N, N)
#define X66(m,o) Q4(N, I(m,o), N, N)
#define X66B(m,o) Q4(N, I(m,o "!"), N, N)
static const ent_t t0f38[256][4] = {
    [0x00] = SS3("pshufb"), [0x01] = SS3("phaddw"), [0x02] = SS3("phaddd"), [0x03] = SS3("phaddsw"),
    [0x04] = SS3("pmaddubsw"), [0x05] = SS3("phsubw"), [0x06] = SS3("phsubd"), [0x07] = SS3("phsubsw"),
    [0x08] = SS3("psignb"), [0x09] = SS3("psignw"), [0x0A] = SS3("psignd"), [0x0B] = SS3("pmulhrsw"),
    [0x10] = X66("pblendvb","Vx,Wx~,X0"), [0x14] = X66("blendvps","Vx,Wx~,X0"), [0x15] = X66("blendvpd","Vx,Wx~,X0"),
    [0x17] = X66("ptest","Vx,Wx"),
    [0x1C] = SS3("pabsb"), [0x1D] = SS3("pabsw"), [0x1E] = SS3("pabsd"),
    [0x20] = X66("pmovsxbw","Vx,Wq"), [0x21] = X66("pmovsxbd","Vx,Wd"), [0x22] = X66("pmovsxbq","Vx,Ww"),
    [0x23] = X66("pmovsxwd","Vx,Wq"), [0x24] = X66("pmovsxwq","Vx,Wd"), [0x25] = X66("pmovsxdq","Vx,Wq"),
    [0x28] = X66("pmuldq","Vx,Wx"), [0x29] = X66("pcmpeqq","Vx,Wx"), [0x2A] = X66("movntdqa","Vx,Mx!"),
    [0x2B] = X66("packusdw","Vx,Wx"),
    [0x30] = X66("pmovzxbw","Vx,Wq"), [0x31] = X66("pmovzxbd","Vx,Wd"), [0x32] = X66("pmovzxbq","Vx,Ww"),
    [0x33] = X66("pmovzxwd","Vx,Wq"), [0x34] = X66("pmovzxwq","Vx,Wd"), [0x35] = X66("pmovzxdq","Vx,Wq"),
    [0x37] = X66("pcmpgtq","Vx,Wx"),
    [0x38] = X66("pminsb","Vx,Wx"), [0x39] = X66("pminsd","Vx,Wx"), [0x3A] = X66("pminuw","Vx,Wx"),
    [0x3B] = X66("pminud","Vx,Wx"), [0x3C] = X66("pmaxsb","Vx,Wx"), [0x3D] = X66("pmaxsd","Vx,Wx"),
    [0x3E] = X66("pmaxuw","Vx,Wx"), [0x3F] = X66("pmaxud","Vx,Wx"),
    [0x40] = X66("pmulld","Vx,Wx"), [0x41] = X66("phminposuw","Vx,Wx"),
    [0x82] = X66("invpcid","Gc,Mx!"),
    [0xDB] = X66("aesimc","Vx,Wx"), [0xDC] = X66("aesenc","Vx,Wx"), [0xDD] = X66("aesenclast","Vx,Wx"),
    [0xDE] = X66("aesdec","Vx,Wx"), [0xDF] = X66("aesdeclast","Vx,Wx"),
    [0xF0] = Q4(I("movbe","Gv,Mv!"), N, N, I("crc32","Gy,Eb!")),
    [0xF1] = Q4(I("movbe","Mv!,Gv"), N, N, I("crc32","Gy,Ev!")),
};
static const ent_t t0f3a[256][4] = {
    [0x08] = X66B("roundps","Vx,Wx,Ib"), [0x09] = X66B("roundpd","Vx,Wx,Ib"),
    [0x0A] = X66B("roundss","Vx,Wd,Ib"), [0x0B] = X66B("roundsd","Vx,Wq,Ib"),
    [0x0C] = X66B("blendps","Vx,Wx,Ib"), [0x0D] = X66B("blendpd","Vx,Wx,Ib"),
    [0x0E] = X66B("pblendw","Vx,Wx,Ib"),
    [0x0F] = Q4(I("palignr","Pq,Qq,Ib"), I("palignr","Vx,Wx,Ib"), N, N),
    [0x14] = X66("pextrb","Ed/b,Vx,Ib!"), [0x15] = X66("pextrw","Ed/w,Vx,Ib!"),
    [0x16] = Q4(N, IF("pextrd/pextrq","Ey,Vx,Ib!",F_W), N, N), [0x17] = X66B("extractps","Ed,Vx,Ib"),
    [0x20] = X66("pinsrb","Vx,Eb,Ib!"), [0x21] = X66B("insertps","Vx,Wd,Ib"),
    [0x22] = Q4(N, IF("pinsrd/pinsrq","Vx,Ey,Ib!",F_W), N, N),
    [0x40] = X66B("dpps","Vx,Wx,Ib"), [0x41] = X66B("dppd","Vx,Wx,Ib"), [0x42] = X66B("mpsadbw","Vx,Wx,Ib"),
    [0x44] = X66("pclmulqdq","Vx,Wx,Ib"),
    [0x60] = X66B("pcmpestrm","Vx,Wx,Ib"), [0x61] = X66B("pcmpestri","Vx,Wx,Ib"),
    [0x62] = X66B("pcmpistrm","Vx,Wx,Ib"), [0x63] = X66B("pcmpistri","Vx,Wx,Ib"),
    [0xDF] = X66B("aeskeygenassist","Vx,Wx,Ib"),
};

/* ---------------------------------------------------------------- x87 --- */
typedef struct { const char *mn; const char *ops; } x87m_t;
static const x87m_t x87mem[8][8] = {
    { {"fadd","Md!"}, {"fmul","Md!"}, {"fcom","Md!"}, {"fcomp","Md!"}, {"fsub","Md!"}, {"fsubr","Md!"}, {"fdiv","Md!"}, {"fdivr","Md!"} },
    { {"fld","Md!"}, {NULL,NULL}, {"fst","Md!"}, {"fstp","Md!"}, {"fldenv","M"}, {"fldcw","M"}, {"fnstenv","M"}, {"fnstcw","M"} },
    { {"fiadd","Md!"}, {"fimul","Md!"}, {"ficom","Md!"}, {"ficomp","Md!"}, {"fisub","Md!"}, {"fisubr","Md!"}, {"fidiv","Md!"}, {"fidivr","Md!"} },
    { {"fild","Md!"}, {"fisttp","Md!"}, {"fist","Md!"}, {"fistp","Md!"}, {NULL,NULL}, {"fld","Mt!"}, {NULL,NULL}, {"fstp","Mt!"} },
    { {"fadd","Mq!"}, {"fmul","Mq!"}, {"fcom","Mq!"}, {"fcomp","Mq!"}, {"fsub","Mq!"}, {"fsubr","Mq!"}, {"fdiv","Mq!"}, {"fdivr","Mq!"} },
    { {"fld","Mq!"}, {"fisttp","Mq!"}, {"fst","Mq!"}, {"fstp","Mq!"}, {"frstor","M"}, {NULL,NULL}, {"fnsave","M"}, {"fnstsw","M"} },
    { {"fiadd","Mw!"}, {"fimul","Mw!"}, {"ficom","Mw!"}, {"ficomp","Mw!"}, {"fisub","Mw!"}, {"fisubr","Mw!"}, {"fidiv","Mw!"}, {"fidivr","Mw!"} },
    { {"fild","Mw!"}, {"fisttp","Mw!"}, {"fist","Mw!"}, {"fistp","Mw!"}, {"fbld","Mt!"}, {"fild","Mq!"}, {"fbstp","Mt!"}, {"fistp","Mq!"} },
};
/* register forms by [opcode-D8][reg]: "STi" one operand, "to STi" DC form */
static const x87m_t x87reg[8][8] = {
    { {"fadd","STi"}, {"fmul","STi"}, {"fcom","STi"}, {"fcomp","STi"}, {"fsub","STi"}, {"fsubr","STi"}, {"fdiv","STi"}, {"fdivr","STi"} },
    { {"fld","STi"}, {"fxch","STi"}, {NULL,NULL}, {NULL,NULL}, {NULL,NULL}, {NULL,NULL}, {NULL,NULL}, {NULL,NULL} },
    { {"fcmovb","STi"}, {"fcmove","STi"}, {"fcmovbe","STi"}, {"fcmovu","STi"}, {NULL,NULL}, {NULL,NULL}, {NULL,NULL}, {NULL,NULL} },
    { {"fcmovnb","STi"}, {"fcmovne","STi"}, {"fcmovnbe","STi"}, {"fcmovnu","STi"}, {NULL,NULL}, {"fucomi","STi"}, {"fcomi","STi"}, {NULL,NULL} },
    { {"fadd","toSTi"}, {"fmul","toSTi"}, {NULL,NULL}, {NULL,NULL}, {"fsubr","toSTi"}, {"fsub","toSTi"}, {"fdivr","toSTi"}, {"fdiv","toSTi"} },
    { {"ffree","STi"}, {NULL,NULL}, {"fst","STi"}, {"fstp","STi"}, {"fucom","STi"}, {"fucomp","STi"}, {NULL,NULL}, {NULL,NULL} },
    { {"faddp","STi"}, {"fmulp","STi"}, {NULL,NULL}, {NULL,NULL}, {"fsubrp","STi"}, {"fsubp","STi"}, {"fdivrp","STi"}, {"fdivp","STi"} },
    { {NULL,NULL}, {NULL,NULL}, {NULL,NULL}, {NULL,NULL}, {NULL,NULL}, {"fucomip","STi"}, {"fcomip","STi"}, {NULL,NULL} },
};
static const char *const d9e0[32] = {
    "fchs", "fabs", NULL, NULL, "ftst", "fxam", NULL, NULL,
    "fld1", "fldl2t", "fldl2e", "fldpi", "fldlg2", "fldln2", "fldz", NULL,
    "f2xm1", "fyl2x", "fptan", "fpatan", "fxtract", "fprem1", "fdecstp", "fincstp",
    "fprem", "fyl2xp1", "fsqrt", "fsincos", "frndint", "fscale", "fsin", "fcos" };

/* ------------------------------------------------------------ decoder --- */
enum { K_NONE, K_E, K_G, K_M, K_R, K_Z, K_I, K_IS, K_J, K_O, K_AP, K_FIX, K_SEG, K_SW,
       K_CR, K_DR, K_ONE, K_ST0, K_STI, K_V, K_W, K_U, K_P, K_Q, K_N, K_X0 };
enum { Z_NONE = 0, Z_B = 8, Z_W = 16, Z_D = 32, Z_Q = 64, Z_T = 80, Z_O = 128,
       Z_V = 1, Z_Z = 2, Z_Y = 3, Z_P = 4, Z_C = 5 };
enum { KW_DEF = 0, KW_SHOW, KW_HIDE };

typedef struct {
    int kind, size, kw, reg;    /* reg: fixed register index / seg index */
    int msize;                  /* memory-form size when it differs ("Ed/b") */
    int memfix;                 /* '^': memory form is a word, ignores 66   */
    int pfxkw;                  /* '%': keyword only with 66/REX.W present  */
    int immsz;                  /* '#': keyword/value by encoded imm size    */
    uint64_t imm;               /* immediate / moffs / far offset          */
    uint16_t sel;               /* far pointer selector                    */
    int isz;                    /* immediate byte count                    */
} opnd_t;

typedef struct {
    const uint8_t *b; size_t avail, pos; int trunc;
    int bits, opsize, adsize, defop;
    int p66, p67, rep, lock, seg, rex;
    int modrm_ok, mod, reg, rm;
    int mbase, midx, mscale, mrip, mnobase; int64_t mdisp; int mdispsz;
    uint64_t pc;
    int c66, c67, crep, cseg, crexw, clock;
    int nomop;                  /* operand size before D64/F64 defaults */
    int nops; opnd_t op[4];
    const ent_t *e;
    unsigned fl;
    const char *mn;
    uint8_t lastop;
    int sp_rmreg;               /* special path: every operand a ModRM/opcode register */
    char text[160]; size_t tl;
} ds_t;

static uint8_t fetch(ds_t *d) {
    if (d->pos >= d->avail || d->pos >= 15) { d->trunc = 1; d->pos++; return 0; }
    return d->b[d->pos++];
}
static uint64_t fetchn(ds_t *d, int n) {
    uint64_t v = 0;
    for (int i = 0; i < n; i++) v |= (uint64_t)fetch(d) << (8 * i);
    return v;
}
static void put(ds_t *d, const char *fmt, ...) {
    if (d->tl >= sizeof d->text - 1) return;
    va_list ap; va_start(ap, fmt);
    int n = vsnprintf(d->text + d->tl, sizeof d->text - d->tl, fmt, ap);
    va_end(ap);
    if (n > 0) d->tl += (size_t)n;
    if (d->tl >= sizeof d->text) d->tl = sizeof d->text - 1;
}

static const char *const r8l[8]  = {"al","cl","dl","bl","ah","ch","dh","bh"};
static const char *const r8x[16] = {"al","cl","dl","bl","spl","bpl","sil","dil",
                                    "r8b","r9b","r10b","r11b","r12b","r13b","r14b","r15b"};
static const char *const r16[16] = {"ax","cx","dx","bx","sp","bp","si","di",
                                    "r8w","r9w","r10w","r11w","r12w","r13w","r14w","r15w"};
static const char *const r32[16] = {"eax","ecx","edx","ebx","esp","ebp","esi","edi",
                                    "r8d","r9d","r10d","r11d","r12d","r13d","r14d","r15d"};
static const char *const r64[16] = {"rax","rcx","rdx","rbx","rsp","rbp","rsi","rdi",
                                    "r8","r9","r10","r11","r12","r13","r14","r15"};
static const char *const sregs[8] = {"es","cs","ss","ds","fs","gs","segr6","segr7"};

static const char *gpr(const ds_t *d, int idx, int sz) {
    switch (sz) {
    case 8:  return d->rex ? r8x[idx & 15] : r8l[idx & 7];
    case 16: return r16[idx & 15];
    case 32: return r32[idx & 15];
    default: return r64[idx & 15];
    }
}
static const char *kwname(int bits) {
    switch (bits) {
    case 8: return "byte"; case 16: return "word"; case 32: return "dword";
    case 64: return "qword"; case 80: return "tword"; case 128: return "oword";
    default: return NULL;
    }
}

static int rsize(const ds_t *d, int z) {
    switch (z) {
    case Z_V: return d->opsize;
    case Z_Z: return d->opsize == 16 ? 16 : 32;
    case Z_Y: return d->opsize == 64 ? 64 : 32;
    case Z_C: return d->bits == 64 ? 64 : 32;
    case Z_P: return d->opsize;
    default:  return z;
    }
}

/* parse one spec token into an operand descriptor */
static void spec_tok(const char *t, size_t n, opnd_t *o) {
    memset(o, 0, sizeof *o);
    o->kw = KW_DEF;
    for (;;) {
        if (n && t[n-1] == '!') { o->kw = KW_SHOW; n--; continue; }
        if (n && t[n-1] == '~') { o->kw = KW_HIDE; n--; continue; }
        if (n && t[n-1] == '^') { o->memfix = 1; n--; continue; }
        if (n && t[n-1] == '&') { o->memfix = 2; n--; continue; }
        if (n && t[n-1] == '%') { o->pfxkw = 1; n--; continue; }
        if (n && t[n-1] == '#') { o->kw = KW_SHOW; o->immsz = 1; n--; continue; }
        break;
    }
    if (n == 4 && t[2] == '/') {   /* "Ed/b": register size d, memory size b */
        char ms = t[3];
        o->msize = ms == 'b' ? Z_B : ms == 'w' ? Z_W : ms == 'd' ? Z_D : ms == 'q' ? Z_Q : 0;
        n = 2;
    }
#define IS(s) (n == strlen(s) && !memcmp(t, s, n))
    if (IS("AL"))  { o->kind = K_FIX; o->reg = 0; o->size = 8;  return; }
    if (IS("CL"))  { o->kind = K_FIX; o->reg = 1; o->size = 8;  o->kw = KW_HIDE; return; }
    if (IS("DX"))  { o->kind = K_FIX; o->reg = 2; o->size = 16; return; }
    if (IS("AX"))  { o->kind = K_FIX; o->reg = 0; o->size = 16; return; }
    if (IS("eAX")) { o->kind = K_FIX; o->reg = 0; o->size = Z_Z; return; }
    if (IS("rAX")) { o->kind = K_FIX; o->reg = 0; o->size = Z_V; return; }
    if (IS("ES"))  { o->kind = K_SEG; o->reg = 0; return; }
    if (IS("CS"))  { o->kind = K_SEG; o->reg = 1; return; }
    if (IS("SS"))  { o->kind = K_SEG; o->reg = 2; return; }
    if (IS("DS"))  { o->kind = K_SEG; o->reg = 3; return; }
    if (IS("FS"))  { o->kind = K_SEG; o->reg = 4; return; }
    if (IS("GS"))  { o->kind = K_SEG; o->reg = 5; return; }
    if (IS("1"))   { o->kind = K_ONE; return; }
    if (IS("ST"))  { o->kind = K_ST0; return; }
    if (IS("STi")) { o->kind = K_STI; return; }
    if (IS("Sw"))  { o->kind = K_SW; o->size = 16; return; }
    if (IS("Cd"))  { o->kind = K_CR; return; }
    if (IS("Dd"))  { o->kind = K_DR; return; }
    if (IS("Ap"))  { o->kind = K_AP; return; }
    if (IS("Is"))  { o->kind = K_IS; o->size = 8; return; }
    if (IS("X0"))  { o->kind = K_X0; return; }
#undef IS
    char k = t[0];
    char s = n > 1 ? t[1] : 0;
    switch (k) {
    case 'E': o->kind = K_E; break;  case 'G': o->kind = K_G; break;
    case 'M': o->kind = K_M; break;  case 'R': o->kind = K_R; break;
    case 'Z': o->kind = K_Z; break;  case 'I': o->kind = K_I; break;
    case 'J': o->kind = K_J; break;  case 'O': o->kind = K_O; break;
    case 'V': o->kind = K_V; break;  case 'W': o->kind = K_W; break;
    case 'U': o->kind = K_U; break;  case 'P': o->kind = K_P; break;
    case 'Q': o->kind = K_Q; break;  case 'N': o->kind = K_N; break;
    default: o->kind = K_NONE; break;
    }
    switch (s) {
    case 'b': o->size = Z_B; break; case 'w': o->size = Z_W; break;
    case 'd': o->size = Z_D; break; case 'q': o->size = Z_Q; break;
    case 't': o->size = Z_T; break; case 'o': case 'x': o->size = Z_O; break;
    case 'v': o->size = Z_V; break; case 'z': o->size = Z_Z; break;
    case 'y': o->size = Z_Y; break; case 'p': o->size = Z_P; break;
    case 'c': o->size = Z_C; break;
    default:  o->size = Z_NONE; break;
    }
    /* xmm/mmx operand sizes for Q/P/N are architecture-fixed */
    if ((o->kind == K_V || o->kind == K_U) && o->size != Z_NONE) o->size = Z_O;
}

static void spec_parse(ds_t *d, const char *spec) {
    d->nops = 0;
    if (!spec) return;
    const char *p = spec;
    while (*p && d->nops < 4) {
        const char *q = p;
        while (*q && *q != ',') q++;
        if (q > p) spec_tok(p, (size_t)(q - p), &d->op[d->nops++]);
        p = *q ? q + 1 : q;
    }
}

static int spec_needs_modrm(const ds_t *d) {
    for (int i = 0; i < d->nops; i++)
        switch (d->op[i].kind) {
        case K_E: case K_G: case K_M: case K_R: case K_SW: case K_CR: case K_DR:
        case K_V: case K_W: case K_U: case K_P: case K_Q: case K_N:
            return 1;
        default: break;
        }
    return 0;
}

/* ModRM + SIB + displacement, address-size aware */
static void parse_modrm(ds_t *d) {
    if (d->modrm_ok) return;
    uint8_t m = fetch(d);
    d->modrm_ok = 1;
    d->mod = m >> 6; d->reg = (m >> 3) & 7; d->rm = m & 7;
}
static void parse_mem(ds_t *d) {
    d->mbase = d->midx = -1; d->mscale = 1; d->mrip = 0; d->mnobase = 0;
    d->mdisp = 0; d->mdispsz = 0;
    if (d->mod == 3) return;
    if (d->adsize == 16) {
        static const int b16[8] = {3, 3, 5, 5, 6, 7, 5, 3};   /* bx bx bp bp si di bp bx */
        static const int i16[8] = {6, 7, 6, 7, -1, -1, -1, -1};
        if (d->mod == 0 && d->rm == 6) { d->mdisp = (int64_t)(uint16_t)fetchn(d, 2); d->mdispsz = 2; d->mnobase = 1; return; }
        d->mbase = b16[d->rm]; d->midx = i16[d->rm];
        if (d->mod == 1) { d->mdisp = (int8_t)fetch(d); d->mdispsz = 1; }
        else if (d->mod == 2) { d->mdisp = (int16_t)fetchn(d, 2); d->mdispsz = 2; }
        return;
    }
    int rexb = (d->rex & 1) ? 8 : 0, rexx = (d->rex & 2) ? 8 : 0;
    if (d->rm == 4) {
        uint8_t sib = fetch(d);
        int sc = sib >> 6, ix = ((sib >> 3) & 7) | rexx, bs = sib & 7;
        d->mscale = 1 << sc;
        if (ix != 4) d->midx = ix;
        if (bs == 5 && d->mod == 0) { d->mdisp = (int32_t)fetchn(d, 4); d->mdispsz = 4; d->mnobase = 1; }
        else d->mbase = bs | rexb;
    } else if (d->rm == 5 && d->mod == 0) {
        d->mdisp = (int32_t)fetchn(d, 4); d->mdispsz = 4; d->mnobase = 1;
        if (d->bits == 64) d->mrip = 1;
        return;
    } else {
        d->mbase = d->rm | rexb;
    }
    if (d->mod == 1) { d->mdisp = (int8_t)fetch(d); d->mdispsz = 1; }
    else if (d->mod == 2) { d->mdisp = (int32_t)fetchn(d, 4); d->mdispsz = 4; }
}

static void put_mem(ds_t *d, int kwbits, int far) {
    if (kwbits && kwname(kwbits)) put(d, "%s ", kwname(kwbits));
    if (far) put(d, "far ");
    put(d, "[");
    d->c67 = 1;
    if (d->seg >= 0) { put(d, "%s:", sregs[d->seg]); d->cseg = 1; }
    if (d->adsize == 16) {
        static const char *const n16[8] = {"ax","cx","dx","bx","sp","bp","si","di"};
        if (d->mnobase) {
            if (d->bits != 16) put(d, "word ");
            put(d, "0x%llx", (unsigned long long)(uint16_t)d->mdisp); put(d, "]"); return;
        }
        put(d, "%s", n16[d->mbase]);
        if (d->midx >= 0) put(d, "+%s", n16[d->midx]);
        if (d->mdispsz) {
            int64_t v = d->mdisp;
            if (v < 0) put(d, "-0x%llx", (unsigned long long)(-v));
            else put(d, "+0x%llx", (unsigned long long)v);
        }
        put(d, "]");
        return;
    }
    const char *const *rn = d->adsize == 64 ? r64 : r32;
    if (d->mrip) {
        uint64_t t = d->pc + d->pos + (uint64_t)d->mdisp;
        if (d->adsize == 32) { t &= 0xFFFFFFFFull; put(d, "dword "); }
        put(d, "rel 0x%llx]", (unsigned long long)t);
        return;
    }
    int any = 0;
    if (d->bits == 16 && d->adsize == 32 && d->rm == 4) put(d, "dword ");   /* SIB in 16-bit code */
    if (d->mbase >= 0) { put(d, "%s", rn[d->mbase]); any = 1; }
    if (d->midx >= 0) {
        put(d, "%s%s", any ? "+" : "", rn[d->midx]);
        if (d->mscale > 1) put(d, "*%d", d->mscale);
        any = 1;
    }
    if (!any) {
        /* absolute disp32 */
        uint64_t v = (uint64_t)d->mdisp;
        if (d->adsize == 32) v &= 0xFFFFFFFFull;
        if (d->rm == 5 && d->mod == 0 && d->bits != 64) put(d, "dword ");
        put(d, "0x%llx]", (unsigned long long)v);
        return;
    }
    if (d->mdispsz) {
        int64_t v = d->mdisp;
        if (d->adsize == 32) v = (int32_t)v;
        if (v < 0) put(d, "-0x%llx", (unsigned long long)(-(uint64_t)v));
        else put(d, "+0x%llx", (unsigned long long)v);
    }
    put(d, "]");
}

/* is some operand a register that fixes the memory operand's size? */
static int has_sizing_reg(const ds_t *d, int except) {
    for (int i = 0; i < d->nops; i++) {
        if (i == except) continue;
        const opnd_t *o = &d->op[i];
        switch (o->kind) {
        case K_G: case K_Z: case K_R: case K_SW: case K_CR: case K_DR:
        case K_V: case K_P: case K_U: case K_N: case K_ST0: case K_STI:
            return 1;
        case K_FIX: if (o->kw != KW_HIDE) return 1; break;
        case K_E: case K_W: case K_Q: if (d->mod == 3) return 1; break;
        default: break;
        }
    }
    return 0;
}

static int x87_or_special(ds_t *d, uint8_t op);

/* immediates for this operand list, in order */
static void parse_imms(ds_t *d) {
    for (int i = 0; i < d->nops; i++) {
        opnd_t *o = &d->op[i];
        switch (o->kind) {
        case K_I: {
            int sz = o->size == Z_B ? 1 : o->size == Z_W ? 2 :
                     o->size == Z_Z ? (d->opsize == 16 ? 2 : 4) :
                     o->size == Z_V ? d->opsize / 8 : 1;
            o->isz = sz; o->imm = fetchn(d, sz);
            break; }
        case K_IS: o->isz = 1; o->imm = fetch(d); break;
        case K_J: {
            int sz = o->size == Z_B ? 1 : (d->opsize == 16 ? 2 : 4);
            o->isz = sz; o->imm = fetchn(d, sz);
            break; }
        case K_O: o->isz = d->adsize / 8; o->imm = fetchn(d, o->isz); break;
        case K_AP: {
            int sz = d->opsize == 16 ? 2 : 4;
            o->isz = sz; o->imm = fetchn(d, sz); o->sel = (uint16_t)fetchn(d, 2);
            break; }
        default: break;
        }
    }
}

static uint64_t sext(uint64_t v, int bytes) {
    switch (bytes) {
    case 1: return (uint64_t)(int64_t)(int8_t)v;
    case 2: return (uint64_t)(int64_t)(int16_t)v;
    case 4: return (uint64_t)(int64_t)(int32_t)v;
    default: return v;
    }
}
static uint64_t maskbits(uint64_t v, int bits) {
    return bits >= 64 ? v : (v & ((1ull << bits) - 1));
}

static void put_operand(ds_t *d, int i, dis_insn_t *out) {
    opnd_t *o = &d->op[i];
    int rex_r = (d->rex & 4) ? 8 : 0, rex_b = (d->rex & 1) ? 8 : 0;
    switch (o->kind) {
    case K_FIX: {
        int sz = rsize(d, o->size);
        if (o->size == Z_V || o->size == Z_Z) d->c66 = 1;
        if (o->size == Z_V && sz == 64) d->crexw = 1;
        put(d, "%s", gpr(d, o->reg, sz));
        break; }
    case K_SEG: put(d, "%s", sregs[o->reg]); break;
    case K_SW: put(d, "%s", sregs[d->reg]); break;
    case K_CR: {
        if (d->bits == 64) d->c66 = 1;
        int cr = d->reg | rex_r;
        if (d->lock && !rex_r && d->bits != 64) { cr |= 8; d->clock = 1; }   /* AMD LOCK MOV CR8 alias */
        put(d, "cr%d", cr);
        break; }
    case K_DR: if (d->bits == 64) d->c66 = 1; put(d, "dr%d", d->reg | rex_r); break;
    case K_ONE: put(d, "1"); break;
    case K_ST0: put(d, "st0"); break;
    case K_STI: put(d, "st%d", d->rm); break;
    case K_X0: put(d, "xmm0"); break;
    case K_G: {
        int sz = rsize(d, o->size);
        if (o->size == Z_V || o->size == Z_Z || o->size == Z_Y) { d->c66 = 1; if (sz == 64) d->crexw = 1; }
        put(d, "%s", gpr(d, d->reg | rex_r, sz));
        break; }
    case K_Z: {
        int sz = rsize(d, o->size);
        if (o->size == Z_V) { d->c66 = 1; if (sz == 64) d->crexw = 1; }
        put(d, "%s", gpr(d, d->op[i].reg | rex_b, sz));
        break; }
    case K_R: case K_E: case K_M: {
        int sz = rsize(d, o->size);
        int isreg = (d->mod == 3 && o->kind != K_M);
        if ((o->size == Z_V || o->size == Z_Z || o->size == Z_Y) && (isreg || !o->memfix)) {
            d->c66 = 1; if (sz == 64) d->crexw = 1;
        }
        if (isreg) { put(d, "%s", gpr(d, d->rm | rex_b, sz)); break; }
        if (o->msize) sz = o->msize;
        if (o->memfix) {
            if (o->memfix == 2 && (d->rex & 8)) { sz = 64; o->kw = KW_SHOW; d->crexw = 1; }
            else { sz = 16; if (o->kw != KW_SHOW) o->kw = KW_HIDE; }
        }
        int far = (o->size == Z_P);
        int kw = 0;
        if (far) kw = (d->bits == 64 || d->p66) ? d->opsize : 0;
        else if (o->pfxkw) kw = (d->p66 || (d->rex & 8)) ? sz : 0;
        else if (o->kw == KW_SHOW) kw = sz;
        else if (o->kw == KW_DEF && o->size != Z_NONE && !has_sizing_reg(d, i)) kw = sz;
        if (far && (d->p66 || d->bits == 64)) { d->c66 = 1; d->crexw = 1; }
        put_mem(d, kw, far);
        break; }
    case K_V: put(d, "xmm%d", d->reg | rex_r); break;
    case K_U: put(d, "xmm%d", d->rm | rex_b); break;
    case K_P: put(d, "mm%d", d->reg); break;
    case K_N: put(d, "mm%d", d->rm); break;
    case K_W: case K_Q: {
        if (d->mod == 3) {
            if (o->kind == K_W) put(d, "xmm%d", d->rm | rex_b);
            else put(d, "mm%d", d->rm);
            break;
        }
        int kw = 0;
        if (o->kw == KW_SHOW) kw = o->size;
        else if (o->kw == KW_DEF && o->kind == K_W) kw = o->size;
        put_mem(d, kw, 0);
        break; }
    case K_I: {
        int sz = o->isz;
        uint64_t v = o->imm;
        int vbits = sz * 8;
        if (o->size == Z_Z || o->size == Z_V) {
            d->c66 = 1;
            if (d->opsize == 64) { d->crexw = 1; if (!o->immsz) { v = sext(v, sz); vbits = 64; } }
        }
        if (o->kw == KW_SHOW) {
            int kb = (o->size == Z_Z || o->size == Z_V) && !o->immsz ? d->opsize : sz * 8;
            put(d, "%s ", kwname(kb));
        }
        put(d, "0x%llx", (unsigned long long)maskbits(v, vbits));
        break; }
    case K_IS: {
        int8_t v = (int8_t)o->imm;
        if (v < 0) put(d, "byte -0x%x", (unsigned)(-(int)v));
        else put(d, "byte +0x%x", (unsigned)v);
        break; }
    case K_J: {
        uint64_t next = d->pc + d->pos;
        uint64_t t = next + sext(o->imm, o->isz);
        int nondef = (o->size == Z_Z && ((d->p66 && d->opsize != d->defop) || (d->rex & 8)));
        t = maskbits(t, o->size == Z_Z ? d->opsize : d->bits);
        out->target = t;
        if (nondef) { put(d, "%s ", kwname(d->opsize)); d->c66 = 1; d->crexw = 1; }
        else if (o->size == Z_Z && (d->fl & F_JCC)) put(d, "near ");
        put(d, "0x%llx", (unsigned long long)t);
        break; }
    case K_O: {
        uint64_t v = o->imm;
        put(d, "[");
        d->c67 = 1;
        if (d->seg >= 0) { put(d, "%s:", sregs[d->seg]); d->cseg = 1; }
        if (d->bits == 64 ? d->adsize == 64 : d->adsize != d->bits) put(d, "%s ", kwname(d->adsize));
        put(d, "0x%llx]", (unsigned long long)v);
        break; }
    case K_AP: {
        if (d->p66) { put(d, "%s ", kwname(d->opsize)); d->c66 = 1; }
        put(d, "0x%x:0x%llx", o->sel, (unsigned long long)o->imm);
        break; }
    default: break;
    }
}

static const char *pick_mnemonic(ds_t *d, char *buf, size_t n) {
    const char *mn = d->mn;
    const char *slash = strchr(mn, '/');
    size_t L = strlen(mn);
    if (slash) {
        const char *parts[3] = {mn, NULL, NULL};
        size_t lens[3] = {0, 0, 0};
        int np = 1;
        for (const char *p = mn; *p; p++) if (*p == '/' && np < 3) parts[np++] = p + 1;
        for (int i = 0; i < np; i++) {
            const char *e = (i + 1 < np) ? parts[i + 1] - 1 : mn + L;
            lens[i] = (size_t)(e - parts[i]);
        }
        int idx;
        if (d->fl & F_W) { idx = (d->rex & 8) ? 1 : 0; if (d->rex & 8) d->crexw = 1; }
        else {
            idx = d->opsize == 16 ? 0 : d->opsize == 32 ? 1 : 2;
            if (np == 2 && idx == 2) idx = 1;
            d->c66 = 1; if (d->opsize == 64) d->crexw = 1;
            if (d->fl & F_JCC) { /* jcxz family: by address size */
                idx = d->adsize == 16 ? 0 : d->adsize == 32 ? 1 : 2;
                d->c67 = 1; d->c66 = (d->adsize == 64); d->crexw = 1;
            }
        }
        if (idx >= np) idx = np - 1;
        snprintf(buf, n, "%.*s", (int)lens[idx], parts[idx]);
        return buf;
    }
    if (L && mn[L-1] == '*') {
        const char *suf = "";
        if (d->opsize != d->defop || (d->rex & 8)) suf = d->opsize == 16 ? "w" : d->opsize == 32 ? "d" : "q";
        else if (!strcmp(mn, "retn*")) { L = 4; }   /* default size: plain "ret" */
        if (!strcmp(mn, "retn*") && d->opsize != 16) L = 4;     /* ndisasm: retnw but retd/retq */
        d->c66 = 1; if (d->opsize == 64) d->crexw = 1;
        snprintf(buf, n, "%.*s%s", (int)(L - 1), mn, suf);
        return buf;
    }
    return mn;
}

/* does this table entry operate on xmm/mmx registers? */
static int is_vec(const ent_t *e) {
    if (!e || !e->ops) return 0;
    for (const char *p = e->ops; *p; ) {
        if (strchr("VWUPQN", *p) || !strncmp(p, "X0", 2)) return 1;
        while (*p && *p != ',') p++;
        if (*p) p++;
    }
    return 0;
}
/* mandatory-prefix selection for a {none,66,F3,F2} quad. Returns NULL for
 * an encoding with no defined form. */
static const ent_t *pick_quad(ds_t *d, const ent_t *q, int strict) {
    if (d->rep == 0xF3 || d->rep == 0xF2) {
        int i = d->rep == 0xF3 ? 2 : 3;
        if (q[i].mn) { d->crep = 1; return &q[i]; }
        if (strict || is_vec(&q[0]) || is_vec(&q[1])) return NULL;   /* reserved */
    }
    if (d->p66) {
        if (q[1].mn) { d->c66 = 1; d->opsize = (d->bits == 64 && (d->rex & 8)) ? 64 : d->defop; return &q[1]; }
        if (is_vec(&q[0])) return NULL;
    }
    return q[0].mn ? &q[0] : NULL;
}

/* ndisasm prints an unused 67 prefix ("a16"/"a32") unless some operand is
 * not a ModRM/opcode-encoded register; in 32/64-bit code every operand
 * counts (each carries a SEG_32BIT/SEG_64BIT flag in its operand record). */
static int a_unused_prints(const ds_t *d) {
    if (d->nops == 0) return 1;
    if (d->bits != 16) return 0;
    for (int i = 0; i < d->nops; i++) {
        switch (d->op[i].kind) {
        case K_G: case K_Z: case K_V: case K_P: case K_SW: case K_CR: case K_DR: case K_STI:
            break;
        case K_E: case K_R: case K_W: case K_Q: case K_U: case K_N:
            if (d->mod != 3) return 0;
            break;
        default:
            return 0;
        }
    }
    return 1;
}

static int spec_is_jz(const char *ops) { return ops && !strcmp(ops, "Jz"); }

static void set_invalid(ds_t *d, dis_insn_t *out) {
    out->valid = 0; out->len = 1; out->chk = DIS_CHK_NONE; out->target = 0;
    snprintf(out->text, sizeof out->text, "db 0x%02x", d->avail ? d->b[0] : 0);
    out->truncated = d->trunc;
}

int dis_decode(const uint8_t *buf, size_t avail, int bits, uint64_t pc, dis_insn_t *out) {
    memset(out, 0, sizeof *out);
    if (!avail) return 0;
    ds_t D; memset(&D, 0, sizeof D);
    ds_t *d = &D;
    d->b = buf; d->avail = avail; d->bits = bits; d->pc = pc; d->seg = -1;

    /* ---- legacy prefixes and REX ---- */
    for (;;) {
        if (d->pos >= d->avail || d->pos >= 14) break;
        uint8_t x = d->b[d->pos];
        int legacy = 1;
        switch (x) {
        case 0x66: d->p66 = 1; break;
        case 0x67: d->p67 = 1; break;
        case 0xF0: d->lock = 1; break;
        case 0xF2: case 0xF3: d->rep = x; break;
        case 0x26: d->seg = 0; break; case 0x2E: d->seg = 1; break;
        case 0x36: d->seg = 2; break; case 0x3E: d->seg = 3; break;
        case 0x64: d->seg = 4; break; case 0x65: d->seg = 5; break;
        default: legacy = 0; break;
        }
        if (legacy) { d->rex = 0; d->pos++; continue; }
        if (bits == 64 && (x & 0xF0) == 0x40) { d->rex = x; d->pos++; continue; }
        break;
    }
    d->defop = bits == 16 ? 16 : 32;
    d->opsize = d->p66 ? (bits == 16 ? 32 : 16) : d->defop;
    if (bits == 64 && (d->rex & 8)) d->opsize = 64;
    d->nomop = d->opsize;
    d->adsize = bits;
    if (d->p67) d->adsize = bits == 32 ? 16 : 32;

    size_t oppos = d->pos;
    uint8_t op = fetch(d);
    if (d->trunc) { set_invalid(d, out); return out->len; }

    const ent_t *e = NULL;
    int map = 1; uint8_t op2 = 0;
    out->chk = DIS_CHK_FALL;

    if (op == 0x0F) {
        op2 = fetch(d);
        map = 2;
        if (op2 == 0x38 || op2 == 0x3A) {
            uint8_t op3 = fetch(d);
            const ent_t *q = op2 == 0x38 ? t0f38[op3] : t0f3a[op3];
            int was66 = d->p66;
            e = pick_quad(d, q, 1);
            if (e && e == &q[3] && was66) { d->opsize = 16; d->c66 = 1; }   /* crc32 r32, r/m16 */
            map = 3;
            d->lastop = op3;
        } else if (op2 == 0x0F) {
            e = NULL;                       /* 3DNow!: out of scope */
        } else {
            e = &op0f[op2];
            d->lastop = op2;
            if (e->fl & F_SSE) e = pick_quad(d, sse0f[op2], 0);
        }
    } else {
        e = &op1[op];
        d->lastop = op;
    }

    /* ---- irregular encodings ---- */
    if (map == 1) {
        int r = x87_or_special(d, op);
        if (r == 1) goto format_done;      /* fully handled */
        if (r == -1) { set_invalid(d, out); return out->len; }
        if (r == 2) e = d->e;               /* replaced entry */
    } else if (map == 2) {
        if (op2 >= 0x19 && op2 <= 0x1F) {
            parse_modrm(d); parse_mem(d);
            int rexb = (d->rex & 1) ? 8 : 0;
            if (op2 == 0x1E && d->rep == 0xF3 && d->mod == 3 && d->reg == 7 && (d->rm == 2 || d->rm == 3)) {
                snprintf(d->text, sizeof d->text, "%s", d->rm == 2 ? "endbr64" : "endbr32");
                d->tl = strlen(d->text); d->crep = 1;
                goto format_done;
            }
            if (d->rep == 0xF3 && op2 == 0x1E && d->mod == 3 && d->reg == 1) { set_invalid(d, out); return out->len; }
            const char *mnem;
            char nb[24];
            if (op2 == 0x1F && d->reg == 0) mnem = "nop";
            else { snprintf(nb, sizeof nb, "hint_nop%d", (op2 - 0x18) * 8 + d->reg); mnem = nb; }
            put(d, "%s ", mnem);
            if (d->mod == 3) { put(d, "%s", gpr(d, d->rm | rexb, d->opsize)); d->sp_rmreg = 1; }
            else put_mem(d, d->opsize, 0);
            d->c66 = 1; if (d->opsize == 64) d->crexw = 1;
            goto format_done;
        }
        if (op2 == 0x71 || op2 == 0x72 || op2 == 0x73) {
            parse_modrm(d);
            if (d->mod != 3 || d->rep) { set_invalid(d, out); return out->len; }
            const ent_t *g;
            int x = d->p66 ? 1 : 0;
            if (op2 == 0x71) g = x ? g71x : g71n; else if (op2 == 0x72) g = x ? g72x : g72n; else g = x ? g73x : g73n;
            if (x) d->c66 = 1;
            e = &g[d->reg];
        }
        if (op2 == 0x01) {
            parse_modrm(d);
            if (d->mod != 3) e = &g0f01m[d->reg];
            else {
                uint8_t mm = (uint8_t)(0xC0 | (d->reg << 3) | d->rm);
                const char *s = NULL;
                switch (mm) {
                case 0xC1: s = "vmcall"; break; case 0xC2: s = "vmlaunch"; break;
                case 0xC3: s = "vmresume"; break; case 0xC4: s = "vmxoff"; break;
                case 0xC8: s = "monitor"; break; case 0xC9: s = "mwait"; break;
                case 0xCA: s = "clac"; break; case 0xCB: s = "stac"; break;
                case 0xD0: s = "xgetbv"; break; case 0xD1: s = "xsetbv"; break;
                case 0xD4: s = "vmfunc"; break; case 0xD5: s = "xend"; break; case 0xD6: s = "xtest"; break;
                case 0xD8: s = "vmrun"; break; case 0xD9: s = "vmmcall"; break;
                case 0xDA: s = "vmload"; break; case 0xDB: s = "vmsave"; break;
                case 0xDC: s = "stgi"; break; case 0xDD: s = "clgi"; break;
                case 0xDE: s = "skinit"; break; case 0xDF: s = "invlpga"; break;
                case 0xEE: s = "rdpkru"; break; case 0xEF: s = "wrpkru"; break;
                case 0xF8: s = bits == 64 ? "swapgs" : NULL; break;
                case 0xF9: s = "rdtscp"; break;
                case 0xFA: s = "monitorx"; break; case 0xFB: s = "mwaitx"; break;
                case 0xFC: s = "clzero"; break;
                default: break;
                }
                if (s && d->rep && mm == 0xC1) { set_invalid(d, out); return out->len; }
                if (s) { put(d, "%s", s); goto format_done; }
                if (d->reg == 4) { static const ent_t smsw = I("smsw","Rv"); e = &smsw; }
                else if (d->reg == 6) { static const ent_t lmsw = I("lmsw","Rw"); e = &lmsw; }
                else { set_invalid(d, out); return out->len; }
            }
        }
        if (op2 == 0xAE) {
            parse_modrm(d);
            if (d->mod != 3) {
                e = &g0faem[d->reg];
                if ((d->rex & 8) && bits == 64 && !d->p66 && !d->rep && (d->reg <= 1 || (d->reg >= 4 && d->reg <= 6))) {
                    static const char *const n64[8] = {"fxsave64","fxrstor64",NULL,NULL,"xsave64","xrstor64","xsaveopt64",NULL};
                    put(d, "%s ", n64[d->reg]); parse_mem(d); put_mem(d, 0, 0); d->crexw = 1;
                    goto format_done;
                }
                if (d->reg == 7 && d->p66 && !d->rep) { static const ent_t cfo = I("clflushopt","M"); e = &cfo; d->c66 = 1; }
                else if (d->p66 || d->rep) { set_invalid(d, out); return out->len; }
            } else if (d->rep == 0xF3 && bits == 64 && d->reg < 4) {
                static const ent_t fsgs[4] = { I("rdfsbase","Ry"), I("rdgsbase","Ry"), I("wrfsbase","Ry"), I("wrgsbase","Ry") };
                e = &fsgs[d->reg]; d->crep = 1;
            } else {
                const char *s = d->reg == 5 ? "lfence" : d->reg == 6 ? "mfence" : d->reg == 7 ? "sfence" : NULL;
                if (!s || d->rm != 0 || d->rep) { set_invalid(d, out); return out->len; }
                put(d, "%s", s);
                goto format_done;
            }
        }
        if (op2 == 0xC7) {
            parse_modrm(d);
            if (d->mod != 3) {
                e = &g0fc7m[d->reg];
                if ((d->rex & 8) && bits == 64 && d->reg >= 3 && d->reg <= 5 && !d->p66 && !d->rep) {
                    static const char *const n64[3] = {"xrstors64","xsavec64","xsaves64"};
                    put(d, "%s ", n64[d->reg - 3]); parse_mem(d); put_mem(d, 0, 0); d->crexw = 1;
                    goto format_done;
                }
                if (d->reg == 1 && (d->rex & 8)) { static const ent_t c16 = I("cmpxchg16b","Mo!"); e = &c16; d->crexw = 1; }
                if (d->reg == 6 && d->p66) { static const ent_t vc = I("vmclear","M"); e = &vc; d->c66 = 1; }
                if (d->reg == 6 && d->rep == 0xF3) { static const ent_t vx = I("vmxon","M"); e = &vx; d->crep = 1; }
            } else if (d->reg == 7 && d->rep == 0xF3) {
                put(d, "rdpid %s", gpr(d, d->rm | ((d->rex & 1) ? 8 : 0), bits == 64 ? 64 : 32));
                d->crep = 1; d->crexw = 1;
                goto format_done;
            } else if (d->rep) { set_invalid(d, out); return out->len; }
            else if (d->reg == 6) { static const ent_t rr = I("rdrand","Rv"); e = &rr; }
            else if (d->reg == 7) { static const ent_t rs = I("rdseed","Rv"); e = &rs; }
            else e = NULL;
        }
        if (op2 == 0x12 && e && !strcmp(e->mn, "movlps")) {
            parse_modrm(d);
            if (d->mod == 3) { static const ent_t hl = I("movhlps","Vx,Ux"); e = &hl; }
        }
        if (op2 == 0x16 && e && !strcmp(e->mn, "movhps")) {
            parse_modrm(d);
            if (d->mod == 3) { static const ent_t lh = I("movlhps","Vx,Ux"); e = &lh; }
        }
        if (op2 >= 0xC8 && op2 <= 0xCF && d->opsize == 16) { set_invalid(d, out); return out->len; }
        if ((op2 == 0xB7 || op2 == 0xBF) && d->opsize == 16) { set_invalid(d, out); return out->len; }
        if ((op2 == 0x78 || op2 == 0x79) && (d->p66 || d->rep)) { set_invalid(d, out); return out->len; }
        if (op2 == 0x37 && (d->p66 || d->p67 || d->rep)) { set_invalid(d, out); return out->len; }
        if (op2 == 0xC3 && (d->p66 || d->rep)) { set_invalid(d, out); return out->len; }
        if (op2 == 0x09 && d->rep == 0xF3) { put(d, "wbnoinvd"); d->crep = 1; goto format_done; }
        if (op2 == 0x18) {
            parse_modrm(d); parse_mem(d);
            if (!d->p66 && !(d->rex & 8) && d->mod != 3 && d->reg < 4) {
                static const char *const pf[4] = {"prefetchnta","prefetcht0","prefetcht1","prefetcht2"};
                put(d, "%s ", pf[d->reg]); put_mem(d, 8, 0);
                goto format_done;
            }
            int rexb = (d->rex & 1) ? 8 : 0;
            put(d, "hint_nop%d ", d->reg);
            if (d->mod == 3) { put(d, "%s", gpr(d, d->rm | rexb, d->opsize)); d->sp_rmreg = 1; }
            else put_mem(d, d->opsize, 0);
            d->c66 = 1; if (d->opsize == 64) d->crexw = 1;
            goto format_done;
        }
    }

    if (!e || (!e->mn && !(e->fl & (F_GRP | F_GRPM)))) { set_invalid(d, out); return out->len; }
    if (e->fl & (F_GRP | F_GRPM)) {
        parse_modrm(d);
        const ent_t *g = e->sub;
        unsigned pfl = e->fl;
        e = (pfl & F_GRPM) ? (d->mod == 3 ? &g[8 + d->reg] : &g[d->reg]) : &g[d->reg];
        if (!e->mn) { set_invalid(d, out); return out->len; }
    }
    d->e = e; d->fl = e->fl; d->mn = e->mn;
    if ((d->fl & F_I64) && bits == 64) { set_invalid(d, out); return out->len; }
    if ((d->fl & F_O64) && bits != 64) { set_invalid(d, out); return out->len; }

    if (bits == 64 && (d->fl & (F_D64 | F_F64))) {
        int relbr = (d->fl & (F_JMP | F_CALL | F_JCC)) && spec_is_jz(e->ops);
        if ((d->fl & F_F64) && !relbr) d->opsize = 64;
        else d->opsize = d->p66 ? 16 : 64;
        if (d->rex & 8) d->opsize = 64;
        d->defop = 64;
    }

    spec_parse(d, e->ops);
    for (int i = 0; i < d->nops; i++) if (d->op[i].kind == K_Z) d->op[i].reg = d->lastop & 7;
    if (spec_needs_modrm(d) || (d->fl & F_MODRM)) {
        parse_modrm(d);
        parse_mem(d);
        for (int i = 0; i < d->nops; i++) {
            int k = d->op[i].kind;
            if ((k == K_M) && d->mod == 3) { set_invalid(d, out); return out->len; }
            if ((k == K_R || k == K_U || k == K_N) && d->mod != 3) { set_invalid(d, out); return out->len; }
        }
        if ((op == 0x8C || op == 0x8E) && map == 1 && d->reg > 5) { set_invalid(d, out); return out->len; }
        if (op == 0x8E && map == 1 && d->reg == 1) { set_invalid(d, out); return out->len; }
    } else if (d->modrm_ok) {
        parse_mem(d);
    }
    parse_imms(d);
    if (map == 2 && (d->lastop == 0xB6 || d->lastop == 0xBE) && d->opsize == 16) d->op[1].kw = KW_HIDE;

    /* ---- operand text ---- */
    {
        char mb[32];
        const char *mn = pick_mnemonic(d, mb, sizeof mb);
        char body[160]; size_t bl;
        d->tl = 0; d->text[0] = 0;
        put(d, "%s", mn);
        for (int i = 0; i < d->nops; i++) {
            put(d, i ? "," : " ");
            put_operand(d, i, out);
        }
        /* aam/aad: ndisasm omits the default base 10 */
        if ((op == 0xD4 || op == 0xD5) && map == 1 && d->op[0].imm == 0x0A) {
            d->tl = 0; d->text[0] = 0; put(d, "%s", op == 0xD4 ? "aam" : "aad");
        }
        /* loop/jcxz with a non-default address size name the count register */
        if (map == 1 && op >= 0xE0 && op <= 0xE2 && d->adsize != d->bits) {
            put(d, ",%s", d->adsize == 16 ? "cx" : d->adsize == 32 ? "ecx" : "rcx");
            d->c67 = 1;
        }
        /* cmpps family: the predicate becomes the mnemonic for 0..7 */
        if (map == 2 && op2 == 0xC2 && d->op[2].kind == K_I) {
            static const char *const pred[8] = {"eq","lt","le","unord","neq","nlt","nle","ord"};
            unsigned imm = (unsigned)d->op[2].imm;
            const char *base = d->mn + 3;   /* "ps"/"pd"/"ss"/"sd" */
            d->tl = 0; d->text[0] = 0;
            int save_nops = d->nops;
            d->nops = 2;
            if (imm < 8) put(d, "cmp%s%s ", pred[imm], base);
            else put(d, "%s ", d->mn);
            put_operand(d, 0, out); put(d, ",");
            put_operand(d, 1, out);
            if (imm >= 8) put(d, ",byte 0x%x", imm);
            d->nops = save_nops;
        }
        if (map == 3 && d->lastop == 0x44 && d->nops == 3) {
            static const char *const pq[4] = {"pclmullqlqdq","pclmulhqlqdq","pclmullqhqdq","pclmulhqhqdq"};
            unsigned imm = (unsigned)d->op[2].imm;
            int k = (imm == 0x00) ? 0 : (imm == 0x01) ? 1 : (imm == 0x10) ? 2 : (imm == 0x11) ? 3 : -1;
            if (d->bits == 16 && d->p67 && d->mod == 3) k = -1;   /* ndisasm keeps the imm form so a32 is absorbed */
            d->tl = 0; d->text[0] = 0;
            int save = d->nops; d->nops = 2;
            put(d, "%s ", k >= 0 ? pq[k] : "pclmulqdq");
            put_operand(d, 0, out); put(d, ","); put_operand(d, 1, out);
            if (k < 0) put(d, ",byte 0x%x", imm);
            d->nops = save;
        }
        bl = d->tl;
        memcpy(body, d->text, bl + 1);
        d->tl = 0; d->text[0] = 0;
        /* prefixes that the operands did not absorb */
        uint8_t lo = d->lastop;
        int near_branch = (map == 1 && ((d->fl & F_JCC) ? lo < 0xE0 || lo > 0xE3 : 0)) ||
                          (map == 2 && (d->fl & F_JCC)) ||
                          (map == 1 && (lo == 0xE8 || lo == 0xE9)) ||
                          (map == 1 && (lo == 0xC2 || lo == 0xC3)) ||
                          (map == 1 && lo == 0xFF && (d->reg == 2 || d->reg == 4));
        int memdst = d->modrm_ok && d->mod != 3;
        int lockable = 0, movst = 0;
        if (memdst && map == 1) {
            if (lo <= 0x31 && (lo & 7) <= 1 && lo != 0x38 && lo != 0x39) lockable = 1;
            if ((lo == 0x80 || lo == 0x81 || lo == 0x83) && d->reg != 7) lockable = 1;
            if ((lo == 0xF6 || lo == 0xF7) && (d->reg == 2 || d->reg == 3)) lockable = 1;
            if ((lo == 0xFE || lo == 0xFF) && d->reg <= 1) lockable = 1;
            if (lo == 0x86 || lo == 0x87) lockable = 2;
        } else if (memdst && map == 2) {
            if (lo == 0xAB || lo == 0xB3 || lo == 0xBB || lo == 0xB0 || lo == 0xB1 ||
                lo == 0xC0 || lo == 0xC1 || (lo == 0xBA && d->reg >= 5) || (lo == 0xC7 && d->reg == 1)) lockable = 1;
        }
        if (map == 1 && (((lo == 0x88 || lo == 0x89) && memdst) || ((lo == 0xC6 || lo == 0xC7) && d->reg == 0))) movst = 1;
        if (d->rep && !d->crep) {
            if ((lockable == 1 && d->lock) || lockable == 2 || (movst && d->rep == 0xF3))
                put(d, "%s ", d->rep == 0xF2 ? "xacquire" : "xrelease");
            else if (d->rep == 0xF2 && near_branch) put(d, "bnd ");
            else if (d->fl & F_CMPS) put(d, "%s ", d->rep == 0xF3 ? "repe" : "repne");
            else put(d, "%s ", d->rep == 0xF3 ? "rep" : "repne");
        }
        if (d->lock && !d->clock) put(d, "lock ");
        if (d->seg >= 0 && !d->cseg) put(d, "%s ", sregs[d->seg]);
        {
            int consumed = (d->rex & 8) ? (d->crexw || d->c66) : d->c66;
            if (!consumed && d->nomop != (d->bits == 16 ? 16 : 32))
                put(d, "%s ", d->nomop == 16 ? "o16" : d->nomop == 32 ? "o32" : "o64");
        }
        if (d->p67 && !d->c67 && a_unused_prints(d))
            put(d, "%s ", d->adsize == 16 ? "a16" : d->adsize == 32 ? "a32" : "a64");
        put(d, "%s", body);
    }
    goto finish;

format_done:
    {
        /* special paths built d->text directly; add unabsorbed prefixes */
        char body[160];
        memcpy(body, d->text, d->tl + 1);
        size_t bl = d->tl;
        d->tl = 0; d->text[0] = 0;
        if (d->rep && !d->crep) put(d, "%s ", d->rep == 0xF3 ? "rep" : "repne");
        if (d->lock) put(d, "lock ");
        if (d->seg >= 0 && !d->cseg) put(d, "%s ", sregs[d->seg]);
        {
            int consumed = (d->rex & 8) ? (d->crexw || d->c66) : d->c66;
            if (!consumed && d->nomop != (d->bits == 16 ? 16 : 32))
                put(d, "%s ", d->nomop == 16 ? "o16" : d->nomop == 32 ? "o32" : "o64");
        }
        if (d->p67 && !d->c67 && (!strchr(body, ' ') || (d->bits == 16 && d->sp_rmreg)))
            put(d, "%s ", d->adsize == 16 ? "a16" : d->adsize == 32 ? "a32" : "a64");
        (void)bl;
        put(d, "%s", body);
    }

finish:
    if (d->trunc || d->pos > 15) { set_invalid(d, out); out->truncated = 1; return out->len; }
    out->valid = 1;
    out->len = (int)d->pos;
    {
        size_t L = strlen(d->text);
        if (L >= sizeof out->text) L = sizeof out->text - 1;
        memcpy(out->text, d->text, L);
        out->text[L] = 0;
    }
    (void)oppos;
    /* ---- self-check class ---- */
    if (d->fl & F_JCC) out->chk = DIS_CHK_COND;
    else if (d->fl & (F_JMP | F_CALL)) out->chk = DIS_CHK_TARGET;
    else if (d->fl & F_NOCHK) out->chk = DIS_CHK_NONE;
    else out->chk = out->chk ? out->chk : DIS_CHK_FALL;
    if (out->chk != DIS_CHK_COND && out->chk != DIS_CHK_TARGET) out->target = 0;
    return out->len;
}

/* x87 and the one-byte irregulars. Returns 0 = use table entry,
 * 1 = text complete, 2 = d->e replaced, -1 = invalid. */
static int x87_or_special(ds_t *d, uint8_t op) {
    int bits = d->bits;
    if (op >= 0xD8 && op <= 0xDF) {
        parse_modrm(d);
        int o = op - 0xD8;
        if (d->mod != 3) {
            parse_mem(d);
            const x87m_t *x = &x87mem[o][d->reg];
            if (!x->mn) return -1;
            opnd_t t; spec_tok(x->ops, strlen(x->ops), &t);
            put(d, "%s ", x->mn);
            put_mem(d, t.kw == KW_SHOW ? t.size : 0, 0);
            d->fl = 0;
            return 1;
        }
        uint8_t mm = (uint8_t)(0xC0 | (d->reg << 3) | d->rm);
        if (o == 1 && mm == 0xD0) { put(d, "fnop"); return 1; }
        if (o == 1 && mm >= 0xE0) { const char *s = d9e0[mm - 0xE0]; if (!s) return -1; put(d, "%s", s); return 1; }
        if (o == 2 && mm == 0xE9) { put(d, "fucompp"); return 1; }
        if (o == 3 && mm == 0xE0) { put(d, "fneni"); return 1; }
        if (o == 3 && mm == 0xE1) { put(d, "fndisi"); return 1; }
        if (o == 3 && mm == 0xE4) { put(d, "fnsetpm"); return 1; }
        if (o == 7 && d->reg == 0) { put(d, "ffreep st%d", d->rm); d->sp_rmreg = 1; return 1; }
        if (o == 3 && mm == 0xE2) { put(d, "fnclex"); return 1; }
        if (o == 3 && mm == 0xE3) { put(d, "fninit"); return 1; }
        if (o == 6 && mm == 0xD9) { put(d, "fcompp"); return 1; }
        if (o == 7 && mm == 0xE0) { put(d, "fnstsw ax"); return 1; }
        const x87m_t *x = &x87reg[o][d->reg];
        if (!x->mn) return -1;
        /* 0 = single "sti", 1 = "st0,sti", 2 = "sti,st0" */
        int two = 0;
        if (d->bits == 16 && d->p67) {
            if (o == 0 || o == 2 || (o == 3 && (d->reg <= 3 || d->reg == 5 || d->reg == 6)) ||
                (o == 5 && (d->reg == 4 || d->reg == 5)) || (o == 7 && (d->reg == 5 || d->reg == 6))) two = 1;
            else if ((o == 1 && d->reg == 1) || o == 4 || (o == 6 && x->mn)) two = 2;
        }
        if (two == 1) { put(d, "%s st0,st%d", x->mn, d->rm); d->c67 = 1; }
        else if (two == 2) { put(d, "%s st%d,st0", x->mn, d->rm); d->c67 = 1; }
        else if (!strcmp(x->ops, "toSTi")) put(d, "%s to st%d", x->mn, d->rm);
        else { put(d, "%s st%d", x->mn, d->rm); d->sp_rmreg = 1; }
        return 1;
    }
    switch (op) {
    case 0x90: {
        int rexb = d->rex & 1;
        if (rexb) {
            int sz = d->opsize;
            put(d, "xchg %s,%s", gpr(d, 0, sz), gpr(d, 8, sz));
            d->c66 = 1; if (sz == 64) d->crexw = 1;
            return 1;
        }
        if (d->rep == 0xF3) { put(d, "pause"); d->crep = 1; return 1; }
        if (d->rex & 8) { put(d, "xchg rax,rax"); d->crexw = 1; return 1; }
        if (d->p66 || d->p67) { put(d, "xchg %s,%s", gpr(d, 0, d->opsize), gpr(d, 0, d->opsize)); d->c66 = 1; d->c67 = 1; return 1; }
        put(d, "nop");
        return 1;
    }
    case 0xE5: case 0xE7: case 0xED: case 0xEF: case 0x6D: case 0x6F:
        if (d->rex & 8) return -1;
        return 0;
    case 0x63:
        if (bits == 64) {
            static const ent_t mx = I("movsxd","Gv,Ed!");
            d->e = &mx;
            return 2;
        }
        return 0;
    case 0xC4: case 0xC5:
        if (bits == 64) return -1;
        if (d->pos < d->avail && (d->b[d->pos] >> 6) == 3) return -1;   /* VEX */
        return 0;
    case 0x62:
        if (bits == 64) return -1;
        if (d->pos < d->avail && (d->b[d->pos] >> 6) == 3) return -1;   /* EVEX */
        return 0;
    case 0x8F:
        if (d->pos < d->avail && ((d->b[d->pos] >> 3) & 7) != 0) return -1;   /* XOP */
        return 0;
    case 0xC6: case 0xC7:
        if (d->pos < d->avail && d->b[d->pos] == 0xF8) {
            d->pos++;
            if (op == 0xC6) { put(d, "xabort 0x%x", fetch(d)); return 1; }
            int sz = d->opsize == 16 ? 2 : 4;
            uint64_t rel = fetchn(d, sz);
            uint64_t t = maskbits(d->pc + d->pos + sext(rel, sz), bits == 64 ? 64 : d->opsize);
            put(d, "xbegin 0x%llx", (unsigned long long)t);
            return 1;
        }
        return 0;
    default:
        return 0;
    }
}
