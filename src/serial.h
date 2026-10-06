#pragma once
/* CHIPSET K4: minimal but honest 16550-ish COM1 register file.
 *
 * Baseline model claimed only 0x3F8 and returned 0x20 on every read;
 * the rest of the block floated high (0xFF). Measured fallout on the
 * AuraLite K4 shell boot: the guest's uart_has_data() polls LSR (0x3FD),
 * saw 0xFF -> UART_DR set forever, while RBR reads came back 0x20 from
 * the one claimed port -- stdin read(0) returned 511 SPACE bytes per
 * call and the prompt loop spun at full CPU.
 */
#include "machine.h"

typedef struct serial_state serial_t;

serial_t *serial_alloc(machine_t *m);
void      serial_free(serial_t *s);   /* unit-lane fixture teardown */
void      serial_io_register(machine_t *m, serial_t *s);
