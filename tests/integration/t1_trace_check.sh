#!/bin/sh
# TOOLING T1 gate: the disassembly trace and its CPU-vs-disassembler
# self-check on real workloads.
#
# Every lane runs the same command twice -- plain, and with
# --trace-file=/dev/null -- and requires:
#   1. [trace] mismatches=0 undecodable=0 unreadable=0 (every executed
#      instruction decodes, and every predictable successor RIP -- fall-
#      through, jcc/loop either way, direct jmp/call target -- is the one
#      the CPU actually produced);
#   2. [trace] lines == [stats] executed (one line per executed attempt);
#   3. stdout byte-identical between the two runs: register dump, the
#      --stats block (P2 TLB and P3 predecode counters included) and the
#      last log lines -- the trace neither perturbs the guest nor changes
#      the emulator's internal path, and never touches the log ring (D0/D2).
# Lanes: the sample firmware on all five platforms (full boot), the K1
# fixture kernel, and -- when KERNEL/INITRD point at the frozen AuraLite-OS
# build -- 40M guest instructions UP and 40M with --smp=2 --cpus=2.
set -eu
root=$(CDPATH= cd -- "$(dirname "$0")/../.." && pwd)
cd "$root"
X86EMU=${X86EMU:-./x86emu}
WORK=${1:-$(mktemp -d)}
PLATFORMS=${T1_PLATFORMS:-"sandybridge ivybridge haswell broadwell baytrail"}
mkdir -p "$WORK"
fail=0
lanes=0

lane() {  # lane <name> <args...>
    name=$1; shift
    "$X86EMU" "$@" --stats >"$WORK/$name.plain.out" 2>"$WORK/$name.plain.err" || true
    "$X86EMU" "$@" --stats --trace-file=/dev/null >"$WORK/$name.trace.out" 2>"$WORK/$name.trace.err" || true
    sum=$(grep '^\[trace\] ' "$WORK/$name.trace.err" | tail -1 || true)
    lines=$(printf '%s\n' "$sum" | sed -n 's/.*lines=\([0-9]*\).*/\1/p')
    exe=$(sed -n 's/^\[stats\] executed=\([0-9]*\).*/\1/p' "$WORK/$name.trace.out")
    ok=1
    case "$sum" in *"mismatches=0 undecodable=0 unreadable=0"*) ;; *) ok=0 ;; esac
    [ -n "$lines" ] && [ "$lines" = "$exe" ] || ok=0
    cmp -s "$WORK/$name.plain.out" "$WORK/$name.trace.out" || ok=0
    lanes=$((lanes + 1))
    if [ "$ok" = 1 ]; then
        printf 'ok   %-14s %s\n' "$name" "$(printf '%s' "$sum" | sed 's/ file=.*//; s/^\[trace\] //')"
    else
        printf 'FAIL %-14s %s (executed=%s)\n' "$name" "$sum" "$exe"
        cmp "$WORK/$name.plain.out" "$WORK/$name.trace.out" || true
        grep '^!' "$WORK/$name.trace.err" | head -3 || true
        fail=1
    fi
}

for p in $PLATFORMS; do
    lane "fw-$p" --platform="$p" --max-instr=20000000 --no-fb
done
# the traced firmware run keeps every test-boot receipt in stdout (D0)
for r in '\[usb-msc\] READ(10) LBA=0 blocks=1' '\[guest\] conventional entry reached at 0x00100000' \
         '\[serial\] GUEST_OK'; do
    if ! grep -q "$r" "$WORK/fw-haswell.trace.out" 2>/dev/null; then
        echo "FAIL receipt missing from traced stdout: $r"; fail=1
    fi
done
[ -f tests/kload_fixture.elf ] && lane k1-fixture --kernel=tests/kload_fixture.elf --max-instr=2000000

KERNEL=${KERNEL:-/home/user/AuraLite-OS/build/kernel.elf}
INITRD=${INITRD:-/home/user/AuraLite-OS/build/initrd.tar}
if [ -f "$KERNEL" ] && [ -f "$INITRD" ]; then
    lane guest-up  --kernel="$KERNEL" --initrd="$INITRD" --max-instr=40000000 --no-fb
    lane guest-smp --kernel="$KERNEL" --initrd="$INITRD" --cpus=2 --smp=2 --max-instr=40000000 --no-fb
else
    echo "SKIP guest lanes: KERNEL/INITRD not found (frozen AuraLite-OS build)"
fi

if [ "$fail" = 0 ]; then
    echo "PASS test-trace ($lanes lanes: 0 mismatches, 0 undecodable, stdout identical with/without trace)"
    exit 0
fi
echo "FAIL test-trace"
exit 1
