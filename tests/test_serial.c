/* tests/test_serial.c -- CHIPSET K4 vectors: 16550 COM1 register file.
 *
 * Bug-pin for the K4 shell storm: with only 0x3F8 claimed and every read
 * answering 0x20, LSR (0x3FD) floated 0xFF -> the guest's uart_has_data()
 * stayed true forever and RBR reads fed it 0x20 spaces: read(0) returned
 * 511 spaces per call, prompt loop at 100% CPU, keyboard drowned. The
 * register-file contract here is what the AuraLite kernel polls:
 *   LSR DR=0 (RX empty), THRE+TEMT set; RBR silent; IIR none pending.
 */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "machine.h"
#include "platform.h"
#include "serial.h"
#include "harness.h"

typedef struct { machine_t m; serial_t *s; } fx_t;
static void fx_init(fx_t *f) {
    setup_machine(&f->m);
    f->s = serial_alloc(&f->m);
    serial_io_register(&f->m, f->s);
}
/* asan/lsan: serial_alloc owns a heap block; return it per fixture. */
static void fx_done(fx_t *f) { serial_free(f->s); f->s = NULL; }

static void test_lsr_dr_clear(void) {
    fx_t f; fx_init(&f);
    assert((io_read(&f.m, 0x3FD, 1) & 0x01) == 0);          /* DR: no RX byte */
    assert(io_read(&f.m, 0x3FD, 1) == 0x60);                /* THRE | TEMT */
    assert(io_read(&f.m, 0x3F8, 1) == 0x00);                /* RBR silent */
    assert(io_read(&f.m, 0x3FA, 1) == 0x01);                /* IIR: no pending */
    assert(io_read(&f.m, 0x3FE, 1) == 0x00);                /* MSR: no modem */
    fx_done(&f);
}

static void test_scratch_and_ctl_roundtrip(void) {
    fx_t f; fx_init(&f);
    io_write(&f.m, 0x3FF, 1, 0x5A);
    assert(io_read(&f.m, 0x3FF, 1) == 0x5A);
    io_write(&f.m, 0x3F9, 1, 0x04);
    assert(io_read(&f.m, 0x3F9, 1) == 0x04);
    io_write(&f.m, 0x3FC, 1, 0x0B);
    assert(io_read(&f.m, 0x3FC, 1) == 0x0B);
    io_write(&f.m, 0x3FB, 1, 0x03);                         /* 8N1, DLAB=0 */
    assert(io_read(&f.m, 0x3FB, 1) == 0x03);
    io_write(&f.m, 0x3FA, 1, 0xC7);                         /* FCR accepted */
    assert(io_read(&f.m, 0x3FA, 1) == 0x01);
    fx_done(&f);
}

static void test_dlab_divisor_latch(void) {
    fx_t f; fx_init(&f);
    io_write(&f.m, 0x3FB, 1, 0x80);                         /* DLAB on */
    io_write(&f.m, 0x3F8, 1, 0x01);                         /* DLL */
    io_write(&f.m, 0x3F9, 1, 0x00);                         /* DLM */
    assert(io_read(&f.m, 0x3F8, 1) == 0x01);
    assert(io_read(&f.m, 0x3F9, 1) == 0x00);
    /* LSR/RBR semantics do not detach under DLAB on real chips either --
     * but the THR/RBR + IER lanes mux to the divisor latch (measured
     * against 16550 datasheet behaviour; the K4 shell init runs this
     * dance verbatim). */
    io_write(&f.m, 0x3FB, 1, 0x03);                         /* DLAB off */
    io_write(&f.m, 0x3F9, 1, 0x02);                         /* IER lane again */
    assert(io_read(&f.m, 0x3F9, 1) == 0x02);
    assert(io_read(&f.m, 0x3F8, 1) == 0x00);                /* RBR again */
    fx_done(&f);
}

int main(void) {
    test_lsr_dr_clear();
    test_scratch_and_ctl_roundtrip();
    test_dlab_divisor_latch();
    puts("test_serial: all vectors passed");
    return 0;
}
