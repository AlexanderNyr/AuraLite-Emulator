#!/bin/sh
# TOOLING T3 gate: the gdbstub against a real gdb (SKIPs without gdb, which
# is not a project dependency) plus the pure-socket protocol client.
#
# Lane P  tests/test_gdbstub.py: 57 protocol checks (always runs; the
#         x87 lane D joined in UPKEEP U3).
# Lane G  gdb -batch on the firmware: target remote, reset registers,
#         break *0x100000, continue, x/4i $pc, stepi, monitor info vcpu,
#         a hardware write watchpoint on framebuffer pixel 0, kill.
# Lane F  gdb -batch on an x87 ROM payload (UPKEEP U3): step to a known
#         FPU stack, `info float` (TOP/tags/FIP interpreted by real gdb),
#         set $st0/$st3, step the four FSTP m80 stores, read guest
#         memory back -- the real-client half of the U3 gate.
# Lane K  gdb -batch on the frozen guest (KERNEL/INITRD set): gdb loads the
#         kernel ELF, `break kmain`, continue, bt, info threads with
#         --smp=2 --cpus=2 (two vCPUs = two threads), detach -> the run
#         completes and stdout equals the plain run.
set -eu
root=$(CDPATH= cd -- "$(dirname "$0")/../.." && pwd)
cd "$root"
X86EMU=${X86EMU:-./x86emu}
WORK=${1:-$(mktemp -d)}
mkdir -p "$WORK"
fail=0
need() {  # need <file> <regex> <label>
    if grep -E -q -- "$2" "$1"; then echo "ok   $3"; else echo "FAIL $3 (missing: $2)"; fail=1; fi
}
port() { python3 -c 'import socket; s=socket.socket(); s.bind(("127.0.0.1",0)); print(s.getsockname()[1])'; }

echo "== lane P: protocol client"
if X86EMU="$X86EMU" python3 tests/test_gdbstub.py >"$WORK/p.out" 2>&1; then
    echo "ok   P $(grep -c '^ok' "$WORK/p.out") protocol checks"
else
    cat "$WORK/p.out"; echo "FAIL P protocol client"; fail=1
fi

if ! command -v gdb >/dev/null 2>&1; then
    echo "SKIP lanes G/K: gdb not installed"
else
    echo "== lane G: gdb -batch on the firmware"
    P=$(port)
    "$X86EMU" --platform=haswell --max-instr=20000000 --gdb=127.0.0.1:"$P" >"$WORK/g.emu.out" 2>"$WORK/g.emu.err" &
    EMU=$!
    cat >"$WORK/g.gdb" <<EOF
set pagination off
set confirm off
target remote 127.0.0.1:$P
info registers rip cs
break *0x100000
continue
x/4i \$pc
stepi
p/x \$pc
monitor info vcpu
watch *(int*)0xd0000000
continue
p/x *(int*)0xd0000000
info registers cr0 cr3 efer
kill
EOF
    timeout 120 gdb -batch -nx -x "$WORK/g.gdb" >"$WORK/g.out" 2>&1 || true
    wait "$EMU" || true
    need "$WORK/g.out" 'rip +0xfff0' "G reset rip in gdb"
    need "$WORK/g.out" 'cs +0xf000' "G reset cs in gdb"
    need "$WORK/g.out" 'Breakpoint 1, 0x0+100000' "G breakpoint at the guest entry"
    need "$WORK/g.out" '=> 0x100000:[[:space:]]+cli' "G gdb disassembles at \$pc (libopcodes, 64-bit)"
    need "$WORK/g.out" '\$1 = 0x100001' "G stepi"
    need "$WORK/g.out" 'vcpu0 run +long64' "G monitor command"
    need "$WORK/g.out" 'Hardware watchpoint 2: \*\(int\*\)0xd0000000' "G hardware watchpoint (Z2)"
    need "$WORK/g.out" 'New value = 2097152' "G watchpoint reports the 0x00200000 marker"
    need "$WORK/g.out" 'cr0 +0xe0000011' "G system registers from target.xml"

    echo "== lane F: gdb -batch on an x87 ROM payload (UPKEEP U3)"
    # fninit; fld1; fldpi; fldl2e; fldz; four fstp m80 to 0x500..; jmp $.
    # Driven by stepi, not breakpoints: in real mode the stub's
    # breakpoints are LINEAR (CS.base+RIP) while gdb reconciles a hit
    # against raw $pc, so a ROM breakpoint would be auto-continued as a
    # spurious trap -- measured with gdb 16.3 (wire trace in the U3
    # receipts); in the flat modes lane G above they coincide.
    python3 - "$WORK/f.rom" <<'PYEOF'
