/* xhci.h -- XHCI plan: USB 3.0 host controller (docs/plans/XHCI_PLAN.md).
 *
 * X1: PCI identity + register file.  The contract is measured from the
 * guest driver (AuraLite-OS@main drivers/usb/xhci.c, see XHCI_PLAN.md
 * section 1a): class 0x0C/0x03/0x30 probe, legacy INTx, 64 KiB BAR0,
 * the exact 11-step bring-up.  State is a plain-data global (the AHCI
 * pattern) so snapshots serialize it via xhci_snap().
 */
#ifndef EMU_XHCI_H
#define EMU_XHCI_H

#include <stdint.h>
#include "machine.h"
#include "snapshot.h"

#define XHCI_MMIO_BASE   0xFEB20000u   /* 64 KiB; clear of EHCI/AHCI/e1000 */
#define XHCI_MMIO_SIZE   0x10000u
#define XHCI_IRQ         10            /* measured free ISA line */
#define XHCI_MAX_SLOTS   32
#define XHCI_MAX_PORTS   4

struct pci_dev;

void     xhci_register(machine_t *m);
void     xhci_snap(machine_t *m, snap_t *s);
uint64_t xhci_pci_cfg_read(struct pci_dev *dev, int reg, int size);
void     xhci_pci_cfg_write(struct pci_dev *dev, int reg, int size, uint64_t val);

#endif
