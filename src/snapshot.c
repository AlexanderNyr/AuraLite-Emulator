// snapshot.c -- TOOLING T4: machine snapshots (see snapshot.h).
//
// File: "AURASNP1", u32 version, u32 sizeof(machine_t), u32 sizeof(cpu_t),
// then records { char tag[16]; u64 length; bytes } in a fixed order that
// one visitor walks for both directions, then an "end" record. Loading
// checks every tag and length against this build, so a record from a
// different build or machine shape is refused instead of misread.
#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <stddef.h>
#include "snapshot.h"
#include "cpu.h"
#include "pci.h"
#include "platform.h"

#define SNAP_MAGIC   "AURASNP1"
#define SNAP_VERSION 1u

typedef struct {
    snap_t pub;              /* first: the modules' view */
    FILE *f;
    int loading;
    int failed;
    char why[320];
    uint64_t bytes;
} sio_t;

static void snap_fail(sio_t *s, const char *fmt, ...) {
    if (s->failed) return;
    s->failed = 1;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(s->why, sizeof s->why, fmt, ap);
    va_end(ap);
}

static void xfer(sio_t *s, void *p, size_t n) {
    if (s->failed || !n) return;
    if (s->loading) { if (fread(p, 1, n, s->f) != n) snap_fail(s, "truncated snapshot file"); }
    else if (fwrite(p, 1, n, s->f) != n) snap_fail(s, "write error");
    s->bytes += n;
}

static void record(sio_t *s, const char *tag, uint64_t len) {
    char t[16];
    memset(t, 0, sizeof t);
    strncpy(t, tag, sizeof t - 1);
    if (!s->loading) { xfer(s, t, sizeof t); xfer(s, &len, sizeof len); return; }
    char rt[16];
    uint64_t rl = 0;
    xfer(s, rt, sizeof rt);
    xfer(s, &rl, sizeof rl);
    if (s->failed) return;
    rt[15] = 0;
    if (memcmp(rt, t, sizeof t))
        snap_fail(s, "record '%s' found where '%s' was expected (different build or machine shape)", rt, tag);
    else if (rl != len)
        snap_fail(s, "record '%s' holds %llu bytes, this build expects %llu (different build or machine shape)",
                  tag, (unsigned long long)rl, (unsigned long long)len);
}

static void sio_bytes(snap_t *pub, const char *tag, void *p, size_t n) {
    sio_t *s = (sio_t *)pub;
    record(s, tag, n);
    xfer(s, p, n);
}

static int page_is_zero(const uint8_t *p, size_t n) {
    for (size_t i = 0; i < n; i++) if (p[i]) return 0;
    return 1;
}

/* a large buffer as a 4 KiB page bitmap plus the non-zero pages */
static void sio_sparse(snap_t *pub, const char *tag, uint8_t *buf, size_t n) {
    sio_t *s = (sio_t *)pub;
    size_t pages = (n + 4095) / 4096, bml = (pages + 7) / 8;
    uint8_t *bm = calloc(1, bml ? bml : 1);
    if (!bm) { snap_fail(s, "out of memory"); return; }
    record(s, tag, n);
    if (!s->loading) {
        for (size_t p = 0; p < pages; p++) {
            size_t off = p * 4096, len = n - off < 4096 ? n - off : 4096;
            if (!page_is_zero(buf + off, len)) bm[p / 8] |= (uint8_t)(1u << (p % 8));
        }
        xfer(s, bm, bml);
    } else {
        xfer(s, bm, bml);
        if (!s->failed && buf) memset(buf, 0, n);
    }
    for (size_t p = 0; p < pages && !s->failed; p++) {
        if (!(bm[p / 8] & (1u << (p % 8)))) continue;
        size_t off = p * 4096, len = n - off < 4096 ? n - off : 4096;
        xfer(s, buf + off, len);
    }
    free(bm);
}

/* ------------------------------------------------------------- shape --- */
typedef struct {
    char platform[32];
    uint64_t rom_hash;
    uint64_t kmain_va;
    int32_t n_vcpus, cfg_cpus, cfg_smp, cfg_no_fb, cfg_no_uhci, pad;
    uint64_t disk_len;
    uint64_t sata_len[AHCI_CTRLS][AHCI_MAX_ATTACH];
    uint32_t pci_devices, owned_windows;
} shape_t;

static uint64_t fnv1a(const uint8_t *p, size_t n) {
    uint64_t h = 1469598103934665603ULL;
    for (size_t i = 0; i < n; i++) { h ^= p[i]; h *= 1099511628211ULL; }
    return h;
}

