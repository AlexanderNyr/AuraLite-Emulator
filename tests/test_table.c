/* tests/test_table.c -- table-driven ISA vector suite (phase C10).
 *
 * Every vector is one row: human-readable name, the exact machine-code
 * bytes as a hex string, a comma-separated machine SETUP, and the list of
 * architectural facts that must hold afterwards. The runner below parses
 * the DSL so the table itself contains zero logic -- new vectors cost one
 * line and no C.
 *
 * DSL keys:
 *   registers: al..bh (8-bit incl. high), ax..di (16), eax..edi, rax..rdi
 *   flags:     cf pf af zf sf df if of   (0/1)
 *   segments:  cs ds es ss               (selector; setup also sets base)
 *   memory:    mb[addr] mw[addr] md[addr] mq[addr]
 *   expect-only: fault=1                 (an architectural fault must fire)
 */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "machine.h"
#include "platform.h"
#include "harness.h"

/* ------------------------------------------------------------------ DSL -- */

typedef struct { const char *n; int idx, w; } regdesc_t;
static const regdesc_t REGS[] = {
    {"al",0,1},{"cl",1,1},{"dl",2,1},{"bl",3,1},
    {"ah",4,1},{"ch",5,1},{"dh",6,1},{"bh",7,1},
    {"ax",0,2},{"cx",1,2},{"dx",2,2},{"bx",3,2},
    {"sp",4,2},{"bp",5,2},{"si",6,2},{"di",7,2},
    {"eax",0,4},{"ecx",1,4},{"edx",2,4},{"ebx",3,4},
    {"esp",4,4},{"ebp",5,4},{"esi",6,4},{"edi",7,4},
    {"rax",0,8},{"rcx",1,8},{"rdx",2,8},{"rbx",3,8},
    {"rsp",4,8},{"rbp",5,8},{"rsi",6,8},{"rdi",7,8},
};
static const struct { const char *n; uint64_t bit; } FLAGS[] = {
    {"cf",FLAG_CF},{"pf",FLAG_PF},{"af",FLAG_AF},{"zf",FLAG_ZF},
    {"sf",FLAG_SF},{"df",FLAG_DF},{"if",FLAG_IF},{"of",FLAG_OF},
};
static const struct { const char *n; int seg; } SEGS[] = {
    {"cs",SEG_CS},{"ds",SEG_DS},{"es",SEG_ES},{"ss",SEG_SS},
};

static uint64_t rmask(int w) { return w==8 ? ~0ull : ((1ull << (8*w)) - 1); }

static uint64_t read_reg(const cpu_t *c, int idx, int w) {
    if (w == 1 && idx >= 4) return (c->gpr[idx-4] >> 8) & 0xFF;
    return c->gpr[idx] & rmask(w);
}
static void write_reg(cpu_t *c, int idx, int w, uint64_t v) {
    if (w == 1 && idx >= 4) { c->gpr[idx-4] = (c->gpr[idx-4] & ~0xFF00ull) | ((v & 0xFF) << 8); return; }
    if (w == 1) { c->gpr[idx] = (c->gpr[idx] & ~0xFFull) | (v & 0xFF); return; }
    if (w == 2) { c->gpr[idx] = (c->gpr[idx] & ~0xFFFFull) | (v & 0xFFFF); return; }
    if (w == 4) { c->gpr[idx] = (uint32_t)v; return; }
    c->gpr[idx] = v;
}

/* Apply a single "key=value" to setup (setup=1) or check it (setup=0).
 * Returns 1 if this was the "fault" marker. */
static int one_kv(machine_t *m, const char *kv, int setup, const char *vname) {
    char key[96]; unsigned long long val;
    const char *eq = strchr(kv, '=');
    assert(eq && "vector entry must be key=value");
    size_t klen = (size_t)(eq - kv);
    assert(klen < sizeof key);
    memcpy(key, kv, klen); key[klen] = 0;
    while (klen && key[klen-1] == ' ') key[--klen] = 0;
    val = strtoull(eq + 1, NULL, 0);

    if (!strcmp(key, "fault")) {
        if (!setup) {
            if (!(m->cpu.fault != 0))
                fprintf(stderr, "vector %s: expected a fault, none fired\n", vname);
            assert(m->cpu.fault != 0);
        }
        return 1;
    }
    if (key[0] == 'm' && key[2] == '[') { /* memory mb/mw/md/mq[addr] */
        int w = key[1]=='b' ? 1 : key[1]=='w' ? 2 : key[1]=='d' ? 4 : 8;
        uint64_t addr = strtoull(key + 3, NULL, 0);
        if (setup) mem_write(m, addr, w, val);
        else {
            uint64_t got = (uint64_t)mem_read(m, addr, w) & rmask(w);
            if (got != (val & rmask(w)))
                fprintf(stderr, "vector %s: %s = %#llx, want %#llx\n",
                        vname, key, (unsigned long long)got,
                        (unsigned long long)(val & rmask(w)));
            assert(got == (val & rmask(w)));
        }
        return 0;
    }
    for (size_t i = 0; i < sizeof FLAGS/sizeof *FLAGS; i++)
        if (!strcmp(key, FLAGS[i].n)) {
            if (setup) {
                if (val) m->cpu.rflags |=  FLAGS[i].bit;
                else     m->cpu.rflags &= ~FLAGS[i].bit;
            } else {
                uint64_t got = (m->cpu.rflags & FLAGS[i].bit) != 0;
                if (got != !!val)
                    fprintf(stderr, "vector %s: %s = %llu, want %llu\n",
                            vname, key, (unsigned long long)got,
                            (unsigned long long)val);
                assert(got == !!val);
            }
            return 0;
        }
    for (size_t i = 0; i < sizeof SEGS/sizeof *SEGS; i++)
        if (!strcmp(key, SEGS[i].n)) {
            int s = SEGS[i].seg;
            if (setup) {
                m->cpu.seg[s].sel = (uint16_t)val;
                m->cpu.seg[s].base = (uint64_t)val << 4;
                m->cpu.seg[s].limit = 0xFFFF;
            } else {
                if (m->cpu.seg[s].sel != (uint16_t)val)
                    fprintf(stderr, "vector %s: %s.sel = %#x, want %#llx\n",
                            vname, key, m->cpu.seg[s].sel, val);
                assert(m->cpu.seg[s].sel == (uint16_t)val);
            }
            return 0;
        }
    for (size_t i = 0; i < sizeof REGS/sizeof *REGS; i++)
        if (!strcmp(key, REGS[i].n)) {
            if (setup) write_reg(&m->cpu, REGS[i].idx, REGS[i].w, val);
            else {
                uint64_t got = read_reg(&m->cpu, REGS[i].idx, REGS[i].w);
                if (got != (val & rmask(REGS[i].w)))
                    fprintf(stderr, "vector %s: %s = %#llx, want %#llx\n",
                            vname, key, (unsigned long long)got,
                            (unsigned long long)(val & rmask(REGS[i].w)));
                assert(got == (val & rmask(REGS[i].w)));
            }
            return 0;
        }
    fprintf(stderr, "vector %s: unknown key '%s'\n", vname, key);
    assert(!"unknown DSL key");
    return 0;
}

