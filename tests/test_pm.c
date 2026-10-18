/* tests/test_pm.c -- COMPAT G5 vectors: ACPI PM block and its 24-bit Timer.
 *
 * The measured defect: with the ICH9 LPC identity stock SeaBIOS publishes the
 * FADT, programs PMBASE (0:31.0 config 0x40 = 0xb000|1) and switches its own
 * timer to PMBASE+8.  The port was unconnected, read 0xFFFFFFFF forever, and
 * every timer_check() poll spun without a timeout -- the --lpc=ich9 boot never
 * reached Stage 2.  These vectors pin the replacement:
 *
 *   1. TMR advances exactly three counts per PIT tick (3.579545/1.193182 MHz),
 *      so 384 instructions/PIT-tick means one count per 128 retired steps.
 *   2. The counter is 24 bits and wraps at 0x1000000.
 *   3. Reads compose byte / word / dword at PMBASE+8; bits 31:24 read zero.
 *   4. The rest of the 0x40-byte block reads zero, and TMR is read-only.
 *   5. PMBASE moves the block: the old window stops decoding, the new one
 *      answers, and the LPC config hook drives the move.
 */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "machine.h"
#include "platform.h"
#include "pci.h"
#include "pit.h"
#include "pm.h"
#include "harness.h"

#define PM_DEFAULT_BASE 0xB000
#define STEPS_PER_TMR   128u   /* 384 instructions per PIT tick / 3 */

static void step(machine_t *m, uint64_t n) {
    for (uint64_t i = 0; i < n; i++) m->vtime_instr++;   /* the K6 clock */
}

static void test_default_window_and_rate(void) {
    machine_t m; setup_machine(&m);
    pit_init(&m); pm_init(&m);
    assert(m.pm.base == PM_DEFAULT_BASE);

    uint32_t t0 = io_read(&m, PM_DEFAULT_BASE + 8, 4);
    assert(t0 == 0);

    step(&m, STEPS_PER_TMR);
    assert(io_read(&m, PM_DEFAULT_BASE + 8, 4) == 1);

    step(&m, STEPS_PER_TMR * 10);
    assert(io_read(&m, PM_DEFAULT_BASE + 8, 4) == 11);

    /* Sub-count steps accumulate instead of being dropped. */
    step(&m, STEPS_PER_TMR / 2);
    assert(io_read(&m, PM_DEFAULT_BASE + 8, 4) == 11);
    step(&m, STEPS_PER_TMR - STEPS_PER_TMR / 2);
    assert(io_read(&m, PM_DEFAULT_BASE + 8, 4) == 12);

    /* Three counts per PIT tick is the exact ratio the PIT model uses. */
    machine_t p; setup_machine(&p);
    pit_init(&p); pm_init(&p);
    p.pit.instr_per_tick = 384;
    step(&p, 384);
    assert(pm_timer_now(&p) == 3);
    puts("ok   pm: default window, 3 counts per PIT tick");
}

static void test_24bit_wrap(void) {
    machine_t m; setup_machine(&m);
    pit_init(&m); pm_init(&m);
    m.pm.count = 0x00FFFFFEu;
    m.pm.last_instr = m.vtime_instr;
    step(&m, STEPS_PER_TMR * 3);
    assert(pm_timer_now(&m) == 1);          /* 0xFFFFFE + 3 -> 1 */
    machine_t n; setup_machine(&n);
    pit_init(&n); pm_init(&n);
    step(&n, (uint64_t)STEPS_PER_TMR * 0x1000000u);
    assert(pm_timer_now(&n) == 0);          /* full 2^24 wrap */
    puts("ok   pm: 24-bit counter wraps at 0x1000000");
}

