#!/usr/bin/env bash
# tests/perf_bench.sh -- PERF P0: the wall-clock bench.
#
# Before P0 this project had no way to state how fast it is.  Every gate in
# the suite compares STATE (make test, make fuzz's "all states match", the
# byte-identical lane tapes) and none compares TIME, so "no performance
# regressions" -- which ROADMAP.md lists as a COMPAT risk -- was not a claim
# anything could check.  This script is the measurement; it gates accounting,
# not a host-dependent wall-time threshold.  Later phases compare builds on
# the same host.
#
# It measures host wall time from OUTSIDE the emulator (process spawn to
# exit), not with a clock inside the run loop: an internal clock would start
# after machine construction and stop before teardown, i.e. it would report
# the emulator as faster than it is.  The cost is that the number includes
# process startup and is therefore machine-relative -- which is fine, because
# P1/P3 are justified by the ratio between two builds on ONE machine, and no
# absolute figure here is portable.  What IS portable, and what the gates
# below assert, is the instruction accounting.
#
# The numerator matters more than the stopwatch.  Three candidates exist and
# two of them are wrong:
#
#   --max-instr     a limit, not a measurement.  The firmware-boot tape stops
#                   at 4837677 attempts; this harness caps at 50000000.
#                   Dividing the cap by wall time overstates throughput by
#                   ~10x on that tape.  This is the mistake P0 exists to make
#                   impossible, and it was made for real while scoping this
#                   phase (a "28.69 MIPS" figure from a 20M cap on a tape that
#                   executed 4.84M).
#   instr_count     the K6 clock: executed work PLUS 512 per all-idle HLT slot
#                   (src/cpu.c), and guest-visible through PIT/RTC/LAPIC/RDTSC.
#                   Correct for a tape that never idles, inflated for one that
#                   does.  tests/test_perf.c vector 2 pins the divergence.
#   executed_steps  instruction attempts actually executed, summed over every context
#                   by machine_executed_total() (src/machine.h) and reported by
#                   --stats as `executed=`.  Faulting attempts count too, so
#                   this is a guest-steps/s metric, NOT ISA-level retirement.
#                   The only honest throughput numerator for this run loop.
#
# Gates, all deterministic (no gate here depends on a timing):
#   1. every repetition of a tape reports the SAME executed/vtime/idle numbers
#      -- a benchmark whose workload varies between runs measures noise;
#   2. no tape may reach its --max-instr cap, because then wall time measured
#      the cap and not the workload (the script refuses to report such a tape);
#   3. executed > 0, so a tape that silently did nothing cannot report MIPS;
#   4. self-check: cap-based MIPS >= executed-based MIPS, with strict > and a
#      printed inflation factor for any tape that stopped before its cap.
#
# Usage: tests/perf_bench.sh [--reps=N] [--nc]
#   --reps=N   repetitions per tape (default 5, must be odd for an exact median)
#   --nc       the phase negative control: report the SAME measurement with the
#              cap as numerator, assert that it inflates, and exit 1.  This is
#              the documented NC for P0 and is not part of `make test`.
#
# Requires the built x86emu plus firmware/firmware.bin and disk/disk.img (the
# nasm artifacts; see docs/plans/NET_PLAN.md 3.9 for the seeding recipe).  The
# --kernel= tapes are deliberately absent: the frozen guest cannot be built
# here (clang/ld.lld/nasm/mformat/mcopy/lld-link/llvm-rc/rustc are all
# missing), so a kernel lane would SKIP rather than measure.
set -euo pipefail
export LC_ALL=C

EMUX=${EMUX:-./x86emu}
REPS=5
NC=0
for a in "$@"; do
    case $a in
        --reps=*) REPS=${a#--reps=} ;;
        --nc)     NC=1 ;;
        *) echo "usage: $0 [--reps=N] [--nc]" >&2; exit 2 ;;
    esac
done
case $REPS in (*[!0-9]*|'') echo "--reps wants a positive integer" >&2; exit 2;; esac
[ "$REPS" -ge 1 ] || { echo "--reps wants >= 1" >&2; exit 2; }
if [ $((REPS % 2)) -eq 0 ]; then
    echo "--reps must be odd so the median is an actual sample, not an average of two" >&2
    exit 2
fi

[ -x "$EMUX" ] || { echo "MISSING: $EMUX is not executable (make x86emu)" >&2; exit 1; }
for f in firmware/firmware.bin disk/disk.img; do
    [ -f "$f" ] || { echo "MISSING: $f (nasm artifact -- see NET_PLAN.md 3.9)" >&2; exit 1; }
done

# name|cap|extra args...   The cap is deliberately generous on every tape so
# that gate 2 passes by stopping early, not by being cut off.
# Per-tape arguments are used verbatim -- the harness adds only
# --max-instr and --stats.  (An earlier draft appended --no-fb to every tape,
# which silently made gfx-fb and text-no-fb the same measurement; the two
# rows came out byte-identical and that is how it was caught.)
TAPES=(
    "boot-haswell|50000000|--platform=haswell"
    "boot-broadwell|50000000|--platform=broadwell"
    "boot-baytrail|50000000|--platform=baytrail"
    "text-no-fb|50000000|--platform=haswell --no-fb"
    "gfx-fb|50000000|--platform=haswell"
    "vbe-800x600x32|50000000|--platform=haswell --vbe=800x600x32"
    "smp2|20000000|--platform=haswell --smp=2 --cpus=2"
)

