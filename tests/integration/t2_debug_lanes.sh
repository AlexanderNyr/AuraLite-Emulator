#!/bin/sh
# TOOLING T2 gate: the REPL on real workloads.
#
# Lane A  firmware transcript: start stop at the reset vector, "break here;
#         continue" from that between-instruction stop (must make progress),
#         breakpoint at the guest entry 0x100000 (vtime 35254), stepi 3,
#         page walk, delete, run to the end (halted at 4837677).
# Lane B  write watchpoint on framebuffer pixel 0: stops after the store
#         with old=0x0 new=0x200000 (the test kernel's marker) at 35283.
# Lane C  non-perturbation, five platforms: break + continue + stepi 1000 +
#         delete + continue; stdout incl. --stats byte-identical to plain.
# Lane D  interactive stdin (pipe): prompts, quit, exit code 0.
# Lane E  frozen guest (KERNEL/INITRD set; SKIP otherwise): symbols from the
#         kernel ELF, `break kmain`, backtrace, --smp=2 --cpus=2, stdout
#         identical to the plain run.
set -eu
root=$(CDPATH= cd -- "$(dirname "$0")/../.." && pwd)
cd "$root"
X86EMU=${X86EMU:-./x86emu}
WORK=${1:-$(mktemp -d)}
mkdir -p "$WORK"
fail=0
need() {  # need <file> <fixed string> <label>
    if grep -F -q -- "$2" "$1"; then echo "ok   $3"; else echo "FAIL $3 (missing: $2)"; fail=1; fi
}
same() {  # same <a> <b> <label>
    if cmp -s "$1" "$2"; then echo "ok   $3"; else echo "FAIL $3"; cmp "$1" "$2" || true; fail=1; fi
}

FW="--platform=haswell --max-instr=20000000"
# shellcheck disable=SC2086
"$X86EMU" $FW --stats >"$WORK/plain.out" 2>/dev/null

cat >"$WORK/a.dbg" <<'EOF'
info regs
break $lin
break 0x100000
continue
x/3i $rip
stepi 3
xlate $rip
delete
continue
EOF
# shellcheck disable=SC2086
"$X86EMU" $FW --stats --debug-script="$WORK/a.dbg" >"$WORK/a.out" 2>"$WORK/a.err"
need "$WORK/a.err" "[dbg] stopped: start vcpu=0 vtime=0 steps=0" "A start stop at power-on"
need "$WORK/a.err" "=> f000:000000000000fff0  jmp 0xe000:0x0" "A reset vector disassembly"
need "$WORK/a.err" "rip 0x000000000000fff0" "A reset RIP"
need "$WORK/a.err" "breakpoint 1 at 0x00000000fffffff0" "A break at the reset vector (linear)"
need "$WORK/a.err" "[dbg] stopped: breakpoint 2 vcpu=0 vtime=35254 steps=35254" "A continue from the start stop progresses to the guest entry"
need "$WORK/a.err" "=> 0000000000100000  fa                   cli" "A x/3i at the entry"
need "$WORK/a.err" "[dbg] stopped: step vcpu=0 vtime=35257 steps=35257" "A stepi 3 = exactly 3 instructions"
need "$WORK/a.err" "linear 0000000000100007 -> physical 0000000000100007" "A xlate (side-effect-free walk)"
need "$WORK/a.err" "[dbg] stopped: run ended (halted) vcpu=0 vtime=4837677" "A run ends where the plain run ends"
same "$WORK/plain.out" "$WORK/a.out" "A stdout incl. --stats identical to the plain run"

cat >"$WORK/b.dbg" <<'EOF'
break 0x100000
continue
watch w 0xd0000000 4
continue
delete
continue
EOF
# shellcheck disable=SC2086
"$X86EMU" $FW --stats --debug-script="$WORK/b.dbg" >"$WORK/b.out" 2>"$WORK/b.err"
need "$WORK/b.err" "[dbg] stopped: watchpoint 2 (w) addr=0xd0000000 old=0x0 new=0x200000 vcpu=0 vtime=35283" "B watchpoint after the marker store"
need "$WORK/b.err" "=> 0008:000000000010003d  add rdi,byte +0x4" "B stop is after the storing instruction"
same "$WORK/plain.out" "$WORK/b.out" "B stdout identical to the plain run"

cat >"$WORK/c.dbg" <<'EOF'
break 0x100000
continue
stepi 1000
delete
continue
EOF
for p in sandybridge ivybridge haswell broadwell baytrail; do
    "$X86EMU" --platform="$p" --max-instr=20000000 --stats >"$WORK/c-$p.plain" 2>/dev/null
    "$X86EMU" --platform="$p" --max-instr=20000000 --stats --debug-script="$WORK/c.dbg" >"$WORK/c-$p.out" 2>"$WORK/c-$p.err"
    same "$WORK/c-$p.plain" "$WORK/c-$p.out" "C $p: debugged run == plain run (stdout, --stats)"
done

set +e
# shellcheck disable=SC2086
printf 'break 0x100000\ncontinue\nregs\nquit\n' | "$X86EMU" $FW --debug >"$WORK/d.out" 2>"$WORK/d.err"
rc=$?
set -e
need "$WORK/d.err" "(x86dbg) " "D interactive prompt"
need "$WORK/d.err" "[dbg] stopped: breakpoint 1 vcpu=0 vtime=35254" "D breakpoint over a pipe"
if [ "$rc" -eq 0 ] && grep -q 'CPU state' "$WORK/d.out"; then echo "ok   D quit exits 0 with the final dump"; else echo "FAIL D quit rc=$rc"; fail=1; fi

KERNEL=${KERNEL:-/home/user/AuraLite-OS/build/kernel.elf}
INITRD=${INITRD:-/home/user/AuraLite-OS/build/initrd.tar}
if [ -f "$KERNEL" ] && [ -f "$INITRD" ]; then
    cat >"$WORK/e.dbg" <<'EOF'
break kmain
continue
bt
x/2i $rip
info vcpu
delete
continue
EOF
    G="--kernel=$KERNEL --initrd=$INITRD --cpus=2 --smp=2 --max-instr=40000000 --stats"
    # shellcheck disable=SC2086
    "$X86EMU" $G >"$WORK/e.plain" 2>/dev/null
    # shellcheck disable=SC2086
    "$X86EMU" $G --debug-script="$WORK/e.dbg" >"$WORK/e.out" 2>"$WORK/e.err"
    need "$WORK/e.err" "[dbg] stopped: breakpoint 1 vcpu=0 vtime=18 steps=18" "E break kmain (ELF symbols)"
    need "$WORK/e.err" "<kmain+0x0>  push rbp" "E symbolized stop"
    need "$WORK/e.err" "#0  " "E backtrace"
    need "$WORK/e.err" "  vcpu1 wait-sipi" "E vCPU table"
    same "$WORK/e.plain" "$WORK/e.out" "E guest --smp=2 stdout identical to the plain run"
else
    echo "SKIP lane E: KERNEL/INITRD not found (frozen AuraLite-OS build)"
fi

if [ "$fail" = 0 ]; then echo "PASS test-debug"; exit 0; fi
echo "FAIL test-debug"; exit 1
