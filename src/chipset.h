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
 *  - Port 0xCF9 (LPC Reset Control Register, Intel PCH RST_CNT).
 *    Bit roles per the PCH register database: SYS_RST(bit1, 0x02)
 *    picks the reset TYPE (0 = soft, 1 = hard), RST_CPU(bit2, 0x04)
 *    is the trigger -- its 0->1 edge fires a soft reset (INIT#
 *    forced active) when SYS_RST is 0 and a hard (PCI) reset when
 *    SYS_RST is 1; FULL_RST(bit3, 0x08) further makes a hard reset
 *    a full power cycle. The canonical reboot writes 0x06 / 0x0E
 *    (or the Linux 0x02-then-0x06 dance) therefore hard-reset, while
 *    0x04 alone asserts INIT# (UPKEEP U5). Values are stored and
 *    read back; FULL_RST's power-cycle flavor is not observable
 *    beyond storage on this board.
 *
 *  - Reset sources. 8042 0xFE pulse (kbc.c, still counted in
 *    reset_pulses), 8042 output-port bit0 falling edge (0xD1 data with
 *    bit0=0), port 0x92 bit0, port 0xCF9. All of them funnel into ONE
 *    pending request consumed at the next instruction boundary
 *    (cpu_step): the request models the real reset-line propagation
 *    delay and keeps a reset out of the middle of an executing
 *    instruction. Back-to-back pulses before a boundary merge into a
 *    single reset, like the hardware line they drive. The request is
 *    a KIND, not a boolean (U5): a pending CPU INIT upgraded by a
 *    full-reset source before the boundary coalesces into the full
 *    reset; a CPU INIT requested behind a pending full reset is
 *    absorbed by it.
 *
 * machine_reset() is the full warm reset, not a halt: CPU back to the
 * reset vector, PIC/PIT/KBC/A20/port state returned to power-on
 * defaults. RAM and ROM contents ride through (like a real warm
 * reset), the RTC rides through untouched (battery-backed silicon:
 * RESET# on an MC146818A clears neither time nor CMOS RAM), and board
 * topology (PCI devices, MMIO windows, platform devices, serial stub)
 * is not re-enumerated. `resets` counts performed resets for tests;
 * `reset_src` remembers the first requester for the log line.
 *
 * machine_cpu_init() is the other consumer (U5): an INIT# broadcast --
 * every CPU context takes the INIT processor state (SDM Table 9-1,
 * via cpu_init_reset + the LAPIC after-INIT state), the BSP at the
 * reset vector, APs parked in wait-for-SIPI. No device is reset or
 * even notified, the chipset's own ports/A20 state ride through, and
 * `resets` does not move.
 */
#ifndef CHIPSET_H
#define CHIPSET_H
#include <stdint.h>

struct machine;

/* The pending boundary request is a kind (U5, no layout movement):
 * 0 = none, 1 = full warm reset, 2 = CPU INIT#. */
#define RESET_KIND_NONE      0
#define RESET_KIND_FULL      1
#define RESET_KIND_CPU_INIT  2

typedef struct chipset {
    int      a20;          /* effective line: 1 = bit20 passes (open) */
    int      a20_kbc;      /* 8042 output-port bit1 contribution */
    int      a20_p92;      /* port 0x92 bit1 contribution */
    uint8_t  p92;          /* System Control Port A register */
    uint8_t  cf9;          /* LPC Reset Control Register */
    int      reset_pending;/* RESET_KIND_*, consumed by cpu_step boundary */
    char     reset_src[48];/* first requester, for the log */
    uint64_t resets;       /* performed FULL resets (observability) */
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
 * remembered in reset_src for logging. A full reset requested behind
 * a pending CPU INIT upgrades (and re-sources) the request. */
void chipset_request_reset(struct machine *m, const char *src);
/* The CPU-INIT counterpart: absorbed by a pending full reset. */
void chipset_request_cpu_init(struct machine *m, const char *src);

/* Full warm reset (see header comment). Called by cpu_step when a
 * request is pending; also directly usable by host code. */
void machine_reset(struct machine *m);
/* INIT# broadcast to all vCPUs; devices untouched. cpu_step calls it
 * when a CPU-INIT request is pending. */
void machine_cpu_init(struct machine *m);

#endif
