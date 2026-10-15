#!/bin/sh
# TOOLING T2's ONE negative control: a scratch dbg.c whose resume path no
# longer arms the skip-once rule. A stop taken between instructions (run
# start, watchpoint, pause) at an address that carries a breakpoint then
# re-triggers that breakpoint before a single instruction runs: "break
# here; continue" produces a spurious zero-progress stop. The unit vector
# with a breakpoint on every address must turn red (37 stops instead of
# 36), and the real lane script (tests/integration/t2_debug_lanes.sh) run
# on the scratch binary must fail lane A: its one "continue" is eaten by a
# second stop at vtime 0, so the guest entry is never reached.
set -eu
root=$(CDPATH= cd -- "$(dirname "$0")/../.." && pwd)
cd "$root"
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT HUP INT TERM
mkdir -p "$tmp/src"
cp src/*.c src/*.h "$tmp/src/"
python3 - "$tmp/src/dbg.c" <<'PY'
from pathlib import Path
import sys
p = Path(sys.argv[1]); s = p.read_text()
a = '    s->skip_once = 1;\n    s->skip_vcpu = st->vcpu;'
b = '    s->skip_once = 0;   /* T2 NC: resume re-triggers the breakpoint */\n    s->skip_vcpu = st->vcpu;'
assert s.count(a) == 1, 'one skip-once anchor required'
p.write_text(s.replace(a, b, 1))
PY
CC=${CC:-cc}
F="-std=c11 -O2 -Wall -Wextra -Werror"
S="$tmp/src"
$CC $F -I"$S" -Itests tests/test_dbg.c "$S/dbg.c" "$S/disasm.c" "$S/cpu.c" "$S/mem.c" "$S/io.c" \
    "$S/platform.c" "$S/pic.c" "$S/pit.c" "$S/rtc.c" "$S/kbc.c" "$S/chipset.c" "$S/lapic.c" "$S/ioapic.c" \
    -o "$tmp/test-dbg" -lm
$CC $F -I"$S" "$S"/*.c -o "$tmp/x86emu" -lm
set +e
"$tmp/test-dbg" >"$tmp/unit.out" 2>&1; rc_unit=$?
X86EMU="$tmp/x86emu" sh tests/integration/t2_debug_lanes.sh "$tmp/lanes" >"$tmp/lanes.out" 2>&1; rc_lane=$?
set -e
broken=0
if [ "$rc_unit" -eq 0 ] || ! grep -q 'FAIL breakpoint stops 37, want 36' "$tmp/unit.out"; then
    echo "NC BROKEN: unit vectors stayed green or failed elsewhere (rc=$rc_unit)" >&2
    cat "$tmp/unit.out" >&2; broken=1
fi
spurious=$(grep -c 'stopped: breakpoint 1 vcpu=0 vtime=0 steps=0' "$tmp/lanes/a.err" 2>/dev/null || true)
if [ "$rc_lane" -eq 0 ] || ! grep -q 'FAIL A continue from the start stop progresses to the guest entry' "$tmp/lanes.out" \
   || [ "$spurious" -lt 1 ]; then
    echo "NC BROKEN: lane A did not fail for the skip-once reason (rc=$rc_lane)" >&2
    cat "$tmp/lanes.out" >&2; broken=1
fi
[ "$broken" = 0 ] || exit 2
printf 'T2 NC: unit rc=%s (breakpoint stops 37, want 36); lanes rc=%s (%s spurious stop at vtime 0; %s)\n' \
    "$rc_unit" "$rc_lane" "$spurious" "$(grep -c '^FAIL' "$tmp/lanes.out") failed checks"
echo 'T2 NC: FAILED AS DESIGNED (resume must skip the breakpoint it stands on)'
exit 1
