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
/* CPU_FEATURES CF1: the SMEP/SMAP contract, advertised only as of this
 * phase. Own fixture: the identity 2MiB is SUPERVISOR-mapped (PD[0] has
 * U/S=0) so CPL0 code at 0x1000 may execute with SMEP on; VA (4MiB)
 * reaches a U/S=1 page (phys 5MiB; code at +0x0, data at +0x800).
 * Case order matters for the NC: the first assertion is the one the
 * pre-CF1 inverted walk fails. */
#define SMEP (1ull << 20)
#define SMAP (1ull << 21)
#define ACF  (1ull << 18)
#define UDATA (VA + 0x800ull)
#define UVAL 0x2222222222222222ull
#define KVAL 0x1111111111111111ull
static void smfix(const uint8_t *code, size_t n) {
    setup_machine(&g_m);
    cpu_t *c = &g_m.cpu;
    w64(0x10000, 0x11000 | P | W | US);   /* PML4[0] */
    w64(0x11000, 0x12000 | P | W | US);   /* PDPT[0] */
    w64(0x12000, P | W | PS);             /* PD[0]: identity 2MiB SUPERVISOR */
    w64(0x12010, 0x13000 | P | W | US);   /* PD[2] -> PT */
    w64(0x13000, 0x500000 | P | W | US);  /* PT[0] -> VA user page */
    w64(0x500800, UVAL);                  /* user data at VA+0x800 */
    w64(0x4000, KVAL);                    /* supervisor data at 0x4000 */
    g_m.ram[0x500000] = 0x90; g_m.ram[0x500001] = 0x90;  /* user NOPs at VA */
    static const uint8_t movusr[] = { 0x48, 0x8B, 0x03 }; /* MOV RAX,[RBX] */
    memcpy(g_m.ram + 0x500010, movusr, sizeof movusr);    /* user code too */
    static const uint8_t stac1[] = { 0x0F, 0x01, 0xCB };  /* STAC */
    memcpy(g_m.ram + 0x500020, stac1, sizeof stac1);
    c->cr3 = 0x10000;
    c->cr4 = 0x20;                           /* PAE; SMEP/SMAP per case */
    c->efer = (1ull<<8) | (1ull<<10) | (1ull<<11);
    c->cr0 = 0x80010033ull;
    c->idtr_base = 0x0C00; c->idtr_limit = 256 * 16 - 1;
    w64(0x2000 + 4, 0x30000);
    c->tr_base = 0x2000; c->tr_limit = 103;
    for (int v = 0; v < 32; v++) gate(v, 8, 0x9000 + (uint64_t)v * 8);
    memset(g_m.ram + 0x9000, 0xF4, 32 * 8);
    c->seg[SEG_CS] = (segment_t){ .sel = 8, .base = 0, .limit = ~0u, .l = 1 };
    c->seg[SEG_SS] = (segment_t){ .sel = 0x10, .base = 0, .limit = ~0u };
    c->gpr[RSP] = 0x8000;
    memcpy(g_m.ram + 0x1000, code, n);       /* supervisor code */
}
static void smstep(uint64_t rip) {
    g_m.cpu.rip = rip;
    assert(cpu_step(&g_m.cpu) == 0);
    assert(!g_m.cpu.fault && !g_m.cpu.exception_taken);
}
static void smep_smap(void) {
    static const uint8_t mov_r[] = { 0x48, 0x8B, 0x03 };        /* MOV RAX,[RBX] */
    static const uint8_t mov_w[] = { 0x48, 0x89, 0x03 };        /* MOV [RBX],RAX */
    static const uint8_t stac_r_clac_r[] = {
        0x0F, 0x01, 0xCB, 0x48, 0x8B, 0x03,
        0x48, 0x8B, 0x03, 0x0F, 0x01, 0xCA, 0x48, 0x8B, 0x03 }; /* STAC MOV MOV CLAC MOV */
    static const uint8_t stac_mov[] = { 0x0F, 0x01, 0xCB, 0x48, 0x8B, 0x03 };
    cpu_t *c;

    /* 1: SMEP on: supervisor code runs, supervisor data reads (the
     * CF0 inversion faulted on this very fetch). */
    smfix(mov_r, sizeof mov_r); c = &g_m.cpu;
    c->cr4 |= SMEP; c->gpr[RBX] = 0x4000;
    smstep(0x1000);
    assert(c->gpr[RAX] == KVAL);
    puts("ok cf1 SMEP: CPL0 executes + reads supervisor pages");

    /* 2: SMEP on: CPL0 fetch from a user page -> #PF err=P|I/D. */
    smfix(mov_r, sizeof mov_r); c = &g_m.cpu;
    c->cr4 |= SMEP;
    c->rip = VA; (void)cpu_step(c);
    assert(c->cr2 == VA && c->rip == 0x9000 + 14 * 8);
    assert(r64(c->gpr[RSP]) == 0x11);
    puts("ok cf1 SMEP: CPL0 exec of user page faults P|I/D");

    /* 3: control: same fetch with SMEP off executes. */
    smfix(mov_r, sizeof mov_r); c = &g_m.cpu;
    smstep(VA);
    puts("ok cf1 control: SMEP off executes the user page");

    /* 4: SMAP on, AC=0: CPL0 read and write of user data fault. */
    smfix(mov_r, sizeof mov_r); c = &g_m.cpu;
    c->cr4 |= SMAP; c->rflags = 0; c->gpr[RBX] = UDATA;
    c->rip = 0x1000; (void)cpu_step(c);
    assert(c->cr2 == UDATA && c->rip == 0x9000 + 14 * 8);
    assert(r64(c->gpr[RSP]) == 0x1);            /* P */
    smfix(mov_w, sizeof mov_w); c = &g_m.cpu;
    c->cr4 |= SMAP; c->rflags = 0; c->gpr[RBX] = UDATA;
    c->rip = 0x1000; (void)cpu_step(c);
    assert(c->cr2 == UDATA && r64(c->gpr[RSP]) == 0x3); /* P|W */
    puts("ok cf1 SMAP AC=0: CPL0 read P / write P|W faults on user data");

    /* 5: the STAC/CLAC sandwich, twice-warmed so a stale TLB hit
     * cannot smuggle the post-CLAC read through (AC is part of the
     * P2 key). Also pins the pre-CF1 fallthrough twice over: the
     * second read must return the USER value (not the IDT image the
     * stray SIDT stored at linear 0 -- the canary) and linear 0 must
     * survive STAC+CLAC byte-identical. */
    smfix(stac_r_clac_r, sizeof stac_r_clac_r); c = &g_m.cpu;
    w64(0, 0xC0FFEE00C0FFEE00ull);
    c->cr4 |= SMAP; c->rflags = 0; c->gpr[RBX] = UDATA;
    smstep(0x1000);                             /* STAC */
    assert(c->rflags & ACF);
    smstep(0x1003);                             /* MOV (warm once) */
    assert(c->gpr[RAX] == UVAL);
    smstep(0x1006);                             /* MOV (warm hit) */
    assert(c->gpr[RAX] == UVAL);
    smstep(0x1009);                             /* CLAC */
    assert(!(c->rflags & ACF));
    assert(r64(0) == 0xC0FFEE00C0FFEE00ull);    /* no stray SIDT store */
    c->rip = 0x100C; (void)cpu_step(c);         /* MOV -> #PF */
    assert(c->cr2 == UDATA && c->rip == 0x9000 + 14 * 8);
    assert(r64(c->gpr[RSP]) == 0x1);
    puts("ok cf1 SMAP: STAC opens, CLAC shuts (warm TLB), no SIDT fallthrough");

    /* 6: SMAP on AC=0: supervisor data is unaffected. */
    smfix(mov_r, sizeof mov_r); c = &g_m.cpu;
    c->cr4 |= SMAP; c->rflags = 0; c->gpr[RBX] = 0x4000;
    smstep(0x1000);
    assert(c->gpr[RAX] == KVAL);
    puts("ok cf1 SMAP AC=0: supervisor data reads normally");

    /* 7: SMAP does not constrain CPL3 at all: user code reading a
     * user page with AC=0 succeeds. */
    smfix(mov_r, sizeof mov_r); c = &g_m.cpu;
    c->cr4 |= SMAP; c->rflags = 0;
    c->seg[SEG_CS].sel = 0x1B; c->seg[SEG_SS].sel = 0x23;
    c->gpr[RSP] = 0x501F00; c->gpr[RBX] = UDATA;
    smstep(VA + 0x10);
    assert(c->gpr[RAX] == UVAL);
    puts("ok cf1 SMAP: CPL3 data access is not the guard's business");

    /* 8: STAC at CPL3 is the architected #UD, delivered. */
    smfix(mov_r, sizeof mov_r); c = &g_m.cpu;
    c->cr4 |= SMAP;
    c->seg[SEG_CS].sel = 0x1B; c->seg[SEG_SS].sel = 0x23;
    c->gpr[RSP] = 0x501F00;
    c->rip = VA + 0x20; (void)cpu_step(c);
    assert(c->exception_taken && c->rip == 0x9000 + 6 * 8);
    assert(!(c->rflags & ACF));
    puts("ok cf1 STAC at CPL3 delivers #UD");

    /* 9: AC across delivery: open door, fault elsewhere; the pushed
     * RFLAGS keeps AC=1 (IRET restores it), the handler starts shut. */
    smfix(stac_mov, sizeof stac_mov); c = &g_m.cpu;
    c->cr4 |= SMAP; c->rflags = 0; c->gpr[RBX] = 0x8000000000ull; /* unmapped */
    smstep(0x1000);                             /* STAC */
    assert(c->rflags & ACF);
    c->rip = 0x1003; (void)cpu_step(c);         /* MOV -> #PF not-present */
    assert(c->rip == 0x9000 + 14 * 8);
    assert(!(c->rflags & ACF));                 /* handler starts shut */
    assert(r64(c->gpr[RSP] + 24) & ACF);        /* frame: err,RIP,CS,RFLAGS */
    puts("ok cf1 AC cleared on delivery, preserved in the frame");
}

