/* tests/test_perf.c -- PERF P0: the honest work counter.
 *
 * P0's premise is that this project had no way to state how fast it is, and
 * every counter it did have measures something else:
 *
 *   cpu.instr_count  the K6 clock.  Retired instructions PLUS the K3 idle
 *                    quantum (+512 per all-idle HLT slot, src/cpu.c), and
 *                    guest-visible through PIT/RTC/LAPIC/RDTSC -- so its
 *                    semantics are frozen by contract and it cannot become
 *                    the throughput numerator.
 *   machine.vtime    the machine-wide master clock; the K6 identity makes it
 *                    equal the BSP's instr_count for one context, and it is
 *                    what --max-instr budgets against, idle quantum included.
 *   --max-instr      a limit, not a measurement.  Dividing it by wall time is
 *                    the mistake this suite exists to make impossible: a tape
 *                    that stops early reports the cap's number, not its own.
 *
 * cpu.executed_steps is the counter P0 adds.  It increments only in cpu_step's
 * done: block (including faulting attempts), so it is 0 for a run that
 * only idled and equals
 * instr_count for a run that never idled.  machine_executed_total() (machine.h)
 * sums it across contexts and is shared with main.c's --stats block, so these
 * vectors assert the shipped formula rather than a copy of it.
 *
 * Every number asserted below is an invariant of the cpu_step done path, not of
 * one run's luck: program length, the 512 quantum, the K6 identity.  Nothing
 * here depends on wall time -- that is tests/perf_bench.sh's job, and it is
 * deliberately machine-relative (see PERF_PLAN.md P0).
 */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "machine.h"
#include "platform.h"
#include "harness.h"

#define FLAG_IF_BIT 9

static machine_t g_m;

static void fresh(void) {
    setup_machine(&g_m);
    g_m.n_vcpus = 0;          /* the historical single-context machine: only
                               * main.c's --smp>=2 wiring ever sets this */
    g_m.vtime_instr = 0;
}

/* Run `len` bytes at physical 0 for at most `steps` cpu_step calls. */
static void run(const uint8_t *prog, size_t len, int steps) {
    memcpy(g_m.ram, prog, len);
    g_m.cpu.rip = 0;
    g_m.cpu.gpr[RSP] = 0x8000;
    for (int i = 0; i < steps; i++)
        if (cpu_step(&g_m.cpu) != 0) break;
}

