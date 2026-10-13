/* PERF P2: successful-translation TLB invariants.  The old walker is the
 * oracle for fault ordering.  These vectors exercise warm cache entries,
 * guest/DMA PTE writes, CR3/CR4/AC/CPL keys, large pages, and INVLPG.
 * No wall time is asserted.  The P2 NC removes the mem_write RAM flush in a
 * scratch source copy and MUST fail the exact PTE-rewrite assertion below.
 */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "machine.h"
#include "platform.h"
#include "harness.h"

#define P  1ull
#define W  2ull
#define US 4ull
#define PS 0x80ull
#define NX (1ull<<63)
#define VA 0x400000ull
#define OLD_VALUE 0x1122334455667788ull
#define NEW_VALUE 0x8877665544332211ull

static machine_t g_m;
static int mmio_root_alt, mmio_root_reads;
static uint64_t mmio_root_read(void *ctx, uint64_t phys, int size) {
    (void)ctx;
    assert(size == 8 && phys == 0xFEBC0000ull);
    ++mmio_root_reads;
    return (mmio_root_alt ? 0x21000ull : 0x11000ull) | P | W | US;
}
static void mmio_root_write(void *ctx, uint64_t phys, int size, uint64_t v) {
    (void)ctx; (void)phys; (void)size; (void)v;
    assert(!"P2 MMIO root is read-only");
}
static void w64(uint64_t pa, uint64_t val) { mem_write(&g_m, pa, 8, val); }
static uint64_t r64(uint64_t pa) { return mem_read(&g_m, pa, 8); }
static void gate(int vec, uint16_t sel, uint64_t off) {
    uint64_t a = 0x0C00 + (uint64_t)vec * 16;
    w64(a, (off & 0xFFFF) | ((uint64_t)sel << 16) | ((uint64_t)0x8F << 40) |
           ((off & 0xFFFF0000ull) << 32));
    w64(a + 8, off >> 32);
}

