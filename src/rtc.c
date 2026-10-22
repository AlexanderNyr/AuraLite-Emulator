/* rtc.c -- MC146818A RTC/CMOS (CHIPSET H3). Contract and the pinned CMOS
 * content live in rtc.h. */
#include <string.h>
#include "machine.h"
#include "rtc.h"
#include "pic.h"
#include "pit.h"

#define RTC_TICKS_PER_SEC 1193182u
#define RTC_UIP_TICKS     2048u     /* pinned update window (~1.7 ms) */
#define RTC_UFBIT 0x10
#define RTC_IRQFBIT 0x80
#define RTC_UIE 0x10                /* register B bit4 */
#define RTC_PFBIT 0x40              /* register C bit6: periodic flag   */
#define RTC_PIE 0x40                /* register B bit6: periodic enable */
#define RTC_SET 0x80                /* register B bit7: inhibit update  */
#define RTC_DM  0x04                /* register B bit2: 1=binary, 0=BCD */
#define RTC_24H 0x02                /* register B bit1: 1=24h, 0=12h    */
#define RTC_EPOCH_2026 1767225600LL /* 2026-01-01 00:00:00 UTC          */

static uint8_t bin2bcd(unsigned v) { return (uint8_t)((v / 10 << 4) | (v % 10)); }

void rtc_init(machine_t *m) {
    memset(&m->rtc, 0, sizeof m->rtc);
    m->rtc.base_sec = RTC_EPOCH_2026;   /* U1: the pinned power-on epoch */
    m->rtc.instr_per_tick = PIT_DEFAULT_IPT; /* K7: stay pinned to the PIT pin
                                              * (12->384): one coherent virtual
                                              * CPU frequency across time sources */
    m->rtc.last_instr = m->vtime_instr; /* K6 master clock */
    /* pinned power-on CMOS content (rtc.h table) */
    m->rtc.ram[0x0A] = 0x26;                 /* reg A: DV=010, RS=0110 */
    m->rtc.ram[0x0B] = 0x02;                 /* reg B: 24h, BCD */
    uint64_t ram_kb = RAM_SIZE / 1024u;
    uint64_t ext_kb = ram_kb > 1024 ? ram_kb - 1024 : 0;
    if (ext_kb > 65535) ext_kb = 65535;      /* classic 16-bit field cap */
    /* SeaBIOS's CMOS fallback treats 0x34/0x35 as bits 16..31 of the
     * byte count above 16 MiB (64-KiB granularity). The classic 0x17/18
     * and 0x30/31 fields saturate below that ceiling, so populate both. */
    uint64_t ext2_bytes = RAM_SIZE > 16u * 1024u * 1024u
                        ? (uint64_t)RAM_SIZE - 16u * 1024u * 1024u : 0;
    ext2_bytes &= ~0xFFFFULL;
    if (ext2_bytes > 0xFFFF0000ULL) ext2_bytes = 0xFFFF0000ULL;
    m->rtc.ram[0x14] = 0x10;
    m->rtc.ram[0x15] = 640 & 0xFF;
    m->rtc.ram[0x16] = 640 >> 8;
    m->rtc.ram[0x17] = (uint8_t)ext_kb;
    m->rtc.ram[0x18] = (uint8_t)(ext_kb >> 8);
    m->rtc.ram[0x30] = (uint8_t)ext_kb;
    m->rtc.ram[0x31] = (uint8_t)(ext_kb >> 8);
    m->rtc.ram[0x34] = (uint8_t)(ext2_bytes >> 16);
    m->rtc.ram[0x35] = (uint8_t)(ext2_bytes >> 24);
    /* COMPAT G6: CMOS 0x5F holds the extra-CPU count (present_cpus - 1) that
     * SeaBIOS qemu_get_present_cpus_count() reads during POST when fw_cfg is
     * absent.  Zero when n_vcpus/cfg_smp <= 1, keeping UP CMOS identical. */
    int smp = m->n_vcpus > 1 ? m->n_vcpus : m->cfg_smp;
    if (smp > EM_MAX_VCPU) smp = EM_MAX_VCPU;
    if (smp > 1)
        m->rtc.ram[0x5F] = (uint8_t)(smp - 1);
}

