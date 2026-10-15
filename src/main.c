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
#include "ahci.h"
#include "dbg.h"     /* TOOLING */
#include "gdbstub.h" /* TOOLING T3 */
#include "snapshot.h" /* TOOLING T4 */

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
    /* VIDEO V1: dump the DISPLAY, not raw VRAM -- the mode-3 console renders
     * in text mode; graphics mode copies m->fb exactly (historical bytes). */
    uint32_t *px = malloc((size_t)m->fb_w * (size_t)m->fb_h * 4);
    if (!px) { fclose(f); return; }
    vga_render_display(m, px, m->fb_w, m->fb_h);
    for (int i = 0; i < m->fb_w*m->fb_h; i++) {
        uint32_t p = px[i];
        uint8_t rgb[3] = { (uint8_t)(p>>16), (uint8_t)(p>>8), (uint8_t)p };
        fwrite(rgb, 1, 3, f);
    }
    free(px);
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
/* TOOLING T4: REPL/gdb `savevm`/`loadvm` reach the snapshot module through
 * dbg_snap_hook with the shape this process was started with. */
static snap_cfg_t g_snapcfg;
static int repl_snapshot(machine_t *m, const char *path, int load) {
    return load ? snap_load(m, path, &g_snapcfg) : snap_save(m, path, &g_snapcfg);
}
/* TOOLING T2: Ctrl-C under --debug pauses the guest instead of exiting. */
static void debug_on_sigint(int sig) { (void)sig; if (sig_m) dbg_request_pause(sig_m); }
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

/* TOOLING T2: one UP iteration of the run loop (formerly inline in main).
 * Inlined at two call sites with `tooled` constant, so the copy without a
 * tool attached carries no tool test at all. Returns 1 to end the run. */
