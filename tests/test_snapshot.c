/* tests/test_snapshot.c -- TOOLING T4: snapshot format vectors.
 *
 * On a harness machine (no devices initialised -- the visitors must cope):
 *   1. split identity: run 100 steps, save, run to 400 (reference); a
 *      fresh machine loads the file and runs to 400 -> identical CPU,
 *      RAM, vtime, P2/P3 counters, log ring;
 *   2. determinism: two saves of the same state are the same bytes, and
 *      save -> load -> save is a fixed point;
 *   3. sparse RAM: a run touching a handful of pages produces a file far
 *      smaller than RAM;
 *   4. refusals with the exact reason: bad magic, other format version,
 *      other build layout, truncation at every record boundary class,
 *      trailing garbage, other platform, other vCPU count;
 *   5. UPKEEP U4: a relocated and a relocated+disabled LAPIC window
 *      save/load equivalently (functions continue at the guest's base).
 */
#include <stdio.h>
#include <string.h>
#include <assert.h>
#include "harness.h"
#include "cpu.h"
#include "lapic.h"
#include "snapshot.h"

static int fails;
#define EXPECT(cond, ...) do { if (!(cond)) { printf("FAIL " __VA_ARGS__); printf("\n"); fails++; } } while (0)

/* counts down from 1000 storing into a 2 KiB ring -> touches several pages */
static const uint8_t prog[] = {
    0xB9, 0xE8, 0x03,             /* mov cx,1000          */
    0xBF, 0x00, 0x30,             /* mov di,0x3000        */
    0x89, 0x0D,                   /* l: mov [di],cx       */
    0x83, 0xC7, 0x02,             /* add di,2             */
    0x81, 0xE7, 0xFF, 0x37,       /* and di,0x37ff        */
    0x49,                         /* dec cx               */
    0x75, 0xF4,                   /* jnz l                */
    0xF4                          /* hlt                  */
};

static machine_t A, B;
static const snap_cfg_t CFG = { "Haswell", 0, 0 };

static void boot(machine_t *m) {
    setup_machine(m);
    cpu_devtick_set(0);   /* a fresh process starts the device cadence at 0 */
    memcpy(m->ram, prog, sizeof prog);
    m->cpu.gpr[RSP] = 0x8000;
}
static void run_to(machine_t *m, uint64_t n) {
    while (m->run_steps < n) {
        if (cpu_step(&m->cpu) != 0) break;
        m->run_steps++;
    }
}

static long fsize(const char *p) {
    FILE *f = fopen(p, "rb");
    if (!f) return -1;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fclose(f);
    return n;
}
static int same_file(const char *a, const char *b) {
    FILE *x = fopen(a, "rb"), *y = fopen(b, "rb");
    int ok = x && y;
    while (ok) {
        int c = fgetc(x), d = fgetc(y);
        if (c != d) ok = 0;
        if (c == EOF || d == EOF) break;
    }
    if (x) fclose(x);
    if (y) fclose(y);
    return ok;
}

/* the reason snap_load printed goes to stderr; capture it */
static int load_reason(machine_t *m, const char *path, const snap_cfg_t *cfg, char *why, size_t n) {
    fflush(stderr);
    FILE *old = stderr;
    FILE *tmp = tmpfile();
    stderr = tmp;
    int rc = snap_load(m, path, cfg);
    fflush(tmp);
    stderr = old;
    rewind(tmp);
    size_t k = fread(why, 1, n - 1, tmp);
    why[k] = 0;
    fclose(tmp);
    return rc;
}

static void write_bytes(const char *path, const void *p, size_t n) {
    FILE *f = fopen(path, "wb");
    assert(f);
    fwrite(p, 1, n, f);
    fclose(f);
}
static size_t read_all(const char *path, uint8_t **out) {
    long n = fsize(path);
    FILE *f = fopen(path, "rb");
    assert(f && n > 0);
    *out = malloc((size_t)n);
    size_t k = fread(*out, 1, (size_t)n, f);
    fclose(f);
    return k;
}

/* UPKEEP U4: a snapshot taken with the LAPIC relocated (and one with it
 * relocated AND globally disabled) restores with the window exactly
 * where the guest left it, via lapic_sync_mmio on the load path. */
