/* tests/kload_fixture.c -- KERNEL-BOOT K1: synthetic guest kernel.
 *
 * A minimal freestanding "kernel" that exercises the exact boot contract
 * the real AuraLite-OS kernel expects from its loaders (the parts K1
 * fabricates): it is higher-half linked at the AuraLite VMA, entered in
 * long mode with paging on and RDI = physical boot_info_t pointer.
 *
 * It validates, through the three mapping flavours the loader must
 * provide, the fields the real kernel reads first:
 *   - boot_info magic + fb geometry via the IDENTITY map (low phys),
 *   - the same magic through the HHDM (boot_info_init's own latch path),
 * and then reports the verdict over COM1: "K1OK\n" or "K1BAD\n".
 * The harness writes to 0x3F8 byte-wise exactly like the real kernel's
 * early uart_init-less debug path does.
 */
#include <stdint.h>

#define BI_PHYS  0x00020000ULL           /* KLOAD_INFO_PHYS */
#define HHDM     0xFFFF800000000000ULL   /* KLOAD_HHDM */
#define MAGIC    0x4155524142544C44ULL   /* BOOT_MAGIC, "AURABLTD" LE */

static void outb(uint16_t port, uint8_t val) {
    __asm__ volatile ("outb %0, %1" : : "a"(val), "dN"(port));
}

/* pinned to .text._start: the linker script leads the segment with it */
__attribute__((section(".text._start")))
void _start(void) {
    volatile uint64_t *ident = (volatile uint64_t *)BI_PHYS;
    volatile uint64_t *hhdm  = (volatile uint64_t *)(HHDM + BI_PHYS);

    /* boot_info.fb.width sits at offset 8 (fb.phys_base) + 8 = 16 */
    volatile uint32_t *id_fb_w = (volatile uint32_t *)(BI_PHYS + 16);
    volatile uint32_t *hh_fb_w = (volatile uint32_t *)(HHDM + BI_PHYS + 16);

    int ok = (*ident == MAGIC) && (*hhdm == MAGIC) &&
             (*id_fb_w == 800u) && (*hh_fb_w == 800u);

    const char *msg = ok ? "K1OK\n" : "K1BAD\n";
    for (const char *p = msg; *p; p++)
        outb(0x3F8, (uint8_t)*p);

    for (;;)
        __asm__ volatile ("cli; hlt");
}
