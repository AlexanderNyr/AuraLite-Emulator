/* tests/test_disasm.c -- TOOLING T1: disassembler + inspection + trace vectors.
 *
 * Part A: oracle vectors. Expected text and length were produced by
 *         ndisasm 2.16.03 (`ndisasm -b<bits> -o <pc>`), frozen here so this
 *         suite runs without nasm; tests/disasm_diff.py keeps the live
 *         differential.
 * Part B: CPU-consistency vectors where ndisasm and the emulator's CPU
 *         disagree (each one is an allowlisted divergence in the diff).
 * Part C: successor classes and branch targets used by the trace check.
 * Part D: rejects: VEX/3DNow!/82, truncation, over-long prefix runs.
 * Part E: side-effect-free inspection on a harness machine (no MMIO
 *         callback, no P2/P3 cache traffic, translate() mirror).
 * Part F: the trace hook on a small real-mode program.
 */
#include <stdio.h>
#include <string.h>
#include <assert.h>
#include "harness.h"
#include "cpu.h"
#include "disasm.h"
#include "dbg.h"

typedef struct { int bits; const char *hex; uint64_t pc; int len; const char *text; } vec_t;

static const vec_t oracle[] = {
    { 16, "ea5be000f0", 0xfff0ULL, 5, "jmp 0xf000:0xe05b" },
    { 16, "fa", 0x0ULL, 1, "cli" },
    { 16, "31c0", 0x0ULL, 2, "xor ax,ax" },
    { 16, "8ed8", 0x0ULL, 2, "mov ds,ax" },
    { 16, "bc00f0", 0x0ULL, 3, "mov sp,0xf000" },
    { 16, "0f0116ff7f", 0x0ULL, 5, "lgdt [0x7fff]" },
    { 16, "0f20c0", 0x0ULL, 3, "mov eax,cr0" },
    { 16, "6683c801", 0x0ULL, 4, "or eax,byte +0x1" },
    { 16, "0f22c0", 0x0ULL, 3, "mov cr0,eax" },
    { 16, "66ea000010000800", 0x1000ULL, 8, "jmp dword 0x8:0x100000" },
    { 16, "e492", 0x0ULL, 2, "in al,0x92" },
    { 16, "0c02", 0x0ULL, 2, "or al,0x2" },
    { 16, "e692", 0x0ULL, 2, "out 0x92,al" },
    { 16, "8b4602", 0x0ULL, 3, "mov ax,[bp+0x2]" },
    { 16, "8b87fe7f", 0x0ULL, 4, "mov ax,[bx+0x7ffe]" },
    { 16, "8b063412", 0x0ULL, 4, "mov ax,[0x1234]" },
    { 16, "268b07", 0x0ULL, 3, "mov ax,[es:bx]" },
    { 16, "f3a4", 0x0ULL, 2, "rep movsb" },
    { 16, "f3a5", 0x0ULL, 2, "rep movsw" },
    { 16, "66f3a5", 0x0ULL, 3, "rep movsd" },
    { 16, "cd10", 0x0ULL, 2, "int 0x10" },
    { 16, "cf", 0x0ULL, 1, "iret" },
    { 16, "e2fe", 0x200ULL, 2, "loop 0x200" },
    { 16, "e3fe", 0x200ULL, 2, "jcxz 0x200" },
    { 16, "9a00000010", 0x0ULL, 5, "call 0x1000:0x0" },
    { 16, "6631c0", 0x0ULL, 3, "xor eax,eax" },
    { 16, "67668b0488", 0x0ULL, 5, "mov eax,[dword eax+ecx*4]" },
    { 16, "c8100000", 0x0ULL, 4, "enter 0x10,0x0" },
    { 16, "c9", 0x0ULL, 1, "leave" },
    { 32, "b800000100", 0x0ULL, 5, "mov eax,0x10000" },
    { 32, "8b442404", 0x0ULL, 4, "mov eax,[esp+0x4]" },
    { 32, "8b0488", 0x0ULL, 3, "mov eax,[eax+ecx*4]" },
    { 32, "8b048d00100000", 0x0ULL, 7, "mov eax,[ecx*4+0x1000]" },
    { 32, "8b0500100000", 0x0ULL, 6, "mov eax,[dword 0x1000]" },
    { 32, "c7050010000001000000", 0x0ULL, 10, "mov dword [dword 0x1000],0x1" },
    { 32, "0f20e0", 0x0ULL, 3, "mov eax,cr4" },
    { 32, "0d20000000", 0x0ULL, 5, "or eax,0x20" },
    { 32, "0f22e0", 0x0ULL, 3, "mov cr4,eax" },
    { 32, "b9800000c0", 0x0ULL, 5, "mov ecx,0xc0000080" },
    { 32, "0f32", 0x0ULL, 2, "rdmsr" },
    { 32, "0fbae808", 0x0ULL, 4, "bts eax,byte 0x8" },
    { 32, "0f30", 0x0ULL, 2, "wrmsr" },
    { 32, "ea000010000800", 0x0ULL, 7, "jmp 0x8:0x100000" },
    { 32, "e9fbffffff", 0x100ULL, 5, "jmp 0x100" },
    { 32, "e800000000", 0x100ULL, 5, "call 0x105" },
    { 32, "7405", 0x100ULL, 2, "jz 0x107" },
    { 32, "0f8400010000", 0x100ULL, 6, "jz near 0x206" },
    { 32, "66a100100000", 0x0ULL, 6, "mov ax,[0x1000]" },
    { 32, "67a10010", 0x0ULL, 4, "mov eax,[word 0x1000]" },
    { 32, "6650", 0x0ULL, 2, "push ax" },
    { 32, "60", 0x0ULL, 1, "pusha" },
    { 32, "6660", 0x0ULL, 2, "pushaw" },
    { 32, "9c", 0x0ULL, 1, "pushf" },
    { 32, "0fa2", 0x0ULL, 2, "cpuid" },
    { 32, "83c0ff", 0x0ULL, 3, "add eax,byte -0x1" },
    { 32, "81c000010000", 0x0ULL, 6, "add eax,0x100" },
    { 32, "6bc005", 0x0ULL, 3, "imul eax,eax,byte +0x5" },
    { 32, "69c000010000", 0x0ULL, 6, "imul eax,eax,dword 0x100" },
    { 32, "c1e004", 0x0ULL, 3, "shl eax,byte 0x4" },
    { 32, "d1e8", 0x0ULL, 2, "shr eax,1" },
    { 32, "d3e0", 0x0ULL, 2, "shl eax,cl" },
    { 32, "f6c101", 0x0ULL, 3, "test cl,0x1" },
    { 32, "f7d8", 0x0ULL, 2, "neg eax" },
    { 32, "fe00", 0x0ULL, 2, "inc byte [eax]" },
    { 32, "ff10", 0x0ULL, 2, "call [eax]" },
    { 32, "ff18", 0x0ULL, 2, "call far [eax]" },
    { 32, "ff30", 0x0ULL, 2, "push dword [eax]" },
    { 32, "8f00", 0x0ULL, 2, "pop dword [eax]" },
    { 32, "0fb6c0", 0x0ULL, 3, "movzx eax,al" },
    { 32, "0fbf00", 0x0ULL, 3, "movsx eax,word [eax]" },
    { 32, "0f94c0", 0x0ULL, 3, "setz al" },
    { 32, "0f44c1", 0x0ULL, 3, "cmovz eax,ecx" },
    { 32, "0fafc1", 0x0ULL, 3, "imul eax,ecx" },
    { 32, "0fc1c8", 0x0ULL, 3, "xadd eax,ecx" },
    { 32, "f00fb10a", 0x0ULL, 4, "lock cmpxchg [edx],ecx" },
    { 32, "0fc8", 0x0ULL, 2, "bswap eax" },
    { 32, "d9e8", 0x0ULL, 2, "fld1" },
    { 32, "dd1c24", 0x0ULL, 3, "fstp qword [esp]" },
    { 32, "dec1", 0x0ULL, 2, "faddp st1" },
    { 32, "dcc1", 0x0ULL, 2, "fadd to st1" },
    { 32, "d9c9", 0x0ULL, 2, "fxch st1" },
    { 32, "dfe0", 0x0ULL, 2, "fnstsw ax" },
    { 64, "488b0500100000", 0x1000ULL, 7, "mov rax,[rel 0x2007]" },
    { 64, "c7050010000001000000", 0x1000ULL, 10, "mov dword [rel 0x200a],0x1" },
    { 64, "48c7c0ffffffff", 0x0ULL, 7, "mov rax,0xffffffffffffffff" },
    { 64, "48b80102030405060708", 0x0ULL, 10, "mov rax,0x807060504030201" },
    { 64, "4c8b4508", 0x0ULL, 4, "mov r8,[rbp+0x8]" },
    { 64, "428b04a0", 0x0ULL, 4, "mov eax,[rax+r12*4]" },
    { 64, "64488b042528000000", 0x0ULL, 9, "mov rax,[fs:0x28]" },
    { 64, "65488b042500000000", 0x0ULL, 9, "mov rax,[gs:0x0]" },
    { 64, "0f05", 0x0ULL, 2, "syscall" },
    { 64, "480f07", 0x0ULL, 3, "o64 sysret" },
    { 64, "0f01f8", 0x0ULL, 3, "swapgs" },
    { 64, "48cf", 0x0ULL, 2, "iretq" },
    { 64, "4863c1", 0x0ULL, 3, "movsxd rax,ecx" },
    { 64, "f3480fb8c1", 0x0ULL, 5, "popcnt rax,rcx" },
    { 64, "f30f1efa", 0x0ULL, 4, "endbr64" },
    { 64, "0f1f440000", 0x0ULL, 5, "nop dword [rax+rax+0x0]" },
    { 64, "660f1f840000000000", 0x0ULL, 9, "nop word [rax+rax+0x0]" },
    { 64, "0f280500100000", 0x1000ULL, 7, "movaps xmm0,oword [rel 0x2007]" },
    { 64, "660fefc0", 0x0ULL, 4, "pxor xmm0,xmm0" },
    { 64, "f30f6f01", 0x0ULL, 4, "movdqu xmm0,oword [rcx]" },
    { 64, "660fd60424", 0x0ULL, 5, "movq [rsp],xmm0" },
    { 64, "0fae1c24", 0x0ULL, 4, "stmxcsr dword [rsp]" },
    { 64, "0fae5c2404", 0x0ULL, 5, "stmxcsr dword [rsp+0x4]" },
    { 64, "0f011500100000", 0x1000ULL, 7, "lgdt [rel 0x2007]" },
    { 64, "0f22d8", 0x0ULL, 3, "mov cr3,rax" },
    { 64, "0f20e0", 0x0ULL, 3, "mov rax,cr4" },
    { 64, "440f22c0", 0x0ULL, 4, "mov cr8,rax" },
    { 64, "0f32", 0x0ULL, 2, "rdmsr" },
    { 64, "ff1500100000", 0x1000ULL, 6, "call [rel 0x2006]" },
    { 64, "ffe0", 0x0ULL, 2, "jmp rax" },
    { 64, "48ff20", 0x0ULL, 3, "jmp qword [rax]" },
    { 64, "e3fe", 0x200ULL, 2, "jrcxz 0x200" },
    { 64, "67e2fe", 0x200ULL, 3, "loop 0x201,ecx" },
    { 64, "f3ab", 0x0ULL, 2, "rep stosd" },
    { 64, "f348ab", 0x0ULL, 3, "rep stosq" },
    { 64, "f3a6", 0x0ULL, 2, "repe cmpsb" },
    { 64, "a11122334455667788", 0x0ULL, 9, "mov eax,[qword 0x8877665544332211]" },
    { 64, "48a31122334455667788", 0x0ULL, 10, "mov [qword 0x8877665544332211],rax" },
    { 64, "9c", 0x0ULL, 1, "pushf" },
    { 64, "fa", 0x0ULL, 1, "cli" },
    { 64, "fb", 0x0ULL, 1, "sti" },
    { 64, "f4", 0x0ULL, 1, "hlt" },
    { 64, "cc", 0x0ULL, 1, "int3" },
    { 64, "cd80", 0x0ULL, 2, "int 0x80" },
    { 64, "0fc70e", 0x0ULL, 3, "cmpxchg8b qword [rsi]" },
    { 64, "480fc70e", 0x0ULL, 4, "cmpxchg16b oword [rsi]" },
    { 64, "f0480fb10a", 0x0ULL, 5, "lock cmpxchg [rdx],rcx" },
    { 64, "488d05f9ffffff", 0x1000ULL, 7, "lea rax,[rel 0x1000]" },
    { 64, "c3", 0x0ULL, 1, "ret" },
    { 64, "c20800", 0x0ULL, 3, "ret 0x8" },
};

