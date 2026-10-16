/* tests/test_pam.c -- Q35 PAM BIOS-shadow vectors.
 *
 * Exercise the real host-bridge config mechanism and the physical bus
 * decoder together. Negative cases pin immutable flash/open-bus reads and
 * write-protection; positive cases pin the Q35 PAM0 BIOS window, PAM1..6
 * 16-KiB boundaries, reserved-bit masks and the top-of-4G flash alias.
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

static void fx_init(fx_t *f) {
    setup_machine(&f->m);
    devices_init_common(&f->m);
    memset(f->m.rom, 0, ROM_SIZE);
}

static void fx_done(fx_t *f) {
    devices_done(&f->m);
}

static void pci_cfg_select(machine_t *m, unsigned reg) {
    io_write(m, PCI_CFG_ADDR, 4, 0x80000000u | (reg & 0xFCu));
}

static uint8_t pci_bdf_cfg_read8(machine_t *m, unsigned bus, unsigned dev,
                                 unsigned func, unsigned reg) {
    uint32_t address = 0x80000000u | (bus << 16) | (dev << 11)
                     | (func << 8) | (reg & 0xFCu);
    io_write(m, PCI_CFG_ADDR, 4, address);
    return (uint8_t)io_read(m, PCI_CFG_DATA + (reg & 3u), 1);
}

static uint32_t pci_bdf_cfg_read32(machine_t *m, unsigned bus, unsigned dev,
                                   unsigned func, unsigned reg) {
    uint32_t address = 0x80000000u | (bus << 16) | (dev << 11)
                     | (func << 8) | (reg & 0xFCu);
    io_write(m, PCI_CFG_ADDR, 4, address);
    return io_read(m, PCI_CFG_DATA, 4);
}

static void pci_bdf_cfg_write32(machine_t *m, unsigned bus, unsigned dev,
                                unsigned func, unsigned reg, uint32_t value) {
    uint32_t address = 0x80000000u | (bus << 16) | (dev << 11)
                     | (func << 8) | (reg & 0xFCu);
    io_write(m, PCI_CFG_ADDR, 4, address);
    io_write(m, PCI_CFG_DATA, 4, value);
}

static uint8_t pci_cfg_read8(machine_t *m, unsigned reg) {
    pci_cfg_select(m, reg);
    return (uint8_t)io_read(m, PCI_CFG_DATA + (reg & 3u), 1);
}

static void pci_cfg_write8(machine_t *m, unsigned reg, uint8_t value) {
    pci_cfg_select(m, reg);
    io_write(m, PCI_CFG_DATA + (reg & 3u), 1, value);
}

static uint8_t debug_peek(machine_t *m, uint64_t phys) {
    uint8_t value = 0;
    assert(mem_debug_peek_byte(m, phys, &value));
    return value;
}

static uint32_t pci_cfg_read32(machine_t *m, unsigned reg) {
    pci_cfg_select(m, reg);
    return io_read(m, PCI_CFG_DATA, 4);
}

static void pci_cfg_write32(machine_t *m, unsigned reg, uint32_t value) {
    pci_cfg_select(m, reg);
    io_write(m, PCI_CFG_DATA, 4, value);
}

static void test_pci_multifunction_exposes_ahci(void) {
    fx_t f; fx_init(&f);
    assert(pci_find(&f.m, 0, 31, 0) != NULL);
    assert(pci_find(&f.m, 0, 31, 2) != NULL);

    /* PCI enumerators only probe functions 1..7 when function 0 advertises
     * a multifunction header. Check through mechanism #1, then verify the
     * discovered function's AHCI class/prog-if identity. */
    assert(pci_bdf_cfg_read8(&f.m, 0, 31, 0, 0x0E) == 0x80);
    assert(pci_bdf_cfg_read8(&f.m, 0, 31, 2, 0x00) == 0x86);
    assert(pci_bdf_cfg_read8(&f.m, 0, 31, 2, 0x01) == 0x80);
    assert(pci_bdf_cfg_read8(&f.m, 0, 31, 2, 0x09) == 0x01); /* AHCI prog-if */
    assert(pci_bdf_cfg_read8(&f.m, 0, 31, 2, 0x0A) == 0x06); /* SATA subclass */
    assert(pci_bdf_cfg_read8(&f.m, 0, 31, 2, 0x0B) == 0x01); /* storage class */

    /* SeaBIOS probes BARs with all ones, then relocates BAR5. Unimplemented
     * BAR0..4 stay hardwired zero; the 4-KiB BAR5 sizing mask is accurate and
     * the MMIO decoder follows the assigned base. */
    assert(pci_bdf_cfg_read32(&f.m, 0, 31, 2, 0x10) == 0);
    assert(pci_bdf_cfg_read32(&f.m, 0, 31, 2, 0x24) == AHCI_ABAR);
    pci_bdf_cfg_write32(&f.m, 0, 31, 2, 0x24, 0xFFFFFFFFu);
    assert(pci_bdf_cfg_read32(&f.m, 0, 31, 2, 0x24) == 0xFFFFF000u);
    pci_bdf_cfg_write32(&f.m, 0, 31, 2, 0x10, 0xFFFFFFFFu);
    assert(pci_bdf_cfg_read32(&f.m, 0, 31, 2, 0x10) == 0);
    const uint32_t moved_abar = 0xFEBFE000u;
    pci_bdf_cfg_write32(&f.m, 0, 31, 2, 0x24, moved_abar);
    assert(pci_bdf_cfg_read32(&f.m, 0, 31, 2, 0x24) == moved_abar);
    assert(mem_read(&f.m, moved_abar + 0x0C, 4) == 0x3Fu);
    assert(mem_read(&f.m, AHCI_ABAR + 0x0C, 4) != 0x3Fu);

    fx_done(&f);
    puts("ok: pci_multifunction_exposes_ahci");
}