static machine_t SC, SD;                       /* snapshot U4 fixtures */
static void drop_mmio(machine_t *m) {
    while (m->mmio_list) {
        mmio_region_t *n = m->mmio_list->next;
        free(m->mmio_list);
        m->mmio_list = n;
    }
}
static void boot_lapic(machine_t *m) {
    drop_mmio(m);
    setup_machine(m);
    cpu_devtick_set(0);
    lapic_init(m);
    lapic_mmio_register(m);
}
/* "mov ecx,0x1B; mov eax,lo; mov edx,hi; wrmsr; jmp $" at 0x5000 */
static void guest_set_apicbase(machine_t *m, uint64_t v) {
    uint8_t code[24]; int n = 0;
    code[n++] = 0x66; code[n++] = 0xB9; code[n++] = 0x1B;
    code[n++] = 0; code[n++] = 0; code[n++] = 0;
    code[n++] = 0x66; code[n++] = 0xB8;
    for (int i = 0; i < 4; i++) code[n++] = (uint8_t)(v >> (8 * i));
    code[n++] = 0x66; code[n++] = 0xBA;
    for (int i = 0; i < 4; i++) code[n++] = (uint8_t)(v >> (8 * i + 32));
    code[n++] = 0x0F; code[n++] = 0x30; code[n++] = 0xEB; code[n++] = 0xFE;
    memcpy(m->ram + 0x5000, code, (size_t)n);
    m->cpu.rip = 0x5000;
    for (int i = 0; i < 8; i++) (void)cpu_step(&m->cpu);
}
static int u4_snapshot_cases(const char *ps, const char *ps2) {
    int f0 = fails;
    const uint64_t NEWB = 0xFEE80000ULL;
    int found;
    /* relocated + enabled */
    boot_lapic(&SC);
    mem_write(&SC, LAPIC_BASE + LAPIC_ID, 4, 0xAB000000u);
    guest_set_apicbase(&SC, NEWB | LAPIC_MSR_EN);
    EXPECT((uint32_t)mem_read(&SC, NEWB + LAPIC_VER, 4) == 0x00050014u, "src relocated window dead");
    assert(snap_save(&SC, ps, &CFG) == 0);
    boot_lapic(&SD);
    assert(snap_load(&SD, ps, &CFG) == 0);
    EXPECT((uint32_t)mem_read(&SD, NEWB + LAPIC_VER, 4) == 0x00050014u, "loaded relocated window dead");
    EXPECT((uint32_t)mem_read(&SD, NEWB + LAPIC_ID, 4) == 0xAB000000u, "loaded LAPIC ID lost");
    EXPECT((uint32_t)mem_read(&SD, LAPIC_BASE + LAPIC_VER, 4) == 0xFFFFFFFFu, "old window resurrected");
    EXPECT(cpu_get_msr(&SD.cpu, LAPIC_MSR_APICBASE, &found) == (NEWB | LAPIC_MSR_EN | LAPIC_MSR_BSP) && found,
           "loaded MSR not the relocated value");
    assert(snap_save(&SD, ps2, &CFG) == 0);
    EXPECT(same_file(ps, ps2), "relocated save -> load -> save not a fixed point");
    /* relocated + globally disabled */
    guest_set_apicbase(&SC, NEWB);                      /* EN=0 at the new base */
    assert(snap_save(&SC, ps, &CFG) == 0);
    boot_lapic(&SD);
    assert(snap_load(&SD, ps, &CFG) == 0);
    EXPECT((uint32_t)mem_read(&SD, NEWB + LAPIC_VER, 4) == 0xFFFFFFFFu, "disabled window decoded");
    EXPECT((uint32_t)mem_read(&SD, LAPIC_BASE + LAPIC_VER, 4) == 0xFFFFFFFFu, "default window decoded");
    EXPECT(cpu_get_msr(&SD.cpu, LAPIC_MSR_APICBASE, &found) == (NEWB | LAPIC_MSR_BSP) && found,
           "loaded MSR not the disabled relocated value");
    guest_set_apicbase(&SD, NEWB | LAPIC_MSR_EN);       /* guest revives it */
    EXPECT((uint32_t)mem_read(&SD, NEWB + LAPIC_VER, 4) == 0x00050014u, "revived window dead");
    EXPECT((uint32_t)mem_read(&SD, NEWB + LAPIC_ID, 4) == 0xAB000000u, "ID lost across disabled snapshot");
    drop_mmio(&SC); drop_mmio(&SD);
    if (fails == f0) printf("ok U4 snapshot: relocated and disabled LAPIC save/load equivalently\n");
    return fails - f0;
}