static int unhex(const char *h, uint8_t *b) {
    int n = 0;
    while (h[0] && h[1]) {
        unsigned v; sscanf(h, "%2x", &v); b[n++] = (uint8_t)v; h += 2;
    }
    return n;
}

static int fails;
#define EXPECT(cond, ...) do { if (!(cond)) { printf("FAIL " __VA_ARGS__); printf("\n"); fails++; } } while (0)

static void part_a(void) {
    int n = (int)(sizeof oracle / sizeof oracle[0]);
    for (int i = 0; i < n; i++) {
        uint8_t b[32]; int bl = unhex(oracle[i].hex, b);
        dis_insn_t in;
        dis_decode(b, (size_t)bl, oracle[i].bits, oracle[i].pc, &in);
        EXPECT(in.valid && in.len == oracle[i].len && !strcmp(in.text, oracle[i].text),
               "A b%d %s: got len %d \"%s\", want len %d \"%s\"", oracle[i].bits, oracle[i].hex,
               in.len, in.text, oracle[i].len, oracle[i].text);
    }
    printf("ok part A: %d ndisasm-frozen vectors\n", n);
}

static const vec_t cpucons[] = {
    { 64, "63c1",     0, 2, "movsxd eax,ecx" },      /* cpu.c case 0x63: 32-bit move w/o REX.W */
    { 32, "d1f0",     0, 2, "sal eax,1" },           /* cpu.c: "SAL aliases SHL" (/6)        */
    { 32, "f6c901",   0, 3, "test cl,0x1" },         /* cpu.c F6/F7 ext==0||ext==1          */
    { 32, "9b",       0, 1, "wait" },                /* executed alone, not a prefix        */
    { 64, "66e80000", 0x100, 4, "call word 0x104" }, /* cpu.c E8: osz==2 -> rel16 (AMD)     */
    { 32, "0f95c9",   0, 3, "setnz cl" },            /* cpu.c SETcc ignores ModRM.reg        */
};

