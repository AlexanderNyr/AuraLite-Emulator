# COMPAT G6 — 4-vCPU BIOS SMP bring-up, MADT publication, and guest SMP tests

Patch **0065** — final rung of `docs/plans/COMPAT_PLAN.md`.

The G6 gate required the BIOS route (`SeaBIOS → Stage 2 → kernel.elf → init`)
to boot a kernel with **more than two** executing contexts (`--smp=4`), wake
real APs over the IOAPIC/LAPIC INIT-SIPI-SIPI path, tick per-AP timers from the
shared virtual clock without perturbing UP or `--smp=2` receipts, and pass the
guest's `[smp] AP #k online`, `irqapwake`, and `smpstress` suites.

## What was missing on the BIOS SMP path

Tracing `smp_setup()` (`0xf1538`), `qemu_get_present_cpus_count()` (`0xe8040`),
`handle_smp()` (`0xef526`), and `build_madt()` (`0xe9a3f`) in SeaBIOS 1.16.3-2
exposed five concrete gaps between the K6 `--kernel --cpus=2 --smp=2` path and
a multi-AP BIOS boot:

1. **Context capacity (`src/machine.h`, `src/main.c`):** `EM_MAX_VCPU` was `2`,
   and `main.c` clamped `--smp>=2` to 2 contexts (`vcpu0` + `vcpu1`) and
   assumed only 1 context remained live whenever any AP parked (`cli; hlt`).
2. **CMOS `0x5F` (`src/rtc.c`, `src/main.c`):** SeaBIOS's `smp_probe()` reads
   CMOS index `0x5F` (`CMOS_BIOS_SMP_COUNT`) and sets
   `MaxCountCPUs = CMOS[0x5F] + 1`. Without `CMOS[0x5F] = N - 1`, `build_madt()`
   emits only 1 Processor Local APIC structure (`cpus=1`) regardless of `--smp`.
3. **Broadcast INIT and SIPI (`src/lapic.c`):** SeaBIOS's `smp_setup()` wakes
   all APs at once using ICR destination shorthand `3` ("all excluding self"):
   `ICR_LO = 0x000C4500` (broadcast INIT) followed by `0x000C4610` (broadcast
   SIPI, vector `0x10` → `0x1000:0x0000`). `lapic_icr_write()` previously
   rejected `shorthand != 0` on the `dm == 5` / `dm == 6` path.
4. **Initial APIC ID in `CPUID(1).EBX[31:24]` (`src/cpu.c`):** Inside the
   SeaBIOS AP trampoline (`smp_write_msr` at `0xef526`), each woken AP executes
   `cpuid` with `EAX=1`, shifts `EBX` right by 24, and sets bit `(1 << apic_id)`
   in the `FoundAPICIDs` bitmask (`0xf5a00`). `build_madt()` then sets each
   MADT Type-0 entry's `Flags.Enabled` bit from `FoundAPICIDs`. `do_cpuid()`
   previously hardcoded `EBX = 0` on leaf 1, so every AP reported `apic_id = 0`
   and `build_madt()` marked APIC IDs `1..N-1` disabled (`flags=0`), which
   Stage 2 (`acpi.inc:211`, `test dword [es:di+4], 1`) skips.
5. **Counter preservation across second-stage `INIT` (`src/lapic.c`):** On the
   BIOS route each AP is woken twice: first by SeaBIOS POST (which parks the AP
   in `cli; hlt` → `VCPU_OFF` after 736–2,038 instructions), and later by the
   kernel's `smp_init()` via targeted physical `INIT` + `SIPI` at `0x8000`.
   `vcpu_take_init()` called `cpu_reset(&s->c)`, wiping the AP's accumulated
   `executed_steps`, `instr_count`, `idle_quantum`, and `last_vtime` from POST
   and breaking the global work invariants `sum(executed_steps) == executed`
   and `sum(instr_count) == vtime`.

## The fix

- `src/machine.h`: raise `EM_MAX_VCPU` from `2` to `8`.
- `src/rtc.c`, `src/main.c`: populate `CMOS[0x5F] = nv - 1` when `nv > 1`
  (`0` in UP, keeping all UP CMOS bytes unchanged).
- `src/cpu.c`: set `CPUID(1).EBX[31:24] = m->lapic.id & 0xFF000000u` (`0` on
  BSP/UP, `i << 24` on `vcpu_i`).
- `src/lapic.c`: handle `shorthand == 2` (all incl. self) and `shorthand == 3`
  (all excl. self) for `dm == 5` (INIT) and `dm == 6` (SIPI), and preserve
  `executed_steps`, `instr_count`, `idle_quantum`, and `last_vtime` across
  `cpu_reset(&s->c)` in `vcpu_take_init()`.
- `src/main.c`: wire `nv = min(cfg_smp, EM_MAX_VCPU)` contexts with
  `l.id = i << 24`, and count remaining `VCPU_RUN` slots when an AP parks in
  `VCPU_OFF`.

