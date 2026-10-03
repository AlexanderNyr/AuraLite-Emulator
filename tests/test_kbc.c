/* tests/test_kbc.c -- CHIPSET H4 vectors: Intel 8042 KBC at 0x60/0x64.
 *
 * At baseline the ports were unclaimed entirely (reads 0xFF, writes
 * vanished). Gate per CHIPSET_PLAN H4: injected key produces IRQ1, the
 * guest's data-port read drains OBF, IRQ1 sits deasserted until the next
 * byte; the self-test sequence is observed by unit test.
 */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "machine.h"
#include "platform.h"
#include "pic.h"
#include "kbc.h"
#include "harness.h"

typedef struct { machine_t m; } fx_t;
static void fx_init(fx_t *f) {
    setup_machine(&f->m);
    pic_init(&f->m); pic_io_register(&f->m);
    kbc_init(&f->m); kbc_io_register(&f->m);
    f->m.ram[0] = 0xEB; f->m.ram[1] = 0xFE;        /* jmp$ spin */
    f->m.cpu.rip = 0;
}
static void fx_steps(fx_t *f, int n) { for (int i = 0; i < n; i++) cpu_step(&f->m.cpu); }

static const uint8_t INIT_PIC[] = {
    0xB0,0x11, 0xE6,0x20, 0xB0,0x20, 0xE6,0x21,
    0xB0,0x04, 0xE6,0x21, 0xB0,0x01, 0xE6,0x21,
    0xB0,0xFD, 0xE6,0x21,                        /* unmask irq1 only */
};
/* run `init` + STI + spin with vector 0x21 -> [0x2000]++ / read 0x60 -> [0x2001] */
static const uint8_t H_KEY[] =
    { 0xFE,0x06,0x00,0x20, 0xE4,0x60, 0xA2,0x01,0x20, 0xB0,0x20, 0xE6,0x20, 0xCF };

static void fx_guest(fx_t *f) {
    uint8_t prog[96]; size_t n = 0;
    memcpy(prog, INIT_PIC, sizeof INIT_PIC); n += sizeof INIT_PIC;
    { static const uint8_t t[] = { 0xFB, 0xC6,0x06,0x02,0x20,0x01, 0xEB,0xFE };
      memcpy(prog + n, t, sizeof t); n += sizeof t; }   /* sti; [0x2002]=1; jmp$ */
    memcpy(f->m.ram, prog, n);
    f->m.cpu.rip = 0;
    mem_write(&f->m, 0x21 * 4, 2, 0x0100);
    mem_write(&f->m, 0x21 * 4 + 2, 2, 0);
    memcpy(f->m.ram + 0x0100, H_KEY, sizeof H_KEY);
}

static void test_self_test_sequence(void) {
    fx_t f; fx_init(&f);
    assert(io_read(&f.m, 0x64, 1) == 0x00);          /* all flags clear */
    assert(io_read(&f.m, 0x60, 1) == 0xFF);          /* empty OB documented */
    /* POST-style BAT: 0xAA -> 0x55, SYS flag set, no IRQ (cmd byte = 0) */
    io_write(&f.m, 0x64, 1, 0xAA);
    assert(io_read(&f.m, 0x64, 1) == 0x0D);          /* OBF | SYS | A2 */
    assert(io_read(&f.m, 0x60, 1) == 0x55);
    assert(io_read(&f.m, 0x64, 1) == 0x0C);          /* drained: SYS | A2 */
    assert(!pic_pending(&f.m));
    /* interface test: 0xAB -> 0x00 */
    io_write(&f.m, 0x64, 1, 0xAB);
    assert((io_read(&f.m, 0x64, 1) & 0x01));
    assert(io_read(&f.m, 0x60, 1) == 0x00);
    assert((io_read(&f.m, 0x64, 1) & 0x08));         /* A2 pinned per datasheet */
}

static void test_command_byte_roundtrip(void) {
    fx_t f; fx_init(&f);
    io_write(&f.m, 0x64, 1, 0x60);                   /* write cmd byte */
    io_write(&f.m, 0x60, 1, 0x45);
    io_write(&f.m, 0x64, 1, 0x20);                   /* read it back */
    assert(io_read(&f.m, 0x64, 1) & 0x01);
    assert(io_read(&f.m, 0x60, 1) == 0x45);
    assert(!pic_pending(&f.m));                      /* no IRQ, bit0 was 0 */
}

/* plan gate: injected key -> IRQ1 -> guest drains OBF -> quiet until next */
static void test_injected_key_irq1_drain_quiet(void) {
    fx_t f; fx_init(&f);
    fx_guest(&f);
    fx_steps(&f, 64);
    assert(mem_read(&f.m, 0x2002, 1) == 1);          /* guest spinning, IF=1 */
    /* enable IRQ1 in the cmd byte (0x60 cmd, data bit0) via the IO layer */
    io_write(&f.m, 0x64, 1, 0x60);
    io_write(&f.m, 0x60, 1, 0x01);
    assert(kbc_inject_scancode(&f.m, 0x1E) == 0);    /* make 'A' */
    assert(io_read(&f.m, 0x64, 1) & 0x01);           /* OBF up before read */
    fx_steps(&f, 32);
    assert(mem_read(&f.m, 0x2000, 1) == 1);          /* IRQ1 delivered... */
    assert(mem_read(&f.m, 0x2001, 1) == 0x1E);       /* ...and guest got 'A' */
    assert(!(io_read(&f.m, 0x64, 1) & 0x01));        /* OBF drained */
    assert(!(f.m.pic.master.irr & 0x02));            /* IRQ1 quiet... */
    fx_steps(&f, 64);
    assert(mem_read(&f.m, 0x2000, 1) == 1);          /* ...stays quiet */
    /* next byte: the whole cycle again */
    assert(kbc_inject_scancode(&f.m, 0x9E) == 0);    /* break 'A' */
    fx_steps(&f, 32);
    assert(mem_read(&f.m, 0x2000, 1) == 2);
    assert(mem_read(&f.m, 0x2001, 1) == 0x9E);
}

