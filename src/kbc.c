/* kbc.c -- Intel 8042 keyboard controller (CHIPSET H4). Contract in kbc.h.
 * IRQ1 is driven as a LEVEL through pic_set_irq (the 8042 holds the line
 * while its output buffer is full and the command byte allows it), which
 * is what makes "IRQ enabled only later" and "drained then refilled"
 * behave like silicon. The output buffer may only be refilled once it is
 * empty, so the level dips between back-to-back bytes -- i.e. one PIC
 * edge per delivered byte even with the default edge-triggered IRR. */
#include <ctype.h>
#include <stdlib.h>
#include <string.h>
#include "machine.h"
#include "kbc.h"
#include "chipset.h"
#include "pic.h"

#define ST_OBF 0x01
#define ST_SYS 0x04
#define ST_A2  0x08
#define ST_INH 0x10
#define CMD_IRQ 0x01
#define CMD_DIS 0x10

void kbc_init(machine_t *m) {
    /* reset_pulses is host-side observability, not a hardware register:
     * it keeps counting across the warm resets machine_reset() puts the
     * controller through, so tests can still compare "pulses seen" with
     * "resets performed" (H5 pins 2 pulses -> 1 boundary-consumed reset). */
    unsigned pulses = m->kbc.reset_pulses;
    memset(&m->kbc, 0, sizeof m->kbc);
    m->kbc.reset_pulses = pulses;
    /* H5: the output port powers up with SRST# deasserted, otherwise the
     * board would hold itself in reset. A20 contribution starts low. */
    m->kbc.outport = 0x01;
    m->kbc.kbd_scan_set = 2;
    m->kbc.kbd_last_reply = 0xFA;
}

/* ------------------------------------------------------------- queue */

static int q_empty(const kbc_t *k) { return k->q_head == k->q_tail; }
static int q_push(kbc_t *k, uint8_t v) {
    unsigned next = (k->q_tail + 1) % KBC_QUEUE;
    if (next == k->q_head) return -1;
    k->queue[k->q_tail] = v; k->q_tail = next;
    return 0;
}
static uint8_t q_pop(kbc_t *k) {
    uint8_t v = k->queue[k->q_head];
    k->q_head = (k->q_head + 1) % KBC_QUEUE;
    return v;
}

/* -------------------------------------------------------- IRQ / fill */

static void kbc_update_irq(machine_t *m) {
    pic_set_irq(m, 1, ((m->kbc.status & ST_OBF) && (m->kbc.cmd & CMD_IRQ)) ? 1 : 0);
}

/* Move the next queued keyboard byte into the output buffer when allowed.
 * Device replies and injected scancodes share FIFO order. */
static void try_fill(machine_t *m) {
    kbc_t *k = &m->kbc;
    if ((k->status & (ST_OBF | ST_INH)) || q_empty(k)) return;
    k->ob = q_pop(k);
    k->status |= ST_OBF;
    kbc_update_irq(m);
}

/* The 8042 routes keyboard-device replies through the same output FIFO as
 * injected scan codes. Queue an entire response before exposing its first
 * byte so ACK/BAT and multi-byte identify replies keep their wire order. */
static void kbd_reply(machine_t *m, const uint8_t *bytes, unsigned count) {
    kbc_t *k = &m->kbc;
    static unsigned probe_replies;
    for (unsigned i = 0; i < count; i++) {
        if (q_push(k, bytes[i]) < 0) break;
        k->kbd_last_reply = bytes[i];
        if (probe_replies++ < 64)
            mlog(&m->log, "[kbd-probe] reply=0x%02x status=0x%02x rip=%llx vt=%llu",
                 bytes[i], k->status, (unsigned long long)m->cpu.rip,
                 (unsigned long long)m->vtime_instr);
    }
    try_fill(m);
}

static void kbd_reply_byte(machine_t *m, uint8_t byte) {
    kbd_reply(m, &byte, 1);
}

static void kbd_ack(machine_t *m) {
    kbd_reply_byte(m, 0xFA);
}