static void part_b(void) {
    int n = (int)(sizeof cpucons / sizeof cpucons[0]);
    for (int i = 0; i < n; i++) {
        uint8_t b[32]; int bl = unhex(cpucons[i].hex, b);
        dis_insn_t in;
        dis_decode(b, (size_t)bl, cpucons[i].bits, cpucons[i].pc, &in);
        EXPECT(in.valid && in.len == cpucons[i].len && !strcmp(in.text, cpucons[i].text),
               "B b%d %s: got len %d \"%s\", want len %d \"%s\"", cpucons[i].bits, cpucons[i].hex,
               in.len, in.text, cpucons[i].len, cpucons[i].text);
    }
    printf("ok part B: %d CPU-consistency vectors\n", n);
}

typedef struct { int bits; const char *hex; uint64_t pc; int chk; uint64_t target; } flow_t;
static const flow_t flows[] = {
    { 64, "7405",           0x1000, DIS_CHK_COND,   0x1007 },
    { 64, "0f8410000000",   0x1000, DIS_CHK_COND,   0x1016 },
    { 64, "ebfe",           0x2000, DIS_CHK_TARGET, 0x2000 },
    { 64, "e9fbffffff",     0x2000, DIS_CHK_TARGET, 0x2000 },
    { 64, "e800000000",     0x2000, DIS_CHK_TARGET, 0x2005 },
    { 16, "e2fe",           0x0200, DIS_CHK_COND,   0x0200 },
    { 16, "e3fe",           0x0200, DIS_CHK_COND,   0x0200 },
    { 16, "ebfe",           0xfffe, DIS_CHK_TARGET, 0xfffe },
    { 64, "c3",             0,      DIS_CHK_NONE,   0 },
    { 64, "ffd0",           0,      DIS_CHK_NONE,   0 },
    { 64, "cd80",           0,      DIS_CHK_NONE,   0 },
    { 64, "0f05",           0,      DIS_CHK_NONE,   0 },
    { 64, "48cf",           0,      DIS_CHK_NONE,   0 },
    { 16, "ea5be000f0",     0,      DIS_CHK_NONE,   0 },
    { 64, "f4",             0,      DIS_CHK_FALL,   0 },
    { 64, "f348ab",         0,      DIS_CHK_FALL,   0 },
    { 64, "488b0500100000", 0,      DIS_CHK_FALL,   0 },
};

