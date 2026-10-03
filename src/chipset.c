/* chipset.c -- A20 gate + system-reset plumbing (CHIPSET H5).
 * Contract in chipset.h. The three reset sources and the two A20
 * sources all meet here; cpu_step consumes the pending reset at the
 * next instruction boundary. */
#include <stdio.h>
#include <string.h>
#include "machine.h"
#include "chipset.h"
#include "pic.h"
#include "pit.h"
#include "rtc.h"
#include "kbc.h"
#include "lapic.h"
#include "ioapic.h"

#define P92_A20   0x02
#define P92_RST   0x01
#define CF9_RST_CPU  0x02
#define CF9_SYS_RST  0x04

/* ------------------------------------------------------------ A20 */

static void a20_recompute(machine_t *m) {
    int now = (m->chipset.a20_kbc || m->chipset.a20_p92) ? 1 : 0;
    if (now != m->chipset.a20) {
        m->chipset.a20 = now;
        mlog(&m->log, "[chipset] A20 gate %s (kbc=%d p92=%d)",
             now ? "OPEN" : "CLOSED", m->chipset.a20_kbc, m->chipset.a20_p92);
    }
}

void chipset_set_a20_kbc(machine_t *m, int on) {
    if (m->chipset.a20_kbc == (on ? 1 : 0)) return;
    m->chipset.a20_kbc = on ? 1 : 0;
    a20_recompute(m);
}

/* ------------------------------------------------------------ reset */

void chipset_request_reset(machine_t *m, const char *src) {
    if (!m->chipset.reset_pending) {
        m->chipset.reset_pending = 1;
        snprintf(m->chipset.reset_src, sizeof m->chipset.reset_src,
                 "%s", src ? src : "unknown");
    }
    /* back-to-back pulses before the boundary merge (documented) */
}

void machine_reset(machine_t *m) {
    char src[sizeof m->chipset.reset_src];
    memcpy(src, m->chipset.reset_src, sizeof src);
    if (!src[0]) snprintf(src, sizeof src, "host call");  /* API-driven reset */
    int trace = m->cpu.trace;      /* cpu_reset() wipes it; debug knob survives */

    cpu_reset(&m->cpu);
    pic_init(m);
    pit_init(m);
    kbc_init(m);                   /* output port back to 0x01 (SRST# high) */
    lapic_init(m);                 /* H6: SVR soft-disabled, LVTs masked */
    ioapic_init(m);                /* H7: all RTEs masked again */
    chipset_init(m);               /* A20 open again, ports cleared */
    m->cpu.trace = trace;

    m->chipset.resets++;
    mlog(&m->log, "[chipset] system reset #%llu (%s) -> F000:FFF0",
         (unsigned long long)m->chipset.resets, src);
    /* RTC rides through (battery-backed), RAM/ROM and board topology
     * preserved -- see chipset.h. */
}

/* ------------------------------------------------------------ ports */

static uint32_t p92_read(void *ctx, uint16_t port, int size) {
    machine_t *m = ctx;
    (void)port; (void)size;
    return m->chipset.p92;
}

static void p92_write(void *ctx, uint16_t port, int size, uint32_t val) {
    machine_t *m = ctx;
    (void)port; (void)size;
    uint8_t old = m->chipset.p92;
    m->chipset.p92 = (uint8_t)val;
    if ((old ^ (uint8_t)val) & P92_A20) {
        m->chipset.a20_p92 = (val & P92_A20) ? 1 : 0;
        a20_recompute(m);
    }
    if (!(old & P92_RST) && (val & P92_RST))   /* INIT_NOW#: 0->1 edge */
        chipset_request_reset(m, "port 0x92 bit0");
}

static uint32_t cf9_read(void *ctx, uint16_t port, int size) {
    machine_t *m = ctx;
    (void)port; (void)size;
    return m->chipset.cf9;
}

static void cf9_write(void *ctx, uint16_t port, int size, uint32_t val) {
    machine_t *m = ctx;
    (void)port; (void)size;
    uint8_t old = m->chipset.cf9;
    m->chipset.cf9 = (uint8_t)val;
    /* Hard reset: SYS_RST rises while RST_CPU is set (0x06/0x0E, also
     * 0x02-then-0x06). 0x02 alone = CPU INIT, not modeled. */
    if ((val & (CF9_SYS_RST | CF9_RST_CPU)) == (CF9_SYS_RST | CF9_RST_CPU) &&
        !(old & CF9_SYS_RST))
        chipset_request_reset(m, "port 0xCF9");
}

/* ------------------------------------------------------------ init */

void chipset_init(machine_t *m) {
    uint64_t resets = m->chipset.resets;   /* observability survives */
    memset(&m->chipset, 0, sizeof m->chipset);
    m->chipset.resets = resets;
    /* Measured H5 baseline: the sample firmware never opens the gate
     * yet executes above 1MB from the first instruction, so -- like
     * Bochs/QEMU and modern PCHs -- this board powers up with the
     * gate OPEN, modeled as port-0x92 bit1 already set. */
    m->chipset.p92 = P92_A20;
    m->chipset.a20_p92 = 1;
    m->chipset.a20 = 1;
}

void chipset_io_register(machine_t *m) {
    io_register(m, 0x92, 1, p92_read, p92_write, m, "System Control Port A");
    io_register(m, 0xCF9, 1, cf9_read, cf9_write, m, "Reset Control");
}
