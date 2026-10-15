/* monitor.c -- TOOLING T5: single-threaded HTTP/1.1 + RFC 6455 monitor.
 *
 * The frontend is polled from dbg.c. While the guest runs every socket is
 * non-blocking; no thread calls cpu_step and all run control goes through
 * the T2 debugger core. The server binds loopback unless explicitly told
 * otherwise: this endpoint has no authentication. */
#define _POSIX_C_SOURCE 200809L
#include <arpa/inet.h>
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>
#include "dbg.h"
#include "devices.h"
#include "kbc.h"
#include "monitor.h"
#include "platform.h"
#include "ws.h"

#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0
#endif

#define MON_HTTP_MAX       8192u
#define MON_WS_MAX         (4u * 1024u * 1024u)
#define MON_INPUT_CAP      (MON_WS_MAX + MON_HTTP_MAX + 32u)
#define MON_OUTPUT_MAX     (8u * 1024u * 1024u)
#define MON_COMMAND_MAX    2048u
#define MON_TEXT_MAX       120u
#define MON_STATE_EVERY    64u          /* dbg poll = 1024 instruction attempts */
#define MON_HTML_MAX       (1024u * 1024u)

struct mon_out {
    uint8_t *data;
    size_t len, off;
    struct mon_out *next;
};

typedef struct monitor_state {
    dbg_frontend_t fe;
    int listen_fd;
    int client_fd;
    int websocket;
    int close_after_flush;
    int at_stop;
    int quit_pending;
    int need_state;
    int need_fb;
    int log_cursor_valid;
    int log_head;
    uint64_t poll_count;
    uint64_t state_serial;
    uint8_t input[MON_INPUT_CAP];
    size_t input_len;
    struct mon_out *out_head, *out_tail;
    size_t out_bytes;
    uint32_t *prev_pixels;
    size_t prev_pixel_count;
    int prev_width, prev_height, prev_valid;
    struct timespec last_activity;
} monitor_state_t;

typedef struct {
    char *data;
    size_t len, cap;
    int failed;
} strbuf_t;

static int set_nonblock(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0 || fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0) return -1;
    return 0;
}

static void sb_free(strbuf_t *b) {
    free(b->data);
    memset(b, 0, sizeof *b);
}

static int sb_reserve(strbuf_t *b, size_t add) {
    if (b->failed || add > MON_WS_MAX || b->len > MON_WS_MAX - add) {
        b->failed = 1;
        return -1;
    }
    size_t need = b->len + add + 1;
    if (need <= b->cap) return 0;
    size_t cap = b->cap ? b->cap : 256;
    while (cap < need) {
        if (cap > MON_WS_MAX / 2) { cap = MON_WS_MAX + 1; break; }
        cap *= 2;
    }
    if (cap > MON_WS_MAX + 1) { b->failed = 1; return -1; }
    char *p = realloc(b->data, cap);
    if (!p) { b->failed = 1; return -1; }
    b->data = p;
    b->cap = cap;
    return 0;
}

static void sb_mem(strbuf_t *b, const char *s, size_t n) {
    if (sb_reserve(b, n) != 0) return;
    if (n) memcpy(b->data + b->len, s, n);
    b->len += n;
    b->data[b->len] = 0;
}

static void sb_str(strbuf_t *b, const char *s) {
    if (s) sb_mem(b, s, strlen(s));
}

static void sb_printf(strbuf_t *b, const char *fmt, ...) {
    if (b->failed) return;
    va_list ap, cp;
    va_start(ap, fmt);
    va_copy(cp, ap);
    int n = vsnprintf(NULL, 0, fmt, cp);
    va_end(cp);
    if (n < 0 || sb_reserve(b, (size_t)n) != 0) {
        va_end(ap);
        b->failed = 1;
        return;
    }
    (void)vsnprintf(b->data + b->len, b->cap - b->len, fmt, ap);
    va_end(ap);
    b->len += (size_t)n;
}

static void sb_json(strbuf_t *b, const char *s) {
    static const char hex[] = "0123456789abcdef";
    sb_str(b, "\"");
    if (!s) s = "";
    for (const unsigned char *p = (const unsigned char *)s; *p && !b->failed; p++) {
        switch (*p) {
        case '"': sb_str(b, "\\\""); break;
        case '\\': sb_str(b, "\\\\"); break;
        case '\b': sb_str(b, "\\b"); break;
        case '\f': sb_str(b, "\\f"); break;
        case '\n': sb_str(b, "\\n"); break;
        case '\r': sb_str(b, "\\r"); break;
        case '\t': sb_str(b, "\\t"); break;
        default:
            if (*p < 0x20) {
                char esc[7] = {'\\', 'u', '0', '0', hex[*p >> 4], hex[*p & 15], 0};
                sb_str(b, esc);
            } else sb_mem(b, (const char *)p, 1);
            break;
        }
    }
    sb_str(b, "\"");
}

static void out_free_all(monitor_state_t *s) {
    struct mon_out *p = s->out_head;
    while (p) {
        struct mon_out *next = p->next;
        free(p->data);
        free(p);
        p = next;
    }
    s->out_head = s->out_tail = NULL;
    s->out_bytes = 0;
}

static int queue_take(monitor_state_t *s, uint8_t *data, size_t len) {
    if (s->client_fd < 0 || !data || len > MON_OUTPUT_MAX ||
        s->out_bytes > MON_OUTPUT_MAX - len) {
        free(data);
        return -1;
    }
    struct mon_out *p = calloc(1, sizeof *p);
    if (!p) { free(data); return -1; }
    p->data = data;
    p->len = len;
    if (s->out_tail) s->out_tail->next = p;
    else s->out_head = p;
    s->out_tail = p;
    s->out_bytes += len;
    return 0;
}

static int queue_raw(monitor_state_t *s, const void *data, size_t len) {
    uint8_t *copy = malloc(len ? len : 1);
    if (!copy) return -1;
    if (len) memcpy(copy, data, len);
    return queue_take(s, copy, len);
}

static int queue_ws(monitor_state_t *s, uint8_t opcode, const void *payload, size_t len) {
    if (s->close_after_flush || len > MON_WS_MAX || len > SIZE_MAX - 14) return -1;
    size_t cap = len + 14;
    uint8_t *frame = malloc(cap);
    if (!frame) return -1;
    size_t n = ws_frame_encode(frame, cap, opcode, 1, 0, NULL, payload, len);
    if (!n) { free(frame); return -1; }
    return queue_take(s, frame, n);
}

