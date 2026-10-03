/* rtc.c -- MC146818A RTC/CMOS (CHIPSET H3). Contract and the pinned CMOS
 * content live in rtc.h. */
#include <string.h>
#include "machine.h"
#include "rtc.h"
#include "pic.h"

#define RTC_TICKS_PER_SEC 1193182u
#define RTC_UIP_TICKS     2048u     /* pinned update window (~1.7 ms) */
#define RTC_UFBIT 0x10
#define RTC_IRQFBIT 0x80
#define RTC_UIE 0x10                /* register B bit4 */

static uint8_t bin2bcd(unsigned v) { return (uint8_t)((v / 10 << 4) | (v % 10)); }

void rtc_init(machine_t *m) {
    memset(&m->rtc, 0, sizeof m->rtc);
    m->rtc.instr_per_tick = 12;
    m->rtc.last_instr = m->cpu.instr_count;
    /* pinned power-on CMOS content (rtc.h table) */
    m->rtc.ram[0x0A] = 0x26;                 /* reg A: DV=010, RS=0110 */
    m->rtc.ram[0x0B] = 0x02;                 /* reg B: 24h, BCD */
    unsigned ram_kb = (unsigned)(RAM_SIZE / 1024);
    unsigned ext_kb = ram_kb > 1024 ? ram_kb - 1024 : 0;
    if (ext_kb > 65535) ext_kb = 65535;      /* classic 16-bit field cap */
    m->rtc.ram[0x14] = 0x10;
    m->rtc.ram[0x15] = 640 & 0xFF;
    m->rtc.ram[0x16] = 640 >> 8;
    m->rtc.ram[0x17] = ext_kb & 0xFF;
    m->rtc.ram[0x18] = ext_kb >> 8;
    m->rtc.ram[0x30] = ext_kb & 0xFF;
    m->rtc.ram[0x31] = ext_kb >> 8;
}

void rtc_tick(machine_t *m) {
    if (!m->rtc.instr_per_tick) return;
    uint64_t now = m->cpu.instr_count;
    uint64_t delta = now - m->rtc.last_instr;
    if (!delta) return;
    m->rtc.last_instr = now;
    uint64_t before = m->rtc.ticks / RTC_TICKS_PER_SEC;
    m->rtc.accum += delta;
    m->rtc.ticks += m->rtc.accum / m->rtc.instr_per_tick;
    m->rtc.accum %= m->rtc.instr_per_tick;
    uint64_t after = m->rtc.ticks / RTC_TICKS_PER_SEC;
    if (after != before) {                    /* seconds boundary: update-ended */
        m->rtc.flags_c |= RTC_UFBIT;
        if (m->rtc.ram[0x0B] & RTC_UIE) {
            m->rtc.flags_c |= RTC_IRQFBIT;
            pic_raise_irq(m, 8);              /* slave line 0 */
        }
    }
}

/* ----------------------------------------------------------- time math */

static int leap(unsigned y) { return (y % 4 == 0 && y % 100 != 0) || y % 400 == 0; }
static unsigned mdays(unsigned y, unsigned mo) {
    static const unsigned t[12] = { 31,28,31,30,31,30,31,31,30,31,30,31 };
    return mo == 2 ? t[1] + leap(y) : t[mo - 1];
}

/* Civil fields of "committed" virtual time: the MC146818 update phase
 * keeps showing the PREVIOUS second while UIP is up. Also reports UIP. */
static void rtc_civil(machine_t *m, unsigned *sec, unsigned *min, unsigned *hr,
                      unsigned *day, unsigned *mon, unsigned *yr, int *uip) {
    uint64_t t = m->rtc.ticks;
    uint64_t s = t / RTC_TICKS_PER_SEC;
    uint64_t in_sec = t % RTC_TICKS_PER_SEC;
    /* the chip powers up non-updating; the UIP window is the first
     * UIP_TICKS ticks of each second (after the very first one), and the
     * clock commits when the window closes */
    *uip = t >= RTC_TICKS_PER_SEC ? in_sec < RTC_UIP_TICKS : 0;
    uint64_t committed = (*uip && s > 0) ? s - 1 : s;
    *sec = (unsigned)(committed % 60); committed /= 60;
    *min = (unsigned)(committed % 60); committed /= 60;
    *hr  = (unsigned)(committed % 24); committed /= 24;
    unsigned y = 2026, mo = 1; uint64_t days = committed;
    while (days >= mdays(y, mo)) { days -= mdays(y, mo); if (++mo > 12) { mo = 1; y++; } }
    *day = 1 + (unsigned)days; *mon = mo; *yr = y;
}

