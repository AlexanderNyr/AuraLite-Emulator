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
#define AHCI_ABAR2       0xFEB12000u   /* S4: --ahci2 controller; the guest maps
                                        * 8 KiB per BAR5 (measured ahci.c), so
                                        * the two windows sit 8 KiB apart */
#define AHCI_ABAR_SIZE   0x1000u
#define AHCI_HW_PORTS    6            /* CAP.NP = 5 (0-based) */

/* S5: INTx lines.  Board wire: controller N asserts ISA line AHCI_IRQ0+N
 * (legacy IDE pairing 14/15) through pic_set_irq(), which fans the same
 * line out to the I/O APIC (CHIPSET H7).  The line is held while the AHCI
 * interrupt condition holds: (PxIS & PxIE) nonzero on any port, or
 * HBA IS nonzero with GHC.IE (AHCI 1.3 interrupt logic).  The guest
 * driver polls with PxIE=0 and never sets GHC.IE (measured), so the line
 * is machine honesty -- pinned by unit vectors, invisible to receipts. */
#define AHCI_IRQ0        14
#define AHCI_IRQ1        15

/* S5: host write-through for one attach slot (the --sata-writethrough
 * lane).  Default policy stays copy-on-attach in-memory -- the S3
 * receipts pin the pristine-host fact and the harness asserts it.  With a
 * write-through file attached, every guest WRITE DMA EXT byte also lands
 * in the host image at the same offset.  close_all flushes and closes
 * every slot (main teardown and the unit fixture both call it). */
int  ahci_wt_attach(machine_t *m, int ctrl, int port, const char *path);
void ahci_wt_close_all(void);

/* Registers + window.  S4: ahci_register() is controller 0 (the historical
 * entry point, every S1-S3 test calls it); ahci_register_ctrl() brings up
 * controller ctrl < AHCI_CTRLS with its own ABAR window and port files.
 * Idempotent per controller; safe to call from devices_init_common after
 * the PCI function and BAR5 are wired up by the caller. */
void ahci_register(machine_t *m);
void ahci_register_ctrl(machine_t *m, int ctrl);

#endif
