/* tests/test_msi.c -- MSI M2 vectors: the LAPIC message-delivery core.
 *
 * msi_deliver() is driven with raw capability-shaped (addr, data) pairs
 * and the landing is observed through the real LAPIC arbitration
 * (deliverable -> intack -> EOI), exactly what a vCPU would see.
 */
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "devices.h"

static int fails;
#define EXPECT(cond, ...) do { if (!(cond)) { printf("FAIL " __VA_ARGS__); printf("\n"); fails++; } } while (0)

static machine_t M;

static void eoi(void) { mem_write(&M, 0xFEE000B0u, 4, 0); }
static int take(void) { int v = lapic_intack(&M); if (v >= 0) eoi(); return v; }

#define MSG_ADDR(dest, logical) (0xFEE00000ull | ((uint64_t)(dest) << 12) | ((logical) ? 4ull : 0ull))
#define MSG_DATA(vec, dmode)    ((uint32_t)(vec) | ((uint32_t)(dmode) << 8))

/* ---- v_fixed: physical Fixed delivery --------------------------------- */
static void v_fixed(void) {
    int f0 = fails;
    EXPECT(lapic_deliverable(&M) < 0, "ring empty at start");
    msi_deliver(&M, MSG_ADDR(0, 0), MSG_DATA(0x42, 0));
    EXPECT(lapic_deliverable(&M) == 0x42, "vector 0x42 pending (got %d)", lapic_deliverable(&M));
    EXPECT(take() == 0x42, "intack returns 0x42");
    EXPECT(lapic_deliverable(&M) < 0, "drained after ack+EOI");
    /* an unroutable physical destination falls back to the BSP (IOAPIC rule) */
    msi_deliver(&M, MSG_ADDR(0x55, 0), MSG_DATA(0x51, 0));
    EXPECT(take() == 0x51, "dest 0x55 (nobody) still lands on the BSP");
    /* a non-APIC address is not an MSI at all */
    msi_deliver(&M, 0xFEC00000ull, MSG_DATA(0x60, 0));
    EXPECT(lapic_deliverable(&M) < 0, "IOAPIC-window address ignored");
    if (fails == f0) printf("ok fixed: physical delivery, BSP fallback, non-APIC ignored\n");
}

/* ---- v_modes: delivery-mode honesty ----------------------------------- */
static void v_modes(void) {
    int f0 = fails;
    /* Lowest Priority: no arbiter on this machine -> lands as Fixed */
    msi_deliver(&M, MSG_ADDR(0, 0), MSG_DATA(0x44, 1));
    EXPECT(take() == 0x44, "Lowest Priority degenerates to Fixed here");
    /* SMI(2), NMI(4), INIT(5), ExtINT(7), reserved(3,6): dropped, logged */
    static const uint32_t dropped[5] = { 2, 3, 4, 5, 7 };
    for (int i = 0; i < 5; i++) {
        msi_deliver(&M, MSG_ADDR(0, 0), MSG_DATA(0x70 + i, dropped[i]));
        EXPECT(lapic_deliverable(&M) < 0, "delivery mode %u fabricated nothing", dropped[i]);
    }
    /* illegal vector: ESR receive-illegal-vector, nothing pending */
    msi_deliver(&M, MSG_ADDR(0, 0), MSG_DATA(0x08, 0));
    EXPECT(lapic_deliverable(&M) < 0, "vector < 16 does not pend");
    EXPECT(M.lapic.esr & (1u << 6), "ESR illegal-vector bit set");
    M.lapic.esr = 0;
    if (fails == f0) printf("ok modes: LP=Fixed, SMI/NMI/INIT/ExtINT dropped, ESR on vec<16\n");
}

/* ---- v_logical: flat logical destinations via LDR --------------------- */
static void v_logical(void) {
    int f0 = fails;
    /* BSP claims logical id 0x02 (LDR[31:24]) */
    mem_write(&M, 0xFEE000D0u, 4, 0x02000000u);
    msi_deliver(&M, MSG_ADDR(0x02, 1), MSG_DATA(0x46, 0));
    EXPECT(take() == 0x46, "logical dest matching LDR delivers");
    /* no claimant for 0x40: BSP fallback, still lands (documented) */
    msi_deliver(&M, MSG_ADDR(0x40, 1), MSG_DATA(0x47, 0));
    EXPECT(take() == 0x47, "unclaimed logical dest falls back to the BSP");
    mem_write(&M, 0xFEE000D0u, 4, 0);
    if (fails == f0) printf("ok logical: LDR claimant wins, unclaimed falls back\n");
}

/* ---- v_shape: the exact values an OS programs for xHCI entry 0 -------- */
static void v_shape(void) {
    int f0 = fails;
    /* What a driver writes into an MSI-X entry for "vector 0x61 to CPU0":
     * addr 0xFEE00000 (possibly with the +0xC interrupt-remapping-free
     * low bits clear), data = the bare vector.  Plus the level/trigger
     * bits some stacks set on level-triggered sources: advisory only. */
    msi_deliver(&M, 0xFEE00000ull, 0x61u);
    EXPECT(take() == 0x61, "bare-vector message lands");
    msi_deliver(&M, 0xFEE00000ull, 0x61u | (1u << 14) | (1u << 15));
    EXPECT(take() == 0x61, "level/trigger bits are advisory, still one edge");
    EXPECT(lapic_deliverable(&M) < 0, "no phantom second edge");
    if (fails == f0) printf("ok shape: driver-programmed addr/data land verbatim\n");
}

/* ---- v_determinism: the same script lands identically twice ---------- */
static void v_determinism(void) {
    int f0 = fails;
    static const struct { uint64_t a; uint32_t d; } script[5] = {
        { 0xFEE00000ull, 0x60 }, { 0xFEE00000ull, 0x61 },
        { 0xFEE04000ull, 0x22 },                    /* dest 4: BSP fallback */
        { 0xFEE00000ull, 0x08 },                    /* illegal vector */
        { 0xFEE00004ull, 0x63 },                    /* logical, dest 0 */
    };
    int got[2][5];
    for (int r = 0; r < 2; r++) {
        M.lapic.esr = 0;
        for (int i = 0; i < 5; i++) {
            msi_deliver(&M, script[i].a, script[i].d);
            got[r][i] = take();
        }
    }
    EXPECT(memcmp(got[0], got[1], sizeof got[0]) == 0,
           "identical message scripts -> identical intack streams");
    EXPECT(got[0][0] == 0x60 && got[0][3] < 0, "script sanity (illegal vec dropped)");
    if (fails == f0) printf("ok determinism: message script replayed x2 identically\n");
}

int main(void) {
    devices_init_common(&M);
    M.lapic.svr |= 0x100u;   /* APIC software-enable, as any OS does first */
    v_fixed();
    v_modes();
    v_logical();
    v_shape();
    v_determinism();
    if (fails) { printf("test-msi: %d FAILED\n", fails); return 1; }
    printf("test-msi: all vectors passed\n");
    return 0;
}