static void part_c(void) {
    int n = (int)(sizeof flows / sizeof flows[0]);
    for (int i = 0; i < n; i++) {
        uint8_t b[32]; int bl = unhex(flows[i].hex, b);
        dis_insn_t in;
        dis_decode(b, (size_t)bl, flows[i].bits, flows[i].pc, &in);
        EXPECT(in.valid && in.chk == flows[i].chk && in.target == flows[i].target,
               "C b%d %s: chk %d target %llx, want chk %d target %llx", flows[i].bits, flows[i].hex,
               in.chk, (unsigned long long)in.target, flows[i].chk, (unsigned long long)flows[i].target);
    }
    printf("ok part C: %d successor-class/target vectors\n", n);
}

static void part_d(void) {
    static const struct { int bits; const char *hex; size_t avail; int truncated; } rej[] = {
        { 64, "c5f81000",   4, 0 },   /* VEX: out of T1 scope        */
        { 64, "c4e2791800", 5, 0 },
        { 32, "0f0fc1b4",   4, 0 },   /* 3DNow!                      */
        { 32, "8200 11",    3, 0 },   /* 82: cpu.c #UD, ndisasm db   */
        { 32, "b801",       2, 1 },   /* imm32 cut off               */
        { 64, "48",         1, 1 },   /* lone REX                    */
    };
    for (size_t i = 0; i < sizeof rej / sizeof rej[0]; i++) {
        uint8_t b[32]; char h[32]; size_t k = 0;
        for (const char *p = rej[i].hex; *p; p++) if (*p != ' ') h[k++] = *p;
        h[k] = 0;
        unhex(h, b);
        dis_insn_t in;
        dis_decode(b, rej[i].avail, rej[i].bits, 0, &in);
        EXPECT(!in.valid && in.len == 1 && !strncmp(in.text, "db 0x", 5) && in.truncated == rej[i].truncated,
               "D b%d %s: valid %d len %d trunc %d \"%s\"", rej[i].bits, rej[i].hex, in.valid, in.len,
               in.truncated, in.text);
    }
    uint8_t pfx[16]; memset(pfx, 0x66, 15); pfx[15] = 0x90;
    dis_insn_t in;
    dis_decode(pfx, 16, 32, 0, &in);
    EXPECT(!in.valid, "D 15 prefixes + nop must not decode (16 bytes)");
    EXPECT(dis_decode(pfx, 0, 32, 0, &in) == 0, "D empty input");
    printf("ok part D: rejects (VEX, 3DNow!, 82, truncation, >15 bytes, empty)\n");
}

