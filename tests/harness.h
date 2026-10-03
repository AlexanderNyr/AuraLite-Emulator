/* tests/harness.h -- shared flat-machine harness for the ISA vector suites.
 *
 * Both test_cpu.c (hand-rolled groups, phases C1-C9) and test_table.c
 * (table-driven vectors, phase C10) drive the same minimal machine: real
 * mode, identity segments, RSP=0x8000, program loaded at physical 0.
 * Keeping this in one header means a harness fix lands in both suites.
 */
#ifndef TESTS_HARNESS_H
#define TESTS_HARNESS_H

#include <stdlib.h>
#include <string.h>
#include "machine.h"
#include "platform.h"
#include "chipset.h"

/* The ASan CI lane (make test-sanitize) requires leak-free exits. A full
 * machine_t takes 128MB of RAM + 128KB of ROM from mem_init; allocating
 * that per vector is what LeakSanitizer measured at ~6GB across a suite
 * run. The harness therefore shares ONE RAM/ROM pair per test process and
 * frees it at exit. Test code only ever touches low memory (vectors live
 * below 0x5000, the stack at 0x8000), so clearing the low 1MB per setup is
 * enough isolation. */
static uint8_t *HARNESS_RAM, *HARNESS_ROM;

static void harness_shutdown(void) {
    free(HARNESS_RAM); HARNESS_RAM = NULL;
    free(HARNESS_ROM); HARNESS_ROM = NULL;
}

static void setup_machine(machine_t *m) {
    if (!HARNESS_RAM) {
        HARNESS_RAM = calloc(1, RAM_SIZE);
        HARNESS_ROM = calloc(1, ROM_SIZE);
        atexit(harness_shutdown);
    }
    memset(m, 0, sizeof *m);
    m->cpu.mach = m;
    m->ram = HARNESS_RAM;
    m->rom = HARNESS_ROM;
    m->rom_len = ROM_SIZE;
    m->mmio_list = NULL;
    memset(HARNESS_RAM, 0, 1u * 1024 * 1024);
    io_init(m);
    m->plat = (struct platform *)platform_by_name("haswell");
    chipset_init(m);   /* H5: power-on defaults -- A20 gate open */
    cpu_reset(&m->cpu);

    /* Start a tiny test program directly at physical address zero. */
    m->cpu.cr0 = 0x60000010ULL; /* real mode, paging disabled */
    /* Identity real-mode segments and a valid stack: the interrupt vectors
     * below push/pop CS:IP and FLAGS, so the selector and RSP must be sane. */
    m->cpu.seg[SEG_CS].sel = 0;
    m->cpu.seg[SEG_CS].base = 0;
    m->cpu.seg[SEG_CS].limit = 0xFFFF;
    m->cpu.seg[SEG_SS].sel = 0;
    m->cpu.seg[SEG_SS].base = 0;
    m->cpu.seg[SEG_SS].limit = 0xFFFF;
    m->cpu.rip = 0;
    m->cpu.gpr[RSP] = 0x8000;
}

/* Load a program at `rip` and run it to completion (HLT/fault) or step limit.
 * (unused in diff_fuzz.c, which steps manually -- hence __attribute__((unused))) */
__attribute__((unused))
static void run_program(machine_t *m, const uint8_t *program, size_t len,
                        uint64_t rip, int max_steps) {
    memcpy(m->ram + rip, program, len);
    m->cpu.rip = rip;
    for (int i = 0; i < max_steps; i++)
        if (cpu_step(&m->cpu) != 0)
            break;
}

#endif
