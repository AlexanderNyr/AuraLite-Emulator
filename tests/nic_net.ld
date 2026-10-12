/* tests/nic_net.ld -- NET N2 wire payload link script.
 *
 * Same contract as tests/kload_fixture.ld, which is the K1 artifact: link at
 * the real AuraLite VMA (KLOAD_KERNEL_VMA + 1 MiB) so kload_boot()'s
 * segment-math path is exercised faithfully rather than around a friendlier
 * address, and keep p_paddr == p_vaddr (the GNU ld ET_EXEC default) because
 * kload_boot() validates exactly that.
 *
 * Why this transport and not the firmware one N1 used: the firmware's USB
 * mass-storage loader reads ONE 512-byte block off LBA 0, and its long-mode
 * map only covers 0x100000..0x1FFFFF of RAM (PD[0] = 0x19B is a 2 MiB page --
 * measured in N1).  A DHCP + ARP + ICMP client with two descriptor rings does
 * not fit in either budget.  kload_boot() gives a freestanding C payload a
 * 4 GiB identity map plus the HHDM, entered in long mode with paging on.
 */
OUTPUT_FORMAT("elf64-x86-64")
OUTPUT_ARCH(i386:x86-64)
ENTRY(_start)

SECTIONS
{
    . = 0xFFFFFFFF80100000;

    .text : {
        *(.text._start)   /* _start must lead: do not rely on traversal order */
        *(.text*)
    }
    . = ALIGN(0x1000);
    .rodata : { *(.rodata*) }
    . = ALIGN(0x1000);
    .data : { *(.data*) *(.got*) }
    .bss : { *(.bss*) *(COMMON) }
}
