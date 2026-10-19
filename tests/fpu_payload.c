/* tests/fpu_payload.c -- FPU F4: the --kernel= freestanding x87 payload.
 *
 * The last lane of the FPU plan: a kernel built from this file is booted
 * through the --kernel= loader (the same kloader.c core the CLI flag
 * drives) and runs a fixed x87 tour, printing COM1 receipt lines
 * ("F4R<n>=<hex>") whose bytes pin the F0-F3 receipt content from inside
 * a real guest: constants, arithmetic, the FSIN host payload, BCD store
 * and load, and FSW bits. tests/test_fpu_gate.c boots it twice on fresh
 * machines and requires the receipts to be byte-identical AND equal the
 * pinned content; tests/integration/t4_snapshot_lanes.sh runs the same
 * file through the real CLI --kernel= lane and pins the receipt line.
 *
 * x87 opcodes are emitted as raw .byte sequences: GAS mis-assembles some
 * x87 mnemonics (the fsubp/fsubrp family), and the vectors here must be
 * readable against an opcode table, so every x87 byte is spelled out.
 * There is no libc here: freestanding, COM1 via `outb`, exit via HLT.
 */
typedef unsigned char  u8;
typedef unsigned short u16;
typedef unsigned int   u32;

/* scratch for the 80-bit stores (16 bytes: the extra 6 are the padding
 * bytes the engine's fst slots also carry -- the receipts print the 10
 * data bytes only) */
static u8 buf[16];

static void outb(u16 port, u8 v) {
    __asm__ volatile("outb %0, %1" :: "a"(v), "Nd"(port));
}
static void pchar(char c) { outb(0x3F8, (u8)c); }
static void pstr(const char *s) { while (*s) pchar(*s++); }
static void phexn(u32 v, int n) {
    static const char d[] = "0123456789abcdef";
    for (int i = n - 1; i >= 0; i--) pchar(d[(v >> (4 * i)) & 15]);
}
/* the 80-bit store's 10 data bytes, first byte first (memory order) */
static void pbytes(const u8 *b) {
    for (int i = 0; i < 10; i++) phexn(b[i], 2);
}
static void pend(void) { pchar('\n'); }

/* FNSTSW AX -> AX (raw like the rest; "=a" on the 16-bit part) */
static u16 fnstsw_ax(void) {
    u16 sw;
    __asm__ volatile(".byte 0xDF,0xE0" : "=a"(sw));
    return sw;
}

/* +0.5 as 80-bit (se=0xBFFE, sig=0x8000) */
static const u8 half[10] = {0,0,0,0,0,0,0,0x80,0xFE,0x3F};
/* 1234.0 as 80-bit (se=0x4009, sig=0x9A40..: byte 6 is 0x40, byte 7 0x9A) */
static const u8 v1234[10] = {0,0,0,0,0,0,0x40,0x9A,0x09,0x40};
/* 1234 as packed BCD: FBSTP/FBLD order is LSD-first (byte 0 = ones/tens) */
static const u8 bcd[10] = {0x34,0x12,0,0,0,0,0,0,0,0};

__attribute__((section(".text._start")))
void _start(void) {
    /* R0: FNINIT + FLD1 + FNSTSW -- TOP=7 (0x3800), no exceptions */
    __asm__ volatile(".byte 0xDB,0xE3" ::: "memory");            /* fninit */
    __asm__ volatile(".byte 0xD9,0xE8" ::: "memory");            /* fld1   */
    pstr("F4R0="); phexn(fnstsw_ax(), 4); pend();

    /* R1: FLD1 -> FSTP m80: the constant's bytes */
    __asm__ volatile(".byte 0xDB,0xE3" ::: "memory");
    __asm__ volatile(".byte 0xD9,0xE8" ::: "memory");
    __asm__ volatile(".byte 0xDB,0x3B" :: "b"(buf) : "memory");  /* fstp [rbx] */
    pstr("F4R1="); pbytes(buf); pend();

    /* R2: FLDPI -> FSTP m80 */
    __asm__ volatile(".byte 0xDB,0xE3" ::: "memory");
    __asm__ volatile(".byte 0xD9,0xEB" ::: "memory");            /* fldpi  */
    __asm__ volatile(".byte 0xDB,0x3B" :: "b"(buf) : "memory");
    pstr("F4R2="); pbytes(buf); pend();

    /* R3: FSIN(0.5) -> FSTP m80: the host-silicon payload (F0/F3 pin) */
    __asm__ volatile(".byte 0xDB,0xE3" ::: "memory");
    __asm__ volatile(".byte 0xDB,0x2B" :: "b"(half) : "memory"); /* fld m80 */
    __asm__ volatile(".byte 0xD9,0xFE" ::: "memory");            /* fsin    */
    __asm__ volatile(".byte 0xDB,0x3B" :: "b"(buf) : "memory");
    pstr("F4R3="); pbytes(buf); pend();

    /* R4: FNSTSW right after FSIN(0.5): TOP=7 | C1 | PE = 0x3a20 */
    __asm__ volatile(".byte 0xDB,0xE3" ::: "memory");
    __asm__ volatile(".byte 0xDB,0x2B" :: "b"(half) : "memory");
    __asm__ volatile(".byte 0xD9,0xFE" ::: "memory");
    pstr("F4R4="); phexn(fnstsw_ax(), 4); pend();

    /* R5: FBSTP(1234.0) -> the 10 BCD bytes */
    __asm__ volatile(".byte 0xDB,0xE3" ::: "memory");
    __asm__ volatile(".byte 0xDB,0x2B" :: "b"(v1234) : "memory");
    __asm__ volatile(".byte 0xDF,0x33" :: "b"(buf) : "memory");  /* fbstp [rbx] */
    pstr("F4R5="); pbytes(buf); pend();

    /* R6: 1.0 + 1.0 = 2.0 through the FADD path */
    __asm__ volatile(".byte 0xDB,0xE3" ::: "memory");
    __asm__ volatile(".byte 0xD9,0xE8" ::: "memory");            /* fld1 */
    __asm__ volatile(".byte 0xD9,0xE8" ::: "memory");            /* fld1 */
    __asm__ volatile(".byte 0xD8,0xC1" ::: "memory");            /* fadd st0,st1 */
    __asm__ volatile(".byte 0xDB,0x3B" :: "b"(buf) : "memory");
    pstr("F4R6="); pbytes(buf); pend();

    /* R7: FBLD of the 1234 BCD bytes -> FSTP: back to 1234.0 */
    __asm__ volatile(".byte 0xDB,0xE3" ::: "memory");
    __asm__ volatile(".byte 0xDF,0x23" :: "b"(bcd) : "memory");  /* fbld [rbx] */
    __asm__ volatile(".byte 0xDB,0x3B" :: "b"(buf) : "memory");
    pstr("F4R7="); pbytes(buf); pend();

    for (;;)
        __asm__ volatile("cli; hlt");
}
