#!/bin/sh
# TOOLING T4 gate: split-run identity.
#
# For every lane, a straight run to N2 must equal (run to N1 + save) then
# (fresh process + load + run to N2), byte for byte, in stdout (register
# dump, --stats incl. P2/P3 counters, last log lines), the PPM, the
# --dump-text console and the full --log file. Plus: two processes saving
# the same state write the same bytes; save -> load -> save is a fixed
# point; REPL savevm/loadvm replays to the plain run's result; mismatched
# shape, foreign files and host-state options are refused with a reason and
# exit code 1.
set -eu
root=$(CDPATH= cd -- "$(dirname "$0")/../.." && pwd)
cd "$root"
X86EMU=${X86EMU:-./x86emu}
WORK=${1:-$(mktemp -d)}
mkdir -p "$WORK"
fail=0
lanes=0

split() {  # split <name> <N1> <N2> <args...>
    name=$1 n1=$2 n2=$3; shift 3
    a="$WORK/$name.a" b="$WORK/$name.b"
    "$X86EMU" "$@" --stats --max-instr="$n2" --dump-fb="$a.ppm" --dump-text="$a.txt" --log="$a.log" >"$a.out" 2>/dev/null || true
    "$X86EMU" "$@" --stats --max-instr="$n1" --snapshot-save="$WORK/$name.snap" >/dev/null 2>"$WORK/$name.save.err" || true
    "$X86EMU" "$@" --stats --max-instr="$n2" --snapshot-load="$WORK/$name.snap" \
        --dump-fb="$b.ppm" --dump-text="$b.txt" --log="$b.log" >"$b.out" 2>"$WORK/$name.load.err" || true
    sed -i "s#$a\.#X.#g" "$a.out"; sed -i "s#$b\.#X.#g" "$b.out"
    lanes=$((lanes + 1))
    bad=""
    for f in out ppm txt log; do cmp -s "$a.$f" "$b.$f" || bad="$bad $f"; done
    grep -q '^\[snapshot\] loaded' "$WORK/$name.load.err" || bad="$bad load"
    size=$(wc -c <"$WORK/$name.snap" 2>/dev/null || echo 0)
    if [ -z "$bad" ]; then printf 'ok   %-22s split %s/%s, snapshot %s bytes\n' "$name" "$n1" "$n2" "$size"
    else printf 'FAIL %-22s split %s/%s differs in:%s\n' "$name" "$n1" "$n2" "$bad"; fail=1; fi
}

for p in sandybridge ivybridge haswell broadwell baytrail; do
    split "fw-$p-mid" 2000000 20000000 --platform="$p"
done
split fw-haswell-early 1000 20000000 --platform=haswell
split fw-haswell-entry 35254 20000000 --platform=haswell
split fw-haswell-late 4837000 20000000 --platform=haswell
split fw-nofb 2000000 20000000 --platform=haswell --no-fb
split fw-vbe 2000000 20000000 --platform=haswell --vbe=800x600x32
split fw-smp2 2000001 20000000 --platform=haswell --smp=2
[ -f tests/kload_fixture.elf ] && split k1-fixture 20 2000000 --kernel=tests/kload_fixture.elf

KERNEL=${KERNEL:-/home/user/AuraLite-OS/build/kernel.elf}
INITRD=${INITRD:-/home/user/AuraLite-OS/build/initrd.tar}
if [ -f "$KERNEL" ] && [ -f "$INITRD" ]; then
    split guest-up 20000000 40000000 --kernel="$KERNEL" --initrd="$INITRD" --no-fb
    split guest-smp 20000001 40000000 --kernel="$KERNEL" --initrd="$INITRD" --cpus=2 --smp=2
else
    echo "SKIP guest lanes: KERNEL/INITRD not found (frozen AuraLite-OS build)"
fi

