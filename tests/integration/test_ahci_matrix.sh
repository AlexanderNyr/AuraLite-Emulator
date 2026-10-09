#!/bin/sh
# tests/integration/test_ahci_matrix.sh -- STORE S4 guest receipt harness.
#
# Mirrors the QEMU-side test_ahci_matrix.sh contract (AuraLite-OS
# tests/integration/cases/test_ahci_matrix.sh, lanes A/B/C -- lane D is the
# q35 *machine* shape and has no emulator equivalent; the emulator's own
# platform profiles are exercised by `make test`) with the emulator's
# input lane (--keys-at=prompt).  Every lane boots the frozen AuraLite-OS
# kernel/initrd to its shell, types a write+cat token round-trip on /fat,
# and greps the host log for the lane's assertions:
#
#   Lane A  onboard controller with a disk at port 0   (baseline shape)
#   Lane B  disk at port 1 only (empty port 0 skipped by enumeration)
#   Lane C  --ahci2 + one disk on each controller (multi-controller scan)
#   Lane E  no --sata at all (negative control: the unattached S1 receipt)
#
# Tokens are lower-case + digits only: the KBC injection lane sends bare
# set-1 make/break pairs with no SHIFT, so '_' cannot be typed (measured
# on the S3 script, which uses the same alphabet).
#
# Determinism: lane B (the cheap lane) runs TWICE sequentially and the two
# serial logs must be byte-identical.  Set MATRIX_DETERMINISM=1 to run
# EVERY key-injecting lane twice and byte-compare (the S5 full-matrix
# determinism gate).
#
# Usage:
#   KERNEL=/path/kernel.elf INITRD=/path/initrd.tar \
#       tests/integration/test_ahci_matrix.sh [workdir]
#
# Required env: KERNEL, INITRD.  Optional: X86EMU (default ./x86emu),
# MATRIX_DETERMINISM=1, MAX_INSTR (default 6000000000).

set -eu

X86EMU=${X86EMU:-./x86emu}
WORK=${1:-/tmp/ahci-matrix-$$}
MAX_INSTR=${MAX_INSTR:-6000000000}
: "${KERNEL:?set KERNEL to the frozen guest kernel.elf}"
: "${INITRD:?set INITRD to the frozen guest initrd.tar}"

mkdir -p "$WORK"
DISK_A="$WORK/disk-a.img"
DISK_B="$WORK/disk-b.img"
DISK_C0="$WORK/disk-c0.img"
DISK_C1="$WORK/disk-c1.img"

# 16 MiB marked images (AURALHCI apex + 55AA).  The marker is not
# decoration: the guest's DMA WRITE verification only runs on a disk
# carrying exactly "AURALHCI" + 0x55AA (drivers/ahci/ahci.c "no scratch
# marker"), and lane A asserts the resulting PASS line.
mk_marked() { # mk_marked <path> <MiB>
    dd if=/dev/zero of="$1" bs=1M count="$2" status=none
    printf 'AURALHCI' | dd of="$1" bs=1 seek=0 conv=notrunc status=none
    printf '\125\252' | dd of="$1" bs=1 seek=510 conv=notrunc status=none
}
mk_marked "$DISK_A" 16
mk_marked "$DISK_B" 16
mk_marked "$DISK_C0" 16
mk_marked "$DISK_C1" 8

# Typed script: write + cat token round-trip on /fat.  Set-1 make/break
# pairs, Enter=0x1C.  Alphabet: a-z 0-9 '/' '.' '-' ' ' (no SHIFT).
mk_keys() { # mk_keys <token> -> echoes scancode list
    tok="$1"
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
                *) echo "mk_keys: unsupported char '$ch' (no SHIFT lane)" >&2; exit 2;;
            esac
            br=$(printf '%02X' $((0x$k | 0x80)))
            out="$out$k,$br,"
        done
    }
    line() { word_scancodes "$1"; out="${out}1C,9C,"; }
    line "write /fat/matrix.txt $tok"
    line "cat /fat/matrix.txt"
    line "exit"
    printf '%s' "${out%,}"
}

fail=0
need() { # need <pattern> <file> <label>
    if ! grep -q "$1" "$2"; then echo "  MISSING: $3"; fail=1; fi
}
need_no() { # need_no <pattern> <file> <label>
    if grep -q "$1" "$2"; then echo "  UNWANTED: $3"; fail=1; fi
}

# run_typed <name> <token> -- <emu args...> : boot, inject the script at the
# guest's own "auralite#" echo, capture the full serial log to <name>.out.
run_typed() {
    name="$1"; tok="$2"; shift 2
    keys=$(mk_keys "$tok")
    "$X86EMU" --kernel="$KERNEL" --initrd="$INITRD" \
        --keys-at=prompt --keys="$keys" --max-instr="$MAX_INSTR" \
        "$@" > "$WORK/$name.out" 2>&1
}

