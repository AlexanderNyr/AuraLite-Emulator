// kloader.c -- KERNEL-BOOT K1: direct kernel-load lane.
//
// Implements, inside the emulator, the loader half of the AuraLite-OS boot
// contract. The kernel side measures this contract down to the byte
// (boot/shared/boot_info.h, kernel/arch/x86_64/boot.asm, kernel.ld --
// AuraLite-OS @ 0ed0d29, MIT, same author); this file is the smallest
// faithful implementation of it:
//
//   * ELF64 is expected higher-half linked at KLOAD_KERNEL_VMA + 1 MiB with
//     p_paddr == p_vaddr; segments are placed at physical p_paddr - VMA.
//   * Paging: identity map and HHDM (0xffff8000_00000000) of the low 4 GiB
//     share one PDPT of 1 GiB PS pages; the kernel's own virtual range is
//     mapped with 4 KiB PTEs (P|RW; NXE deliberately left off -- K2 notes).
//   * boot_info_t at KLOAD_INFO_PHYS: magic, fb descriptor matching our
//     GPU VRAM window (0xD0000000, 800x600x32), E820-style memmap, HHDM
//     offset, cpu_count=1 (BSP lapic_id 0), rsdp_phys -> a fabricated
//     RSDP/RSDT/XSDT/MADT set at KLOAD_ACPI_BASE (K3: LAPIC 0, I/O APIC at
//     0xFEC00000 GSI base 0, ISA IRQ0 -> GSI2 override; before K3 rsdp was
//     0 and the kernel fell back to the PC-standard hardcode -- measured),
//     no initrd (K4 scope).
//   * CPU: CR0=PE|MP|NE|PG, CR4=PAE|PGE, EFER=LME|LMA, flat 64-bit CS/SS,
//     RFLAGS=2 (IF=0 as the contract requires), RDI=boot_info phys,
//     RSP=KLOAD_STACK_TOP, RIP=ELF entry.
//
// All loader structures are written straight into the RAM backing store,
// not through mem_write: a real loader flashes/prepares memory off-bus
// before the CPU starts, and bypassing the bus model also keeps us immune
// to the A20 gate's state at power-on (measured OPEN -- chipset.h -- but
// not architecturally guaranteed forever).
#include <elf.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stddef.h>
#include "kloader.h"
#include "acpi.h"

/* ------------------------------------------------------------------ */
/* AuraLite boot_info ABI mirror (attribution above).                  */
/* The _Static_assert lattice below is the anti-drift device: it pins  */
/* the layout we fabricate to the layout the kernel consumes, measured */
/* from boot/shared/boot_info.h with offsetof() on the host compiler.  */
/* ------------------------------------------------------------------ */
#define BOOT_MAGIC           0x4155524142544C44ULL  /* "AURABLTD" LE */
#define BOOT_MAX_MMAP        256
#define KLOAD_ACPI_BASE      0x0009F000ULL  /* K3: RSDP set window, ACPI_RECLAIM in the memmap */
#define BOOT_MAX_CPUS        64
#define BOOT_MEM_USABLE      1u
#define BOOT_MEM_RESERVED    2u
#define BOOT_MEM_ACPI_RECLAIM 3u
#define BOOT_MEM_BOOTLOADER  6u
#define BOOT_MEM_KERNEL      7u
#define BOOT_MEM_FRAMEBUF    8u

typedef struct {
    uint64_t base;
    uint64_t length;
    uint32_t type;
    uint32_t _pad;
} k_mmap_entry_t;

typedef struct {
    uint64_t phys_base;
    uint32_t width, height, pitch;
    uint8_t  bpp, red_shift, green_shift, blue_shift, _pad;
} k_fb_t;

typedef struct {
    uint32_t processor_id, lapic_id;
    uint64_t goto_address, extra_argument;
} k_cpu_t;

