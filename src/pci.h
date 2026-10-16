// pci.h -- PCI configuration mechanism #1 (ports 0xCF8/0xCFC) + a generic
// MMCONFIG-style memory-mapped config window, since the firmware pokes both.
#ifndef PCI_H
#define PCI_H
#include <stdint.h>
#include "machine.h"

/* Q35 PAM layout: PAM0 bits 5:4 control the entire 0xF0000-0xFFFFF BIOS
 * window; PAM1..6 control 0xC0000-0xEFFFF in two 16-KiB windows per byte.
 * Each 2-bit field selects read-from-RAM and write-to-RAM independently. */
#define PCI_DEVICE_ID_INTEL_Q35_MCH 0x29C0
#define PCI_Q35_PAM0                 0x90
#define PCI_Q35_PAM_COUNT            7

typedef struct pci_dev {
    int bus, dev, func;
    uint8_t cfg[256];                 /* raw config space backing store */
    uint64_t (*cfg_read)(struct pci_dev*, int reg, int size);
    void     (*cfg_write)(struct pci_dev*, int reg, int size, uint64_t val);
    void *ctx;
    const char *name;
    struct pci_dev *next;
} pci_dev_t;

void pci_init(machine_t *m);
void pci_done(machine_t *m);   /* C10: frees the device list */
pci_dev_t *pci_add_device(machine_t *m, int bus, int dev, int func, const char *name,
                           uint16_t vendor, uint16_t device, uint8_t class_, uint8_t subclass, uint8_t progif);
pci_dev_t *pci_find(machine_t *m, int bus, int dev, int func);
void pci_default_cfg_write(pci_dev_t *d, int reg, int size, uint64_t val);  /* VIDEO V1 */

#endif
