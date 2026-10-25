# AuraLite Emulator Documentation

This directory holds the technical documentation for the emulator. The
repository root keeps the two documents a newcomer reads first: the
[README](../README.md) (what it is, how to build and run it) and the
[ROADMAP](../ROADMAP.md) (the plan ledger and the north star).

## Recommended reading order

1. [`../README.md`](../README.md) — scope, build, run, option reference.
2. [`STATUS.md`](STATUS.md) — what is implemented right now, with measured
   receipts.
3. [`architecture.md`](architecture.md) — layers, the machine contract, the CPU
   core, the chipset and device models, the three boot paths, the timebase, the
   tooling, and the invariants that must not move.
4. [`memory_map.md`](memory_map.md) — physical memory, I/O ports, the PCI tree,
   interrupt routing and the direct-kernel lane's low-memory layout.
5. [`plans/`](plans/) — 17 dependency-ordered plans (CORE, CHIPSET,
   KERNEL_BOOT, STORE, USB, VIDEO, NET, PERF, TOOLING, COMPAT, FPU, XHCI, MSI,
   PM, UPKEEP, CPU_FEATURES, VIRTIO), each with a definition of done and a test
   gate per phase.
6. [`BUGS.md`](BUGS.md) — firmware observations and how they were measured.
7. [`WINDOWS.md`](WINDOWS.md) — building native binaries on Windows 10/11 with
   MinGW-w64, or delegating to WSL2.

## Other references

- [`../CHANGELOG.md`](../CHANGELOG.md) — chronological changes, defect by
  defect, with the measurement that filed each one.
- [`../NOTICE.md`](../NOTICE.md) — licensing: Apache-2.0 for the emulator, MIT
  for the sample firmware.
- [`evidence/`](evidence/) — recorded stdout/stderr bundles with `SHA256SUMS`,
  re-checked by the acceptance gates.
- [`research/`](research/) — measurement notes, including the rejected PERF P4
  dispatch experiment.
- [`plans/PERF_PLAN.md`](plans/PERF_PLAN.md), [`plans/TOOLING_PLAN.md`](plans/TOOLING_PLAN.md) —
  the two plans that define how performance claims and observability are
  measured here.

## Documentation conventions

- **Closed** in a plan means every phase has its implementation, its regression
  vector, its one negative control, and a doc row — not that the code is
  finished for all time.
- **Receipt** means a recorded, comparable artifact (stdout line, `SHA256SUMS`
  entry, byte-identical dump), not a prose assertion.
- **SKIP** in a test lane means a dependency is absent (a stock `bios.bin`,
  frozen guest artifacts, `gdb`, a display). A skip is reported as a skip and
  never as a pass.
- Numbers in these documents are measured from the tree at the stated commit;
  when a count is quoted, the command that produced it is quoted next to it or
  in the changelog entry.

When you change code, update the matching table in this directory — and, if the
change is user-visible, the README option reference — in the same patch.
