/* TOOLING T5: RFC 6455 framing and handshake codec vectors. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "ws.h"

static int failures;

static void check(int ok, const char *name) {
    printf("%s %s\n", ok ? "ok  " : "FAIL", name);
    if (!ok) failures++;
}

static void hex(const uint8_t *p, size_t n, char *out) {
    static const char digits[] = "0123456789abcdef";
    for (size_t i = 0; i < n; i++) {
        out[i * 2] = digits[p[i] >> 4];
        out[i * 2 + 1] = digits[p[i] & 15];
    }
    out[n * 2] = 0;
}

static void sha1_vector(const uint8_t *msg, size_t n, const char *want, const char *name) {
    uint8_t digest[20];
    char got[41];
    ws_sha1(msg, n, digest);
    hex(digest, sizeof digest, got);
    check(strcmp(got, want) == 0, name);
}

static void frame_vector(size_t n, int masked) {
    uint8_t *payload = malloc(n ? n : 1);
    size_t cap = n + 14;
    uint8_t *wire = malloc(cap);
    uint8_t *decoded = malloc(n ? n : 1);
    const uint8_t mask[4] = { 0x37, 0xFA, 0x21, 0x3D };
    if (!payload || !wire || !decoded) {
        check(0, "frame vector allocation");
        free(payload); free(wire); free(decoded);
        return;
    }
    for (size_t i = 0; i < n; i++) payload[i] = (uint8_t)(i * 17u + 3u);
    size_t written = ws_frame_encode(wire, cap, 1, 1, masked,
                                     masked ? mask : NULL, payload, n);
    ws_frame_header_t h;
    int parsed = written ? ws_frame_parse_header(wire, written, &h) : -1;
    int ok = parsed == 1 && h.fin && h.opcode == 1 && h.masked == (uint8_t)masked &&
             h.payload_len == n && written == h.header_len + n;
    if (ok && n) {
        memcpy(decoded, wire + h.header_len, n);
        if (h.masked) ws_frame_apply_mask(decoded, n, h.mask, 0);
        ok = memcmp(decoded, payload, n) == 0;
    }
    char label[96];
    snprintf(label, sizeof label, "frame length=%zu %s", n, masked ? "masked" : "unmasked");
    check(ok, label);
    free(payload); free(wire); free(decoded);
}

int main(void) {
    static const uint8_t long_msg[] =
        "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq";
    sha1_vector((const uint8_t *)"", 0,
                "da39a3ee5e6b4b0d3255bfef95601890afd80709", "FIPS SHA-1 empty string");
    sha1_vector((const uint8_t *)"abc", 3,
                "a9993e364706816aba3e25717850c26c9cd0d89d", "FIPS SHA-1 abc");
    sha1_vector(long_msg, sizeof long_msg - 1,
                "84983e441c3bd26ebaae4aa1f95129e5e54670f1", "FIPS SHA-1 56-byte vector");

    char accept[32];
    int n = ws_accept_key("dGhlIHNhbXBsZSBub25jZQ==", accept, sizeof accept);
    check(n == 28 && strcmp(accept, "s3pPLMBiTxaQ9kYGzzhZRbK+xOo=") == 0,
          "RFC 6455 Sec-WebSocket-Accept sample key");
    check(ws_accept_key("", accept, sizeof accept) < 0, "reject empty WebSocket key");

    static const size_t lengths[] = { 0, 125, 126, 65535, 65536 };
    for (size_t i = 0; i < sizeof lengths / sizeof lengths[0]; i++) {
        frame_vector(lengths[i], 0);
        frame_vector(lengths[i], 1);
    }

    /* Headers that must not be accepted: non-canonical lengths, a set high
     * bit in a 64-bit length, and control frames with payload >125. */
    ws_frame_header_t h;
    const uint8_t short_126[] = { 0x81, 126, 0, 125 };
    const uint8_t short_127[] = { 0x81, 127, 0, 0, 0, 0, 0, 0, 0, 126 };
    const uint8_t high_bit[] = { 0x81, 127, 0x80, 0, 0, 0, 0, 1, 0, 0 };
    const uint8_t large_ping[] = { 0x89, 126, 0, 126 };
    check(ws_frame_parse_header(short_126, sizeof short_126, &h) < 0,
          "reject non-canonical 16-bit frame length");
    check(ws_frame_parse_header(short_127, sizeof short_127, &h) < 0,
          "reject non-canonical 64-bit frame length");
    check(ws_frame_parse_header(high_bit, sizeof high_bit, &h) < 0,
          "reject 64-bit frame length with high bit set");
    check(ws_frame_parse_header(large_ping, sizeof large_ping, &h) < 0,
          "reject oversized WebSocket control frame");

    if (failures) {
        fprintf(stderr, "FAIL test-monitor-codec (%d)\n", failures);
        return 1;
    }
    puts("PASS test-monitor-codec");
    return 0;
}
