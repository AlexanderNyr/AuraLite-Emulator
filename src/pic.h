/* pic.h -- Intel 8259A Programmable Interrupt Controller pair (master+slave
 * cascade), CHIPSET H0+H1. PC defaults at reset: master vector base 0x08,
 * slave 0x70, all lines masked (IMR=0xFF), fixed priority 0 > 1 > ... > 7,
 * edge-triggered IRR, slave cascades through master IRQ2.
 *
 * H1 adds the full priority machinery: lowest-priority-line register with
 * rotation on EOI / set-priority command / rotate-in-AEOI, fully-nested
 * in-service gating, special mask mode, spurious IRQ7/IRQ15 vector return
 * with no ISR set, the poll command, and level-triggered lines (ICW1 LTIM)
 * with re-assertion while the line stays high. ICW4 BUF/M/S/SFNM are
 * stored, not acted on (single-PIC-pair system).
 */
#ifndef PIC_H
#define PIC_H
#include <stdint.h>

struct machine;

typedef struct pic_chip {
    uint8_t imr;          /* interrupt mask register (OCW1, data port)   */
    uint8_t irr;          /* interrupt request register                  */
    uint8_t isr;          /* in-service register                         */
    uint8_t icw_step;     /* 0=operational; 1..3 = expecting ICW2..ICW4  */
    uint8_t icw3_needed;  /* ICW1: cascade mode -> ICW3 follows ICW2     */
    uint8_t icw4_needed;  /* ICW1 bit0: ICW4 follows                     */
    uint8_t vector_base;  /* ICW2 (low 3 bits always zero)               */
    uint8_t read_isr;     /* OCW3: command-port read returns ISR vs IRR  */
    uint8_t auto_eoi;     /* ICW4 bit1 (AEOI)                            */
    uint8_t ltim;         /* ICW1 bit3: 1=level-triggered, 0=edge        */
    uint8_t prio_low;     /* line with LOWEST priority (reset: 7)        */
    uint8_t smm;          /* OCW3 special mask mode (ESMM+SMM)           */
    uint8_t rotate_aeoi;  /* OCW2 0x80/0x00: rotate in auto-EOI mode     */
    uint8_t poll_armed;   /* OCW3 poll command: next cmd read = poll byte*/
    uint8_t sfnm;         /* ICW4 bit4, stored only                      */
    uint8_t buf;          /* ICW4 bit3, stored only                      */
    uint8_t ms;           /* ICW4 bit2, stored only                      */
    uint8_t lines;        /* current IRQ pin levels (pic_set_irq)        */
} pic_chip_t;

typedef struct pic {
    pic_chip_t master;    /* ports 0x20 (cmd) / 0x21 (data)              */
    pic_chip_t slave;     /* ports 0xA0 (cmd) / 0xA1 (data), on master 2 */
} pic_t;

void pic_init(struct machine *m);
void pic_io_register(struct machine *m);

/* Edge-strobe an IRQ line: latched into IRR regardless of trigger mode
 * (a pulse; level-mode devices should use pic_set_irq with a held level).
 * irq 0..7 -> master, 8..15 -> slave (which in turn raises master IRQ2). */
void pic_raise_irq(struct machine *m, int irq);

/* Drive an IRQ line to a level. In edge mode only 0->1 transitions latch
 * IRR; in level mode IRR follows the level (with re-assertion while the
 * line stays high, and clearing on deassert). Public so devices (PIT/KBC/
 * RTC in H2-H4) can drive interrupt lines correctly. */
void pic_set_irq(struct machine *m, int irq, int level);

/* Any line deliverable (unmasked and allowed by the in-service gating)?
 * Sampling gate for IF delivery and the HLT wake. */
int  pic_pending(struct machine *m);

/* INTA cycle: pick the highest-priority deliverable line, move it into
 * ISR, clear its IRR bit, honor AEOI/rotation, and return its vector.
 * With nothing pending the 8259A returns the spurious vector (base+7)
 * WITHOUT setting an ISR bit -- callers that only INTA when INTR is
 * asserted (cpu_step) never observe it, direct callers can. */
int  pic_intack(struct machine *m);

#endif