/* ---------------------------------------------------------------- ports */

static uint32_t rtc_data_read(machine_t *m) {
    rtc_tick(m);
    unsigned sel = m->rtc.idx & 0x7F;
    unsigned sec, min, hr, day, mon, yr; int uip;
    switch (sel) {
    case 0x00: rtc_civil(m, &sec,&min,&hr,&day,&mon,&yr,&uip); return bin2bcd(sec);
    case 0x02: rtc_civil(m, &sec,&min,&hr,&day,&mon,&yr,&uip); return bin2bcd(min);
    case 0x04: rtc_civil(m, &sec,&min,&hr,&day,&mon,&yr,&uip); return bin2bcd(hr);
    case 0x06: { /* day of week: 2026-01-01 was a Thursday (5, 1=Sun) */
        uint64_t t = m->rtc.ticks, s = t / RTC_TICKS_PER_SEC;
        int u = t >= RTC_TICKS_PER_SEC ? (t % RTC_TICKS_PER_SEC) < RTC_UIP_TICKS : 0;
        if (u && s > 0) s--;                      /* same commit rule */
        return 1 + (uint8_t)((4 + s / 86400) % 7); }
    case 0x07: rtc_civil(m, &sec,&min,&hr,&day,&mon,&yr,&uip); return bin2bcd(day);
    case 0x08: rtc_civil(m, &sec,&min,&hr,&day,&mon,&yr,&uip); return bin2bcd(mon);
    case 0x09: rtc_civil(m, &sec,&min,&hr,&day,&mon,&yr,&uip); return bin2bcd(yr % 100);
    case 0x0A: { rtc_civil(m, &sec,&min,&hr,&day,&mon,&yr,&uip);
                 return (m->rtc.ram[0x0A] & 0x7F) | (uip ? 0x80 : 0); }
    case 0x0B: return m->rtc.ram[0x0B];
    case 0x0C: { uint8_t v = m->rtc.flags_c; m->rtc.flags_c = 0; return v; }
    case 0x0D: return 0x80;
    case 0x32: rtc_civil(m, &sec,&min,&hr,&day,&mon,&yr,&uip); return bin2bcd(yr / 100);
    default: return m->rtc.ram[sel & 0x7F];
    }
}

static void rtc_data_write(machine_t *m, uint8_t v) {
    rtc_tick(m);
    unsigned sel = m->rtc.idx & 0x7F;
    switch (sel) {
    case 0x00: case 0x02: case 0x04: case 0x06: case 0x07: case 0x08: case 0x09:
    case 0x0C: case 0x0D: case 0x32:
        return;                             /* computed/read-only: documented */
    case 0x0A: m->rtc.ram[0x0A] = v & 0x7F; return;   /* UIP is read-only */
    default:   m->rtc.ram[sel & 0x7F] = v;  return;
    }
}

static uint32_t rtc_port_read(void *ctx, uint16_t port, int size) {
    machine_t *m = ctx;
    (void)size;
    if (port == 0x70) return m->rtc.idx;
    return rtc_data_read(m);
}

static void rtc_port_write(void *ctx, uint16_t port, int size, uint32_t val) {
    machine_t *m = ctx;
    (void)size;
    if (port == 0x70) m->rtc.idx = (uint8_t)val;
    else rtc_data_write(m, (uint8_t)val);
}

void rtc_io_register(machine_t *m) {
    io_register(m, 0x70, 2, rtc_port_read, rtc_port_write, m, "MC146818A RTC/CMOS");
}
