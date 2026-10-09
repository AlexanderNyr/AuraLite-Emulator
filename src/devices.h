#ifndef DEVICES_H
#define DEVICES_H
#include "machine.h"

void devices_init_common(machine_t *m);     /* LPC, SATA, GPU, EHCI, CMOS, SuperIO, CAR window */
void devices_init_platform(machine_t *m);   /* DDR controller models for the selected platform */
void devices_add_ahci2(machine_t *m);       /* STORE S4: second AHCI controller (--ahci2) */
void devices_done(machine_t *m);            /* C10: frees every mmio region + rw2 window */

/* Decode the big-endian 16-bit fields used by SCSI READ(10). */
uint16_t usb_msc_be16(const uint8_t *p);

#endif
