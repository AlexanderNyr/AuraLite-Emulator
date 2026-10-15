#!/bin/sh
# TOOLING T1's ONE negative control: a scratch disasm.c whose group-83
# immediate is decoded as imm16/32 ("Ev,Iz") instead of a sign-extended
# imm8 ("Ev,Is") -- a pure length defect. All three T1 gates must turn red
# for that reason: the frozen unit vector `83c0ff`, the ndisasm
# differential (length disagreement on every 83 encoding), and the trace
# self-check (the firmware executes `cmp edx,byte ...` 10,010 times per
# boot; each one's successor RIP now disagrees with the decode).
set -eu
root=$(CDPATH= cd -- "$(dirname "$0")/../.." && pwd)
cd "$root"
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT HUP INT TERM
mkdir -p "$tmp/src" "$tmp/tools"
cp src/*.c src/*.h "$tmp/src/"
cp tools/x86dis.c "$tmp/tools/"
python3 - "$tmp/src/disasm.c" <<'PY'
from pathlib import Path
import sys
p = Path(sys.argv[1]); s = p.read_text()
a = 'static const ent_t g83[8] = ALU8("Ev,Is");'
b = 'static const ent_t g83[8] = ALU8("Ev,Iz");   /* T1 NC: wrong imm size */'
assert s.count(a) == 1, 'one group-83 anchor required'
p.write_text(s.replace(a, b, 1))
PY
CC=${CC:-cc}
F="-std=c11 -O2 -Wall -Wextra -Werror"
$CC $F -I"$tmp/src" -Itests tests/test_disasm.c "$tmp/src/disasm.c" "$tmp/src/dbg.c" \
    "$tmp/src/cpu.c" "$tmp/src/mem.c" "$tmp/src/io.c" "$tmp/src/platform.c" "$tmp/src/pic.c" \
    "$tmp/src/pit.c" "$tmp/src/rtc.c" "$tmp/src/kbc.c" "$tmp/src/chipset.c" "$tmp/src/lapic.c" \
    "$tmp/src/ioapic.c" -o "$tmp/test-disasm" -lm
$CC $F -I"$tmp/src" "$tmp/tools/x86dis.c" "$tmp/src/disasm.c" -o "$tmp/x86dis"
$CC $F -I"$tmp/src" "$tmp"/src/*.c -o "$tmp/x86emu" -lm
set +e
"$tmp/test-disasm" >"$tmp/unit.out" 2>&1; rc_unit=$?
rc_diff=skip
if command -v ndisasm >/dev/null 2>&1; then
    python3 tests/disasm_diff.py --x86dis "$tmp/x86dis" --work "$tmp/diff" --show 0 >"$tmp/diff.out" 2>&1; rc_diff=$?
fi
"$tmp/x86emu" --platform=haswell --max-instr=20000000 --no-fb --trace-file=/dev/null >/dev/null 2>"$tmp/trace.err"
set -e
mism=$(sed -n 's/^\[trace\] .*mismatches=\([0-9]*\).*/\1/p' "$tmp/trace.err")
broken=0
if [ "$rc_unit" -eq 0 ] || ! grep -q 'FAIL A b32 83c0ff' "$tmp/unit.out"; then
    echo "NC BROKEN: unit vectors did not fail on 83c0ff (rc=$rc_unit)" >&2; broken=1
fi
if [ "$rc_diff" != skip ] && { [ "$rc_diff" -eq 0 ] || ! grep -q 'FAIL disasm-diff' "$tmp/diff.out"; }; then
    echo "NC BROKEN: ndisasm differential stayed green (rc=$rc_diff)" >&2; broken=1
fi
if [ -z "$mism" ] || [ "$mism" -eq 0 ]; then
    echo "NC BROKEN: trace self-check saw no successor mismatch" >&2; broken=1
fi
[ "$broken" = 0 ] || exit 2
printf 'T1 NC: unit rc=%s (FAIL A b32 83c0ff); disasm-diff rc=%s (%s); trace mismatches=%s\n' \
    "$rc_unit" "$rc_diff" "$(grep -o 'diff=[0-9]*' "$tmp/diff.out" 2>/dev/null | head -1)" "$mism"
echo 'T1 NC: FAILED AS DESIGNED (a one-entry length defect is caught by all three T1 gates)'
exit 1