/* CPU_FEATURES CF3: the PCID + INVPCID contract. The TLB is
 * flush-always by design (P2), so tagging is semantically inert
 * here; what these vectors pin is everything a guest can
 * *observe*: CR4.PCIDE legality edges, CR3[63] as hint-not-state,
 * the PCID field store/echo, correct translations across the
 * guest's CR3-reload-with-tags switch (AuraLite-OS's deferred PCID
 * runtime, whose "no invpcid" fallback is exactly that reload),
 * and INVPCID's full decode + #GP surface. Case 1 leads with a
 * legality reject so the NC isolation starts at the first assert. */
#define PCIDE (1ull << 17)
#define PV2 0x3333333333333333ull
/* smfix's T1 root (VA -> phys 0x500000, UDATA == UVAL) plus a
 * second root T2 at 0x14000 mapping the same VA to 0x600000. */
static void pcfix2(const uint8_t *code, size_t n) {
    smfix(code, n);
    w64(0x14000, 0x15000 | P | W | US);   /* T2 PML4[0] */
    w64(0x15000, 0x16000 | P | W | US);   /* T2 PDPT[0] */
    w64(0x16000, P | W | PS);             /* T2 PD[0]: identity 2MiB */
    w64(0x16010, 0x17000 | P | W | US);   /* T2 PD[2] -> PT */
    w64(0x17000, 0x600000 | P | W | US);  /* T2 PT[0] -> VA, alt page */
    w64(0x600800, PV2);                   /* UDATA under T2 */
}
/* one INVPCID (rax=type, desc at [0x3000]) in a fresh smfix;
 * returns the delivered vector, or -1 when it executed clean. */
