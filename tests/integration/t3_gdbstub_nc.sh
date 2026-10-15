#!/bin/sh
# TOOLING T3's ONE negative control: a scratch gdbstub.c whose running-
# state poll never reads the socket. gdb's Ctrl-C (0x03) is then never seen
# while the guest runs: lane B of tests/test_gdbstub.py must report no stop
# reply within 5 s (the guest is an endless `jmp $`), while the stopped-
# state protocol (lanes A and C) stays green -- the failure is exactly the
# asynchronous interrupt path.
set -eu
root=$(CDPATH= cd -- "$(dirname "$0")/../.." && pwd)
cd "$root"
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT HUP INT TERM
mkdir -p "$tmp/src"
cp src/*.c src/*.h "$tmp/src/"
python3 - "$tmp/src/gdbstub.c" <<'PY'
from pathlib import Path
import sys
p = Path(sys.argv[1]); s = p.read_text()
a = '    if ((++g->poll_div & 63u) != 0) return;   /* every 64Ki steps */'
b = '    (void)m; return;   /* T3 NC: never look for 0x03 while running */'
assert s.count(a) == 1, 'one poll anchor required'
p.write_text(s.replace(a, b, 1))
PY
${CC:-cc} -std=c11 -O2 -Wall -Wextra -Werror -I"$tmp/src" "$tmp"/src/*.c -o "$tmp/x86emu" -lm 2>"$tmp/cc.err" || {
    # the early return leaves the rest of the function unreachable but valid C
    cat "$tmp/cc.err" >&2; exit 2; }
set +e
X86EMU="$tmp/x86emu" python3 tests/test_gdbstub.py >"$tmp/p.out" 2>&1
rc=$?
set -e
broken=0
if [ "$rc" -eq 0 ] || ! grep -q '^FAIL B Ctrl-C (0x03) while running -> T02 (<no stop reply within 5 s>' "$tmp/p.out"; then
    echo "NC BROKEN: lane B did not fail on the missing interrupt (rc=$rc)" >&2; cat "$tmp/p.out" >&2; broken=1
fi
if grep -q '^FAIL [AC] ' "$tmp/p.out"; then
    echo "NC BROKEN: the stopped-state protocol failed too" >&2; grep '^FAIL' "$tmp/p.out" >&2; broken=1
fi
[ "$broken" = 0 ] || exit 2
printf 'T3 NC: client rc=%s; %s; lanes A and C green (%s ok)\n' "$rc" \
    "$(grep '^FAIL B' "$tmp/p.out" | head -1 | sed 's/^FAIL //')" "$(grep -c '^ok' "$tmp/p.out")"
echo 'T3 NC: FAILED AS DESIGNED (the running-state 0x03 poll is what makes Ctrl-C work)'
exit 1
