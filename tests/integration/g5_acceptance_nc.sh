#!/bin/sh
# COMPAT G5 acceptance gate — negative control (patch 0064).
#
# Each case copies the bundle, applies one tampering, and requires
# tests/test_g5_acceptance.py to reject it.  Cases 1-3 re-sign SHA256SUMS, so
# the manifest cannot be what fails: the semantic checks must catch each
# mutation themselves.  Case 4 leaves the manifest stale to prove the digest
# check works.
set -eu

HERE=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
BUNDLE="$HERE/docs/evidence/compat-g5"
GATE="$HERE/tests/test_g5_acceptance.py"
TMP=${TMPDIR:-/tmp}/g5-acceptance-nc.$$
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

# Case 1: shell-active marker removed; manifest re-signed.
C1="$TMP/1-no-shell"
cp -a "$BUNDLE" "$C1"
gzip -dc "$C1/x86emu-ich9.serial.log.gz" \
    | grep -v 'shell active; kmain idling' \
    | gzip -9n > "$C1/x86emu-ich9.serial.log.gz.new"
mv "$C1/x86emu-ich9.serial.log.gz.new" "$C1/x86emu-ich9.serial.log.gz"
resign "$C1"
expect_reject "serial log without the shell-active marker" "$C1"

# Case 2: the ICH9 RCRB reservation dropped from boot_info_t; manifest re-signed.
C2="$TMP/2-no-rcrb"
cp -a "$BUNDLE" "$C2"
python3 - "$C2/bootinfo-ich9.bin" <<'PY'
import struct, sys
path = sys.argv[1]
raw = bytearray(open(path, "rb").read())
count = struct.unpack_from("<I", raw, 6184)[0]
assert count == 8, count
# Drop entry 6 (0xfed1c000+0x4000) by shifting entry 7 down and counting 7.
base = 40
raw[base + 6 * 24: base + 7 * 24] = raw[base + 7 * 24: base + 8 * 24]
raw[base + 7 * 24: base + 8 * 24] = b"\0" * 24
struct.pack_into("<I", raw, 6184, 7)
open(path, "wb").write(raw)
PY
resign "$C2"
expect_reject "E820 without the ICH9 RCRB reservation" "$C2"

# Case 3: the PM-timer negative control edited to claim the mutant boots.
C3="$TMP/3-nc-claims-pass"
cp -a "$BUNDLE" "$C3"
sed -i 's/^frozen:  BL3=absent steps=300000000/frozen:  BL3=present steps=297585739/' \
    "$C3/nc-pm.txt"
resign "$C3"
expect_reject "negative control claims the frozen timer boots" "$C3"

# Case 4: RSDP zeroed in boot_info_t, manifest left stale.
C4="$TMP/4-stale-manifest"
cp -a "$BUNDLE" "$C4"
python3 - "$C4/bootinfo-ich9.bin" <<'PY'
import sys
path = sys.argv[1]
raw = bytearray(open(path, "rb").read())
raw[7760:7768] = b"\0" * 8
open(path, "wb").write(raw)
PY
expect_reject "RSDP zeroed with a stale manifest" "$C4"

echo "g5 acceptance NC: all four cases rejected"
