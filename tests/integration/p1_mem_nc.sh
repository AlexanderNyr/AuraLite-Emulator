#!/bin/sh
# PERF P1 -- one negative control for the 8-byte RAM fast path.
#
# Create a throwaway copy of mem.c with the optimized read's return byte-
# swapped.  It is still an 8-byte load, and the code still runs, but it is
# now WRONG for the guest's little-endian contract.  Compile the very same
# test_perf_mem.c shipped in the phase and demand that its first LE assertion
# fail.  This mutation touches neither the checkout nor the passing gates.
# The script exits 1 intentionally when it has proved the NC is red.
# `make test-perf-mem-nc` wraps the recipe error and exits 2.
set -eu

root=$(CDPATH= cd -- "$(dirname "$0")/../.." && pwd)
cd "$root"
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT HUP INT TERM
cp src/mem.c "$tmp/mem.c"
python3 - "$tmp/mem.c" <<'PY'
from pathlib import Path
import sys
p = Path(sys.argv[1]); s = p.read_text()
a = 'return v;  /* PERF P1: fast path, exactly 8 in-range RAM bytes */'
b = 'return __builtin_bswap64(v);  /* PERF P1 NC: wrong endian */'
assert s.count(a) == 1, 'NC mutation anchor must be exactly one fast path'
p.write_text(s.replace(a, b, 1))
PY

${CC:-cc} -std=c11 -O2 -Wall -Wextra -Werror -Isrc -Itests \
    tests/test_perf_mem.c "$tmp/mem.c" src/cpu.c src/io.c src/platform.c \
    src/pic.c src/pit.c src/rtc.c src/kbc.c src/chipset.c src/lapic.c \
    src/ioapic.c -o "$tmp/test-perf-mem-nc" -lm
ulimit -c 0 || true
set +e
"$tmp/test-perf-mem-nc" >"$tmp/out" 2>"$tmp/err"
rc=$?
set -e
if [ "$rc" -eq 0 ]; then
    echo 'NC BROKEN: swapped-endian fast path passed the P1 vector' >&2
    exit 2
fi
if ! grep -F -q -e 'mem_read(&g_m, 0x123, 8) == 0x1716151413121110ULL' "$tmp/err"; then
    echo "NC BROKEN: failed for another reason (inner rc=$rc):" >&2
    cat "$tmp/err" >&2
    exit 2
fi
printf 'P1 NC: inner rc=%s; assert mem_read(&g_m, 0x123, 8) == 0x1716151413121110ULL failed as designed\n' "$rc"
echo 'P1 NC: FAILED AS DESIGNED (swapped-endian RAM fast path is detected)'
exit 1
