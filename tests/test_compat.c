/* tests/test_compat.c -- COMPAT G4: LPC identity pairing vectors.
 *
 * SeaBIOS rel-1.16.3's acpi_setup() recognizes only two LPC functions as
 * ACPI-capable: PIIX4 PM 8086:7113 and ICH9 LPC 8086:2918.  This machine
 * advertises a Q35 host bridge (8086:29c0) but the historical LPC identity
 * is Panther Point 8086:1e55 (the ID the bundled sample firmware knows), so
 * stock SeaBIOS skips RSDP publication.  --lpc=ich9 switches the LPC
 * function to the ICH9 identity that pairs with the advertised host bridge;
 * the default must stay byte-identical for every historical lane.
 *
 * Vectors:
 *  1. default: LPC 0:31.0 is 8086:1e55
 *  2. default: class 06/01 and the multifunction header bit are intact
 *  3. ich9:    LPC is 8086:2918
 *  4. ich9:    class/subclass/header are unchanged by the switch
 *  5. both:    the host bridge stays 8086:29c0 (PAM ownership)
 *  6. both:    AHCI 0:31.2 stays 8086:1e03 (function 2 untouched)
 *  7. ich9:    Q35 PAM decode still works under the ICH9 LPC identity
 */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "devices.h"
#include "harness.h"
#include "pci.h"
#include "ahci.h"

#define PCI_CFG_ADDR 0xCF8
#define PCI_CFG_DATA 0xCFC

typedef struct { machine_t m; } fx_t;

static void fx_init(fx_t *f, int lpc_ich9) {
    setup_machine(&f->m);
    f->m.cfg_lpc_ich9 = lpc_ich9;      /* consumed by devices_init_common */
    devices_init_common(&f->m);
}

static void fx_done(fx_t *f) {
    devices_done(&f->m);
}

static uint32_t cfg32(machine_t *m, unsigned bus, unsigned dev, unsigned func,
                      unsigned reg) {
    io_write(m, PCI_CFG_ADDR, 4,
             0x80000000u | (bus << 16) | (dev << 11) | (func << 8) | (reg & 0xFCu));
    return io_read(m, PCI_CFG_DATA, 4);
}

static void test_lpc_identity(void) {
    fx_t f;
    fx_init(&f, 0);
    uint32_t id = cfg32(&f.m, 0, 31, 0, 0x00);
    assert((id & 0xFFFF) == 0x8086);            /* vector 1 */
    assert((id >> 16) == 0x1E55);
    uint32_t cls = cfg32(&f.m, 0, 31, 0, 0x08);
    assert(((cls >> 16) & 0xFF) == 0x01);       /* subclass: ISA bridge  */
    assert(((cls >> 24) & 0xFF) == 0x06);       /* class: bridge         */
    uint32_t hdr = cfg32(&f.m, 0, 31, 0, 0x0C);
    assert(((hdr >> 16) & 0xFF) == 0x80);       /* vector 2: multifunction */
    assert(cfg32(&f.m, 0, 0, 0, 0x00) == 0x29C08086u);  /* vector 5 */
    assert(cfg32(&f.m, 0, 31, 2, 0x00) == 0x1E038086u); /* vector 6 */
    fx_done(&f);

    fx_init(&f, 1);
    id = cfg32(&f.m, 0, 31, 0, 0x00);
    assert((id & 0xFFFF) == 0x8086);            /* vector 3 */
    assert((id >> 16) == 0x2918);
    cls = cfg32(&f.m, 0, 31, 0, 0x08);
    assert(((cls >> 16) & 0xFF) == 0x01);       /* vector 4 */
    assert(((cls >> 24) & 0xFF) == 0x06);
    hdr = cfg32(&f.m, 0, 31, 0, 0x0C);
    assert(((hdr >> 16) & 0xFF) == 0x80);
    assert(cfg32(&f.m, 0, 0, 0, 0x00) == 0x29C08086u);
    assert(cfg32(&f.m, 0, 31, 2, 0x00) == 0x1E038086u);

    /* vector 7: PAM is owned by the host bridge, so the LPC identity must
     * not disturb it.  Open the F0000-FFFFF window for RAM reads/writes,
     * store through the bus, read back; then close it and observe flash. */
    f.m.rom[0x10000] = 0x5A;                    /* ROM byte at 0xF0000 */
    io_write(&f.m, PCI_CFG_ADDR, 4, 0x80000000u | PCI_Q35_PAM0);
    io_write(&f.m, PCI_CFG_DATA, 1, 0x30);      /* PAM0 bits 5:4 = 11 */
    mem_write(&f.m, 0xF0000, 1, 0x77);
    assert(mem_read(&f.m, 0xF0000, 1) == 0x77);
    io_write(&f.m, PCI_CFG_DATA, 1, 0x00);      /* back to flash */
    assert(mem_read(&f.m, 0xF0000, 1) == 0x5A);
    fx_done(&f);
    puts("ok: lpc_identity_pairing");
}

int main(void) {
    test_lpc_identity();
    puts("compat vectors: 7/7 ok");
    return 0;
}
