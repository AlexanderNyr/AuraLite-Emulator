/* kbc.h -- Intel 8042 keyboard controller (CHIPSET H4).
 *
 * Ports 0x60 (data) / 0x64 (status read, command write). Modeled:
 *  - status register: OBF (bit0), IBF (bit1 -- always observed 0; host
 *    bytes are consumed synchronously, documented), SYS (bit2, set by
 *    self-test), A2 (bit3), INH (bit4, keyboard inhibited), 0 elsewhere;
 *  - command byte: read (0x20) / write (0x60+data); bit0 = IRQ1 enable,
 *    bit4 = keyboard-inhibit mirror (also via 0xAD/0xAE commands);
 *  - output port (P20-P27 latch, H5): bit0 = SRST# (a 0xD1 write with
 *    bit0=0 pulls it low -> system reset), bit1 = A20GATE (one of the
 *    two OR sources for the A20 line, chipset.h). Read via 0xD0,
 *    written via 0xD1 + data byte. Powers up 0x01 (SRST# high,
 *    A20 contribution low);
 *  - commands: 0xAA self-test (replies 0x55, sets SYS), 0xAB interface
 *    test (replies 0x00), 0xAD/0xAE disable/enable, 0xD0/0xD1 output
 *    port read/write, 0xFE CPU-reset pulse (counted in reset_pulses
 *    AND, since H5, requests the real machine reset through
 *    chipset_request_reset -- port 0x92/0xCF9 are the other sources);
 *  - IRQ1 is a LEVEL of (output-buffer-full AND command-byte bit0):
 *    scan codes obviously, command responses too when the guest left
 *    the interrupt enabled (POST clears it, so self-test stays quiet).
 *
 * Keyboard side: scancode-set-1 injection. kbc_inject_scancode() queues
 * bytes; one byte at a time moves into the output buffer (OBF + edge
 * IRQ1), the next moves immediately after the guest's data-port read
 * drains the previous one (no inter-byte delay modeled -- documented).
 * While INH the queue holds. Extended (0xE0-prefixed) codes are passed
 * through as their byte stream; make/break distinction is the guest's.
 *
 * CLI: --keys=1E,9E,39 -- comma-separated hex scancode bytes, queued at
 * startup (main.c; kbc_queue_keys() does the parsing).
 */
#ifndef KBC_H
#define KBC_H
#include <stdint.h>

struct machine;

#define KBC_QUEUE 64

typedef struct kbc {
    uint8_t  status;      /* dynamic: OBF|SYS|A2|INH */
    uint8_t  cmd;         /* command byte */
    uint8_t  ob;          /* output buffer */
    uint8_t  outport;     /* P20-P27 latch: bit0 SRST#, bit1 A20GATE (H5) */
    uint8_t  expect_data; /* 0 none, 1 payload for 0x60, 2 payload for 0xD1 */
    uint8_t  queue[KBC_QUEUE];
    unsigned q_head, q_tail;      /* ring: head=next out, tail=next in */
    unsigned reset_pulses;        /* 0xFE pulse count (observability) */
} kbc_t;

void kbc_init(struct machine *m);
void kbc_io_register(struct machine *m);

/* Inject one scancode byte (set 1; caller pushes 0xE0 first for
 * extended codes). Returns 0 on success, -1 when the queue is full. */
int  kbc_inject_scancode(struct machine *m, uint8_t code);

/* Parse "1E,9E,39" (hex, comma-separated) into the queue. Returns the
 * number of bytes queued, or -1 on a parse error. */
int  kbc_queue_keys(struct machine *m, const char *hexlist);

#endif
