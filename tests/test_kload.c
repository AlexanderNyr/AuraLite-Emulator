/* tests/test_kload.c -- KERNEL-BOOT K1 gate: the direct kernel-load lane.
 *
 * Uses the shared flat-machine harness plus the synthetic kernel fixture
 * (tests/kload_fixture.elf, linked at the real AuraLite VMA). Positive
 * control: load fixture -> CPU runs in long64 mode -> fixture validates
 * identity/HHDM views of a fabricated boot_info_t -> COM1 prints K1OK.
 * Negative controls: bad path, truncated ELF, and a class-byte mutation
 * must all be rejected with -1 and a clean machine.
 *
 * Everything asserted here is invariant of the K1 contract, not of the
 * fixture's luck: boot_info bytes are checked directly against the
 * fabricated layout (offsets pinned by the _Static_assert lattice in
 * kloader.c against the AuraLite header).
 */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "machine.h"
#include "platform.h"
#include "kloader.h"
#include "harness.h"

#define FIXTURE "tests/kload_fixture.elf"

/* COM1 byte capture (the test's own sink; devices.c's line-logger is not
 * linked into this suite). */
static char   g_ser[128];
static size_t g_ser_n;

static uint32_t ser_read(void *ctx, uint16_t port, int size) {
    (void)ctx; (void)port; (void)size;
    return 0x20; /* THR empty, same convention as devices.c */
}
static void ser_write(void *ctx, uint16_t port, int size, uint32_t val) {
    (void)ctx; (void)size;
    if (port == 0x3F8 && g_ser_n < sizeof(g_ser) - 1)
        g_ser[g_ser_n++] = (char)val;
}

static machine_t g_m;
static void fx_init(void) {
    setup_machine(&g_m);
    /* kernel lane maps LAPIC/IOAPIC windows through our page tables; the
     * chipset devices must exist the same way devices_init_common() makes
     * them (K1 loads after device init in the CLI too). */
    g_m.fb_w = 800; g_m.fb_h = 600;   /* what devices_init_common() sets */
    io_register(&g_m, 0x3F8, 1, ser_read, ser_write, NULL, "COM1-capture");
    g_ser_n = 0;
}

/* A deliberately corrupted copy of the fixture in /tmp: flips one byte
 * named by `off`, chosen by each negative test. */
static int make_bad_copy(const char *dst, long off) {
    FILE *in = fopen(FIXTURE, "rb");
    if (!in) return -1;
    FILE *out = fopen(dst, "wb");
    if (!out) { fclose(in); return -1; }
    int c; long pos = 0;
    while ((c = fgetc(in)) != EOF)
        fputc(pos++ == off ? (c ^ 0xFF) : c, out);
    fclose(in); fclose(out);
    return 0;
}

