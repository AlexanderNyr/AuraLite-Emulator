#!/bin/sh
# COMPAT G5 — negative control for the ACPI PM timer (patch 0064).
#
# The fix claims one thing: the ICH9 boot hangs because PMBASE+8 has no state.
# This script restores exactly that defect in a throw-away copy and requires the
# boot to stall again, while the unmodified tree reaches Stage 2 on the same
# image and the same instruction budget.
#
#   case "frozen": pm_timer_now() returns 0xFFFFFFFF (the pre-fix port float).
#     -> must NOT reach [BL3], must burn the whole budget.
#   case "control": unmodified sources, same budget.
#     -> must reach [BL3].
#
# The 300M budget is measured, not guessed: the fixed boot prints [BL3] at
# executed_steps=297,585,739 and the frozen one prints nothing in 300M.
set -eu

HERE=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
TMP=${TMPDIR:-/tmp}/g5-pm-nc.$$
BUDGET=300000000
mkdir -p "$TMP"
trap 'rm -rf "$TMP"' EXIT INT TERM

ROM=${G5_ROM:-/tmp/sbpkg/usr/share/seabios/bios.bin}
ISO=${G5_ISO:-/tmp/iso-candidate.iso}
DISK="$HERE/docs/evidence/compat-g3/blank-usb-1m.img"

for f in "$ROM" "$ISO" "$DISK"; do
    [ -f "$f" ] || { echo "NC setup: missing $f" >&2; exit 2; }
done

run_boot() {   # $1 = tree, $2 = tag
    ( cd "$1" && ./x86emu --lpc=ich9 --rom="$ROM" --platform=haswell \
        --disk="$DISK" --sata="$ISO" --max-instr="$BUDGET" --stats ) \
        >"$TMP/$2.out" 2>"$TMP/$2.serial.log"
}

# --- case 1: the defect, restored ------------------------------------------
cp -a "$HERE" "$TMP/mutant"
python3 - "$TMP/mutant/src/pm.c" <<'PY'
import sys
path = sys.argv[1]
src = open(path).read()
old = """uint32_t pm_timer_now(machine_t *m) {
    pm_advance(m);
    return m->pm.count & PM_TMR_MASK;
}"""
new = """uint32_t pm_timer_now(machine_t *m) {
    /* G5 NC: pre-fix behaviour -- unconnected port floats high. */
    (void)m;
    return 0xFFFFFFFFu;
}"""
assert old in src, "NC setup: pm_timer_now() shape changed; update the mutation"
src = src.replace(old, new, 1)
src = src.replace("static void pm_advance(machine_t *m) {",
                  "__attribute__((unused)) static void pm_advance(machine_t *m) {", 1)
open(path, "w").write(src)
PY
( cd "$TMP/mutant" && make -j4 x86emu >"$TMP/mutant.build.log" 2>&1 ) || {
    echo "NC setup: mutant build failed; see $TMP/mutant.build.log" >&2; exit 2; }

run_boot "$TMP/mutant" frozen
steps=$(sed -n 's/.*executed_steps=\([0-9]*\).*/\1/p' "$TMP/frozen.out" | head -1)
if grep -qa '\[BL3\]' "$TMP/frozen.serial.log"; then
    echo "NC FAIL: frozen TMR still reached Stage 2 (steps=$steps)"
    exit 1
fi
[ "${steps:-0}" = "$BUDGET" ] || {
    echo "NC FAIL: frozen run stopped early at steps=$steps, expected $BUDGET"
    exit 1; }
echo "NC PASS: frozen TMR never reaches [BL3] (steps=$steps)"

# --- case 2: control --------------------------------------------------------
run_boot "$HERE" control
csteps=$(sed -n 's/.*executed_steps=\([0-9]*\).*/\1/p' "$TMP/control.out" | head -1)
if ! grep -qa '\[BL3\]' "$TMP/control.serial.log"; then
    echo "NC FAIL: control did not reach [BL3] within $BUDGET"
    exit 1
fi
[ "${csteps:-0}" -lt "$BUDGET" ] || {
    echo "NC FAIL: control only reached [BL3] by exhausting the budget"
    exit 1; }
echo "NC PASS: control reaches [BL3] at steps=$csteps (< $BUDGET)"

diff_log="$HERE/docs/evidence/compat-g5/nc-pm.txt"
{
    echo "# COMPAT G5 PM-timer negative control"
    echo "# rom=$ROM iso=$ISO"
    echo "# md5=$(md5sum <"$ROM" | cut -d' ' -f1)"
    echo "frozen:  BL3=absent steps=$steps"
    echo "control: BL3=present steps=$csteps"
} > "$diff_log" 2>/dev/null || true
echo "NC: log -> ${diff_log#$HERE/}"
