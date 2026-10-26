/* d2-differential/emu.c -- the emulator half of the D2 differential.
 *
 * Runs exactly the cases ref32.S measured on the host (i386 compatibility
 * mode executes these opcodes natively) through the emulated core in real
 * mode and prints the same line format, so `diff` between the two outputs is
 * the whole verification.
 */
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include "machine.h"
#include "platform.h"
#include "harness.h"

typedef struct { uint32_t eax_in, flags_in, eax_out, flags_out;
                 uint16_t dst_in, src_in, dst_out; uint32_t zf_out; } st_t;

#define MASK_ADJ (0x001u | 0x004u | 0x010u | 0x040u | 0x080u)
#define CODE 0x1000ULL
#define BUF  0x2000ULL

static void one(machine_t *m, const char *nm, const uint8_t *code, int n,
                uint8_t al, uint8_t ah, int cf, int af) {
    setup_machine(m);
    memcpy(m->ram + CODE, code, (size_t)n);
    m->ram[CODE + n] = 0xF4;                       /* HLT */
    m->cpu.rip = CODE;
    m->cpu.gpr[RAX] = 0x5A5A0000ull | ((uint64_t)ah << 8) | al;
    m->cpu.rflags = 0x202ull | (cf ? 1u : 0u) | (af ? 0x10u : 0u);
    cpu_step(&m->cpu);
    if (m->cpu.fault) {
        printf("%-8s al=%02x ah=%02x cf=%d af=%d -> FAULT %s\n",
               nm, al, ah, cf, af, m->cpu.fault_msg);
        return;
    }
    printf("%-8s al=%02x ah=%02x cf=%d af=%d -> al=%02x ah=%02x f=%02x\n",
           nm, al, ah, cf, af,
           (unsigned)(m->cpu.gpr[RAX] & 0xFF),
           (unsigned)((m->cpu.gpr[RAX] >> 8) & 0xFF),
           (unsigned)(m->cpu.rflags & MASK_ADJ));
}

int main(void) {
    static const struct { const char *nm; uint8_t c[3]; int n; } OPS[] = {
        { "daa",   { 0x27, 0, 0 },          1 },
        { "das",   { 0x2F, 0, 0 },          1 },
        { "aaa",   { 0x37, 0, 0 },          1 },
        { "aas",   { 0x3F, 0, 0 },          1 },
        { "aam10", { 0xD4, 0x0A, 0 },       2 },
        { "aam16", { 0xD4, 0x10, 0 },       2 },
        { "aad10", { 0xD5, 0x0A, 0 },       2 },
        { "aad16", { 0xD5, 0x10, 0 },       2 },
    };
    machine_t m;
    for (size_t f = 0; f < sizeof OPS / sizeof *OPS; f++)
        for (int cf = 0; cf < 2; cf++)
            for (int af = 0; af < 2; af++)
                for (int al = 0; al < 256; al++)
                    one(&m, OPS[f].nm, OPS[f].c, OPS[f].n,
                        (uint8_t)al, 0x5A, cf, af);

    /* ARPL: ARPL %src, mem16 -> opcode 63 /r with a memory destination in
     * real mode, so SS:BX style addressing reaches physical BUF. */
    for (int d = 0; d < 4; d++)
        for (int s = 0; s < 4; s++) {
            setup_machine(&m);
            /* arpl word [0x2000], dx */
            const uint8_t code[] = { 0x63, 0x16, 0x00, 0x20, 0xF4 };
            memcpy(m.ram + CODE, code, sizeof code);
            m.ram[CODE + sizeof code] = 0xF4;
            mem_write(&m, BUF, 2, 0xFFFCu | (unsigned)d);
            m.cpu.gpr[RDX] = (uint64_t)(unsigned)s;
            m.cpu.rip = CODE;
            cpu_step(&m.cpu);
            uint16_t out = (uint16_t)mem_read(&m, BUF, 2);
            printf("%-8s dst=%04x src=%04x -> dst=%04x zf=%u\n",
                   "arpl", (unsigned)(0xFFFCu | (unsigned)d), (unsigned)s,
                   out, (unsigned)((m.cpu.rflags >> 6) & 1));
        }
    return 0;
}