static void shape_of(machine_t *m, const snap_cfg_t *cfg, shape_t *sh) {
    memset(sh, 0, sizeof *sh);
    snprintf(sh->platform, sizeof sh->platform, "%s", cfg && cfg->platform ? cfg->platform : "?");
    sh->rom_hash = m->rom ? fnv1a(m->rom, m->rom_len) : 0;
    sh->kmain_va = m->kmain_va;
    sh->n_vcpus = m->n_vcpus; sh->cfg_cpus = m->cfg_cpus; sh->cfg_smp = m->cfg_smp;
    sh->cfg_no_fb = m->cfg_no_fb; sh->cfg_no_uhci = m->cfg_no_uhci;
    sh->disk_len = m->disk_len;
    for (int c = 0; c < AHCI_CTRLS; c++)
        for (int i = 0; i < AHCI_MAX_ATTACH; i++) sh->sata_len[c][i] = m->sata_len[c][i];
    for (struct pci_dev *d = m->pci_devices; d; d = d->next) sh->pci_devices++;
    for (mmio_region_t *r = m->mmio_list; r; r = r->next) sh->owned_windows += r->owned_ctx ? 1u : 0u;
}

static void shape_rec(machine_t *m, sio_t *s, const snap_cfg_t *cfg) {
    shape_t live, file;
    shape_of(m, cfg, &live);
    file = live;
    sio_bytes(&s->pub, "shape", &file, sizeof file);
    if (!s->loading || s->failed) return;
#define SAME(f, fmt) if (file.f != live.f) { snap_fail(s, "machine shape differs: " #f " " fmt " in the snapshot, " fmt " here", file.f, live.f); return; }
    if (strncmp(file.platform, live.platform, sizeof file.platform)) {
        snap_fail(s, "machine shape differs: platform %.31s in the snapshot, %.31s here", file.platform, live.platform);
        return;
    }
    if (file.rom_hash != live.rom_hash) { snap_fail(s, "machine shape differs: a different ROM image"); return; }
    if (file.kmain_va != live.kmain_va) { snap_fail(s, "machine shape differs: a different kernel (kmain at %llx vs %llx)",
                                                  (unsigned long long)file.kmain_va, (unsigned long long)live.kmain_va); return; }
    SAME(n_vcpus, "%d") SAME(cfg_cpus, "%d") SAME(cfg_smp, "%d") SAME(cfg_no_fb, "%d") SAME(cfg_no_uhci, "%d")
    if (file.disk_len != live.disk_len) { snap_fail(s, "machine shape differs: disk image size"); return; }
    if (memcmp(file.sata_len, live.sata_len, sizeof file.sata_len)) { snap_fail(s, "machine shape differs: SATA attachments"); return; }
    SAME(pci_devices, "%u") SAME(owned_windows, "%u")
#undef SAME
}

/* --------------------------------------------------------- machine_t --- */
/* Everything but the I/O dispatch table (static configuration), then the
 * host pointers and configuration fields are put back from the live
 * machine: they belong to this process, not to the snapshot. */
static void machine_rec(machine_t *m, sio_t *s) {
    size_t io_off = offsetof(machine_t, io), io_end = io_off + sizeof m->io;
    machine_t *t = malloc(sizeof *t);
    if (!t) { snap_fail(s, "out of memory"); return; }
    memcpy(t, m, sizeof *t);
    if (!s->loading) {   /* host addresses never reach the file: deterministic snapshots */
        t->cpu.mach = NULL; t->ram = NULL; t->rom = NULL; t->mmio_list = NULL;
        t->pci_devices = NULL; t->plat = NULL; t->fb = NULL; t->disk = NULL; t->dbg = NULL;
        memset(t->sata_img, 0, sizeof t->sata_img);
        for (int i = 0; i < EM_MAX_VCPU; i++) t->vcpu[i].c.mach = NULL;
    }
    sio_bytes(&s->pub, "machine.lo", t, io_off);
    sio_bytes(&s->pub, "machine.hi", (uint8_t *)t + io_end, sizeof *t - io_end);
    if (s->loading && !s->failed) {
        t->cpu.mach = m->cpu.mach;  t->cpu.trace = m->cpu.trace;
        t->ram = m->ram;  t->rom = m->rom;  t->rom_len = m->rom_len;
        t->mmio_list = m->mmio_list;  t->pci_devices = m->pci_devices;  t->plat = m->plat;
        t->fb = m->fb;  t->disk = m->disk;  t->disk_len = m->disk_len;
        memcpy(t->sata_img, m->sata_img, sizeof t->sata_img);
        memcpy(t->sata_len, m->sata_len, sizeof t->sata_len);
        t->max_instructions = m->max_instructions;
        t->kmain_va = m->kmain_va;
        t->cfg_cpus = m->cfg_cpus;  t->cfg_no_fb = m->cfg_no_fb;  t->cfg_no_uhci = m->cfg_no_uhci;
        t->cfg_smp = m->cfg_smp;  t->dbg_pit1 = m->dbg_pit1;  t->watch_phys = m->watch_phys;
        for (int i = 0; i < EM_MAX_VCPU; i++) { t->vcpu[i].c.mach = m->vcpu[i].c.mach; t->vcpu[i].c.trace = m->vcpu[i].c.trace; }
        t->dbg = m->dbg;
        t->stop_requested = 0;
        memcpy(m, t, io_off);
        memcpy((uint8_t *)m + io_end, (uint8_t *)t + io_end, sizeof *m - io_end);
    }
    free(t);
}