static void apply_list(machine_t *m, const char *list, int setup,
                       const char *vname, int *want_fault) {
    if (!list || !*list) return;
    char buf[512];
    assert(strlen(list) < sizeof buf);
    strcpy(buf, list);
    for (char *tok = strtok(buf, ","); tok; tok = strtok(NULL, ",")) {
        while (*tok == ' ') tok++;
        if (one_kv(m, tok, setup, vname) && want_fault) *want_fault = 1;
    }
}

static int hex_bytes(const char *hex, uint8_t *out, int max) {
    /* tokens are 2 hex chars, space separated */
    int n = 0; const char *p = hex;
    while (*p) {
        while (*p == ' ') p++;
        if (!*p) break;
        unsigned v; int used = 0;
        if (sscanf(p, "%2x%n", &v, &used) != 1) break;
        assert(n < max);
        out[n++] = (uint8_t)v;
        p += used;
    }
    return n;
}

/* -------------------------------------------------------------- vectors -- */

typedef struct { const char *name, *hex, *setup, *expect; } vec_t;
#define V(n,h,s,e) {n,h,s,e}

static const vec_t VECTORS[] = {
    /* -- A: 0x83 group, AX --------------------------------------------- */
    V("add ax,0x7F into sign",   "83 C0 7F", "ax=0x7F81", "ax=0x8000,of=1,sf=1,af=1,pf=1,cf=0"),
    V("add ax,-1 wraps to zero", "83 C0 FF", "ax=1",      "ax=0,zf=1,cf=1,af=1,pf=1,of=0"),
    V("add ax,1 overflows 16b",  "83 C0 01", "ax=0xFFFF", "ax=0,zf=1,cf=1,af=1,of=0"),
    V("add ax,5 near top",       "83 C0 05", "ax=0xFFFB", "ax=0,zf=1,cf=1"),
    V("add ax,0x10 signed ovf",  "83 C0 10", "ax=0x7FF0", "ax=0x8000,of=1,sf=1,cf=0,af=0"),
    V("or ax,0x0F low bits",     "83 C8 0F", "ax=0x00F0", "ax=0x00FF,zf=0,sf=0,cf=0,of=0"),
    V("or ax,-16 sign-ext imm",  "83 C8 F0", "ax=0x0F0F", "ax=0xFFFF,sf=1,cf=0,of=0"),
    V("or ax,1 keeps sign",      "83 C8 01", "ax=0x8000", "ax=0x8001,sf=1"),
    V("or ax,0 sets zero",       "83 C8 00", "ax=0",      "ax=0,zf=1"),
    V("or ax,0x7F merge",        "83 C8 7F", "ax=0x1234", "ax=0x127F"),
    V("adc ax,1 carry-in wrap",  "83 D0 01", "ax=0xFFFF,cf=1", "ax=1,cf=1,zf=0"),
    V("adc ax,0 carry-in",       "83 D0 00", "ax=0x000E,cf=1", "ax=0x000F,cf=0,af=0"),
    V("adc ax,-1 carry-in",      "83 D0 FF", "ax=0,cf=1", "ax=0,cf=1,zf=1"),
    V("adc ax,1 signed ovf",     "83 D0 01", "ax=0x7FFF,cf=1", "ax=0x8001,of=1,sf=1"),
    V("adc ax,0x10 plain",       "83 D0 10", "ax=0x0F00,cf=0", "ax=0x0F10,cf=0,of=0"),
    V("sbb ax,1 no borrow",      "83 D8 01", "ax=5,cf=0", "ax=4,cf=0"),
    V("sbb ax,1 with borrow",    "83 D8 01", "ax=5,cf=1", "ax=3,cf=0"),
    V("sbb ax,1 from zero",      "83 D8 01", "ax=0,cf=0", "ax=0xFFFF,cf=1,sf=1,af=1,pf=1"),
    V("sbb ax,-1",               "83 D8 FF", "ax=0x8000,cf=0", "ax=0x8001,cf=1,of=0"),
    V("sbb ax,1 signed ovf",     "83 D8 01", "ax=0x8000,cf=1", "ax=0x7FFE,of=1,sf=0"),
    V("and ax,0x0F zero result", "83 E0 0F", "ax=0xF0F0", "ax=0,zf=1,cf=0,of=0,pf=1"),
    V("and ax,-1 keep",          "83 E0 FF", "ax=0x00FF", "ax=0x00FF,sf=0"),
    V("and ax,0x0F low",         "83 E0 0F", "ax=0xFFFF", "ax=0x000F"),
    V("and ax,0x55 pattern",     "83 E0 55", "ax=0xA5A5", "ax=5"),
    V("and ax,0x80 sign-ext",    "83 E0 80", "ax=0xFFFF", "ax=0xFF80,sf=1,pf=0"),
    V("sub ax,1 borrow",         "83 E8 01", "ax=0",      "ax=0xFFFF,cf=1,sf=1"),
    V("sub ax,1 signed ovf",     "83 E8 01", "ax=0x8000", "ax=0x7FFF,of=1,sf=0,af=1,pf=1"),
    V("sub ax,-1 of-carry",      "83 E8 FF", "ax=0x7FFF", "ax=0x8000,of=1,sf=1,cf=1"),
    V("sub ax,0x0F af",          "83 E8 0F", "ax=0x0F00", "ax=0x0EF1,af=1,pf=0"),
    V("sub ax,0 plain",          "83 E8 00", "ax=0x1234", "ax=0x1234,zf=0,cf=0"),
    V("xor ax,-1",               "83 F0 FF", "ax=0x00FF", "ax=0xFF00,sf=1,pf=1"),
    V("xor ax,-1 to zero",       "83 F0 FF", "ax=0xFFFF", "ax=0,zf=1,pf=1"),
    V("xor same bit",            "83 F0 01", "ax=1",      "ax=0,zf=1"),
    V("xor ax,0x55",             "83 F0 55", "ax=0xAAAA", "ax=0xAAFF"),
    V("xor ax,0x0F",             "83 F0 0F", "ax=0xF0F0", "ax=0xF0FF"),
    V("cmp ax,0 equal",          "83 F8 00", "ax=0",      "ax=0,zf=1"),
    V("cmp ax,1 above",          "83 F8 01", "ax=5",      "ax=5,cf=0"),
    V("cmp ax,5 equal",          "83 F8 05", "ax=5",      "ax=5,zf=1,cf=0"),
    V("cmp ax,6 below",          "83 F8 06", "ax=5",      "ax=5,cf=1"),
    V("cmp ax,-1 borrow",        "83 F8 FF", "ax=0x8000", "ax=0x8000,cf=1,sf=1"),

    /* -- B: 0x83 group, memory form ------------------------------------- */
    V("add word[2000],1",        "83 06 00 20 01", "mw[0x2000]=0xFFFF", "mw[0x2000]=0,cf=1,zf=1"),
    V("sub word[2000],2",        "83 2E 00 20 02", "mw[0x2000]=1",      "mw[0x2000]=0xFFFF,cf=1,sf=1"),
    V("and word[2000],0x0F",     "83 26 00 20 0F", "mw[0x2000]=0x00FF", "mw[0x2000]=0x000F"),
    V("or word[2000],0x7F",      "83 0E 00 20 7F", "mw[0x2000]=0",      "mw[0x2000]=0x007F"),
    V("xor word[2000],1",        "83 36 00 20 01", "mw[0x2000]=1",      "mw[0x2000]=0,zf=1"),
    V("cmp word[2000],0x10",     "83 3E 00 20 10", "mw[0x2000]=0x0010", "mw[0x2000]=0x0010,zf=1"),

    /* -- C: 0x80 group, 8-bit ------------------------------------------- */
    V("add al,1 signed ovf",     "80 C0 01", "al=0x7F", "al=0x80,of=1,sf=1,af=1,pf=0"),
    V("or al,0x80",              "80 C8 80", "al=0",    "al=0x80,sf=1,cf=0,of=0,pf=0"),
    V("adc cl,-1",               "80 D1 FF", "cl=1,cf=0", "cl=0,cf=1,zf=1,pf=1"),
    V("sbb dl,1 borrow-in",      "80 DA 01", "dl=0,cf=1", "dl=0xFE,cf=1,sf=1"),
    V("and bl,0xF0",             "80 E3 F0", "bl=0xFF", "bl=0xF0,sf=1,pf=1"),
    V("sub ah,0x0F",             "80 EC 0F", "ah=0x10", "ah=1,af=1"),
    V("xor bh,-1",               "80 F7 FF", "bh=0xFF", "bh=0,zf=1"),
    V("cmp al,0x7F equal",       "80 F8 7F", "al=0x7F", "al=0x7F,zf=1"),
    V("cmp dh,1 below",          "80 FE 01", "dh=0",    "dh=0,cf=1"),
    V("sub bl,-1",               "80 EB FF", "bl=0x80", "bl=0x81,cf=1,sf=1,af=1,of=0"),
    V("or dl,0 zero",            "80 CA 00", "dl=0",    "dl=0,zf=1"),
    V("adc ah,0 carry-in",       "80 D4 00", "ah=0xFF,cf=1", "ah=0,cf=1,zf=1,af=1"),
    V("sbb al,0 borrow-in",      "80 D8 00", "al=0,cf=1", "al=0xFF,cf=1,sf=1"),
    V("xor al,0x55",             "80 F0 55", "al=0xAA", "al=0xFF"),
    V("cmp bl,-1 equal",         "80 FB FF", "bl=0xFF", "zf=1"),

    /* -- D: shifts/rotates ---------------------------------------------- */
    V("shl ax,1 into sign",      "D1 E0", "ax=0x4000", "ax=0x8000,cf=0,of=1,sf=1"),
    V("shl ax,1 out top bit",    "D1 E0", "ax=0x8000", "ax=0,cf=1,of=1,zf=1"),
    V("shl bx,3",                "C1 E3 03", "bx=0x9111", "bx=0x8888,cf=0,sf=1,pf=1"),
    V("shl cx,8",                "C1 E1 08", "cx=0x00FF", "cx=0xFF00,cf=0,sf=1,pf=1"),
    V("shl ax,count33 masks",    "C1 E0 21", "ax=0x4000", "ax=0x8000,sf=1"),
    V("shr ax,1 of=msb",         "D1 E8", "ax=0x8000", "ax=0x4000,cf=0,of=1"),
    V("shr ax,1 out low bit",    "D1 E8", "ax=1",      "ax=0,cf=1,zf=1,of=0"),
    V("shr bx,4",                "C1 EB 04", "bx=0x00F8", "bx=0x000F,cf=1"),
    V("shr dx,3",                "C1 EA 03", "dx=0x00E7", "dx=0x001C,cf=1"),
    V("sar ax,1 keeps sign",     "D1 F8", "ax=0x8000", "ax=0xC000,cf=0,sf=1,of=0"),
    V("sar cx,15 all sign",      "C1 F9 0F", "cx=0xF000", "cx=0xFFFF,cf=1,sf=1"),
    V("sar dx,4 low out",        "C1 FA 04", "dx=0x800E", "dx=0xF800,cf=1"),
    V("rol ax,1 around",         "D1 C0", "ax=0x8001", "ax=3,cf=1,of=1"),
    V("rol bx,4",                "C1 C3 04", "bx=0x1234", "bx=0x2341,cf=1"),
    V("rol cl,4 8-bit",          "C0 C1 04", "cl=0x3C", "cl=0xC3,cf=1"),
    V("ror ax,1",                "D1 C8", "ax=3",      "ax=0x8001,cf=1,of=1"),
    V("ror bx,4",                "C1 CB 04", "bx=0xABCD", "bx=0xDABC,cf=1"),
    V("ror dh,1 8-bit",          "D0 CE", "dh=0x81", "dh=0xC0,cf=1,of=0"),
    V("rcl ax,1 cf0",            "D1 D0", "ax=0x8000,cf=0", "ax=0,cf=1,of=1"),
    V("rcl ax,1 cf1",            "D1 D0", "ax=0x8000,cf=1", "ax=1,cf=1"),
    V("rcl bx,4 through cf",     "C1 D3 04", "bx=0x1234,cf=1", "bx=0x2348,cf=1"),
    V("rcr ax,1 cf-in to top",   "D1 D8", "ax=1,cf=1", "ax=0x8000,cf=1,of=1"),
    V("rcr bx,4",                "C1 DB 04", "bx=0x2346,cf=0", "bx=0xC234,cf=0"),
    V("shl dx,cl=4",             "D3 E2", "cl=4,dx=0x00F0", "dx=0x0F00,cf=0"),
    V("sar bl,2 8-bit",          "C0 FB 02", "bl=0xF4", "bl=0xFD,cf=0,sf=1"),
    V("shr ah,3 8-bit",          "C0 EC 03", "ah=0xAF", "ah=0x15,cf=1"),
    V("shift count0 keeps cf",   "D3 E2", "cl=0,dx=5,cf=1", "dx=5,cf=1"),
    V("sar ax,count33 masked",   "D3 F8", "cl=33,ax=0x8002", "ax=0xC001"),
    V("shl al,1 8-bit",          "D0 E0", "al=0x80", "al=0,cf=1,zf=1,of=1"),
    V("shr al,1 8-bit",          "D0 E8", "al=1", "al=0,cf=1,zf=1,of=0"),
    V("rol al,1 8-bit",          "D0 C0", "al=0x81", "al=3,cf=1,of=1"),
    V("ror al,1 8-bit",          "D0 C8", "al=1", "al=0x80,cf=1,of=1"),
    V("rcl al,1 8-bit",          "D0 D0", "al=0x80,cf=1", "al=1,cf=1"),
    V("rcr al,1 8-bit",          "D0 D8", "al=0,cf=1", "al=0x80,cf=0,of=1"),

    /* -- E: Jcc short + near (taken => ax stays 9; skipped => ax=1) ------ */
    V("jo taken",   "70 03 B8 01 00 F4", "ax=9,of=1", "ax=9"),
    V("jo not",     "70 03 B8 01 00 F4", "ax=9,of=0", "ax=1"),
    V("jb taken",   "72 03 B8 01 00 F4", "ax=9,cf=1", "ax=9"),
    V("jae not",    "73 03 B8 01 00 F4", "ax=9,cf=1", "ax=1"),
    V("je taken",   "74 03 B8 01 00 F4", "ax=9,zf=1", "ax=9"),
    V("jne not",    "75 03 B8 01 00 F4", "ax=9,zf=1", "ax=1"),
    V("jbe taken",  "76 03 B8 01 00 F4", "ax=9,zf=1,cf=0", "ax=9"),
    V("ja not",     "77 03 B8 01 00 F4", "ax=9,zf=1,cf=0", "ax=1"),
    V("js taken",   "78 03 B8 01 00 F4", "ax=9,sf=1", "ax=9"),
    V("jns not",    "79 03 B8 01 00 F4", "ax=9,sf=1", "ax=1"),
    V("jp taken",   "7A 03 B8 01 00 F4", "ax=9,pf=1", "ax=9"),
    V("jnp not",    "7B 03 B8 01 00 F4", "ax=9,pf=1", "ax=1"),
    V("jl taken",   "7C 03 B8 01 00 F4", "ax=9,of=1,sf=0", "ax=9"),
    V("jge not",    "7D 03 B8 01 00 F4", "ax=9,of=1,sf=0", "ax=1"),
    V("jle taken",  "7E 03 B8 01 00 F4", "ax=9,sf=1,of=0", "ax=9"),
    V("jg not",     "7F 03 B8 01 00 F4", "ax=9,sf=1,of=0", "ax=1"),
    V("jo near taken",  "0F 80 03 00 B8 01 00 F4", "ax=9,of=1", "ax=9"),
    V("jno near not",   "0F 81 03 00 B8 01 00 F4", "ax=9,of=1", "ax=1"),
    V("jb near taken",  "0F 82 03 00 B8 01 00 F4", "ax=9,cf=1", "ax=9"),
    V("jae near not",   "0F 83 03 00 B8 01 00 F4", "ax=9,cf=1", "ax=1"),
    V("je near taken",  "0F 84 03 00 B8 01 00 F4", "ax=9,zf=1", "ax=9"),
    V("jne near not",   "0F 85 03 00 B8 01 00 F4", "ax=9,zf=1", "ax=1"),
    V("jbe near taken", "0F 86 03 00 B8 01 00 F4", "ax=9,zf=1,cf=0", "ax=9"),
    V("ja near not",    "0F 87 03 00 B8 01 00 F4", "ax=9,zf=1,cf=0", "ax=1"),
    V("js near taken",  "0F 88 03 00 B8 01 00 F4", "ax=9,sf=1", "ax=9"),
    V("jns near not",   "0F 89 03 00 B8 01 00 F4", "ax=9,sf=1", "ax=1"),
    V("jp near taken",  "0F 8A 03 00 B8 01 00 F4", "ax=9,pf=1", "ax=9"),
    V("jnp near not",   "0F 8B 03 00 B8 01 00 F4", "ax=9,pf=1", "ax=1"),
    V("jl near taken",  "0F 8C 03 00 B8 01 00 F4", "ax=9,of=1,sf=0", "ax=9"),
    V("jge near not",   "0F 8D 03 00 B8 01 00 F4", "ax=9,of=1,sf=0", "ax=1"),
    V("jle near taken", "0F 8E 03 00 B8 01 00 F4", "ax=9,sf=1,of=0", "ax=9"),
    V("jg near not",    "0F 8F 03 00 B8 01 00 F4", "ax=9,sf=1,of=0", "ax=1"),

    /* -- F: LOOP family --------------------------------------------------- */
    V("loop 3x",        "40 E2 FD F4", "ax=0,cx=3", "ax=3,cx=0"),
    V("loop 2x",        "40 E2 FD F4", "ax=0,cx=2", "ax=2,cx=0"),
    V("loope runs out", "E1 FE F4", "cx=3,zf=1", "cx=0"),
    V("loope exits zf0","E1 FE F4", "cx=3,zf=0", "cx=2"),
    V("loopne runs out","E0 FE F4", "cx=2,zf=0", "cx=0"),
    V("loopne exits zf","E0 FE F4", "cx=5,zf=1", "cx=4"),
    V("jcxz taken",     "E3 03 B8 07 00 F4", "ax=0xCAFE,cx=0", "ax=0xCAFE"),
    V("jcxz not taken", "E3 03 B8 07 00 F4", "ax=0xCAFE,cx=1", "ax=7"),

    /* -- G: MOV forms ----------------------------------------------------- */
    V("mov ax,bx",       "89 D8", "bx=7",       "ax=7"),
    V("mov bx,ax",       "89 C3", "ax=0x55",    "bx=0x55"),
    V("mov ax,cx",       "89 C8", "cx=0x1234",  "ax=0x1234"),
    V("mov cx,dx",       "89 D1", "dx=0xBEEF",  "cx=0xBEEF"),
    V("mov al,bl",       "88 D8", "bl=0xA5",    "al=0xA5"),
    V("mov ah,ch",       "88 EC", "ch=0x3C",    "ah=0x3C"),
    V("mov [2000],bx",   "89 1E 00 20", "bx=0xDEAD", "mw[0x2000]=0xDEAD"),
    V("mov cx,[2002]",   "8B 0E 02 20", "mw[0x2002]=0x1234", "cx=0x1234"),
    V("mov al,[2010]",   "A0 10 20", "mb[0x2010]=0x5A", "al=0x5A"),
    V("mov [2011],al",   "A2 11 20", "al=0xC3", "mb[0x2011]=0xC3"),
    V("mov ax,[2012]",   "A1 12 20", "mw[0x2012]=0x7777", "ax=0x7777"),
    V("mov [2014],ax",   "A3 14 20", "ax=0x1357", "mw[0x2014]=0x1357"),
    V("mov byte[2016],42","C6 06 16 20 42", "", "mb[0x2016]=0x42"),
    V("mov word[2018],imm","C7 06 18 20 78 56", "", "mw[0x2018]=0x5678"),
    V("mov eax,[2020]",  "66 A1 20 20", "md[0x2020]=0x89ABCDEF", "eax=0x89ABCDEF"),
    V("mov [2024],eax",  "66 A3 24 20", "eax=0x0BADF00D", "md[0x2024]=0x0BADF00D"),
    V("mov ds:load",     "8E D8 A1 00 02", "ax=0x0400,mw[0x4200]=0x4242", "ax=0x4242"),
    V("mov cx,es",       "8C C1", "es=0x0100", "cx=0x0100"),
    V("mov ss,dx",       "8E D2", "dx=0x0800", "ss=0x0800"),
    V("lea bx,[2000]",   "8D 1E 00 20", "", "bx=0x2000"),
    V("lea si,[bx+8]",   "8D 77 08", "bx=0x1000", "si=0x1008"),
    V("cs: overrides ds", "2E 8B 06 10 20", "ds=0x0100,mw[0x2010]=0x5151", "ax=0x5151"),
    V("es: override st", "26 89 06 20 20", "es=0x0100,ax=0xCAFE", "mw[0x3020]=0xCAFE"),
    V("mov [di],ax",     "89 05", "di=0x2100,ax=0x9999", "mw[0x2100]=0x9999"),
    V("mov dx,[si]",     "8B 14", "si=0x2200,mw[0x2200]=0x5678", "dx=0x5678"),

    /* -- H: stack + control ----------------------------------------------- */
    V("push bx",         "53", "bx=0x1111", "sp=0x7FFE,mw[0x7FFE]=0x1111"),
    V("pop bx",          "5B", "sp=0x7FFE,mw[0x7FFE]=0x2222", "bx=0x2222,sp=0x8000"),
    V("push imm16",      "68 34 12", "", "mw[0x7FFE]=0x1234"),
    V("push imm8 -1",    "6A FF", "", "mw[0x7FFE]=0xFFFF"),
    V("pushf has bit1",  "9C 58", "zf=1", "ax=0x0042"),
    V("popf sets cf",    "9D", "sp=0x7FFE,mw[0x7FFE]=0x0001", "cf=1,sp=0x8000"),
    V("call/ret balance","E8 03 00 90 90 F4 40 C3", "ax=0", "ax=1,sp=0x8000"),
    V("jmp short fwd",   "EB 02 90 90 40 F4", "ax=0", "ax=1"),
    V("jmp near fwd",    "E9 02 00 90 F4 90 40", "ax=0", "ax=1"),
    V("push cs",         "0E", "", "mw[0x7FFE]=0"),
    V("pop ds",          "1F", "sp=0x7FFE,mw[0x7FFE]=0x0300", "ds=0x0300,sp=0x8000"),
    V("push ds pop es",  "1E 07", "ds=0x0500", "es=0x0500,sp=0x8000"),
    V("push sp = original","54", "", "mw[0x7FFE]=0x8000"),
    V("push bp pop bx",  "55 5B", "bp=0x4321", "bx=0x4321,sp=0x8000"),

    /* -- I: mul/div/neg/not ------------------------------------------------ */
    V("mul bl small",   "F6 E3", "al=6,bl=7",  "ax=42,cf=0,of=0"),
    V("mul bl big",     "F6 E3", "al=0xFF,bl=0xFF", "ax=0xFE01,cf=1,of=1"),
    V("imul cl neg",    "F6 E9", "al=0xFE,cl=3", "ax=0xFFFA,cf=0,of=0"),
    V("imul dl neg",    "F6 EA", "al=4,dl=0xFE", "ax=0xFFF8"),
    V("div cl exact",   "F6 F1", "ax=100,cl=5", "al=20,ah=0"),
    V("div bl rem",     "F6 F3", "ax=103,bl=5", "al=20,ah=3"),
    V("idiv cl neg",    "F6 F9", "ax=0xFFFB,cl=2", "al=0xFE,ah=0xFF"),
    V("mul cx 16 ovf",  "F7 E1", "ax=0x1000,cx=0x10", "ax=0,dx=1,cf=1,of=1"),
    V("mul bx 16 no",   "F7 E3", "ax=3,bx=3", "ax=9,dx=0,cf=0,of=0"),
    V("imul dx fits",   "F7 EA", "ax=0xFFFF,dx=2", "ax=0xFFFE,dx=0xFFFF,cf=0,of=0"),
    V("imul si ovf",    "F7 EE", "ax=0x1234,si=0x1234", "ax=0x5A90,dx=0x014B,cf=1,of=1"),
    V("div bx dx:ax",   "F7 F3", "dx=1,ax=0,bx=0x1000", "ax=0x10,dx=0"),
    V("div cx rem",     "F7 F1", "dx=0,ax=100,cx=3", "ax=33,dx=1"),
    V("idiv bx neg",    "F7 FB", "dx=0xFFFF,ax=0xFFFB,bx=2", "ax=0xFFFE,dx=0xFFFF"),
    V("neg bl",         "F6 DB", "bl=5",   "bl=0xFB,cf=1,sf=1"),
    V("not bl keeps cf","F6 D3", "bl=0xF0,cf=1", "bl=0x0F,cf=1"),
    V("neg ax 0x8000",  "F7 D8", "ax=0x8000", "ax=0x8000,of=1"),
    V("not ax keeps cf","F7 D0", "ax=0,cf=1", "ax=0xFFFF,cf=1"),
    V("neg cx zero",    "F7 D9", "cx=0",   "cx=0,cf=0,zf=1"),

    /* -- J: string ops ------------------------------------------------------ */
    V("movsb fwd",      "A4", "si=0x3000,di=0x3010,mb[0x3000]=0x77",
        "mb[0x3010]=0x77,si=0x3001,di=0x3011"),
    V("movsw fwd",      "A5", "si=0x3000,di=0x3020,mw[0x3000]=0xBEEF",
        "mw[0x3020]=0xBEEF,si=0x3002"),
    V("rep movsb 5",    "F3 A4", "cx=5,si=0x3000,di=0x3100,mb[0x3004]=0x99",
        "mb[0x3104]=0x99,cx=0,si=0x3005,di=0x3105"),
    V("rep movsw 3",    "F3 A5", "cx=3,si=0x3000,di=0x3200,mw[0x3004]=0x1234",
        "mw[0x3204]=0x1234,cx=0"),
    V("movsb backward", "A4", "df=1,si=0x3001,di=0x3011,mb[0x3001]=0x42",
        "mb[0x3011]=0x42,si=0x3000,di=0x3010"),
    V("stosb",          "AA", "di=0x3300,al=0x5A", "mb[0x3300]=0x5A,di=0x3301"),
    V("rep stosw 4",    "F3 AB", "cx=4,di=0x3400,ax=0xA5A5",
        "mw[0x3406]=0xA5A5,cx=0,di=0x3408"),
    V("rep stos cx0 noop","F3 AA", "cx=0,al=0x7E,di=0x3500", "mb[0x3500]=0,di=0x3500"),
    V("lodsb",          "AC", "si=0x3600,mb[0x3600]=0x3C", "al=0x3C,si=0x3601"),
    V("lodsw backward", "AD", "df=1,si=0x3700,mw[0x3700]=0x2345", "ax=0x2345,si=0x36FE"),
    V("scasb equal",    "AE", "di=0x3800,al=0x77,mb[0x3800]=0x77", "zf=1,di=0x3801"),
    V("scasw less",     "AF", "di=0x3810,ax=3,mw[0x3810]=5", "zf=0,cf=1"),
    V("repne scasb hit","F2 AE", "cx=4,al=0x41,di=0x3900,mb[0x3902]=0x41", "zf=1,cx=1"),
    V("repe cmpsb 3rd", "F3 A6",
        "cx=3,si=0x3A00,di=0x3A10,mb[0x3A00]=0x11,mb[0x3A10]=0x11,"
        "mb[0x3A01]=0x22,mb[0x3A11]=0x22,mb[0x3A02]=0x01,mb[0x3A12]=0x02",
        "zf=0,cf=1,cx=0,si=0x3A03,di=0x3A13"),
    V("cmpsw equal",    "A7", "si=0x3B00,di=0x3B10,mw[0x3B00]=0xAAAA,mw[0x3B10]=0xAAAA", "zf=1"),
    V("movsw twice",    "A5 A5",
        "si=0x3000,di=0x3C00,mw[0x3000]=0x1111,mw[0x3002]=0x2222",
        "mw[0x3C02]=0x2222,si=0x3004,di=0x3C04"),
    V("stosd 32",       "66 AB", "di=0x3D00,eax=0xDEADBEEF", "md[0x3D00]=0xDEADBEEF,di=0x3D04"),
    V("cmpsb single",   "A6", "si=0x3E00,di=0x3E08,mb[0x3E00]=9,mb[0x3E08]=9",
        "zf=1,si=0x3E01,di=0x3E09"),
    V("repne scasb miss","F2 AE", "cx=3,al=0x5A,di=0x3F00", "cx=0,zf=0"),
    V("lodsd 32",       "66 AD", "si=0x3000,md[0x3000]=0x12345678", "eax=0x12345678,si=0x3004"),

    /* -- K: xchg / xadd / cmpxchg / bswap / misc ---------------------------- */
    V("xchg ax,bx",     "87 D8", "ax=0x1111,bx=0x2222", "ax=0x2222,bx=0x1111"),
    V("xchg al,bl",     "86 D8", "al=0x12,bl=0x34", "al=0x34,bl=0x12"),
    V("xchg ax,[2000]", "87 06 00 20", "ax=0xAAAA,mw[0x2000]=0x5555",
        "ax=0x5555,mw[0x2000]=0xAAAA"),
    V("xchg ax,cx 91",  "91", "ax=1,cx=2", "ax=2,cx=1"),
    V("xchg ax,dx 92",  "92", "ax=1,dx=2", "ax=2,dx=1"),
    V("xchg ax,si 96",  "96", "ax=1,si=2", "ax=2,si=1"),
    V("xchg ax,di 97",  "97", "ax=1,di=2", "ax=2,di=1"),
    V("nop 90",         "90", "ax=0x1234", "ax=0x1234"),
    V("xadd ax,bx",     "0F C1 D8", "ax=5,bx=3", "ax=8,bx=5"),
    V("xadd al,bl of",  "0F C0 D8", "al=0x7F,bl=1", "al=0x80,bl=0x7F,of=1"),
    V("xadd [2000],dx", "0F C1 16 00 20", "mw[0x2000]=0x1000,dx=0x2000",
        "mw[0x2000]=0x3000,dx=0x1000"),
    V("cmpxchg bx,cx eq","0F B1 CB", "ax=0x2222,bx=0x2222,cx=0x9999", "bx=0x9999,zf=1"),
    V("cmpxchg bx,cx ne","0F B1 CB", "ax=0x1111,bx=0x2222,cx=0x9999",
        "ax=0x2222,bx=0x2222,zf=0"),
    V("cmpxchg bl,cl eq","0F B0 CB", "al=0x42,bl=0x42,cl=0x7E", "bl=0x7E,zf=1"),
    V("cmpxchg [2000],dx ne","0F B1 16 00 20", "ax=1,mw[0x2000]=2,dx=3", "ax=2,mw[0x2000]=2,zf=0"),
    V("bswap eax",      "66 0F C8", "eax=0x12345678", "eax=0x78563412"),
    V("bswap ebx",      "66 0F CB", "ebx=0x00ABCDEF", "ebx=0xEFCDAB00"),
    V("cbw neg",        "98", "al=0x80", "ax=0xFF80"),
    V("cbw pos",        "98", "al=0x7F", "ax=0x007F"),
    V("cwd neg",        "99", "ax=0x8001", "dx=0xFFFF"),
    V("cwd pos",        "99", "ax=0x7FFF", "dx=0"),
    V("cwde",           "66 98", "eax=0x8000", "eax=0xFFFF8000"),
    V("cdq",            "66 99", "eax=0x80000000", "edx=0xFFFFFFFF"),
    V("pause nop",      "F3 90", "ax=5", "ax=5"),
    V("fences nop",     "0F AE E8 0F AE F0 0F AE F8", "ax=9", "ax=9"),

    /* -- K2: movsx / movzx --------------------------------------------------- */
    V("movsx ax,bl -1",  "0F BE C3", "bl=0xFF", "ax=0xFFFF"),
    V("movsx cx,bl -128","0F BE CB", "bl=0x80", "cx=0xFF80"),
    V("movsx eax,bx 32", "66 0F BF C3", "bx=0x8000", "eax=0xFFFF8000"),
    V("movzx ax,bl",     "0F B6 C3", "bl=0xFF", "ax=0x00FF"),
    V("movzx cx,bh",     "0F B6 CF", "bh=0x81", "cx=0x0081"),
    V("movzx edx,[2000]","66 0F B6 16 00 20", "mb[0x2000]=0xA5", "edx=0xA5"),
    V("movsx eax,[2002]","66 0F BF 06 02 20", "mw[0x2002]=0x8001", "eax=0xFFFF8001"),
    V("movzx bx,al",     "0F B6 D8", "al=0xC3", "bx=0x00C3"),
    V("movzx si,[2004]", "0F B6 36 04 20", "mb[0x2004]=0x77", "si=0x0077"),

    /* -- L: CMOVcc + SETcc (bx=9 src; dest ax/bl starts 0) ------------------- */
    V("cmovo taken",  "0F 40 C3", "bx=9,of=1", "ax=9"),
    V("cmovno not",   "0F 41 C3", "bx=9,of=1", "ax=0"),
    V("cmovb taken",  "0F 42 C3", "bx=9,cf=1", "ax=9"),
    V("cmovae not",   "0F 43 C3", "bx=9,cf=1", "ax=0"),
    V("cmove taken",  "0F 44 C3", "bx=9,zf=1", "ax=9"),
    V("cmovne not",   "0F 45 C3", "bx=9,zf=1", "ax=0"),
    V("cmovbe taken", "0F 46 C3", "bx=9,zf=1,cf=0", "ax=9"),
    V("cmova not",    "0F 47 C3", "bx=9,zf=1,cf=0", "ax=0"),
    V("cmovs taken",  "0F 48 C3", "bx=9,sf=1", "ax=9"),
    V("cmovns not",   "0F 49 C3", "bx=9,sf=1", "ax=0"),
    V("cmovp taken",  "0F 4A C3", "bx=9,pf=1", "ax=9"),
    V("cmovnp not",   "0F 4B C3", "bx=9,pf=1", "ax=0"),
    V("cmovl taken",  "0F 4C C3", "bx=9,of=1,sf=0", "ax=9"),
    V("cmovge not",   "0F 4D C3", "bx=9,of=1,sf=0", "ax=0"),
    V("cmovle taken", "0F 4E C3", "bx=9,sf=1,of=0", "ax=9"),
    V("cmovg not",    "0F 4F C3", "bx=9,sf=1,of=0", "ax=0"),
    V("seto 1",  "0F 90 C3", "of=1", "bl=1"),
    V("setno 0", "0F 91 C3", "of=1", "bl=0"),
    V("setb 1",  "0F 92 C3", "cf=1", "bl=1"),
    V("setae 0", "0F 93 C3", "cf=1", "bl=0"),
    V("sete 1",  "0F 94 C3", "zf=1", "bl=1"),
    V("setne 0", "0F 95 C3", "zf=1", "bl=0"),
    V("setbe 1", "0F 96 C3", "zf=1,cf=0", "bl=1"),
    V("seta 0",  "0F 97 C3", "zf=1,cf=0", "bl=0"),
    V("sets 1",  "0F 98 C3", "sf=1", "bl=1"),
    V("setns 0", "0F 99 C3", "sf=1", "bl=0"),
    V("setp 1",  "0F 9A C3", "pf=1", "bl=1"),
    V("setnp 0", "0F 9B C3", "pf=1", "bl=0"),
    V("setl 1",  "0F 9C C3", "of=1,sf=0", "bl=1"),
    V("setge 0", "0F 9D C3", "of=1,sf=0", "bl=0"),
    V("setle 1", "0F 9E C3", "sf=1,of=0", "bl=1"),
    V("setg 0",  "0F 9F C3", "sf=1,of=0", "bl=0"),

    /* -- M: flag control / inc / dec ----------------------------------------- */
    V("stc", "F9", "cf=0", "cf=1"),
    V("clc", "F8", "cf=1", "cf=0"),
    V("cmc 1->0", "F5", "cf=1", "cf=0"),
    V("cmc 0->1", "F5", "cf=0", "cf=1"),
    V("std", "FD", "df=0", "df=1"),
    V("std then cld", "FD FC", "df=0", "df=0"),
    V("cli sti", "FA FB", "if=0", "if=1"),
    V("inc ax of edge", "40", "ax=0x7FFF", "ax=0x8000,of=1,af=1,sf=1"),
    V("dec ax of edge", "48", "ax=0x8000", "ax=0x7FFF,of=1,af=1,pf=1"),
    V("inc cx wraps", "41", "cx=0xFFFF,cf=1", "cx=0,zf=1,cf=1"),
    V("dec bx af/pf", "4B", "bx=0x10", "bx=0x0F,af=1,pf=1"),

    /* -- N: CPUID / RDTSC / faults ------------------------------------------- */
    V("cpuid leaf1 sig", "66 B8 01 00 00 00 0F A2", "", "eax=0x000306C3"),
    V("cpuid leaf0 genu","66 B8 00 00 00 00 0F A2", "", "ebx=0x756E6547"),
    V("cpuid max ext",   "66 B8 00 00 00 80 0F A2", "", "eax=0x80000008"),
    V("rdtsc t0",        "0F 31", "", "eax=0,edx=0"),
    V("rdtsc t1 ratio",  "90 0F 31", "", "eax=92"),

    /* -- O: hint block 0F 18-1F (PREFETCHh / multibyte NOP), KERNEL-BOOT K2 --
     * decode-only: no memory access, no flag writes, never a fault -- first
     * measured need: clang's 0F 1F /0 alignment NOPs in the AuraLite kernel */
    V("hint nop [bx+si]",  "0F 1F 00",           "rax=0x1234", "rax=0x1234"),
    V("hint nop disp16",   "0F 1F 84 78 56",     "cf=1", "cf=1"),
    V("hint nop disp8",    "0F 1F 43 10",        "zf=0", "zf=0"),
    V("hint nop 0f1d",     "0F 1D 00",           "", "rax=0"),
    V("prefetchh 0f18",    "0F 18 00",           "mb[0x200]=0x5A", "mb[0x200]=0x5A"),
    V("hint nop flags",    "0F 1F 40 20",        "cf=1,of=1,zf=1", "cf=1,of=1,zf=1"),

    /* -- P: BT/BTS/BTR/BTC bit-string family + 0F BA group, KERNEL-BOOT K2 --
     * CF := old bit; other arithmetic flags architecturally undefined (left) */
    V("bt reg set",        "0F A3 C1",           "rcx=0xF0,rax=4,cf=0", "cf=1"),
    V("bt reg clear",      "0F A3 C1",           "rcx=0,rax=2,cf=1", "cf=0"),
    V("bt mem word",       "0F A3 06 00 02",     "rax=3,mw[0x200]=0xFFFF", "cf=1"),
    V("bt mem neg sel",    "0F A3 06 00 02",     "rax=0xFFFFFFFC,mw[0x1FE]=0x0002", "cf=0"),
    V("bts reg sets",      "0F AB C1",           "rcx=0,rax=5,cf=1", "rcx=0x20,cf=0"),
    V("btr reg clears",    "0F B3 C1",           "rcx=0xFF,rax=3,cf=0", "rcx=0xF7,cf=1"),
    V("btc reg toggles",   "0F BB C1",           "rcx=0xFF,rax=3", "rcx=0xF7,cf=1"),
    V("bts mem",           "0F AB 06 00 02",     "rax=9,mw[0x200]=0", "mw[0x200]=0x0200,cf=0"),
    V("grp bt imm",        "0F BA E1 03",        "rcx=0x8,cf=0", "cf=1"),
    V("grp bts imm",       "0F BA E9 01",        "rcx=0", "rcx=0x2,cf=0"),
    V("grp btr imm set",   "0F BA F1 02",        "rcx=0xF", "rcx=0xB,cf=1"),
    V("grp btc imm",       "0F BA F9 05",        "rcx=0x20,cf=1", "rcx=0,cf=1"),

    /* -- Q: MOVSXD long-mode form boundary (K2) -- in real16/prot the byte
     * 0x63 is ARPL, documented out of scope: a deliberate #UD, pinned here. */
    V("arpl scope pin",    "63 C1",              "", "fault=1"),

    /* -- R: BSF/BSR (K3) -- scan direction, zero source keeps the
     * destination register (silicon-real, SDM "undefined") and raises ZF;
     * a set source always clears ZF. */
    V("bsf eax,ebx",       "66 0F BC C3",        "ebx=0x40", "eax=6,zf=0"),
    V("bsr eax,ebx",       "66 0F BD C3",        "ebx=0x80000000", "eax=31,zf=0"),
    V("bsf zero keeps",    "66 0F BC C3",        "ebx=0,eax=0x11", "eax=0x11,zf=1"),
    V("bsr zero keeps",    "66 0F BD C3",        "ebx=0,eax=0x22", "eax=0x22,zf=1"),
    V("bsf 16-bit src",    "0F BC C3",           "bx=0x10", "ax=4,zf=0"),
    V("bsr low bit",       "66 0F BD C3",        "ebx=0x1", "eax=0,zf=0"),

    /* -- T: x87 control subset (K3) -- FNINIT/FNCLEX are pure no-ops
     * (flags preserved), and the data-op boundary is a pinned #UD. */
    V("fninit preserves",  "DB E3",              "cf=1", "cf=1"),
    V("fnclex preserves",  "DB E2",              "zf=1", "zf=1"),
    V("x87 data stays #UD","D9 C0",              "", "fault=1"),

    /* SWAPGS is 64-bit only; the legacy pin guards the deliberate #UD. */
    V("swapgs legacy pin", "0F 01 F8",           "", "fault=1"),

    /* -- S: group-15 memory forms (K3) -- FXSAVE/FXRSTOR/LDMXCSR/STMXCSR
     * decode; the XSAVE side is CPUID-gated so #UD is the architected
     * answer here, and the mod==3 /0..3 space is reserved. */
    V("xm/xsave #UD",      "0F AE 2C 24",        "", "fault=1"),
    V("grp15 mod3 resv",   "0F AE C0",           "", "fault=1"),
    V("ud2 faults",      "0F 0B", "", "fault=1"),
    V("0F FF faults",    "0F FF", "", "fault=1"),
    V("0F AE bad faults","0F AE 30", "", "fault=1"),

    /* -- O: imul forms --------------------------------------------------------- */
    V("imul ax,bx small", "0F AF C3", "ax=7,bx=9", "ax=63,cf=0,of=0"),
    V("imul ax,bx ovf",   "0F AF C3", "ax=0x4000,bx=4", "ax=0,cf=1,of=1"),
    V("imul cx,dx neg",   "0F AF CA", "cx=0xFFFF,dx=2", "cx=0xFFFE,cf=0,of=0"),
    V("imul ax,ax,16",    "6B C0 10", "ax=3", "ax=48,cf=0,of=0"),
    V("imul ax,ax,-1",    "6B C0 FF", "ax=5", "ax=0xFFFB"),
    V("imul bx,cx,0x20",  "6B D9 20", "cx=9", "bx=0x120"),
    V("imul ax,ax,256",   "69 C0 00 01", "ax=5", "ax=0x500"),
    V("imul dx,dx,-1",    "69 D2 FF FF", "dx=3", "dx=0xFFFD"),

    /* -- P: test / acc-imm / FF group / 8-bit mem ------------------------------ */
    V("test al,1 nonzero","A8 01", "al=3", "zf=0,sf=0"),
    V("test ax,0x8000",  "A9 00 80", "ax=0x8000", "sf=1,zf=0"),
    V("test cx,cx zero", "85 C9", "cx=0", "zf=1"),
    V("test dx,dx nz",   "85 D2", "dx=0x55", "zf=0"),
    V("test ax,imm",     "F7 C0 34 12", "ax=0x1234", "zf=0"),
    V("test al,-1 sign", "F6 C0 FF", "al=0x80", "sf=1"),
    V("test cl,1 zero",  "F6 C1 01", "cl=2", "zf=1"),
    V("add al,5",        "04 05", "al=0x7C", "al=0x81,sf=1,pf=1"),
    V("add ax,0x8000 of","05 00 80", "ax=0x8000", "ax=0,cf=1,zf=1,of=1"),
    V("or al,0x80",      "0C 80", "al=1", "al=0x81"),
    V("adc ax,0x1234",   "15 34 12", "ax=0x1234,cf=1", "ax=0x2469"),
    V("sbb ax,1",        "1D 01 00", "ax=5,cf=1", "ax=3"),
    V("and ax,0x0FF0",   "25 F0 0F", "ax=0xFFFF", "ax=0x0FF0"),
    V("sub ax,1",        "2D 01 00", "ax=0", "ax=0xFFFF,cf=1"),
    V("xor ax,-1",       "35 FF FF", "ax=0xAAAA", "ax=0x5555"),
    V("cmp ax,0",        "3D 00 00", "ax=5", "zf=0"),
    V("cmp al,0x7F",     "3C 7F", "al=0x7F", "zf=1"),
    V("inc word[2000]",  "FF 06 00 20", "mw[0x2000]=0xFFFF,cf=1", "mw[0x2000]=0,zf=1,cf=1"),
    V("dec word[2002]",  "FF 0E 02 20", "mw[0x2002]=0,cf=1", "mw[0x2002]=0xFFFF,sf=1,cf=1"),
    V("push word[2006]", "FF 36 06 20", "mw[0x2006]=0x77AA", "mw[0x7FFE]=0x77AA,sp=0x7FFE"),
    V("mov [si],al",     "88 04", "si=0x2100,al=0xAB", "mb[0x2100]=0xAB"),
    V("mov al,[si]",     "8A 04", "si=0x2200,mb[0x2200]=0x5F", "al=0x5F"),
    V("mov [si],bl",     "88 1C", "si=0x2100,bl=0xCD", "mb[0x2100]=0xCD"),
    V("mov ah,[di]",     "8A 25", "di=0x2300,mb[0x2300]=0x66", "ah=0x66"),
    V("mov dh,[bx+si]",  "8A 30", "bx=0x2000,si=0x10,mb[0x2010]=0x99", "dh=0x99"),
    V("mov byte[bp],42", "C6 46 00 42", "bp=0x2400", "mb[0x2400]=0x42"),

    /* -- Q: 32-bit forms -------------------------------------------------------- */
    V("add eax,ebx cf",  "66 01 D8", "eax=0xFFFFFFFE,ebx=3", "eax=1,cf=1"),
    V("sub eax,ebx cf",  "66 29 D8", "eax=5,ebx=10", "eax=0xFFFFFFFB,cf=1"),
    V("xor eax,ebx self","66 31 D8", "eax=7,ebx=7", "eax=0,zf=1"),
    V("mov eax,ebx",     "66 89 D8", "ebx=0x00CAFE00", "eax=0x00CAFE00"),
    V("imul eax,ebx ovf","66 0F AF C3", "eax=0x10000,ebx=0x10000", "eax=0,cf=1,of=1"),
    V("cmp eax,0",       "66 83 F8 00", "eax=0", "zf=1"),
};

/* --------------------------------------------------------------- runner -- */

int main(void) {
    size_t n = sizeof VECTORS / sizeof *VECTORS;
    for (size_t i = 0; i < n; i++) {
        const vec_t *v = &VECTORS[i];
        machine_t m;
        setup_machine(&m);
        apply_list(&m, v->setup, 1, v->name, NULL);
        uint8_t code[64];
        int len = hex_bytes(v->hex, code, (int)sizeof code);
        assert(len > 0);
        /* append HLT unless the vector declares an expected fault */
        int want_fault = strstr(v->expect, "fault=1") != NULL;
        if (!want_fault) code[len] = 0xF4, len++;
        run_program(&m, code, (size_t)len, 0, 512);
        if (m.cpu.fault && !want_fault)
            fprintf(stderr, "vector %u '%s' faulted: %s\n",
                    (unsigned)i, v->name, m.cpu.fault_msg);
        assert(want_fault || !m.cpu.fault);
        assert(!want_fault || m.cpu.fault);
        apply_list(&m, v->expect, 0, v->name, NULL);
    }
    printf("table vectors: %zu/%zu ok\n", n, n);
    assert(n >= 300);
    return 0;
}