static void close_client(monitor_state_t *s) {
    if (s->client_fd >= 0) close(s->client_fd);
    s->client_fd = -1;
    s->websocket = 0;
    s->close_after_flush = 0;
    s->input_len = 0;
    s->need_state = 0;
    s->need_fb = 0;
    s->log_cursor_valid = 0;
    s->prev_valid = 0;
    out_free_all(s);
}

static void note_activity(monitor_state_t *s) {
    (void)clock_gettime(CLOCK_MONOTONIC, &s->last_activity);
}

static void expire_idle_http(monitor_state_t *s) {
    if (s->client_fd < 0 || s->websocket) return;
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) return;
    if (now.tv_sec - s->last_activity.tv_sec > 15) close_client(s);
}

static void flush_output(monitor_state_t *s) {
    while (s->client_fd >= 0 && s->out_head) {
        struct mon_out *p = s->out_head;
        ssize_t n = send(s->client_fd, p->data + p->off, p->len - p->off,
                         MSG_DONTWAIT | MSG_NOSIGNAL);
        if (n > 0) {
            p->off += (size_t)n;
            s->out_bytes -= (size_t)n;
            note_activity(s);
            if (p->off < p->len) return;
            s->out_head = p->next;
            if (!s->out_head) s->out_tail = NULL;
            free(p->data);
            free(p);
            if (s->close_after_flush && !s->out_head) {
                close_client(s);
                return;
            }
            continue;
        }
        if (n < 0 && (errno == EINTR)) continue;
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) return;
        close_client(s);
        return;
    }
}

static int accept_client(monitor_state_t *s) {
    if (s->client_fd >= 0) return 0;
    int fd = accept(s->listen_fd, NULL, NULL);
    if (fd < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) return 0;
        return -1;
    }
    if (set_nonblock(fd) != 0) { close(fd); return -1; }
    s->client_fd = fd;
    s->websocket = 0;
    s->close_after_flush = 0;
    s->input_len = 0;
    s->need_state = 0;
    s->need_fb = 0;
    s->prev_valid = 0;
    s->log_cursor_valid = 0;
    note_activity(s);
    return 1;
}

static const char *stop_name(dbg_stop_reason_t r) {
    switch (r) {
    case DBG_STOP_START: return "start";
    case DBG_STOP_BREAK: return "breakpoint";
    case DBG_STOP_STEP: return "step";
    case DBG_STOP_WATCH: return "watchpoint";
    case DBG_STOP_PAUSE: return "pause";
    case DBG_STOP_UNTIL: return "until";
    case DBG_STOP_END: return "end";
    default: return "none";
    }
}

static const char *vcpu_state_name(machine_t *m, int v) {
    if (m->n_vcpus <= 1) return "run";
    if (m->vcpu[v].state == VCPU_RUN) return "run";
    if (m->vcpu[v].state == VCPU_WAIT_SIPI) return "wait-sipi";
    return "off";
}

static void append_regs_json(strbuf_t *b, const cpu_t *c) {
    static const char *const name[16] = {
        "rax", "rcx", "rdx", "rbx", "rsp", "rbp", "rsi", "rdi",
        "r8", "r9", "r10", "r11", "r12", "r13", "r14", "r15"
    };
    sb_str(b, "{\"rip\":");
    sb_printf(b, "\"0x%016llx\"", (unsigned long long)c->rip);
    for (int i = 0; i < 16; i++) {
        sb_str(b, ",\""); sb_str(b, name[i]); sb_str(b, "\":");
        sb_printf(b, "\"0x%016llx\"", (unsigned long long)c->gpr[i]);
    }
    sb_printf(b, ",\"rflags\":\"0x%016llx\",\"cr0\":\"0x%016llx\","
              "\"cr2\":\"0x%016llx\",\"cr3\":\"0x%016llx\","
              "\"cr4\":\"0x%016llx\",\"efer\":\"0x%016llx\","
              "\"mxcsr\":\"0x%08x\"}",
              (unsigned long long)c->rflags, (unsigned long long)c->cr0,
              (unsigned long long)c->cr2, (unsigned long long)c->cr3,
              (unsigned long long)c->cr4, (unsigned long long)c->efer, c->mxcsr);
}

static int build_state(monitor_state_t *s, machine_t *m, strbuf_t *b,
                       int *new_log_head) {
    int nv = dbg_nvcpus(m);
    if (nv > EM_MAX_VCPU) nv = EM_MAX_VCPU;
    int selected = s->at_stop ? dbg_selected_vcpu(m) : 0;
    if (selected < 0 || selected >= nv) selected = 0;
    cpu_t *c = dbg_vcpu(m, selected);
    const dbg_stop_t *stop = dbg_last_stop(m);
    dbg_stop_reason_t reason = s->at_stop && stop ? stop->reason : DBG_STOP_NONE;
    uint64_t lin = c->seg[SEG_CS].base + c->rip;
    char dis[160] = "<unreadable>";
    int dis_len = 0;
    (void)dbg_disas_at(m, c, lin, dbg_code_bits(c), c->rip, dis, sizeof dis, &dis_len);
    (void)dis_len;

    int head = m->log.head;
    if (!s->log_cursor_valid) {
        int keep = m->log.count < 64 ? m->log.count : 64;
        s->log_head = (head - keep + LOG_RING) % LOG_RING;
    }
    int delta = (head - s->log_head + LOG_RING) % LOG_RING;
    if (delta > LOG_RING) delta = LOG_RING;

    sb_str(b, "{\"kind\":\"state\",\"version\":1,\"serial\":");
    sb_printf(b, "%llu,\"platform\":", (unsigned long long)++s->state_serial);
    sb_json(b, m->plat ? m->plat->name : "unknown");
    sb_str(b, ",\"mode\":");
    sb_json(b, cpu_mode_name(c));
    sb_printf(b, ",\"vtime\":\"%llu\",\"executed\":\"%llu\",\"selected_vcpu\":%d,"
              "\"stop_reason\":",
              (unsigned long long)m->vtime_instr,
              (unsigned long long)machine_executed_total(m), selected);
    sb_json(b, s->at_stop ? stop_name(reason) : "running");
    sb_str(b, ",\"stop_vcpu\":");
    sb_printf(b, "%d,\"stop_lin\":\"0x%016llx\",\"disassembly\":",
              stop ? stop->vcpu : selected,
              (unsigned long long)(s->at_stop && stop ? stop->lin : lin));
    sb_json(b, dis);
    sb_str(b, ",\"registers\":");
    append_regs_json(b, c);
    sb_str(b, ",\"vcpus\":[");
    for (int i = 0; i < nv; i++) {
        cpu_t *vc = dbg_vcpu(m, i);
        if (i) sb_str(b, ",");
        sb_printf(b, "{\"id\":%d,\"state\":", i);
        sb_json(b, vcpu_state_name(m, i));
        sb_str(b, ",\"mode\":"); sb_json(b, cpu_mode_name(vc));
        sb_printf(b, ",\"rip\":\"0x%016llx\",\"executed_steps\":\"%llu\",\"halted\":%s}",
                  (unsigned long long)vc->rip,
                  (unsigned long long)vc->executed_steps, vc->halted ? "true" : "false");
    }
    sb_printf(b, "],\"framebuffer\":{\"width\":%d,\"height\":%d},\"logs\":[",
              m->fb_w, m->fb_h);
    for (int i = 0; i < delta; i++) {
        int idx = (s->log_head + i) % LOG_RING;
        if (i) sb_str(b, ",");
        sb_json(b, m->log.lines[idx]);
    }
    sb_str(b, "]}");
    if (b->failed) return -1;
    *new_log_head = head;
    return 0;
}

