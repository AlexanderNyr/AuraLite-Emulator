#!/bin/sh
# COMPAT G6 acceptance gate — negative control (patch 0065).
#
# Each case copies the bundle, applies one tampering, and requires
# tests/test_g6_acceptance.py to reject it.  Cases 1-4 re-sign SHA256SUMS, so
# the manifest cannot be what fails: the semantic checks must catch each
# mutation themselves.  Case 5 leaves the manifest stale to prove the digest
# check works.
set -eu

HERE=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
BUNDLE="$HERE/docs/evidence/compat-g6"
GATE="$HERE/tests/test_g6_acceptance.py"
TMP=${TMPDIR:-/tmp}/g6-acceptance-nc.$$
mkdir -p "$TMP"
trap 'rm -rf "$TMP"' EXIT INT TERM

[ -d "$BUNDLE" ] || { echo "NC setup: missing $BUNDLE" >&2; exit 1; }
[ -f "$GATE" ] || { echo "NC setup: missing $GATE" >&2; exit 1; }

resign() {
    ( cd "$1" && sha256sum $(ls | grep -v '^SHA256SUMS$') > SHA256SUMS )
}

expect_reject() {
    case_name=$1
    dir=$2
    if python3 "$GATE" --evidence "$dir" >"$dir.gate.log" 2>&1; then
        echo "NC FAIL: $case_name — gate accepted a tampered bundle"
        cat "$dir.gate.log"
        exit 1
    fi
    echo "NC PASS: $case_name (${dir}.gate.log)"
}

# Case 1: AP #2 online marker removed from serial log; manifest re-signed.
C1="$TMP/1-missing-ap2"
cp -a "$BUNDLE" "$C1"
gzip -dc "$C1/x86emu-smp4.serial.log.gz" \
    | grep -v 'AP #2 online (lapic_id=3)' \
    | gzip -9n > "$C1/x86emu-smp4.serial.log.gz.new"
mv "$C1/x86emu-smp4.serial.log.gz.new" "$C1/x86emu-smp4.serial.log.gz"
resign "$C1"
expect_reject "serial log missing AP #2 online" "$C1"

# Case 2: SMPSTRESS PASS marker removed from serial log; manifest re-signed.
C2="$TMP/2-no-smpstress"
cp -a "$BUNDLE" "$C2"
gzip -dc "$C2/x86emu-smp4.serial.log.gz" \
    | grep -v 'SMPSTRESS PASS' \
    | gzip -9n > "$C2/x86emu-smp4.serial.log.gz.new"
mv "$C2/x86emu-smp4.serial.log.gz.new" "$C2/x86emu-smp4.serial.log.gz"
resign "$C2"
expect_reject "serial log without SMPSTRESS PASS" "$C2"

# Case 3: vcpu=1 executed_steps Perturbed in stdout (breaks sum(executed_steps) == executed).
C3="$TMP/3-counter-drift"
cp -a "$BUNDLE" "$C3"
sed -i 's/\[stats\] vcpu=1 executed_steps=82923322/[stats] vcpu=1 executed_steps=82922586/' \
    "$C3/x86emu-smp4.stdout.txt"
resign "$C3"
expect_reject "AP counter reset across second-stage INIT" "$C3"

# Case 4: cpu_count in boot_info_t tampered from 4 to 1; manifest re-signed.
C4="$TMP/4-bootinfo-cpus1"
cp -a "$BUNDLE" "$C4"
python3 - "$C4/bootinfo-smp4.bin" <<'PY'
import struct, sys
path = sys.argv[1]
raw = bytearray(open(path, "rb").read())
assert struct.unpack_from("<I", raw, 6216)[0] == 4
struct.pack_into("<I", raw, 6216, 1)
open(path, "wb").write(raw)
PY
resign "$C4"
expect_reject "boot_info_t with cpu_count=1 instead of 4" "$C4"

# Case 5: MADT cpus=4 altered in acpi-receipt-smp4.txt, manifest left stale.
C5="$TMP/5-stale-manifest"
cp -a "$BUNDLE" "$C5"
sed -i 's/cpus=4/cpus=1/' "$C5/acpi-receipt-smp4.txt"
expect_reject "ACPI receipt edited with a stale manifest" "$C5"

echo "g6 acceptance NC: all five cases rejected"
