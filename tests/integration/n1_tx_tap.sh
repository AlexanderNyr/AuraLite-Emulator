#!/bin/sh
# tests/integration/n1_tx_tap.sh -- NET N1 wire + guest receipt harness.
#
# N1's claim is that a frame the guest submits to the TX ring actually reaches
# the wire, and that the descriptor is completed inside the doorbell store so
# the guest's e1000_send() DD spin-wait retires.  Three lanes measure that:
#
#   Lane A (wire, always runs): the real firmware-boot transport loads
#     tests/nic_tx.asm off LBA 0 of a throwaway --disk= image and jumps to it
#     in long mode; the payload programs the TX ring exactly the way the
#     frozen guest driver does, rings the TDT doorbell and reports on COM1.
#     n1_tap_responder.py holds the ground-rule endpoint (UDP 127.0.0.1:8080)
#     and records what arrived.  Receipts required:
#       guest   [serial] N1TXOK  status=01 tdh=01 icr=81
#       wire    ffffffffffff52540012345608004e3154584f4b   (one frame, exact)
#     status=01 is DD in the status BYTE (descriptor offset 12), tdh=01 is the
#     head advance, icr=81 is TXDW|TXQE.  Run twice: both .out tapes and both
#     frame logs must be byte-identical (determinism pair).
#
#   Lane B (opt-in, always runs): the same payload WITHOUT --tap=.  The e1000
#     PCI function does not exist then (ground rule 3), so BAR0 reads hit the
#     open bus and return all-ones (measured: status=00 tdh=ff icr=ff), and the
#     payload must report failure.  This is the pin that keeps every historical
#     no-NIC lane byte-stable.
#
#   Lane C (guest receipts, SKIPs without the frozen guest): the N0 lane
#     re-run.  N1's DoD is that `[e1000] TX timeout` disappears and `[dhcp]`
#     reaches the wire, with the rest of the tape unchanged modulo those TX
#     receipts.  Requires the frozen AuraLite-OS kernel/initrd (the recipe is
#     recorded in docs/plans/NET_PLAN.md N0/N1).
#
# Usage:
#   tests/integration/n1_tx_tap.sh [workdir]
# Optional env: X86EMU (default ./x86emu), KERNEL, INITRD (lane C),
#   RESP_TIMEOUT (default 20).

set -eu

ROOT=$(cd "$(dirname "$0")/../.." && pwd)
X86EMU=${X86EMU:-$ROOT/x86emu}
WORK=${1:-/tmp/n1-tx-tap}   # fixed default: the tape embeds the --disk= path, so a
                             # stable workdir is what makes the recorded shas reproducible
KERNEL=${KERNEL:-/home/user/AuraLite-OS/build/kernel.elf}
INITRD=${INITRD:-/home/user/AuraLite-OS/build/initrd.tar}
RESP_TIMEOUT=${RESP_TIMEOUT:-20}
TAP=127.0.0.1:8080
FRAME_HEX=ffffffffffff52540012345608004e3154584f4b

mkdir -p "$WORK"
fail=0
need()    { grep -q "$1" "$2"    || { echo "  MISSING: $3"; fail=1; }; }
need_no() { grep -q "$1" "$2" && { echo "  PRESENT: $3"; fail=1; } || :; }

command -v nasm    >/dev/null 2>&1 || { echo "SKIP: nasm missing"; exit 0; }
command -v python3 >/dev/null 2>&1 || { echo "SKIP: python3 missing"; exit 0; }
[ -x "$X86EMU" ] || { echo "ERROR: build x86emu first (make x86emu)"; exit 1; }

# ---- the payload and its throwaway disk image -----------------------------
nasm -f bin "$ROOT/tests/nic_tx.asm" -o "$WORK/nic_tx.bin"
python3 - "$WORK/nic_tx.bin" "$WORK/disk.img" <<'EOF' || exit 1
import sys
p = open(sys.argv[1], 'rb').read()
assert len(p) <= 512, 'payload must fit the firmware loader single block (%d)' % len(p)
open(sys.argv[2], 'wb').write(p + b'\0' * (1024 * 1024 - len(p)))
EOF

