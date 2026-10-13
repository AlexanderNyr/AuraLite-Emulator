#!/bin/sh
# PERF P2 -- the one negative control: drop the RAM-store TLB invalidation
# in a scratch copy of mem.c.  The SAME test_tlb.c as the positive gate must
# fail after a warmed 4KiB mapping is rewritten through mem_write (the
# physical page changes 5MiB -> 6MiB).  A success on the mutated source
# would mean the vector does not actually guard stale PTEs.  No checkout
# file is changed.  Script exits 1 when the expected assertion is observed;
# make wraps the failing recipe and exits 2.
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
a = '        perf_tlb_flush(m);\n        return;'
b = '        /* P2 NC: deliberately leave stale PTE entries */\n        return;'
assert s.count(a) == 1, 'NC mutation anchor must be the unique RAM-store flush'
p.write_text(s.replace(a, b, 1))
PY
${CC:-cc} -std=c11 -O2 -Wall -Wextra -Werror -Isrc -Itests \
    tests/test_tlb.c "$tmp/mem.c" src/cpu.c src/io.c src/platform.c \
    src/pic.c src/pit.c src/rtc.c src/kbc.c src/chipset.c src/lapic.c \
    src/ioapic.c -o "$tmp/test-tlb-nc" -lm
ulimit -c 0 || true
set +e
"$tmp/test-tlb-nc" >"$tmp/out" 2>"$tmp/err"
rc=$?
set -e
if [ "$rc" -eq 0 ]; then
    echo 'NC BROKEN: stale PTE returned the new data without a RAM-store flush' >&2
    exit 2
fi
if ! grep -F -q -e 'c->gpr[RAX] == NEW_VALUE' "$tmp/err"; then
    echo "NC BROKEN: failed for another reason (inner rc=$rc):" >&2
    cat "$tmp/err" >&2
    exit 2
fi
printf 'P2 NC: inner rc=%s; assert c->gpr[RAX] == NEW_VALUE failed after warmed PTE write\n' "$rc"
echo 'P2 NC: FAILED AS DESIGNED (RAM-store flush is necessary for PTE coherence)'
exit 1
