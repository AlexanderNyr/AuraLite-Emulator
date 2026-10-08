// main.c -- CLI harness: loads firmware.bin as the boot ROM, wires up the
// machine for a chosen chipset-generation profile, runs the CPU, and
// reports what happened (registers, mode, PCI/log trail, optional
// framebuffer dump) -- a debugging tool for the firmware as much as a demo.
#include <signal.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "machine.h"
#include "devices.h"
#include "pci.h"
#include "platform.h"
#include "kloader.h"

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

/* K4: the --log= ring used to flush only on clean exit, so any run stopped
 * externally (timeout kills, stop signals) took the entire boot history with
 * it. Stash the flush context and flush on SIGTERM/SIGINT too. */
static machine_t *sig_m;
static const char *sig_log;
static void flush_on_signal(int sig) {
    if (sig_m && sig_log) {
        FILE *f = fopen(sig_log, "w");
        if (f) {
            for (int i = 0; i < sig_m->log.count; i++) {
                int idx = (sig_m->log.head - sig_m->log.count + i + LOG_RING) % LOG_RING;
                fprintf(f, "%s\n", sig_m->log.lines[idx]);
            }
            fclose(f);
        }
    }
    _exit(128 + sig);
}

int main(int argc, char **argv) {
    const char *rom_path = "firmware/firmware.bin";
    const char *disk_path = "disk/disk.img";
    const char *platform_name = "haswell";
    const char *fb_out = NULL;
    const char *log_out = NULL;
    const char *keys_list = NULL;   /* --keys=1E,9E: scancode set-1 bytes (H4) */
    uint64_t keys_at = 0;           /* --keys-at=N: delay injection to instr N */
    int keys_at_prompt = 0;         /* --keys-at=prompt: fire when the guest echoes
                                     * "auralite#" -- the shell-arrival instant is
                                     * budget-noisy across machine shapes (measured:
                                     * storage storms shift the prompt by >0.5G).
                                     * (K4: the PS/2 driver's boot drain
                                     * `while (STATUS.OBF) read DATA` eats any
                                     * pre-boot keys verbatim -- measured.) */
    const char *kernel_path = NULL; /* --kernel=path: KERNEL-BOOT K1 direct-load lane */
    const char *initrd_path = NULL; /* --initrd=path: KERNEL-BOOT K4 USTAR rootfs */
    const char *sata_cfg[AHCI_MAX_ATTACH] = {0}; /* --sata/ --sata-portN (STORE S1+) */
    int cfg_cpus = 0;               /* --cpus=N: K5 SMP-path metering; boot_info
                                     * publishes N CPUs while the emulator still
                                     * runs one vCPU (0/1 = historical UP) */
    int cfg_smp = 0;                /* --smp=N: K6 real execution contexts
                                     * (currently max 2; 0/1 = one vCPU) */
    int smp_probe = 0;              /* EMU_DBG_SMP: vcpu1 vital-signs probe */
    uint64_t watch_phys = 0;        /* --watch-phys=addr: store probe */
    uint64_t max_instr = 50ull*1000*1000;
    int trace = 0;

    for (int i = 1; i < argc; i++) {
        if (!strncmp(argv[i], "--rom=", 6)) rom_path = argv[i]+6;
        else if (!strncmp(argv[i], "--disk=", 7)) disk_path = argv[i]+7;
        else if (!strncmp(argv[i], "--platform=", 11)) platform_name = argv[i]+11;
        else if (!strncmp(argv[i], "--max-instr=", 12)) max_instr = strtoull(argv[i]+12, NULL, 0);
        else if (!strncmp(argv[i], "--dump-fb=", 10)) fb_out = argv[i]+10;
        else if (!strncmp(argv[i], "--log=", 6)) log_out = argv[i]+6;
        else if (!strncmp(argv[i], "--keys=", 7)) keys_list = argv[i]+7;
        else if (!strncmp(argv[i], "--keys-at=", 10)) {
            if (!strcmp(argv[i]+10, "prompt")) keys_at_prompt = 1;
            else keys_at = strtoull(argv[i]+10, NULL, 0);
        }
        else if (!strncmp(argv[i], "--kernel=", 9)) kernel_path = argv[i]+9;
        else if (!strncmp(argv[i], "--initrd=", 9)) initrd_path = argv[i]+9;
        /* STORE S1+: SATA attachments.  --sata=path attaches to HBA port 0;
         * --sata-portN=path pins a specific port (0..AHCI_MAX_ATTACH-1).
         * --sata-max must not equal --sata prefix wise, so --sata= is tested
         * after the longer --sata-port prefix. */
        else if (!strncmp(argv[i], "--sata-port", 11)) {
            const char *eq = strchr(argv[i] + 11, '=');
            int port = atoi(argv[i] + 11);
            if (eq && port >= 0 && port < AHCI_MAX_ATTACH && port < 64)
                sata_cfg[port] = eq + 1;
        }
        else if (!strncmp(argv[i], "--sata=", 7)) sata_cfg[0] = argv[i]+7;
        else if (!strncmp(argv[i], "--cpus=", 7)) cfg_cpus = atoi(argv[i]+7);
        else if (!strncmp(argv[i], "--smp=", 6)) cfg_smp = atoi(argv[i]+6);
        else if (!strcmp(argv[i], "--smp-probe")) smp_probe = 1;
        else if (!strncmp(argv[i], "--watch-phys=", 13))
            watch_phys = strtoull(argv[i]+13, NULL, 0);
        else if (!strcmp(argv[i], "--trace")) trace = 1;
        else if (!strcmp(argv[i], "--help")) {
            printf("usage: %s [--rom=path] [--disk=path] [--platform=sandybridge|ivybridge|haswell|broadwell|baytrail]\n"
                   "          [--max-instr=N] [--trace] [--dump-fb=out.ppm] [--log=out.txt]\n"
                   "          [--keys=1E,9E,...] scancode set-1 bytes queued to the KBC (H4)\n"
                   "          [--kernel=path] direct-load an AuraLite-OS kernel.elf (KERNEL-BOOT K1);\n"
                   "          [--initrd=path] USTAR rootfs published via boot_info (KERNEL-BOOT K4)\n"
                   "          [--cpus=N] publish N CPUs in boot_info/MADT with ONE vCPU (KERNEL-BOOT K5 metering)\n"
                   "          [--smp=N] run N real vCPUs (max 2; use with --cpus=N for kernel SMP bring-up, K6)\n"
                   "                      skips --rom entirely and starts at the ELF entry\n", argv[0]);
            return 0;
        }
    }

    size_t rom_len = 0;
    uint8_t *rom = NULL;
    if (!kernel_path) {
        rom = read_file(rom_path, &rom_len);
        if (!rom) return 1;
    }

    machine_t *m = calloc(1, sizeof *m);
    m->cpu.mach = m;
    m->cfg_cpus = cfg_cpus;   /* K5: read by kload_boot's boot_info fill */
    m->cfg_smp  = cfg_smp;    /* K6: vcpu contexts; wired after devices_init */
    m->dbg_pit1 = smp_probe;  /* --smp-probe also arms the PIT ch2 trace */
    m->watch_phys = watch_phys;
    sig_m = m; sig_log = log_out;
    if (log_out) { signal(SIGTERM, flush_on_signal); signal(SIGINT, flush_on_signal); }
    mem_init(m, rom, rom_len);
    io_init(m);
    m->plat = (struct platform *)platform_by_name(platform_name);
    devices_init_common(m);
    devices_init_platform(m);
    if (keys_list && !keys_at && !keys_at_prompt && kbc_queue_keys(m, keys_list) < 0) {
        mlog(&m->log, "[kbc] --keys parse error (want hex bytes like 1E,9E,39)");
        return 1;
    }

    size_t disk_len = 0;
    uint8_t *disk = read_file(disk_path, &disk_len);
    if (disk) { m->disk = disk; m->disk_len = disk_len; }
    else if (!kernel_path) { mlog(&m->log, "[boot] no disk image loaded -- USB mass-storage reads will return nothing"); }

    /* STORE S1+: attach SATA port images the same way the USB stick gets its
     * backing. */
    for (int i = 0; i < AHCI_MAX_ATTACH; i++) {
        if (!sata_cfg[i]) continue;
        size_t len = 0;
        uint8_t *img = read_file(sata_cfg[i], &len);
        if (!img) { mlog(&m->log, "[ahci] port %d: cannot read %s -- port stays dark", i, sata_cfg[i]); continue; }
        m->sata_img[i] = img; m->sata_len[i] = len;
        mlog(&m->log, "[ahci] port %d: attached %s (%zu bytes)", i, sata_cfg[i], len);
    }

    if (kernel_path) {
        /* KERNEL-BOOT K1: the loader owns CPU initial state; cpu_reset()
         * happens inside kload_boot() before the contract overrides. */
        if (kload_boot(m, kernel_path, initrd_path) != 0) {
            devices_done(m);
            pci_done(m);
            mem_done(m);
            free(disk);
            for (int i = 0; i < AHCI_MAX_ATTACH; i++) free(m->sata_img[i]);
            free(m);
            return EXIT_INPUT_ERROR;
        }
    } else {
        cpu_reset(&m->cpu);
    }
    m->cpu.trace = trace;

    if (kernel_path)
        mlog(&m->log, "[boot] platform=%s kernel=%s (--kernel direct load; firmware skipped)",
             platform_by_name(platform_name)->name, kernel_path);
    else
        mlog(&m->log, "[boot] platform=%s rom=%s (%zu bytes) disk=%s (%zu bytes)",
             platform_by_name(platform_name)->name, rom_path, rom_len, disk_path, disk_len);

    /* K6 (--smp=2): wire the second execution context BEFORE the run.
     * Pristine power-on CPU image + the H6-reset LAPIC image with ID 1
     * (matching --cpus=2 kload/MADT metadata); parked in wait-for-SIPI,
     * exactly the MP spec's boot-time AP state. */
    if (cfg_smp >= 2) {
        m->n_vcpus = 2;
        m->vcpu[0].c = m->cpu; m->vcpu[0].l = m->lapic;
        m->vcpu[0].state = VCPU_RUN;
        m->vcpu[1].c.mach = m;
        cpu_reset(&m->vcpu[1].c);
        m->vcpu[1].l = m->lapic;
        m->vcpu[1].l.id = 1u << 24;
        m->vcpu[1].state = VCPU_WAIT_SIPI;
        m->cur_vcpu = 0;
        mlog(&m->log, "[smp] K6: 2 vCPU contexts live; vcpu1 parked in "
             "wait-for-SIPI (LAPIC id 1), strict 1:1 round-robin");
    }

    uint64_t n = 0;
    int step_result = 0;
    while (n < max_instr) {
        if (m->n_vcpus <= 1) {
        if (!m->guest_entry_seen && m->cpu.rip == 0x00100000ULL) {
            m->guest_entry_seen = 1;
            mlog(&m->log, "[guest] conventional entry reached at 0x00100000");
        }
        step_result = cpu_step(&m->cpu);
        if (step_result != 0)
            break;
        n++;
        if (!m->kmain_seen && m->kmain_va && m->cpu.rip == m->kmain_va) {
            m->kmain_seen = 1;
            mlog(&m->log, "[kernel] kmain reached -- AuraLite-OS C entry "
                 "(%llu instructions in)", (unsigned long long)n);
        }
        if (keys_list && ((keys_at && m->vtime_instr >= keys_at) ||
                          (keys_at_prompt && m->shell_prompt_seen))) {
            if (kbc_queue_keys(m, keys_list) < 0)
                mlog(&m->log, "[kbc] --keys-at parse error (want hex bytes like 1E,9E,39)");
            else
                mlog(&m->log, "[kbc] --keys-at fired (instr=%llu, prompt=%d)",
                     (unsigned long long)m->vtime_instr, keys_at_prompt);
            keys_at = 0; keys_at_prompt = 0;
        }
        if (m->stop_requested)
            break;
        } else {
        /* K6 dual-context: strict 1:1 round-robin.  A vcpu that hard-
         * stops (hlt+cli or a cpu fault) is parked off the rotation; the
         * run ends only when EVERY active context has stopped -- the same
         * endpoint semantics as UP, where there is one context. */
        if (smp_probe) {
            /* dbg probe (--smp-probe): the AP wake window only -- from the
             * WAIT_SIPI->RUN transition onward.  Tight + sparse on purpose:
             * a uniform probe in steady state flooded the 4096-line log
             * ring before flush (measured), hiding the exact window this
             * probe exists to see. */
            static int last1 = 0, wcount = 0;
            static uint64_t w0 = 0, wnext = 0;
            if (m->vcpu[1].state == VCPU_RUN && last1 != VCPU_RUN) {
                w0 = m->vtime_instr; wnext = w0;
                mlog(&m->log, "[smp-probe] AP wake window opens: vtime=%llu",
                     (unsigned long long)w0);
            }
            last1 = m->vcpu[1].state;
            /* First 2000 wake-window probes at 2^17 vtime spacing: the
             * race against the kernel's 100 ms bounded wait (~1.43M
             * vtime) needs sub-window resolution, but the ring must not
             * flood (measured k6-probe1). */
            if (w0 && wcount < 2000 && m->vtime_instr >= wnext &&
                m->vcpu[1].state == VCPU_RUN) {
                wcount++;
                vcpu_slot_t *s = &m->vcpu[1];
                mlog(&m->log, "[smp-probe] vt=%llu ap.i=%llu rip=%llx "
                     "brip=%llx ccr=%u ict=%u irr=%02x%02x",
                     (unsigned long long)(m->vtime_instr - w0),
                     (unsigned long long)s->c.instr_count,
                     (unsigned long long)s->c.rip,
                     (unsigned long long)m->vcpu[0].c.rip,
                     s->l.tmccur, s->l.tmict, s->l.irr[4], s->l.irr[3]);
                wnext = m->vtime_instr + (1ull << 17);
            }
        }
        int ran = 0;
        for (int i = 0; i < m->n_vcpus; i++) {
            if (m->vcpu[i].state != VCPU_RUN) continue;
            vcpu_load(m, i);
            step_result = cpu_step(&m->cpu);
            vcpu_store(m, i);
            n++;
            ran = 1;
            if (step_result != 0) {
                mlog(&m->log, "[smp] vcpu%d stopped (rc=%d) -- parked; "
                     "%d context(s) still live", i, step_result,
                     m->vcpu[1-i].state == VCPU_RUN ? 1 : 0);
                m->vcpu[i].state = VCPU_OFF;
            }
            if (keys_list && ((keys_at && m->vtime_instr >= keys_at) ||
                              (keys_at_prompt && m->shell_prompt_seen))) {
                if (kbc_queue_keys(m, keys_list) < 0)
                    mlog(&m->log, "[kbc] --keys-at parse error (want hex bytes like 1E,9E,39)");
                else
                    mlog(&m->log, "[kbc] --keys-at fired (instr=%llu, prompt=%d)",
                         (unsigned long long)m->vtime_instr, keys_at_prompt);
                keys_at = 0; keys_at_prompt = 0;
            }
            if (m->stop_requested)
                break;
        }
        if (!ran) {
            mlog(&m->log, "[smp] all vCPU contexts stopped/parked -- run ends");
            break;               /* step_result stays whatever it was */
        }
        if (m->stop_requested)
            break;
        }
    }

    /* K6: bring the BSP context back into the live pair for the final
     * register dump / fault verdict (its slot copy is authoritative). */
    if (m->n_vcpus > 1) vcpu_load(m, 0);
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
    for (int i = 0; i < AHCI_MAX_ATTACH; i++) free(m->sata_img[i]);
    free(rom);
    free(m);
    return rc;
}
