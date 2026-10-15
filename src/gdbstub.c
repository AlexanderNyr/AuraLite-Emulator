// gdbstub.c -- TOOLING T3: GDB Remote Serial Protocol over TCP.
//
// One connection, single-threaded: the stub is a T2 front end. While the
// guest runs, the core polls it every 64Ki steps for a Ctrl-C (0x03) or a
// disconnect; while stopped, the core poll()s its socket and the stub
// answers packets until gdb resumes (c/s/vCont), detaches (D) or kills (k).
#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <poll.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include "gdbstub.h"
#include "dbg.h"
#include "cpu.h"

typedef struct {
    dbg_frontend_t fe;
    int fd;
    int noack;               /* QStartNoAckMode negotiated                */
    int running;             /* gdb resumed us and awaits a stop reply    */
    int g_vcpu;              /* Hg: thread for register/memory access     */
    char in[16384];
    size_t inlen;
    char *last;              /* last packet sent (for '-' retransmits)    */
    unsigned poll_div;
    int dead;
    int left;                /* k/D already chose the resume action      */
} gdb_t;

/* ------------------------------------------------------------ framing --- */
static const char hexd[] = "0123456789abcdef";

static void send_all(gdb_t *g, const char *p, size_t n) {
    while (n && !g->dead) {
        ssize_t w = send(g->fd, p, n, MSG_NOSIGNAL);
        if (w < 0) { if (errno == EINTR) continue; g->dead = 1; return; }
        p += w; n -= (size_t)w;
    }
}

static void send_packet(gdb_t *g, const char *data) {
    size_t n = strlen(data);
    char *pk = malloc(n + 5);
    if (!pk) return;
    unsigned sum = 0;
    for (size_t i = 0; i < n; i++) sum += (unsigned char)data[i];
    pk[0] = '$';
    memcpy(pk + 1, data, n);
    pk[n + 1] = '#'; pk[n + 2] = hexd[(sum >> 4) & 15]; pk[n + 3] = hexd[sum & 15]; pk[n + 4] = 0;
    send_all(g, pk, n + 4);
    free(g->last);
    g->last = pk;
}

