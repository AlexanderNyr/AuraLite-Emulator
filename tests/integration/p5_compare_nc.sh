#!/bin/sh
# P5's ONE negative control: remove ONLY the cross-build guest-work/clock
# equality check from a scratch comparator. The same vector must go red.
set -eu
root=$(CDPATH= cd -- "$(dirname "$0")/../.." && pwd)
cd "$root"
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT HUP INT TERM
cp tests/perf_compare.py "$tmp/perf_compare.py"
python3 - "$tmp/perf_compare.py" <<'PY'
from pathlib import Path
import sys
p = Path(sys.argv[1]); s = p.read_text()
a = '    if any(before[k] != after[k] for k in WORK_KEYS):'
b = '    if False:  # P5 NC: accept changed guest clock/work as speedup'
assert s.count(a) == 1, 'one work-equality guard must exist'
p.write_text(s.replace(a, b, 1))
PY
set +e
PERF_COMPARE_MODULE="$tmp/perf_compare.py" python3 tests/test_perf_compare.py >"$tmp/out" 2>"$tmp/err"
rc=$?
set -e
if [ "$rc" -eq 0 ]; then
    echo 'P5 NC BROKEN: mismatched virtual clocks were accepted as equal' >&2
    exit 2
fi
if ! grep -F -q -e 'AssertionError: ValueError not raised' "$tmp/err" || \
   ! grep -F -q -e 'test_work_mismatch_rejected' "$tmp/err"; then
    echo "P5 NC BROKEN: failed for another reason (inner rc=$rc):" >&2
    cat "$tmp/err" >&2
    exit 2
fi
printf 'P5 NC: inner rc=%s; test_work_mismatch_rejected: ValueError not raised\n' "$rc"
echo 'P5 NC: FAILED AS DESIGNED (cross-build guest work/clock equality is required)'
exit 1