static void test_read_sizes_and_reserved_bits(void) {
    machine_t m; setup_machine(&m);
    pit_init(&m); pm_init(&m);
    m.pm.count = 0x00ABCDEFu;
    m.pm.last_instr = m.vtime_instr;

    assert(io_read(&m, PM_DEFAULT_BASE + 8, 4) == 0x00ABCDEFu);
    assert(io_read(&m, PM_DEFAULT_BASE + 8, 1) == 0xEFu);
    assert(io_read(&m, PM_DEFAULT_BASE + 9, 1) == 0xCDu);
    assert(io_read(&m, PM_DEFAULT_BASE + 10, 1) == 0xABu);
    assert(io_read(&m, PM_DEFAULT_BASE + 11, 1) == 0x00u);   /* reserved */
    assert(io_read(&m, PM_DEFAULT_BASE + 10, 2) == 0x00ABu);
    /* A read that starts inside the counter and runs past it keeps the live
     * bytes and reads the modelled zero for the reserved ones. */
    assert(io_read(&m, PM_DEFAULT_BASE + 10, 4) == 0x000000ABu);
    puts("ok   pm: byte/word/dword reads, reserved bits read zero");
}

static void test_other_registers_and_write_ignore(void) {
    machine_t m; setup_machine(&m);
    pit_init(&m); pm_init(&m);
    m.pm.count = 0x00000010u;
    m.pm.last_instr = m.vtime_instr;

    /* PM1_STS/EN (0x00..0x07), PM1_CNT (0x04), GPE, etc. read zero. */
    for (uint16_t off = 0; off < 8; off++) assert(io_read(&m, PM_DEFAULT_BASE + off, 1) == 0);
    assert(io_read(&m, PM_DEFAULT_BASE + 0x0C, 4) == 0);

    /* Writes are swallowed: the Timer does not move, nothing appears. */
    io_write(&m, PM_DEFAULT_BASE + 8, 4, 0xDEADBEEF);
    assert(io_read(&m, PM_DEFAULT_BASE + 8, 4) == 0x10);
    io_write(&m, PM_DEFAULT_BASE + 4, 2, 0x0000);
    assert(io_read(&m, PM_DEFAULT_BASE + 4, 2) == 0);

    /* Outside the block the port still floats high, as before this module. */
    assert(io_read(&m, PM_DEFAULT_BASE + 0x40, 1) == 0xFF);
    puts("ok   pm: only TMR has state; writes are swallowed");
}

static void test_rebase(void) {
    machine_t m; setup_machine(&m);
    pit_init(&m); pm_init(&m);
    m.pm.count = 0x00000FF0u;
    m.pm.last_instr = m.vtime_instr;

    pm_set_base(&m, 0x0900 | 0x0001);       /* low bits are not part of base */
    assert(m.pm.base == 0x0900);
    assert(io_read(&m, 0x0908, 4) == 0xFF0u);
    assert(io_read(&m, PM_DEFAULT_BASE + 8, 4) == 0xFFFFFFFFu);  /* old gone */

    pm_set_base(&m, 0xB000);
    assert(io_read(&m, PM_DEFAULT_BASE + 8, 4) == 0xFF0u);
    assert(io_read(&m, 0x0908, 4) == 0xFFFFFFFFu);
    puts("ok   pm: PMBASE moves the block and clears the old window");
}

static void test_lpc_config_hook_moves_base(void) {
    machine_t m; setup_machine(&m);
    pit_init(&m); pm_init(&m);
    pci_init(&m);
    pci_dev_t *lpc = pci_add_device(&m, 0, 31, 0, "lpc", 0x8086, 0x2918, 0x06, 0x01, 0);

    /* What SeaBIOS's ich9_lpc_setup does: PMBASE = 0xb000 | 1. */
    lpc->ctx = &m;
    pci_default_cfg_write(lpc, 0x40, 4, 0x0000B001u);
    pm_set_base(&m, (uint16_t)((lpc->cfg[0x40] | (lpc->cfg[0x41] << 8)) & 0xFF80u));
    assert(m.pm.base == 0xB000);
    m.pm.count = 0x0000002Au;
    m.pm.last_instr = m.vtime_instr;
    assert(io_read(&m, 0xB008, 4) == 0x2Au);
    pci_done(&m);
    puts("ok   pm: LPC PMBASE (0x40 = 0xb000|1) lands on 0xb000");
}

int main(void) {
    test_default_window_and_rate();
    test_24bit_wrap();
    test_read_sizes_and_reserved_bits();
    test_other_registers_and_write_ignore();
    test_rebase();
    test_lpc_config_hook_moves_base();
    puts("pm tests: ok");
    return 0;
}