static int hexval(int c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static void put_le(char *out, uint64_t v, int bytes) {
    for (int i = 0; i < bytes; i++) {
        uint8_t b = (uint8_t)(v >> (8 * i));
        out[2 * i] = hexd[b >> 4]; out[2 * i + 1] = hexd[b & 15];
    }
    out[2 * bytes] = 0;
}
static uint64_t get_le(const char *in, int bytes) {
    uint64_t v = 0;
    for (int i = 0; i < bytes; i++) {
        int hi = hexval(in[2 * i]), lo = hexval(in[2 * i + 1]);
        if (hi < 0 || lo < 0) break;
        v |= (uint64_t)((hi << 4) | lo) << (8 * i);
    }
    return v;
}

/* ---------------------------------------------------- target registers --- */
/* GDB amd64 numbering (target.xml below): 0-15 GPRs in gdb order, 16 rip,
 * 17 eflags, 18-23 cs ss ds es fs gs, 24-31 st0-7, 32-39 x87 control,
 * 40-55 xmm0-15, 56 mxcsr, 57 fs_base, 58 gs_base, 59-63 cr0 cr2 cr3 cr4
 * efer. */
static const int gdb2gpr[16] = { RAX, RBX, RCX, RDX, RSI, RDI, RBP, RSP,
                                 R8, R9, R10, R11, R12, R13, R14, R15 };
static const int gdbseg[6] = { SEG_CS, SEG_SS, SEG_DS, SEG_ES, SEG_FS, SEG_GS };
#define NREGS 64

static int reg_size(int r) {
    if (r < 17) return 8;
    if (r < 24) return 4;
    if (r < 32) return 10;
    if (r < 40) return 4;
    if (r < 56) return 16;
    if (r == 56) return 4;
    return 8;
}

static void reg_hex(cpu_t *c, int r, char *out) {
    int sz = reg_size(r);
    if (r < 16)       put_le(out, c->gpr[gdb2gpr[r]], 8);
    else if (r == 16) put_le(out, c->rip, 8);
    else if (r == 17) put_le(out, c->rflags, 4);
    else if (r < 24)  put_le(out, c->seg[gdbseg[r - 18]].sel, 4);
    else if (r < 32)  { memset(out, '0', (size_t)sz * 2); out[sz * 2] = 0; }   /* no x87 data model */
    else if (r == 32) put_le(out, c->fcw, 4);
    else if (r == 34) put_le(out, 0xFFFF, 4);                                 /* all tags empty */
    else if (r < 40)  put_le(out, 0, 4);
    else if (r < 56)  { put_le(out, c->xmm[r - 40][0], 8); put_le(out + 16, c->xmm[r - 40][1], 8); }
    else if (r == 56) put_le(out, c->mxcsr, 4);
    else if (r == 57) put_le(out, c->seg[SEG_FS].base, 8);
    else if (r == 58) put_le(out, c->seg[SEG_GS].base, 8);
    else if (r == 59) put_le(out, c->cr0, 8);
    else if (r == 60) put_le(out, c->cr2, 8);
    else if (r == 61) put_le(out, c->cr3, 8);
    else if (r == 62) put_le(out, c->cr4, 8);
    else              put_le(out, c->efer, 8);
}

/* Writable: GPRs, rip, eflags, xmm, mxcsr, fs/gs base. Segment selectors,
 * x87 and control registers are read-only here (a selector write without a
 * descriptor load, or a CR write without the CPU's side effects, would
 * create a machine state the CPU itself could never reach). */
static void reg_set(machine_t *m, cpu_t *c, int r, const char *hex) {
    if (r < 16) c->gpr[gdb2gpr[r]] = get_le(hex, 8);
    else if (r == 16) c->rip = get_le(hex, 8);
    else if (r == 17) c->rflags = (get_le(hex, 4) & 0x00257FD5ull) | 2;
    else if (r >= 40 && r < 56) { c->xmm[r - 40][0] = get_le(hex, 8); c->xmm[r - 40][1] = get_le(hex + 16, 8); }
    else if (r == 56) c->mxcsr = (uint32_t)get_le(hex, 4);
    else if (r == 57) c->seg[SEG_FS].base = get_le(hex, 8);
    else if (r == 58) c->seg[SEG_GS].base = get_le(hex, 8);
    (void)m;
}

static const char target_xml[] =
"<?xml version=\"1.0\"?>\n"
"<!DOCTYPE target SYSTEM \"gdb-target.dtd\">\n"
"<target version=\"1.0\">\n"
"<architecture>i386:x86-64</architecture>\n"
"<feature name=\"org.gnu.gdb.i386.core\">\n"
" <flags id=\"i386_eflags\" size=\"4\">\n"
"  <field name=\"CF\" start=\"0\" end=\"0\"/><field name=\"\" start=\"1\" end=\"1\"/>\n"
"  <field name=\"PF\" start=\"2\" end=\"2\"/><field name=\"AF\" start=\"4\" end=\"4\"/>\n"
"  <field name=\"ZF\" start=\"6\" end=\"6\"/><field name=\"SF\" start=\"7\" end=\"7\"/>\n"
"  <field name=\"TF\" start=\"8\" end=\"8\"/><field name=\"IF\" start=\"9\" end=\"9\"/>\n"
"  <field name=\"DF\" start=\"10\" end=\"10\"/><field name=\"OF\" start=\"11\" end=\"11\"/>\n"
"  <field name=\"NT\" start=\"14\" end=\"14\"/><field name=\"RF\" start=\"16\" end=\"16\"/>\n"
"  <field name=\"VM\" start=\"17\" end=\"17\"/><field name=\"AC\" start=\"18\" end=\"18\"/>\n"
"  <field name=\"VIF\" start=\"19\" end=\"19\"/><field name=\"VIP\" start=\"20\" end=\"20\"/>\n"
"  <field name=\"ID\" start=\"21\" end=\"21\"/>\n"
" </flags>\n"
" <reg name=\"rax\" bitsize=\"64\" type=\"int64\" regnum=\"0\"/>\n"
" <reg name=\"rbx\" bitsize=\"64\" type=\"int64\"/>\n"
" <reg name=\"rcx\" bitsize=\"64\" type=\"int64\"/>\n"
" <reg name=\"rdx\" bitsize=\"64\" type=\"int64\"/>\n"
" <reg name=\"rsi\" bitsize=\"64\" type=\"int64\"/>\n"
" <reg name=\"rdi\" bitsize=\"64\" type=\"int64\"/>\n"
" <reg name=\"rbp\" bitsize=\"64\" type=\"data_ptr\"/>\n"
" <reg name=\"rsp\" bitsize=\"64\" type=\"data_ptr\"/>\n"
" <reg name=\"r8\" bitsize=\"64\" type=\"int64\"/>\n"
" <reg name=\"r9\" bitsize=\"64\" type=\"int64\"/>\n"
" <reg name=\"r10\" bitsize=\"64\" type=\"int64\"/>\n"
" <reg name=\"r11\" bitsize=\"64\" type=\"int64\"/>\n"
" <reg name=\"r12\" bitsize=\"64\" type=\"int64\"/>\n"
" <reg name=\"r13\" bitsize=\"64\" type=\"int64\"/>\n"
" <reg name=\"r14\" bitsize=\"64\" type=\"int64\"/>\n"
" <reg name=\"r15\" bitsize=\"64\" type=\"int64\"/>\n"
" <reg name=\"rip\" bitsize=\"64\" type=\"code_ptr\"/>\n"
" <reg name=\"eflags\" bitsize=\"32\" type=\"i386_eflags\"/>\n"
" <reg name=\"cs\" bitsize=\"32\" type=\"int32\"/>\n"
" <reg name=\"ss\" bitsize=\"32\" type=\"int32\"/>\n"
" <reg name=\"ds\" bitsize=\"32\" type=\"int32\"/>\n"
" <reg name=\"es\" bitsize=\"32\" type=\"int32\"/>\n"
" <reg name=\"fs\" bitsize=\"32\" type=\"int32\"/>\n"
" <reg name=\"gs\" bitsize=\"32\" type=\"int32\"/>\n"
" <reg name=\"st0\" bitsize=\"80\" type=\"i387_ext\"/>\n"
" <reg name=\"st1\" bitsize=\"80\" type=\"i387_ext\"/>\n"
" <reg name=\"st2\" bitsize=\"80\" type=\"i387_ext\"/>\n"
" <reg name=\"st3\" bitsize=\"80\" type=\"i387_ext\"/>\n"
" <reg name=\"st4\" bitsize=\"80\" type=\"i387_ext\"/>\n"
" <reg name=\"st5\" bitsize=\"80\" type=\"i387_ext\"/>\n"
" <reg name=\"st6\" bitsize=\"80\" type=\"i387_ext\"/>\n"
" <reg name=\"st7\" bitsize=\"80\" type=\"i387_ext\"/>\n"
" <reg name=\"fctrl\" bitsize=\"32\" type=\"int\" group=\"float\"/>\n"
" <reg name=\"fstat\" bitsize=\"32\" type=\"int\" group=\"float\"/>\n"
" <reg name=\"ftag\" bitsize=\"32\" type=\"int\" group=\"float\"/>\n"
" <reg name=\"fiseg\" bitsize=\"32\" type=\"int\" group=\"float\"/>\n"
" <reg name=\"fioff\" bitsize=\"32\" type=\"int\" group=\"float\"/>\n"
" <reg name=\"foseg\" bitsize=\"32\" type=\"int\" group=\"float\"/>\n"
" <reg name=\"fooff\" bitsize=\"32\" type=\"int\" group=\"float\"/>\n"
" <reg name=\"fop\" bitsize=\"32\" type=\"int\" group=\"float\"/>\n"
"</feature>\n"
"<feature name=\"org.gnu.gdb.i386.sse\">\n"
" <vector id=\"v4f\" type=\"ieee_single\" count=\"4\"/>\n"
" <vector id=\"v2d\" type=\"ieee_double\" count=\"2\"/>\n"
" <vector id=\"v16i8\" type=\"int8\" count=\"16\"/>\n"
" <vector id=\"v8i16\" type=\"int16\" count=\"8\"/>\n"
" <vector id=\"v4i32\" type=\"int32\" count=\"4\"/>\n"
" <vector id=\"v2i64\" type=\"int64\" count=\"2\"/>\n"
" <union id=\"vec128\">\n"
"  <field name=\"v4_float\" type=\"v4f\"/><field name=\"v2_double\" type=\"v2d\"/>\n"
"  <field name=\"v16_int8\" type=\"v16i8\"/><field name=\"v8_int16\" type=\"v8i16\"/>\n"
"  <field name=\"v4_int32\" type=\"v4i32\"/><field name=\"v2_int64\" type=\"v2i64\"/>\n"
"  <field name=\"uint128\" type=\"uint128\"/>\n"
" </union>\n"
" <flags id=\"i386_mxcsr\" size=\"4\">\n"
"  <field name=\"IE\" start=\"0\" end=\"0\"/><field name=\"DE\" start=\"1\" end=\"1\"/>\n"
"  <field name=\"ZE\" start=\"2\" end=\"2\"/><field name=\"OE\" start=\"3\" end=\"3\"/>\n"
"  <field name=\"UE\" start=\"4\" end=\"4\"/><field name=\"PE\" start=\"5\" end=\"5\"/>\n"
"  <field name=\"DAZ\" start=\"6\" end=\"6\"/><field name=\"IM\" start=\"7\" end=\"7\"/>\n"
"  <field name=\"DM\" start=\"8\" end=\"8\"/><field name=\"ZM\" start=\"9\" end=\"9\"/>\n"
"  <field name=\"OM\" start=\"10\" end=\"10\"/><field name=\"UM\" start=\"11\" end=\"11\"/>\n"
"  <field name=\"PM\" start=\"12\" end=\"12\"/><field name=\"FZ\" start=\"15\" end=\"15\"/>\n"
" </flags>\n"
" <reg name=\"xmm0\" bitsize=\"128\" type=\"vec128\" regnum=\"40\"/>\n"
" <reg name=\"xmm1\" bitsize=\"128\" type=\"vec128\"/>\n"
" <reg name=\"xmm2\" bitsize=\"128\" type=\"vec128\"/>\n"
" <reg name=\"xmm3\" bitsize=\"128\" type=\"vec128\"/>\n"
" <reg name=\"xmm4\" bitsize=\"128\" type=\"vec128\"/>\n"
" <reg name=\"xmm5\" bitsize=\"128\" type=\"vec128\"/>\n"
" <reg name=\"xmm6\" bitsize=\"128\" type=\"vec128\"/>\n"
" <reg name=\"xmm7\" bitsize=\"128\" type=\"vec128\"/>\n"
" <reg name=\"xmm8\" bitsize=\"128\" type=\"vec128\"/>\n"
" <reg name=\"xmm9\" bitsize=\"128\" type=\"vec128\"/>\n"
" <reg name=\"xmm10\" bitsize=\"128\" type=\"vec128\"/>\n"
" <reg name=\"xmm11\" bitsize=\"128\" type=\"vec128\"/>\n"
" <reg name=\"xmm12\" bitsize=\"128\" type=\"vec128\"/>\n"
" <reg name=\"xmm13\" bitsize=\"128\" type=\"vec128\"/>\n"
" <reg name=\"xmm14\" bitsize=\"128\" type=\"vec128\"/>\n"
" <reg name=\"xmm15\" bitsize=\"128\" type=\"vec128\"/>\n"
" <reg name=\"mxcsr\" bitsize=\"32\" type=\"i386_mxcsr\" group=\"vector\"/>\n"
"</feature>\n"
"<feature name=\"org.gnu.gdb.i386.segments\">\n"
" <reg name=\"fs_base\" bitsize=\"64\" type=\"int\"/>\n"
" <reg name=\"gs_base\" bitsize=\"64\" type=\"int\"/>\n"
"</feature>\n"
"<feature name=\"org.auralite.x86emu.sys\">\n"
" <reg name=\"cr0\" bitsize=\"64\" type=\"int\" group=\"system\"/>\n"
" <reg name=\"cr2\" bitsize=\"64\" type=\"int\" group=\"system\"/>\n"
" <reg name=\"cr3\" bitsize=\"64\" type=\"int\" group=\"system\"/>\n"
" <reg name=\"cr4\" bitsize=\"64\" type=\"int\" group=\"system\"/>\n"
" <reg name=\"efer\" bitsize=\"64\" type=\"int\" group=\"system\"/>\n"
"</feature>\n"
"</target>\n";

/* ------------------------------------------------------------ helpers --- */
static int tid_to_vcpu(machine_t *m, long tid) {
    if (tid <= 0) return dbg_selected_vcpu(m);   /* -1 all / 0 any */
    if (tid > dbg_nvcpus(m)) return -1;
    return (int)tid - 1;
}

static void stop_reply(machine_t *m, gdb_t *g, const dbg_stop_t *st) {
    char buf[128];
    if (st->reason == DBG_STOP_END) {
        cpu_t *c = dbg_vcpu(m, 0);
        snprintf(buf, sizeof buf, c->fault ? "X04" : "W00");
    } else if (st->reason == DBG_STOP_WATCH) {
        const char *kind = st->watch_kind == 'r' ? "rwatch" : "watch";
        snprintf(buf, sizeof buf, "T05%s:%llx;thread:%x;", kind, (unsigned long long)st->watch_addr, st->vcpu + 1);
    } else if (st->reason == DBG_STOP_PAUSE) {
        snprintf(buf, sizeof buf, "T02thread:%x;", st->vcpu + 1);
    } else if (st->reason == DBG_STOP_BREAK) {
        snprintf(buf, sizeof buf, "T05thread:%x;swbreak:;", st->vcpu + 1);
    } else {
        snprintf(buf, sizeof buf, "T05thread:%x;", st->vcpu + 1);
    }
    send_packet(g, buf);
}

/* read len bytes at linear addr through the side-effect-free path */
static void cmd_read_mem(machine_t *m, gdb_t *g, const char *args) {
    char *end;
    uint64_t addr = strtoull(args, &end, 16);
    if (*end != ',') { send_packet(g, "E01"); return; }
    uint64_t len = strtoull(end + 1, NULL, 16);
    if (len > 2048) len = 2048;
    uint8_t buf[2048];
    size_t n = dbg_read_lin(m, dbg_vcpu(m, g->g_vcpu), addr, buf, (size_t)len);
    if (!n) { send_packet(g, "E14"); return; }
    char out[4097];
    for (size_t i = 0; i < n; i++) { out[2 * i] = hexd[buf[i] >> 4]; out[2 * i + 1] = hexd[buf[i] & 15]; }
    out[2 * n] = 0;
    send_packet(g, out);
}

static void cmd_write_mem(machine_t *m, gdb_t *g, const char *args, int binary, size_t plen) {
    char *end;
    uint64_t addr = strtoull(args, &end, 16);
    if (*end != ',') { send_packet(g, "E01"); return; }
    uint64_t len = strtoull(end + 1, &end, 16);
    if (*end != ':' || len > 4096) { send_packet(g, "E01"); return; }
    const char *d = end + 1;
    uint8_t buf[4096];
    size_t n = 0;
    if (binary) {
        const char *stop = args + plen;
        while (d < stop && n < len) {
            uint8_t ch = (uint8_t)*d++;
            if (ch == '}' && d < stop) ch = (uint8_t)(*d++ ^ 0x20);
            buf[n++] = ch;
        }
    } else {
        while (d[0] && d[1] && n < len) { buf[n++] = (uint8_t)((hexval(d[0]) << 4) | hexval(d[1])); d += 2; }
    }
    if (n != len) { send_packet(g, "E01"); return; }
    if (len && dbg_write_lin(m, dbg_vcpu(m, g->g_vcpu), addr, buf, (size_t)len) != len) { send_packet(g, "E14"); return; }
    send_packet(g, "OK");
}

static void cmd_breakpoint(machine_t *m, gdb_t *g, const char *p, int insert) {
    int type = p[0] - '0';
    char *end;
    uint64_t addr = strtoull(p + 2, &end, 16);
    uint64_t kind = (*end == ',') ? strtoull(end + 1, NULL, 16) : 1;
    int rc;
    if (type == 0 || type == 1) rc = insert ? (dbg_break_add(m, addr, 0) < 0 ? -1 : 0) : dbg_break_del_addr(m, addr);
    else if (type >= 2 && type <= 4) {
        int k = type == 2 ? 'w' : type == 3 ? 'r' : 'a';
        rc = insert ? (dbg_watch_add(m, addr, (int)kind, k) < 0 ? -1 : 0) : dbg_watch_del_addr(m, addr, (int)kind, k);
    } else { send_packet(g, ""); return; }
    send_packet(g, rc == 0 ? "OK" : "E01");
}

static void cmd_monitor(machine_t *m, gdb_t *g, const char *hex) {
    char cmd[512]; size_t k = 0;
    while (hex[0] && hex[1] && k < sizeof cmd - 1) { cmd[k++] = (char)((hexval(hex[0]) << 4) | hexval(hex[1])); hex += 2; }
    cmd[k] = 0;
    char *text = NULL; size_t tlen = 0;
    FILE *o = open_memstream(&text, &tlen);
    if (!o) { send_packet(g, "E01"); return; }
    int act = dbg_exec(m, cmd, o);
    if (act != DBG_ACT_NONE) fprintf(o, "run control belongs to gdb: use continue/stepi/kill there\n");
    fclose(o);
    for (size_t off = 0; off < tlen; off += 512) {
        size_t n = tlen - off < 512 ? tlen - off : 512;
        char pk[1 + 1024 + 1];
        pk[0] = 'O';
        for (size_t i = 0; i < n; i++) { uint8_t c = (uint8_t)text[off + i]; pk[1 + 2 * i] = hexd[c >> 4]; pk[2 + 2 * i] = hexd[c & 15]; }
        pk[1 + 2 * n] = 0;
        send_packet(g, pk);
    }
    free(text);
    send_packet(g, "OK");
}

/* vCont;a1[:t1];a2[:t2]... -- step the thread named by 's', continue otherwise */
static void cmd_vcont(machine_t *m, gdb_t *g, const char *p) {
    int step_vcpu = -1;
    while (*p == ';') {
        p++;
        char act = *p++;
        if (act == 'C' || act == 'S') { while (*p && *p != ':' && *p != ';') p++; }   /* signal ignored */
        long tid = -1;
        if (*p == ':') { p++; tid = strtol(p, (char **)&p, 16); }
        if ((act == 's' || act == 'S') && step_vcpu < 0) step_vcpu = tid > 0 ? (int)tid - 1 : dbg_selected_vcpu(m);
        while (*p && *p != ';') p++;
    }
    g->running = 1;
    if (step_vcpu >= 0) dbg_resume(m, DBG_ACT_STEP, 1, step_vcpu);
    else dbg_resume(m, DBG_ACT_CONTINUE, 0, -1);
}

static void handle_packet(machine_t *m, gdb_t *g, char *p, size_t plen) {
    char buf[NREGS * 34 + 16];
    cpu_t *c = dbg_vcpu(m, g->g_vcpu);
    switch (p[0]) {
    case '?': stop_reply(m, g, dbg_last_stop(m)); return;
    case 'g': {
        size_t o = 0;
        for (int r = 0; r < NREGS; r++) { reg_hex(c, r, buf + o); o += (size_t)reg_size(r) * 2; }
        buf[o] = 0;
        send_packet(g, buf);
        return; }
    case 'G': {
        const char *h = p + 1;
        for (int r = 0; r < NREGS && *h; r++) { reg_set(m, c, r, h); h += reg_size(r) * 2; }
        send_packet(g, "OK");
        return; }
    case 'p': {
        int r = (int)strtol(p + 1, NULL, 16);
        if (r < 0 || r >= NREGS) { send_packet(g, "E01"); return; }
        reg_hex(c, r, buf);
        send_packet(g, buf);
        return; }
    case 'P': {
        char *eq;
        int r = (int)strtol(p + 1, &eq, 16);
        if (r < 0 || r >= NREGS || *eq != '=') { send_packet(g, "E01"); return; }
        reg_set(m, c, r, eq + 1);
        send_packet(g, "OK");
        return; }
    case 'm': cmd_read_mem(m, g, p + 1); return;
    case 'M': cmd_write_mem(m, g, p + 1, 0, plen - 1); return;
    case 'X': cmd_write_mem(m, g, p + 1, 1, plen - 1); return;
    case 'c': case 's': {
        if (p[1]) { uint64_t a = strtoull(p + 1, NULL, 16); dbg_vcpu(m, dbg_selected_vcpu(m))->rip = a; }
        g->running = 1;
        if (p[0] == 's') dbg_resume(m, DBG_ACT_STEP, 1, g->g_vcpu);
        else dbg_resume(m, DBG_ACT_CONTINUE, 0, -1);
        return; }
    case 'Z': case 'z': cmd_breakpoint(m, g, p + 1, p[0] == 'Z'); return;
    case 'H': {
        long tid = strtol(p + 2, NULL, 16);
        int v = tid_to_vcpu(m, tid);
        if (v < 0) { send_packet(g, "E01"); return; }
        if (p[1] == 'g') g->g_vcpu = v;
        send_packet(g, "OK");
        return; }
    case 'T': {
        long tid = strtol(p + 1, NULL, 16);
        send_packet(g, (tid >= 1 && tid <= dbg_nvcpus(m)) ? "OK" : "E01");
        return; }
    case 'k':
        dbg_resume(m, DBG_ACT_QUIT, 0, -1);
        g->dead = 1; g->left = 1;
        return;
    case 'D':
        send_packet(g, "OK");
        dbg_resume(m, DBG_ACT_DETACH, 0, -1);
        g->dead = 1; g->left = 1;
        return;
    case 'v':
        if (!strncmp(p, "vCont?", 6)) { send_packet(g, "vCont;c;C;s;S"); return; }
        if (!strncmp(p, "vCont;", 6)) { cmd_vcont(m, g, p + 5); return; }
        if (!strncmp(p, "vKill", 5)) { send_packet(g, "OK"); dbg_resume(m, DBG_ACT_QUIT, 0, -1); g->dead = 1; g->left = 1; return; }
        send_packet(g, "");
        return;
    case 'q':
        if (!strncmp(p, "qSupported", 10)) {
            send_packet(g, "PacketSize=4000;qXfer:features:read+;QStartNoAckMode+;swbreak+;hwbreak+;vContSupported+");
            return;
        }
        if (!strncmp(p, "qXfer:features:read:target.xml:", 31)) {
            char *end;
            size_t off = (size_t)strtoull(p + 31, &end, 16);
            size_t len = (*end == ',') ? (size_t)strtoull(end + 1, NULL, 16) : 0;
            size_t total = sizeof target_xml - 1;
            if (off >= total) { send_packet(g, "l"); return; }
            if (len > 3900) len = 3900;
            size_t n = total - off < len ? total - off : len;
            char *pk = malloc(n + 2);
            if (!pk) { send_packet(g, "E01"); return; }
            pk[0] = (off + n >= total) ? 'l' : 'm';
            memcpy(pk + 1, target_xml + off, n);
            pk[n + 1] = 0;
            send_packet(g, pk);
            free(pk);
            return;
        }
        if (!strncmp(p, "qXfer:", 6)) { send_packet(g, ""); return; }
        if (!strcmp(p, "qfThreadInfo")) {
            size_t o = 0;
            o += (size_t)snprintf(buf + o, sizeof buf - o, "m");
            for (int i = 0; i < dbg_nvcpus(m); i++) o += (size_t)snprintf(buf + o, sizeof buf - o, "%s%x", i ? "," : "", i + 1);
            send_packet(g, buf);
            return;
        }
        if (!strcmp(p, "qsThreadInfo")) { send_packet(g, "l"); return; }
        if (!strcmp(p, "qC")) { snprintf(buf, sizeof buf, "QC%x", dbg_selected_vcpu(m) + 1); send_packet(g, buf); return; }
        if (!strncmp(p, "qThreadExtraInfo,", 17)) {
            int v = tid_to_vcpu(m, strtol(p + 17, NULL, 16));
            if (v < 0) { send_packet(g, "E01"); return; }
            cpu_t *vc = dbg_vcpu(m, v);
            char info[96];
            snprintf(info, sizeof info, "vcpu%d %s%s", v, cpu_mode_name(vc), vc->halted ? " halted" : "");
            size_t k = 0;
            for (const char *s = info; *s; s++) { buf[k++] = hexd[((uint8_t)*s) >> 4]; buf[k++] = hexd[((uint8_t)*s) & 15]; }
            buf[k] = 0;
            send_packet(g, buf);
            return;
        }
        if (!strcmp(p, "qAttached")) { send_packet(g, "1"); return; }
        if (!strcmp(p, "qOffsets")) { send_packet(g, "Text=0;Data=0;Bss=0"); return; }
        if (!strncmp(p, "qSymbol", 7)) { send_packet(g, "OK"); return; }
        if (!strncmp(p, "qRcmd,", 6)) { cmd_monitor(m, g, p + 6); return; }
        send_packet(g, "");
        return;
    case 'Q':
        if (!strcmp(p, "QStartNoAckMode")) { send_packet(g, "OK"); g->noack = 1; return; }
        send_packet(g, "");
        return;
    default:
        send_packet(g, "");
        return;
    }
}

/* consume complete packets from g->in */
static void process_input(machine_t *m, gdb_t *g, int running) {
    size_t i = 0;
    while (i < g->inlen) {
        char ch = g->in[i];
        if (ch == 0x03) { i++; if (running) dbg_request_pause(m); continue; }
        if (ch == '+') { i++; continue; }
        if (ch == '-') { i++; if (g->last) send_all(g, g->last, strlen(g->last)); continue; }
        if (ch != '$') { i++; continue; }
        char *hash = memchr(g->in + i, '#', g->inlen - i);
        if (!hash || (size_t)(hash - g->in) + 2 >= g->inlen) break;   /* incomplete */
        size_t start = i + 1, end = (size_t)(hash - g->in);
        unsigned sum = 0;
        for (size_t k = start; k < end; k++) sum += (unsigned char)g->in[k];
        int want = (hexval(hash[1]) << 4) | hexval(hash[2]);
        i = end + 3;
        if ((int)(sum & 0xFF) != want) { if (!g->noack) send_all(g, "-", 1); continue; }
        if (!g->noack) send_all(g, "+", 1);
        char pkt[16384];
        size_t plen = end - start;
        if (plen >= sizeof pkt) plen = sizeof pkt - 1;
        memcpy(pkt, g->in + start, plen);
        pkt[plen] = 0;
        if (running) continue;            /* only 0x03 is meaningful while running */
        handle_packet(m, g, pkt, plen);
        if (g->dead || g->running) { i = g->inlen; break; }
    }
    memmove(g->in, g->in + i, g->inlen - i);
    g->inlen -= i;
}

static int read_some(gdb_t *g, int nonblock) {
    if (g->inlen >= sizeof g->in) g->inlen = 0;
    ssize_t r = recv(g->fd, g->in + g->inlen, sizeof g->in - g->inlen, nonblock ? MSG_DONTWAIT : 0);
    if (r == 0) { g->dead = 1; return -1; }
    if (r < 0) { if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) return 0; g->dead = 1; return -1; }
    g->inlen += (size_t)r;
    return (int)r;
}

