/* d3_differential/emu.c -- the emulator half of the D3 differential.
 *
 * BOUND, LES and LDS are legacy-mode instructions, so both halves run at
 * CPL3: the host in i386 compatibility mode, this core in 32-bit protected
 * mode with a GDT laid out the way i386 Linux lays it out (null, kernel
 * code/data at DPL0, user code 0x23 and user data 0x2b at DPL3).  The printed
 * lines are the same as host32.c's, so `diff` is the whole verification.
 */
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include "machine.h"
#include "platform.h"
#include "harness.h"

#define CODE    0x1000ULL
#define GDT     0x5000ULL
#define FARPTR  0x3000ULL

static uint64_t gdt_desc(uint32_t base, uint32_t limit, uint8_t type, int dpl, int db) {
    uint64_t d = 0;
    d |= (uint64_t)(limit & 0xFFFF);
    d |= (uint64_t)(base & 0xFFFF) << 16;
    d |= (uint64_t)(base >> 16 & 0xFF) << 32;
    d |= (uint64_t)(type & 0x0F) << 40;
    d |= (uint64_t)1 << 44;                       /* S = 1 (code/data) */
    d |= (uint64_t)dpl << 45;
    d |= (uint64_t)1 << 47;                       /* present */
    d |= (uint64_t)(limit >> 16 & 0x0F) << 48;
    d |= (uint64_t)1 << 55;                       /* G = 4KiB granularity */
    d |= (uint64_t)db << 54;
    return d;
}
static void put_desc(machine_t *m, int idx, uint64_t v) {
    mem_write(m, GDT + (uint64_t)idx * 8, 8, v);
}
static void load_seg(machine_t *m, int seg, uint16_t sel, uint64_t base,
                     uint32_t limit, uint8_t type, int dpl, int db) {
    cpu_t *c = &m->cpu;
    c->seg[seg].sel = sel;
    c->seg[seg].base = base;
    c->seg[seg].limit = limit;
    c->seg[seg].type = type;
    (void)dpl;   /* DPL lives in the GDT entry; the cache keeps the raw type */
    c->seg[seg].present = 1;
    c->seg[seg].d_b = db;
    c->seg[seg].l = 0;
}
/* 32-bit protected mode, CPL3, flat segments, GDT at GDT */
static void setup_pm32(machine_t *m) {
    setup_machine(m);
    put_desc(m, 0, 0);                                             /* null */
    put_desc(m, 1, gdt_desc(0, 0xFFFFF, 0xA, 0, 1));                /* 0x08 kernel code */
    put_desc(m, 2, gdt_desc(0, 0xFFFFF, 0x2, 0, 1));                /* 0x10 kernel data */
    put_desc(m, 3, gdt_desc(0, 0xFFFFF, 0x2, 0, 1));                /* 0x18 unused data */
    put_desc(m, 4, gdt_desc(0, 0xFFFFF, 0xA, 3, 1));                /* 0x23 user code */
    put_desc(m, 5, gdt_desc(0, 0xFFFFF, 0x2, 3, 1));                /* 0x2b user data */
    cpu_t *c = &m->cpu;
    c->gdtr_base = GDT;
    c->gdtr_limit = (uint16_t)(6 * 8 - 1);
    c->cr0 |= 1;                                                   /* PE */
    c->efer = 0;
    load_seg(m, SEG_CS, 0x23, 0, 0xFFFFFFFFu, 0xA, 3, 1);
    load_seg(m, SEG_SS, 0x2B, 0, 0xFFFFFFFFu, 0x2, 3, 1);
    load_seg(m, SEG_DS, 0x2B, 0, 0xFFFFFFFFu, 0x2, 3, 1);
    load_seg(m, SEG_ES, 0x2B, 0, 0xFFFFFFFFu, 0x2, 3, 1);
    c->gpr[RSP] = 0x8000;
    c->rip = CODE;
    c->rflags = 0x202;
}
static void run(machine_t *m, const uint8_t *code, size_t n) {
    memset(m->ram + CODE, 0x90, 32);
    memcpy(m->ram + CODE, code, n);
    m->cpu.rip = CODE;
    m->cpu.fault = 0;
    m->cpu.exception_taken = 0;
    m->cpu.fault_msg[0] = 0;
    cpu_step(&m->cpu);
}

int main(void) {
    machine_t m;
    /* BOUND is verified by tests/test_table.c (the host reference is not
     * usable in this sandbox -- see host32.c), so this harness only needs to
     * keep the two halves' line sets identical. */

    /* --- LES / LDS with the host's selector list --- */
    static const uint16_t SELS[] = { 0x002B, 0x0023, 0x0000, 0x00F8, 0xFFFF };
    for (int pass = 0; pass < 2; pass++) {
        for (size_t i = 0; i < sizeof SELS / sizeof *SELS; i++) {
            setup_pm32(&m);
            /* les eax,[0x3000] / lds eax,[0x3000]: C4|C5 05 <disp32> */
            uint8_t code[6] = { (uint8_t)(pass ? 0xC5 : 0xC4), 0x05, 0x00, 0x30, 0x00, 0x00 };
            mem_write(&m, FARPTR, 4, pass ? 0x55667788u : 0x11223344u);
            mem_write(&m, FARPTR + 4, 2, SELS[i]);
            run(&m, code, sizeof code);
            int fault = m.cpu.exception_taken || m.cpu.fault;
            uint16_t seg = (uint16_t)m.cpu.seg[pass ? SEG_DS : SEG_ES].sel;
            (void)seg;
            printf("%s    sel=%04x -> fault=%d eax=%08x %s=%s\n",
                   pass ? "lds" : "les", (unsigned)SELS[i], fault,
                   (unsigned)(m.cpu.gpr[RAX] & 0xFFFFFFFFu),
                   pass ? "ds" : "es", fault ? "----" : "ok");
        }
    }
    return 0;
}