/* Small PS/2 keyboard protocol model: SeaBIOS reset/BAT, scan-set setup,
 * typematic/LED parameters, enable/disable scanning, and identify. The
 * controller itself still consumes host writes synchronously (IBF=0). */
static void kbd_data_write(machine_t *m, uint8_t byte) {
    kbc_t *k = &m->kbc;
    static unsigned probe_writes;
    if (probe_writes++ < 64)
        mlog(&m->log, "[kbd-probe] write=0x%02x pending=0x%02x status=0x%02x rip=%llx vt=%llu",
             byte, k->kbd_expect_param, k->status,
             (unsigned long long)m->cpu.rip, (unsigned long long)m->vtime_instr);

    if (k->kbd_expect_param) {
        uint8_t command = k->kbd_expect_param;
        k->kbd_expect_param = 0;
        if (command == 0xF0) {
            if (byte == 0) {
                const uint8_t reply[] = { 0xFA, k->kbd_scan_set };
                kbd_reply(m, reply, sizeof reply);
                return;
            }
            if (byte >= 1 && byte <= 3) k->kbd_scan_set = byte;
        }
        kbd_ack(m);
        return;
    }

    switch (byte) {
    case 0xFF: {                                  /* reset, ACK, then BAT */
        static const uint8_t reply[] = { 0xFA, 0xAA };
        k->kbd_expect_param = 0;
        k->kbd_scan_set = 2;
        kbd_reply(m, reply, sizeof reply);
        return;
    }
    case 0xF2: {                                  /* identify: MF2 keyboard */
        static const uint8_t reply[] = { 0xFA, 0xAB, 0x83 };
        kbd_reply(m, reply, sizeof reply);
        return;
    }
    case 0xF0: case 0xF3: case 0xED:              /* command + one parameter */
        k->kbd_expect_param = byte;
        kbd_ack(m);
        return;
    case 0xF4: case 0xF5:                         /* scanning on/off */
    case 0xF6:                                    /* restore defaults */
    case 0xF7: case 0xF8: case 0xF9: case 0xFA:  /* key-type modes */
        if (byte == 0xF6) k->kbd_scan_set = 2;
        kbd_ack(m);
        return;
    case 0xFB: case 0xFC: case 0xFD:              /* key mode + key byte */
        k->kbd_expect_param = byte;
        kbd_ack(m);
        return;
    case 0xFE:                                    /* resend last device byte */
        kbd_reply_byte(m, k->kbd_last_reply);
        return;
    case 0xEE:                                    /* echo */
        kbd_reply_byte(m, 0xEE);
        return;
    default:                                      /* unsupported command */
        kbd_reply_byte(m, 0xFE);                  /* request host resend */
        return;
    }
}

int kbc_inject_scancode(machine_t *m, uint8_t code) {
    if (q_push(&m->kbc, code) < 0) return -1;
    try_fill(m);
    return 0;
}

int kbc_queue_keys(machine_t *m, const char *hexlist) {
    int n = 0;
    const char *p = hexlist;
    while (*p) {
        while (*p == ',' || isspace((unsigned char)*p)) p++;
        if (!*p) break;
        char *end = NULL;
        long v = strtol(p, &end, 16);
        if (end == p || v < 0 || v > 0xFF) return -1;
        if (kbc_inject_scancode(m, (uint8_t)v) < 0) return -1;
        n++;
        p = end;
    }
    return n;
}

/* ---------------------------------------------------------------- ports */

static uint32_t kbc_status_read(machine_t *m) {
    static unsigned probe_status;
    if (probe_status++ < 96)
        mlog(&m->log, "[kbd-probe] status=0x%02x rip=%llx vt=%llu",
             m->kbc.status, (unsigned long long)m->cpu.rip,
             (unsigned long long)m->vtime_instr);
    return m->kbc.status;   /* IBF/TME/RCV/PERR always observed 0 */
}

