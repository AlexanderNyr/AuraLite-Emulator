/* src/serial.c -- CHIPSET K4: COM1 (16550-subset) register file.
 *
 * Guest-visible contract measured on the AuraLite kernel + shell:
 *   LSR  (R 0x3FD): bit5 THR-empty + bit6 transmitter-empty set,
 *                   bit0 DR CLEAR -- there is never RX data in this
 *                   model. (An 0xFF float read as DR=1 and rained
 *                   phantom bytes into the shell's stdin -- K4.)
 *   RBR  (R 0x3F8, DLAB=0): empty -> 0x00 (documented silence).
 *   THR  (W 0x3F8, DLAB=0): byte out, accumulated into the log line
 *                   and flushed at '\n' (the pre-existing behavior).
 *   IER  (RW 0x3F9, DLAB=0), LCR (RW 0x3FB), MCR (RW 0x3FC),
 *   SCR  (RW 0x3FF): plain storage.
 *   IIR  (R 0x3FA): 0x01 = no interrupt pending (FCR writes accepted).
 *   MSR  (R 0x3FE): 0x00 -- no modem lines asserted.
 *   DLL/DLM (RW 0x3F8/0x3F9 with DLAB=1, LCR bit7): stored; THR/RBR
 *   semantics detach while DLAB is set, like real silicon.
 */
#include "serial.h"
#include <stdlib.h>
#include <string.h>

#define UART_RBR 0x0  /* + THR / DLL (W) */
#define UART_IER 0x1  /* / DLM with DLAB */
#define UART_IIR 0x2  /* FCR on write */
#define UART_LCR 0x3
#define UART_MCR 0x4
#define UART_LSR 0x5
#define UART_MSR 0x6
#define UART_SCR 0x7

#define LSR_DR   0x01
#define LSR_THRE 0x20
#define LSR_TEMT 0x40
#define IIR_NONE 0x01
#define LCR_DLAB 0x80

struct serial_state {
    machine_t *m;
    uint8_t ier, lcr, mcr, scr;
    uint8_t dll, dlm;
    char    line[256];
    size_t  len;
};

serial_t *serial_alloc(machine_t *m) {
    serial_t *s = calloc(1, sizeof *s);
    s->m = m;
    return s;
}

void serial_free(serial_t *s) { free(s); }

static uint32_t serial_read(void *ctx, uint16_t off, int size) {
    (void)size;
    serial_t *s = ctx;
    switch (off & 7) {
    case UART_RBR: return (s->lcr & LCR_DLAB) ? s->dll : 0x00;
    case UART_IER: return (s->lcr & LCR_DLAB) ? s->dlm : s->ier;
    case UART_IIR: return IIR_NONE;                    /* nothing pending */
    case UART_LCR: return s->lcr;
    case UART_MCR: return s->mcr;
    case UART_LSR: return LSR_THRE | LSR_TEMT;         /* TX idle, RX empty */
    case UART_MSR: return 0x00;
    case UART_SCR: return s->scr;
    }
    return 0xFF;
}

static void serial_write(void *ctx, uint16_t off, int size, uint32_t val) {
    (void)size;
    serial_t *s = ctx;
    uint8_t v = (uint8_t)val;
    switch (off & 7) {
    case UART_RBR:                                     /* THR / DLL */
        if (s->lcr & LCR_DLAB) { s->dll = v; return; }
        if (v == '\r') return;
        if (v == '\n' || s->len == sizeof(s->line) - 1) {
            s->line[s->len] = 0;
            mlog(&s->m->log, "[serial] %s", s->line);
            s->len = 0;
            return;
        }
        s->line[s->len++] = (char)v;
        return;
    case UART_IER: if (s->lcr & LCR_DLAB) s->dlm = v; else s->ier = v; return;
    case UART_IIR: return;                             /* FCR: accepted */
    case UART_LCR: s->lcr = v; return;
    case UART_MCR: s->mcr = v; return;
    case UART_MSR: return;
    case UART_SCR: s->scr = v; return;
    }
}

void serial_io_register(machine_t *m, serial_t *s) {
    io_register(m, 0x3F8, 8, serial_read, serial_write, s, "16550 COM1");
}