# bash, not the #!/bin/sh of the other harnesses, for one reason: TIMEFORMAT
# gives millisecond resolution.  POSIX `time` resolves to 10 ms, which on the
# shortest tape here (~0.18 s) is a ~5% error in the measurement itself --
# larger than the effect a dispatch-table change is supposed to show.
TIMEFORMAT='%3R'
trap 'rm -f /tmp/perf_bench.$$.out' EXIT

median_ns() {   # stdin: one "seconds" float per line -> stdout: median in ns
    sort -g | awk '{a[NR]=$1} END {printf "%.0f", a[int((NR+1)/2)] * 1e9}'
}

stat() {        # $1=stats text  $2=key
    printf '%s\n' "$1" | sed -n "s/.*[ ]$2=\([0-9]*\).*/\1/p" | head -1
}

fail=0
rows=""
if [ "$NC" -eq 0 ]; then
    printf '%-16s %5s %10s %12s %9s %9s %10s %8s\n' \
        tape reps med_ms executed mips ns/instr cap_mips inflate
fi
for t in "${TAPES[@]}"; do
    IFS='|' read -r name cap args <<< "$t"
    times=(); first=""
    for _ in $(seq 1 "$REPS"); do
        # shellcheck disable=SC2086
        s=$( { TIMEFORMAT='%3R'; time "$EMUX" $args --max-instr="$cap" \
                 --stats >/tmp/perf_bench.$$.out 2>/dev/null; } 2>&1 )
        times+=("$s")
        st=$(cat /tmp/perf_bench.$$.out)
        exec_n=$(stat "$st" executed); vt=$(stat "$st" vtime)
        idle=$(stat "$st" idle_quantum); cr=$(stat "$st" cap_reached)
        line="executed=$exec_n vtime=$vt idle=$idle cap_reached=$cr"
        if [ -z "$first" ]; then first=$line
        elif [ "$first" != "$line" ]; then
            echo "NONDETERMINISTIC: $name" >&2
            echo "  rep1: $first" >&2; echo "  repN: $line" >&2
            fail=1
        fi
    done
    rm -f /tmp/perf_bench.$$.out

    # gate 3: the tape must have done work
    if [ "${exec_n:-0}" -eq 0 ]; then
        echo "ERROR: $name executed=0 -- nothing to measure" >&2; fail=1; continue
    fi
    # gate 2: a capped run measured the cap, not the workload
    if [ "${cr:-1}" -eq 1 ]; then
        echo "ERROR: $name hit --max-instr=$cap, so its wall time is the cap's." >&2
        echo "       Raise the cap until the tape stops on its own." >&2
        fail=1; continue
    fi

    med=$(printf '%s\n' "${times[@]}" | median_ns)
    read -r mips nsinstr capmips inflate <<< "$(awk -v med="$med" -v ex="$exec_n" \
        -v cap="$cap" 'BEGIN {
            m  = ex / (med / 1e9) / 1e6;
            printf "%.2f %.1f %.2f %.2f\n", m, med / ex,
                   cap / (med / 1e9) / 1e6, (m > 0 ? (cap / (med / 1e9) / 1e6) / m : 0);
        }')"

    # gate 4: the cap can only ever overstate
    if awk -v c="$capmips" -v m="$mips" 'BEGIN { exit !(c + 1e-9 < m) }'; then
        echo "ERROR: $name cap-based MIPS ($capmips) < executed-based ($mips)" >&2
        echo "       impossible unless the numerator is not the cap -- harness bug" >&2
        fail=1
    fi

    if [ "$NC" -eq 1 ]; then
        # The negative control reports THIS measurement with the cap as the
        # numerator and asserts that it inflates.  It must fail: a tape that
        # stops before its cap is the normal case here, and a harness that
        # could not tell the two numerators apart is the defect P0 fixes.
        rows="$rows$(printf '%-16s cap_mips=%-7s real_mips=%-6s inflate=%sx\n' \
                     "$name" "$capmips" "$mips" "$inflate")"$'\n'
        if awk -v i="$inflate" 'BEGIN { exit !(i > 1.05) }'; then :; else
            echo "ERROR: $name inflate=${inflate}x -- the cap-based figure is not" >&2
            echo "       distinguishable from the real one, so this NC proves nothing" >&2
            fail=1
        fi
        continue
    fi

    printf '%-16s %5s %10s %12s %9s %9s %10s %8s\n' \
        "$name" "$REPS" "$(awk -v n="$med" 'BEGIN{printf "%.1f", n/1e6}')" \
        "$exec_n" "$mips" "$nsinstr" "$capmips" "${inflate}x"
    rows="$rows$name $mips"$'\n'
done

if [ "$NC" -eq 1 ]; then
    printf '%s' "$rows"
    echo
    echo "NC (PERF P0): the same wall-time measurement, but with --max-instr as the"
    echo "MIPS numerator instead of executed instruction attempts.  Every tape above stops"
    echo "before its cap, so every cap-based figure is inflated -- the boot tapes"
    echo "~10x, because the guest halts at 4837677 instructions while the default"
    echo "cap is 50000000.  This is the arithmetic that produced a real 28.69 MIPS"
    echo "claim during P0 scoping on a tape that executed 4.84M instructions"
    echo "(7.06 MIPS).  The shipped harness uses executed= and refuses capped"
    echo "tapes; this variant exists to be run and to fail."
    echo "perf bench NC: FAILED AS DESIGNED (cap-based MIPS is not a measurement)"
    exit 1
fi

[ "$fail" -eq 0 ] || { echo "perf bench: FAILED" >&2; exit 1; }
echo
echo "Perf baseline: all tapes deterministic, none capped, MIPS from actual"
echo "instruction attempts only.  These wall times are THIS machine's; compare ratios"
echo "between two builds here, never against a number quoted elsewhere."
echo "perf bench: ok"
