#!/bin/sh
# TOOLING T3 gate: the gdbstub against a real gdb (SKIPs without gdb, which
# is not a project dependency) plus the pure-socket protocol client.
#
# Lane P  tests/test_gdbstub.py: 30 protocol checks (always runs).
# Lane G  gdb -batch on the firmware: target remote, reset registers,
#         break *0x100000, continue, x/4i $pc, stepi, monitor info vcpu,
#         a hardware write watchpoint on framebuffer pixel 0, kill.
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
