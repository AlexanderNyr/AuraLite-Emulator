#!/bin/sh
# PERF P3's ONE negative control: remove only the decode epoch match in a
# scratch CPU source.  Keep P2's RAM/PTE translation flush intact.  The
# ordinary warmed self-modifying opcode vector must now execute stale STC.
set -eu
root=$(CDPATH= cd -- "$(dirname "$0")/../.." && pwd)
cd "$root"
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT HUP INT TERM
cp src/cpu.c "$tmp/cpu.c"
python3 - "$tmp/cpu.c" <<'PY'
from pathlib import Path
import sys
p = Path(sys.argv[1]); s = p.read_text()
a = 'if (!e->valid || e->generation != m->perf_tlb_generation ||'
b = 'if (!e->valid || /* P3 NC: reuse decode after a RAM store */'
assert s.count(a) == 1, 'one decode-lookup epoch anchor required'
p.write_text(s.replace(a, b, 1))
PY
${CC:-cc} -std=c11 -O2 -Wall -Wextra -Werror -Isrc -Itests \
    tests/test_decode.c "$tmp/cpu.c" src/mem.c src/io.c src/platform.c \
    src/pic.c src/pit.c src/rtc.c src/kbc.c src/chipset.c src/lapic.c \
    src/ioapic.c -o "$tmp/test-decode-nc" -lm
ulimit -c 0 || true
set +e
"$tmp/test-decode-nc" >"$tmp/out" 2>"$tmp/err"
rc=$?
set -e
if [ "$rc" -eq 0 ]; then
    echo 'NC BROKEN: stale decode returned the new opcode without the epoch check' >&2
    exit 2
fi
if ! grep -F -q -e '!(c->rflags & CF)' "$tmp/err"; then
    echo "NC BROKEN: failed for another reason (inner rc=$rc):" >&2
    cat "$tmp/err" >&2
    exit 2
fi
printf 'P3 NC: inner rc=%s; assert !(c->rflags & CF) failed after warmed opcode rewrite\n' "$rc"
echo 'P3 NC: FAILED AS DESIGNED (shared RAM-write generation is necessary for predecode coherence)'
exit 1