static int publish_state(monitor_state_t *s, machine_t *m) {
    strbuf_t b = {0};
    int log_head = s->log_head;
    if (build_state(s, m, &b, &log_head) != 0 || queue_ws(s, 1, b.data, b.len) != 0) {
        sb_free(&b);
        return -1;
    }
    s->log_head = log_head;
    s->log_cursor_valid = 1;
    s->need_state = 0;
    sb_free(&b);
    return 0;
}

static void put16le(uint8_t *p, uint16_t v) {
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8);
}

static int publish_framebuffer(monitor_state_t *s, machine_t *m) {
    int w = m->fb_w, h = m->fb_h;
    if (w <= 0 || h <= 0 || w > 4096 || h > 4096 || (size_t)w > SIZE_MAX / (size_t)h)
        return -1;
    size_t count = (size_t)w * (size_t)h;
    if (count > 16u * 1024u * 1024u) return -1;
    uint32_t *pixels = malloc(count * sizeof *pixels);
    if (!pixels) return -1;
    vga_render_display(m, pixels, w, h); /* side-effect-free display snapshot */
    int keyframe = !s->prev_valid || s->prev_width != w || s->prev_height != h ||
                   s->prev_pixel_count != count;
    unsigned rows = 0;
    for (int y = 0; y < h; y++) {
        const uint32_t *row = pixels + (size_t)y * (size_t)w;
        const uint32_t *old = keyframe ? NULL : s->prev_pixels + (size_t)y * (size_t)w;
        if (keyframe || memcmp(row, old, (size_t)w * sizeof *row) != 0) rows++;
    }
    if (!rows) {
        s->need_fb = 0;
        free(pixels);
        return 0;
    }
    size_t row_bytes = 2u + (size_t)w * 3u;
    if ((size_t)rows > (SIZE_MAX - 10u) / row_bytes) { free(pixels); return -1; }
    size_t payload_len = 10u + (size_t)rows * row_bytes;
    uint8_t *payload = malloc(payload_len);
    if (!payload) { free(pixels); return -1; }
    payload[0] = 'F'; payload[1] = 'B'; payload[2] = 1;
    payload[3] = (uint8_t)(keyframe ? 1 : 0);
    put16le(payload + 4, (uint16_t)w);
    put16le(payload + 6, (uint16_t)h);
    put16le(payload + 8, (uint16_t)rows);
    size_t off = 10;
    for (int y = 0; y < h; y++) {
        const uint32_t *row = pixels + (size_t)y * (size_t)w;
        const uint32_t *old = keyframe ? NULL : s->prev_pixels + (size_t)y * (size_t)w;
        if (!keyframe && memcmp(row, old, (size_t)w * sizeof *row) == 0) continue;
        put16le(payload + off, (uint16_t)y); off += 2;
        for (int x = 0; x < w; x++) {
            uint32_t p = row[x];
            payload[off++] = (uint8_t)(p >> 16);
            payload[off++] = (uint8_t)(p >> 8);
            payload[off++] = (uint8_t)p;
        }
    }
    if (queue_ws(s, 2, payload, payload_len) != 0) {
        free(payload); free(pixels); return -1;
    }
    if (!s->prev_pixels || s->prev_pixel_count != count) {
        uint32_t *prev = realloc(s->prev_pixels, count * sizeof *prev);
        if (!prev) {
            /* The delta was queued, but without a baseline a future frame
             * must be a keyframe rather than a corrupt delta. */
            s->prev_valid = 0;
            s->need_fb = 1;
            free(payload); free(pixels);
            return 0;
        }
        s->prev_pixels = prev;
    }
    memcpy(s->prev_pixels, pixels, count * sizeof *pixels);
    s->prev_pixel_count = count;
    s->prev_width = w; s->prev_height = h; s->prev_valid = 1;
    s->need_fb = 0;
    free(payload); free(pixels);
    return 0;
}

static void publish_pending(monitor_state_t *s, machine_t *m) {
    if (!s->websocket || s->client_fd < 0 || s->close_after_flush) return;
    if (s->need_state && s->out_bytes < MON_OUTPUT_MAX / 2) (void)publish_state(s, m);
    if (s->need_fb && s->out_bytes < MON_OUTPUT_MAX / 2) (void)publish_framebuffer(s, m);
}

static void send_reply(monitor_state_t *s, const char *cmd, int ok,
                       const char *message, const char *output) {
    strbuf_t b = {0};
    sb_str(&b, "{\"kind\":\"reply\",\"ok\":");
    sb_str(&b, ok ? "true" : "false");
    sb_str(&b, ",\"cmd\":"); sb_json(&b, cmd);
    sb_str(&b, ",\"message\":"); sb_json(&b, message);
    if (output) { sb_str(&b, ",\"output\":"); sb_json(&b, output); }
    sb_str(&b, "}");
    if (!b.failed) (void)queue_ws(s, 1, b.data, b.len);
    sb_free(&b);
}

