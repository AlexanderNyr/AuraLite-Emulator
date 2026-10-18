#!/bin/sh
# COMPAT G3 acceptance gate — negative control (patch 0063).
#
# Each case copies the acceptance bundle, applies one tampering, and requires
# tests/test_g3_acceptance.py to reject it.  Cases 1-3 recompute SHA256SUMS
# after tampering, so the manifest cannot be what fails: the semantic checks
# (serial receipts, handoff layout) must catch the mutation themselves.
# Case 4 leaves the manifest stale to prove the digest check works.
set -eu

HERE=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
BUNDLE="$HERE/docs/evidence/compat-g3/acceptance"
GATE="$HERE/tests/test_g3_acceptance.py"
TMP=${TMPDIR:-/tmp}/g3-acceptance-nc.$$
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

# Case 1: candidate serial loses the ELF PT_LOAD receipt; manifest re-signed.
C1="$TMP/1-marker"
cp -a "$BUNDLE" "$C1"
gzip -dc "$C1/x86emu-stock-candidate.serial.log.gz" \
    | grep -v 'ELF PT_LOAD segments copied to phys' \
    | gzip -9 > "$C1/x86emu-stock-candidate.serial.log.gz.new"
mv "$C1/x86emu-stock-candidate.serial.log.gz.new" "$C1/x86emu-stock-candidate.serial.log.gz"
resign "$C1"
expect_reject "candidate log without ELF PT_LOAD marker" "$C1"

# Case 2: handoff block magic corrupted; manifest re-signed.
C2="$TMP/2-bootinfo"
cp -a "$BUNDLE" "$C2"
python3 - "$C2/bootinfo.bin" <<'PY'
import sys
path = sys.argv[1]
data = bytearray(open(path, "rb").read())
data[0] ^= 0xFF
open(path, "wb").write(data)
PY
resign "$C2"
expect_reject "corrupted boot_info_t magic" "$C2"

# Case 3: pinned and candidate serial receipts swapped; manifest re-signed.
C3="$TMP/3-swapped"
cp -a "$BUNDLE" "$C3"
cp "$BUNDLE/x86emu-stock-pinned.serial.log.gz" "$C3/x86emu-stock-candidate.serial.log.gz"
cp "$BUNDLE/x86emu-stock-candidate.serial.log.gz" "$C3/x86emu-stock-pinned.serial.log.gz"
resign "$C3"
expect_reject "swapped pinned/candidate receipts" "$C3"

# Case 4: a hashed file changes while SHA256SUMS stays stale.
C4="$TMP/4-stale-manifest"
cp -a "$BUNDLE" "$C4"
printf '\n' >> "$C4/x86emu-stock-candidate.stats.log"
expect_reject "stale SHA256SUMS manifest" "$C4"

# Case 5: replay receipt no longer matches the primary candidate run.
C5="$TMP/5-replay"
cp -a "$BUNDLE" "$C5"
gzip -dc "$C5/x86emu-stock-candidate-replay.serial.log.gz" \
    | sed 's/shell active; kmain idling\./shell active; kmain idling (tampered)./' \
    | gzip -9 > "$C5/x86emu-stock-candidate-replay.serial.log.gz.new"
mv "$C5/x86emu-stock-candidate-replay.serial.log.gz.new" \
   "$C5/x86emu-stock-candidate-replay.serial.log.gz"
resign "$C5"
expect_reject "candidate replay receipt diverges" "$C5"

echo "G3 acceptance negative control: PASS (5 mutations rejected)"