static int inv_run(uint64_t type, uint64_t dlo, uint64_t dhi, int pcide) {
    static const uint8_t prog[] = { 0x66, 0x0F, 0x38, 0x82, 0x03,   /* INVPCID RAX,[RBX] */
                                     0x48, 0x8B, 0x01 };             /* MOV RAX,[RCX] */
    smfix(prog, sizeof prog);
    cpu_t *c = &g_m.cpu;
    if (pcide) c->cr4 |= PCIDE;
    w64(0x3000, dlo); w64(0x3008, dhi);
    c->gpr[RAX] = type; c->gpr[RBX] = 0x3000; c->gpr[RCX] = UDATA;
    c->rip = 0x1000; (void)cpu_step(c);
    return c->exception_taken ? (int)((c->rip - 0x9000) / 8) : -1;
}
static void pcid(void) {
    static const uint8_t setpcide[] = {
        0x48, 0xB8, 0x20, 0x00, 0x02, 0x00, 0x00, 0x00, 0x00, 0x00, /* MOV RAX,PAE|PCIDE */
        0x0F, 0x22, 0xE0 };                                         /* MOV CR4,RAX @10 */
    static const uint8_t cr3b63[] = {
        0x48, 0xB8, 0x07, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x80, /* MOV RAX,B63|T1|7 */
        0x0F, 0x22, 0xD8,                                           /* MOV CR3,RAX @10 */
        0x0F, 0x20, 0xD8 };                                         /* MOV RAX,CR3 @13 */
    cpu_t *c;

    /* 1: the 0->1 PCIDE edge with CR3[11:0]!=0 -> delivered #GP(0),
     * CR4 untouched. */
    smfix(setpcide, sizeof setpcide); c = &g_m.cpu;
    c->cr3 |= 5;                          /* stale low bits (ignored while PCIDE=0) */
    smstep(0x1000);
    c->rip = 0x100A; (void)cpu_step(c);
    assert(c->exception_taken && c->rip == 0x9000 + 13 * 8);
    assert(r64(c->gpr[RSP]) == 0);
    assert(!(c->cr4 & PCIDE));
    puts("ok cf3 PCIDE 0->1 with CR3[11:0]!=0 delivers #GP(0)");

    /* 2: writing PCIDE=1 outside IA-32e -> #GP(0), here in real
     * mode through the IVT (the !LMA branch covers legacy too). */
    setup_machine(&g_m); c = &g_m.cpu;
    g_m.ram[0x8000] = 0xF4;
    mem_write(&g_m, 13 * 4, 2, 0x8000); mem_write(&g_m, 13 * 4 + 2, 2, 0x0000);
    static const uint8_t rm_prog[] = { 0x66, 0xB8, 0x20, 0x00, 0x02, 0x00, /* MOV EAX,PAE|PCIDE */
                                       0x0F, 0x22, 0xE0 };                 /* MOV CR4,EAX @6 */
    memcpy(g_m.ram + 0x1000, rm_prog, sizeof rm_prog);
    c->gpr[RSP] = 0x7000;
    c->rip = 0x1000; assert(cpu_step(c) == 0 && !c->fault && !c->exception_taken);
    c->rip = 0x1006; (void)cpu_step(c);
    assert(c->exception_taken && c->rip == 0x8000 && c->seg[SEG_CS].sel == 0);
    assert(!(c->cr4 & PCIDE));
    puts("ok cf3 PCIDE=1 outside IA-32e delivers #GP (real mode/IVT)");

    /* 3: the legal edge: CR3[11:0]==0 -> PCIDE sticks; writing it
     * again while already 1 is not an edge (low CR3 bits may now be
     * a live PCID) and must not fault. */
    smfix(setpcide, sizeof setpcide); c = &g_m.cpu;
    smstep(0x1000); smstep(0x100A);
    assert(c->cr4 & PCIDE);
    c->cr3 |= 9;
    smstep(0x1000); smstep(0x100A);
    assert(c->cr4 & PCIDE);
    puts("ok cf3 PCIDE legal edge sticks; re-write while 1 is no edge");

    /* 4: MOV CR3 bit 63 with PCIDE=0 -> #GP(0), CR3 unchanged. */
    smfix(cr3b63, sizeof cr3b63); c = &g_m.cpu;
    smstep(0x1000);
    c->rip = 0x100A; (void)cpu_step(c);
    assert(c->exception_taken && c->rip == 0x9000 + 13 * 8);
    assert(r64(c->gpr[RSP]) == 0 && c->cr3 == 0x10000);
    puts("ok cf3 CR3 bit63 with PCIDE=0 delivers #GP(0)");

    /* 5: with PCIDE=1 bit 63 is the no-invalidate hint: accepted,
     * never stored; PCID [11:0] stores and MOV-from-CR3 echoes it. */
    smfix(cr3b63, sizeof cr3b63); c = &g_m.cpu;
    c->cr4 |= PCIDE;
    smstep(0x1000); smstep(0x100A); smstep(0x100D);
    assert(c->gpr[RAX] == 0x10007 && c->cr3 == 0x10007);
    puts("ok cf3 CR3 bit63 hint unstored; PCID stored + echoed");

    /* 6: the guest's way -- address-space switches as CR3 reloads
     * with PCID tags and the bit-63 no-flush hint. Every read must
     * see the CURRENT root's mapping (flush-always + cr3-keyed TLB
     * make stale survival impossible, hint or not). */
    static const uint8_t swprog[] = {
        0x48, 0xB8, 0x01, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x80, /*  0: B63|T1|1 */
        0x0F, 0x22, 0xD8,                                           /* 10: MOV CR3,RAX */
        0x48, 0x8B, 0x03,                                           /* 13: MOV RAX,[RBX] */
        0x48, 0xB8, 0x02, 0x40, 0x01, 0x00, 0x00, 0x00, 0x00, 0x80, /* 16: B63|T2|2 */
        0x0F, 0x22, 0xD8,                                           /* 26 */
        0x48, 0x8B, 0x03,                                           /* 29 */
        0x48, 0xB8, 0x01, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x80, /* 32: back to T1|1 */
        0x0F, 0x22, 0xD8,                                           /* 42 */
        0x48, 0x8B, 0x03 };                                         /* 45 */
    pcfix2(swprog, sizeof swprog); c = &g_m.cpu;
    c->cr4 |= PCIDE; c->gpr[RBX] = UDATA;
    smstep(0x1000); smstep(0x100A); smstep(0x100D);
    assert(c->gpr[RAX] == UVAL);
    smstep(0x1010); smstep(0x101A); smstep(0x101D);
    assert(c->gpr[RAX] == PV2);
    smstep(0x1020); smstep(0x102A); smstep(0x102D);
    assert(c->gpr[RAX] == UVAL);
    puts("ok cf3 two-PCID CR3-reload switch reads the current root only");

    /* 7: all four INVPCID types execute at CPL0/PCIDE=1 and leave
     * translation correct. */
    for (uint64_t t = 0; t < 4; t++) {
        assert(inv_run(t, 2, VA, 1) == -1);
        smstep(0x1005);
        assert(g_m.cpu.gpr[RAX] == UVAL);
    }
    puts("ok cf3 INVPCID types 0..3 execute; translations survive");

    /* 8: type > 3 -> #GP(0). */
    assert(inv_run(4, 0, 0, 1) == 13);
    assert(r64(g_m.cpu.gpr[RSP]) == 0);
    puts("ok cf3 INVPCID type 4 delivers #GP(0)");

    /* 9: descriptor reserved band [63:12] -> #GP(0). */
    assert(inv_run(0, 1ull << 12, VA, 1) == 13);
    assert(r64(g_m.cpu.gpr[RSP]) == 0);
    puts("ok cf3 INVPCID descriptor [63:12] set delivers #GP(0)");

    /* 10: type-0 address must be canonical (48-bit VA). */
    assert(inv_run(0, 0, 0x0000800000000000ull, 1) == 13);
    assert(inv_run(0, 0, 0xFFFF800000000000ull, 1) == -1);
    puts("ok cf3 INVPCID type-0: non-canonical #GP(0), high-canonical ok");

    /* 11: PCIDE=0 does not #UD the instruction -- it narrows it:
     * types 0/1 must name PCID 000H, types 2/3 run (SDM text). */
    assert(inv_run(1, 1, 0, 0) == 13);
    assert(inv_run(0, 0, VA, 0) == -1);
    assert(inv_run(2, 0, 0, 0) == -1);
    assert(inv_run(3, 0, 0, 0) == -1);
    puts("ok cf3 INVPCID with PCIDE=0: narrowed, not #UD");

    /* 12: encoding law: 66 is opcode-defining, m128 is memory-only. */
    static const uint8_t no66[] = { 0x0F, 0x38, 0x82, 0x03 };
    smfix(no66, sizeof no66); c = &g_m.cpu;
    c->cr4 |= PCIDE;
    c->rip = 0x1000; (void)cpu_step(c);
    assert(c->exception_taken && c->rip == 0x9000 + 6 * 8);
    static const uint8_t regform[] = { 0x66, 0x0F, 0x38, 0x82, 0xC3 }; /* mod=3 */
    smfix(regform, sizeof regform); c = &g_m.cpu;
    c->cr4 |= PCIDE;
    c->rip = 0x1000; (void)cpu_step(c);
    assert(c->exception_taken && c->rip == 0x9000 + 6 * 8);
    puts("ok cf3 INVPCID without 66 / with mod=3 delivers #UD");

    /* 13: CPL3 -> #GP(0) (privilege is checked before the descriptor
     * is ever touched). */
    static const uint8_t invb[] = { 0x66, 0x0F, 0x38, 0x82, 0x03 };
    smfix(invb, sizeof invb); c = &g_m.cpu;
    memcpy(g_m.ram + 0x500030, invb, sizeof invb);   /* user copy at VA+0x30 */
    c->cr4 |= PCIDE;
    c->seg[SEG_CS].sel = 0x1B; c->seg[SEG_SS].sel = 0x23;
    c->gpr[RSP] = 0x501F00; c->gpr[RAX] = 2; c->gpr[RBX] = UDATA;
    c->rip = VA + 0x30; (void)cpu_step(c);
    assert(c->exception_taken && c->rip == 0x9000 + 13 * 8);
    assert(r64(c->gpr[RSP]) == 0);
    puts("ok cf3 INVPCID at CPL3 delivers #GP(0)");
}