## Measured results (`--lpc=ich9 --smp=4`)

1. **Diagnostic SeaBIOS POST (`seabios-diag-smp4.stdout.txt`, 10 M cap):**

       [smp] G6: 4 vCPU contexts live; vcpu1..3 parked in wait-for-SIPI (LAPIC ids 1..3), strict 1:1 round-robin
       handle_smp: apic_id=0x1
       handle_smp: apic_id=0x2
       handle_smp: apic_id=0x3
       Found 4 cpu(s) max supported 4 cpu(s)

2. **ACPI MADT and `boot_info_t` handoff (`acpi-receipt-smp4.txt`, `bootinfo-smp4-parse.txt`):**
   - MADT (`sig=APIC`, `len=134`, `checksum=ok`) publishes `cpus=4 ioapics=1 isos=4`
     (up from `len=110, cpus=1` in G5: `3 * 8 = 24` bytes of additional enabled
     Type-0 Processor Local APIC records).
   - Stage 2 parses all 4 enabled entries into `boot_info_t` at `0x10000`:
     `cpu_count=4`, `bsp_lapic_id=0`, `cpu[0..3]` with `processor_id = lapic_id = 0..3`.

3. **Full BIOS boot + guest SMP test suite (`x86emu-smp4.serial.log.gz`, `x86emu-smp4.stdout.txt`):**

       [serial] [BL9] ACPI RSDP found
       [serial] [BL9] ACPI MADT parsed
       [serial] [smp] BSP lapic_id=0, 4 total CPUs detected
       [serial] [smp] AP #0 online (lapic_id=1)
       [serial] [smp] AP #1 online (lapic_id=2)
       [serial] [smp] AP #2 online (lapic_id=3)
       [serial] [smp] 4 CPU(s) online (3 AP(s) woken)
       [serial] [smp] PASS: multi-core system detected
       [serial] [sched] R5 receipt: user thread pid=6 on AP cpu=3
       [serial] [kernel] shell active; kmain idling.
       [serial] auralite#
       [kbc] --keys-at fired (instr=2362343099, prompt=1)
       [serial] [smpwake] PASS: 14 RTC(GSI8) device IRQs delivered to cpu1 (apic id 1); hlt looper woken 5 times -- RES-16 receipt
       [serial] IRQAPWAKE PASS: kernel receipt returned 0
       [serial] auralite#
       [serial] SMPSTRESS: O_APPEND 6 x 200 x 32 B intact
       [serial] SMPSTRESS: 100 fork/wait cycles precise
       [serial] SMPSTRESS: 8 x 10 signal deliveries counted
       [serial] SMPSTRESS PASS
       [serial] auralite#

   Final per-vCPU stats (`cap=4000000000`, `fault=0` on all 4 vCPUs):

       [stats] vcpu=0 executed_steps=935101005 instr_count=46810588237 halted=0 fault=0
       [stats] vcpu=1 executed_steps=82923322 instr_count=44799802170 halted=1 fault=0
       [stats] vcpu=2 executed_steps=77538628 instr_count=44794425668 halted=1 fault=0
       [stats] vcpu=3 executed_steps=91904984 instr_count=44808793560 halted=1 fault=0
       [stats] executed=1187467939 idle_quantum=180026141696 instr_count_bsp=46810588237 vtime=181213609635

   Exact work identities:
   - `sum(executed_steps) = 935101005 + 82923322 + 77538628 + 91904984 = 1187467939 == executed`
   - `sum(instr_count) = 46810588237 + 44799802170 + 44794425668 + 44808793560 = 181213609635 == vtime`
   - `executed + idle_quantum = 1187467939 + 180026141696 = 181213609635 == vtime`

4. **Historical lane byte-identity:** `tests/integration/p5_history.sh` (all 9
   firmware/video/`smp2` lanes) plus 40 M guest `--kernel` UP and `--cpus=2 --smp=2`
   runs are byte-identical against the G5 binary (`ad81722`).

## Files

- `x86emu-smp4.receipt` — RC and wall time of the 4 G acceptance run.
- `x86emu-smp4.stdout.txt` — final CPU state and 4-vCPU PERF P0 stats block.
- `x86emu-smp4.serial.log.gz` — full serial + emulator log (`gzip -9n`).
- `bootinfo-smp4.bin` / `bootinfo-smp4-parse.txt` — 7,776-byte `boot_info_t`
  handoff block with `cpu_count=4` and its decoded report.
- `acpi-receipt-smp4.txt` — RSDP → RSDT → 134-byte MADT (`cpus=4`).
- `seabios-diag-smp4.stdout.txt` — diagnostic SeaBIOS 4-CPU POST output.
- `nc-smp.txt` — negative control proof (`tests/integration/g6_smp_nc.sh`).
- `commands.txt`, `inputs.txt`, `SHA256SUMS`.