# ---- one lane: responder up first (UDP connect() is a no-op, so an unbound
# ---- endpoint would silently cost us the frame), then the emulator.
run_tx_lane() { # run_tx_lane <tag> <extra emu args...>
    tag=$1; shift
    python3 "$ROOT/tests/integration/n1_tap_responder.py" \
        --out "$WORK/$tag-frames.hex" --count 1 --timeout "$RESP_TIMEOUT" \
        > "$WORK/$tag-resp.out" 2>&1 &
    resp=$!
    # wait until the endpoint is actually bound
    i=0
    while [ $i -lt 100 ]; do
        if python3 -c "
import socket,sys
s=socket.socket(socket.AF_INET,socket.SOCK_DGRAM)
try:
    s.bind(('127.0.0.1',8080))
except OSError:
    sys.exit(0)
s.close(); sys.exit(1)" 2>/dev/null; then break; fi
        i=$((i+1)); sleep 0.05
    done
    "$X86EMU" --platform=haswell --disk="$WORK/disk.img" "$@" \
        --max-instr=600000 > "$WORK/$tag.out" 2>&1
    wait "$resp" || true
}

echo "== lane A: TX through the real firmware-boot transport (x2 determinism)"
run_tx_lane a1 --tap="$TAP"
run_tx_lane a2 --tap="$TAP"
for t in a1 a2; do
    need "N1TXOK  status=01 tdh=01 icr=81" "$WORK/$t.out" \
        "lane $t: DD + TDH + TXDW/TXQE receipt"
    need "\[guest\] conventional entry reached at 0x00100000" "$WORK/$t.out" \
        "lane $t: firmware transport reached the payload"
    if [ "$(cat "$WORK/$t-frames.hex" 2>/dev/null)" != "$FRAME_HEX" ]; then
        echo "  WIRE: lane $t frame mismatch:"
        echo "    want $FRAME_HEX"
        echo "    got  $(cat "$WORK/$t-frames.hex" 2>/dev/null)"
        fail=1
    fi
done
cmp -s "$WORK/a1.out" "$WORK/a2.out" \
    || { echo "  NONDETERMINISTIC: lane A tapes differ"; fail=1; }
cmp -s "$WORK/a1-frames.hex" "$WORK/a2-frames.hex" \
    || { echo "  NONDETERMINISTIC: lane A wire frames differ"; fail=1; }

echo "== lane B: no --tap= means no e1000 at all (opt-in ground rule)"
"$X86EMU" --platform=haswell --disk="$WORK/disk.img" \
    --max-instr=600000 > "$WORK/b1.out" 2>&1
need "N1TXFAIL  status=00 tdh=ff icr=ff" "$WORK/b1.out" \
    "lane B: BAR0 reads open-bus 0xFF without the device"
need "\[guest\] conventional entry reached at 0x00100000" "$WORK/b1.out" \
    "lane B: the same transport still boots"
need_no "\[e1000\]" "$WORK/b1.out" "lane B: no e1000 device lines"

echo "== lane C: frozen-guest receipts (the N0 lane re-run)"
if [ -r "$KERNEL" ] && [ -r "$INITRD" ]; then
    python3 "$ROOT/tests/integration/n1_tap_responder.py" \
        --out "$WORK/c1-frames.hex" --count 1 --timeout "$RESP_TIMEOUT" \
        > "$WORK/c1-resp.out" 2>&1 &
    "$X86EMU" --platform=haswell --kernel="$KERNEL" --initrd="$INITRD" \
        --tap="$TAP" --log="$WORK/c1.log" \
        --max-instr=4500000000 > "$WORK/c1.out" 2>&1 || true
    wait || true
    need "\[net\] using NIC: e1000"                "$WORK/c1.log" "lane C: NIC bound"
    need "\[dhcp\] starting DHCP discovery"        "$WORK/c1.log" "lane C: DHCP ran"
    need_no "\[e1000\] TX timeout"                 "$WORK/c1.log" "lane C: D2 receipt gone"
    [ -s "$WORK/c1-frames.hex" ] \
        || { echo "  MISSING: lane C put nothing on the wire"; fail=1; }
else
    echo "SKIP: frozen guest kernel/initrd missing ($KERNEL / $INITRD)"
    echo "      reproduce with:"
    echo "        KERNEL=$KERNEL INITRD=$INITRD $0 $WORK"
fi

if [ "$fail" -eq 0 ]; then
    echo "N1 TX wire receipts: ALL PASS"
else
    echo "N1 TX wire receipts: FAIL (see $WORK)"
    exit 1
fi