/* CPU_FEATURES CF4: the validation surface, switched on only after
 * a scratch census (instrumented sites, firmware 5 profiles, both
 * SeaBIOS ISO lanes, the full suite) counted ZERO writes outside
 * every mask below -- and one census find fell out of reading the
 * sites: MOV CRn and RDMSR/WRMSR had no CPL check at all. The
 * unknown-MSR whitelist is this phase's evidenced NO-GO (pinned
 * permissive in tests/test_cpu.c::test_cf4_units). */
static int mk_crimm(uint8_t *b, uint64_t imm, uint8_t modrm) {
    b[0] = 0x48; b[1] = 0xB8;
    for (int i = 0; i < 8; i++) b[2 + i] = (uint8_t)(imm >> (8 * i));
    b[10] = 0x0F; b[11] = 0x22; b[12] = modrm;
    return 13;
}
static void expect_gp0(cpu_t *c, uint64_t rip) {
    c->rip = rip; (void)cpu_step(c);
    assert(c->exception_taken && c->rip == 0x9000 + 13 * 8);
    assert(r64(c->gpr[RSP]) == 0);
}
static void cf4_validate(void) {
    uint8_t prog[16];
    cpu_t *c;
    int n;

    /* 1: a reserved CR4 bit (LA57: unadvertised) -> #GP(0), CR4
     * untouched. Lead case = the NC discriminator. */
    n = mk_crimm(prog, 0x20 | (1ull << 12), 0xE0);
    smfix(prog, (size_t)n); c = &g_m.cpu;
    smstep(0x1000); expect_gp0(c, 0x100A);
    assert(c->cr4 == 0x20);
    puts("ok cf4 CR4 reserved bit delivers #GP(0)");

    /* 2: the mask is exact, not approximate: every allowed bit at
     * once (base 0-10 + PCIDE + SMEP + SMAP) lands clean. */
    n = mk_crimm(prog, 0x3207FFull, 0xE0);
    smfix(prog, (size_t)n); c = &g_m.cpu;
    smstep(0x1000); smstep(0x100A);
    assert(c->cr4 == 0x3207FFull);
    puts("ok cf4 CR4 full advertised mask accepted verbatim");

    /* 3-5: CR0's three #GP families; CD|NW together stay legal. */
    n = mk_crimm(prog, 0x80010033ull | (1ull << 32), 0xC0);
    smfix(prog, (size_t)n); c = &g_m.cpu;
    smstep(0x1000); expect_gp0(c, 0x100A);
    assert(c->cr0 == 0x80010033ull);
    puts("ok cf4 CR0[63:32] delivers #GP(0)");

    n = mk_crimm(prog, 0x80000032ull, 0xC0);            /* PG, no PE */
    smfix(prog, (size_t)n); c = &g_m.cpu;
    smstep(0x1000); expect_gp0(c, 0x100A);
    puts("ok cf4 CR0 PG-without-PE delivers #GP(0)");

    n = mk_crimm(prog, 0x80010033ull | (1ull << 29), 0xC0); /* NW, no CD */
    smfix(prog, (size_t)n); c = &g_m.cpu;
    smstep(0x1000); expect_gp0(c, 0x100A);
    n = mk_crimm(prog, 0x80010033ull | (1ull << 29) | (1ull << 30), 0xC0);
    smfix(prog, (size_t)n); c = &g_m.cpu;
    smstep(0x1000); smstep(0x100A);
    assert(c->cr0 == (0x80010033ull | (1ull << 29) | (1ull << 30)));
    puts("ok cf4 CR0 NW-needs-CD (#GP) with CD|NW accepted");

    /* 6: PG cannot be cleared in 64-bit mode; and the compat-mode
     * twin from CF3's deferral list: with PCIDE it #GPs there too,
     * without PCIDE it is the legal IA-32e exit. */
    n = mk_crimm(prog, 0x00010033ull, 0xC0);
    smfix(prog, (size_t)n); c = &g_m.cpu;
    smstep(0x1000); expect_gp0(c, 0x100A);
    assert(c->cr0 & (1ull << 31));
    static const uint8_t pgclr32[] = { 0xB8, 0x33, 0x00, 0x01, 0x00,  /* MOV EAX,0x10033 */
                                       0x0F, 0x22, 0xC0 };             /* MOV CR0,EAX @5 */
    smfix(pgclr32, sizeof pgclr32); c = &g_m.cpu;
    c->seg[SEG_CS].l = 0; c->seg[SEG_CS].d_b = 1;       /* compat sub-mode */
    c->cr4 |= PCIDE;
    smstep(0x1000);
    c->rip = 0x1005; (void)cpu_step(c);   /* no expect_gp0: this core's
        delivery picks the frame width from the current CS, so a compat
        fault lands as a 32-bit frame (err is the low dword at [RSP]).
        Silicon in IA-32e always frames 64-bit; the width rule is a
        K4-era delivery property, recorded in the plan, not fixed here. */
    assert(c->exception_taken && c->rip == 0x9000 + 13 * 8);
    assert((uint32_t)r64(c->gpr[RSP]) == 0);
    smfix(pgclr32, sizeof pgclr32); c = &g_m.cpu;
    c->seg[SEG_CS].l = 0; c->seg[SEG_CS].d_b = 1;
    smstep(0x1000); smstep(0x1005);                     /* the legal exit */
    assert(!(c->cr0 & (1ull << 31)) && !(c->efer & (1ull << 10)));
    puts("ok cf4 CR0 PG-clear: #GP in 64-bit + compat/PCIDE, legal exit without");

    /* 7: CR3 above the 40-bit physical address -> #GP(0). */
    n = mk_crimm(prog, 0x10000ull | (1ull << 40), 0xD8);
    smfix(prog, (size_t)n); c = &g_m.cpu;
    smstep(0x1000); expect_gp0(c, 0x100A);
    assert(c->cr3 == 0x10000);
    puts("ok cf4 CR3[62:40] delivers #GP(0)");

    /* 8-9: EFER is the strict MSR: outside {SCE,LME,LMA,NXE} #GPs;
     * LMA itself is a read-only output (source bit ignored, live
     * value preserved, table and register congruent, RDMSR agrees). */
    static const uint8_t wr_rd[] = { 0x0F, 0x30, 0x0F, 0x32 };       /* WRMSR; RDMSR */
    smfix(wr_rd, sizeof wr_rd); c = &g_m.cpu;
    c->gpr[RCX] = 0xC0000080; c->gpr[RAX] = 0x100; c->gpr[RDX] = 1;  /* bit32 garbage */
    expect_gp0(c, 0x1000);
    assert(c->efer == ((1ull << 8) | (1ull << 10) | (1ull << 11)));
    puts("ok cf4 EFER reserved bit delivers #GP(0)");

    smfix(wr_rd, sizeof wr_rd); c = &g_m.cpu;
    c->gpr[RCX] = 0xC0000080; c->gpr[RAX] = 0x101; c->gpr[RDX] = 0;  /* SCE|LME, LMA=0 */
    smstep(0x1000);
    assert(c->efer == 0x501);                                        /* LMA preserved */
    { int f; assert(cpu_get_msr(c, 0xC0000080, &f) == 0x501 && f); } /* one EFER only */
    smstep(0x1002);
    assert((uint32_t)c->gpr[RAX] == 0x501);
    puts("ok cf4 EFER write: LMA preserved, readback congruent");

    /* 10: the census find -- the privilege gates that were missing
     * entirely. WRMSR and MOV CRn from ring 3 both deliver #GP(0). */
    static const uint8_t uwr[] = { 0x0F, 0x30 };
    smfix(uwr, sizeof uwr); c = &g_m.cpu;
    memcpy(g_m.ram + 0x500040, uwr, sizeof uwr);
    c->seg[SEG_CS].sel = 0x1B; c->seg[SEG_SS].sel = 0x23;
    c->gpr[RSP] = 0x501F00; c->gpr[RCX] = 0x277; c->gpr[RAX] = 0; c->gpr[RDX] = 0;
    expect_gp0(c, VA + 0x40);
    static const uint8_t ucr[] = { 0x0F, 0x22, 0xE0 };                /* MOV CR4,RAX */
    smfix(ucr, sizeof ucr); c = &g_m.cpu;
    memcpy(g_m.ram + 0x500040, ucr, sizeof ucr);
    c->seg[SEG_CS].sel = 0x1B; c->seg[SEG_SS].sel = 0x23;
    c->gpr[RSP] = 0x501F00; c->gpr[RAX] = 0x20;
    expect_gp0(c, VA + 0x40);
    puts("ok cf4 CPL3 WRMSR and MOV CRn deliver #GP(0)");

    /* 11: D6's other half at 64-bit: POPCNT across the full width;
     * bare 0F B8 keeps the house #UD (host fault, not delivered). */
    static const uint8_t pc64[] = { 0xF3, 0x48, 0x0F, 0xB8, 0xC3 };   /* POPCNT RAX,RBX */
    smfix(pc64, sizeof pc64); c = &g_m.cpu;
    c->gpr[RBX] = 0x8000000000000001ull; c->rflags = 0xCD5;
    smstep(0x1000);
    assert(c->gpr[RAX] == 2 && !(c->rflags & 0x8D5));
    static const uint8_t jmpe[] = { 0x48, 0x0F, 0xB8, 0xC3 };         /* no F3 */
    smfix(jmpe, sizeof jmpe); c = &g_m.cpu;
    c->rip = 0x1000; (void)cpu_step(c);
    assert(c->fault && !c->exception_taken);
    puts("ok cf4 POPCNT 64-bit counts + flags; bare 0F B8 stays #UD");
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
    smep_smap();
    pcid();
    cf4_validate();
    puts("test-tlb: all vectors passed");
    return 0;
}
