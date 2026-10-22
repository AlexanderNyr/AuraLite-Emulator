// tests/test_acpi.c -- KERNEL-BOOT K3 unit lane for the fabricated ACPI
// table set (src/acpi.c). Positive rows assert the byte contract a guest
// parser can rely on; the negative rows prove the builder's checksums
// actually detect tampering and that misfit windows are refused.
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "../src/acpi.h"

static uint8_t sum8(const uint8_t *p, uint32_t n) {
    uint8_t s = 0; for (uint32_t i = 0; i < n; i++) s = (uint8_t)(s + p[i]);
    return s;
}

static uint32_t le32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static uint64_t le64(const uint8_t *p) {
    return (uint64_t)le32(p) | ((uint64_t)le32(p + 4) << 32);
}

int main(void) {
    static uint8_t ram[64 * 1024];
    const uint64_t base = 0x1000;

    /* fit guards (negative rows for the builder itself) */
    assert(acpi_build_table_set(ram, sizeof ram, 0x1234) == 0);   /* unaligned */
    assert(acpi_build_table_set(ram, 0x1800, 0x1000) == 0);      /* too small */
    assert(acpi_build_table_set(NULL, sizeof ram, base) == 0);   /* no RAM */

    assert(acpi_build_table_set(ram, sizeof ram, base) == base);
    const uint8_t *w = ram + base;

    /* RSDP rev 2 */
    const uint8_t *rsdp = w;
    assert(memcmp(rsdp, "RSD PTR ", 8) == 0);
    assert(rsdp[8] == 2);
    assert(sum8(rsdp, 20) == 0);
    assert(sum8(rsdp, 36) == 0);
    assert(le32(rsdp + 16) == base + 0x040);
    assert(le32(rsdp + 20) == 36);
    assert(le64(rsdp + 24) == base + 0x080);

    /* RSDT / XSDT: FACP then MADT (PM P2), valid checksums */
    const uint8_t *rsdt = w + 0x040;
    assert(memcmp(rsdt, "RSDT", 4) == 0);
    assert(le32(rsdt + 4) == 44);
    assert(sum8(rsdt, 44) == 0);
    assert(le32(rsdt + 36) == base + 0x900);
    assert(le32(rsdt + 40) == base + 0x0C0);

    const uint8_t *xsdt = w + 0x080;
    assert(memcmp(xsdt, "XSDT", 4) == 0);
    assert(le32(xsdt + 4) == 52);
    assert(sum8(xsdt, 52) == 0);
    assert(le64(xsdt + 36) == base + 0x900);
    assert(le64(xsdt + 44) == base + 0x0C0);

    /* PM P2: the FADT advertises exactly what pm.c implements */
    const uint8_t *fadt = w + 0x900;
    assert(memcmp(fadt, "FACP", 4) == 0);
    assert(le32(fadt + 4) == 244 && fadt[8] == 4);
    assert(sum8(fadt, 244) == 0);
    assert(le32(fadt + 40) == base + 0xA00);       /* DSDT pointer    */
    assert(le64(fadt + 140) == base + 0xA00);      /* X_DSDT          */
    assert((fadt[46] | (fadt[47] << 8)) == 9);     /* SCI_INT = IRQ9  */
    assert(le32(fadt + 48) == 0);                  /* SMI_CMD: no SMM */
    assert(le32(fadt + 56) == 0xB000);             /* PM1a_EVT_BLK    */
    assert(le32(fadt + 64) == 0xB004);             /* PM1a_CNT_BLK    */
    assert(le32(fadt + 76) == 0xB008);             /* PM_TMR_BLK      */
    assert(le32(fadt + 80) == 0xB020);             /* GPE0_BLK        */
    assert(fadt[88] == 4 && fadt[89] == 2 && fadt[91] == 4 && fadt[92] == 8);
    assert((le32(fadt + 112) & 0x100) == 0);       /* 24-bit timer    */
    assert((le32(fadt + 112) & 0x400) != 0);       /* RESET_REG_SUP   */
    assert(fadt[116] == 1 && fadt[117] == 8 && le64(fadt + 120) == 0xCF9);
    assert(fadt[128] == 0x0E);                     /* RESET_VALUE     */
    /* the X_ GAS mirrors agree with the 32-bit fields */
    assert(fadt[148] == 1 && le64(fadt + 152) == 0xB000);
    assert(fadt[172] == 1 && le64(fadt + 176) == 0xB004);
    assert(fadt[208] == 1 && le64(fadt + 212) == 0xB008);
    assert(fadt[220] == 1 && le64(fadt + 224) == 0xB020);

    /* and the DSDT is exactly \_S5 = Package(7, 0) + \_S1 =
     * Package(1, 0): the two sleep types pm.c decodes, pinned
     * byte-for-byte to the decoder (PM P1/P3). */
    const uint8_t *dsdt = w + 0xA00;
    assert(memcmp(dsdt, "DSDT", 4) == 0);
    assert(le32(dsdt + 4) == 60);
    assert(sum8(dsdt, 60) == 0);
    static const uint8_t ss_aml[24] = {
        0x08, '_', 'S', '5', '_', 0x12, 0x06, 0x02, 0x0A, 0x07, 0x0A, 0x00,
        0x08, '_', 'S', '1', '_', 0x12, 0x06, 0x02, 0x0A, 0x01, 0x0A, 0x00
    };
    assert(memcmp(dsdt + 36, ss_aml, 24) == 0);

    /* MADT walk: LAPIC(0), IOAPIC(1 @ 0xFEC00000, GSI 0), ISO(IRQ0 -> GSI2) */
    const uint8_t *madt = w + 0x0C0;
    assert(memcmp(madt, "APIC", 4) == 0);
    uint32_t len = le32(madt + 4);
    assert(len == acpi_madt_len());
    assert(sum8(madt, len) == 0);
    assert(le32(madt + 36) == 0xFEE00000u);

    const uint8_t *e = madt + 44, *end = madt + len;
    int saw_lapic = 0, saw_ioapic = 0, saw_iso = 0;
    while (e < end) {
        assert(e[1] >= 8);                       /* sane entry length */
        if (e[0] == 0) {
            assert(e[2] == 0 && e[3] == 0 && le32(e + 4) == 1);
            saw_lapic = 1;
        } else if (e[0] == 1) {
            assert(e[1] == 12);
            assert(le32(e + 4) == 0xFEC00000u);
            assert(le32(e + 8) == 0);
            saw_ioapic = 1;
        } else if (e[0] == 2) {
            assert(e[1] == 10);
            assert(e[2] == 0 && e[3] == 0);      /* bus 0, source IRQ0 */
            assert(le32(e + 4) == 2);            /* GSI 2 */
            saw_iso = 1;
        }
        e += e[1];
    }
    assert(e == end);                            /* entries tile exactly */
    assert(saw_lapic && saw_ioapic && saw_iso);

    /* negative control: one flipped byte must break the table checksum */
    for (int t = 0; t < 5; t++) {
        uint8_t *tab = (uint8_t *)(t == 0 ? madt : t == 1 ? rsdt : t == 2 ? xsdt
                                                           : t == 3 ? fadt : dsdt);
        uint32_t  n  = le32(tab + 4);
        uint8_t save = tab[n - 1];
        tab[n - 1] ^= 0xA5;
        assert(sum8(tab, n) != 0);
        tab[n - 1] = save;
        assert(sum8(tab, n) == 0);
        (void)tab;
    }

    puts("test-acpi: all vectors passed");
    return 0;
}
