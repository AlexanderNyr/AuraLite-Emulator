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

    /* RSDT / XSDT: one MADT pointer each, valid checksums */
    const uint8_t *rsdt = w + 0x040;
    assert(memcmp(rsdt, "RSDT", 4) == 0);
    assert(le32(rsdt + 4) == 40);
    assert(sum8(rsdt, 40) == 0);
    assert(le32(rsdt + 36) == base + 0x0C0);

    const uint8_t *xsdt = w + 0x080;
    assert(memcmp(xsdt, "XSDT", 4) == 0);
    assert(le32(xsdt + 4) == 44);
    assert(sum8(xsdt, 44) == 0);
    assert(le64(xsdt + 36) == base + 0x0C0);

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
    for (int t = 0; t < 3; t++) {
        uint8_t *tab = (uint8_t *)(t == 0 ? madt : t == 1 ? rsdt : xsdt);
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