typedef struct {
    uint64_t        magic;
    k_fb_t          fb;
    k_mmap_entry_t  mmap[BOOT_MAX_MMAP];
    uint32_t        mmap_count, _pad_mmap;
    uint64_t        hhdm_offset;
    uint64_t        initrd_phys, initrd_size;
    uint32_t        cpu_count, bsp_lapic_id;
    k_cpu_t         cpus[BOOT_MAX_CPUS];
    uint64_t        rsdp_phys;
    uint8_t         boot_from_uefi, _pad[7];
} k_boot_info_t;

/* Measured against the real header (host gcc offsetof/sizeof, K1):        */
_Static_assert(offsetof(k_boot_info_t, magic)        == 0,    "ABI: magic");
_Static_assert(offsetof(k_boot_info_t, fb)           == 8,    "ABI: fb");
_Static_assert(offsetof(k_boot_info_t, mmap)         == 40,   "ABI: mmap");
_Static_assert(offsetof(k_boot_info_t, mmap_count)   == 6184, "ABI: mmap_count");
_Static_assert(offsetof(k_boot_info_t, hhdm_offset)  == 6192, "ABI: hhdm_offset");
_Static_assert(offsetof(k_boot_info_t, initrd_phys)  == 6200, "ABI: initrd_phys");
_Static_assert(offsetof(k_boot_info_t, cpu_count)    == 6216, "ABI: cpu_count");
_Static_assert(offsetof(k_boot_info_t, cpus)         == 6224, "ABI: cpus");
_Static_assert(offsetof(k_boot_info_t, rsdp_phys)    == 7760, "ABI: rsdp_phys");
_Static_assert(offsetof(k_boot_info_t, boot_from_uefi) == 7768, "ABI: boot_from_uefi");
_Static_assert(sizeof(k_boot_info_t)                 == 7776, "ABI: sizeof");
_Static_assert(sizeof(k_fb_t)                        == 32,   "ABI: fb sizeof");
_Static_assert(KLOAD_INFO_PHYS + sizeof(k_boot_info_t) < KLOAD_MEMMAP_BR + 0x1000,
               "boot_info must fit its memmap region");

/* ------------------------------------------------------------------ */
/* File loading + bounds-checked ELF walking. Every pointer derived    */
/* from ELF offsets is validated against the file size first: the      */
/* sanitizer lanes (ASan) treat the image as hostile input.            */
/* ------------------------------------------------------------------ */
static uint8_t *kread_file(const char *path, size_t *out_len) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    if (n <= 0) { fclose(f); return NULL; }
    uint8_t *buf = malloc((size_t)n);
    if (!buf || fread(buf, 1, (size_t)n, f) != (size_t)n) { free(buf); fclose(f); return NULL; }
    fclose(f);
    *out_len = (size_t)n;
    return buf;
}

static int inside(uint64_t off, uint64_t need, uint64_t total) {
    return off <= total && need <= total - off;
}

/* Finds the `want` symbol in the first SHT_SYMTAB; 0 when absent. The
 * kernel stays bootable without a symtab -- this only feeds the CLI's
 * "kmain reached" marker. */
