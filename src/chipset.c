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
/* Intel PCH RST_CNT (U5 bit-role correction, measured in the PCH
 * register databases: SYS_RST is the TYPE bit, RST_CPU the trigger
 * edge -- pre-U5 code carried the two names swapped, with identical
 * hard-reset outcomes but no INIT# anywhere). */
#define CF9_SYS_RST   0x02   /* bit1: type -- 0 soft (INIT#), 1 hard */
#define CF9_RST_CPU   0x04   /* bit2: trigger, fires on the 0->1 edge */
#define CF9_FULL_RST  0x08   /* bit3: hard reset becomes a power cycle */

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
    if (m->chipset.reset_pending != RESET_KIND_FULL) {
        /* fresh request, or an upgrade of a pending CPU INIT: the
         * coalesced boundary outcome is the full reset, so its src
         * replaces the INIT's in the pending record */
        m->chipset.reset_pending = RESET_KIND_FULL;
        snprintf(m->chipset.reset_src, sizeof m->chipset.reset_src,
                 "%s", src ? src : "unknown");
    }
    /* back-to-back pulses before the boundary merge (documented) */
}

void chipset_request_cpu_init(machine_t *m, const char *src) {
    if (!m->chipset.reset_pending) {
        m->chipset.reset_pending = RESET_KIND_CPU_INIT;
        snprintf(m->chipset.reset_src, sizeof m->chipset.reset_src,
                 "%s", src ? src : "unknown");
    }
    /* behind a pending full reset the INIT is absorbed by it */
}

void machine_reset(machine_t *m) {
    char src[sizeof m->chipset.reset_src];
    memcpy(src, m->chipset.reset_src, sizeof src);
    if (!src[0]) snprintf(src, sizeof src, "host call");  /* API-driven reset */
    int trace = m->cpu.trace;      /* cpu_reset() wipes it; debug knob survives */

    cpu_reset(&m->cpu);
    perf_tlb_flush(m);    /* PERF P2: reset boundary, even if RAM survives */
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

/* U5: the INIT# counterpart. Every vCPU takes the INIT state (BSP
 * runnable at the reset vector, APs in wait-for-SIPI -- all of it in
 * lapic.c); nothing else on the board is reset, re-enumerated or
 * even told. `resets` counts full resets only, so it stays put. */
void machine_cpu_init(machine_t *m) {
    char src[sizeof m->chipset.reset_src];
    memcpy(src, m->chipset.reset_src, sizeof src);
    if (!src[0]) snprintf(src, sizeof src, "host call");
    m->chipset.reset_pending = 0;
    m->chipset.reset_src[0] = 0;

    perf_tlb_flush(m);    /* INIT invalidates the TLBs (SDM 9.1) */
    lapic_broadcast_init(m);
    mlog(&m->log, "[chipset] CPU INIT (%s) -> F000:FFF0, devices untouched", src);
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
    static unsigned probe_writes;
    if (probe_writes++ < 16)
        mlog(&m->log, "[cf9-probe] write=0x%02x old=0x%02x CS:IP=%04x:%llx vt=%llu",
             (unsigned)val & 0xff, m->chipset.cf9, m->cpu.seg[SEG_CS].sel,
             (unsigned long long)m->cpu.rip, (unsigned long long)m->vtime_instr);
    uint8_t old = m->chipset.cf9;
    m->chipset.cf9 = (uint8_t)val;
    /* PCH RST_CNT (U5): the type is read at the RST_CPU 0->1 edge:
     * SYS_RST set then -> hard reset (0x06/0x0E single-shot, or the
     * 0x02-then-0x06 staging), SYS_RST clear -> INIT# asserted (the
     * 0x04 soft reset). No edge, no action: the type bit alone (0x02),
     * FULL_RST alone and repeated writes with RST_CPU already set are
     * all plain storage. */
    if (!(old & CF9_RST_CPU) && (val & CF9_RST_CPU)) {
        if (val & CF9_SYS_RST)
            chipset_request_reset(m, "port 0xCF9 hard reset");
        else
            chipset_request_cpu_init(m, "port 0xCF9 RST_CPU (INIT#)");
    }
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
