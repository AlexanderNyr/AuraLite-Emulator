// kloader.h -- KERNEL-BOOT K1: direct kernel-load lane.
//
// Lets the CLI boot the north-star guest (AuraLite-OS kernel.elf) without
// any firmware in between: parses the higher-half ELF64, builds the paging
// hierarchy the kernel's boot contract demands (identity + HHDM + kernel
// VMA), fabricates the AuraLite boot_info_t handoff block, and parks the
// CPU at the ELF entry ready to execute _start.
//
// The contract implemented here is neither guessed nor paraphrased: it is
// measured from AuraLite-OS @ 0ed0d29 (MIT), concretely from
// boot/shared/boot_info.h (ABI + magic + struct layout, offsets pinned by
// _Static_assert against that very header) and kernel/arch/x86_64/boot.asm
// (long mode, paging on, higher half pre-mapped, RDI = physical
// boot_info_t*, IF=0, any RSP).
//
// Page-table shape K1 builds: identity view and HHDM view of the low 4 GiB
// share ONE PDPT holding four 1 GiB PS entries (both views are byte-
// identical 1:1 maps), plus the kernel's own higher-half range mapped with
// 4 KiB PTEs -- seven table pages in total for the AuraLite kernel image.
#ifndef KLOADER_H
#define KLOADER_H
#include "machine.h"

/* Physical layout of the loader-provided low-memory block. Everything lives
 * below the ROM-alias shadow window (0xE0000, see machine.h), in the RAM
 * region real loaders call "conventional". The memmap we publish marks
 * [KLOAD_PT_BASE, KLOAD_MEMMAP_BR) as BOOT_MEM_BOOTLOADER and hands the
 * rest to the kernel's PMM as USABLE. */
#define KLOAD_PT_BASE     0x00010000ULL  /* PML4 + friends, bump-allocated up */
#define KLOAD_PT_LIMIT    0x00020000ULL  /* hard guard: tables stay < boot_info */
#define KLOAD_KERNEL_MIN  0x00100000ULL  /* lowest legal kernel phys address */
#define KLOAD_INFO_PHYS   0x00020000ULL  /* AuraLite boot_info_t (7776 bytes) */
#define KLOAD_STACK_TOP   0x00070000ULL  /* temporary RSP for boot.asm */

#define KLOAD_MEMMAP_BR   0x00021000ULL  /* end of BOOTLOADER memmap region */

/* Architectural constants of the north-star guest, not ours: the kernel's
 * link base (kernel.ld KERNEL_VMA) and the x86_64 HHDM offset the
 * boot_info ABI mandates. */
#define KLOAD_KERNEL_VMA  0xFFFFFFFF80000000ULL
#define KLOAD_HHDM        0xFFFF800000000000ULL

/* Loads the ELF64 x86-64 kernel at `elf_path` and prepares the machine for
 * immediate execution. On success returns 0 with the CPU standing at the
 * kernel entry in 64-bit long mode, paging on; m->kmain_va holds the
 * address of the `kmain` symbol when a symtab is present (0 otherwise),
 * used by the CLI as the "C entry reached" run marker. On failure logs the
 * exact reason (mlog) and returns -1 with the machine untouched except for
 * power-on RAM contents. */
int kload_boot(machine_t *m, const char *elf_path, const char *initrd_path);

#endif
