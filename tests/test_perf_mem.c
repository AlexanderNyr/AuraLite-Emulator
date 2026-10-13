/* tests/test_perf_mem.c -- PERF P1: narrow, profile-guided RAM read fast path.
 *
 * Five O2 -pg boot runs: 313666250 mem_read calls, 235535030 from translate's
 * 8-byte PTE/PDE/PDPTE/PML4E walks; mem_read 45.66% of samples (before).
 * Only an 8-byte read ENTIRELY inside RAM with A20 open may take memcpy;
 * everything else must produce exactly the old byte-loop/bus result.
 * No wall-clock threshold here: these are architectural/bus invariants.
 *
 * The one P1 negative control changes the 8-byte fast-path return to
 * __builtin_bswap64(v) in a scratch copy of mem.c.  The first vector below
 * asserts the exact LE value and fails (NC script verifies the assertion).
 */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "machine.h"
#include "platform.h"
#include "harness.h"

static machine_t g_m;
static unsigned mmio_reads;

static uint64_t mmio_test_read(void *ctx, uint64_t addr, int size) {
    (void)ctx;
    assert(addr == 0xFEBC0000ULL);
    assert(size == 8);
    ++mmio_reads;
    return 0x1020304050607080ULL;
}
static void mmio_test_write(void *ctx, uint64_t addr, int size, uint64_t val) {
    (void)ctx; (void)addr; (void)size; (void)val;
    assert(!"MMIO write not part of this vector");
}