typedef struct { const char *keys_list; uint64_t keys_at; int keys_at_prompt; } keys_trigger_t;
static inline __attribute__((always_inline))
int run_up_step(machine_t *m, uint64_t *n, int *step_result, keys_trigger_t *k, const int tooled) {
    if (!m->guest_entry_seen && m->cpu.rip == 0x00100000ULL) {
        m->guest_entry_seen = 1;
        mlog(&m->log, "[guest] conventional entry reached at 0x00100000");
    }
    *step_result = cpu_step(&m->cpu);
    if (*step_result != 0)
        return 1;
    (*n)++;
    if (tooled) { m->run_steps = *n; dbg_post_step(m); *n = m->run_steps; }   /* stops; loadvm resyncs */
    if (!m->kmain_seen && m->kmain_va && m->cpu.rip == m->kmain_va) {
        m->kmain_seen = 1;
        mlog(&m->log, "[kernel] kmain reached -- AuraLite-OS C entry "
             "(%llu instructions in)", (unsigned long long)*n);
    }
    if (k->keys_list && ((k->keys_at && m->vtime_instr >= k->keys_at) ||
                         (k->keys_at_prompt && m->shell_prompt_seen))) {
        if (kbc_queue_keys(m, k->keys_list) < 0)
            mlog(&m->log, "[kbc] --keys-at parse error (want hex bytes like 1E,9E,39)");
        else
            mlog(&m->log, "[kbc] --keys-at fired (instr=%llu, prompt=%d)",
                 (unsigned long long)m->vtime_instr, k->keys_at_prompt);
        k->keys_at = 0; k->keys_at_prompt = 0;
    }
    return m->stop_requested ? 1 : 0;
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
    const char *sata_cfg[AHCI_CTRLS][AHCI_MAX_ATTACH] = {{0}}; /* --sata/ --sata-portN (STORE S1+); S4: [ctrl][port], row 1 = --ahci2 */
    int cfg_ahci2 = 0;              /* --ahci2: second AHCI PCI function (STORE S4) */
    int cfg_no_uhci = 0;            /* --no-usb-uhci: EHCI-only machine shape (USB U5) */
    int cfg_no_fb = 0;              /* --no-fb: no linear fb published -- VGA text box (VIDEO V1) */
    int cfg_stats = 0;              /* --stats: PERF P0 measurement block (opt-in) */
    const char *text_out = NULL;    /* --dump-text=path: mode-3 console dump (VIDEO V1) */
    const char *vbe_arg = NULL;     /* --vbe=WxHxBPP|off: harness-side VBE mode-set (VIDEO V3) */
    const char *tap_arg = NULL;     /* --tap=host:port: e1000 + TAP socket (NET N0, opt-in) */
    int sata_wt = 0;                /* --sata-writethrough (STORE S5): guest writes
                                     * land in the host image files too */
    int cfg_cpus = 0;               /* --cpus=N: K5 SMP-path metering; boot_info
                                     * publishes N CPUs while the emulator still
                                     * runs one vCPU (0/1 = historical UP) */
    int cfg_smp = 0;                /* --smp=N: K6 real execution contexts
                                     * (currently max 2; 0/1 = one vCPU) */
    int smp_probe = 0;              /* EMU_DBG_SMP: vcpu1 vital-signs probe */
    uint64_t watch_phys = 0;        /* --watch-phys=addr: store probe */
    uint64_t max_instr = 50ull*1000*1000;
    int trace = 0;
    const char *trace_file = NULL;  /* --trace-file=path: TOOLING T1 disassembly trace */
    int debug_repl = 0;             /* --debug: TOOLING T2 REPL on stdin, stop at start */
    const char *debug_script = NULL;/* --debug-script=path: REPL commands from a file */
    const char *symbols_path = NULL;/* --symbols=elf: debugger symbols (default: --kernel) */
    const char *gdb_spec = NULL;    /* --gdb=[host:]port: TOOLING T3 gdbstub */
    const char *snap_save_path = NULL; /* --snapshot-save=path: TOOLING T4, at run end */
    const char *snap_load_path = NULL; /* --snapshot-load=path: TOOLING T4, before the run */
    uint64_t trace_from = 0;        /* --trace-from=VTIME: first traced instruction */
    uint64_t trace_count = 0;       /* --trace-count=N: stop after N lines (0 = all) */

    for (int i = 1; i < argc; i++) {
        if (!strncmp(argv[i], "--rom=", 6)) rom_path = argv[i]+6;
        else if (!strncmp(argv[i], "--disk=", 7)) disk_path = argv[i]+7;
        else if (!strncmp(argv[i], "--platform=", 11)) platform_name = argv[i]+11;
        else if (!strncmp(argv[i], "--max-instr=", 12)) max_instr = strtoull(argv[i]+12, NULL, 0);
        else if (!strncmp(argv[i], "--dump-fb=", 10)) fb_out = argv[i]+10;
        else if (!strncmp(argv[i], "--dump-text=", 12)) text_out = argv[i]+12;
        else if (!strcmp(argv[i], "--no-fb")) cfg_no_fb = 1;
        else if (!strcmp(argv[i], "--stats")) cfg_stats = 1;   /* PERF P0 */
        else if (!strncmp(argv[i], "--vbe=", 6)) vbe_arg = argv[i]+6;
        else if (!strncmp(argv[i], "--tap=", 6)) tap_arg = argv[i]+6;
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
         * after the longer --sata-port prefix.  S4: the --sata2 family
         * attaches to the --ahci2 controller the same way (longest
         * prefixes first so --sata2-portN cannot fall into --sata2= or
         * --sata=). */
        else if (!strncmp(argv[i], "--sata2-port", 12)) {
            const char *eq = strchr(argv[i] + 12, '=');
            int port = atoi(argv[i] + 12);
            if (eq && port >= 0 && port < AHCI_MAX_ATTACH && port < 64)
                sata_cfg[1][port] = eq + 1;
        }
        else if (!strncmp(argv[i], "--sata2=", 8)) sata_cfg[1][0] = argv[i]+8;
        else if (!strcmp(argv[i], "--ahci2")) cfg_ahci2 = 1;
        else if (!strcmp(argv[i], "--no-usb-uhci")) cfg_no_uhci = 1;
        else if (!strcmp(argv[i], "--sata-writethrough")) sata_wt = 1;   /* S5 */
        else if (!strncmp(argv[i], "--sata-port", 11)) {
            const char *eq = strchr(argv[i] + 11, '=');
            int port = atoi(argv[i] + 11);
            if (eq && port >= 0 && port < AHCI_MAX_ATTACH && port < 64)
                sata_cfg[0][port] = eq + 1;
        }
        else if (!strncmp(argv[i], "--sata=", 7)) sata_cfg[0][0] = argv[i]+7;
        else if (!strncmp(argv[i], "--cpus=", 7)) cfg_cpus = atoi(argv[i]+7);
        else if (!strncmp(argv[i], "--smp=", 6)) cfg_smp = atoi(argv[i]+6);
        else if (!strcmp(argv[i], "--smp-probe")) smp_probe = 1;
        else if (!strncmp(argv[i], "--watch-phys=", 13))
            watch_phys = strtoull(argv[i]+13, NULL, 0);
        else if (!strcmp(argv[i], "--trace")) trace = 1;
        else if (!strncmp(argv[i], "--trace-file=", 13)) trace_file = argv[i]+13;   /* TOOLING T1 */
        else if (!strncmp(argv[i], "--trace-from=", 13)) trace_from = strtoull(argv[i]+13, NULL, 0);
        else if (!strncmp(argv[i], "--trace-count=", 14)) trace_count = strtoull(argv[i]+14, NULL, 0);
        else if (!strcmp(argv[i], "--debug")) debug_repl = 1;                         /* TOOLING T2 */
        else if (!strncmp(argv[i], "--debug-script=", 15)) debug_script = argv[i]+15;
        else if (!strncmp(argv[i], "--symbols=", 10)) symbols_path = argv[i]+10;
        else if (!strncmp(argv[i], "--gdb=", 6)) gdb_spec = argv[i]+6;                   /* TOOLING T3 */
        else if (!strncmp(argv[i], "--snapshot-save=", 16)) snap_save_path = argv[i]+16;  /* TOOLING T4 */
        else if (!strncmp(argv[i], "--snapshot-load=", 16)) snap_load_path = argv[i]+16;
        else if (!strcmp(argv[i], "--help")) {
            printf("usage: %s [--rom=path] [--disk=path] [--platform=sandybridge|ivybridge|haswell|broadwell|baytrail]\n"
                   "          [--max-instr=N] [--trace] [--dump-fb=out.ppm] [--dump-text=out.txt] [--log=out.txt]\n"
                   "          [--no-fb] no linear framebuffer: VGA text mode 3 box (VIDEO V1)\n"
                  "          [--vbe=WxHxBPP|off] harness-side VBE mode-set through the dispi (VIDEO V3)\n"
                  "          [--tap=host:port] e1000 + TAP socket (NET N0; ground rule 127.0.0.1:8080)\n"
                   "          [--keys=1E,9E,...] scancode set-1 bytes queued to the KBC (H4)\n"
                   "          [--kernel=path] direct-load an AuraLite-OS kernel.elf (KERNEL-BOOT K1);\n"
                   "          [--initrd=path] USTAR rootfs published via boot_info (KERNEL-BOOT K4)\n"
                   "          [--cpus=N] publish N CPUs in boot_info/MADT with ONE vCPU (KERNEL-BOOT K5 metering)\n"
                   "          [--smp=N] run N real vCPUs (max 2; use with --cpus=N for kernel SMP bring-up, K6)\n"
                   "                      skips --rom entirely and starts at the ELF entry\n"
                   "          [--sata=img|--sata-portN=img] raw image on the onboard AHCI (STORE S1+)\n"
                   "          [--ahci2] second AHCI controller at 0:31:3 (STORE S4)\n"
                   "          [--no-usb-uhci] omit the UHCI companion (EHCI-only USB, USB U5)\n"
                   "          [--sata2=img|--sata2-portN=img] image on the --ahci2 controller (STORE S4)\n"
                   "          [--sata-writethrough] guest writes land in the host image files too (STORE S5)\n"
                  "          [--trace-file=path] disassembled trace, one line per instruction, with a\n"
                  "                      CPU-vs-disassembler self-check (TOOLING T1); never touches\n"
                  "                      the log ring.  [--trace-from=VTIME] [--trace-count=N]\n"
                  "          [--debug] stop before the first instruction and read debugger commands\n"
                  "                      from stdin (TOOLING T2; `help` lists them; Ctrl-C pauses)\n"
                  "          [--debug-script=path] the same commands from a file; EOF detaches\n"
                  "          [--symbols=elf] debugger symbols (default: the --kernel ELF)\n"
                  "          [--gdb=[host:]port] wait for gdb (`target remote host:port`; TOOLING T3);\n"
                  "                      host defaults to 127.0.0.1; vCPUs are threads; `monitor help`\n"
                  "          [--snapshot-save=path] save the machine when the run loop ends (TOOLING T4)\n"
                  "          [--snapshot-load=path] start from a saved machine; same build and machine\n"
                  "                      shape required; --max-instr stays a total budget\n"
                  "          [--stats] print the PERF P0 measurement block: executed_steps / instr_count /\n"
                  "                      vtime / cap.  Executed attempts, not idle clock\n"
                  "                      quanta, are the throughput numerator (faults count)\n", argv[0]);
            return 0;
        }
    }

    FILE *debug_in = NULL;
    if (debug_script) {   /* TOOLING T2: fail before any machine state exists */
        debug_in = fopen(debug_script, "r");
        if (!debug_in) { fprintf(stderr, "cannot open --debug-script=%s\n", debug_script); return EXIT_INPUT_ERROR; }
    }
    if (trace_file) {   /* TOOLING T1: fail before any machine state exists */
        FILE *tf = fopen(trace_file, "w");
        if (!tf) { fprintf(stderr, "cannot open --trace-file=%s\n", trace_file); return EXIT_INPUT_ERROR; }
        fclose(tf);
    }

    size_t rom_len = 0;
    uint8_t *rom = NULL;
    if (!kernel_path) {
        rom = read_file(rom_path, &rom_len);
        if (!rom) return 1;
    }

    int rc = EXIT_OK;
    machine_t *m = calloc(1, sizeof *m);
    m->cpu.mach = m;
    m->cfg_cpus = cfg_cpus;   /* K5: read by kload_boot's boot_info fill */
    m->cfg_no_uhci = cfg_no_uhci; /* USB U5: EHCI-only machine shape */
    m->cfg_no_fb = cfg_no_fb; /* VIDEO V1: VGA text box, no published fb */
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
    /* VIDEO V3: --vbe=WxHxBPP runs a harness-side VBE client's mode-set
     * through the real dispi io path before the CPU starts; --vbe=off is the
     * client's mode-3-forcing disable.  The display source follows the same
     * documented mode state a guest client's writes produce. */
    if (vbe_arg) {
        int vw, vh, vb;
        if (!strcmp(vbe_arg, "off")) {
            vbe_mode_disable(m);
        } else if (sscanf(vbe_arg, "%dx%dx%d", &vw, &vh, &vb) == 3 && vw > 0 && vh > 0) {
            vbe_mode_set(m, vw, vh, vb);
        } else {
            mlog(&m->log, "[vbe] --vbe parse error (want WxHxBPP or off)");
            return 1;
        }
    }
    /* NET N0: --tap=host:port attaches the opt-in e1000 with its TAP socket
     * (ground rule 127.0.0.1:8080).  Without the flag no PCI function is
     * added and the historical no-NIC receipts stay byte-stable. */
    if (tap_arg && e1000_attach(m, tap_arg) != 0) {
        mlog(&m->log, "[e1000] --tap attach failed (want host:port, default port 8080)");
        return 1;
    }
    if (keys_list && !keys_at && !keys_at_prompt && kbc_queue_keys(m, keys_list) < 0) {
        mlog(&m->log, "[kbc] --keys parse error (want hex bytes like 1E,9E,39)");
        return 1;
    }

    size_t disk_len = 0;
    uint8_t *disk = read_file(disk_path, &disk_len);
    if (disk) { m->disk = disk; m->disk_len = disk_len; }
    else if (!kernel_path) { mlog(&m->log, "[boot] no disk image loaded -- USB mass-storage reads will return nothing"); }

    /* STORE S1+: attach SATA port images the same way the USB stick gets its
     * backing.  S4: row 1 feeds the --ahci2 controller; bring the second
     * controller up only when asked (or when its ports carry images). */
    for (int c = 0; c < AHCI_CTRLS; c++) {
        if (c == 1 && !cfg_ahci2) {
            int any2 = 0;
            for (int i = 0; i < AHCI_MAX_ATTACH; i++) if (sata_cfg[1][i]) any2 = 1;
            if (!any2) continue;
            cfg_ahci2 = 1;   /* --sata2 without --ahci2: imply the controller */
        }
        if (c == 1) devices_add_ahci2(m);
        for (int i = 0; i < AHCI_MAX_ATTACH; i++) {
            if (!sata_cfg[c][i]) continue;
            size_t len = 0;
            uint8_t *img = read_file(sata_cfg[c][i], &len);
            if (!img) {
                mlog(&m->log, "[ahci%s] port %d: cannot read %s -- port stays dark",
                     c ? "2" : "", i, sata_cfg[c][i]);
                continue;
            }
            m->sata_img[c][i] = img; m->sata_len[c][i] = len;
            mlog(&m->log, "[ahci%s] port %d: attached %s (%zu bytes)",
                 c ? "2" : "", i, sata_cfg[c][i], len);
            /* S5: opt-in host write-through (default: copy-on-attach). */
            if (sata_wt) {
                if (ahci_wt_attach(m, c, i, sata_cfg[c][i]) != 0)
                    mlog(&m->log, "[ahci%s] port %d: writethrough open %s failed -- staying copy-on-attach",
                         c ? "2" : "", i, sata_cfg[c][i]);
                else
                    mlog(&m->log, "[ahci%s] port %d: writethrough -> %s",
                         c ? "2" : "", i, sata_cfg[c][i]);
            }
        }
    }

    if (kernel_path) {
        /* KERNEL-BOOT K1: the loader owns CPU initial state; cpu_reset()
         * happens inside kload_boot() before the contract overrides. */
        if (kload_boot(m, kernel_path, initrd_path) != 0) {
            devices_done(m);
            pci_done(m);
            mem_done(m);
            free(disk);
            ahci_wt_close_all();                    /* S5 */
            for (int c = 0; c < AHCI_CTRLS; c++)
                for (int i = 0; i < AHCI_MAX_ATTACH; i++) free(m->sata_img[c][i]);
            free(m);
            return EXIT_INPUT_ERROR;
        }
        /* VIDEO V1: boot_info.fb published -> the LFB is the display source
         * (vga_set_display_mode is a no-op under --no-fb, which locks the
         * display to the VGA text console). */
        vga_set_display_mode(m, 0);
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

    /* TOOLING T4: restore a saved machine before any tool attaches, so the
     * debugger, the trace and the run loop all start from it. */
    snap_cfg_t snapcfg = { platform_by_name(platform_name)->name, tap_arg != NULL, sata_wt };
    g_snapcfg = snapcfg;
    dbg_snap_hook = repl_snapshot;
    if (snap_load_path && snap_load(m, snap_load_path, &snapcfg) != 0) {
        rc = EXIT_INPUT_ERROR;
        if (debug_in) fclose(debug_in);
        goto teardown;
    }

    /* TOOLING T1: the trace hook reads code side-effect-free at every
     * instruction boundary; the CPU's own fetch/predecode path is untouched. */
    if (trace_file && dbg_trace_open(m, trace_file, trace_from, trace_count) != 0)
        fprintf(stderr, "cannot open --trace-file=%s\n", trace_file);

    /* TOOLING T2: debugger front end. Stops are wall-clock pauses only;
     * nothing on this path changes guest state unless the user does. */
    int debugging = debug_repl || debug_in || gdb_spec;
    if (debugging) {
        dbg_attach(m);
        const char *symp = symbols_path ? symbols_path : kernel_path;
        if (symp) {
            int ns = dbg_symbols_load(m, symp);
            fprintf(stderr, "[dbg] symbols: %d from %s\n", ns < 0 ? 0 : ns, symp);
        }
        if ((debug_repl || debug_in) &&
            dbg_repl_attach(m, debug_in ? debug_in : stdin, !debug_in, stderr) == 0) {
            debug_in = NULL;   /* owned by the REPL front end now */
            if (!debug_script) signal(SIGINT, debug_on_sigint);
        }
        if (gdb_spec && gdb_attach(m, gdb_spec) != 0)
            fprintf(stderr, "[gdb] not attached; the run continues without gdb\n");
        dbg_run_start(m, 1);
    }
    if (debug_in) fclose(debug_in);

    /* TOOLING T2: the step counter is published as m->run_steps for tools.
     * The UP loop exists twice -- `tooled` is a constant in each copy -- so
     * the untooled loop is the pre-T2 body (measured: a per-step tool test
     * cost ~3 host instructions per step). */
    keys_trigger_t keys = { keys_list, keys_at, keys_at_prompt };
    uint64_t n = m->run_steps;
    int step_result = 0;
    if (m->n_vcpus <= 1) {
        if (m->dbg) { while (n < max_instr) if (run_up_step(m, &n, &step_result, &keys, 1)) break; }
        else        { while (n < max_instr) if (run_up_step(m, &n, &step_result, &keys, 0)) break; }
    } else while (n < max_instr) {
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
            if (m->dbg && step_result == 0) { m->run_steps = n + 1; dbg_post_step(m); }   /* TOOLING T2 */
            vcpu_store(m, i);
            n++;
            ran = 1;
            if (step_result != 0) {
                mlog(&m->log, "[smp] vcpu%d stopped (rc=%d) -- parked; "
                     "%d context(s) still live", i, step_result,
                     m->vcpu[1-i].state == VCPU_RUN ? 1 : 0);
                m->vcpu[i].state = VCPU_OFF;
            }
            if (keys.keys_list && ((keys.keys_at && m->vtime_instr >= keys.keys_at) ||
                              (keys.keys_at_prompt && m->shell_prompt_seen))) {
                if (kbc_queue_keys(m, keys.keys_list) < 0)
                    mlog(&m->log, "[kbc] --keys-at parse error (want hex bytes like 1E,9E,39)");
                else
                    mlog(&m->log, "[kbc] --keys-at fired (instr=%llu, prompt=%d)",
                         (unsigned long long)m->vtime_instr, keys.keys_at_prompt);
                keys.keys_at = 0; keys.keys_at_prompt = 0;
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

    /* K6: bring the BSP context back into the live pair for the final
     * register dump / fault verdict (its slot copy is authoritative). */
    m->run_steps = n;
    /* TOOLING T4: the loop boundary is the snapshot point (before the BSP
     * is reloaded for the final dump, so SMP slots are exactly as left). */
    int snap_save_failed = snap_save_path && snap_save(m, snap_save_path, &snapcfg) != 0;
    if (m->n_vcpus > 1) vcpu_load(m, 0);
    if (m->dbg) dbg_run_end(m, step_result);   /* TOOLING T2: post-mortem stop */
    if (trace_file) dbg_trace_close(m, 1);   /* TOOLING T1: summary to stderr */
    if (m->guest_entry_seen && m->fb && m->fb[0] == 0x00200000u)
        mlog(&m->log, "[guest] framebuffer marker OK: pixel[0]=0x%08x", m->fb[0]);
    dump_regs(&m->cpu);

    /* PERF P0: the measurement block, opt-in so every existing tape keeps its
     * byte-exact stdout (make test greps it; the V/NET lanes hash it).
     *
     * Three counters, three different meanings, and the difference is the
     * defect this phase exists to make visible:
     *   executed_steps     instruction attempts actually executed, summed over every vCPU.
     *               Faults count as attempts (not architectural retirement).
     *               The only correct MIPS numerator for this run loop.
     *   instr_count the K6 clock: executed_steps PLUS 512 per all-idle HLT slot.
     *               Guest-visible, frozen by contract, and a fine way to
     *               overstate throughput on an idle-heavy tape.
     *   vtime       the machine-wide master clock; equals the BSP's
     *               instr_count when n_vcpus==1 (the K6 identity).
     * cap_reached=1 means the run was cut off, so its wall time measures the
     * cap and not the workload -- tests/perf_bench.sh refuses to report MIPS
     * for such a tape. */
    if (cfg_stats) {
        int nctx = m->n_vcpus > 1 ? m->n_vcpus : 1;
        uint64_t idle_sum = 0;
        printf("\n---- stats (PERF P0) ----\n");
        for (int i = 0; i < nctx && i < EM_MAX_VCPU; i++) {
            const cpu_t *vc = (m->n_vcpus > 1 && i > 0) ? &m->vcpu[i].c : &m->cpu;
            idle_sum += vc->instr_count - vc->executed_steps;
            printf("[stats] vcpu=%d executed_steps=%llu instr_count=%llu halted=%d fault=%d\n",
                   i, (unsigned long long)vc->executed_steps,
                   (unsigned long long)vc->instr_count,
                   vc->halted ? 1 : 0, vc->fault ? 1 : 0);
        }
        printf("[stats] executed=%llu idle_quantum=%llu instr_count_bsp=%llu vtime=%llu\n",
               (unsigned long long)machine_executed_total(m),
               (unsigned long long)idle_sum,
               (unsigned long long)m->cpu.instr_count,
               (unsigned long long)m->vtime_instr);
        printf("[stats] cap=%llu cap_reached=%d n_vcpus=%d\n",
               (unsigned long long)max_instr,
               (m->vtime_instr >= max_instr) ? 1 : 0, nctx);
        printf("[stats] tlb_hits=%llu tlb_misses=%llu tlb_flushes=%llu\n",
               (unsigned long long)m->perf_tlb_hits,
               (unsigned long long)m->perf_tlb_misses,
               (unsigned long long)m->perf_tlb_flushes);
        printf("[stats] decode_hits=%llu decode_misses=%llu\n",
               (unsigned long long)m->perf_decode_hits,
               (unsigned long long)m->perf_decode_misses);
    }

    printf("\n---- last log lines ----\n");
    int start = m->log.count < 60 ? 0 : m->log.count - 60;
    for (int i = start; i < m->log.count; i++) {
        int idx = (m->log.head - m->log.count + i + LOG_RING) % LOG_RING;
        printf("%s\n", m->log.lines[idx]);
    }

    if (fb_out) { write_ppm(m, fb_out); printf("\n[fb] framebuffer written to %s (%dx%d)\n", fb_out, m->fb_w, m->fb_h); }
    if (text_out) {
        vga_dump_text(m, text_out);
        printf("\n[vga] text console written to %s (%dx%d)\n", text_out, 80, 25);
    }
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
    if (m->cpu.fault)
        rc = EXIT_CPU_FAULT;
    else if (step_result != 0 && !m->cpu.halted)
        rc = EXIT_CPU_FAULT;
    if (snap_save_failed && rc == EXIT_OK) rc = EXIT_INPUT_ERROR;

teardown:

    /* C10: orderly teardown keeps the ASan/UBSan lane leak-clean. */
    dbg_detach(m);                                  /* TOOLING */
    devices_done(m);
    pci_done(m);
    mem_done(m);
    free(disk);
    ahci_wt_close_all();                            /* S5 */
    for (int c = 0; c < AHCI_CTRLS; c++)
        for (int i = 0; i < AHCI_MAX_ATTACH; i++) free(m->sata_img[c][i]);
    free(rom);
    free(m);
    return rc;
}
