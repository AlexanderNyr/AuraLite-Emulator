#!/bin/sh
# tests/integration/test_ahci_large_read.sh -- STORE S5 guest receipt harness.
#
# Mirrors the QEMU-side test_ahci_large_read.sh contract (AuraLite-OS
# tests/integration/cases/test_ahci_large_read.sh) with the emulator's
# input lane: a purpose-built FAT32 disk carrying one 16 MiB file of a
# known size, read back to EOF through the AHCI 128-KiB bounce path.
#
# The disk layout is not arbitrary: the kernel mounts whatever valid FAT32
# BPB sits at LBA 64.  An image formatted at offset 0 has no signature
# there, so the kernel decides the disk is blank and FORMATS it -- destroying
# the very file under test.  Hence the 64 zero sectors in front (the QEMU
# gate's own lesson, quoted verbatim).
#
# The 16 MiB size is MEASURED on the QEMU side, not guessed (the regression
# this gate exists for -- a per-transfer bounce leak -- survived a 1 MiB
# payload); the emulator's own S5 unit vectors cover the 128-KiB transfer
# shape directly, and this lane proves the guest-visible end-to-end read.
#
# Assertions:
#   * the guest mounts the test volume WITHOUT reformatting it,
#   * /apps/filesize reads exactly 16777216 bytes to EOF,
#   * no truncation at the original leak ceiling, no read error, no
#     exception/panic.
#
# Usage:
#   KERNEL=/path/kernel.elf INITRD=/path/initrd.tar \
#       tests/integration/test_ahci_large_read.sh [workdir]
#
# Required env: KERNEL, INITRD.  Optional: X86EMU (default ./x86emu),
# MAX_INSTR (default 6000000000), LARGE_DETERMINISM=0 to skip the x2 pair.
# Needs mtools (mformat/mcopy).

set -eu

X86EMU=${X86EMU:-./x86emu}
WORK=${1:-/tmp/ahci-large-$$}
MAX_INSTR=${MAX_INSTR:-6000000000}
: "${KERNEL:?set KERNEL to the frozen guest kernel.elf}"
: "${INITRD:?set INITRD to the frozen guest initrd.tar}"

command -v mformat >/dev/null || { echo "mformat (mtools) required"; exit 2; }

mkdir -p "$WORK"
DISK="$WORK/ahci_read_test.img"
PART="$WORK/ahci_read_part.img"
PAYLOAD="$WORK/ahci_read_payload.bin"

PAYLOAD_BYTES=$((16 * 1024 * 1024))

rm -f "$DISK" "$PART" "$PAYLOAD"

# Deterministic contents, so a corrupt read is a size mismatch rather than
# an accident of whatever was in /dev/urandom.
dd if=/dev/zero bs=1M count=16 2>/dev/null | tr '\0' 'A' > "$PAYLOAD"

dd if=/dev/zero of="$PART" bs=1M count=48 2>/dev/null
mformat -i "$PART" -F -h 32 -s 32 -t 96 ::
mcopy -i "$PART" "$PAYLOAD" ::/payload.bin

# 64 zero sectors, then the filesystem -- see the header comment.
dd if=/dev/zero of="$DISK" bs=512 count=64 2>/dev/null
cat "$PART" >> "$DISK"

# Fail early and clearly if the layout is wrong, rather than letting the
# guest silently reformat the disk and report a confusing 0 bytes.
python3 - "$DISK" <<'PY'
import sys
d = open(sys.argv[1], 'rb').read()
bpb = d[64 * 512:65 * 512]
assert bpb[510] == 0x55 and bpb[511] == 0xAA, "no 0x55AA at LBA 64"
assert bpb[82:87] == b'FAT32', "no FAT32 signature at LBA 64"
PY

# Typed script: set-1 make/break pairs, no SHIFT (alphabet a-z 0-9 / . - ' '),
# Enter=0x1C.  "run /apps/filesize /fat/payload.bin" then "exit".
mk_keys() {
    out=""
    word_scancodes() {
        w="$1"
        while [ -n "$w" ]; do
            ch="${w%"${w#?}"}"; w="${w#?}"
            case "$ch" in
                a) k=1E;; b) k=30;; c) k=2E;; d) k=20;; e) k=12;; f) k=21;;
                g) k=22;; h) k=23;; i) k=17;; j) k=24;; k) k=25;; l) k=26;;
                m) k=32;; n) k=31;; o) k=18;; p) k=19;; q) k=10;; r) k=13;;
                s) k=1F;; t) k=14;; u) k=16;; v) k=2F;; w) k=11;; x) k=2D;;
                y) k=15;; z) k=2C;;
                0) k=0B;; 1) k=02;; 2) k=03;; 3) k=04;; 4) k=05;;
                5) k=06;; 6) k=07;; 7) k=08;; 8) k=09;; 9) k=0A;;
                /) k=35;; .) k=34;; -) k=0C;; ' ') k=39;;
                *) echo "mk_keys: unsupported char '$ch'" >&2; exit 2;;
            esac
            br=$(printf '%02X' $((0x$k | 0x80)))
            out="$out$k,$br,"
        done
    }
    line() { word_scancodes "$1"; out="${out}1C,9C,"; }
    line "run /apps/filesize /fat/payload.bin"
    line "exit"
    printf '%s' "${out%,}"
}

KEYS=$(mk_keys)

run_lane() { # run_lane <name>
    "$X86EMU" --kernel="$KERNEL" --initrd="$INITRD" --sata="$DISK" \
        --keys-at=prompt --keys="$KEYS" --max-instr="$MAX_INSTR" \
        > "$WORK/$1.out" 2>&1
}

echo "== large read: 16 MiB file through the 128-KiB bounce path"
run_lane large

fail=0
need() {
    if ! grep -q "$1" "$2"; then echo "  MISSING: $3"; fail=1; fi
}
need_no() {
    if grep -q "$1" "$2"; then echo "  UNWANTED: $3"; fail=1; fi
}

need "fat32. mounted FAT32 at /fat"                    "$WORK/large.out" "volume mounted"
need_no "fat32. formatting default FAT32 volume"        "$WORK/large.out" "kernel did not reformat the test disk"
need "FILESIZE /fat/payload.bin $PAYLOAD_BYTES"         "$WORK/large.out" "read back all $PAYLOAD_BYTES bytes"
need_no "FILESIZE /fat/payload.bin 173824"              "$WORK/large.out" "not truncated at the DMA-leak ceiling"
need_no "FILESIZE-READ-ERROR"                           "$WORK/large.out" "no read error"
need_no "FILESIZE-OPEN-FAIL"                            "$WORK/large.out" "file opened"
need_no "UNHANDLED EXCEPTION"                           "$WORK/large.out" "no exception"
need_no "PANIC"                                         "$WORK/large.out" "no panic"

# Determinism pair (S5): a second sequential run must be byte-identical.
if [ "${LARGE_DETERMINISM:-1}" = "1" ]; then
    echo "== determinism: re-running the large-read lane"
    run_lane large-rerun
    if ! cmp -s "$WORK/large.out" "$WORK/large-rerun.out"; then
        echo "  NONDETERMINISTIC: large-read runs differ"; fail=1
    fi
fi

rm -f "$DISK" "$PART" "$PAYLOAD"

if [ "$fail" -eq 0 ]; then
    echo "S5 large-read receipts: ALL PASS"
else
    echo "S5 large-read receipts: FAIL (see $WORK)"
    exit 1
fi
