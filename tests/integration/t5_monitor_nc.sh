#!/bin/sh
# TOOLING T5's ONE negative control: a scratch ws.c uses the wrong RFC 6455
# GUID. HTTP serving and the 101 upgrade remain healthy, but the client-side
# sample-key accept check must fail. The production source tree is untouched.
set -eu
root=$(CDPATH= cd -- "$(dirname "$0")/../.." && pwd)
cd "$root"
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT HUP INT TERM
mkdir -p "$tmp/src"
cp src/*.c src/*.h "$tmp/src/"
python3 - "$tmp/src/ws.c" <<'PY'
from pathlib import Path
import sys
p = Path(sys.argv[1])
s = p.read_text()
a = '258EAFA5-E914-47DA-95CA-C5AB0DC85B11'
b = '258EAFA5-E914-47DA-95CA-C5AB0DC85B12'
assert s.count(a) == 1, 'one RFC 6455 GUID anchor required'
p.write_text(s.replace(a, b, 1))
PY
${CC:-cc} -std=c11 -O2 -Wall -Wextra -Werror -I"$tmp/src" "$tmp"/src/*.c -o "$tmp/x86emu" -lm
set +e
X86EMU="$tmp/x86emu" python3 tests/test_monitor.py --lane handshake >"$tmp/client.out" 2>&1
rc=$?
set -e
accept_fails=$(grep -c '^FAIL RFC 6455 sample key -> Sec-WebSocket-Accept$' "$tmp/client.out" || true)
other_fails=$(grep '^FAIL ' "$tmp/client.out" | grep -v '^FAIL test-monitor ' | \
              grep -v '^FAIL RFC 6455 sample key -> Sec-WebSocket-Accept$' || true)
if [ "$rc" -eq 0 ] || \
   ! grep -q '^ok   HTTP GET / serves the live monitor and offline file controls$' "$tmp/client.out" || \
   ! grep -q '^ok   WebSocket upgrade returns 101$' "$tmp/client.out" || \
   [ "$accept_fails" -ne 1 ] || \
   [ -n "$other_fails" ]; then
    echo "NC BROKEN: wrong GUID did not isolate the client accept-key check (rc=$rc)" >&2
    cat "$tmp/client.out" >&2
    exit 2
fi
printf 'T5 NC: client rc=%s; HTTP and 101 green; accept-key vector red\n' "$rc"
echo 'T5 NC: FAILED AS DESIGNED (the RFC 6455 accept GUID is checked by the client)'
exit 1
