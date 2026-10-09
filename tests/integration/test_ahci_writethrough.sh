#!/bin/sh
# tests/integration/test_ahci_writethrough.sh -- STORE S5 guest receipt.
#
# One lane, two policies, same guest actions:
#
#   Run 1  default copy-on-attach (the S3 policy): the guest formats
#          diskfs/FAT32 and writes a typed token to /fat -- the HOST image
#          must stay byte-pristine (the S3 harness's own assertion shape).
#   Run 2  --sata-writethrough: the same typed write must land in the host
#          image file (the token bytes are visible host-side after the run).
#
# Token alphabet is the no-SHIFT set-1 pair convention (a-z 0-9 / . - ' ').
#
# Usage:
#   KERNEL=/path/kernel.elf INITRD=/path/initrd.tar \
#       tests/integration/test_ahci_writethrough.sh [workdir]
#
# Required env: KERNEL, INITRD.  Optional: X86EMU (default ./x86emu),
# MAX_INSTR (default 3200000000).

set -eu

X86EMU=${X86EMU:-./x86emu}
WORK=${1:-/tmp/ahci-wt-$$}
MAX_INSTR=${MAX_INSTR:-3200000000}
: "${KERNEL:?set KERNEL to the frozen guest kernel.elf}"
: "${INITRD:?set INITRD to the frozen guest initrd.tar}"

mkdir -p "$WORK"
TOK=writethrok

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
    line "write /fat/wt.txt $TOK"
    line "cat /fat/wt.txt"
    line "exit"
    printf '%s' "${out%,}"
}
KEYS=$(mk_keys)

fail=0
need()   { if ! grep -q "$1" "$2"; then echo "  MISSING: $3"; fail=1; fi; }

mk_disk() { # mk_disk <path>
    dd if=/dev/zero of="$1" bs=1M count=16 status=none
    printf 'AURALHCI' | dd of="$1" bs=1 seek=0 conv=notrunc status=none
    printf '\125\252' | dd of="$1" bs=1 seek=510 conv=notrunc status=none
}

run_one() { # run_one <name> <img> [extra args...]
    name="$1"; img="$2"; shift 2
    "$X86EMU" --kernel="$KERNEL" --initrd="$INITRD" --sata="$img" \
        --keys-at=prompt --keys="$KEYS" --max-instr="$MAX_INSTR" \
        "$@" > "$WORK/$name.out" 2>&1
}

echo "== run 1: copy-on-attach (host stays pristine)"
mk_disk "$WORK/disk-pristine.img"
run_one pristine "$WORK/disk-pristine.img"
need "$TOK" "$WORK/pristine.out" "token round-trip (copy-on-attach)"
python3 - "$WORK/disk-pristine.img" "$TOK" <<'PY' || fail=1
import sys
img = open(sys.argv[1], 'rb').read()
tok = sys.argv[2].encode()
assert img[0:8] == b'AURALHCI' and img[510:512] == b'\x55\xaa', 'apex lost'
assert tok not in img, 'copy-on-attach leaked guest writes into the host image'
print('  ok: host image byte-pristine after guest writes')
PY

echo "== run 2: --sata-writethrough (host image moves)"
mk_disk "$WORK/disk-wt.img"
run_one writethrough "$WORK/disk-wt.img" --sata-writethrough
need "$TOK" "$WORK/writethrough.out" "token round-trip (writethrough)"
need "writethrough ->" "$WORK/writethrough.out" "writethrough attach receipt"
python3 - "$WORK/disk-wt.img" "$TOK" <<'PY' || fail=1
import sys
img = open(sys.argv[1], 'rb').read()
tok = sys.argv[2].encode()
assert tok in img, 'writethrough did not move guest writes into the host image'
print('  ok: host image carries the guest token')
PY

if [ "$fail" -eq 0 ]; then
    echo "S5 writethrough receipts: ALL PASS"
else
    echo "S5 writethrough receipts: FAIL (see $WORK)"
    exit 1
fi