static void test_q35_pam_shadow_and_protection(void) {
    fx_t f; fx_init(&f);
    machine_t *m = &f.m;
    pci_dev_t *hb = pci_find(m, 0, 0, 0);
    assert(hb != NULL);
    assert(pci_cfg_read8(m, 0x00) == 0x86 && pci_cfg_read8(m, 0x01) == 0x80);
    assert(pci_cfg_read8(m, 0x02) == 0xC0 && pci_cfg_read8(m, 0x03) == 0x29);
    assert(pci_cfg_read8(m, PCI_Q35_PAM0) == 0);
    assert(pci_cfg_read8(m, PCI_Q35_PAM0 + 5) == 0);

    /* E0000 starts the 128-KiB flash alias; seed distinct flash and shadow
     * contents so the selected source is observable. */
    m->rom[0x00000] = 0x11;  /* E0000 */
    m->rom[0x04000] = 0x22;  /* E4000 */
    m->rom[0x10000] = 0x33;  /* F0000 */
    m->rom[0x18000] = 0x44;  /* F8000 */
    m->rom[0x1FFFF] = 0x55;  /* FFFFF */
    m->ram[0xE0000] = 0xA1;
    m->ram[0xE4000] = 0xA2;
    m->ram[0xF0000] = 0xA3;
    m->ram[0xF8000] = 0xA4;
    m->ram[0xFFFFF] = 0xA6;
    m->ram[0xC0000] = 0xA5;

    /* PAM reset/negative path: flash reads remain visible and writes are
     * discarded. The empty C-D option-ROM window is open bus, not RAM. */
    assert(mem_read(m, 0xE0000, 1) == 0x11);
    assert(mem_read(m, 0xC0000, 1) == 0xFF);
    assert(mem_read(m, 0xF0000, 1) == 0x33);
    assert(mem_read(m, 0xF8000, 1) == 0x44);
    assert(debug_peek(m, 0xC0000) == 0xFF);
    assert(debug_peek(m, 0xF0000) == 0x33);
    mem_write(m, 0xE0000, 1, 0x51);
    mem_write(m, 0xC0000, 1, 0x52);
    mem_write(m, 0xF0000, 1, 0x53);
    mem_write(m, 0xF8000, 1, 0x54);
    assert(m->ram[0xE0000] == 0xA1 && m->ram[0xC0000] == 0xA5);
    assert(m->ram[0xF0000] == 0xA3 && m->ram[0xF8000] == 0xA4);
    assert(mem_read(m, 0xE0000, 1) == 0x11);

    /* PAM5: low 16-KiB half is read/write (3), high half is read-only (1).
     * Program it via PCI mechanism #1, not by mutating config backing. */
    uint64_t generation = m->perf_tlb_generation;
    pci_cfg_write8(m, PCI_Q35_PAM0 + 5, 0x13);
    assert(pci_cfg_read8(m, PCI_Q35_PAM0 + 5) == 0x13);
    assert(m->perf_tlb_generation != generation); /* invalidate old ROM decode */
    assert(mem_read(m, 0xE0000, 1) == 0xA1);
    assert(mem_read(m, 0xE4000, 1) == 0xA2);
    mem_write(m, 0xE0000, 1, 0xB1);
    mem_write(m, 0xE4000, 1, 0xB2);
    assert(mem_read(m, 0xE0000, 1) == 0xB1);
    assert(m->ram[0xE4000] == 0xA2 && mem_read(m, 0xE4000, 1) == 0xA2);

    /* A dword store straddling the 16-KiB boundary writes only its first
     * two bytes; the read-only half continues to accept reads from shadow. */
    m->ram[0xE3FFE] = 0; m->ram[0xE3FFF] = 0;
    m->ram[0xE4000] = 0xA2; m->ram[0xE4001] = 0xA6;
    mem_write(m, 0xE3FFE, 4, 0x44332211);
    assert(m->ram[0xE3FFE] == 0x11 && m->ram[0xE3FFF] == 0x22);
    assert(m->ram[0xE4000] == 0xA2 && m->ram[0xE4001] == 0xA6);
    assert(mem_read(m, 0xE3FFE, 4) == 0xA6A22211u);

    /* Reserved PAM bits read as zero. Writing 0xFF leaves both defined
     * fields enabled, while clearing the write bit locks the shadow again. */
    pci_cfg_write8(m, PCI_Q35_PAM0 + 5, 0xFF);
    assert(pci_cfg_read8(m, PCI_Q35_PAM0 + 5) == 0x33);
    pci_cfg_write8(m, PCI_Q35_PAM0 + 5, 0x11);
    assert(pci_cfg_read8(m, PCI_Q35_PAM0 + 5) == 0x11);
    mem_write(m, 0xE0000, 1, 0x61);
    assert(mem_read(m, 0xE0000, 1) == 0xB1); /* read-only shadow retains data */
    pci_cfg_write8(m, PCI_Q35_PAM0 + 5, 0x00);
    assert(mem_read(m, 0xE0000, 1) == 0x11); /* disable shadow reads -> flash */

    /* PAM1 independently controls the first two C0000 16-KiB regions. */
    pci_cfg_write8(m, PCI_Q35_PAM0 + 1, 0x03);
    assert(mem_read(m, 0xC0000, 1) == 0xA5);
    mem_write(m, 0xC0000, 1, 0xB5);
    assert(mem_read(m, 0xC0000, 1) == 0xB5);
    assert(mem_read(m, 0xC4000, 1) == 0xFF);

    /* PAM0 bits 5:4 cover the full 64-KiB F0000-FFFFF BIOS window.
     * SeaBIOS writes 0x30 while copying writable state, then locks it at
     * 0x10. Reserved PAM0 bits are discarded by the PCI config mechanism. */
    generation = m->perf_tlb_generation;
    pci_cfg_write8(m, PCI_Q35_PAM0, 0xFF);
    assert(pci_cfg_read8(m, PCI_Q35_PAM0) == 0x30);
    assert(m->perf_tlb_generation != generation);
    assert(mem_read(m, 0xF0000, 1) == 0xA3);
    assert(mem_read(m, 0xF8000, 1) == 0xA4);
    assert(mem_read(m, 0xFFFFF, 1) == 0xA6);
    mem_write(m, 0xF0000, 1, 0xB3);
    mem_write(m, 0xF8000, 1, 0xB4);
    mem_write(m, 0xFFFFF, 1, 0xB5);
    assert(mem_read(m, 0xF0000, 1) == 0xB3);
    assert(mem_read(m, 0xF8000, 1) == 0xB4);
    assert(mem_read(m, 0xFFFFF, 1) == 0xB5);
    assert(debug_peek(m, 0xF0000) == 0xB3);
    assert(debug_peek(m, 0xF8000) == 0xB4);

    pci_cfg_write8(m, PCI_Q35_PAM0, 0x10);
    assert(pci_cfg_read8(m, PCI_Q35_PAM0) == 0x10);
    mem_write(m, 0xF0000, 1, 0x63);
    mem_write(m, 0xF8000, 1, 0x64);
    mem_write(m, 0xFFFFF, 1, 0x65);
    assert(mem_read(m, 0xF0000, 1) == 0xB3);
    assert(mem_read(m, 0xF8000, 1) == 0xB4);
    assert(mem_read(m, 0xFFFFF, 1) == 0xB5);
    assert(debug_peek(m, 0xF0000) == 0xB3);
    pci_cfg_write8(m, PCI_Q35_PAM0, 0x00);
    assert(mem_read(m, 0xF0000, 1) == 0x33);
    assert(debug_peek(m, 0xF0000) == 0x33);
    assert(mem_read(m, 0xF8000, 1) == 0x44);
    assert(mem_read(m, 0xFFFFF, 1) == 0x55);

    /* SeaBIOS programs PAM in dword writes. PAM0 reserves bits 3:0 and 7:6,
     * while PAM1..3 expose two defined 2-bit fields apiece. */
    pci_cfg_write32(m, PCI_Q35_PAM0, 0xFFFFFFFFu);
    assert(pci_cfg_read32(m, PCI_Q35_PAM0) == 0x33333330u);
    pci_cfg_write32(m, PCI_Q35_PAM0, 0);
    assert(pci_cfg_read32(m, PCI_Q35_PAM0) == 0);

    /* PAM is bounded to C0000-FFFFF. Ordinary RAM above it still writes;
     * the separate reset-vector flash alias remains read-only. */
    mem_write(m, 0x100000, 1, 0x72);
    assert(mem_read(m, 0x100000, 1) == 0x72);
    mem_write(m, ROM_TOPOF4G_BASE, 1, 0x73);
    assert(mem_read(m, ROM_TOPOF4G_BASE, 1) == 0x11);

    fx_done(&f);
    puts("ok: q35_pam_shadow_and_protection");
}

int main(void) {
    test_pci_multifunction_exposes_ahci();
    test_q35_pam_shadow_and_protection();
    puts("pam tests: ok");
    return 0;
}