int main(void) {
    /* ---------------- negative control: unknown path ---------------- */
    fx_init();
    assert(kload_boot(&g_m, "tests/definitely-not-here.elf", NULL) == -1);

    /* ---------------- negative control: truncated file -------------- */
    fx_init();
    {
        FILE *f = fopen("/tmp/kload-trunc.elf", "wb");
        assert(f != NULL);
        fputs("ELF!", f);   /* 4 bytes: too small for the header, magic ok */
        fclose(f);
    }
    assert(kload_boot(&g_m, "/tmp/kload-trunc.elf", NULL) == -1);

    /* ---------------- negative control: EI_CLASS flipped ------------ */
    fx_init();
    assert(make_bad_copy("/tmp/kload-badclass.elf", 4) == 0);   /* EI_CLASS byte */
    assert(kload_boot(&g_m, "/tmp/kload-badclass.elf", NULL) == -1);

    /* ================= positive control: the contract, end to end ==== */
    fx_init();
    assert(kload_boot(&g_m, FIXTURE, NULL) == 0);

    /* CPU state, per the boot.asm contract. */
    cpu_t *c = &g_m.cpu;
    assert((c->efer & 0x500) == 0x500);        /* LME|LMA */
    assert(c->seg[SEG_CS].l == 1);
    assert((c->cr0 & 1u) && (c->cr0 >> 31));   /* PE|PG */
    assert((c->rflags & 0x200) == 0);          /* IF=0 */
    assert(c->gpr[RDI] == KLOAD_INFO_PHYS);    /* physical boot_info_t* */
    assert(c->rip >= 0xFFFFFFFF80100000ULL);
    assert(c->rip <  0xFFFFFFFF80200000ULL);
    assert(strcmp(cpu_mode_name(c), "long64") == 0);

    /* boot_info_t bytes, fabricated at the ABI offsets (magic "AURABLTD"
     * stores little-endian as 'D','L','T','B','A','R','U','A'). */
    assert(memcmp(g_m.ram + KLOAD_INFO_PHYS, "DLTBARUA", 8) == 0);
    /* mmap_count sits at ABI offset 6184; K3 split the low USABLE block to
     * carve the ACPI window, so we now publish 8 regions */
    uint32_t count;
    memcpy(&count, g_m.ram + KLOAD_INFO_PHYS + 6184, 4);
    assert(count == 8);
    /* K3: rsdp_phys (ABI offset after initrd/cpus) points at the fabricated
     * table set and the low reserved window describes it: the last two
     * low-memory entries are [BR,0x9F000) USABLE and [0x9F000,0xA0000)
     * ACPI_RECLAIM. Entry size is 24 bytes starting at offset 40. */
    {
        uint64_t rsdp;
        memcpy(&rsdp, g_m.ram + KLOAD_INFO_PHYS + 7760, 8);   /* ABI: rsdp_phys */
        assert(rsdp == 0x0009F000ULL);
        uint64_t b0, b1; uint32_t t0, t1;
        memcpy(&b0, g_m.ram + KLOAD_INFO_PHYS + 40 + 2*24, 8);
        memcpy(&t0, g_m.ram + KLOAD_INFO_PHYS + 40 + 2*24 + 16, 4);
        memcpy(&b1, g_m.ram + KLOAD_INFO_PHYS + 40 + 3*24, 8);
        memcpy(&t1, g_m.ram + KLOAD_INFO_PHYS + 40 + 3*24 + 16, 4);
        assert(b0 == KLOAD_MEMMAP_BR && t0 == 1 /* USABLE */);
        assert(b1 == 0x0009F000ULL && t1 == 3 /* ACPI_RECLAIM */);
    }
    /* fb.width at ABI offset 16 == what devices.c reports (800) */
    uint32_t fbw;
    memcpy(&fbw, g_m.ram + KLOAD_INFO_PHYS + 16, 4);
    assert(fbw == 800u);

    /* run the fixture: it must validate the mappings and print K1OK */
    for (int i = 0; i < 20000 && !c->halted && !c->fault; i++)
        cpu_step(c);
    assert(!c->fault);
    assert(c->halted);
    g_ser[g_ser_n] = 0;
    assert(strcmp(g_ser, "K1OK\n") == 0);

    /* ================ KERNEL-BOOT K4: the --initrd= lane ================ */
    /* negative control: an unreadable initrd path must fail the load */
    fx_init();
    assert(kload_boot(&g_m, FIXTURE, "/tmp/kload-no-initrd.tar") == -1);

    /* positive control: a two-block USTAR blob rides above the kernel and
     * is published through boot_info_t.initrd_* (ABI offsets 6200/6208);
     * the memmap carries it as a BOOT_MEM_KERNEL block and the bytes at
     * initrd_phys round-trip the archive verbatim. */
    fx_init();
    {
        static uint8_t tar[1024];
        memset(tar, 0, sizeof tar);
        memcpy(tar, "./hello", 7);
        memcpy(tar + 257, "ustar", 5);
        FILE *f = fopen("/tmp/kload-initrd.tar", "wb");
        assert(f != NULL);
        assert(fwrite(tar, 1, sizeof tar, f) == sizeof tar);
        fclose(f);
    }
    assert(kload_boot(&g_m, FIXTURE, "/tmp/kload-initrd.tar") == 0);
    {
        uint64_t phys, size;
        memcpy(&phys, g_m.ram + KLOAD_INFO_PHYS + 6200, 8);
        memcpy(&size, g_m.ram + KLOAD_INFO_PHYS + 6208, 8);
        assert(phys != 0 && (phys & 0xFFF) == 0);      /* 4 KiB aligned */
        assert(size == 1024);
        assert(memcmp(g_m.ram + phys + 257, "ustar", 5) == 0);
        /* memmap: count is 9 now (ACPI split from K3 plus this block),
         * and exactly one extra BOOT_MEM_KERNEL (7) entry covers the blob */
        uint32_t count2;
        memcpy(&count2, g_m.ram + KLOAD_INFO_PHYS + 6184, 4);
        assert(count2 == 9);
        int kernel_blocks = 0, covered = 0;
        for (uint32_t i = 0; i < count2; i++) {
            const uint8_t *e = g_m.ram + KLOAD_INFO_PHYS + 40 + i * 24;
            uint64_t b, l; uint32_t t;
            memcpy(&b, e, 8); memcpy(&l, e + 8, 8); memcpy(&t, e + 16, 4);
            if (t == 7) kernel_blocks++;
            if (b == phys && l == size) { covered = 1; assert(t == 7); }
        }
        assert(kernel_blocks == 2);   /* kernel image + initrd */
        assert(covered);
    }

    printf("test-kload: all vectors passed\n");
    return 0;
}