/* ---------------------------------------------------------- MMIO list --- */
typedef struct { uint64_t base, size; int32_t enabled, owned; char name[48]; } mmio_rec_t;

static void mmio_rec(machine_t *m, sio_t *s) {
    uint32_t n = 0;
    for (mmio_region_t *r = m->mmio_list; r; r = r->next) n++;
    uint32_t fn = n;
    sio_bytes(&s->pub, "mmio.count", &fn, sizeof fn);
    if (s->failed) return;
    if (!s->loading) {
        record(s, "mmio.list", (uint64_t)n * sizeof(mmio_rec_t));
        for (mmio_region_t *r = m->mmio_list; r; r = r->next) {
            mmio_rec_t e;
            memset(&e, 0, sizeof e);
            e.base = r->base; e.size = r->size; e.enabled = r->enabled; e.owned = r->owned_ctx;
            snprintf(e.name, sizeof e.name, "%s", r->name ? r->name : "");
            xfer(s, &e, sizeof e);
        }
        return;
    }
    record(s, "mmio.list", (uint64_t)fn * sizeof(mmio_rec_t));
    if (s->failed) return;
    mmio_rec_t *v = calloc(fn ? fn : 1, sizeof *v);
    if (!v) { snap_fail(s, "out of memory"); return; }
    xfer(s, v, (size_t)fn * sizeof *v);
    /* regions registered at run time sit at the head of the saved list:
     * replay them oldest first (only the host bridge's ECAM window does
     * this), then require an exact match */
    if (!s->failed && fn > n) {
        for (uint32_t i = fn - n; i-- > 0;) {
            if (strcmp(v[i].name, "MMCONFIG/ECAM")) { snap_fail(s, "snapshot has an unknown run-time MMIO region '%s'", v[i].name); break; }
            pci_ecam_map(m, v[i].base);
        }
    }
    uint32_t k = 0;
    for (mmio_region_t *r = m->mmio_list; r && !s->failed; r = r->next, k++) {
        if (k >= fn || r->base != v[k].base || r->size != v[k].size || strcmp(v[k].name, r->name ? r->name : "")) {
            snap_fail(s, "MMIO map differs from the snapshot at region %u (%s)", k, r->name ? r->name : "?");
            break;
        }
        r->enabled = v[k].enabled;
    }
    if (!s->failed && k != fn) snap_fail(s, "MMIO map differs from the snapshot (%u vs %u regions)", k, fn);
    free(v);
}

/* ------------------------------------------------------------ driver --- */
static void visit(machine_t *m, sio_t *s, const snap_cfg_t *cfg) {
    shape_rec(m, s, cfg);
    machine_rec(m, s);
    /* cpu.c statics: the device-cadence phase and whether the lazy
     * EMU_DBG_U lookup has run (it gates the P3 predecode path) */
    uint32_t cs[2] = { cpu_devtick_get(), (uint32_t)cpu_dbg_knob_resolved() };
    sio_bytes(&s->pub, "cpu.statics", cs, sizeof cs);
    if (s->loading && !s->failed) { cpu_devtick_set(cs[0]); if (cs[1]) cpu_dbg_knob_resolve(); }
    sio_sparse(&s->pub, "ram", m->ram, RAM_SIZE);
    mmio_rec(m, s);
    pci_snap(m, &s->pub);
    devices_snap(m, &s->pub);
    ahci_snap(m, &s->pub);
    if (m->disk) sio_sparse(&s->pub, "disk.usb", m->disk, m->disk_len);
    for (int c = 0; c < AHCI_CTRLS; c++)
        for (int i = 0; i < AHCI_MAX_ATTACH; i++)
            if (m->sata_img[c][i]) {
                char tag[16];
                snprintf(tag, sizeof tag, "disk.sata%u.%u", (unsigned)c & 7u, (unsigned)i & 63u);
                sio_sparse(&s->pub, tag, m->sata_img[c][i], m->sata_len[c][i]);
            }
    uint32_t end = 0x454E4421u;   /* "!DNE" */
    sio_bytes(&s->pub, "end", &end, sizeof end);
    if (s->loading && !s->failed && end != 0x454E4421u) snap_fail(s, "corrupt end marker");
}

