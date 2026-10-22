// acpi.c -- KERNEL-BOOT K3: fabricate RSDP/RSDT/XSDT/MADT for the kernel
// lane. Layout constants are kept local; the byte offsets follow the ACPI
// spec (5.2 table formats) closely enough for a parser walk, which is all
// the kernel does with them (ioapic.c: RSDP -> RSDT|XSDT -> first MADT
// type-1 entry + type-2 ISO overrides).
#include <string.h>
#include "acpi.h"
#include "pm.h"          /* PM_DEFAULT_BASE: the FADT must advertise it */

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
/* PM P2: FADT after the largest MADT (255 CPUs end at 0x8FA). */
#define FADT_OFF 0x900
#define DSDT_OFF 0xA00
#define FADT_LEN 244u           /* revision 4 (ACPI 5.0 shape) */

/* The DSDT carries the two sleep objects src/pm.c decodes: \_S5 =
 * Package(7, 0) (PM P1) and \_S1 = Package(1, 0) (PM P3, the
 * zero-duration suspend).  Hand-encoded AML: 08 <NameSeg> 12 06 02
 * 0A <typ> 0A 00 -- NameOp, PackageOp (PkgLength 6, NumElements 2,
 * ByteConst typ, ByteConst 0). */
static const uint8_t DSDT_AML[] = {
    0x08, '_', 'S', '5', '_', 0x12, 0x06, 0x02, 0x0A, 0x07, 0x0A, 0x00,
    0x08, '_', 'S', '1', '_', 0x12, 0x06, 0x02, 0x0A, 0x01, 0x0A, 0x00
};
#define DSDT_LEN (36u + (uint32_t)sizeof DSDT_AML)

/* One Generic Address Structure pointing at an I/O port register. */
static void gas_io(uint8_t *g, unsigned width_bits, uint64_t addr) {
    memset(g, 0, 12);
    g[0] = 1;                    /* System I/O space */
    g[1] = (uint8_t)width_bits;
    g[2] = 0;                    /* bit offset */
    g[3] = width_bits == 8 ? 1 : width_bits == 16 ? 2 : width_bits == 32 ? 3 : 4;
    LE64(g + 4, addr);
}

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

    uint32_t rsdt_len = 36 + 2 * 4;    /* FACP, APIC (PM P2) */
    uint32_t xsdt_len = 36 + 2 * 8;

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

    /* PM P2: DSDT (just \_S5) and the FADT that points at it. The FADT
     * advertises exactly what pm.c implements: PM1a EVT/CNT + PM Timer
     * + GPE0, SCI on IRQ9, SMI_CMD=0 (no SMM exists here -- the honest
     * no-op), 24-bit timer, C2/C3 unsupported latencies, and the 0xCF9
     * reset register the chipset already decodes (0x0E = SYS_RST |
     * RST_CPU). Offsets per the revision-4 FADT layout. */
    uint8_t *dsdt = w + DSDT_OFF;
    hdr(dsdt, "DSDT", DSDT_LEN, 1);
    memcpy(dsdt + 36, DSDT_AML, sizeof DSDT_AML);
    acpi_checksum(dsdt, DSDT_LEN);

    uint8_t *fadt = w + FADT_OFF;
    hdr(fadt, "FACP", FADT_LEN, 4);
    const uint64_t pb = PM_DEFAULT_BASE;
    LE32(fadt + 40, (uint32_t)(base + DSDT_OFF));        /* DSDT (32-bit) */
    LE16(fadt + 46, 9);                                  /* SCI_INT       */
    LE32(fadt + 56, (uint32_t)pb);                       /* PM1a_EVT_BLK  */
    LE32(fadt + 64, (uint32_t)(pb + 4));                 /* PM1a_CNT_BLK  */
    LE32(fadt + 76, (uint32_t)(pb + 8));                 /* PM_TMR_BLK    */
    LE32(fadt + 80, (uint32_t)(pb + 0x20));              /* GPE0_BLK      */
    fadt[88] = 4; fadt[89] = 2;                          /* EVT/CNT bytes */
    fadt[91] = 4;                                        /* TMR bytes     */
    fadt[92] = 8;                                        /* GPE0: 4+4     */
    LE16(fadt + 96, 0x0FFF);                             /* C2 latency: no*/
    LE16(fadt + 98, 0x0FFF);                             /* C3 latency: no*/
    LE16(fadt + 109, 0x0003);            /* legacy devices + 8042 present */
    LE32(fadt + 112, 0x00000405u);       /* WBINVD | C1 | RESET_REG      */
    gas_io(fadt + 116, 8, 0xCF9);                        /* RESET_REG     */
    fadt[128] = 0x0E;                                    /* RESET_VALUE   */
    LE64(fadt + 140, base + DSDT_OFF);                   /* X_DSDT        */
    gas_io(fadt + 148, 32, pb);                          /* X_PM1a_EVT    */
    gas_io(fadt + 172, 16, pb + 4);                      /* X_PM1a_CNT    */
    gas_io(fadt + 208, 32, pb + 8);                      /* X_PM_TMR      */
    gas_io(fadt + 220, 64, pb + 0x20);                   /* X_GPE0        */
    acpi_checksum(fadt, FADT_LEN);

    /* RSDT: FACP first (the PM discovery path), then MADT. */
    uint8_t *rsdt = w + RSDT_OFF;
    hdr(rsdt, "RSDT", rsdt_len, 1);
    LE32(rsdt + 36, base + FADT_OFF);
    LE32(rsdt + 40, base + MADT_OFF);
    acpi_checksum(rsdt, rsdt_len);

    /* XSDT: the same pair in 64-bit. */
    uint8_t *xsdt = w + XSDT_OFF;
    hdr(xsdt, "XSDT", xsdt_len, 1);
    LE64(xsdt + 36, base + FADT_OFF);
    LE64(xsdt + 44, base + MADT_OFF);
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