void rtc_tick(machine_t *m) {
    if (!m->rtc.instr_per_tick) return;
    uint64_t now = m->vtime_instr;  /* K6: machine-wide master clock */
    uint64_t delta = now - m->rtc.last_instr;
    if (!delta) return;
    m->rtc.last_instr = now;
    uint64_t tb_before = m->rtc.ticks;
    uint64_t before = tb_before / RTC_TICKS_PER_SEC;
    m->rtc.accum += delta;
    m->rtc.ticks += m->rtc.accum / m->rtc.instr_per_tick;
    m->rtc.accum %= m->rtc.instr_per_tick;
    uint64_t after = m->rtc.ticks / RTC_TICKS_PER_SEC;
    if (after != before && !(m->rtc.ram[0x0B] & RTC_SET)) {
        /* seconds boundary: update-ended (inhibited in SET mode, U1) */
        m->rtc.flags_c |= RTC_UFBIT;
        if (m->rtc.ram[0x0B] & RTC_UIE) {
            m->rtc.flags_c |= RTC_IRQFBIT;
            pic_raise_irq(m, 8);              /* slave line 0 */
        }
    }
    /* Periodic interrupt chain (MC146818A): rate from register A RS
     * (3..15), period 2^(RS-1)/32768 s; PF|IRQF + IRQ8 per boundary.
     * Real HW derives this from a 32 kHz divider chain; we derive it from
     * the same PIT-tick base as the seconds counter.  Inert unless PIE is
     * set (power-on: off), so UP/legacy paths keep the K3-pinned 1 Hz
     * UF-only behavior. */
    unsigned rs = m->rtc.ram[0x0A] & 0x0F;
    if ((m->rtc.ram[0x0B] & RTC_PIE) && rs >= 3 && rs <= 15) {
        /* period in PIT ticks: RTC_TICKS_PER_SEC * 2^(RS-1) / 32768 */
        uint64_t period = (uint64_t)RTC_TICKS_PER_SEC << (rs - 1) >> 15;
        if (period && m->rtc.ticks / period != tb_before / period) {
            m->rtc.flags_c |= RTC_PFBIT | RTC_IRQFBIT;
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
 * keeps showing the PREVIOUS second while UIP is up. Also reports UIP.
 * U1: absolute time is base_sec (Unix seconds) + the tick count, so a
 * guest-set date before the 2026 power-on anchor is representable. */
static void rtc_civil(machine_t *m, unsigned *sec, unsigned *min, unsigned *hr,
                      unsigned *dow, unsigned *day, unsigned *mon,
                      unsigned *yr, int *uip) {
    uint64_t t = m->rtc.ticks;
    uint64_t s = t / RTC_TICKS_PER_SEC;
    uint64_t in_sec = t % RTC_TICKS_PER_SEC;
    /* the chip powers up non-updating; the UIP window is the first
     * UIP_TICKS ticks of each second (after the very first one), and the
     * clock commits when the window closes */
    *uip = t >= RTC_TICKS_PER_SEC ? in_sec < RTC_UIP_TICKS : 0;
    uint64_t committed = (uint64_t)m->rtc.base_sec + s;
    if (*uip && committed > 0) committed--;
    *sec = (unsigned)(committed % 60); committed /= 60;
    *min = (unsigned)(committed % 60); committed /= 60;
    *hr  = (unsigned)(committed % 24); committed /= 24;
    uint64_t days = committed;
    *dow = 1 + (unsigned)((4 + days) % 7);      /* 1970-01-01: Thursday */
    unsigned y = 1970;
    while (days >= 365u + (unsigned)leap(y)) { days -= 365u + (unsigned)leap(y); y++; }
    unsigned mo = 1;
    while (days >= mdays(y, mo)) { days -= mdays(y, mo); mo++; }
    *day = 1 + (unsigned)days; *mon = mo; *yr = y;
}

/* Register-B format conversion (DM: 1=binary else BCD; 24H bit). */
static uint8_t rtc_to_reg(machine_t *m, unsigned v) {
    return (m->rtc.ram[0x0B] & RTC_DM) ? (uint8_t)v : bin2bcd(v);
}
static unsigned rtc_from_reg(machine_t *m, uint8_t v) {
    if (m->rtc.ram[0x0B] & RTC_DM) return v;
    return (v >> 4) * 10u + (v & 0x0Fu);
}
static uint8_t rtc_hour_to_reg(machine_t *m, unsigned hr24) {
    if (m->rtc.ram[0x0B] & RTC_24H) return rtc_to_reg(m, hr24);
    unsigned h = hr24 % 12u ? hr24 % 12u : 12u;
    return rtc_to_reg(m, h) | (hr24 >= 12u ? 0x80u : 0u);   /* PM bit */
}
static unsigned rtc_hour_from_reg(machine_t *m, uint8_t v) {
    unsigned h = rtc_from_reg(m, v & 0x7Fu);
    if (!(m->rtc.ram[0x0B] & RTC_24H)) {
        h %= 12u;
        if (v & 0x80u) h += 12u;
    }
    return h;
}

/* SET rising edge: snapshot the live date into the register bytes in
 * the format Register B currently selects (so partial field writes
 * and an untouched century stay coherent), like QEMU's update of
 * CMOS to the stopping time.  SET falling edge: parse the staged
 * bytes under that same (old) format and, if they name a real date,
 * move base_sec; the sub-second phase is kept.  Invalid fields log
 * and leave the clock alone rather than manufacturing civil state. */
static void rtc_seed_staged(machine_t *m) {
    unsigned sec, min, hr, dow, day, mon, yr; int uip;
    rtc_civil(m, &sec, &min, &hr, &dow, &day, &mon, &yr, &uip);
    m->rtc.ram[0x00] = rtc_to_reg(m, sec);
    m->rtc.ram[0x02] = rtc_to_reg(m, min);
    m->rtc.ram[0x04] = rtc_hour_to_reg(m, hr);
    m->rtc.ram[0x06] = rtc_to_reg(m, dow);
    m->rtc.ram[0x07] = rtc_to_reg(m, day);
    m->rtc.ram[0x08] = rtc_to_reg(m, mon);
    m->rtc.ram[0x09] = rtc_to_reg(m, yr % 100);
    m->rtc.ram[0x32] = rtc_to_reg(m, yr / 100);
}

static void rtc_commit_staged(machine_t *m) {
    unsigned sec = rtc_from_reg(m, m->rtc.ram[0x00]);
    unsigned min = rtc_from_reg(m, m->rtc.ram[0x02]);
    unsigned hr  = rtc_hour_from_reg(m, m->rtc.ram[0x04]);
    unsigned day = rtc_from_reg(m, m->rtc.ram[0x07]);
    unsigned mon = rtc_from_reg(m, m->rtc.ram[0x08]);
    unsigned yr  = rtc_from_reg(m, m->rtc.ram[0x09]);
    unsigned cen = rtc_from_reg(m, m->rtc.ram[0x32]);
    unsigned year = cen ? cen * 100u + yr : 2000u + yr;
    if (sec > 59 || min > 59 || hr > 23 || mon < 1 || mon > 12 ||
        year < 1970 || year > 9999 || day < 1 || day > mdays(year, mon)) {
        mlog(&m->log, "[rtc] SET exits with invalid date fields "
             "(%04u-%02u-%02u %02u:%02u:%02u): clock unchanged",
             year, mon, day, hr, min, sec);
        return;
    }
    uint64_t days = 0;
    for (unsigned y = 1970; y < year; y++) days += 365u + (unsigned)leap(y);
    for (unsigned mo = 1; mo < mon; mo++) days += mdays(year, mo);
    days += day - 1;
    m->rtc.base_sec = (int64_t)(days * 86400ull + hr * 3600u + min * 60u + sec);
    m->rtc.ticks %= RTC_TICKS_PER_SEC;
    mlog(&m->log, "[rtc] clock set to %04u-%02u-%02u %02u:%02u:%02u UTC",
         year, mon, day, hr, min, sec);
}

/* ---------------------------------------------------------------- ports */

static uint32_t rtc_data_read(machine_t *m) {
    rtc_tick(m);
    unsigned sel = m->rtc.idx & 0x7F;
    int setting = (m->rtc.ram[0x0B] & RTC_SET) != 0;
    unsigned sec, min, hr, dow, day, mon, yr; int uip;
    switch (sel) {
    /* In SET mode the staged register bytes read back verbatim;
     * otherwise the computed clock in Register B's format. */
    case 0x00: case 0x02: case 0x04: case 0x06:
    case 0x07: case 0x08: case 0x09: case 0x32:
        if (setting) return m->rtc.ram[sel];
        rtc_civil(m, &sec, &min, &hr, &dow, &day, &mon, &yr, &uip);
        switch (sel) {
        case 0x00: return rtc_to_reg(m, sec);
        case 0x02: return rtc_to_reg(m, min);
        case 0x04: return rtc_hour_to_reg(m, hr);
        case 0x06: return rtc_to_reg(m, dow);
        case 0x07: return rtc_to_reg(m, day);
        case 0x08: return rtc_to_reg(m, mon);
        case 0x09: return rtc_to_reg(m, yr % 100);
        default:   return rtc_to_reg(m, yr / 100);
        }
    case 0x0A: { rtc_civil(m, &sec, &min, &hr, &dow, &day, &mon, &yr, &uip);
                 return (m->rtc.ram[0x0A] & 0x7F) | ((uip && !setting) ? 0x80 : 0); }
    case 0x0B: return m->rtc.ram[0x0B];
    case 0x0C: { uint8_t v = m->rtc.flags_c; m->rtc.flags_c = 0; return v; }
    case 0x0D: return 0x80;
    default: return m->rtc.ram[sel & 0x7F];
    }
}

static void rtc_data_write(machine_t *m, uint8_t v) {
    rtc_tick(m);
    unsigned sel = m->rtc.idx & 0x7F;
    switch (sel) {
    case 0x0C: case 0x0D:
        return;                             /* read-only */
    case 0x0A: m->rtc.ram[0x0A] = v & 0x7F; return;   /* UIP is read-only */
    case 0x0B: {                            /* U1: the SET edges */
        uint8_t old = m->rtc.ram[0x0B];
        if ((v & RTC_SET) && !(old & RTC_SET)) {
            rtc_seed_staged(m);
            v &= (uint8_t)~RTC_UIE;         /* datasheet: SET clears UIE */
        } else if (!(v & RTC_SET) && (old & RTC_SET)) {
            rtc_commit_staged(m);           /* parses under the old format */
        }
        m->rtc.ram[0x0B] = v;
        return; }
    default:   m->rtc.ram[sel & 0x7F] = v;  return;   /* dates stage here */
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
