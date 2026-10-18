#!/bin/sh
# COMPAT G6 — negative control for SeaBIOS SMP bring-up (patch 0065).
#
# SeaBIOS rel-1.16.3's smp_setup() wakes APs with broadcast INIT-assert
# (ICR_LO = 0x000C4500, shorthand = 3 "all excluding self") followed by
# broadcast SIPI (ICR_LO = 0x000C4610, shorthand = 3).  Before G6,
# lapic_icr_write() rejected shorthand != 0 on the dm == 5 / dm == 6 path, so
# every AP stayed in wait-for-SIPI and SeaBIOS built a 1-CPU MADT even when
# CMOS 0x5F advertised extra processors.
#
#   case "mutant": restore `if (shorthand != 0) return;` on the INIT/SIPI path.
#     -> must print `Found 1 cpu(s) max supported 4 cpu(s)` and leave
#        vcpu1..3 at executed_steps=0.
#   case "control": unmodified tree, same diagnostic ROM and 10M budget.
#     -> must print `handle_smp: apic_id=0x1..0x3` and
#        `Found 4 cpu(s) max supported 4 cpu(s)` with all 4 vCPUs executing.
set -eu

HERE=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
TMP=${TMPDIR:-/tmp}/g6-smp-nc.$$
BUDGET=10000000
mkdir -p "$TMP"
trap 'rm -rf "$TMP"' EXIT INT TERM

ROM="$HERE/docs/evidence/compat-g3/seabios-diagnostic-128k.bin"
[ -f "$ROM" ] || { echo "NC setup: missing $ROM" >&2; exit 2; }

run_post() {   # $1 = tree, $2 = tag
    ( cd "$1" && ./x86emu --rom="$ROM" --lpc=ich9 --smp=4 \
        --max-instr="$BUDGET" --stats ) \
        >"$TMP/$2.out" 2>"$TMP/$2.serial.log"
}

# --- case 1: pre-G6 shorthand!=0 early return restored ----------------------
cp -a "$HERE" "$TMP/mutant"
python3 - "$TMP/mutant/src/lapic.c" <<'PY'
import sys
path = sys.argv[1]
src = open(path).read()
needle = "if (m->n_vcpus <= 1 || (shorthand != 2 && shorthand != 3)) {"
repl = "if (1) { /* G6 NC: pre-G6 shorthand != 0 rejection */"
assert needle in src, "NC setup: lapic_icr_write broadcast block not found"
src = src.replace(needle, repl, 1)
open(path, "w").write(src)
PY
( cd "$TMP/mutant" && make -j2 x86emu >"$TMP/mutant.build.log" 2>&1 ) || {
    echo "NC setup: mutant build failed; see $TMP/mutant.build.log" >&2; exit 2; }

run_post "$TMP/mutant" mutant
m_woken=$(grep -ac "handle_smp: apic_id=" "$TMP/mutant.serial.log" || true)
m_found=$(sed -n 's/.*Found \([0-9]*\) cpu(s) max supported \([0-9]*\) cpu(s).*/\1\/\2/p' "$TMP/mutant.serial.log" | head -1)
[ -n "$m_found" ] || m_found="absent"
m_ap1=$(sed -n 's/.*\[stats\] vcpu=1 executed_steps=\([0-9]*\).*/\1/p' "$TMP/mutant.out" | head -1)
m_ap2=$(sed -n 's/.*\[stats\] vcpu=2 executed_steps=\([0-9]*\).*/\1/p' "$TMP/mutant.out" | head -1)
m_ap3=$(sed -n 's/.*\[stats\] vcpu=3 executed_steps=\([0-9]*\).*/\1/p' "$TMP/mutant.out" | head -1)
if [ "$m_woken" != "0" ] || [ "$m_found" = "4/4" ] || [ "$m_ap1" != "0" ] || [ "$m_ap2" != "0" ] || [ "$m_ap3" != "0" ]; then
    echo "NC FAIL: mutant unexpectedly woke APs (woken=$m_woken found=$m_found ap_steps=$m_ap1,$m_ap2,$m_ap3)" >&2
    exit 1
fi
grep -qa "broadcast form not modeled for INIT/SIPI" "$TMP/mutant.serial.log" || {
    echo "NC FAIL: mutant did not log broadcast rejection" >&2; exit 1; }
echo "NC PASS: mutant leaves APs asleep (woken=$m_woken found=$m_found ap_steps=$m_ap1,$m_ap2,$m_ap3)"

# --- case 2: control --------------------------------------------------------
run_post "$HERE" control
c_woken=$(grep -ac "handle_smp: apic_id=" "$TMP/control.serial.log" || true)
c_found=$(sed -n 's/.*Found \([0-9]*\) cpu(s) max supported \([0-9]*\) cpu(s).*/\1\/\2/p' "$TMP/control.serial.log" | head -1)
c_ap1=$(sed -n 's/.*\[stats\] vcpu=1 executed_steps=\([0-9]*\).*/\1/p' "$TMP/control.out" | head -1)
c_ap2=$(sed -n 's/.*\[stats\] vcpu=2 executed_steps=\([0-9]*\).*/\1/p' "$TMP/control.out" | head -1)
c_ap3=$(sed -n 's/.*\[stats\] vcpu=3 executed_steps=\([0-9]*\).*/\1/p' "$TMP/control.out" | head -1)
if [ "$c_woken" != "3" ] || [ "$c_found" != "4/4" ] || [ "${c_ap1:-0}" -le 0 ] || [ "${c_ap2:-0}" -le 0 ] || [ "${c_ap3:-0}" -le 0 ]; then
    echo "NC FAIL: control did not wake all 3 APs (woken=$c_woken found=$c_found ap_steps=$c_ap1,$c_ap2,$c_ap3)" >&2
    exit 1
fi
for id in 0x1 0x2 0x3; do
    grep -qa "handle_smp: apic_id=$id" "$TMP/control.serial.log" || {
        echo "NC FAIL: control missing handle_smp: apic_id=$id" >&2; exit 1; }
done
echo "NC PASS: control wakes all 3 APs (woken=$c_woken found=$c_found ap_steps=$c_ap1,$c_ap2,$c_ap3)"

diff_log="$HERE/docs/evidence/compat-g6/nc-smp.txt"
if [ -d "$HERE/docs/evidence/compat-g6" ]; then
    {
        echo "# COMPAT G6 SMP broadcast INIT/SIPI negative control"
        echo "# rom=docs/evidence/compat-g3/seabios-diagnostic-128k.bin budget=$BUDGET"
        echo "mutant:  woken=$m_woken found=$m_found ap_steps=$m_ap1,$m_ap2,$m_ap3"
        echo "control: woken=$c_woken found=$c_found ap_steps=$c_ap1,$c_ap2,$c_ap3"
    } > "$diff_log" 2>/dev/null || true
    echo "NC: log -> ${diff_log#$HERE/}"
fi
