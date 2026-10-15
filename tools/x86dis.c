// x86dis.c -- TOOLING T1: ndisasm-style listing from src/disasm.c.
//
//   x86dis [-b16|-b32|-b64] [-o ORIGIN] [--stride=N] FILE
//
// Prints "OFFSET  HEXBYTES  TEXT" lines like `ndisasm`. With --stride=N it
// decodes exactly one instruction at every multiple of N (the layout the
// differential corpus of tests/disasm_diff.py uses) instead of a linear
// sweep.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../src/disasm.h"

static void line(uint64_t off, const uint8_t *b, const dis_insn_t *in) {
    char hex[40]; size_t h = 0;
    for (int i = 0; i < in->len && h + 2 < sizeof hex; i++) h += (size_t)snprintf(hex + h, sizeof hex - h, "%02X", b[i]);
    printf("%08llX  %-16s  %s\n", (unsigned long long)off, hex, in->text);
}

int main(int argc, char **argv) {
    int bits = 16; uint64_t org = 0; size_t stride = 0; const char *path = NULL;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-b16")) bits = 16;
        else if (!strcmp(argv[i], "-b32")) bits = 32;
        else if (!strcmp(argv[i], "-b64")) bits = 64;
        else if (!strcmp(argv[i], "-o") && i + 1 < argc) org = strtoull(argv[++i], NULL, 0);
        else if (!strncmp(argv[i], "--stride=", 9)) stride = (size_t)strtoull(argv[i] + 9, NULL, 0);
        else if (argv[i][0] == '-') { fprintf(stderr, "x86dis: unknown option %s\n", argv[i]); return 1; }
        else path = argv[i];
    }
    if (!path) { fprintf(stderr, "usage: x86dis [-b16|-b32|-b64] [-o ORIGIN] [--stride=N] FILE\n"); return 1; }
    FILE *f = fopen(path, "rb");
    if (!f) { perror(path); return 1; }
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    if (n < 0) { fclose(f); return 1; }
    uint8_t *buf = malloc((size_t)n + 16);
    if (!buf) { fclose(f); return 1; }
    size_t got = fread(buf, 1, (size_t)n, f);
    fclose(f);
    dis_insn_t in;
    if (stride) {
        for (size_t off = 0; off < got; off += stride) {
            size_t avail = got - off < stride ? got - off : stride;
            dis_decode(buf + off, avail, bits, org + off, &in);
            line(org + off, buf + off, &in);
        }
    } else {
        size_t off = 0;
        while (off < got) {
            dis_decode(buf + off, got - off, bits, org + off, &in);
            if (in.len <= 0) break;
            line(org + off, buf + off, &in);
            off += (size_t)in.len;
        }
    }
    free(buf);
    return 0;
}
