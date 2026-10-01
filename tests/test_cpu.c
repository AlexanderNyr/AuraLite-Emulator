#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "machine.h"
#include "platform.h"

static void setup_machine(machine_t *m) {
    memset(m, 0, sizeof *m);
    m->cpu.mach = m;
    mem_init(m, NULL, 0);
    io_init(m);
    m->plat = (struct platform *)platform_by_name("haswell");
    cpu_reset(&m->cpu);

    /* Start a tiny test program directly at physical address zero. */
    m->cpu.cr0 = 0x60000010ULL; /* real mode, paging disabled */
    m->cpu.seg[SEG_CS].base = 0;
    m->cpu.seg[SEG_CS].limit = 0xFFFF;
    m->cpu.seg[SEG_SS].base = 0;
    m->cpu.seg[SEG_SS].limit = 0xFFFF;
    m->cpu.rip = 0;
}

static void test_mov_add_hlt(void) {
    machine_t m;
    setup_machine(&m);

    /* mov ax,123; add ax,10; hlt */
    const uint8_t program[] = {
        0xB8, 0x7B, 0x00,
        0x05, 0x0A, 0x00,
        0xF4
    };
    memcpy(m.ram, program, sizeof program);

    assert(cpu_step(&m.cpu) == 0);
    assert((m.cpu.gpr[RAX] & 0xFFFF) == 123);
    assert(cpu_step(&m.cpu) == 0);
    assert((m.cpu.gpr[RAX] & 0xFFFF) == 133);
    assert(cpu_step(&m.cpu) != 0);
    assert(m.cpu.halted);
    assert(!m.cpu.fault);
}

static void test_memory_round_trip(void) {
    machine_t m;
    setup_machine(&m);

    mem_write(&m, 0x1234, 4, 0xA1B2C3D4);
    assert(mem_read(&m, 0x1234, 4) == 0xA1B2C3D4);
}

static void test_platform_cpuid_profile(void) {
    const platform_t *p = platform_by_name("haswell");
    uint32_t a, b, c, d;
    platform_cpuid(p, 1, 0, &a, &b, &c, &d);
    assert(a == 0x000306C3u);
}

int main(void) {
    test_mov_add_hlt();
    test_memory_round_trip();
    test_platform_cpuid_profile();
    puts("unit tests: ok");
    return 0;
}
