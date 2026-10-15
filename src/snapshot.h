// snapshot.h -- TOOLING T4: machine snapshots.
//
// A snapshot is the complete machine state at a run-loop boundary: RAM
// (sparse), machine_t minus host pointers and configuration, every device
// model's state through per-module visitors, PCI config spaces, the MMIO
// list including runtime ECAM registrations, RAM-window buffers
// (framebuffer, CAR, DDR), disk/SATA images, the log ring, the P2/P3
// caches (so --stats stays identical) and the device-cadence phase.
// Harness actions (--keys, --keys-at, --vbe) are not machine state and are
// never stored. Loading requires the same build (layout fingerprint) and
// the same machine shape (platform, ROM, kernel entry symbol, vCPUs, disk
// and SATA sizes, ...); anything else is refused with a reason.
#ifndef SNAPSHOT_H
#define SNAPSHOT_H
#include <stdio.h>
#include <stddef.h>
#include <stdint.h>
#include "machine.h"

/* --- the visitor used by every module (same code path saves and loads).
 * Modules call through these function pointers, so devices.c/ahci.c/
 * pci.c/serial.c need this header but never link against snapshot.c --
 * the narrow unit harnesses keep their link lines (cf. cpu_devices_tick). */
typedef struct snap {
    int loading;
    void (*bytes)(struct snap *s, const char *tag, void *p, size_t n);
    void (*sparse)(struct snap *s, const char *tag, uint8_t *buf, size_t n);
} snap_t;
static inline int  snap_loading(const snap_t *s) { return s->loading; }
static inline void snap_bytes(snap_t *s, const char *tag, void *p, size_t n) { s->bytes(s, tag, p, n); }
static inline void snap_sparse(snap_t *s, const char *tag, uint8_t *buf, size_t n) { s->sparse(s, tag, buf, n); }

/* --- module visitors (defined next to the state they own) */
void devices_snap(machine_t *m, snap_t *s);   /* devices.c (+ serial)  */
void ahci_snap(machine_t *m, snap_t *s);      /* ahci.c                */
struct serial_state;
void serial_snap(struct serial_state *ser, snap_t *s);   /* serial.c   */
void pci_snap(machine_t *m, snap_t *s);       /* pci.c                 */
void pci_ecam_map(machine_t *m, uint64_t base);  /* pci.c: replay a run-time ECAM window */

/* --- whole-machine save/load */
typedef struct {
    const char *platform;   /* platform name (shape)                     */
    int tap;                /* --tap: host socket state -> refused       */
    int sata_wt;            /* --sata-writethrough: host files -> refused*/
} snap_cfg_t;

/* 0 on success; -1 with the reason on stderr. */
int snap_save(machine_t *m, const char *path, const snap_cfg_t *cfg);
int snap_load(machine_t *m, const char *path, const snap_cfg_t *cfg);

#endif