int main(void) {
    const char *p1 = "/tmp/test-snap-1.snap", *p2 = "/tmp/test-snap-2.snap", *p3 = "/tmp/test-snap-3.snap";
    const char *bad = "/tmp/test-snap-bad.snap";

    /* 1. split identity */
    boot(&A);
    run_to(&A, 100);
    mlog(&A.log, "[test] before save");
    assert(snap_save(&A, p1, &CFG) == 0);
    run_to(&A, 400);
    cpu_t ref = A.cpu;
    uint64_t ref_vt = A.vtime_instr, th = A.perf_tlb_hits, dh = A.perf_decode_hits;
    uint8_t *ref_ram = malloc(0x10000);
    memcpy(ref_ram, A.ram, 0x10000);
    logring_t ref_log = A.log;

    boot(&B);
    mlog(&B.log, "[test] a different history that the load must replace");
    assert(snap_load(&B, p1, &CFG) == 0);
    EXPECT(B.run_steps == 100 && B.vtime_instr == 100, "restored counters %llu/%llu",
           (unsigned long long)B.run_steps, (unsigned long long)B.vtime_instr);
    run_to(&B, 400);
    EXPECT(B.cpu.rip == ref.rip && !memcmp(B.cpu.gpr, ref.gpr, sizeof ref.gpr) && B.cpu.rflags == ref.rflags &&
           B.cpu.executed_steps == ref.executed_steps, "CPU state after split differs (rip %llx vs %llx)",
           (unsigned long long)B.cpu.rip, (unsigned long long)ref.rip);
    EXPECT(B.vtime_instr == ref_vt && B.perf_tlb_hits == th && B.perf_decode_hits == dh, "vtime/P2/P3 counters differ");
    EXPECT(!memcmp(B.ram, ref_ram, 0x10000), "RAM differs after split");
    EXPECT(B.log.count == ref_log.count && B.log.head == ref_log.head &&
           !memcmp(B.log.lines, ref_log.lines, sizeof ref_log.lines), "log ring differs after split");
    printf("ok split identity: save at 100, load in a fresh machine, run to 400 == straight run\n");

    /* 2. determinism and round trip */
    boot(&A);
    run_to(&A, 100);
    mlog(&A.log, "[test] before save");
    assert(snap_save(&A, p2, &CFG) == 0);
    EXPECT(same_file(p1, p2), "two saves of the same state differ");
    boot(&B);
    assert(snap_load(&B, p1, &CFG) == 0);
    assert(snap_save(&B, p3, &CFG) == 0);
    EXPECT(same_file(p1, p3), "save -> load -> save is not a fixed point");
    printf("ok determinism: identical files for identical states; save -> load -> save fixed point\n");

    /* 3. sparse RAM */
    u4_snapshot_cases("/tmp/test-snap-u4a.snap", "/tmp/test-snap-u4b.snap");
    long sz = fsize(p1);
    EXPECT(sz > 0 && sz < 2 * 1024 * 1024, "snapshot of a tiny program is %ld bytes", sz);
    printf("ok sparse: %ld-byte file for a %u MiB machine\n", sz, RAM_SIZE >> 20);

    /* 4. refusals */
    char why[512];
    uint8_t *img;
    size_t n = read_all(p1, &img);
    write_bytes(bad, "NOTASNAPSHOT", 12);
    EXPECT(load_reason(&B, bad, &CFG, why, sizeof why) != 0 && strstr(why, "bad magic"), "bad magic: %s", why);
    uint8_t *mod = malloc(n);
    memcpy(mod, img, n); mod[8] ^= 0x7F;                       /* format version */
    write_bytes(bad, mod, n);
    EXPECT(load_reason(&B, bad, &CFG, why, sizeof why) != 0 && strstr(why, "format version"), "version: %s", why);
    memcpy(mod, img, n); mod[12] ^= 0x01;                      /* sizeof(machine_t) */
    write_bytes(bad, mod, n);
    EXPECT(load_reason(&B, bad, &CFG, why, sizeof why) != 0 && strstr(why, "different build"), "layout: %s", why);
    static const size_t cuts[] = { 20, 30, 200, 70000, 0 };
    for (int i = 0; cuts[i]; i++) {
        if (cuts[i] >= n) continue;
        write_bytes(bad, img, cuts[i]);
        EXPECT(load_reason(&B, bad, &CFG, why, sizeof why) != 0 && strstr(why, "truncated"), "cut at %zu: %s", cuts[i], why);
    }
    write_bytes(bad, img, n - 1);
    EXPECT(load_reason(&B, bad, &CFG, why, sizeof why) != 0, "cut 1 byte short must fail");
    memcpy(mod, img, n);
    {
        FILE *f = fopen(bad, "wb");
        fwrite(img, 1, n, f);
        fputc(0x55, f);
        fclose(f);
    }
    EXPECT(load_reason(&B, bad, &CFG, why, sizeof why) != 0 && strstr(why, "trailing"), "trailing: %s", why);
    snap_cfg_t other = { "Broadwell", 0, 0 };
    EXPECT(load_reason(&B, p1, &other, why, sizeof why) != 0 && strstr(why, "platform Haswell in the snapshot, Broadwell here"),
           "platform: %s", why);
    boot(&B);
    B.n_vcpus = 2;
    EXPECT(load_reason(&B, p1, &CFG, why, sizeof why) != 0 && strstr(why, "n_vcpus"), "vcpus: %s", why);
    snap_cfg_t tap = { "Haswell", 1, 0 };
    EXPECT(load_reason(&B, p1, &tap, why, sizeof why) != 0 && strstr(why, "--tap"), "tap: %s", why);
    printf("ok refusals: bad magic, version, build layout, truncation x6, trailing bytes, platform, vCPUs, --tap\n");
    free(img); free(mod); free(ref_ram);
    remove(p1); remove(p2); remove(p3); remove(bad);
    remove("/tmp/test-snap-u4a.snap"); remove("/tmp/test-snap-u4b.snap");
    if (fails) { printf("test-snap: %d FAILED\n", fails); return 1; }
    puts("test-snap: all vectors passed");
    return 0;
}