/* ------------------------------------------------------ front-end hooks --- */
static void gdb_on_stop(machine_t *m, dbg_frontend_t *fe, const dbg_stop_t *st) {
    gdb_t *g = fe->ctx;
    g->g_vcpu = st->vcpu;   /* the stopping vCPU is gdb's current thread */
    if (g->running) { g->running = 0; stop_reply(m, g, st); }
    if (st->reason == DBG_STOP_END) {
        /* the target is gone: gdb received W/X (or will, via '?') */
        g->dead = 1;
    }
}

static int gdb_stop_fd(machine_t *m, dbg_frontend_t *fe) {
    gdb_t *g = fe->ctx;
    (void)m;
    return g->dead ? -1 : g->fd;
}

static void gdb_finish(machine_t *m, gdb_t *g) {
    fprintf(stderr, "[gdb] connection closed\n");
    if (!g->left) dbg_resume(m, DBG_ACT_DETACH, 0, -1);   /* k keeps QUIT */
    dbg_remove_frontend(m, &g->fe);
}

static void gdb_service(machine_t *m, dbg_frontend_t *fe) {
    gdb_t *g = fe->ctx;
    if (!g->dead) {
        read_some(g, 1);
        process_input(m, g, 0);
    }
    if (g->dead) gdb_finish(m, g);
}