static uint64_t elf_find_symbol(const uint8_t *img, size_t len,
                                const Elf64_Ehdr *eh, const char *want) {
    if (!eh->e_shoff || !eh->e_shnum || eh->e_shentsize != sizeof(Elf64_Shdr))
        return 0;
    if (!inside(eh->e_shoff, (uint64_t)eh->e_shnum * sizeof(Elf64_Shdr), len))
        return 0;
    const Elf64_Shdr *sh = (const Elf64_Shdr *)(const void *)(img + eh->e_shoff);
    for (uint16_t i = 0; i < eh->e_shnum; i++) {
        if (sh[i].sh_type != SHT_SYMTAB) continue;
        if (sh[i].sh_link >= eh->e_shnum) return 0;
        const Elf64_Shdr *str = &sh[sh[i].sh_link];
        if (!inside(str->sh_offset, str->sh_size, len)) return 0;
        if (!inside(sh[i].sh_offset, sh[i].sh_size, len)) return 0;
        size_t nsyms = sh[i].sh_size / sizeof(Elf64_Sym);
        const Elf64_Sym *syms = (const Elf64_Sym *)(const void *)(img + sh[i].sh_offset);
        const char *strs = (const char *)(img + str->sh_offset);
        for (size_t s = 0; s < nsyms; s++) {
            if (syms[s].st_name >= str->sh_size) continue;
            size_t maxn = (size_t)str->sh_size - syms[s].st_name;
            if (strncmp(strs + syms[s].st_name, want, maxn) == 0)
                return syms[s].st_value;
        }
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* Page-table construction. Direct RAM writes (see the file header).   */
/* ------------------------------------------------------------------ */
typedef struct {
    uint8_t  *raw;      /* guest RAM backing store */
    uint64_t  next;     /* bump allocator, page-granular */
    int       pages;    /* diagnostics: tables allocated */
    machine_t *m;
} ptb_t;

static uint64_t pt_page(ptb_t *b) {
    uint64_t p = b->next;
    if (p + 0x1000 > KLOAD_PT_LIMIT) {
        mlog(&b->m->log, "[kload] page-table budget exhausted at 0x%llx (limit 0x%llx)",
             (unsigned long long)p, (unsigned long long)KLOAD_PT_LIMIT);
        return 0;
    }
    memset(b->raw + p, 0, 0x1000);
    b->next = p + 0x1000;
    b->pages++;
    return p;
}

static uint64_t *pt_entries(ptb_t *b, uint64_t phys) {
    return (uint64_t *)(void *)(b->raw + phys);
}

/* Maps one 4 KiB page va->pa with P|RW (user=0, NX off; see file header). */
static int kmap4k(ptb_t *b, uint64_t va, uint64_t pa) {
    int i4 = (int)((va >> 39) & 0x1FF);
    int i3 = (int)((va >> 30) & 0x1FF);
    int i2 = (int)((va >> 21) & 0x1FF);
    int i1 = (int)((va >> 12) & 0x1FF);
    uint64_t *pml4 = pt_entries(b, KLOAD_PT_BASE);

    if (!(pml4[i4] & 1)) {
        uint64_t t = pt_page(b); if (!t) return -1;
        pml4[i4] = t | 3; /* P|RW */
    }
    uint64_t *pdpt = pt_entries(b, pml4[i4] & ~0xFFFULL);
    if (!(pdpt[i3] & 1)) {
        uint64_t t = pt_page(b); if (!t) return -1;
        pdpt[i3] = t | 3;
    }
    if (pdpt[i3] & 0x80) return 0; /* already inside a 1 GiB page */
    uint64_t *pd = pt_entries(b, pdpt[i3] & ~0xFFFULL);
    if (!(pd[i2] & 1)) {
        uint64_t t = pt_page(b); if (!t) return -1;
        pd[i2] = t | 3;
    }
    if (pd[i2] & 0x80) { mlog(&b->m->log, "[kload] 2 MiB/4 KiB mapping clash at va 0x%llx",
                              (unsigned long long)va); return -1; }
    uint64_t *pt = pt_entries(b, pd[i2] & ~0xFFFULL);
    pt[i1] = pa | 3;
    return 0;
}

/* ------------------------------------------------------------------ */
/* boot_info fabrication.                                              */
/* ------------------------------------------------------------------ */
static uint32_t kfill_boot_info(machine_t *m, uint64_t kernel_end,
                                  uint64_t initrd_phys, uint64_t initrd_size) {
    k_boot_info_t bi;
    memset(&bi, 0, sizeof bi);
    bi.magic = BOOT_MAGIC;

    /* Framebuffer: the GPU VRAM window from devices.c. Pixel format is
     * 8:8:8:8 with B in the low byte (little-endian BGRA), matching how
     * write_ppm() decodes m->fb. */
    bi.fb.phys_base  = 0xD0000000ULL;
    bi.fb.width      = (uint32_t)m->fb_w;
    bi.fb.height     = (uint32_t)m->fb_h;
    bi.fb.pitch      = (uint32_t)m->fb_w * 4u;
    bi.fb.bpp        = 32;
    bi.fb.red_shift  = 16;
    bi.fb.green_shift = 8;
    bi.fb.blue_shift = 0;

    uint32_t n = 0;
    /* parameter names are prefixed: struct members .base/.length/.type must
     * survive macro expansion on the left-hand side */
    #define MMAP(b_, l_, t_) do { \
        bi.mmap[n].base = (b_); bi.mmap[n].length = (l_); \
        bi.mmap[n].type = (t_); n++; } while (0)

    MMAP(0, KLOAD_PT_BASE, BOOT_MEM_USABLE);
    /* page tables + boot_info itself: reclaimable once the kernel has
     * copied out everything it needs (boot_info.h header says exactly that) */
    MMAP(KLOAD_PT_BASE, KLOAD_MEMMAP_BR - KLOAD_PT_BASE, BOOT_MEM_BOOTLOADER);
    /* K3: the last 4 KiB before the legacy hole holds the ACPI table set
     * (RSDP/RSDT/XSDT/MADT, flashed below) -- callers walk it via
     * rsdp_phys; type 3 = the canonical "ACPI tables" reclaim entry. */
    MMAP(KLOAD_MEMMAP_BR, KLOAD_ACPI_BASE - KLOAD_MEMMAP_BR, BOOT_MEM_USABLE);
    MMAP(KLOAD_ACPI_BASE, 0x000A0000ULL - KLOAD_ACPI_BASE, BOOT_MEM_ACPI_RECLAIM);
    /* legacy VGA hole + our ROM-alias shadow window */
    MMAP(0x000A0000ULL, 0x00060000ULL, BOOT_MEM_RESERVED);
    MMAP(KLOAD_KERNEL_MIN, kernel_end - KLOAD_KERNEL_MIN, BOOT_MEM_KERNEL);
    /* K4: the initrd rides as a second BOOT_MEM_KERNEL block (the kernel's
     * boot_info.h header literally says "kernel image + initrd"), parked
     * directly above the kernel image, 4 KiB aligned. */
    uint64_t usable_from = kernel_end;
    if (initrd_size) {
        MMAP(initrd_phys, initrd_size, BOOT_MEM_KERNEL);
        usable_from = initrd_phys + initrd_size;
    }
    if (usable_from < RAM_SIZE)
        MMAP(usable_from, (uint64_t)RAM_SIZE - usable_from, BOOT_MEM_USABLE);
    /* fb window, matching the devices.c GPU VRAM MMIO region */
    MMAP(0xD0000000ULL, (uint64_t)m->fb_w * (uint64_t)m->fb_h * 4u, BOOT_MEM_FRAMEBUF);
    #undef MMAP
    bi.mmap_count = n;

    bi.hhdm_offset  = KLOAD_HHDM;
    bi.initrd_phys  = initrd_phys;       /* K4: 0 = "no initrd loaded" */
    bi.initrd_size  = initrd_size;
    /* K5 (--cpus): publish m->cfg_cpus entries; the emulator still runs
     * exactly one vCPU, so entries 1..ncpus-1 exist purely to let the
     * kernel's smp_init() walk its AP bring-up path (calibration, INIT-
     * SIPI-SIPI, bounded "did not respond" skip).  goto_address stays 0:
     * the kernel never uses it (it self-serves wake-up via the LAPIC ICR),
     * measured fact documented in the K5 phase notes. */
    int ncpus = m->cfg_cpus;
    if (ncpus < 1) ncpus = 1;
    if (ncpus > BOOT_MAX_CPUS) ncpus = BOOT_MAX_CPUS;
    bi.cpu_count    = (uint32_t)ncpus;
    bi.bsp_lapic_id = 0;
    for (int i = 0; i < ncpus; i++) {
        bi.cpus[i].processor_id  = (uint32_t)i;
        bi.cpus[i].lapic_id      = (uint32_t)i;
        bi.cpus[i].goto_address  = 0;
        bi.cpus[i].extra_argument = 0;
    }
    bi.rsdp_phys    = KLOAD_ACPI_BASE;   /* K3: real MADT -> kernel "MADT agree" */
    bi.boot_from_uefi = 0;

    if (ncpus > 1) {
        if (m->cfg_smp > 1)
            mlog(&m->log, "[kload] K6: boot_info publishes %d CPUs; "
                "--smp=%d wires %d real vCPU context(s)",
                ncpus, m->cfg_smp, m->cfg_smp);
        else
            mlog(&m->log, "[kload] K5 metering: boot_info publishes %d CPUs "
                "(emulator still runs ONE vCPU; APs never respond to SIPI)",
                ncpus);
    }

    if (!acpi_build_table_set_smp(m->ram, RAM_SIZE, KLOAD_ACPI_BASE, ncpus)) {
        mlog(&m->log, "[kload] ERROR: ACPI table set does not fit at 0x%llx",
             (unsigned long long)KLOAD_ACPI_BASE);
        return -1;
    }
    memcpy(m->ram + KLOAD_INFO_PHYS, &bi, sizeof bi);
    return n;
}

/* ------------------------------------------------------------------ */
/* The lane itself.                                                    */
/* ------------------------------------------------------------------ */
int kload_boot(machine_t *m, const char *elf_path, const char *initrd_path) {
    size_t len = 0;
    uint8_t *img = kread_file(elf_path, &len);
    if (!img) {
        mlog(&m->log, "[kload] cannot read kernel image %s", elf_path);
        return -1;
    }
    if (len < sizeof(Elf64_Ehdr)) {
        mlog(&m->log, "[kload] %s: too small for an ELF64 header (%zu bytes)", elf_path, len);
        free(img); return -1;
    }
    const Elf64_Ehdr *eh = (const Elf64_Ehdr *)(const void *)img;
    if (memcmp(eh->e_ident, ELFMAG, SELFMAG) != 0 ||
        eh->e_ident[EI_CLASS] != ELFCLASS64 ||
        eh->e_ident[EI_DATA]  != ELFDATA2LSB ||
        eh->e_machine != EM_X86_64 ||
        eh->e_type    != ET_EXEC ||
        eh->e_version != EV_CURRENT) {
        mlog(&m->log, "[kload] %s: not an x86-64 ET_EXEC ELF64 (class=%d machine=%u type=%u)",
             elf_path, eh->e_ident[EI_CLASS], eh->e_machine, eh->e_type);
        free(img); return -1;
    }
    if (!eh->e_phoff || !eh->e_phnum || eh->e_phentsize != sizeof(Elf64_Phdr) ||
        !inside(eh->e_phoff, (uint64_t)eh->e_phnum * sizeof(Elf64_Phdr), len)) {
        mlog(&m->log, "[kload] %s: malformed program-header table", elf_path);
        free(img); return -1;
    }

    /* ---- place PT_LOAD segments at physical (p_paddr - KERNEL_VMA) ---- */
    const Elf64_Phdr *ph = (const Elf64_Phdr *)(const void *)(img + eh->e_phoff);
    uint64_t lo = ~0ULL, hi = 0;
    int nseg = 0;
    for (uint16_t i = 0; i < eh->e_phnum; i++) {
        if (ph[i].p_type != PT_LOAD) continue;
        if (ph[i].p_vaddr != ph[i].p_paddr) {
            mlog(&m->log, "[kload] segment %d: vaddr 0x%llx != paddr 0x%llx (kernel.ld convention violated)",
                 i, (unsigned long long)ph[i].p_vaddr, (unsigned long long)ph[i].p_paddr);
            free(img); return -1;
        }
        if (ph[i].p_memsz < ph[i].p_filesz ||
            !inside(ph[i].p_offset, ph[i].p_filesz, len)) {
            mlog(&m->log, "[kload] segment %d: bad file/memory sizes", i);
            free(img); return -1;
        }
        if (ph[i].p_paddr < KLOAD_KERNEL_VMA) {
            mlog(&m->log, "[kload] segment %d: paddr 0x%llx below KERNEL_VMA (not a higher-half kernel?)",
                 i, (unsigned long long)ph[i].p_paddr);
            free(img); return -1;
        }
        uint64_t pa = ph[i].p_paddr - KLOAD_KERNEL_VMA;
        if (pa < KLOAD_KERNEL_MIN || pa + ph[i].p_memsz > RAM_SIZE) {
            mlog(&m->log, "[kload] segment %d: phys range 0x%llx..0x%llx outside guest RAM",
                 i, (unsigned long long)pa, (unsigned long long)(pa + ph[i].p_memsz));
            free(img); return -1;
        }
        memcpy(m->ram + pa, img + ph[i].p_offset, ph[i].p_filesz);
        /* .bss tail is already zero: guest RAM came up calloc'ed */
        if (pa < lo) lo = pa;
        if (pa + ph[i].p_memsz > hi) hi = pa + ph[i].p_memsz;
        nseg++;
    }
    if (!nseg) {
        mlog(&m->log, "[kload] %s: no PT_LOAD segments", elf_path);
        free(img); return -1;
    }
    if (eh->e_entry < KLOAD_KERNEL_VMA + lo || eh->e_entry >= KLOAD_KERNEL_VMA + hi) {
        mlog(&m->log, "[kload] entry 0x%llx outside the loaded segments [0x%llx..0x%llx)",
             (unsigned long long)eh->e_entry,
             (unsigned long long)(KLOAD_KERNEL_VMA + lo),
             (unsigned long long)(KLOAD_KERNEL_VMA + hi));
        free(img); return -1;
    }

    uint64_t kmain = elf_find_symbol(img, len, eh, "kmain");

    /* ---- page tables: shared identity/HHDM PDPT + kernel VMA 4K ---- */
    ptb_t b = { .raw = m->ram, .next = KLOAD_PT_BASE, .pages = 0, .m = m };
    if (!pt_page(&b)) { free(img); return -1; }                    /* PML4 @ KLOAD_PT_BASE */
    uint64_t *pml4 = pt_entries(&b, KLOAD_PT_BASE);
    uint64_t id_pdpt = pt_page(&b);
    if (!id_pdpt) { free(img); return -1; }
    uint64_t *pdpt = pt_entries(&b, id_pdpt);
    for (uint64_t g = 0; g < 4; g++)
        pdpt[g] = (g << 30) | 0x83;           /* 1 GiB PS page: P|RW|PS */
    pml4[0]   = id_pdpt | 3;                   /* identity 0..4 GiB (LAPIC/IOAPIC incl.) */
    pml4[256] = id_pdpt | 3;                   /* HHDM: KLOAD_HHDM has PML4 index 256 */

    uint64_t map_lo = lo & ~0xFFFULL;
    uint64_t map_hi = (hi + 0xFFFULL) & ~0xFFFULL;
    uint64_t npages = 0;
    for (uint64_t pa = map_lo; pa < map_hi; pa += 0x1000) {
        if (kmap4k(&b, KLOAD_KERNEL_VMA + pa, pa) != 0) { free(img); return -1; }
        npages++;
    }

    /* ---- K4: optional initrd (USTAR tar) parked above the kernel image ---- */
    uint64_t initrd_phys = 0, initrd_size = 0;
    uint8_t *initrd_img = NULL;
    if (initrd_path) {
        size_t initrd_len = 0;
        initrd_img = kread_file(initrd_path, &initrd_len);
        initrd_size = initrd_len;
        if (!initrd_img) {
            mlog(&m->log, "[kload] cannot read initrd image %s", initrd_path);
            free(img); return -1;
        }
        uint64_t cap = (uint64_t)RAM_SIZE - map_hi;
        if (initrd_size == 0 || initrd_size >= cap) {
            mlog(&m->log, "[kload] initrd %s (%llu bytes) does not fit above the kernel image (room %llu)",
                 initrd_path, (unsigned long long)initrd_size, (unsigned long long)cap);
            free(initrd_img); free(img); return -1;
        }
        initrd_phys = map_hi;
        memcpy(m->ram + initrd_phys, initrd_img, initrd_size);
        /* USTAR magic at offset 257 ("ustar\0") -- warn only: the kernel's
         * initrd parser is the loud verifier, the loader is the carrier. */
        if (initrd_size < 512 || memcmp(initrd_img + 257, "ustar", 5) != 0)
            mlog(&m->log, "[kload] WARNING: initrd %s lacks the USTAR magic at +257; "
                          "the kernel parser will refuse it if this is not a tar", initrd_path);
        mlog(&m->log, "[kload] initrd=%s (%llu bytes) at phys 0x%llx",
             initrd_path, (unsigned long long)initrd_size, (unsigned long long)initrd_phys);
    }

    uint32_t mmap_n = kfill_boot_info(m, map_hi, initrd_phys, initrd_size);
    free(initrd_img);

    /* ---- CPU state: exactly the boot.asm contract ---- */
    cpu_reset(&m->cpu);
    m->cpu.cr0 = 0x80000023ULL;                  /* PE|MP|NE|PG */
    m->cpu.cr4 = 0x000000A0ULL;                  /* PAE|PGE */
    m->cpu.efer = 0x00000500ULL;                 /* LME|LMA */
    m->cpu.cr3 = KLOAD_PT_BASE;
    m->cpu.rflags = 0x2;                         /* IF=0 (contract) */
    m->cpu.gpr[RDI] = KLOAD_INFO_PHYS;           /* physical boot_info_t* */
    m->cpu.gpr[RSP] = KLOAD_STACK_TOP;           /* any temp stack; replaced in boot.asm */
    m->cpu.rip = eh->e_entry;
    m->cpu.seg[SEG_CS] = (segment_t){ .sel = 0x08, .base = 0, .limit = 0xFFFFFFFFu,
                                      .d_b = 0, .l = 1, .present = 1, .type = 0x9B };
    m->cpu.seg[SEG_SS] = (segment_t){ .sel = 0x10, .base = 0, .limit = 0xFFFFFFFFu,
                                      .d_b = 1, .l = 0, .present = 1, .type = 0x93 };
    m->cpu.seg[SEG_DS] = m->cpu.seg[SEG_ES] = (segment_t){ .sel = 0x10, .base = 0,
                                      .limit = 0xFFFFFFFFu, .d_b = 1, .present = 1, .type = 0x93 };
    m->cpu.seg[SEG_FS] = m->cpu.seg[SEG_GS] = (segment_t){ .sel = 0, .base = 0,
                                      .limit = 0xFFFFFFFFu, .d_b = 1, .present = 1, .type = 0x93 };
    m->kmain_va = kmain;
    m->kmain_seen = 0;

    mlog(&m->log, "[kload] kernel=%s (%zu bytes), %d segment(s) at phys [0x%llx..0x%llx)",
         elf_path, len, nseg, (unsigned long long)lo, (unsigned long long)hi);
    mlog(&m->log, "[kload] entry=0x%llx cr3=0x%llx: %d table pages "
         "(identity 4G + HHDM 4G shared PDPT, kernel VMA %llu x 4KiB pages)",
         (unsigned long long)eh->e_entry, (unsigned long long)m->cpu.cr3,
         b.pages, (unsigned long long)npages);
    mlog(&m->log, "[kload] boot_info @ phys 0x%llx: fb %dx%d @ 0xD0000000, "
         "memmap=%u entries, usable to %u MiB, cpus=%d, rsdp=0x%llx, initrd=%s",
         (unsigned long long)KLOAD_INFO_PHYS, m->fb_w, m->fb_h,
         (unsigned)mmap_n, (unsigned)(RAM_SIZE / (1024u*1024u)),
         (unsigned)(m->cfg_cpus < 1 ? 1 : m->cfg_cpus),
         (unsigned long long)KLOAD_ACPI_BASE,
         initrd_phys ? "phys+size" : "none");
    if (kmain)
        mlog(&m->log, "[kload] symtab: kmain=0x%llx (reach marker armed)",
             (unsigned long long)kmain);

    free(img);
    return 0;
}
