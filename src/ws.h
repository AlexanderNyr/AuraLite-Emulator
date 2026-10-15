/* ws.h -- small RFC 6455 framing and RFC 6455 handshake helpers (TOOLING T5). */
#ifndef WS_H
#define WS_H
#include <stddef.h>
#include <stdint.h>

typedef struct {
    uint8_t fin;
    uint8_t opcode;
    uint8_t masked;
    uint8_t mask[4];
    uint64_t payload_len;
    size_t header_len;
} ws_frame_header_t;

/* FIPS 180-4 SHA-1, used only for the WebSocket opening handshake. */
void ws_sha1(const uint8_t *data, size_t len, uint8_t out[20]);

/* RFC 6455 Sec-WebSocket-Accept (key + GUID, SHA-1, Base64). */
int ws_accept_key(const char *key, char *out, size_t out_size);

/* Encode one complete, unfragmented frame. Returns bytes written, or 0 if
 * capacity is insufficient. Client masking uses the supplied 4-byte key. */
size_t ws_frame_encode(uint8_t *out, size_t capacity, uint8_t opcode, int fin,
                       int masked, const uint8_t mask[4],
                       const uint8_t *payload, size_t payload_len);

/* Parse a frame header. Returns 1 when complete, 0 when more bytes are
 * needed, and -1 for an invalid/non-canonical RFC 6455 header. */
int ws_frame_parse_header(const uint8_t *data, size_t len, ws_frame_header_t *out);

/* XOR a payload with the frame mask, starting at stream offset `offset`. */
void ws_frame_apply_mask(uint8_t *data, size_t len, const uint8_t mask[4], uint64_t offset);

#endif
