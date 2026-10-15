/* ws.c -- RFC 6455 framing helpers and handshake SHA-1 (TOOLING T5).
 * No crypto or WebSocket dependency is needed at build time. */
#include <string.h>
#include "ws.h"

static uint32_t rol32(uint32_t x, unsigned n) {
    return (x << n) | (x >> (32u - n));
}

static void sha1_block(uint32_t h[5], const uint8_t block[64]) {
    uint32_t w[80];
    for (unsigned i = 0; i < 16; i++) {
        unsigned j = i * 4;
        w[i] = ((uint32_t)block[j] << 24) | ((uint32_t)block[j + 1] << 16) |
               ((uint32_t)block[j + 2] << 8) | block[j + 3];
    }
    for (unsigned i = 16; i < 80; i++)
        w[i] = rol32(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);

    uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4];
    for (unsigned i = 0; i < 80; i++) {
        uint32_t f, k;
        if (i < 20) {
            f = (b & c) | ((~b) & d); k = 0x5A827999u;
        } else if (i < 40) {
            f = b ^ c ^ d; k = 0x6ED9EBA1u;
        } else if (i < 60) {
            f = (b & c) | (b & d) | (c & d); k = 0x8F1BBCDCu;
        } else {
            f = b ^ c ^ d; k = 0xCA62C1D6u;
        }
        uint32_t t = rol32(a, 5) + f + e + k + w[i];
        e = d; d = c; c = rol32(b, 30); b = a; a = t;
    }
    h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e;
}

void ws_sha1(const uint8_t *data, size_t len, uint8_t out[20]) {
    uint32_t h[5] = { 0x67452301u, 0xEFCDAB89u, 0x98BADCFEu,
                      0x10325476u, 0xC3D2E1F0u };
    size_t full = len & ~(size_t)63;
    for (size_t off = 0; off < full; off += 64) sha1_block(h, data + off);

    uint8_t tail[128] = {0};
    size_t rem = len - full;
    if (rem) memcpy(tail, data + full, rem);
    tail[rem] = 0x80;
    size_t total = rem < 56 ? 64 : 128;
    uint64_t bits = (uint64_t)len * 8u;
    for (unsigned i = 0; i < 8; i++)
        tail[total - 1u - i] = (uint8_t)(bits >> (i * 8));
    sha1_block(h, tail);
    if (total == 128) sha1_block(h, tail + 64);
    for (unsigned i = 0; i < 5; i++) {
        out[i * 4] = (uint8_t)(h[i] >> 24);
        out[i * 4 + 1] = (uint8_t)(h[i] >> 16);
        out[i * 4 + 2] = (uint8_t)(h[i] >> 8);
        out[i * 4 + 3] = (uint8_t)h[i];
    }
}

static const char b64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

int ws_accept_key(const char *key, char *out, size_t out_size) {
    static const char guid[] = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
    if (!key || !out || out_size < 29) return -1;
    size_t n = strlen(key);
    if (!n || n > 128) return -1;
    uint8_t input[128 + sizeof guid - 1], digest[20];
    memcpy(input, key, n);
    memcpy(input + n, guid, sizeof guid - 1);
    ws_sha1(input, n + sizeof guid - 1, digest);

    size_t o = 0;
    for (size_t i = 0; i < sizeof digest; i += 3) {
        uint32_t v = (uint32_t)digest[i] << 16;
        size_t left = sizeof digest - i;
        if (left > 1) v |= (uint32_t)digest[i + 1] << 8;
        if (left > 2) v |= digest[i + 2];
        out[o++] = b64[(v >> 18) & 63];
        out[o++] = b64[(v >> 12) & 63];
        out[o++] = left > 1 ? b64[(v >> 6) & 63] : '=';
        out[o++] = left > 2 ? b64[v & 63] : '=';
    }
    out[o] = 0;
    return (int)o;
}

size_t ws_frame_encode(uint8_t *out, size_t capacity, uint8_t opcode, int fin,
                       int masked, const uint8_t mask[4],
                       const uint8_t *payload, size_t payload_len) {
    size_t ext = payload_len < 126 ? 0 : payload_len <= 0xFFFFu ? 2 : 8;
    size_t header = 2 + ext + (masked ? 4 : 0);
    if (!out || (payload_len && !payload) || capacity < header ||
        payload_len > capacity - header || (masked && !mask)) return 0;
    out[0] = (uint8_t)((fin ? 0x80u : 0u) | (opcode & 0x0Fu));
    size_t p = 2;
    if (ext == 0) out[1] = (uint8_t)(payload_len & 0x7Fu);
    else if (ext == 2) {
        out[1] = 126;
        out[p++] = (uint8_t)(payload_len >> 8);
        out[p++] = (uint8_t)payload_len;
    } else {
        out[1] = 127;
        uint64_t n = (uint64_t)payload_len;
        for (unsigned i = 0; i < 8; i++) out[p++] = (uint8_t)(n >> (56u - 8u * i));
    }
    if (masked) {
        out[1] |= 0x80u;
        memcpy(out + p, mask, 4);
        p += 4;
    }
    if (payload_len) memcpy(out + p, payload, payload_len);
    if (masked) {
        for (size_t i = 0; i < payload_len; i++) out[p + i] ^= mask[i & 3u];
    }
    return header + payload_len;
}

int ws_frame_parse_header(const uint8_t *data, size_t len, ws_frame_header_t *out) {
    if (!data || !out) return -1;
    if (len < 2) return 0;
    uint8_t first = data[0], second = data[1];
    if (first & 0x70u) return -1;          /* RSV bits require an extension */
    uint8_t opcode = first & 0x0Fu;
    if (opcode != 0 && opcode != 1 && opcode != 2 && opcode != 8 && opcode != 9 && opcode != 10)
        return -1;
    uint8_t short_len = second & 0x7Fu;
    int masked = (second & 0x80u) != 0;
    size_t ext = short_len == 126 ? 2 : short_len == 127 ? 8 : 0;
    size_t head = 2 + ext + (masked ? 4u : 0u);
    if (len < 2 + ext || len < head) return 0;

    uint64_t payload_len = short_len;
    size_t p = 2;
    if (short_len == 126) {
        payload_len = ((uint64_t)data[p] << 8) | data[p + 1];
        if (payload_len < 126) return -1;   /* non-canonical extended length */
        p += 2;
    } else if (short_len == 127) {
        if (data[p] & 0x80u) return -1;     /* RFC 6455: most-significant bit zero */
        payload_len = 0;
        for (unsigned i = 0; i < 8; i++) payload_len = (payload_len << 8) | data[p++];
        if (payload_len <= 0xFFFFu) return -1;
    }
    if ((opcode & 8u) && (!((first & 0x80u) != 0) || payload_len > 125)) return -1;
    out->fin = (uint8_t)((first >> 7) & 1u);
    out->opcode = opcode;
    out->masked = (uint8_t)masked;
    out->payload_len = payload_len;
    out->header_len = head;
    memset(out->mask, 0, sizeof out->mask);
    if (masked) memcpy(out->mask, data + p, 4);
    return 1;
}

void ws_frame_apply_mask(uint8_t *data, size_t len, const uint8_t mask[4], uint64_t offset) {
    if (!data || !mask) return;
    for (size_t i = 0; i < len; i++) data[i] ^= mask[(size_t)((offset + i) & 3u)];
}