static int hex_value(int c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static void skip_ws(const uint8_t *p, size_t n, size_t *i) {
    while (*i < n && (p[*i] == ' ' || p[*i] == '\t' || p[*i] == '\r' || p[*i] == '\n')) (*i)++;
}

static int parse_json_string(const uint8_t *p, size_t n, size_t *i,
                             char *out, size_t cap) {
    if (*i >= n || p[*i] != '"' || cap == 0) return -1;
    (*i)++;
    size_t o = 0;
    while (*i < n) {
        unsigned char c = p[(*i)++];
        if (c == '"') { out[o] = 0; return 0; }
        if (c < 0x20) return -1;
        if (c == '\\') {
            if (*i >= n) return -1;
            c = p[(*i)++];
            switch (c) {
            case '"': case '\\': case '/': break;
            case 'b': c = '\b'; break;
            case 'f': c = '\f'; break;
            case 'n': c = '\n'; break;
            case 'r': c = '\r'; break;
            case 't': c = '\t'; break;
            case 'u': {
                if (n - *i < 4) return -1;
                unsigned code = 0;
                for (int k = 0; k < 4; k++) {
                    int v = hex_value(p[*i + (size_t)k]);
                    if (v < 0) return -1;
                    code = (code << 4) | (unsigned)v;
                }
                *i += 4;
                if (code >= 0xD800 && code <= 0xDFFF) return -1;
                if (code < 0x80) {
                    if (o + 1 >= cap) return -1;
                    out[o++] = (char)code;
                    continue;
                } else if (code < 0x800) {
                    if (o + 2 >= cap) return -1;
                    out[o++] = (char)(0xC0 | (code >> 6));
                    out[o++] = (char)(0x80 | (code & 0x3F));
                    continue;
                } else {
                    if (o + 3 >= cap) return -1;
                    out[o++] = (char)(0xE0 | (code >> 12));
                    out[o++] = (char)(0x80 | ((code >> 6) & 0x3F));
                    out[o++] = (char)(0x80 | (code & 0x3F));
                    continue;
                }
            }
            default: return -1;
            }
        }
        if (o + 1 >= cap) return -1;
        out[o++] = (char)c;
    }
    return -1;
}

static int skip_json_value(const uint8_t *p, size_t n, size_t *i) {
    skip_ws(p, n, i);
    if (*i >= n) return -1;
    if (p[*i] == '"') {
        char discard[2];
        /* Skip arbitrary long strings without copying. */
        (*i)++;
        while (*i < n) {
            uint8_t c = p[(*i)++];
            if (c == '"') return 0;
            if (c == '\\') { if (*i >= n) return -1; (*i)++; }
            else if (c < 0x20) return -1;
        }
        (void)discard;
        return -1;
    }
    if (p[*i] == '{' || p[*i] == '[') {
        uint8_t open = p[(*i)++];
        int depth = 1, quoted = 0, escape = 0;
        while (*i < n && depth) {
            uint8_t c = p[(*i)++];
            if (quoted) {
                if (escape) escape = 0;
                else if (c == '\\') escape = 1;
                else if (c == '"') quoted = 0;
            } else if (c == '"') quoted = 1;
            else if (c == '{' || c == '[') depth++;
            else if (c == '}' || c == ']') depth--;
        }
        (void)open;
        return depth == 0 ? 0 : -1;
    }
    while (*i < n && p[*i] != ',' && p[*i] != '}' && p[*i] != ']' &&
           p[*i] != ' ' && p[*i] != '\r' && p[*i] != '\n' && p[*i] != '\t') (*i)++;
    return 0;
}

static int json_get_string(const uint8_t *p, size_t n, const char *key,
                           char *out, size_t cap) {
    size_t i = 0;
    skip_ws(p, n, &i);
    if (i >= n || p[i++] != '{') return -1;
    for (;;) {
        skip_ws(p, n, &i);
        if (i < n && p[i] == '}') return 0;
        char k[64];
        if (parse_json_string(p, n, &i, k, sizeof k) != 0) return -1;
        skip_ws(p, n, &i);
        if (i >= n || p[i++] != ':') return -1;
        skip_ws(p, n, &i);
        if (strcmp(k, key) == 0) {
            if (i >= n || p[i] != '"') return 0;
            return parse_json_string(p, n, &i, out, cap) == 0 ? 1 : -1;
        }
        if (skip_json_value(p, n, &i) != 0) return -1;
        skip_ws(p, n, &i);
        if (i < n && p[i] == ',') { i++; continue; }
        if (i < n && p[i] == '}') return 0;
        return -1;
    }
}

static size_t kbc_queue_free(const kbc_t *k) {
    return (size_t)((k->q_head + KBC_QUEUE - k->q_tail - 1u) % KBC_QUEUE);
}

static int inject_codes(machine_t *m, const char *text) {
    uint8_t codes[MON_COMMAND_MAX / 2];
    size_t count = 0;
    const char *p = text;
    while (*p) {
        while (*p == ',' || isspace((unsigned char)*p)) p++;
        if (!*p) break;
        char *end = NULL;
        long v = strtol(p, &end, 16);
        if (end == p || v < 0 || v > 0xFF ||
            (*end && *end != ',' && !isspace((unsigned char)*end)) ||
            count >= sizeof codes) return -1;
        codes[count++] = (uint8_t)v;
        p = end;
    }
    if (count == 0) return -1;
    if (count > kbc_queue_free(&m->kbc)) return -2;
    for (size_t i = 0; i < count; i++) {
        if (kbc_inject_scancode(m, codes[i]) < 0) return -2;
    }
    return (int)count;
}

static int scancode_for_ascii(unsigned char c, uint8_t *code, int *shift) {
    static const uint8_t alpha[26] = {
        0x1E, 0x30, 0x2E, 0x20, 0x12, 0x21, 0x22, 0x23, 0x17, 0x24,
        0x25, 0x26, 0x32, 0x31, 0x18, 0x19, 0x10, 0x13, 0x1F, 0x14,
        0x16, 0x2F, 0x11, 0x2D, 0x15, 0x2C
    };
    *shift = 0;
    if (c >= 'a' && c <= 'z') { *code = alpha[c - 'a']; return 0; }
    if (c >= 'A' && c <= 'Z') { *code = alpha[c - 'A']; *shift = 1; return 0; }
    if (c >= '1' && c <= '9') { *code = (uint8_t)(0x02 + c - '1'); return 0; }
    if (c == '0') { *code = 0x0B; return 0; }
    switch (c) {
    case '!': *code = 0x02; *shift = 1; return 0;
    case '@': *code = 0x03; *shift = 1; return 0;
    case '#': *code = 0x04; *shift = 1; return 0;
    case '$': *code = 0x05; *shift = 1; return 0;
    case '%': *code = 0x06; *shift = 1; return 0;
    case '^': *code = 0x07; *shift = 1; return 0;
    case '&': *code = 0x08; *shift = 1; return 0;
    case '*': *code = 0x09; *shift = 1; return 0;
    case '(': *code = 0x0A; *shift = 1; return 0;
    case ')': *code = 0x0B; *shift = 1; return 0;
    case ' ': *code = 0x39; return 0;
    case '\n': case '\r': *code = 0x1C; return 0;
    case '\t': *code = 0x0F; return 0;
    case '\b': *code = 0x0E; return 0;
    case '-': *code = 0x0C; return 0; case '_': *code = 0x0C; *shift = 1; return 0;
    case '=': *code = 0x0D; return 0; case '+': *code = 0x0D; *shift = 1; return 0;
    case '[': *code = 0x1A; return 0; case '{': *code = 0x1A; *shift = 1; return 0;
    case ']': *code = 0x1B; return 0; case '}': *code = 0x1B; *shift = 1; return 0;
    case '\\': *code = 0x2B; return 0; case '|': *code = 0x2B; *shift = 1; return 0;
    case ';': *code = 0x27; return 0; case ':': *code = 0x27; *shift = 1; return 0;
    case '\'': *code = 0x28; return 0; case '"': *code = 0x28; *shift = 1; return 0;
    case '`': *code = 0x29; return 0; case '~': *code = 0x29; *shift = 1; return 0;
    case ',': *code = 0x33; return 0; case '<': *code = 0x33; *shift = 1; return 0;
    case '.': *code = 0x34; return 0; case '>': *code = 0x34; *shift = 1; return 0;
    case '/': *code = 0x35; return 0; case '?': *code = 0x35; *shift = 1; return 0;
    default: return -1;
    }
}

static int inject_text(machine_t *m, const char *text) {
    size_t chars = strlen(text);
    if (chars > MON_TEXT_MAX) return -1;
    uint8_t codes[KBC_QUEUE];
    size_t n = 0;
    for (size_t i = 0; i < chars; i++) {
        uint8_t code; int shift;
        if (scancode_for_ascii((unsigned char)text[i], &code, &shift) != 0) return -1;
        size_t need = shift ? 4 : 2;
        if (n + need > sizeof codes) return -1;
        if (shift) codes[n++] = 0x2A;
        codes[n++] = code;
        codes[n++] = (uint8_t)(code | 0x80u);
        if (shift) codes[n++] = 0xAA;
    }
    if (n > kbc_queue_free(&m->kbc)) return -2;
    for (size_t i = 0; i < n; i++) {
        if (kbc_inject_scancode(m, codes[i]) < 0) return -2;
    }
    return (int)n;
}

static int command_is_readonly(const char *line) {
    while (isspace((unsigned char)*line)) line++;
    char word[32]; size_t n = 0;
    while (*line && !isspace((unsigned char)*line) && n + 1 < sizeof word)
        word[n++] = (char)tolower((unsigned char)*line++);
    word[n] = 0;
    if (!strcmp(word, "help") || !strcmp(word, "h") || !strcmp(word, "?")) return 1;
    if (!strcmp(word, "r") || !strcmp(word, "regs") || !strcmp(word, "registers") ||
        !strcmp(word, "where") || !strcmp(word, "info") || !strcmp(word, "p") ||
        !strcmp(word, "print") || !strcmp(word, "sym") || !strcmp(word, "xlate") ||
        !strcmp(word, "bt") || !strcmp(word, "backtrace") || !strcmp(word, "log")) return 1;
    if (word[0] == 'x' && (word[1] == 0 || word[1] == '/')) return 1;
    if (!strncmp(word, "p/", 2) || !strncmp(word, "print/", 6)) return 1;
    return 0;
}

static int monitor_is_end_stop(machine_t *m) {
    const dbg_stop_t *st = dbg_last_stop(m);
    return st && st->reason == DBG_STOP_END;
}

static void handle_command(monitor_state_t *s, machine_t *m,
                           const uint8_t *payload, size_t len) {
    char cmd[32] = "", line[MON_COMMAND_MAX] = "", addr[128] = "";
    char codes[MON_COMMAND_MAX] = "", text[MON_COMMAND_MAX] = "";
    int got = json_get_string(payload, len, "cmd", cmd, sizeof cmd);
    if (got != 1 || !cmd[0]) { send_reply(s, "", 0, "missing or invalid cmd", NULL); return; }
    int gdb = dbg_has_frontend(m, "gdb");

    if (!strcmp(cmd, "pause")) {
        if (gdb) { send_reply(s, cmd, 0, "read-only while GDB is attached", NULL); return; }
        if (s->at_stop) { send_reply(s, cmd, 1, "already stopped", NULL); return; }
        dbg_request_pause(m);
        send_reply(s, cmd, 1, "pause requested", NULL);
        return;
    }
    if (!strcmp(cmd, "continue")) {
        if (gdb) { send_reply(s, cmd, 0, "read-only while GDB is attached", NULL); return; }
        if (!s->at_stop) { send_reply(s, cmd, 0, "machine is already running", NULL); return; }
        if (monitor_is_end_stop(m)) { send_reply(s, cmd, 0, "run has ended", NULL); return; }
        s->at_stop = 0;
        s->need_state = 1;
        dbg_resume(m, DBG_ACT_CONTINUE, 0, -1);
        send_reply(s, cmd, 1, "continued", NULL);
        return;
    }
    if (!strcmp(cmd, "step")) {
        if (gdb) { send_reply(s, cmd, 0, "read-only while GDB is attached", NULL); return; }
        if (!s->at_stop || monitor_is_end_stop(m)) {
            send_reply(s, cmd, 0, "step requires a live stopped machine", NULL); return;
        }
        int v = dbg_selected_vcpu(m);
        s->at_stop = 0;
        s->need_state = 1;
        dbg_resume(m, DBG_ACT_STEP, 1, v);
        send_reply(s, cmd, 1, "single-step requested", NULL);
        return;
    }
    if (!strcmp(cmd, "break")) {
        if (gdb) { send_reply(s, cmd, 0, "read-only while GDB is attached", NULL); return; }
        if (json_get_string(payload, len, "addr", addr, sizeof addr) != 1 || !addr[0]) {
            send_reply(s, cmd, 0, "break needs an addr string", NULL); return;
        }
        uint64_t value = 0; const char *err = NULL;
        if (!dbg_eval(m, addr, &value, &err)) {
            send_reply(s, cmd, 0, err ? err : "invalid address", NULL); return;
        }
        int id = dbg_break_add(m, value, 0);
        if (id < 0) { send_reply(s, cmd, 0, "breakpoint table is full", NULL); return; }
        char message[160];
        snprintf(message, sizeof message, "breakpoint %d at 0x%016llx", id,
                 (unsigned long long)value);
        send_reply(s, cmd, 1, message, NULL);
        return;
    }
    if (!strcmp(cmd, "exec")) {
        if (json_get_string(payload, len, "line", line, sizeof line) != 1 || !line[0]) {
            send_reply(s, cmd, 0, "exec needs a line string", NULL); return;
        }
        if ((gdb || !s->at_stop) && !command_is_readonly(line)) {
            send_reply(s, cmd, 0, gdb ? "read-only while GDB is attached" :
                       "only inspection commands are allowed while running", NULL);
            return;
        }
        char *output = NULL; size_t out_len = 0;
        FILE *stream = open_memstream(&output, &out_len);
        if (!stream) { send_reply(s, cmd, 0, "cannot allocate command output", NULL); return; }
        int action = dbg_exec(m, line, stream);
        if (fclose(stream) != 0) { free(output); send_reply(s, cmd, 0, "command output failed", NULL); return; }
        if (out_len > 16384) { output[16384] = 0; out_len = 16384; }
        (void)out_len;
        if (action != DBG_ACT_NONE) {
            if (!s->at_stop || gdb) {
                send_reply(s, cmd, 0, "run control is unavailable here", output ? output : "");
                free(output);
                return;
            }
            if (action == DBG_ACT_DETACH) {
                send_reply(s, cmd, 0, "use the WebSocket close button to detach", output ? output : "");
                free(output);
                return;
            }
            s->at_stop = 0;
            s->need_state = 1;
            dbg_resume(m, (dbg_action_t)action, 0, -1);
        }
        send_reply(s, cmd, 1, "command executed", output ? output : "");
        free(output);
        s->need_state = 1;
        return;
    }
    if (!strcmp(cmd, "keys")) {
        if (json_get_string(payload, len, "codes", codes, sizeof codes) != 1) {
            send_reply(s, cmd, 0, "keys needs a comma-separated codes string", NULL); return;
        }
        int n = inject_codes(m, codes);
        if (n < 0) send_reply(s, cmd, 0, n == -2 ? "keyboard queue is full" : "invalid scancode list", NULL);
        else { char message[80]; snprintf(message, sizeof message, "queued %d scancode byte(s)", n); send_reply(s, cmd, 1, message, NULL); }
        return;
    }
    if (!strcmp(cmd, "type")) {
        if (json_get_string(payload, len, "text", text, sizeof text) != 1) {
            send_reply(s, cmd, 0, "type needs a text string", NULL); return;
        }
        int n = inject_text(m, text);
        if (n < 0) send_reply(s, cmd, 0, n == -2 ? "keyboard queue is full" :
                              "type accepts up to 120 US-ASCII characters", NULL);
        else { char message[80]; snprintf(message, sizeof message, "queued %d scancode byte(s)", n); send_reply(s, cmd, 1, message, NULL); }
        return;
    }
    if (!strcmp(cmd, "quit")) {
        if (gdb) { send_reply(s, cmd, 0, "read-only while GDB is attached", NULL); return; }
        send_reply(s, cmd, 1, "quitting emulator", NULL);
        if (s->at_stop) {
            s->at_stop = 0;
            dbg_resume(m, DBG_ACT_QUIT, 0, -1);
        } else {
            s->quit_pending = 1;
            dbg_request_pause(m);
        }
        return;
    }
    send_reply(s, cmd, 0, "unknown command", NULL);
}

static void protocol_close(monitor_state_t *s, uint16_t code) {
    uint8_t payload[2] = { (uint8_t)(code >> 8), (uint8_t)code };
    if (s->websocket) (void)queue_ws(s, 8, payload, sizeof payload);
    s->close_after_flush = 1;
}

static void process_ws_frames(monitor_state_t *s, machine_t *m) {
    size_t off = 0;
    while (off < s->input_len && s->client_fd >= 0 && !s->close_after_flush) {
        ws_frame_header_t h;
        int rc = ws_frame_parse_header(s->input + off, s->input_len - off, &h);
        if (rc == 0) break;
        if (rc < 0 || !h.masked || !h.fin) { protocol_close(s, 1002); break; }
        if (h.payload_len > MON_WS_MAX) { protocol_close(s, 1009); break; }
        if (h.header_len > SIZE_MAX - (size_t)h.payload_len) { protocol_close(s, 1009); break; }
        size_t total = h.header_len + (size_t)h.payload_len;
        if (s->input_len - off < total) break;
        uint8_t *payload = s->input + off + h.header_len;
        ws_frame_apply_mask(payload, (size_t)h.payload_len, h.mask, 0);
        if (h.opcode == 1) {
            if (h.payload_len > MON_COMMAND_MAX) {
                send_reply(s, "", 0, "text command is too long", NULL);
            } else {
                handle_command(s, m, payload, (size_t)h.payload_len);
            }
        } else if (h.opcode == 8) {
            if (h.payload_len >= 2) (void)queue_ws(s, 8, payload, (size_t)h.payload_len);
            else (void)queue_ws(s, 8, NULL, 0);
            s->close_after_flush = 1;
        } else if (h.opcode == 9) {
            (void)queue_ws(s, 10, payload, (size_t)h.payload_len);
        } else if (h.opcode == 10) {
            /* Pong: no state transition. */
        } else {
            protocol_close(s, 1003); /* binary commands are not supported */
            break;
        }
        off += total;
    }
    if (off) {
        memmove(s->input, s->input + off, s->input_len - off);
        s->input_len -= off;
    }
}

static int find_header_end(const uint8_t *p, size_t n, size_t *where) {
    if (n < 4) return 0;
    for (size_t i = 0; i + 3 < n; i++) {
        if (p[i] == '\r' && p[i + 1] == '\n' && p[i + 2] == '\r' && p[i + 3] == '\n') {
            *where = i;
            return 1;
        }
    }
    return 0;
}

static int http_header(const char *req, const char *name, char *out, size_t cap) {
    const char *p = strstr(req, "\r\n");
    if (!p) return 0;
    p += 2;
    size_t want = strlen(name);
    while (*p && !(p[0] == '\r' && p[1] == '\n')) {
        const char *end = strstr(p, "\r\n");
        if (!end) return 0;
        const char *colon = memchr(p, ':', (size_t)(end - p));
        if (colon && (size_t)(colon - p) == want && strncasecmp(p, name, want) == 0) {
            const char *v = colon + 1;
            while (v < end && (*v == ' ' || *v == '\t')) v++;
            while (end > v && (end[-1] == ' ' || end[-1] == '\t')) end--;
            size_t n = (size_t)(end - v);
            if (n + 1 > cap) return -1;
            memcpy(out, v, n); out[n] = 0;
            return 1;
        }
        p = end + 2;
    }
    return 0;
}

static int contains_token_ci(const char *list, const char *token) {
    size_t n = strlen(token);
    const char *p = list;
    while (*p) {
        while (*p == ',' || isspace((unsigned char)*p)) p++;
        const char *q = p;
        while (*q && *q != ',' && !isspace((unsigned char)*q)) q++;
        if ((size_t)(q - p) == n && strncasecmp(p, token, n) == 0) return 1;
        p = q;
    }
    return 0;
}

static int http_reply(monitor_state_t *s, int status, const char *reason,
                      const char *content_type, const void *body, size_t body_len) {
    char head[512];
    int n = snprintf(head, sizeof head,
                     "HTTP/1.1 %d %s\r\nContent-Type: %s\r\nContent-Length: %zu\r\n"
                     "Cache-Control: no-store\r\nConnection: close\r\n\r\n",
                     status, reason, content_type, body_len);
    if (n < 0 || (size_t)n >= sizeof head || body_len > SIZE_MAX - (size_t)n) return -1;
    size_t total = (size_t)n + body_len;
    uint8_t *buf = malloc(total ? total : 1);
    if (!buf) return -1;
    memcpy(buf, head, (size_t)n);
    if (body_len) memcpy(buf + n, body, body_len);
    if (queue_take(s, buf, total) != 0) return -1;
    s->close_after_flush = 1;
    return 0;
}

static int serve_html(monitor_state_t *s) {
    FILE *f = fopen("web/monitor.html", "rb");
    if (!f) {
        static const char err[] = "web/monitor.html not found; run x86emu from the project root\n";
        return http_reply(s, 500, "Internal Server Error", "text/plain; charset=utf-8", err, sizeof err - 1);
    }
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return -1; }
    long size = ftell(f);
    if (size < 0 || (unsigned long)size > MON_HTML_MAX || fseek(f, 0, SEEK_SET) != 0) {
        fclose(f); return -1;
    }
    uint8_t *body = malloc(size ? (size_t)size : 1);
    if (!body) { fclose(f); return -1; }
    size_t n = fread(body, 1, (size_t)size, f);
    fclose(f);
    if (n != (size_t)size) { free(body); return -1; }
    int rc = http_reply(s, 200, "OK", "text/html; charset=utf-8", body, n);
    free(body);
    return rc;
}

