/* PERF P3: only ordinary-RAM, single-page prefix/opcode predecode can hit.
 * Live ModRM (GPR-derived EA), SIB, immediate and instruction faults never
 * come from the cache.  The NC removes ONLY the decode epoch comparison;
 * P2's walker still flushes, so the SMC assertion must turn red. */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "machine.h"
#include "platform.h"
#include "harness.h"

#define P 1ull
#define W 2ull
#define US 4ull
#define VA 0x400000ull
#define CF 1ull
static machine_t g_m;
static void w64(uint64_t p, uint64_t v) { mem_write(&g_m, p, 8, v); }
static void step_at(uint64_t rip) {
    g_m.cpu.rip = rip;
    assert(cpu_step(&g_m.cpu) == 0);
    assert(!g_m.cpu.fault && !g_m.cpu.exception_taken);
}
static void fixture(void) {
    setup_machine(&g_m);
    cpu_t *c = &g_m.cpu;
    w64(0x10000, 0x11000 | P | W | US);
    w64(0x11000, 0x12000 | P | W | US);
    w64(0x12000, P | W | US | 0x80); /* identity-mapped 2MiB PS */
    w64(0x12010, 0x13000 | P | W | US);
    w64(0x13000, 0x500000 | P | W | US);
    c->cr3 = 0x10000; c->cr4 = 0x20;
    c->efer = (1ull << 8) | (1ull << 10) | (1ull << 11);
    c->cr0 = 0x80010033ull;
    c->seg[SEG_CS] = (segment_t){.sel=8, .base=0, .limit=~0u, .l=1};
    c->seg[SEG_SS] = (segment_t){.sel=16, .base=0, .limit=~0u};
    c->gpr[RSP] = 0x8000;
}
static void smc_and_pte(void) {
    fixture();
    cpu_t *c = &g_m.cpu;
    /* Two physical implementations of the same virtual byte, separately
     * changed through the bus: first opcode SMC, then PTE remap. */
    mem_write(&g_m, 0x500000, 1, 0xF9);  /* STC */
    mem_write(&g_m, 0x600000, 1, 0xF8);  /* CLC */
    c->rflags &= ~CF;
    step_at(VA);
    assert(c->rflags & CF);
    uint64_t hits = g_m.perf_decode_hits;
    c->rflags &= ~CF;
    step_at(VA);
    assert(c->rflags & CF);
    assert(g_m.perf_decode_hits > hits); /* really exercised warm cache */
    uint64_t gen = g_m.perf_tlb_generation;
    mem_write(&g_m, 0x500000, 1, 0xF8);
    assert(g_m.perf_tlb_generation != gen);
    c->rflags |= CF;
    step_at(VA);
    assert(!(c->rflags & CF)); /* P3 NC: stale STC must FAIL here */
    mem_write(&g_m, 0x500000, 1, 0xF9);
    step_at(VA);
    assert(c->rflags & CF);
    w64(0x13000, 0x600000 | P | W | US);
    step_at(VA);
    assert(!(c->rflags & CF));
    puts("ok p3 warm predecode, RAM opcode rewrite and PTE remap");
}
static void live_modrm_and_immediate(void) {
    fixture();
    cpu_t *c = &g_m.cpu;
    /* MOV RAX,[RBX+RCX*4], reusing SAME opcode at same RIP with a NEW
     * dynamic SIB address.  A cached rm_t would produce the OLD operand. */
    const uint8_t sib[] = {0x48,0x8B,0x04,0x8B};
    memcpy(g_m.ram+0x1000, sib, sizeof sib);
    w64(0x500000, 0x1122334455667788ull);
    w64(0x500010, 0x8877665544332211ull);
    c->gpr[RBX] = VA; c->gpr[RCX] = 0;
    step_at(0x1000);
    assert(c->gpr[RAX] == 0x1122334455667788ull);
    uint64_t hits = g_m.perf_decode_hits;
    c->gpr[RCX] = 4;
    step_at(0x1000);
    assert(g_m.perf_decode_hits > hits);
    assert(c->gpr[RAX] == 0x8877665544332211ull);

    /* ADD RAX, imm32; only prefix/opcode is saved.  This byte changes via
     * bus between invocations, which also flushes the epoch.  More
     * importantly, forcing an existing cached entry's immutable opcode
     * with a NEW GPR must not snapshot the immediate into the entry. */
    const uint8_t add[] = {0x48,0x05,0x03,0,0,0};
    memcpy(g_m.ram+0x1100, add, sizeof add);
    c->gpr[RAX] = 10;
    step_at(0x1100);
    assert(c->gpr[RAX] == 13);
    c->gpr[RAX] = 20;
    hits = g_m.perf_decode_hits;
    step_at(0x1100);
    assert(g_m.perf_decode_hits > hits && c->gpr[RAX] == 23);
    mem_write(&g_m, 0x1102, 1, 7);
    c->gpr[RAX] = 20;
    step_at(0x1100);
    assert(c->gpr[RAX] == 27);
    puts("ok p3 live ModRM/SIB and immediate, with repeated opcode hits");
}
static void boundary_mode_and_trace(void) {
    fixture();
    cpu_t *c = &g_m.cpu;
    /* Prefix byte straddles a 4KiB boundary: never install, even after
     * two successful executions.  These are live two-page fetches. */
    g_m.ram[0x1FFF] = 0x66;
    g_m.ram[0x2000] = 0xF9;
    uint64_t hits = g_m.perf_decode_hits;
    step_at(0x1FFF);
    step_at(0x1FFF);
    assert(c->rflags & CF);
    assert(g_m.perf_decode_hits == hits);

    /* CS.L and CS.D_B change the decode of 0x48 between REX and DEC.
     * A direct mode change is stricter than an architected, flushing one. */
    g_m.ram[0x3000] = 0x48; g_m.ram[0x3001] = 0xF9;
    step_at(0x3000);
    hits = g_m.perf_decode_hits;
    step_at(0x3000);
    assert(g_m.perf_decode_hits > hits && c->rip == 0x3002);
    c->seg[SEG_CS].l = 0; c->seg[SEG_CS].d_b = 1;
    c->gpr[RAX] = 2;
    hits = g_m.perf_decode_hits;
    step_at(0x3000);
    assert(g_m.perf_decode_hits == hits);
    assert(c->gpr[RAX] == 1 && c->rip == 0x3001);
    c->seg[SEG_CS].l = 1;
    c->trace = 1;
    hits = g_m.perf_decode_hits;
    step_at(0x3000);
    assert(g_m.perf_decode_hits == hits && c->rip == 0x3002);
    puts("ok p3 page boundary, CS mode key and trace bypass");
}
static int imm_reads;
static uint64_t immediate_mmio(void *ctx, uint64_t pa, int n) {
    (void)ctx; (void)pa; assert(n == 4);
    return ++imm_reads == 1 ? 3 : 7;
}
static void immutable_opcode_live_mmio_imm(void) {
    fixture();
    cpu_t *c = &g_m.cpu;
    /* RAM opcode/prefix end exactly at the page boundary.  imm32 lives
     * on the NEXT page, mapped to changing MMIO; cache MUST hit the RAM
     * predecode while fetching the live immediate twice (no RAM write). */
    mem_write(&g_m, 0x500FFE, 1, 0x48); /* REX.W */
    mem_write(&g_m, 0x500FFF, 1, 0x05); /* ADD RAX, imm32 */
    w64(0x13008, 0xFEBD0000ull | P | W | US);
    mem_register_mmio(&g_m, 0xFEBD0000ull, 4096,
                      immediate_mmio, NULL, NULL, "p3-imm");
    c->gpr[RAX] = 10;
    step_at(VA + 0xFFE);
    assert(c->gpr[RAX] == 13);
    uint64_t hits = g_m.perf_decode_hits;
    c->gpr[RAX] = 10;
    step_at(VA + 0xFFE);
    assert(g_m.perf_decode_hits > hits);
    assert(c->gpr[RAX] == 17 && imm_reads == 2);
    free(g_m.mmio_list); g_m.mmio_list = NULL;
    puts("ok p3 cached RAM opcode with changing MMIO immediate stays live");
}
static void gate(int vec, uint16_t sel, uint64_t off) {
    uint64_t a = 0x0C00 + (uint64_t)vec * 16;
    w64(a, (off & 0xFFFF) | ((uint64_t)sel << 16) |
        ((uint64_t)0x8F << 40) | ((off & 0xFFFF0000ull) << 32));
    w64(a + 8, off >> 32);
}
static void nx_after_warm(void) {
    fixture();
    cpu_t *c = &g_m.cpu;
    /* Warm the EXEC predecode, then revoke execute rights via RAM PTE.
     * Fault must occur before opcode dispatch; old #PF CR2/err/RIP and
     * one instruction-attempt accounting still apply. */
    c->idtr_base = 0x0C00; c->idtr_limit = 256 * 16 - 1;
    w64(0x2004, 0x30000); c->tr_base = 0x2000; c->tr_limit = 103;
    gate(14, 8, 0x9000 + 14 * 8);
    g_m.ram[0x9000 + 14 * 8] = 0xF4;
    mem_write(&g_m, 0x500000, 1, 0xF9);
    step_at(VA);
    step_at(VA);
    uint64_t hits = g_m.perf_decode_hits;
    assert(hits > 0);
    w64(0x13000, 0x500000 | P | W | US | (1ull << 63));
    c->rip = VA;
    uint64_t steps = c->executed_steps;
    (void)cpu_step(c);
    assert(c->cr2 == VA && c->rip == 0x9000 + 14 * 8);
    assert(mem_read(&g_m, c->gpr[RSP], 8) == 0x11); /* P|I/D */
    assert(c->executed_steps == steps + 1);
    puts("ok p3 warm EXEC -> NX #PF preserves CR2/err/RIP/clock");
}
static void no_paging_or_a20(void) {
    fixture();
    cpu_t *c = &g_m.cpu;
    g_m.ram[0x3000] = 0xF9;
    g_m.chipset.a20 = 0;
    step_at(0x3000); step_at(0x3000);
    assert(g_m.perf_decode_hits == 0);
    g_m.chipset.a20 = 1;
    c->cr0 &= ~(1ull << 31);
    step_at(0x3000); step_at(0x3000);
    assert(g_m.perf_decode_hits == 0);
    puts("ok p3 A20-closed and nonpaging execution bypass predecode");
}
static int mmio_reads;
static uint64_t mmio_code(void *ctx, uint64_t pa, int n) {
    (void)ctx; (void)pa; assert(n == 1);
    return ++mmio_reads == 1 ? 0xF9 : 0xF8;
}
static void mmio_write(void *ctx, uint64_t pa, int n, uint64_t v) {
    (void)ctx;(void)pa;(void)n;(void)v; assert(!"MMIO code is read-only");
}
static void mmio_fetch(void) {
    fixture();
    cpu_t *c = &g_m.cpu;
    mem_register_mmio(&g_m, 0xFEBD0000ull, 4096, mmio_code, mmio_write, NULL, "p3-code");
    w64(0x13000, 0xFEBD0000ull | P | W | US);
    uint64_t hits = g_m.perf_decode_hits;
    step_at(VA); assert(c->rflags & CF);
    step_at(VA); assert(!(c->rflags & CF));
    assert(mmio_reads == 2 && g_m.perf_decode_hits == hits);
    free(g_m.mmio_list); g_m.mmio_list = NULL;
    puts("ok p3 MMIO execution remains live across two fetches");
}
int main(void) {
    smc_and_pte();
    live_modrm_and_immediate();
    immutable_opcode_live_mmio_imm();
    boundary_mode_and_trace();
    nx_after_warm();
    no_paging_or_a20();
    mmio_fetch();
    puts("test-decode: all vectors passed");
    return 0;
}
