/* tests/kload_fixture.ld -- links the synthetic kernel at the exact VMA
 * the real AuraLite kernel.ld uses (KERNEL_VMA + 1 MiB), so the loader's
 * segment-math path is exercised faithfully, not around a friendlier
 * address. p_paddr == p_vaddr by GNU ld default for ET_EXEC, which is the
 * convention kload_boot() validates.
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