static void fixture(void) {
    setup_machine(&g_m);
    cpu_t *c = &g_m.cpu;
    /* 4-level long-mode walk: identity-map the first 2MiB via a large
     * page, then map VA (4MiB) through PT[0] to physical 5MiB.  The
     * alternate root at 0x20000 maps the SAME VA to physical 6MiB. */
    w64(0x10000, 0x11000 | P | W | US);  /* PML4[0] */
    w64(0x11000, 0x12000 | P | W | US);  /* PDPT[0] */
    w64(0x12000, P | W | US | PS);        /* PD[0] identity 2MiB */
    w64(0x12010, 0x13000 | P | W | US); /* PD[2] -> PT */
    w64(0x13000, 0x500000 | P | W | US); /* PT[0] -> 5MiB */
    w64(0x20000, 0x21000 | P | W | US);
    w64(0x21000, 0x22000 | P | W | US);
    w64(0x22000, P | W | US | PS);
    w64(0x22010, 0x23000 | P | W | US);
    w64(0x23000, 0x600000 | P | W | US);
    w64(0x500000, OLD_VALUE);
    w64(0x600000, NEW_VALUE);
    c->cr3 = 0x10000;
    c->cr4 = 0x20;                           /* PAE */
    c->efer = (1ull<<8) | (1ull<<10) | (1ull<<11); /* LME LMA NXE */
    c->cr0 = 0x80010033ull;                  /* PG|WP|PE */
    c->idtr_base = 0x0C00; c->idtr_limit = 256 * 16 - 1;
    w64(0x2000 + 4, 0x30000);               /* TSS.RSP0 */
    c->tr_base = 0x2000; c->tr_limit = 103;
    for (int v = 0; v < 32; v++) gate(v, 8, 0x9000 + (uint64_t)v * 8);
    memset(g_m.ram + 0x9000, 0xF4, 32 * 8);
    c->seg[SEG_CS] = (segment_t){ .sel = 8, .base = 0, .limit = ~0u, .l = 1 };
    c->seg[SEG_SS] = (segment_t){ .sel = 0x10, .base = 0, .limit = ~0u };
    c->gpr[RSP] = 0x8000;
    c->gpr[RBX] = VA;
    /* MOV RAX,[RBX] in long mode.  Written before any cached code fetch. */
    const uint8_t code[] = { 0x48, 0x8B, 0x03 };
    memcpy(g_m.ram + 0x1000, code, sizeof code);
}
static void read_once(void) {
    g_m.cpu.rip = 0x1000;
    assert(cpu_step(&g_m.cpu) == 0);
    assert(!g_m.cpu.fault && !g_m.cpu.exception_taken);
}
static void pte_rewrite(void) {
    fixture();
    cpu_t *c = &g_m.cpu;
    read_once();
    assert(c->gpr[RAX] == OLD_VALUE);
    uint64_t before = g_m.perf_tlb_hits;
    read_once();
    assert(c->gpr[RAX] == OLD_VALUE);
    assert(g_m.perf_tlb_hits > before);       /* real cache hit, not a no-op cache */
    uint64_t gen = g_m.perf_tlb_generation;
    /* Guest/DMA write changes the PTE's physical page.  The NC deletes
     * the RAM-store generation bump.  The warmed entry then returns the
     * OLD value and this assertion fails -- not a timing-dependent NC. */
    w64(0x13000, 0x600000 | P | W | US);
    read_once();
    assert(c->gpr[RAX] == NEW_VALUE); /* P2 NC: without invalidation, OLD wins */
    assert(g_m.perf_tlb_generation != gen);
    puts("ok p2 warm hit, then PTE write invalidates (OLD -> NEW)");
}
static void cr3_and_invlpg(void) {
    fixture();
    cpu_t *c = &g_m.cpu;
    read_once();
    assert(c->gpr[RAX] == OLD_VALUE);
    /* CR3 write by guest instruction, with the alternate root already
     * populated; bypassing stale root MUST give the new data. */
    const uint8_t movcr3[] = { 0x0F, 0x22, 0xD8 }; /* MOV CR3,RAX */
    memcpy(g_m.ram + 0x1100, movcr3, sizeof movcr3);
    c->gpr[RAX] = 0x20000;
    uint64_t gen = g_m.perf_tlb_generation;
    c->rip = 0x1100;
    assert(cpu_step(c) == 0 && !c->fault);
    assert(c->cr3 == 0x20000 && g_m.perf_tlb_generation != gen);
    read_once();
    assert(c->gpr[RAX] == NEW_VALUE);
    /* INVLPG [RBX] must not read its operand.  Hint an UNMAPPED VA and
     * assert no #PF and global flush.  The original K2 no-fault behavior
     * must survive P2 even when there is no TLB entry for the hint. */
    const uint8_t invlpg[] = { 0x0F, 0x01, 0x3B }; /* INVLPG [RBX] */
    memcpy(g_m.ram + 0x1200, invlpg, sizeof invlpg);
    c->gpr[RBX] = 0x3000000000ull; /* unmapped */
    gen = g_m.perf_tlb_generation;
    c->rip = 0x1200;
    assert(cpu_step(c) == 0 && !c->fault && !c->exception_taken);
    assert(g_m.perf_tlb_generation != gen);
    c->gpr[RBX] = VA;
    read_once();
    assert(c->gpr[RAX] == NEW_VALUE);
    puts("ok p2 MOV CR3 and no-fault INVLPG invalidate the shared TLB");
}
static void faults_after_warm(void) {
    fixture();
    cpu_t *c = &g_m.cpu;
    read_once();
    /* Warm success must not mask a later not-present #PF.  Error bits:
     * P=0, W=0, U=0, I=0; the guest's exception gate is at 0x9000+14*8. */
    w64(0x13000, 0);
    c->rip = 0x1000;
    (void)cpu_step(c);
    assert(c->cr2 == VA);
    assert(c->rip == 0x9000 + 14 * 8);
    assert(r64(c->gpr[RSP]) == 0);            /* #PF error code */
    puts("ok p2 warm success -> nonpresent fault has exact CR2/err/RIP");

    fixture();
    c = &g_m.cpu;
    read_once();
    /* Warm READ then PTE NX; instruction fetch at VA must fault with
     * present-chain=1, I/D=1 (EFER.NXE), W/R=0 and correct CR2. */
    w64(0x13000, 0x500000 | P | W | US | NX);
    c->rip = VA;
    (void)cpu_step(c);
    assert(c->cr2 == VA);
    assert(c->rip == 0x9000 + 14 * 8);
    assert(r64(c->gpr[RSP]) == 0x11);         /* P|I/D */
    puts("ok p2 READ success -> NX fetch fault has exact CR2/err/RIP");
}
static void mode_control_flushes(void) {
    fixture();
    cpu_t *c = &g_m.cpu;
    read_once();
    uint64_t gen;
    /* Same-value writes still flush: the old full walk would re-read
     * tables after every architected CR0/CR4/EFER/LMSW operation. */
    const uint8_t cr0[] = { 0x0F, 0x22, 0xC0 };   /* MOV CR0,RAX */
    memcpy(g_m.ram + 0x1100, cr0, sizeof cr0);
    c->gpr[RAX] = c->cr0;
    gen = g_m.perf_tlb_generation;
    c->rip = 0x1100;
    assert(cpu_step(c) == 0 && !c->fault);
    assert(g_m.perf_tlb_generation != gen);
    read_once();
    assert(c->gpr[RAX] == OLD_VALUE);

    const uint8_t cr4[] = { 0x0F, 0x22, 0xE0 };   /* MOV CR4,RAX */
    memcpy(g_m.ram + 0x1200, cr4, sizeof cr4);
    c->gpr[RAX] = c->cr4;
    gen = g_m.perf_tlb_generation;
    c->rip = 0x1200;
    assert(cpu_step(c) == 0 && !c->fault);
    assert(g_m.perf_tlb_generation != gen);
    read_once();
    assert(c->gpr[RAX] == OLD_VALUE);

    const uint8_t wrmsr[] = { 0x0F, 0x30 };      /* WRMSR EFER */
    memcpy(g_m.ram + 0x1300, wrmsr, sizeof wrmsr);
    c->gpr[RCX] = 0xC0000080;
    c->gpr[RAX] = (uint32_t)c->efer;
    c->gpr[RDX] = c->efer >> 32;
    gen = g_m.perf_tlb_generation;
    c->rip = 0x1300;
    assert(cpu_step(c) == 0 && !c->fault);
    assert(g_m.perf_tlb_generation != gen);
    read_once();
    assert(c->gpr[RAX] == OLD_VALUE);

    const uint8_t lmsw[] = { 0x0F, 0x01, 0xF0 }; /* LMSW AX */
    memcpy(g_m.ram + 0x1400, lmsw, sizeof lmsw);
    c->gpr[RAX] = (uint16_t)c->cr0;
    gen = g_m.perf_tlb_generation;
    c->rip = 0x1400;
    assert(cpu_step(c) == 0 && !c->fault);
    assert(g_m.perf_tlb_generation != gen);
    read_once();
    assert(c->gpr[RAX] == OLD_VALUE);
    puts("ok p2 MOV CR0/CR4, WRMSR EFER and LMSW flush");
}
static void permissions_after_warm(void) {
    fixture();
    cpu_t *c = &g_m.cpu;
    read_once();
    /* Supervisor WP=1: same VA was readable, but its newly read-only
     * PTE must raise #PF on WRITE with err=P|W (3), not reuse the read
     * mapping.  Fault/RIP/CR2 are exactly the original walker contract. */
    w64(0x13000, 0x500000 | P | US);
    const uint8_t store[] = { 0x48, 0x89, 0x03 }; /* MOV [RBX], RAX */
    memcpy(g_m.ram + 0x1000, store, sizeof store);
    c->rip = 0x1000;
    (void)cpu_step(c);
    assert(c->cr2 == VA);
    assert(c->rip == 0x9000 + 14 * 8);
    assert(r64(c->gpr[RSP]) == 3);
    puts("ok p2 READ success -> RO WRITE #PF err=P|W with WP");

    fixture();
    c = &g_m.cpu;
    read_once();
    /* CPL3 user read after a warm supervisor read, but the PTE has U/S
     * cleared via a guest write.  Code remains in the US=1 2MiB page,
     * so the fault source is VA, not the instruction fetch. */
    w64(0x13000, 0x500000 | P | W);
    c->seg[SEG_CS].sel = 0x1B;
    c->seg[SEG_SS].sel = 0x23;
    c->gpr[RSP] = 0x1F000;
    c->rip = 0x1000;
    (void)cpu_step(c);
    assert(c->cr2 == VA);
    assert(c->rip == 0x9000 + 14 * 8);
    assert(r64(c->gpr[RSP]) == 0x5);         /* P|U/S */
    puts("ok p2 supervisor READ success -> CPL3 U/S fault err=P|U");
}
static void large_pages_and_a20(void) {
    fixture();
    cpu_t *c = &g_m.cpu;
    /* PD[0] is a 2MiB PS leaf, which must cache with the RIGHT physical
     * 4KiB subpage offset; an unaligned data address exercises the offset. */
    w64(0x1A1234, 0x7D6C5B4A39281706ull);
    c->gpr[RBX] = 0x1A1234;
    read_once();
    assert(c->gpr[RAX] == 0x7D6C5B4A39281706ull);
    read_once();
    assert(c->gpr[RAX] == 0x7D6C5B4A39281706ull);
    /* Convert PDPT[0] into a 1GiB PS leaf (identity map): VA maps to
     * phys 4MiB, not phys 5MiB from the old PT.  w64 flushes the TLB. */
    w64(0x11000, P | W | US | PS);
    w64(0x400000, 0xAABBCCDDEEFF1234ull);
    c->gpr[RBX] = VA;
    read_once();
    assert(c->gpr[RAX] == 0xAABBCCDDEEFF1234ull);
    puts("ok p2 2MiB/1GiB PS leaves use correct 4KiB subpage offset");

    fixture();
    c = &g_m.cpu;
    read_once();
    assert(c->gpr[RAX] == OLD_VALUE);
    /* Turning A20 OFF is not a RAM write; bypass all TLB hits and walk
     * the page tables through the old bus.  The PTE still resolves to
     * 5MiB but mem_read masks bit20 -> 4MiB.  Turning A20 back ON may
     * reuse the cached entry if no table was written (we write 4MiB
     * first, which intentionally flushes before the next measurement). */
    w64(0x400000, 0x0D0C0B0A09080706ull);
    g_m.chipset.a20 = 0;
    read_once();
    assert(c->gpr[RAX] == 0x0D0C0B0A09080706ull);
    g_m.chipset.a20 = 1;
    read_once();
    assert(c->gpr[RAX] == OLD_VALUE);
    puts("ok p2 closed A20 bypasses TLB, open A20 preserves prior bus");
}
static void mmio_table_uncacheable(void) {
    fixture();
    cpu_t *c = &g_m.cpu;
    /* A page-table ROOT in MMIO is a permitted physical-bus read.  It may
     * return a new PML4E on every call without calling mem_write, so it
     * MUST NOT be cached.  The callback switches between the existing
     * 5MiB/6MiB PDPT trees without any explicit flush. */
    mmio_root_alt = mmio_root_reads = 0;
    mem_register_mmio(&g_m, 0xFEBC0000ull, 0x1000, mmio_root_read,
                      mmio_root_write, NULL, "P2 dynamic MMIO PML4");
    c->cr3 = 0xFEBC0000ull;
    read_once();
    assert(c->gpr[RAX] == OLD_VALUE);
    uint64_t hits = g_m.perf_tlb_hits;
    mmio_root_alt = 1;
    read_once();
    assert(c->gpr[RAX] == NEW_VALUE);
    assert(mmio_root_reads >= 2);
    /* Code fetches use the same MMIO PML4E; no successful translation
     * with a side-effecting table may be cached, even for code. */
    assert(g_m.perf_tlb_hits == hits);
    mmio_region_t *r = g_m.mmio_list;
    assert(r && r->base == 0xFEBC0000ull);
    g_m.mmio_list = r->next;
    free(r);
    puts("ok p2 dynamic MMIO-backed page table never cached");
}
static void smp_and_mode_key(void) {
    fixture();
    cpu_t *c = &g_m.cpu;
    read_once();
    /* Direct CR3 change by harness (no MOV CR3) must also miss due to
     * ASID key; the alternate root gives the new mapping. */
    c->cr3 = 0x20000;
    uint64_t misses = g_m.perf_tlb_misses;
    read_once();
    assert(c->gpr[RAX] == NEW_VALUE && g_m.perf_tlb_misses > misses);
    /* A K6 context switch copies CPU state but NOT the machine-wide TLB.
     * vCPU0 and vCPU1 with distinct CR3/CS/RFLAGS cannot share a hit. */
    g_m.n_vcpus = 2;
    g_m.vcpu[0].c = g_m.cpu;
    g_m.vcpu[1].c = g_m.cpu;
    g_m.vcpu[1].c.cr3 = 0x10000;
    /* A cached entry with the SAME CR3 and security context may validly
     * be shared across vCPUs.  Change AC as well to pin a distinct key,
     * rather than wrongly demanding a miss for an identical context. */
    g_m.vcpu[1].c.rflags |= 1ull << 18;
    g_m.vcpu[0].state = g_m.vcpu[1].state = VCPU_RUN;
    vcpu_load(&g_m, 1);
    misses = g_m.perf_tlb_misses;
    read_once();
    assert(g_m.cpu.gpr[RAX] == OLD_VALUE && g_m.perf_tlb_misses > misses);
    vcpu_store(&g_m, 1);
    vcpu_load(&g_m, 0);
    read_once();
    assert(g_m.cpu.gpr[RAX] == NEW_VALUE);
    puts("ok p2 direct CR3 key + SMP context separation");
}
int main(void) {
    pte_rewrite();
    cr3_and_invlpg();
    mode_control_flushes();
    faults_after_warm();
    permissions_after_warm();
    large_pages_and_a20();
    mmio_table_uncacheable();
    smp_and_mode_key();
    puts("test-tlb: all vectors passed");
    return 0;
}