static int refuse(const snap_cfg_t *cfg, const char *what) {
    if (cfg && cfg->tap) { fprintf(stderr, "[snapshot] %s refused: --tap connects the e1000 to host socket state outside the machine\n", what); return 1; }
    if (cfg && cfg->sata_wt) { fprintf(stderr, "[snapshot] %s refused: --sata-writethrough makes host image files part of the machine state\n", what); return 1; }
    return 0;
}

int snap_save(machine_t *m, const char *path, const snap_cfg_t *cfg) {
    if (refuse(cfg, "save")) return -1;
    FILE *f = fopen(path, "wb");
    if (!f) { fprintf(stderr, "[snapshot] cannot write %s\n", path); return -1; }
    sio_t s;
    memset(&s, 0, sizeof s);
    s.f = f;
    s.pub.bytes = sio_bytes; s.pub.sparse = sio_sparse; s.pub.loading = 0;
    uint32_t hdr[3] = { SNAP_VERSION, (uint32_t)sizeof(machine_t), (uint32_t)sizeof(cpu_t) };
    xfer(&s, SNAP_MAGIC, 8);
    xfer(&s, hdr, sizeof hdr);
    visit(m, &s, cfg);
    if (fclose(f) != 0 && !s.failed) snap_fail(&s, "write error");
    if (s.failed) { fprintf(stderr, "[snapshot] save to %s failed: %s\n", path, s.why); remove(path); return -1; }
    fprintf(stderr, "[snapshot] saved %s: %llu bytes at vtime %llu (step %llu)\n", path,
            (unsigned long long)s.bytes, (unsigned long long)m->vtime_instr, (unsigned long long)m->run_steps);
    return 0;
}

int snap_load(machine_t *m, const char *path, const snap_cfg_t *cfg) {
    if (refuse(cfg, "load")) return -1;
    FILE *f = fopen(path, "rb");
    if (!f) { fprintf(stderr, "[snapshot] cannot read %s\n", path); return -1; }
    sio_t s;
    memset(&s, 0, sizeof s);
    s.f = f;
    s.loading = 1;
    s.pub.bytes = sio_bytes; s.pub.sparse = sio_sparse; s.pub.loading = 1;
    char magic[8];
    uint32_t hdr[3] = {0, 0, 0};
    if (fread(magic, 1, 8, f) != 8 || memcmp(magic, SNAP_MAGIC, 8)) snap_fail(&s, "not an x86emu snapshot (bad magic)");
    xfer(&s, hdr, sizeof hdr);
    if (s.failed) { /* reported below */ }
    else if (hdr[0] != SNAP_VERSION) snap_fail(&s, "snapshot format version %u, this build reads %u", hdr[0], SNAP_VERSION);
    else if (!s.failed && (hdr[1] != sizeof(machine_t) || hdr[2] != sizeof(cpu_t)))
        snap_fail(&s, "snapshot from a different build (machine_t %u/%zu, cpu_t %u/%zu bytes)", hdr[1], sizeof(machine_t), hdr[2], sizeof(cpu_t));
    if (!s.failed) visit(m, &s, cfg);
    if (!s.failed && fgetc(f) != EOF) snap_fail(&s, "trailing bytes after the end record");
    fclose(f);
    if (s.failed) { fprintf(stderr, "[snapshot] load of %s refused: %s\n", path, s.why); return -1; }
    /* No perf_tlb_flush here: the restored P2/P3 entries and their epoch
     * are exactly the saving process's, consistent with the restored RAM,
     * and flushing would make --stats differ from the straight run. */
    fprintf(stderr, "[snapshot] loaded %s: vtime %llu, step %llu\n", path,
            (unsigned long long)m->vtime_instr, (unsigned long long)m->run_steps);
    return 0;
}
