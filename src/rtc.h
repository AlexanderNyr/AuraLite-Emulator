/* rtc.h -- MC146818A RTC/CMOS (CHIPSET H3).
 *
 * Ports 0x70 (index: bit7 NMI-disable stored, bits 6:0 select) / 0x71
 * (data). Dynamic registers: 0x00-0x09 clock (BCD from the virtual
 * epoch; writes ignored -- the clock stays epoch-anchored, documented),
 * 0x0A (DV/RS stored, UIP bit7 computed), 0x0B (stored; UIE bit4 moves
 * the update-ended IRQ8), 0x0C (flags, cleared on read), 0x0D (0x80),
 * 0x32 (BCD century, computed). Everything else is battery-backed RAM.
 *
 * Virtual time (D6, same model as the PIT): one second == 1193182 ticks
 * == instr_per_tick*1193182 retired instructions, counted from the
 * rtc_init() anchor. The deterministic epoch is pinned at
 * 2026-01-01 00:00:00 (UTC, 24h/BCD) so tests assert exact values.
 *
 * Update phase, per the MC146818: once per second the chip lifts UIP
 * (register A bit7) for an update window during which the clock
 * registers still show the PREVIOUS second (they commit when UIP
 * drops), then an update-ended event sets register C UF (+IRQF, and
 * IRQ8 through the slave PIC when UIE=1). Pinned: UIP window = 2048
 * ticks (~1.7 ms). Periodic/alarm interrupts are H3 scope-out (RS/PIE/
 * AIE stored, no delivery).
 *
 * CMOS content the BIOS-era firmware reads (RBIL layout):
 *   0x14 equipment byte            = 0x10 (80x25 color video, no FDD/FPU/mouse)
 *   0x15/0x16 base memory KB       = 640 (0x0280 LE)
 *   0x17/0x18 extended memory KB   = min(RAM_KB-1024, 65535) -- classic cap
 *   0x30/0x31 extended memory KB   = same value (POST mirror)
 *   0x32 century (BCD)             = computed (0x20 for 2026-epoch)
 *   0x2E/0x2F checksum             = 0x0000, intentionally never computed
 *   all other non-dynamic bytes    = 0x00 at rtc_init, guest-writable
 */
#ifndef RTC_H
#define RTC_H
#include <stdint.h>

struct machine;

typedef struct rtc {
    uint8_t  idx;           /* 0x70: bit7 NMI disable, bits 6:0 select  */
    uint8_t  flags_c;       /* pending register-C flags (UF/IRQF)       */
    uint8_t  ram[128];      /* battery-backed bytes (dynamic regs bypass)*/
    uint64_t ticks;         /* virtual RTC ticks since the epoch anchor */
    uint64_t accum;         /* sub-divisor instruction residue          */
    uint64_t last_instr;
    uint32_t instr_per_tick;/* D6 divisor (12, same pin as the PIT)     */
} rtc_t;

void rtc_init(struct machine *m);
void rtc_io_register(struct machine *m);

/* Virtual-time advance -- called once per retired instruction from
 * cpu_step(); no-op until rtc_init(). IRQ8 (update-ended) is raised
 * here on the virtual second boundary. */
void rtc_tick(struct machine *m);

#endif