# ---------------- Lane A: onboard controller, disk at port 0 ----------------
echo "== lane A: one controller, disk at port 0"
run_typed laneA laneaok --sata="$DISK_A"
need "keys-at fired"                                    "$WORK/laneA.out" "typed-script injection"
need "PASS: SATA read/write DMA works"                  "$WORK/laneA.out" "lane A: DMA self-test"
need "laneaok"                                          "$WORK/laneA.out" "lane A: FAT32 token round-trip"
need "fat32. PASS: FAT32 read/write"                    "$WORK/laneA.out" "lane A: fat32 format+mount"
need_no "unsupported ATA cmd"                           "$WORK/laneA.out" "lane A: unsupported ATA cmd"
need_no "\[ahci\] FAIL|\[diskfs\] FAIL|\[fat32\] FAIL" "$WORK/laneA.out" "lane A: FAIL lines"

# ---------------- Lane B: disk at port 1, port 0 empty ----------------
echo "== lane B: disk at port 1 only (empty port 0 skipped)"
run_typed laneB lanebok --sata-port1="$DISK_B"
need "keys-at fired"                                    "$WORK/laneB.out" "typed-script injection"
need "hw port 1: SATA disk"                             "$WORK/laneB.out" "lane B: disk on hw port 1"
need_no "hw port 0: SATA disk"                          "$WORK/laneB.out" "lane B: empty port 0 skipped"
need "lanebok"                                          "$WORK/laneB.out" "lane B: FAT32 works on port 1"

# Determinism pair for lane B (always on): sequential re-run, byte-compare.
run_typed laneB-rerun lanebok --sata-port1="$DISK_B"
if ! cmp -s "$WORK/laneB.out" "$WORK/laneB-rerun.out"; then
    echo "  NONDETERMINISTIC: lane B runs differ"; fail=1
fi

# ---------------- Lane C: --ahci2, one disk per controller ----------------
echo "== lane C: two controllers, one disk each"
run_typed laneC lanecok --ahci2 --sata="$DISK_C0" --sata2="$DISK_C1"
need "keys-at fired"                                    "$WORK/laneC.out" "typed-script injection"
need "controller 0 at PCI 0:31.2"                       "$WORK/laneC.out" "lane C: controller 0 bound"
need "controller 1 at PCI 0:31.3"                       "$WORK/laneC.out" "lane C: controller 1 bound"
need "2 controller.*2 SATA device.* ready"               "$WORK/laneC.out" "lane C: both controllers yield a device"
need "fat32. PASS: FAT32 read/write"                    "$WORK/laneC.out" "lane C: FAT32 on controller 0 disk"
need "ext2"                                             "$WORK/laneC.out" "lane C: ext2 takes controller 1 disk"
need "lanecok"                                          "$WORK/laneC.out" "lane C: file round-trip"

# ---------------- Lane E: no storage (negative control) ----------------
echo "== lane E: no --sata (unattached receipt)"
"$X86EMU" --kernel="$KERNEL" --initrd="$INITRD" \
    --max-instr="$MAX_INSTR" > "$WORK/laneE.out" 2>&1
need "1 controller(s), 0 SATA device(s) ready"          "$WORK/laneE.out" "lane E: unattached receipt"
need "self-test: no devices"                            "$WORK/laneE.out" "lane E: no-device self-test"
need_no "hw port"                                       "$WORK/laneE.out" "lane E: no disk enumerated"

# ---------------- Optional: full-matrix determinism (S5 gate) ----------------
if [ "${MATRIX_DETERMINISM:-0}" = "1" ]; then
    echo "== determinism x2: re-running lanes A, C, E"
    run_typed laneA-rerun laneaok --sata="$DISK_A"
    if ! cmp -s "$WORK/laneA.out" "$WORK/laneA-rerun.out"; then
        echo "  NONDETERMINISTIC: lane A runs differ"; fail=1
    fi
    run_typed laneC-rerun lanecok --ahci2 --sata="$DISK_C0" --sata2="$DISK_C1"
    if ! cmp -s "$WORK/laneC.out" "$WORK/laneC-rerun.out"; then
        echo "  NONDETERMINISTIC: lane C runs differ"; fail=1
    fi
    "$X86EMU" --kernel="$KERNEL" --initrd="$INITRD" \
        --max-instr="$MAX_INSTR" > "$WORK/laneE-rerun.out" 2>&1
    if ! cmp -s "$WORK/laneE.out" "$WORK/laneE-rerun.out"; then
        echo "  NONDETERMINISTIC: lane E runs differ"; fail=1
    fi
fi

if [ "$fail" -eq 0 ]; then
    if [ "${MATRIX_DETERMINISM:-0}" = "1" ]; then
        echo "S4/S5 matrix + full determinism receipts: ALL PASS"
    else
        echo "S4 matrix receipts: ALL PASS"
    fi
else
    echo "S4 matrix receipts: FAIL (see $WORK)"
    exit 1
fi
