/* ioapic.h -- 82093AA I/O APIC (CHIPSET H7).
 *
 * MMIO at 0xFEC00000: IOREGSEL (offset 0x00) selects an indirect
 * register, IOWIN (0x10) reads/writes it. Registers: 0x00 APIC ID (RW,
 * bits 24-27), 0x01 VERSION (RO: 0x11 with MaxRedir=23), 0x02 ARB
 * (RO = ID<<24), 0x10-0x3F the 24-entry 64-bit redirection table (RTE).
 *
 * Board model (documented): every ISA IRQ line is wired to BOTH the
 * 8259 pair and the same-numbered IOAPIC INTIN pin. The fan-out lives
 * INSIDE pic_raise_irq()/pic_set_irq(): devices keep driving the PIC
 * exactly as before (D7), and the IOAPIC sees the same edges/levels --
 * IRQ0 strobes from the 8254, held levels from the 8042, IRQ8 strobes
 * from the RTC.
 *
 * RTE fields stored: vector (0-7), delivery mode (8-10), dest mode
 * (11), polarity (13), remote-IRR/readback (14), trigger (15), mask
 * (16), destination (56-63). DELIVERED: delivery mode Fixed (0) to the
 * single vCPU's LAPIC regardless of the destination field (like the
 * LAPIC's own ICR model); NMI/SMI/INIT/ExtINT styles are stored, not
 * delivered -- ExtINT's LINT0 bypass is a H6 LINT story, not H7.
 *
 * Semantics:
 *  - Edge-triggered RTE: a line event latches an internal pending edge
 *    (yes, real IOAPICs hold masked edges -- unlike LAPIC LVTs) which
 *    flushes to the LAPIC's IRR on unmask/reprogram. Single-shot.
 *  - Level-triggered RTE: while the line is at its asserted polarity,
 *    an unmasked entry with remote_IRR clear delivers and sets
 *    remote_IRR; the LAPIC's EOI on that vector calls back
 *    (ioapic_eoi_notify) and clears remote_IRR, re-arming the entry --
 *    so a HELD level redelivers after each EOI, like the bus it models.
 *  - Evaluation points: line transitions from pic_*, RTE writes, and
 *    the EOI callback (documented; there is no free-running scan).
 */
#ifndef IOAPIC_H
#define IOAPIC_H
#include <stdint.h>

struct machine;

#define IOAPIC_BASE   0xFEC00000ULL
#define IOAPIC_SIZE   0x20
#define IOAPIC_MAXRED 24

typedef struct ioapic_rte {
    uint32_t lo, hi;         /* raw dword views of the 64-bit RTE */
} ioapic_rte_t;

typedef struct ioapic {
    uint8_t      regsel;
    uint32_t     id;
    ioapic_rte_t rte[IOAPIC_MAXRED];
    uint8_t      lines;      /* pin voltage as asserted by the board */
    uint32_t     edge_pend;  /* latched edges awaiting delivery (bit/pin) */
    uint32_t     remote_irr; /* level-mode in-service pins (bit/pin) */
} ioapic_t;

void ioapic_init(struct machine *m);
void ioapic_mmio_register(struct machine *m);

/* board-level line drivers (called from pic.c's fan-out) */
void ioapic_set_gsi(struct machine *m, int irq, int level);
void ioapic_edge_gsi(struct machine *m, int irq);

/* LAPIC EOI feedback loop (called from lapic.c) */
void ioapic_eoi_notify(struct machine *m, int vector);

#endif