static void gdb_poll_running(machine_t *m, dbg_frontend_t *fe) {
    gdb_t *g = fe->ctx;
    if ((++g->poll_div & 63u) != 0) return;   /* every 64Ki steps */
    if (g->dead) return;
    if (read_some(g, 1) < 0) { fprintf(stderr, "[gdb] disconnected while running; continuing\n"); dbg_remove_frontend(m, fe); return; }
    if (g->inlen) process_input(m, g, 1);
}

static void gdb_detach_fe(machine_t *m, dbg_frontend_t *fe) {
    gdb_t *g = fe->ctx;
    (void)m;
    if (g->fd >= 0) close(g->fd);
    free(g->last);
    free(g);
}

int gdb_attach(machine_t *m, const char *spec) {
    char host[128] = "127.0.0.1";
    const char *colon = strrchr(spec, ':');
    int port;
    if (colon) {
        size_t hl = (size_t)(colon - spec);
        if (hl >= sizeof host) hl = sizeof host - 1;
        if (hl) { memcpy(host, spec, hl); host[hl] = 0; }
        port = atoi(colon + 1);
    } else port = atoi(spec);
    if (port <= 0 || port > 65535) { fprintf(stderr, "--gdb: bad port in '%s'\n", spec); return -1; }
    int ls = socket(AF_INET, SOCK_STREAM, 0);
    if (ls < 0) { perror("--gdb socket"); return -1; }
    int one = 1;
    setsockopt(ls, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    struct sockaddr_in sa;
    memset(&sa, 0, sizeof sa);
    sa.sin_family = AF_INET;
    sa.sin_port = htons((uint16_t)port);
    if (inet_pton(AF_INET, host, &sa.sin_addr) != 1) { fprintf(stderr, "--gdb: bad host '%s'\n", host); close(ls); return -1; }
    if (bind(ls, (struct sockaddr *)&sa, sizeof sa) < 0 || listen(ls, 1) < 0) {
        fprintf(stderr, "--gdb: cannot listen on %s:%d: %s\n", host, port, strerror(errno));
        close(ls);
        return -1;
    }
    fprintf(stderr, "[gdb] listening on %s:%d -- waiting for gdb (target remote %s:%d)\n", host, port, host, port);
    int fd;
    do { fd = accept(ls, NULL, NULL); } while (fd < 0 && errno == EINTR);
    close(ls);
    if (fd < 0) { perror("--gdb accept"); return -1; }
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
    fprintf(stderr, "[gdb] connected\n");
    gdb_t *g = calloc(1, sizeof *g);
    if (!g) { close(fd); return -1; }
    g->fd = fd;
    g->g_vcpu = 0;
    g->fe.name = "gdb";
    g->fe.ctx = g;
    g->fe.on_stop = gdb_on_stop;
    g->fe.stop_fd = gdb_stop_fd;
    g->fe.service = gdb_service;
    g->fe.poll_running = gdb_poll_running;
    g->fe.on_detach = gdb_detach_fe;
    g->fe.wants_end_stop = 1;
    if (dbg_add_frontend(m, &g->fe) != 0) { close(fd); free(g); return -1; }
    return 0;
}
