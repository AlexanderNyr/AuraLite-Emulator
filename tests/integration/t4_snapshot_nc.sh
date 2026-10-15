#!/bin/sh
# TOOLING T4's ONE negative control: a scratch devices.c whose snapshot
# visitor forgets the RAM-backed MMIO windows (framebuffer, CAR, DDR
# registers) -- the classic "one device's state missing" bug. The file
# format stays self-consistent, so loads succeed; the split-run lanes must
# then catch the lost state: every lane split after the test kernel's
# first framebuffer store (vtime 35,283) must differ in the PPM and in
# stdout (the "[guest] framebuffer marker OK" receipt disappears), while
# the lane split at step 1,000 -- before any framebuffer write -- must stay
# green, proving the failure is that state and nothing else.
set -eu
root=$(CDPATH= cd -- "$(dirname "$0")/../.." && pwd)
cd "$root"
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT HUP INT TERM
mkdir -p "$tmp/src"
cp src/*.c src/*.h "$tmp/src/"
python3 - "$tmp/src/devices.c" <<'PY'
from pathlib import Path
import sys
p = Path(sys.argv[1]); s = p.read_text()
a = '        snap_sparse(s, tag, w->buf, (size_t)w->size);'
b = '        (void)tag; (void)w;   /* T4 NC: RAM windows not carried */'
assert s.count(a) == 1, 'one RAM-window anchor required'
p.write_text(s.replace(a, b, 1))
PY
${CC:-cc} -std=c11 -O2 -Wall -Wextra -Werror -I"$tmp/src" "$tmp"/src/*.c -o "$tmp/x86emu" -lm
set +e
X86EMU="$tmp/x86emu" sh tests/integration/t4_snapshot_lanes.sh "$tmp/lanes" >"$tmp/lanes.out" 2>&1
rc=$?
set -e
broken=0
if [ "$rc" -eq 0 ]; then echo "NC BROKEN: the split lanes stayed green" >&2; broken=1; fi
if ! grep -q '^FAIL fw-haswell-mid .*differs in: out ppm' "$tmp/lanes.out"; then
    echo "NC BROKEN: fw-haswell-mid did not fail on stdout+PPM" >&2; broken=1
fi
if ! grep -q '^ok   fw-haswell-early' "$tmp/lanes.out"; then
    echo "NC BROKEN: the control split (before any framebuffer write) failed too" >&2; broken=1
fi
[ "$broken" = 0 ] || { cat "$tmp/lanes.out" >&2; exit 2; }
printf 'T4 NC: lanes rc=%s; %s lanes FAIL (e.g. %s); control fw-haswell-early ok\n' "$rc" \
    "$(grep -c '^FAIL fw-\|^FAIL k1-\|^FAIL guest-' "$tmp/lanes.out")" \
    "$(grep -m1 '^FAIL fw-haswell-mid' "$tmp/lanes.out" | sed 's/^FAIL //; s/  */ /g')"
echo 'T4 NC: FAILED AS DESIGNED (a device whose state is not carried is caught by split-run identity)'
exit 1