/* ---- Part E: side-effect-free inspection ---- */
static machine_t g_m;
static int mmio_reads;
static uint64_t mmio_rd(void *ctx, uint64_t a, int s) { (void)ctx; (void)a; (void)s; mmio_reads++; return 0x5A; }
static void mmio_wr(void *ctx, uint64_t a, int s, uint64_t v) { (void)ctx; (void)a; (void)s; (void)v; }
static void w64(uint64_t pa, uint64_t v) { memcpy(g_m.ram + pa, &v, 8); }

static void part_e(void) {
    setup_machine(&g_m);
    cpu_t *c = &g_m.cpu;
    /* 4-level tables at 0x10000: PML4[0] -> PDPT 0x11000; PDPT[0] -> PD 0x12000;
     * PD[0] -> PT 0x13000; PT[1] -> 0x5000 (4K); PD[1] -> 2M leaf at 0x400000;
     * PDPT[1] -> 1G leaf at 0 */
    w64(0x10000, 0x11000 | 3);
    w64(0x11000, 0x12000 | 3);
    w64(0x11008, 0x00000000 | 0x83);
    w64(0x12000, 0x13000 | 3);
    w64(0x12008, 0x400000 | 0x83);
    w64(0x13008, 0x5000 | 3);
    c->cr3 = 0x10000; c->cr0 |= (1ULL << 31) | 1; c->efer |= (1ULL << 10) | (1ULL << 8);
    uint64_t p;
    EXPECT(dbg_translate(&g_m, c, 0x1234, &p) && p == 0x5234, "E 4K leaf: %llx", (unsigned long long)p);
    EXPECT(dbg_translate(&g_m, c, 0x212345, &p) && p == 0x412345, "E 2M leaf: %llx", (unsigned long long)p);
    EXPECT(dbg_translate(&g_m, c, 0x40001234ULL, &p) && p == 0x1234, "E 1G leaf: %llx", (unsigned long long)p);
    EXPECT(!dbg_translate(&g_m, c, 0x2000, &p), "E not-present PTE must not translate");
    EXPECT(!dbg_translate(&g_m, c, 0x8000000000ULL, &p), "E not-present PML4E must not translate");
    /* no cache traffic, no MMIO callback */
    uint64_t th = g_m.perf_tlb_hits, tm = g_m.perf_tlb_misses, tf = g_m.perf_tlb_flushes;
    uint64_t dh = g_m.perf_decode_hits, dm = g_m.perf_decode_misses;
    uint8_t buf[16];
    g_m.ram[0x5234] = 0xC3;
    EXPECT(dbg_read_lin(&g_m, c, 0x1234, buf, 1) == 1 && buf[0] == 0xC3, "E read through 4K page");
    mem_register_mmio(&g_m, 0xFEBD0000ull, 4096, mmio_rd, mmio_wr, NULL, "t1-probe");
    w64(0x13010, 0xFEBD0000ull | 3);   /* PT[2] -> MMIO page */
    EXPECT(dbg_read_lin(&g_m, c, 0x2000, buf, 4) == 0, "E MMIO page must read 0 bytes");
    EXPECT(dbg_read_phys(&g_m, 0xFEBD0010ull, buf, 4) == 0, "E MMIO phys must read 0 bytes");
    EXPECT(mmio_reads == 0, "E MMIO callback ran %d times", mmio_reads);
    EXPECT(g_m.perf_tlb_hits == th && g_m.perf_tlb_misses == tm && g_m.perf_tlb_flushes == tf &&
           g_m.perf_decode_hits == dh && g_m.perf_decode_misses == dm, "E P2/P3 counters moved");
    /* open bus, ROM windows, A20 */
    EXPECT(dbg_read_phys(&g_m, 0x200000000ULL, buf, 1) == 1 && buf[0] == 0xFF, "E open bus reads 0xFF");
    g_m.rom[ROM_SIZE - 16] = 0xEA;
    EXPECT(dbg_read_phys(&g_m, 0xFFFFFFF0ULL, buf, 1) == 1 && buf[0] == 0xEA, "E reset-vector ROM window");
    EXPECT(dbg_read_phys(&g_m, ROM_ALIAS_LOW_BASE + ROM_SIZE - 16, buf, 1) == 1 && buf[0] == 0xEA, "E low ROM alias");
    g_m.ram[0x5000] = 0x11; g_m.ram[0x105000] = 0x22;
    g_m.chipset.a20 = 0;
    EXPECT(dbg_read_phys(&g_m, 0x105000, buf, 1) == 1 && buf[0] == 0x11, "E A20 closed wraps bit 20");
    g_m.chipset.a20 = 1;
    EXPECT(dbg_read_phys(&g_m, 0x105000, buf, 1) == 1 && buf[0] == 0x22, "E A20 open");
    /* writes: RAM only, ROM refused, epoch bumped */
    uint8_t v = 0x77;
    uint64_t gen = g_m.perf_tlb_generation;
    EXPECT(dbg_write_phys(&g_m, 0x6000, &v, 1) == 1 && g_m.ram[0x6000] == 0x77, "E RAM write");
    EXPECT(g_m.perf_tlb_generation != gen, "E RAM write must bump the P2/P3 epoch");
    EXPECT(dbg_write_phys(&g_m, 0xFFFFFFF0ULL, &v, 1) == 0, "E ROM write refused");
    EXPECT(dbg_write_phys(&g_m, 0xFEBD0000ull, &v, 1) == 0 && mmio_reads == 0, "E MMIO write refused");
    free(g_m.mmio_list); g_m.mmio_list = NULL;
    printf("ok part E: inspection is side-effect-free (translate mirror, no MMIO, no cache traffic)\n");
}

