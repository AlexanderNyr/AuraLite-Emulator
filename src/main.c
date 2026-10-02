// main.c -- CLI harness: loads firmware.bin as the boot ROM, wires up the
// machine for a chosen chipset-generation profile, runs the CPU, and
// reports what happened (registers, mode, PCI/log trail, optional
// framebuffer dump) -- a debugging tool for the firmware as much as a demo.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "machine.h"
#include "devices.h"
#include "pci.h"
#include "platform.h"
#include "pci.h"
#include "devices.h"
#include "pci.h"

static uint8_t *read_file(const char *path, size_t *len) {
    FILE *f = fopen(path, "rb");
    if (!f) { fprintf(stderr, "cannot open %s\n", path); return NULL; }
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    uint8_t *buf = malloc(n);
    if (fread(buf, 1, n, f) != (size_t)n) { fprintf(stderr, "short read %s\n", path); }
    fclose(f);
    if (len) *len = (size_t)n;
    return buf;
}

static void write_ppm(machine_t *m, const char *path) {
    FILE *f = fopen(path, "wb");
    if (!f) return;
    fprintf(f, "P6\n%d %d\n255\n", m->fb_w, m->fb_h);
    for (int i = 0; i < m->fb_w*m->fb_h; i++) {
        uint32_t px = m->fb[i];
        uint8_t rgb[3] = { (uint8_t)(px>>16), (uint8_t)(px>>8), (uint8_t)px };
        fwrite(rgb, 1, 3, f);
    }
    fclose(f);
}

static void dump_regs(cpu_t *c) {
    printf("---- CPU state (mode=%s, instr=%llu) ----\n", cpu_mode_name(c), (unsigned long long)c->instr_count);
    printf("RAX=%016llx RBX=%016llx RCX=%016llx RDX=%016llx\n", (unsigned long long)c->gpr[RAX],(unsigned long long)c->gpr[RBX],(unsigned long long)c->gpr[RCX],(unsigned long long)c->gpr[RDX]);
    printf("RSI=%016llx RDI=%016llx RBP=%016llx RSP=%016llx\n", (unsigned long long)c->gpr[RSI],(unsigned long long)c->gpr[RDI],(unsigned long long)c->gpr[RBP],(unsigned long long)c->gpr[RSP]);
    printf("R8 =%016llx R9 =%016llx R10=%016llx R11=%016llx\n", (unsigned long long)c->gpr[8],(unsigned long long)c->gpr[9],(unsigned long long)c->gpr[10],(unsigned long long)c->gpr[11]);
    printf("R12=%016llx R13=%016llx R14=%016llx R15=%016llx\n", (unsigned long long)c->gpr[12],(unsigned long long)c->gpr[13],(unsigned long long)c->gpr[14],(unsigned long long)c->gpr[15]);
    printf("RIP=%016llx RFLAGS=%016llx CR0=%016llx CR3=%016llx CR4=%016llx EFER=%016llx\n",
        (unsigned long long)c->rip,(unsigned long long)c->rflags,(unsigned long long)c->cr0,
        (unsigned long long)c->cr3,(unsigned long long)c->cr4,(unsigned long long)c->efer);
    printf("CS=%04x base=%016llx L=%d D=%d   SS=%04x base=%016llx\n",
        c->seg[SEG_CS].sel, (unsigned long long)c->seg[SEG_CS].base, c->seg[SEG_CS].l, c->seg[SEG_CS].d_b,
        c->seg[SEG_SS].sel, (unsigned long long)c->seg[SEG_SS].base);
    if (c->halted) printf("STATE: HALTED (HLT executed)\n");
    if (c->fault)  printf("STATE: FAULT -- %s (at rip=0x%llx)\n", c->fault_msg, (unsigned long long)c->fault_rip);
}

enum {
    EXIT_OK = 0,
    EXIT_INPUT_ERROR = 1,
    EXIT_CPU_FAULT = 2
};

