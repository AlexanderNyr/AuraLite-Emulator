#!/usr/bin/env bash
# VIDEO V3 mode-switch + V1 guest lanes, each twice: every pair byte-identical.
#
#   mode-gfx  -- a mode-switch lane through the dispi interface (--vbe=800x600x32
#               runs a harness-side VBE client's mode-set via ports 0x1CE/0x1CF):
#               the display shows the LFB surface -- the guest's Limine splash
#               panel (measured: 0x10101c background + 0xdcdcdc foreground).
#   mode-txt  -- the V1 guest lane (--no-fb): the display shows the mode-3
#               console render (measured: uniform 0x000000 cls at this slice).
#
# Each lane runs twice; ppm/txt/log pairs must be byte-identical.  The recipe
# is the frozen guest recipe (kernel/initrd measured in docs/plans/VIDEO_PLAN.md)
# with --vbe/--no-fb and a deterministic instruction slice (no disk needed).  The x2 pairs land in tests/integration/
# (the repo's *.ppm/*.log gitignore keeps the bulky artifacts local; the
# measured pair shas are recorded in docs/plans/VIDEO_PLAN.md).
set -u
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
EMU="$ROOT/x86emu"
K=/home/user/AuraLite-OS/build/kernel.elf
I=/home/user/AuraLite-OS/build/initrd.tar
INSTR=80000000

command -v python3 >/dev/null 2>&1 || { echo "SKIP: python3 missing"; exit 0; }
[ -x "$EMU" ] || { echo "ERROR: build x86emu first (make x86emu)"; exit 1; }
[ -r "$K" ] && [ -r "$I" ] || { echo "SKIP: frozen guest kernel/initrd missing ($K)"; exit 0; }

run() { # run <name> <extra-arg>
    local n="$1" extra="$2"
    "$EMU" --platform=haswell --kernel="$K" --initrd="$I" $extra \
        --dump-fb="$ROOT/tests/integration/$n.ppm" \
        --dump-text="$ROOT/tests/integration/$n.txt" \
        --log="$ROOT/tests/integration/$n.log" \
        --max-instr=$INSTR >/dev/null 2>&1
}

for n in 1 2; do
    run "mode-gfx-$n" "--vbe=800x600x32" &
    run "mode-txt-$n" "--no-fb" &
done
wait

pass=1
for d in mode-gfx mode-txt; do for e in ppm txt log; do
    if cmp -s "$ROOT/tests/integration/$d-1.$e" "$ROOT/tests/integration/$d-2.$e"; then
        echo "cmp $d.$e: byte-identical ($(sha256sum < "$ROOT/tests/integration/$d-1.$e" | cut -c1-12))"
    else
        echo "cmp $d.$e: MISMATCH"; pass=0
    fi
done; done

# display output assertions per mode (measured expectations, above)
(cd "$ROOT" && python3 - <<'EOF'
import sys
from collections import Counter
def colors(path):
    d = open(path, 'rb').read()
    i = d.index(b'255\n') + 4
    px = d[i:]
    return Counter(px[j:j+3] for j in range(0, len(px), 3))
gfx = colors('tests/integration/mode-gfx-1.ppm')
txt = colors('tests/integration/mode-txt-1.ppm')
ok = True
if gfx.get(b'\x10\x10\x1c', 0) < 100000 or gfx.get(b'\xdc\xdc\xdc', 0) < 10000:
    print('display gfx: FAIL (splash panel missing)', gfx.most_common(2)); ok = False
else:
    print('display gfx: ok (Limine splash panel on the LFB surface)')
if list(txt) != [b'\x00\x00\x00']:
    print('display txt: FAIL (mode-3 console not the cls render)', txt.most_common(2)); ok = False
else:
    print('display txt: ok (mode-3 console render)')
sys.exit(0 if ok else 1)
EOF
)
[ $? -eq 0 ] || pass=0

if [ $pass -eq 1 ]; then
    echo "test_video_modes: ALL PASS (mode-switch + V1 guest lanes, x2 byte-identical)"
else
    echo "test_video_modes: FAIL"; exit 1
fi
