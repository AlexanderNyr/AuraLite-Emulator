/* pit.h -- Intel 8254 Programmable Interval Timer (CHIPSET H2).
 *
 * Counters 0 and 2 with modes 0/2/3 (6/7 map to 2/3 per clone behavior;
 * modes 1/4/5 are stored and run mode-0-like without IRQ semantics --
 * recorded, guests are not expected to use them on the supported path).
 * Counter 1 (DRAM refresh) exists and counts but its output goes nowhere.
 *
 * Virtual time (CHIPSET_PLAN D6): the machine has no wall clock; the PIT
 * advances instr_per_tick retired instructions per 1193182 Hz tick
 * (default 12 => ~99.4 virtual MIPS). All cadence is therefore
 * deterministic and testable; wall-clock scaling is PERF scope.
 *
 * IRQ0 (counter 0) is driven to the PIC as an edge strobe on each
 * terminal event (OUT 0->1 in mode 0, one strobe per period in modes
 * 2/3); the PIC's edge IRR is the PC-compatible latch for those.
 * Counter 2's OUT is visible via port 0x61 bit5; 0x61 bit0 is GATE2,
 * bit1 is speaker-data enable (stored).
 */
#ifndef PIT_H
#define PIT_H
#include <stdint.h>

struct machine;

typedef struct pit_counter {
    uint8_t  rw;        /* access mode: 1=LSB, 2=MSB, 3=LSB/MSB         */
    uint8_t  mode;      /* raw CW mode field 0..7                        */
    uint8_t  bcd;       /* stored; counting is binary (documented)       */
    uint8_t  wphase;    /* rw==3: next count byte expected (0=LSB,1=MSB) */
    uint8_t  rphase;    /* rw==3: next read byte (0=LSB,1=MSB)           */
    uint8_t  counting;  /* a full count was loaded since last CW         */
    uint8_t  latched;   /* OL holds a snapshot (latch command)           */
    uint8_t  out;       /* current OUT level                             */
    uint16_t cr;        /* count register (reload value; 0 == 65536)     */
    uint16_t latch;     /* output latch                                  */
    uint16_t read_snap; /* tearing guard for live rw==3 pairs            */
    int64_t  rem;       /* ticks remaining in this cycle, 1..period      */
    uint64_t fired;     /* terminal events since pit_init (diagnostic)   */
} pit_counter_t;

typedef struct pit {
    pit_counter_t ch[3];
    uint8_t  port_b;          /* 0x61: bit0=GATE2, bit1=SPK enable       */
    uint32_t instr_per_tick;  /* D6 virtual-time divisor (default 12)    */
    uint64_t last_instr;      /* instr_count at last pit_tick            */
    uint64_t accum;           /* fractional tick accumulator             */
} pit_t;

void pit_init(struct machine *m);
void pit_io_register(struct machine *m);

/* Advance virtual time; called once per retired instruction from
 * cpu_step(). No-op until pit_init() set instr_per_tick. */
void pit_tick(struct machine *m);

#endif