int main(void) {
    setup_machine(&g_m);
    assert(g_m.chipset.a20);

    /* 1. Unaligned 8-byte little-endian load.  memcpy's host byte order
     * must be the same as the byte loop's LE assembly, and alignment must
     * not produce UB.  The NC deliberately swaps the 8 bytes so it fails
     * THIS assertion (not a later timing threshold). */
    for (int i = 0; i < 16; i++) g_m.ram[0x123 + i] = (uint8_t)(0x10 + i);
    assert(mem_read(&g_m, 0x123, 8) == 0x1716151413121110ULL);
    assert(mem_read(&g_m, 0x124, 8) == 0x1817161514131211ULL);
    puts("ok p1 unaligned LE RAM 8-byte load");

    /* 2. Last eight in RAM are in-range, last seven aren't.  Old mem_read
     * reads bytes inside RAM and fills out-of-range positions with 0xFF,
     * NOT by consulting the unmapped/open-bus handler for the whole read.
     * Using only phys<RAM_SIZE as the fast-path guard would read past the
     * allocation and break this contract. */
    for (int i = 0; i < 8; i++) g_m.ram[RAM_SIZE - 8 + i] = (uint8_t)(0xA0 + i);
    assert(mem_read(&g_m, RAM_SIZE - 8, 8) == 0xA7A6A5A4A3A2A1A0ULL);
    assert(mem_read(&g_m, RAM_SIZE - 7, 8) == 0xFFA7A6A5A4A3A2A1ULL);
    assert(mem_read(&g_m, RAM_SIZE - 1, 8) == 0xFFFFFFFFFFFFFFA7ULL);
    assert(mem_read(&g_m, RAM_SIZE, 8) == 0xFFFFFFFFFFFFFFFFULL);
    mem_write(&g_m, RAM_SIZE - 2, 8, 0x1122334455667788ULL);
    assert(mem_read(&g_m, RAM_SIZE - 2, 8) == 0xFFFFFFFFFFFF7788ULL);
    puts("ok p1 RAM limit: last 8 in range, partial reads fill FF; writes clipped");

    /* 3. ROM alias lives INSIDE RAM.  It must win before the fast path.
     * Crossing the top of the alias wraps (old off+i modulo ROM_SIZE),
     * but an 8-byte read that STARTS below it still reads RAM across the
     * boundary.  That starting-address decode is a historical contract,
     * not an invitation to fix mixed-bus reads during PERF. */
    for (int i = 0; i < 8; i++) g_m.rom[i] = (uint8_t)(0x20 + i);
    for (int i = 0; i < 4; i++) g_m.rom[ROM_SIZE - 4 + i] = (uint8_t)(0x30 + i);
    for (int i = -4; i < 4; i++)
        g_m.ram[ROM_ALIAS_LOW_BASE + i] = (uint8_t)(0x40 + i + 4);
    assert(mem_read(&g_m, ROM_ALIAS_LOW_BASE, 8) == 0x2726252423222120ULL);
    assert(mem_read(&g_m, ROM_TOPOF4G_BASE, 8) == 0x2726252423222120ULL);
    assert(mem_read(&g_m, ROM_ALIAS_LOW_BASE + ROM_SIZE - 4, 8) ==
           0x2322212033323130ULL);
    assert(mem_read(&g_m, ROM_ALIAS_LOW_BASE - 4, 8) ==
           0x4746454443424140ULL);
    mem_write(&g_m, ROM_ALIAS_LOW_BASE, 8, 0xFFFFFFFFFFFFFFFFULL);
    assert(mem_read(&g_m, ROM_ALIAS_LOW_BASE, 8) == 0x2726252423222120ULL);
    puts("ok p1 ROM aliases precede RAM; wrap and start-address decode unchanged");

    /* 4. A20 closed: bus masks bit 20 of the START address only.  This
     * case stays on the old byte loop (even if the masked address is RAM);
     * both modes must agree on the low bytes.  Also exercise the old
     * start-only mask on an 8-byte read straddling the 3-MiB boundary.
     * A 1-MiB straddle would START inside the 0xE0000..0xFFFFF ROM alias
     * after masking A20; it would test ROM wrap rather than RAM. */
    for (int i = 0; i < 8; i++) {
        g_m.ram[0x100 + i] = (uint8_t)(0x50 + i);
        g_m.ram[0x100100 + i] = (uint8_t)(0x60 + i);
    }
    assert(mem_read(&g_m, 0x100100, 8) == 0x6766656463626160ULL);
    g_m.chipset.a20 = 0;
    assert(mem_read(&g_m, 0x100100, 8) == 0x5756555453525150ULL);
    for (int i = 0; i < 8; i++) g_m.ram[0x2FFFFE + i] = (uint8_t)(0x70 + i);
    assert(mem_read(&g_m, 0x3FFFFE, 8) == 0x7776757473727170ULL);
    g_m.chipset.a20 = 1;
    puts("ok p1 disabled A20 retains start-only mask and slow path");

    /* 5. Other widths are untouched, including unaligned reads. */
    assert(mem_read(&g_m, 0x123, 1) == 0x10);
    assert(mem_read(&g_m, 0x123, 2) == 0x1110);
    assert(mem_read(&g_m, 0x123, 4) == 0x13121110);
    puts("ok p1 1/2/4-byte reads unchanged");

    /* 6. MMIO is decoded only after RAM and ROM.  Above-RAM BAR dispatches
     * its read callback; below-RAM registered region still reads RAM (the
     * existing RAM-priority contract).  This vector also guarantees no
     * BAR or firmware-facing MMIO contract was widened by the fast path. */
    mmio_reads = 0;
    mem_register_mmio(&g_m, 0xFEBC0000ULL, 8, mmio_test_read,
                      mmio_test_write, NULL, "P1 test high BAR");
    assert(mem_read(&g_m, 0xFEBC0000ULL, 8) == 0x1020304050607080ULL);
    assert(mmio_reads == 1);
    mem_register_mmio(&g_m, 0x123, 8, mmio_test_read,
                      mmio_test_write, NULL, "P1 test RAM-priority");
    assert(mem_read(&g_m, 0x123, 8) == 0x1716151413121110ULL);
    assert(mmio_reads == 1);
    while (g_m.mmio_list) {
        mmio_region_t *r = g_m.mmio_list;
        g_m.mmio_list = r->next;
        free(r);
    }
    puts("ok p1 MMIO BAR and RAM priority unchanged");

    puts("test-perf-mem: all vectors passed");
    return 0;
}