static int do_websocket_handshake(monitor_state_t *s, const char *key) {
    char accept[32], response[512];
    if (ws_accept_key(key, accept, sizeof accept) < 0) return -1;
    int n = snprintf(response, sizeof response,
                     "HTTP/1.1 101 Switching Protocols\r\n"
                     "Upgrade: websocket\r\nConnection: Upgrade\r\n"
                     "Sec-WebSocket-Accept: %s\r\n\r\n", accept);
    if (n < 0 || (size_t)n >= sizeof response || queue_raw(s, response, (size_t)n) != 0) return -1;
    s->websocket = 1;
    s->close_after_flush = 0;
    s->need_state = 1;
    s->need_fb = 1;
    s->prev_valid = 0;
    s->log_cursor_valid = 0;
    return 0;
}

static void process_http(monitor_state_t *s) {
    size_t end;
    if (!find_header_end(s->input, s->input_len, &end)) {
        if (s->input_len >= MON_HTTP_MAX) {
            static const char msg[] = "HTTP header too large\n";
            (void)http_reply(s, 431, "Request Header Fields Too Large", "text/plain", msg, sizeof msg - 1);
        }
        return;
    }
    if (end >= MON_HTTP_MAX) {
        static const char msg[] = "HTTP header too large\n";
        (void)http_reply(s, 431, "Request Header Fields Too Large", "text/plain", msg, sizeof msg - 1);
        return;
    }
    char req[MON_HTTP_MAX + 1];
    memcpy(req, s->input, end + 4);
    req[end + 4] = 0;
    char method[16], path[512], version[16];
    if (sscanf(req, "%15s %511s %15s", method, path, version) != 3 || strcmp(version, "HTTP/1.1") != 0) {
        static const char msg[] = "bad HTTP/1.1 request\n";
        (void)http_reply(s, 400, "Bad Request", "text/plain", msg, sizeof msg - 1);
        return;
    }
    size_t consumed = end + 4;
    memmove(s->input, s->input + consumed, s->input_len - consumed);
    s->input_len -= consumed;

    char upgrade[64] = "", connection[256] = "", key[128] = "", wsver[32] = "";
    int has_upgrade = http_header(req, "Upgrade", upgrade, sizeof upgrade);
    int has_connection = http_header(req, "Connection", connection, sizeof connection);
    int has_key = http_header(req, "Sec-WebSocket-Key", key, sizeof key);
    int has_version = http_header(req, "Sec-WebSocket-Version", wsver, sizeof wsver);
    if (!strcmp(method, "GET") && has_upgrade == 1 && strcasecmp(upgrade, "websocket") == 0 &&
        has_connection == 1 && contains_token_ci(connection, "upgrade")) {
        if (has_key != 1 || has_version != 1 || strcmp(wsver, "13") != 0 ||
            strncmp(path, "/ws", 3) != 0 || (path[3] && path[3] != '?' && path[3] != '/')) {
            static const char msg[] = "invalid WebSocket upgrade\n";
            (void)http_reply(s, 400, "Bad Request", "text/plain", msg, sizeof msg - 1);
            return;
        }
        if (do_websocket_handshake(s, key) != 0) {
            static const char msg[] = "invalid WebSocket key\n";
            (void)http_reply(s, 400, "Bad Request", "text/plain", msg, sizeof msg - 1);
        }
        return;
    }
    if (strcmp(method, "GET") != 0) {
        static const char msg[] = "GET required\n";
        (void)http_reply(s, 405, "Method Not Allowed", "text/plain", msg, sizeof msg - 1);
        return;
    }
    char *query = strchr(path, '?');
    if (query) *query = 0;
    if (!strcmp(path, "/") || !strcmp(path, "/monitor.html")) (void)serve_html(s);
    else {
        static const char msg[] = "not found\n";
        (void)http_reply(s, 404, "Not Found", "text/plain", msg, sizeof msg - 1);
    }
}