/* two queued bytes flow back to back, each with its own IRQ edge. The
 * per-delivery history handler (count-indexed cell) is needed because
 * both deliveries may complete inside one step window (measured on the
 * first version of this vector with a single value cell). */
static const uint8_t H_KEY2[] =
    { 0xFE,0x06,0x00,0x20,                    /* inc count */
      0xE4,0x60,                              /* in al,0x60 */
      0x80,0x3E,0x00,0x20,0x01,               /* cmp count,1 */
      0x75,0x05,                              /* jne -> store2 */
      0xA2,0x01,0x20,                         /* store -> [0x2001] */
      0xEB,0x03,                              /* jmp -> eoi */
      0xA2,0x03,0x20,                         /* store2 -> [0x2003] */
      0xB0,0x20, 0xE6,0x20, 0xCF };
static void test_queue_flows_in_order(void) {
    fx_t f; fx_init(&f);
    fx_guest(&f);
    memcpy(f.m.ram + 0x0100, H_KEY2, sizeof H_KEY2);  /* override handler */
    io_write(&f.m, 0x64, 1, 0x60);
    io_write(&f.m, 0x60, 1, 0x01);
    kbc_inject_scancode(&f.m, 0x1E);
    kbc_inject_scancode(&f.m, 0x30);                 /* 'B' */
    fx_steps(&f, 48);
    assert(mem_read(&f.m, 0x2000, 1) == 2);
    assert(mem_read(&f.m, 0x2001, 1) == 0x1E);       /* first ... */
    assert(mem_read(&f.m, 0x2003, 1) == 0x30);       /* ... then, in order */
}

/* INH (0xAD): queue holds, nothing moves; 0xAE re-arms the flow */
static void test_keyboard_inhibit(void) {
    fx_t f; fx_init(&f);
    fx_guest(&f);
    io_write(&f.m, 0x64, 1, 0x60);
    io_write(&f.m, 0x60, 1, 0x01);
    io_write(&f.m, 0x64, 1, 0xAD);                   /* disable keyboard */
    kbc_inject_scancode(&f.m, 0x1E);
    fx_steps(&f, 64);
    assert(mem_read(&f.m, 0x2000, 1) == 0);
    assert(!(io_read(&f.m, 0x64, 1) & 0x01));        /* held at the source */
    io_write(&f.m, 0x64, 1, 0xAE);                   /* enable: flows now */
    fx_steps(&f, 32);
    assert(mem_read(&f.m, 0x2000, 1) == 1);
    assert(mem_read(&f.m, 0x2001, 1) == 0x1E);
}

/* cmd bit0 = 0: OBF carries the byte but no IRQ fires; enabling later
 * raises the LEVEL for the already-sitting byte (8042 semantics) */
static void test_irq_enable_is_level(void) {
    fx_t f; fx_init(&f);
    fx_guest(&f);
    assert(io_read(&f.m, 0x60, 1) == 0xFF);
    kbc_inject_scancode(&f.m, 0x39);                 /* cmd byte is 0: no IRQ */
    assert(io_read(&f.m, 0x64, 1) & 0x01);           /* OBF yes */
    fx_steps(&f, 64);
    assert(mem_read(&f.m, 0x2000, 1) == 0);          /* but silent */
    io_write(&f.m, 0x64, 1, 0x60);                   /* enable now */
    io_write(&f.m, 0x60, 1, 0x01);
    fx_steps(&f, 32);
    assert(mem_read(&f.m, 0x2000, 1) == 1);          /* level IRQ appeared */
    assert(mem_read(&f.m, 0x2001, 1) == 0x39);
}

static void test_reset_pulse_counted(void) {
    fx_t f; fx_init(&f);
    assert(f.m.kbc.reset_pulses == 0);
    io_write(&f.m, 0x64, 1, 0xFE);                   /* CPU reset request */
    io_write(&f.m, 0x64, 1, 0xFE);
    assert(f.m.kbc.reset_pulses == 2);               /* H5 will consume */
}

static void test_cli_parse(void) {
    fx_t f; fx_init(&f);
    assert(kbc_queue_keys(&f.m, "1E,9E, 39") == 3);
    /* cmd byte 0: bytes queue, first is in OB; drain through the port */
    assert(io_read(&f.m, 0x60, 1) == 0x1E);
    assert(io_read(&f.m, 0x60, 1) == 0x9E);
    assert(io_read(&f.m, 0x60, 1) == 0x39);
    assert(io_read(&f.m, 0x60, 1) == 0xFF);          /* empty */
    assert(kbc_queue_keys(&f.m, "1G") == -1);        /* parse error */
    assert(kbc_queue_keys(&f.m, "1FF") == -1);       /* range error */
}

int main(void) {
    test_self_test_sequence();
    test_command_byte_roundtrip();
    test_injected_key_irq1_drain_quiet();
    test_queue_flows_in_order();
    test_keyboard_inhibit();
    test_irq_enable_is_level();
    test_reset_pulse_counted();
    test_cli_parse();
    puts("kbc tests: ok");
    return 0;
}
