#ifndef DEVICES_H
#define DEVICES_H
#include "machine.h"

void devices_init_common(machine_t *m);     /* LPC, SATA, GPU, EHCI, CMOS, SuperIO, CAR window */
void devices_init_platform(machine_t *m);   /* DDR controller models for the selected platform */

#endif