static void read_client(monitor_state_t *s, machine_t *m) {
    if (s->client_fd < 0) return;
    for (;;) {
        if (s->input_len == sizeof s->input) {
            if (s->websocket) protocol_close(s, 1009);
            else {
                static const char msg[] = "request too large\n";
                (void)http_reply(s, 413, "Payload Too Large", "text/plain", msg, sizeof msg - 1);
            }
            break;
        }
        ssize_t n = recv(s->client_fd, s->input + s->input_len,
                         sizeof s->input - s->input_len, MSG_DONTWAIT);
        if (n > 0) {
            s->input_len += (size_t)n;
            note_activity(s);
            continue;
        }
        if (n == 0) { close_client(s); return; }
        if (errno == EINTR) continue;
        if (errno == EAGAIN || errno == EWOULDBLOCK) break;
        close_client(s); return;
    }
    if (s->client_fd < 0) return;
    if (s->websocket) process_ws_frames(s, m);
    else process_http(s);
    if (s->websocket && s->input_len) process_ws_frames(s, m);
}

static void poll_io(monitor_state_t *s, machine_t *m) {
    expire_idle_http(s);
    if (s->client_fd < 0) (void)accept_client(s);
    if (s->client_fd >= 0) read_client(s, m);
    publish_pending(s, m);
    flush_output(s);
}

