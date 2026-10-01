// pci.h -- PCI configuration mechanism #1 (ports 0xCF8/0xCFC) + a generic
// MMCONFIG-style memory-mapped config window, since the firmware pokes both.
#ifndef PCI_H
#define PCI_H
#include <stdint.h>
#include "machine.h"

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
pci_dev_t *pci_add_device(machine_t *m, int bus, int dev, int func, const char *name,
                           uint16_t vendor, uint16_t device, uint8_t class_, uint8_t subclass, uint8_t progif);
pci_dev_t *pci_find(machine_t *m, int bus, int dev, int func);

#endif
