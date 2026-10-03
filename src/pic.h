/* pic.h -- Intel 8259A Programmable Interrupt Controller pair (master+salue
 * cascade), CHIPSET H0. PC defaults at reset: master vector base 0x08,
 * slave 0x70, all lines masked (IMR=0xFF), fixed priority 0 > 1 > ... > 7,
 * edge-triggered IRR, slave cascades through master IRQ2. Rotation, special
 * mask mode, spurious-vector handling and level triggering are H1 scope.
 */
#ifndef PIC_H
#define PIC_H
#include <stdint.h>

struct machine;

typedef struct pic_chip {
    uint8_t imr;          /* interrupt mask register (OCW1, data port)   */
    uint8_t irr;          /* interrupt request register (edge latched)   */
    uint8_t isr;          /* in-service register                         */
    uint8_t icw_step;     /* 0=operational; 1..3 = expecting ICW2..ICW4  */
    uint8_t icw3_needed;  /* ICW1: cascade mode -> ICW3 follows ICW2     */
    uint8_t icw4_needed;  /* ICW1 bit0: ICW4 follows                     */
    uint8_t vector_base;  /* ICW2 (low 3 bits always zero)               */
    uint8_t read_isr;     /* OCW3: command-port read returns ISR vs IRR  */
    uint8_t auto_eoi;     /* ICW4 bit1                                   */
} pic_chip_t;

typedef struct pic {
    pic_chip_t master;    /* ports 0x20 (cmd) / 0x21 (data)              */
    pic_chip_t slave;     /* ports 0xA0 (cmd) / 0xA1 (data), on master 2 */
} pic_t;

void pic_init(struct machine *m);
void pic_io_register(struct machine *m);

/* Assert an IRQ line (edge-triggered). irq 0..7 -> master, 8..15 -> slave
 * (which in turn raises master IRQ2). Public so devices (PIT/KBC/RTC in
 * H2-H4) can drive interrupt lines. */
void pic_raise_irq(struct machine *m, int irq);

/* Any unmasked line pending INTR? (Sampling gate for IF delivery.) */
int  pic_pending(struct machine *m);

/* INTA cycle: pick the highest-priority pending line, move it into ISR,
 * clear its IRR bit (edge), honor AEOI, and return its vector, or -1. */
int  pic_intack(struct machine *m);

#endif
