/* chipset.h -- A20 gate + system-reset plumbing (CHIPSET H5).
 *
 * Modeled here, all with measured/observable semantics:
 *
 *  - A20 gate. Effective line state = (8042 output-port bit1) OR
 *    (port 0x92 bit1), exactly the classic PC wiring. When the gate is
 *    closed, physical address bit 20 is forced to zero on the bus
 *    (mem.c applies the mask to reads AND writes, fetches included):
 *    every odd megabyte aliases onto the even one below it -- the
 *    0xFFFF:0x0010 -> 0x0000:0x0000 wrap every DOS extender probes.
 *    Measured baseline (H5): the sample firmware touches neither the
 *    KBC A20 commands nor port 0x92 in its first 2M instructions, yet
 *    runs/fetches above 1MB (up to the 0xFFFFFFF0 reset vector), so
 *    like Bochs/QEMU and modern PCHs this machine powers up with the
 *    gate OPEN (port-0x92 bit1 reads back set at reset). Vectors in
 *    tests/test_chipset.c pin close/open/wrap from both sources.
 *
 *  - Port 0x92 (System Control Port A). bit1 = fast A20 (OR source),
 *    bit0 = fast system reset on a 0->1 write edge (ICH INIT_NOW#).
 *    Other bits are stored and read back, no behavior modeled.
 *
 *  - Port 0xCF9 (LPC Reset Control Register). The classic hard-reset
 *    sequence: a write with SYS_RST(bit2) transitioning 0->1 while
 *    RST_CPU(bit1) is set -- i.e. the canonical 0x06/0x0E, also the
 *    Linux 0x02-then-0x06 dance. bit1 alone (0x02, CPU INIT) and
 *    FULL_RST(bit3) alone do not reset; stored and read back.
 *
 *  - Reset sources. 8042 0xFE pulse (kbc.c, still counted in
 *    reset_pulses), 8042 output-port bit0 falling edge (0xD1 data with
 *    bit0=0), port 0x92 bit0, port 0xCF9. All of them funnel into ONE
 *    pending request consumed at the next instruction boundary
 *    (cpu_step): the request models the real reset-line propagation
 *    delay and keeps a reset out of the middle of an executing
 *    instruction. Back-to-back pulses before a boundary merge into a
 *    single reset, like the hardware line they drive.
 *
 * machine_reset() is the full warm reset, not a halt: CPU back to the
 * reset vector, PIC/PIT/KBC/A20/port state returned to power-on
 * defaults. RAM and ROM contents ride through (like a real warm
 * reset), the RTC rides through untouched (battery-backed silicon:
 * RESET# on an MC146818A clears neither time nor CMOS RAM), and board
 * topology (PCI devices, MMIO windows, platform devices, serial stub)
 * is not re-enumerated. `resets` counts performed resets for tests;
 * `reset_src` remembers the first requester for the log line.
 */
#ifndef CHIPSET_H
#define CHIPSET_H
#include <stdint.h>

struct machine;

typedef struct chipset {
    int      a20;          /* effective line: 1 = bit20 passes (open) */
    int      a20_kbc;      /* 8042 output-port bit1 contribution */
    int      a20_p92;      /* port 0x92 bit1 contribution */
    uint8_t  p92;          /* System Control Port A register */
    uint8_t  cf9;          /* LPC Reset Control Register */
    int      reset_pending;/* consumed by cpu_step at the boundary */
    char     reset_src[48];/* first requester, for the log */
    uint64_t resets;       /* performed-resets counter (observability) */
} chipset_t;

/* Power-on/reset defaults: gate open (p92 = 0x02), ports cleared.
 * Called from devices_init_common() at power-on and from
 * machine_reset(); preserves nothing but what's re-established. */
void chipset_init(struct machine *m);
void chipset_io_register(struct machine *m);

/* 8042 side (kbc.c) reports its output-port bit1 through this so the
 * effective line stays OR-true. */
void chipset_set_a20_kbc(struct machine *m, int on);

/* Any reset source funnels here; first source before the boundary is
 * remembered in reset_src for logging. */
void chipset_request_reset(struct machine *m, const char *src);

/* Full warm reset (see header comment). Called by cpu_step when a
 * request is pending; also directly usable by host code. */
void machine_reset(struct machine *m);

#endif