static void monitor_on_stop(machine_t *m, dbg_frontend_t *fe, const dbg_stop_t *st) {
    monitor_state_t *s = fe->ctx;
    s->at_stop = 1;
    s->need_state = 1;
    s->need_fb = 1;
    publish_pending(s, m);
    flush_output(s);
    if (s->quit_pending) {
        s->quit_pending = 0;
        dbg_resume(m, DBG_ACT_QUIT, 0, -1);
    }
    /* A final stop is useful if a browser is connected, but a headless
     * monitor invocation must not keep a completed run alive indefinitely. */
    if (st->reason == DBG_STOP_END && !s->websocket)
        dbg_remove_frontend(m, fe);
}

static int monitor_stop_fd(machine_t *m, dbg_frontend_t *fe) {
    monitor_state_t *s = fe->ctx;
    (void)m;
    expire_idle_http(s);
    return s->client_fd >= 0 ? s->client_fd : s->listen_fd;
}

static int monitor_stop_events(machine_t *m, dbg_frontend_t *fe) {
    monitor_state_t *s = fe->ctx;
    (void)m;
    int events = POLLIN | POLLERR | POLLHUP;
    if (s->out_head) events |= POLLOUT;
    return events;
}

static void monitor_service(machine_t *m, dbg_frontend_t *fe) {
    monitor_state_t *s = fe->ctx;
    poll_io(s, m);
}