/* ---- Part F: trace hook on a real-mode program ---- */
static void part_f(void) {
    setup_machine(&g_m);
    /* mov cx,3 ; l: dec cx ; jnz l ; call f ; hlt ; f: ret */
    static const uint8_t prog[] = { 0xB9, 0x03, 0x00, 0x49, 0x75, 0xFD, 0xE8, 0x01, 0x00, 0xF4, 0xC3 };
    memcpy(g_m.ram, prog, sizeof prog);
    const char *path = "/tmp/test-disasm-trace.txt";
    assert(dbg_trace_open(&g_m, path, 0, 0) == 0);
    for (int i = 0; i < 64; i++) if (cpu_step(&g_m.cpu) != 0) break;
    dbg_trace_stats_t ts;
    assert(dbg_trace_stats(&g_m, &ts) == 0);
    dbg_trace_close(&g_m, 0);
    dbg_detach(&g_m);
    /* mov, 3x(dec, jnz), call, ret, hlt = 10 instructions. Checked
     * successors: mov + 3 dec (fall) and 3 jnz + call (branch) = 8; ret is
     * not predictable (CHK_NONE) and hlt ends the run. */
    EXPECT(ts.lines == 10, "F lines %llu", (unsigned long long)ts.lines);
    EXPECT(ts.mismatches == 0 && ts.undecodable == 0 && ts.unreadable == 0, "F mismatches/undecodable");
    EXPECT(ts.fall == 4, "F fall successors %llu", (unsigned long long)ts.fall);
    EXPECT(ts.branch == 4, "F branch successors %llu (3 jnz + call)", (unsigned long long)ts.branch);
    FILE *f = fopen(path, "r");
    char line[256]; int n = 0, found = 0;
    while (f && fgets(line, sizeof line, f)) {
        if (line[0] == '#') continue;
        if (n == 0) EXPECT(!strncmp(line, "0 0 real16 0000:0000000000000000 0000000000000000 b90300 mov cx,0x3", 66), "F first line: %s", line);
        if (strstr(line, "e80100 call 0xa")) found = 1;
        n++;
    }
    if (f) fclose(f);
    remove(path);
    EXPECT(n == 10 && found, "F trace file lines %d, call line %d", n, found);
    printf("ok part F: trace hook writes one line per instruction and checks every successor\n");
}

int main(void) {
    part_a(); part_b(); part_c(); part_d(); part_e(); part_f();
    if (fails) { printf("test-disasm: %d FAILED\n", fails); return 1; }
    puts("test-disasm: all vectors passed");
    return 0;
}
