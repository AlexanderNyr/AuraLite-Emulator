#!/bin/sh
# tests/integration/test_ahci_rw.sh -- STORE S3 guest receipt harness.
#
# Mirrors the QEMU-side test_ahci_rw.sh contract with the emulator's own
# input lane (--keys-at=prompt).  Two lanes over a 16 MiB --sata= image,
# both driven against the frozen AuraLite-OS kernel/initrd (see the phase
# receipts in docs/plans/STORE_PLAN.md S3):
#
#   Lane A (marked image: AURALHCI apex + 55AA at LBA0):
#     * guest self-test must print  PASS: SATA read/write DMA works
#       (scratch-sector write -> verify -> restore through the 0x35 path),
#     * diskfs (AUFS at LBA2) + FAT32 (LBA64) auto-format + mount clean,
#     * typed `write /disk/ci.txt <tok1>` + `cat` round-trip,
#     * typed `write /fat/ci.txt <tok2>` + `cat` round-trip,
#     * zero `[ahci|diskfs|fat32] FAIL` / zero `unsupported ATA cmd`.
#   Lane B (blank image), run TWICE sequentially:
#     * self-test prints the blank-LBA0 line "write verification skipped",
#     * same format + mount + PASS lines,
#     * the two full serial logs must be BYTE-IDENTICAL (determinism).
#
# Host-side persistence policy (S3 measured fact): --sata= is a
# copy-on-attach in-memory image; the guest format+tokens never touch the
# host file.  Writethrough policy is S5 scope (see STORE_PLAN.md).
#
# Usage:
#   KERNEL=/path/kernel.elf INITRD=/path/initrd.tar \
#       tests/integration/test_ahci_rw.sh [workdir]
#
# Required env: KERNEL, INITRD.  Optional: X86EMU (default ./x86emu).

set -eu

X86EMU=${X86EMU:-./x86emu}
WORK=${1:-/tmp/ahci-rw-$$}
: "${KERNEL:?set KERNEL to the frozen guest kernel.elf}"
: "${INITRD:?set INITRD to the frozen guest initrd.tar}"

mkdir -p "$WORK"
MARKED="$WORK/disk-marked.img"
BLANK="$WORK/disk-blank.img"

# 16 MiB blank image; marked = AURALHCI apex + 55AA boot signature.
dd if=/dev/zero of="$BLANK" bs=1M count=16 status=none
cp "$BLANK" "$MARKED"
printf 'AURALHCI' | dd of="$MARKED" bs=1 seek=0 conv=notrunc status=none
printf '\125\252' | dd of="$MARKED" bs=1 seek=510 conv=notrunc status=none

# Typed script: write+cat token round-trips on /disk and /fat.  Set-1
# make/break pairs, lower-case only (no SHIFT scancodes), Enter=0x1C.
KEYS="11,91,13,93,17,97,14,94,12,92,39,B9,35,B5,20,A0,17,97,1F,9F,25,A5,35,B5,2E,AE,17,97,34,B4,14,94,2D,AD,14,94,39,B9,1F,9F,04,84,20,A0,17,97,1F,9F,25,A5,02,82,18,98,25,A5,1C,9C,2E,AE,1E,9E,14,94,39,B9,35,B5,20,A0,17,97,1F,9F,25,A5,35,B5,2E,AE,17,97,34,B4,14,94,2D,AD,14,94,1C,9C,11,91,13,93,17,97,14,94,12,92,39,B9,35,B5,21,A1,1E,9E,14,94,35,B5,2E,AE,17,97,34,B4,14,94,2D,AD,14,94,39,B9,1F,9F,04,84,21,A1,1E,9E,14,94,03,83,03,83,18,98,25,A5,1C,9C,2E,AE,1E,9E,14,94,39,B9,35,B5,21,A1,1E,9E,14,94,35,B5,2E,AE,17,97,34,B4,14,94,2D,AD,14,94,1C,9C"

echo "== lane A: marked image + typed token round-trips"
"$X86EMU" --kernel="$KERNEL" --initrd="$INITRD" --sata="$MARKED" \
    --keys-at=prompt --keys="$KEYS" --max-instr=6000000000 \
    > "$WORK/laneA.out" 2>&1

fail=0
need() {  # need <pattern> <file> <label>
    if ! grep -q "$1" "$2"; then echo "  MISSING: $3"; fail=1; fi
}
need "PASS: SATA read/write DMA works"               "$WORK/laneA.out" "guest RW-DMA self-test"
need "keys-at fired"                                  "$WORK/laneA.out" "typed-script injection"
need "s3disk1ok"                                      "$WORK/laneA.out" "/disk token round-trip"
need "s3fat22ok"                                      "$WORK/laneA.out" "/fat token round-trip"
need "diskfs. PASS: persistent"                       "$WORK/laneA.out" "diskfs format+mount"
need "fat32. PASS: FAT32 read/write"                  "$WORK/laneA.out" "fat32 format+mount"

echo "== lane B: blank image x2 (sequential determinism pair)"
"$X86EMU" --kernel="$KERNEL" --initrd="$INITRD" --sata="$BLANK" \
    --max-instr=4500000000 > "$WORK/laneB1.out" 2>&1
"$X86EMU" --kernel="$KERNEL" --initrd="$INITRD" --sata="$BLANK" \
    --max-instr=4500000000 > "$WORK/laneB2.out" 2>&1

need "write verification skipped"                     "$WORK/laneB1.out" "blank-LBA0 self-test"
need "diskfs. PASS: persistent"                       "$WORK/laneB1.out" "blank diskfs format"
need "fat32. PASS: FAT32 read/write"                  "$WORK/laneB1.out" "blank fat32 format"

for f in laneA.out laneB1.out; do
    if grep -qE "unsupported ATA cmd|\[ahci\] FAIL|\[diskfs\] FAIL|\[fat32\] FAIL" "$WORK/$f"; then
        echo "  DIRTY: $f contains FAIL/unsupported lines"; fail=1
    fi
done

if ! cmp -s "$WORK/laneB1.out" "$WORK/laneB2.out"; then
    echo "  NONDETERMINISTIC: lane B runs differ"; fail=1
fi

# In-memory persistence: the host image must be byte-pristine after lane A.
python3 - "$MARKED" <<'EOF' || fail=1
import sys
img = open(sys.argv[1], 'rb').read()
assert img[0:8] == b'AURALHCI' and img[510:512] == b'\x55\xaa', 'marked apex lost'
assert not any(img[1024:1032]), 'guest format reached the host image (LBA2)'
assert not any(img[64*512:64*512+16]), 'guest format reached the host image (LBA64)'
EOF

if [ "$fail" -eq 0 ]; then
    echo "S3 guest receipts: ALL PASS"
else
    echo "S3 guest receipts: FAIL (see $WORK)"
    exit 1
fi
