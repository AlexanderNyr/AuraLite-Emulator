// acpi.h -- KERNEL-BOOT K3: minimal ACPI table set for the kernel lane.
//
// The AuraLite-OS kernel's ioapic.c walks RSDP -> RSDT|XSDT -> MADT and
// compares the type-1 (I/O APIC) entry against its PC-standard hardcode,
// printing "MADT agree" / "MADT says 0x..., hardcode stands" / "no MADT".
// Both of the kernel's own loaders publish rsdp_phys, so our kloader does
// the same: it flashes a tiny but canonical set (RSDP rev 2 with both
// RSDT and XSDT, one MADT) into reserved guest RAM and points boot_info at
// it. The set carries exactly the machine we build: BSP LAPIC 0, one
// I/O APIC at 0xFEC00000 with GSI base 0, and the ISA IRQ0->GSI2 interrupt
// source override every PC carries (pit.c drives that pin, measured in K2).
#ifndef KLOAD_ACPI_H
#define KLOAD_ACPI_H

#include <stdint.h>
#include <stddef.h>

/* One page covers the whole set; layout (aligned inside):
 *   +0x000  RSDP (36 bytes, rev 2)
 *   +0x040  RSDT (36 + 4*N)
 *   +0x080  XSDT (36 + 8*N)
 *   +0x0C0  MADT (44 + entries = 74 bytes)
 * All checksum bytes are filled so the 8-bit sum of each table is 0. */
#define ACPI_SET_SIZE 0x1000ULL
#define ACPI_MADT_LEN 74u

/* Flash the set at `base` (page-aligned) inside the guest RAM backing
 * store. Returns `base` (the RSDP physical address) on success, 0 if the
 * window does not fit. Builder writes are off-bus (like the rest of the
 * kloader's structures): the same note in kloader.c applies. */
uint64_t acpi_build_table_set(uint8_t *ram, uint64_t ram_size, uint64_t base);

/* KERNEL-BOOT K5: same table set but with `ncpus` LAPIC entries in the
 * MADT (1 = the historical UP table set; kloader passes m->cfg_cpus). */
uint64_t acpi_build_table_set_smp(uint8_t *ram, uint64_t ram_size,
                                  uint64_t base, int ncpus);

/* Pure helpers, exported for the unit lane (tests/test_acpi.c). */
void     acpi_checksum(uint8_t *table, uint32_t len);
uint32_t acpi_madt_len(void);
uint32_t acpi_madt_len_for(int ncpus);

#endif
