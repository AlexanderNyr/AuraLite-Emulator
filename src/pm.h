/* pm.h -- ACPI PM timer block (COMPAT G5).
 *
 * The ICH9/Q35 LPC function owns an ACPI PM register block at PMBASE.
 * SeaBIOS programs PMBASE (`pci config 0:31.0 0x40 = 0xb000|1`) and then,
 * once the FADT is published, switches its own timer to the 24-bit PM Timer
 * at PMBASE+8 -- `timer_setup()` logs "Using pmtimer".  Before this module
 * existed the port was unconnected, so it read 0xFFFFFFFF forever and every
 * `timer_check()` poll spun without a timeout: with `--lpc=ich9` the AHCI
 * port scan never expired and the BIOS never reached the boot path.
 *
 * Time base is the same deterministic one the PIT uses (pit.h, D6/K7): the
 * PM timer runs at 3.579545 MHz, exactly three PIT ticks (1.193182 MHz),
 * so it advances three counts per PIT tick.  There is no wall clock and no
 * host timing anywhere in this file.
 *
 * Scope: the Timer register (offset 0x08, 24 bits, read-only) is modelled;
 * every other byte of the 0x40-byte block reads as zero and ignores writes.
 * PM1_CNT sleep transitions, PM1_STS/EN, GPE and SMBus are not modelled --
 * a guest that writes them sees no effect instead of a fabricated one.
 */
#ifndef PM_H
#define PM_H

#include <stdint.h>

struct machine;

typedef struct pm {
    uint16_t base;      /* PMBASE as programmed (bits 15:7), default 0xb000 */
    uint32_t count;     /* 24-bit TMR counter                              */
    uint64_t last_instr;/* vtime_instr at the last advance                  */
    uint64_t accum;     /* fractional PIT-tick accumulator (TMR runs 3x)   */
    uint64_t reads;     /* TMR reads (diagnostic)                          */
    uint16_t sts;       /* PM1_STS latch (W1C bits; PM P1)                 */
    uint16_t en;        /* PM1_EN                                          */
    uint16_t cnt;       /* PM1_CNT stored bits (SCI_EN, BM_RLD)            */
    int sci_level;      /* last level driven on the SCI line (IRQ9)        */
    uint32_t gpe_sts;   /* GPE0_STS latch at +0x20 (W1C; PM P2)           */
    uint32_t gpe_en;    /* GPE0_EN at +0x24                               */
} pm_t;

#define PM_SCI_IRQ 9
#define PM_DEFAULT_BASE 0xB000   /* Q35/SeaBIOS PMBASE; the FADT
                                    advertises this address (PM P2) */

/* Reset the block and decode it at the Q35/SeaBIOS default base 0xb000. */
void pm_init(struct machine *m);

/* Move the 0x40-byte register block; the old window stops decoding. */
void pm_set_base(struct machine *m, uint16_t base);

/* Current 24-bit Timer value, advanced from vtime_instr. */
uint32_t pm_timer_now(struct machine *m);

/* Advance the Timer and re-drive SCI (device-tick cadence, PM P1). */
void pm_tick(struct machine *m);

#endif