int main(int argc, char **argv) {
    const char *rom_path = "firmware/firmware.bin";
    const char *disk_path = "disk/disk.img";
    const char *platform_name = "haswell";
    const char *fb_out = NULL;
    const char *log_out = NULL;
    uint64_t max_instr = 50ull*1000*1000;
    int trace = 0;

    for (int i = 1; i < argc; i++) {
        if (!strncmp(argv[i], "--rom=", 6)) rom_path = argv[i]+6;
        else if (!strncmp(argv[i], "--disk=", 7)) disk_path = argv[i]+7;
        else if (!strncmp(argv[i], "--platform=", 11)) platform_name = argv[i]+11;
        else if (!strncmp(argv[i], "--max-instr=", 12)) max_instr = strtoull(argv[i]+12, NULL, 0);
        else if (!strncmp(argv[i], "--dump-fb=", 10)) fb_out = argv[i]+10;
        else if (!strncmp(argv[i], "--log=", 6)) log_out = argv[i]+6;
        else if (!strcmp(argv[i], "--trace")) trace = 1;
        else if (!strcmp(argv[i], "--help")) {
            printf("usage: %s [--rom=path] [--disk=path] [--platform=sandybridge|ivybridge|haswell|broadwell|baytrail]\n"
                   "          [--max-instr=N] [--trace] [--dump-fb=out.ppm] [--log=out.txt]\n", argv[0]);
            return 0;
        }
    }

    size_t rom_len = 0;
    uint8_t *rom = read_file(rom_path, &rom_len);
    if (!rom) return 1;

    machine_t *m = calloc(1, sizeof *m);
    m->cpu.mach = m;
    mem_init(m, rom, rom_len);
    io_init(m);
    m->plat = (struct platform *)platform_by_name(platform_name);
    devices_init_common(m);
    devices_init_platform(m);

    size_t disk_len = 0;
    uint8_t *disk = read_file(disk_path, &disk_len);
    if (disk) { m->disk = disk; m->disk_len = disk_len; }
    else { mlog(&m->log, "[boot] no disk image loaded -- USB mass-storage reads will return nothing"); }

    cpu_reset(&m->cpu);
    m->cpu.trace = trace;

    mlog(&m->log, "[boot] platform=%s rom=%s (%zu bytes) disk=%s (%zu bytes)",
         platform_by_name(platform_name)->name, rom_path, rom_len, disk_path, disk_len);

    uint64_t n = 0;
    int step_result = 0;
    while (n < max_instr) {
        if (!m->guest_entry_seen && m->cpu.rip == 0x00100000ULL) {
            m->guest_entry_seen = 1;
            mlog(&m->log, "[guest] conventional entry reached at 0x00100000");
        }
        step_result = cpu_step(&m->cpu);
        if (step_result != 0)
            break;
        n++;
        if (!m->guest_entry_seen && m->cpu.rip == 0x00100000ULL) {
            m->guest_entry_seen = 1;
            mlog(&m->log, "[guest] conventional entry reached at 0x00100000");
        }
        if (m->stop_requested)
            break;
    }

    if (m->guest_entry_seen && m->fb && m->fb[0] == 0x00200000u)
        mlog(&m->log, "[guest] framebuffer marker OK: pixel[0]=0x%08x", m->fb[0]);
    dump_regs(&m->cpu);
    printf("\n---- last log lines ----\n");
    int start = m->log.count < 60 ? 0 : m->log.count - 60;
    for (int i = start; i < m->log.count; i++) {
        int idx = (m->log.head - m->log.count + i + LOG_RING) % LOG_RING;
        printf("%s\n", m->log.lines[idx]);
    }

    if (fb_out) { write_ppm(m, fb_out); printf("\n[fb] framebuffer written to %s (%dx%d)\n", fb_out, m->fb_w, m->fb_h); }
    if (log_out) {
        FILE *f = fopen(log_out, "w");
        if (f) {
            int st = 0;
            for (int i = st; i < m->log.count; i++) {
                int idx = (m->log.head - m->log.count + i + LOG_RING) % LOG_RING;
                fprintf(f, "%s\n", m->log.lines[idx]);
            }
            fclose(f);
        }
    }
    int rc = EXIT_OK;
    if (m->cpu.fault)
        rc = EXIT_CPU_FAULT;
    else if (step_result != 0 && !m->cpu.halted)
        rc = EXIT_CPU_FAULT;

    /* C10: orderly teardown keeps the ASan/UBSan lane leak-clean. */
    devices_done(m);
    pci_done(m);
    mem_done(m);
    free(disk);
    free(rom);
    free(m);
    return rc;
}
