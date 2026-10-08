#ifndef AURALITE_EMU_AHCI_H
#define AURALITE_EMU_AHCI_H

/* STORE plan: AHCI HBA (Intel ICH9-class 0x1E03, class 01/06/01).
 *
 * S1 scope: the register model behind BAR5 (ABAR MMIO window) -- global
 * CAP/GHC/PI/VS, per-port presence (PxSSTS DET, PxSIG), the COMRESET pulse
 * the guest drives through PxSCTL, and the stop/start handshake
 * (PxCMD.ST/FRE mirrored into CR/FR immediately, exactly fast enough for
 * the guest's spin loops).  No command engine: a PxCI write latches and
 * the machine logs it once -- the guest's own timeout path is the S1
 * receipt (reads/writes land in S2/S3).
 */

#include "machine.h"

#define AHCI_ABAR        0xFEB10000u
#define AHCI_ABAR_SIZE   0x1000u
#define AHCI_HW_PORTS    6            /* CAP.NP = 5 (0-based) */

/* Registers + window. Idempotent; safe to call from devices_init_common
 * after the PCI function and BAR5 are wired up by the caller. */
void ahci_register(machine_t *m);

#endif