static void monitor_poll_running(machine_t *m, dbg_frontend_t *fe) {
    monitor_state_t *s = fe->ctx;
    poll_io(s, m);
    if (s->websocket && (++s->poll_count % MON_STATE_EVERY) == 0) {
        s->need_state = 1;
        s->need_fb = 1;
        publish_pending(s, m);
        flush_output(s);
    }
}

static void monitor_detach(machine_t *m, dbg_frontend_t *fe) {
    monitor_state_t *s = fe->ctx;
    (void)m;
    if (s->listen_fd >= 0) close(s->listen_fd);
    close_client(s);
    free(s->prev_pixels);
    free(s);
}

static int parse_spec(const char *spec, char host[INET_ADDRSTRLEN], int *port) {
    const char *colon = strrchr(spec, ':');
    const char *ptext = spec;
    snprintf(host, INET_ADDRSTRLEN, "127.0.0.1");
    if (colon) {
        size_t n = (size_t)(colon - spec);
        if (!n || n >= INET_ADDRSTRLEN) return -1;
        memcpy(host, spec, n); host[n] = 0;
        ptext = colon + 1;
    }
    if (!*ptext) return -1;
    char *end = NULL;
    errno = 0;
    long v = strtol(ptext, &end, 10);
    if (errno || end == ptext || *end || v < 1 || v > 65535) return -1;
    struct in_addr test;
    if (inet_pton(AF_INET, host, &test) != 1) return -1;
    *port = (int)v;
    return 0;
}

int monitor_attach(machine_t *m, const char *spec) {
    char host[INET_ADDRSTRLEN];
    int port;
    if (!spec || parse_spec(spec, host, &port) != 0) {
        fprintf(stderr, "--monitor: expected [IPv4:]port, got '%s'\n", spec ? spec : "");
        return -1;
    }
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) { perror("--monitor socket"); return -1; }
    int one = 1;
    (void)setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    struct sockaddr_in sa;
    memset(&sa, 0, sizeof sa);
    sa.sin_family = AF_INET;
    sa.sin_port = htons((uint16_t)port);
    if (inet_pton(AF_INET, host, &sa.sin_addr) != 1 ||
        bind(fd, (struct sockaddr *)&sa, sizeof sa) < 0 || listen(fd, 4) < 0 || set_nonblock(fd) != 0) {
        fprintf(stderr, "--monitor: cannot listen on %s:%d: %s\n", host, port, strerror(errno));
        close(fd);
        return -1;
    }
    monitor_state_t *s = calloc(1, sizeof *s);
    if (!s) { close(fd); return -1; }
    s->listen_fd = fd;
    s->client_fd = -1;
    s->fe.name = "monitor";
    s->fe.ctx = s;
    s->fe.on_stop = monitor_on_stop;
    s->fe.stop_fd = monitor_stop_fd;
    s->fe.stop_events = monitor_stop_events;
    s->fe.service = monitor_service;
    s->fe.poll_running = monitor_poll_running;
    s->fe.on_detach = monitor_detach;
    s->fe.wants_end_stop = 1;
    if (dbg_add_frontend(m, &s->fe) != 0) {
        close(fd); free(s);
        fprintf(stderr, "--monitor: debugger frontend limit reached\n");
        return -1;
    }
    fprintf(stderr, "[monitor] listening at http://%s:%d/ (WebSocket /ws)\n", host, port);
    return 0;
}
