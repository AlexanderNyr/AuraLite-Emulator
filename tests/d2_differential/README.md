# d2_differential -- host reference for the decimal-adjust opcodes

`DAA/DAS/AAA/AAS/AAM/AAD` and legacy-mode `ARPL` are `#UD` in 64-bit mode, so
the host measures them in **i386 compatibility mode**: a static `-m32` Linux
binary executes them natively. Both halves print the same one-line format, so
the whole verification is a `diff`.

```sh
# host reference (needs gcc-multilib)
gcc -m32 -O1 -no-pie -o host32 host32.c ref32.S && ./host32 > host.out

# the emulated core, from the repository root
gcc -std=c11 -O1 -Isrc -Itests tests/d2_differential/emu.c \
    src/cpu.c src/mem.c src/io.c src/platform.c src/pic.c src/pit.c \
    src/pm.c src/rtc.c src/kbc.c src/chipset.c src/lapic.c src/ioapic.c \
    -o emu -lm && ./emu > emu.out

diff host.out emu.out   # empty = 8208/8208 cases identical
```

Coverage: all eight opcode forms x 256 values of `AL` x `CF` x `AF`, plus every
`dst`/`src` RPL pair of `ARPL` (16 cases).

`probe32.c` is the tool that *derived* the flag rules (it sweeps `AH` for
`AAD` and prints where `CF`/`AF` turn on); it is kept because the SDM
pseudocode disagrees with silicon in three places:

| Instruction | SDM says | Hardware does |
|---|---|---|
| `DAA`/`DAS` | second stage compares the **adjusted** `AL` with `9FH` | compares the **original** `AL` with `99H` |
| `DAS` | `CF` comes from the second stage only | a first-stage borrow also sets `CF` (`DAS 00`/`AF=1 -> 0xFA`, `CF=1`) |
| `AAD` | `CF`/`AF` undefined | `CF`/`AF` are the carry-outs of an **8-bit** add of `(AH*base)&0xFF` and `AL` |
| `AAA`/`AAS` | `AH := AH +- 1` | the `+-6` carries into `AH` too (`AL=FA` bumps `AH` twice); `AH`'s high nibble survives |
