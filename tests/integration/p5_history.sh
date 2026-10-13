#!/usr/bin/env bash
# PERF P5: paired historical firmware/video receipts using identical output
# paths for each binary.  Relative artifact names resolve from the repo root.
# This complements N2/N3's separate x2 wire gates in `make test`.
set -euo pipefail
root=$(CDPATH= cd -- "$(dirname "$0")/../.." && pwd)
cd "$root"
[ "$#" -eq 2 ] || { echo "usage: $0 BEFORE_BINARY AFTER_BINARY" >&2; exit 2; }
pre=$(realpath "$1"); post=$(realpath "$2")
[ -x "$pre" ] && [ -x "$post" ] || { echo 'missing executable' >&2; exit 2; }
for artifact in firmware/firmware.bin disk/disk.img; do
    [ -f "$artifact" ] || { echo "missing $artifact" >&2; exit 2; }
done
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
# Deliberately keep the first four original P2/P3 firmware/video lane shapes
# (including the exact --dump-fb and --log option paths).
lanes=(
 'boot-haswell|--platform=haswell'
 'vbe-on|--platform=haswell --vbe=800x600x32'
 'vbe-off|--platform=haswell --vbe=off'
 'no-fb|--platform=haswell --no-fb'
 'boot-sandybridge|--platform=sandybridge'
 'boot-ivybridge|--platform=ivybridge'
 'boot-broadwell|--platform=broadwell'
 'boot-baytrail|--platform=baytrail'
 'smp2|--platform=haswell --smp=2 --cpus=2'
)
for lane in "${lanes[@]}"; do
    IFS='|' read -r name opts <<< "$lane"
    for side in pre post; do
        if [ "$side" = pre ]; then bin=$pre; else bin=$post; fi
        # Same names in stdout for both runs, independent of output directory.
        # shellcheck disable=SC2086
        "$bin" $opts --max-instr=600000 --log="$work/shared.log" \
            --dump-fb="$work/shared.ppm" >"$work/$side.out" 2>"$work/$side.err"
        cp "$work/shared.log" "$work/$side.log"
        cp "$work/shared.ppm" "$work/$side.ppm"
    done
    for artifact in out err log ppm; do
        if ! cmp -s "$work/pre.$artifact" "$work/post.$artifact"; then
            echo "P5 HISTORY DIFF: $name $artifact" >&2
            exit 1
        fi
    done
    if [ "$name" = boot-haswell ]; then
        cp "$work/pre.ppm" "$work/gfx.ppm"
    elif [ "$name" = no-fb ]; then
        # D4: reject a harness that secretly appended --no-fb to gfx too.
        if cmp -s "$work/gfx.ppm" "$work/pre.ppm"; then
            echo 'P5 HISTORY D4: gfx and no-fb framebuffers unexpectedly equal' >&2
            exit 1
        fi
        echo 'P5 HISTORY D4: gfx and no-fb framebuffers are distinct'
    fi
    echo "P5 HISTORY: $name stdout/stderr/log/PPM byte-identical (RC=0/0)"
done
echo 'P5 HISTORY: all nine available firmware/video/SMP lanes byte-identical'