import sys
img = bytearray(b"\xf4" * 131072)
code = bytes([0xDB,0xE3, 0xD9,0xE8, 0xD9,0xEB, 0xD9,0xEA, 0xD9,0xEE,
              0xDB,0x3E,0x00,0x05, 0xDB,0x3E,0x10,0x05,
              0xDB,0x3E,0x20,0x05, 0xDB,0x3E,0x30,0x05, 0xEB,0xFE])
img[0x1FF00:0x1FF00+len(code)] = code
img[0x1FFF0:0x1FFF3] = b"\xe9\x0d\xff"
open(sys.argv[1], "wb").write(img)
PYEOF
    P=$(port)
    "$X86EMU" --rom="$WORK/f.rom" --max-instr=100000000000 --gdb=127.0.0.1:"$P" >"$WORK/f.emu.out" 2>"$WORK/f.emu.err" &
    EMU=$!
    cat >"$WORK/f.gdb" <<EOF
set pagination off
set confirm off
target remote 127.0.0.1:$P
stepi
stepi
stepi
stepi
stepi
stepi
info float
p \$st0
p \$st3
set \$st0 = -2.5
set \$st3 = 0.5
p \$st0
p \$st3
stepi
stepi
stepi
stepi
x/10xb 0x500
x/10xb 0x530
info float
kill
EOF
    timeout 120 gdb -batch -nx -x "$WORK/f.gdb" >"$WORK/f.out" 2>&1 || true
    wait "$EMU" || true
    need "$WORK/f.out" 'TOP: 4' "F info float: TOP=4 after four pushes"
    need "$WORK/f.out" 'R4: Zero' "F info float: physical R4 is the zero on top"
    need "$WORK/f.out" '3\.1415926535897932' "F info float: pi value in R6"
    need "$WORK/f.out" 'Tag Word: +0x01ff' "F info float: full physical tag word"
    need "$WORK/f.out" 'Instruction Pointer: 0xf000:0x0000ff08' "F FIP names the FLDZ"
    need "$WORK/f.out" '\$3 = -2\.5' "F gdb writes \$st0"
    need "$WORK/f.out" '0x508:.*0xc0' "F FSTP consumed the written -2.5"
    need "$WORK/f.out" '0x538:.*0xfe.*0x3f' "F FSTP consumed the written logical st3 0.5"
    need "$WORK/f.out" 'Tag Word: +0xffff' "F stack empty after the pops"

    KERNEL=${KERNEL:-/home/user/AuraLite-OS/build/kernel.elf}
    INITRD=${INITRD:-/home/user/AuraLite-OS/build/initrd.tar}
    if [ -f "$KERNEL" ] && [ -f "$INITRD" ]; then
        echo "== lane K: gdb -batch on the frozen guest (symbols, two vCPUs)"
        A="--kernel=$KERNEL --initrd=$INITRD --cpus=2 --smp=2 --max-instr=40000000 --stats"
        # shellcheck disable=SC2086
        "$X86EMU" $A >"$WORK/k.plain" 2>/dev/null
        P=$(port)
        # shellcheck disable=SC2086
        "$X86EMU" $A --gdb=127.0.0.1:"$P" >"$WORK/k.emu.out" 2>"$WORK/k.emu.err" &
        EMU=$!
        cat >"$WORK/k.gdb" <<EOF
set pagination off
set confirm off
file $KERNEL
target remote 127.0.0.1:$P
break kmain
continue
bt
info threads
x/2i \$pc
delete
detach
EOF
        timeout 300 gdb -batch -nx -x "$WORK/k.gdb" >"$WORK/k.out" 2>&1 || true
        wait "$EMU" || true
        need "$WORK/k.out" 'Breakpoint 1, (0x[0-9a-f]+ in )?kmain' "K break kmain with gdb's own ELF symbols"
        need "$WORK/k.out" '#0 .*kmain' "K backtrace"
        need "$WORK/k.out" 'Thread 2' "K two vCPUs listed as threads"
        need "$WORK/k.out" '<kmain(\+[0-9]+)?>:' "K x/2i symbolized by gdb"
        if cmp -s "$WORK/k.plain" "$WORK/k.emu.out"; then echo "ok   K detached run completes; stdout identical to the plain run"
        else echo "FAIL K stdout differs from the plain run"; fail=1; fi
    else
        echo "SKIP lane K: KERNEL/INITRD not found (frozen AuraLite-OS build)"
    fi
fi

if [ "$fail" = 0 ]; then echo "PASS test-gdb"; exit 0; fi
echo "FAIL test-gdb"; exit 1
