# d3_differential -- host reference for LES / LDS (and why BOUND has none)

`LES`/`LDS`/`BOUND` are legacy-mode only (`C4`/`C5` are VEX and `62` is EVEX in
64-bit mode), so the reference runs at **CPL3**: the host as a static `-m32`
binary, this core in **32-bit protected mode** with a GDT laid out the way
i386 Linux lays it out (null, kernel code/data at DPL0, user code `0x23`, user
data `0x2b` at DPL3).

```sh
gcc -m32 -O1 -no-pie -o host32 host32.c ref32.S && ./host32 > host.out

gcc -std=c11 -O1 -Isrc -Itests tests/d3_differential/emu.c \
    src/cpu.c src/mem.c src/io.c src/platform.c src/pic.c src/pit.c \
    src/pm.c src/rtc.c src/kbc.c src/chipset.c src/lapic.c src/ioapic.c \
    -o emu -lm && ./emu > emu.out

diff host.out emu.out   # empty = 10/10 identical
```

The selector matrix is the interesting part, because the SDM's rule is *not*
"a data segment": `0x2b` (user data) and `0x23` (user **code**, readable) both
load, `0000` loads and clears the register, `00F8` (past the GDT) and `FFFF`
raise `#GP` **without** writing the offset register. All five behave
identically on both sides, for `LES` and for `LDS`.

## BOUND: no host reference here

`bound` on this sandbox's 32-bit personality dies with `SEGV_MAPERR` at the
instruction for **every** index, in range or not -- the sandbox, not the CPU,
is at fault, and no amount of harness work gets a usable reference out of it.
The emulated `BOUND` is therefore pinned by the SDM rule (signed compare at
the operand size, `#BR` = vector 5) plus the `tests/test_table.c` group V
vectors, which cover in-range, the 16-bit negative window, and both
out-of-range edges through a real-mode IVT `#BR` delivery.

Two host-harness traps worth remembering, both hit while building this:

1. A *successful* load of the null selector leaves `ES`/`DS` unusable, and the
   next libc string move faults for a reason that has nothing to do with the
   instruction under test. The reference restores the segment register inside
   the asm before touching memory again.
2. Consequently the segment register printed *after* a fault is stale in the
   host build; both sides print `----` for faulting cases instead of inventing
   a value.