int main(void) {
    /* ---- 1. executed_steps counts exactly the instructions executed ------------
     * 1000 x NOP (0x90, one byte each) then HLT.  run_program stops on the
     * first non-zero cpu_step return, and HLT with IF=0 returns -1, so the
     * count is the program length and not "however far it got". */
    {
        fresh();
        static uint8_t prog[1001];
        memset(prog, 0x90, 1000);
        prog[1000] = 0xF4;                       /* HLT */
        run(prog, sizeof prog, 5000);
        assert(g_m.cpu.executed_steps == 1001);
        /* No idle slot was ever taken, so the clock and the work agree --
         * this is the UP firmware-boot shape (measured: executed_steps ==
         * instr_count == 4837677 when cap is 50000000). */
        assert(g_m.cpu.instr_count == 1001);
        assert(g_m.cpu.instr_count - g_m.cpu.executed_steps == 0);
        printf("ok   1000 x NOP + HLT executes exactly 1001, no idle inflation\n");
    }

    /* ---- 2. the idle quantum inflates the clock and NOT the work --------
     * STI;HLT is the K3 "sti;hlt stays alive" case: cpu_step returns 0 and
     * the caller keeps stepping, so every further slot takes the all-idle
     * branch and charges +512 to instr_count and vtime while retiring
     * nothing.  Two idle slots: 2 + 512 + 512 = 1026. */
    {
        fresh();
        const uint8_t prog[] = { 0xFB, 0xF4 };   /* STI ; HLT */
        run(prog, sizeof prog, 4);
        assert(g_m.cpu.executed_steps == 2);
        assert(g_m.cpu.instr_count == 1026);
        assert(g_m.vtime_instr == 1026);
        assert(g_m.cpu.instr_count - g_m.cpu.executed_steps == 1024);
        assert(g_m.cpu.halted == 1);
        assert((g_m.cpu.rflags >> FLAG_IF_BIT) & 1);
        printf("ok   idle slots charge the clock 512 each and execute 0 "
               "(executed_steps=2, instr_count=vtime=1026)\n");
    }

    /* ---- 3. the K6 identity still holds ---------------------------------
     * P0 adds a counter; it must not disturb the one the guest's clock is
     * built on.  With one context, vtime_instr equals the BSP's instr_count
     * bit for bit -- including the idle quantum, which is the point: vtime
     * is a clock, not a work count. */
    {
        fresh();
        const uint8_t prog[] = { 0xFB, 0xF4 };
        run(prog, sizeof prog, 3);
        assert(g_m.vtime_instr == g_m.cpu.instr_count);
        assert(g_m.vtime_instr == 514);          /* 2 executed_steps + one quantum */
        printf("ok   K6 identity vtime == BSP instr_count preserved (514)\n");
    }

    /* ---- 4. a faulting instruction is attempted ------------------------------
     * cpu_step's done: block counts the attempt (instr_count++) and then
     * reports the fault, so executed_steps follows it.  Asserted so that P1's
     * translation cache cannot quietly change which attempts are counted. */
    {
        fresh();
        /* UD2.  Deliberately not 0x06 (PUSH ES): the harness machine is real
         * mode -- CR0=0x60000010 with EFER.LMA clear -- where 0x06 is a VALID
         * 16-bit instruction and would retire without faulting. */
        const uint8_t prog[] = { 0x0F, 0x0B };   /* UD2 -> #UD */
        run(prog, sizeof prog, 4);
        assert(g_m.cpu.fault == 1);
        assert(g_m.cpu.executed_steps == 1);
        assert(g_m.cpu.instr_count == 1);
        printf("ok   a faulting attempt is counted once (executed_steps=1, fault=1)\n");
    }

    /* ---- 5. machine_executed_total sums the contexts ---------------------
     * Slot 0 lives in m->cpu (the K6 exit path re-loads it and a single
     * context never writes the slot); the rest are authoritative in their
     * slots.  n_vcpus is 0 for the historical machine, so the effective
     * context count is max(1, n) -- with 0 the loop must still count the
     * BSP, which is the bug this vector pins. */
    {
        fresh();
        g_m.cpu.executed_steps = 10;
        assert(g_m.n_vcpus == 0);
        assert(machine_executed_total(&g_m) == 10);

        g_m.n_vcpus = 1;
        assert(machine_executed_total(&g_m) == 10);

        g_m.n_vcpus = 2;
        g_m.vcpu[1].c.executed_steps = 7;
        assert(machine_executed_total(&g_m) == 17);
        printf("ok   machine_executed_total: 10 for n_vcpus 0 and 1, 17 for 2\n");
    }

    /* ---- 6. executed_steps scales with the program, not with the step budget ---
     * The same tape stepped 10000 times instead of 5000 executes the same
     * number: HLT with IF=0 ends the run, so a bigger budget cannot inflate
     * the count.  This is the unit-level form of the cap-vs-real check that
     * tests/perf_bench.sh applies to whole tapes. */
    {
        fresh();
        static uint8_t prog[1001];
        memset(prog, 0x90, 1000);
        prog[1000] = 0xF4;
        run(prog, sizeof prog, 10000);
        assert(g_m.cpu.executed_steps == 1001);
        assert(machine_executed_total(&g_m) == 1001);
        printf("ok   a larger step budget does not inflate executed_steps (1001)\n");
    }

    printf("test-perf: all vectors passed\n");
    return 0;
}
