// acpi.c -- KERNEL-BOOT K3: fabricate RSDP/RSDT/XSDT/MADT for the kernel
// lane. Layout constants are kept local; the byte offsets follow the ACPI
// spec (5.2 table formats) closely enough for a parser walk, which is all
// the kernel does with them (ioapic.c: RSDP -> RSDT|XSDT -> first MADT
// type-1 entry + type-2 ISO overrides).
#include <string.h>
#include "acpi.h"

#define LE16(p, v)  do { (p)[0]=(uint8_t)(v); (p)[1]=(uint8_t)((v)>>8); } while(0)
#define LE32(p, v)  do { (p)[0]=(uint8_t)(v); (p)[1]=(uint8_t)((v)>>8); (p)[2]=(uint8_t)((v)>>16); (p)[3]=(uint8_t)((v)>>24); } while(0)
#define LE64(p, v)  do { LE32(p,(uint32_t)(v)); LE32((p)+4,(uint32_t)((uint64_t)(v)>>32)); } while(0)

#define OEMID     "AURALT"   /* 6 */
#define OEMTABLE  "AURALT  " /* 8 */
#define CREATOR   "ARNA"     /* 4 */

/* Common 36-byte table header. */
static void hdr(uint8_t *t, const char sig[4], uint32_t len, uint8_t rev) {
    memset(t, 0, 36);
    memcpy(t + 0, sig, 4);
    LE32(t + 4, len);
    t[8] = rev;
    /* t[9] checksum: last, per-table */
    memcpy(t + 10, OEMID, 6);
    memcpy(t + 16, OEMTABLE, 8);
    LE32(t + 24, 1);             /* OEM revision */
    memcpy(t + 28, CREATOR, 4);
    LE32(t + 32, 1);             /* creator revision */
}

void acpi_checksum(uint8_t *table, uint32_t len) {
    uint8_t sum = 0;
    for (uint32_t i = 0; i < len; i++) sum = (uint8_t)(sum + table[i]);
    table[9] = (uint8_t)(0 - sum);   /* header checksum lives at +9 */
}

uint32_t acpi_madt_len(void) { return ACPI_MADT_LEN; }

/* KERNEL-BOOT K5 (--cpus>1): MADT length with `ncpus` LAPIC (type 0)
 * entries; the first is the BSP (id 0), ids 1..ncpus-1 are the APs the
 * kernel's smp_init() will try to INIT-SIPI-SIPI wake. */
uint32_t acpi_madt_len_for(int ncpus) {
    if (ncpus < 1) ncpus = 1;
    return ACPI_MADT_LEN + 8u * (uint32_t)(ncpus - 1);
}

#define RSDP_OFF 0x000
#define RSDT_OFF 0x040
#define XSDT_OFF 0x080
#define MADT_OFF 0x0C0

uint64_t acpi_build_table_set(uint8_t *ram, uint64_t ram_size, uint64_t base) {
    return acpi_build_table_set_smp(ram, ram_size, base, 1);
}

uint64_t acpi_build_table_set_smp(uint8_t *ram, uint64_t ram_size,
                                  uint64_t base, int ncpus) {
    if (ncpus < 1) ncpus = 1;
    if (ncpus > 255) ncpus = 255;
    if (!ram || (base & 0xFFF) != 0 || base + ACPI_SET_SIZE > ram_size) return 0;
    uint8_t *w = ram + base;
    memset(w, 0, ACPI_SET_SIZE);

    uint32_t rsdt_len = 36 + 4;
    uint32_t xsdt_len = 36 + 8;

    /* MADT: header + LAPIC x ncpus + IOAPIC(1@0xFEC00000, GSI base 0)
     *       + ISO(IRQ0 -> GSI2). */
    uint32_t madt_len = acpi_madt_len_for(ncpus);
    uint8_t *madt = w + MADT_OFF;
    hdr(madt, "APIC", madt_len, 1);
    LE32(madt + 36, 0xFEE00000u); /* local APIC address */
    LE32(madt + 40, 1);           /* flags: dual-8259 installed */
    uint8_t *p = madt + 44;
    for (int i = 0; i < ncpus; i++) {
        p[0] = 0; p[1] = 8; p[2] = (uint8_t)i; p[3] = (uint8_t)i;
        LE32(p + 4, 1);           /* LAPIC uid=i id=i enabled */
        p += 8;
    }
    p[0] = 1; p[1] = 12; p[2] = 1; p[3] = 0; LE32(p + 4, 0xFEC00000u); LE32(p + 8, 0); p += 12;
    p[0] = 2; p[1] = 10; p[2] = 0; p[3] = 0; LE32(p + 4, 2); p += 10;   /* IRQ0 -> GSI2, default flags */
    acpi_checksum(madt, madt_len);

    /* RSDT: one pointer (MADT). */
    uint8_t *rsdt = w + RSDT_OFF;
    hdr(rsdt, "RSDT", rsdt_len, 1);
    LE32(rsdt + 36, base + MADT_OFF);
    acpi_checksum(rsdt, rsdt_len);

    /* XSDT: one pointer (MADT). */
    uint8_t *xsdt = w + XSDT_OFF;
    hdr(xsdt, "XSDT", xsdt_len, 1);
    LE64(xsdt + 36, base + MADT_OFF);
    acpi_checksum(xsdt, xsdt_len);

    /* RSDP rev 2: to remain livable for rev-0 parsers RsdtAddress is
     * filled too, with XsdtAddress carrying the 64-bit pointer. */
    uint8_t *rsdp = w + RSDP_OFF;
    memcpy(rsdp, "RSD PTR ", 8);
    /* rsdp[9] checksum over the first 20 bytes */
    memcpy(rsdp + 10, OEMID, 6);
    rsdp[8] = 2;                 /* revision: 2 => XSDT present */
    LE32(rsdp + 16, base + RSDT_OFF);
    LE32(rsdp + 20, 36);
    LE64(rsdp + 24, base + XSDT_OFF);
    /* extended checksum over all 36 bytes */
    {
        uint8_t s20 = 0;
        for (int i = 0; i < 20; i++) s20 = (uint8_t)(s20 + rsdp[i]);
        rsdp[9] = (uint8_t)(0 - s20);
        uint8_t s36 = 0;
        for (int i = 0; i < 36; i++) s36 = (uint8_t)(s36 + rsdp[i]);
        rsdp[32] = (uint8_t)(0 - s36);
        rsdp[33] = rsdp[34] = rsdp[35] = 0;
    }
    return base;
}