# deterministic files: the same machine state saved by two processes is the
# same bytes (host addresses never reach the file)
"$X86EMU" --platform=haswell --stats --max-instr=2000000 --snapshot-save="$WORK/again.snap" >/dev/null 2>&1 || true
if cmp -s "$WORK/fw-haswell-mid.snap" "$WORK/again.snap"; then echo "ok   determinism: two processes, same state -> identical snapshot files"
else echo "FAIL determinism: snapshot files differ"; fail=1; fi
# save -> load -> save is a fixed point (the second run executes nothing)
"$X86EMU" --platform=haswell --max-instr=2000000 --snapshot-load="$WORK/fw-haswell-mid.snap" \
    --snapshot-save="$WORK/resave.snap" >/dev/null 2>&1 || true
if cmp -s "$WORK/fw-haswell-mid.snap" "$WORK/resave.snap"; then echo "ok   round trip: save -> load -> save byte-identical"
else echo "FAIL round trip differs"; fail=1; fi

# refusals: exit code 1 and a reason
refuse() {  # refuse <label> <pattern> <args...>
    label=$1 pat=$2; shift 2
    set +e
    "$X86EMU" "$@" >/dev/null 2>"$WORK/refuse.err"
    rc=$?
    set -e
    if [ "$rc" -eq 1 ] && grep -q -- "$pat" "$WORK/refuse.err"; then echo "ok   refuse: $label"
    else echo "FAIL refuse: $label (rc=$rc)"; cat "$WORK/refuse.err"; fail=1; fi
}
S="$WORK/fw-haswell-mid.snap"
refuse "other platform" "platform Haswell (LGA1150, model 0x3C) in the snapshot, Broadwell" --platform=broadwell --snapshot-load="$S"
refuse "other vCPU count" "n_vcpus" --platform=haswell --smp=2 --snapshot-load="$S"
printf 'NOTASNAP' >"$WORK/bogus.snap"
refuse "foreign file" "bad magic" --platform=haswell --snapshot-load="$WORK/bogus.snap"
head -c 100000 "$S" >"$WORK/trunc.snap"
refuse "truncated file" "truncated" --platform=haswell --snapshot-load="$WORK/trunc.snap"
refuse "--tap host state" "tap" --platform=haswell --tap=127.0.0.1:9 --snapshot-load="$S"
refuse "--sata-writethrough" "writethrough" --platform=haswell --sata-writethrough --snapshot-load="$S"

# REPL savevm/loadvm: save at the guest entry, run on, load, run to the end
# -> the result equals the plain run (the replay is deterministic)
cat >"$WORK/repl.dbg" <<EOF
break 0x100000
continue
savevm $WORK/repl.snap
stepi 5000
p \$vtime
loadvm $WORK/repl.snap
p \$vtime
delete
continue
EOF
"$X86EMU" --platform=haswell --max-instr=20000000 --stats >"$WORK/repl.plain" 2>/dev/null
"$X86EMU" --platform=haswell --max-instr=20000000 --stats --debug-script="$WORK/repl.dbg" >"$WORK/repl.out" 2>"$WORK/repl.err"
if grep -q 'savevm deferred' "$WORK/repl.err" && grep -q "savevm $WORK/repl.snap done at vtime 35255" "$WORK/repl.err" \
   && grep -q "loadvm $WORK/repl.snap done" "$WORK/repl.err" && grep -q '^0x89b7 (35255)' "$WORK/repl.err" \
   && cmp -s "$WORK/repl.plain" "$WORK/repl.out"; then
    echo "ok   REPL savevm (deferred to vtime 35255) / stepi 5000 / loadvm -> back at 35255; run == plain run"
else
    echo "FAIL REPL savevm/loadvm"; grep -E 'savevm|loadvm|^0x' "$WORK/repl.err" || true; fail=1
fi

if [ "$fail" = 0 ]; then echo "PASS test-snapshot ($lanes split lanes byte-identical)"; exit 0; fi
echo "FAIL test-snapshot"; exit 1