static uint32_t kbc_data_read(machine_t *m) {
    kbc_t *k = &m->kbc;
    if (!(k->status & ST_OBF)) return 0xFF;       /* nothing there (documented) */
    uint8_t v = k->ob;
    static unsigned probe_reads;
    if (probe_reads++ < 64)
        mlog(&m->log, "[kbd-probe] read=0x%02x status=0x%02x rip=%llx vt=%llu",
             v, k->status, (unsigned long long)m->cpu.rip,
             (unsigned long long)m->vtime_instr);
    k->status &= (uint8_t)~ST_OBF;   /* A2 untouched: it tracks last WRITE */
    kbc_update_irq(m);                            /* level dips... */
    try_fill(m);                                  /* ...and re-arms per byte */
    return v;
}

static void kbc_data_write(machine_t *m, uint8_t v) {
    kbc_t *k = &m->kbc;
    if (k->expect_data == 1) {                    /* payload of the 0x60 cmd */
        k->expect_data = 0;
        k->cmd = v;
        k->status = (uint8_t)((k->status & ~ST_INH) | ((v & CMD_DIS) ? ST_INH : 0));
        try_fill(m);                              /* re-arm if uninhibited */
        kbc_update_irq(m);                        /* irq-enable may arm now */
        return;
    }
    if (k->expect_data == 2) {                    /* payload of the 0xD1 cmd */
        k->expect_data = 0;
        /* bit0 = SRST#: falling edge pulls the reset line low (the slow
         * sibling of 0xFE). bit1 = A20GATE, an OR source (chipset.c). */
        if ((k->outport & 0x01) && !(v & 0x01))
            chipset_request_reset(m, "KBC output-port bit0");
        k->outport = v;
        chipset_set_a20_kbc(m, (v >> 1) & 1);
        return;
    }
    /* Byte "to the keyboard": data-port write, so A2 clears. Device
     * responses enter the normal output FIFO and appear synchronously. */
    k->status &= (uint8_t)~ST_A2;
    kbd_data_write(m, v);
}

static void kbc_cmd_write(machine_t *m, uint8_t v) {
    kbc_t *k = &m->kbc;
    k->status |= ST_A2;                           /* last write was 0x64 */
    switch (v) {
    case 0x20:                                    /* read command byte */
        k->ob = k->cmd; k->status |= ST_OBF; kbc_update_irq(m); return;
    case 0x60: k->expect_data = 1; return;        /* write command byte */
    case 0xD0:                                    /* read output port */
        k->ob = k->outport; k->status |= ST_OBF; kbc_update_irq(m); return;
    case 0xD1: k->expect_data = 2; return;        /* write output port */
    case 0xAA:                                    /* self-test */
        k->ob = 0x55; k->status |= ST_OBF | ST_SYS; kbc_update_irq(m); return;
    case 0xAB:                                    /* keyboard interface test */
        k->ob = 0x00; k->status |= ST_OBF; kbc_update_irq(m); return;
    case 0xAD: k->status |= ST_INH; k->cmd |= CMD_DIS; return;
    case 0xAE: k->status &= (uint8_t)~ST_INH; k->cmd &= (uint8_t)~CMD_DIS;
               try_fill(m); return;
    case 0xFE: k->reset_pulses++;                 /* counted for observability */
               chipset_request_reset(m, "KBC 0xFE pulse"); return;
    default:   return;                            /* undocumented: sink */
    }
}

static uint32_t kbc_port_read(void *ctx, uint16_t port, int size) {
    machine_t *m = ctx;
    (void)size;
    return port == 0x64 ? kbc_status_read(m) : kbc_data_read(m);
}

static void kbc_port_write(void *ctx, uint16_t port, int size, uint32_t val) {
    machine_t *m = ctx;
    (void)size;
    if (port == 0x64) kbc_cmd_write(m, (uint8_t)val);
    else kbc_data_write(m, (uint8_t)val);
}

void kbc_io_register(machine_t *m) {
    io_register(m, 0x60, 1, kbc_port_read, kbc_port_write, m, "8042 KBC data");
    io_register(m, 0x64, 1, kbc_port_read, kbc_port_write, m, "8042 KBC status/cmd");
}
