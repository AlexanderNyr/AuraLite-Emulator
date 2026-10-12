#!/bin/sh
# tests/integration/n2_responder.sh -- NET N2 host-responder wire harness.
#
# N2's claim is that a frame the guest puts on the wire gets a protocol-correct
# answer back, and that the guest's own filters accept it.  That is a two-sided
# claim, so it is measured on both sides:
#
#   Lane A (wire, always runs, x2): tools/tap_responder.py holds the ground-rule
#     TAP endpoint (UDP 127.0.0.1:8080) and answers as slirp's user-mode stack
#     does; tests/nic_net.c rides the K1 kernel-load transport (--kernel=, the
#     4 GiB identity + HHDM map) and runs the frozen guest's own protocol code:
#     DHCP DISCOVER/OFFER/REQUEST/ACK, then ARP who-has for the gateway, then an
#     ICMP echo request with the guest's 32-byte 0x41..0x60 payload.  Receipts
#     required, in the guest's own wording where the guest has wording:
#       [dhcp] PASS: IP 10.0.2.15, gateway 10.0.2.2
#       [net]  ICMP payload verified (ident+seq+32 bytes)
#       [net]  PASS: lease + ARP + echo reply (payload verified)
#       N2OK
#     The wire tape must be byte-identical between the two runs.
#
#     NOTE (N3): the payload grew.  `tests/nic_net.c` now continues past the
#     ICMP echo into DNS and TCP, so lane A's tape is 76 frames rather than 4
#     and the summary marker is `N3OK`.  This lane still asserts only N2's own
#     receipts -- the frame count and the DNS/TCP tape belong to
#     `n3_net_e2e.sh` -- and the four N2 frames are still the first four on the
#     wire, unchanged.
#
#   Lane B (no device, always runs): the same payload WITHOUT --tap=.  The e1000
#     function does not exist then (ground rule 3), so STATUS reads all-ones off
#     the open bus -- LU is set, the link looks up -- and the payload proceeds
#     into a TX ring nobody services.  It must report the guest's own timeout
#     receipt rather than hang:
#       [e1000] TX timeout: desc[0].status=0x00 TDH=4294967295 TDT=4294967295
#       [dhcp] FAIL: DISCOVER transmit failed (link down or TX timeout)
#       N3FAIL   (the payload's summary marker; it was N2FAIL until N3)
#     This is the pin that keeps every historical no-NIC lane byte-stable.
#
#   Lane C (frozen guest, SKIPs without it): the real AuraLite-OS kernel + initrd
#     against the same responder.  N2's DoD receipts are the two lines the
#     guest's own integration cases assert (tests/integration/cases/test_e1000e.sh
#     :48 and test_vmxnet3.sh:49 in AuraLite-OS):
#       [dhcp] PASS: IP 10.0.2.15, gateway 10.0.2.2
#       [net] PASS: ping 10.0.2.2 successful (ICMP echo reply received)
#     Requires the frozen build; the recipe is in docs/plans/NET_PLAN.md N2.
#
#   Lane D (negative control, always runs): the responder up with
#     --drop=arp,icmp,dhcp.  Nothing is answered, so the run must degrade to the
#     first timeout and nothing further:
#       [dhcp] FAIL: no OFFER received
#       [net] FAIL: no DHCP lease
#       N3FAIL   (again: the payload's summary marker)
#     and this lane exits non-zero.  The emulator-side control for the same phase
#     (removing src/e1000.c's RX descriptor *length* writeback -- not the DD bit,
#     which the guest never checks) is documented in CHANGELOG.md and NET_PLAN.md
#     with its measured receipts; it needs a rebuilt emulator, so it stays a
#     documented manual step instead of a lane that quietly rebuilds the tree.
#
# Usage:
#   tests/integration/n2_responder.sh [workdir]
# Optional env: X86EMU (default ./x86emu), KERNEL, INITRD (lane C),
#   RESP_TIMEOUT (default 25), MAX_INSTR (default 60000000).

set -eu

ROOT=$(cd "$(dirname "$0")/../.." && pwd)
X86EMU=${X86EMU:-$ROOT/x86emu}
WORK=${1:-/tmp/n2-responder}
KERNEL=${KERNEL:-/home/user/AuraLite-OS/build/kernel.elf}
INITRD=${INITRD:-/home/user/AuraLite-OS/build/initrd.tar}
RESP_TIMEOUT=${RESP_TIMEOUT:-25}
MAX_INSTR=${MAX_INSTR:-60000000}
TAP=127.0.0.1:8080
NC_TAP=127.0.0.1:8081          # lane D binds its own port so it cannot race A
GW_MAC=52:55:0a:00:02:02

mkdir -p "$WORK"
fail=0
need()    { grep -q -e "$1" "$2"    || { echo "  MISSING: $3"; fail=1; }; }
need_no() { grep -q -e "$1" "$2" && { echo "  PRESENT: $3"; fail=1; } || :; }

command -v python3 >/dev/null 2>&1 || { echo "SKIP: python3 missing"; exit 0; }
[ -x "$X86EMU" ] || { echo "ERROR: build x86emu first (make x86emu)"; exit 1; }

# ---- the payload ----------------------------------------------------------
# Built here rather than by the Makefile so `sh tests/integration/n2_responder.sh`
# works on a fresh clone.  Flags are the frozen K1 guest-image flags: no
# sanitizers, no PIE, kernel code model, no SSE/red zone.
CC=${CC:-cc}
"$CC" -std=c11 -O1 -Wall -Wextra -Werror -ffreestanding -fno-stack-protector -fno-pie \
    -mcmodel=kernel -mno-sse -mno-sse2 -mno-mmx -mno-red-zone -nostdlib -no-pie \
    -Wl,-z,max-page-size=0x1000 -T "$ROOT/tests/nic_net.ld" \
    "$ROOT/tests/nic_net.c" -o "$WORK/nic_net.elf"

# wait until a port is actually bound, so the emulator's UDP connect() (a no-op
# against an unbound endpoint) cannot silently cost us the first frame.
# The lanes share the ground-rule port with everything else on the host, and
# SO_REUSEADDR lets two UDP sockets bind the SAME port -- in which case
# incoming datagrams are split between them and a lane silently loses replies
# (measured: a responder left over from a hand run cost lane A its whole tape,
# 3 frames in and `replies arp=0 dhcp=0 icmp=0 dns=0 tcp=0`, while the very
# next lane was perfect).  So refuse to start a lane on a busy port instead of
# reporting a failure that is really the harness's own mess.
port_free() { # port_free <port> -> 0 when nothing holds it
    python3 -c "
import socket,sys
s=socket.socket(socket.AF_INET,socket.SOCK_DGRAM)
try:
    s.bind(('127.0.0.1',$1))
except OSError:
    sys.exit(1)
s.close(); sys.exit(0)" 2>/dev/null
}

wait_bound() { # wait_bound <port>
    i=0
    while [ "$i" -lt 100 ]; do
        if python3 -c "
import socket,sys
s=socket.socket(socket.AF_INET,socket.SOCK_DGRAM)
try:
    s.bind(('127.0.0.1',$1))
except OSError:
    sys.exit(0)
s.close(); sys.exit(1)" 2>/dev/null; then return 0; fi
        i=$((i+1)); sleep 0.05
    done
    echo "  ERROR: nothing bound UDP $1"
    return 1
}

run_lane() { # run_lane <tag> <tap|none> <responder args...>
    tag=$1; mode=$2; shift 2
    if [ "$mode" != "none" ]; then
        port_free "${mode##*:}" \
            || { echo "  ERROR: UDP ${mode##*:} is already in use -- a leftover"
                 echo "         responder would split the lane's datagrams;"
                 echo "         pkill -f tools/tap_responder.py and re-run"
                 fail=1; return 1; }
        python3 "$ROOT/tools/tap_responder.py" \
            --out "$WORK/$tag-frames.hex" --timeout "$RESP_TIMEOUT" "$@" \
            > "$WORK/$tag-resp.out" 2>&1 &
        resp=$!
        wait_bound "${mode##*:}" || { fail=1; return 1; }
    fi
    if [ "$mode" = "none" ]; then
        "$X86EMU" --platform=haswell --kernel="$WORK/nic_net.elf" \
            --max-instr="$MAX_INSTR" --log="$WORK/$tag.log" \
            > "$WORK/$tag.out" 2>&1 || true
    else
        "$X86EMU" --platform=haswell --kernel="$WORK/nic_net.elf" \
            --tap="$mode" --max-instr="$MAX_INSTR" --log="$WORK/$tag.log" \
            > "$WORK/$tag.out" 2>&1 || true
        wait "$resp" || true
    fi
}

echo "== protocol vectors: tools/tap_responder.py against the guest's own filters"
python3 "$ROOT/tests/test_tap_responder.py" > "$WORK/unit.out" 2>&1 \
    || { echo "  FAIL: responder protocol vectors (see $WORK/unit.out)"; fail=1; }
need "test-tap-responder: all tests passed" "$WORK/unit.out" \
    "responder protocol vectors"

echo "== lane A: DHCP + ARP + ICMP over the real wire (x2 determinism)"
run_lane a1 "$TAP"
run_lane a2 "$TAP"
for t in a1 a2; do
    need "\[kload\] entry=0xffffffff80100000"          "$WORK/$t.log" \
        "lane $t: K1 transport reached the payload in long mode"
    need "\[dhcp\] OFFER: IP 10.0.2.15, server 10.0.2.2" "$WORK/$t.log" \
        "lane $t: OFFER accepted by net.c's filter"
    need "\[dhcp\] PASS: IP 10.0.2.15, gateway 10.0.2.2" "$WORK/$t.log" \
        "lane $t: lease receipt (the guest's own integration assert)"
    need "\[net\] ARP reply: gateway is $GW_MAC"       "$WORK/$t.log" \
        "lane $t: ARP reply accepted, gateway MAC learned"
    need "\[net\] ICMP payload verified (ident+seq+32 bytes)" "$WORK/$t.log" \
        "lane $t: echo reply payload is the request's 0x41..0x60"
    need "\[net\] PASS: lease + ARP + echo reply (payload verified)" "$WORK/$t.log" \
        "lane $t: N2 composite receipt"
    need "N3OK"                                        "$WORK/$t.log" \
        "lane $t: payload summary"
    need_no "N3FAIL"                                   "$WORK/$t.log" \
        "lane $t: payload reported failure"
    need "DHCP DISCOVER"                               "$WORK/$t-resp.out" \
        "lane $t: responder saw the DISCOVER"
    need "DHCP REQUEST"                                "$WORK/$t-resp.out" \
        "lane $t: responder saw the REQUEST"
    need "who-has 10.0.2.2 tell 10.0.2.15"             "$WORK/$t-resp.out" \
        "lane $t: responder saw the ARP request"
    need "ICMP echo request from 10.0.2.15 (id 0x1234 seq 1, 32 payload bytes)" \
        "$WORK/$t-resp.out" "lane $t: responder saw the echo request"
    # The frame COUNT is n3_net_e2e.sh's business (the payload now also runs
    # DNS and TCP); what this lane pins is that N2's four frames are still the
    # first four, byte for byte.
    head -4 "$WORK/$t-frames.hex" > "$WORK/$t-n2frames.hex"
done
cmp -s "$WORK/a1-n2frames.hex" "$WORK/a2-n2frames.hex" \
    || { echo "  NONDETERMINISTIC: lane A's first four frames differ"; fail=1; }
cmp -s "$WORK/a1.log" "$WORK/a2.log" \
    || { echo "  NONDETERMINISTIC: lane A serial tapes differ"; fail=1; }
cmp -s "$WORK/a1-frames.hex" "$WORK/a2-frames.hex" \
    || { echo "  NONDETERMINISTIC: lane A wire frames differ"; fail=1; }
cmp -s "$WORK/a1-resp.out" "$WORK/a2-resp.out" \
    || { echo "  NONDETERMINISTIC: lane A responder logs differ"; fail=1; }

echo "== lane B: no --tap= means no e1000 at all (opt-in ground rule)"
run_lane b1 none
need "\[e1000\] TX timeout: desc\[0\].status=0x00 TDH=4294967295 TDT=4294967295" \
    "$WORK/b1.log" "lane B: open-bus STATUS still reports LU, TX ring unserviced"
need "\[dhcp\] FAIL: DISCOVER transmit failed" "$WORK/b1.log" \
    "lane B: the payload reports instead of hanging"
need "N3FAIL"    "$WORK/b1.log" "lane B: payload summary"
need_no "N3OK"   "$WORK/b1.log" "lane B: payload claimed success without a NIC"
need_no "\[dhcp\] PASS" "$WORK/b1.log" "lane B: no lease without a device"

echo "== lane C: frozen-guest receipts (the DoD as the guest prints it)"
if [ -r "$KERNEL" ] && [ -r "$INITRD" ]; then
    python3 "$ROOT/tools/tap_responder.py" \
        --out "$WORK/c1-frames.hex" --timeout "$RESP_TIMEOUT" \
        > "$WORK/c1-resp.out" 2>&1 &
    "$X86EMU" --platform=haswell --kernel="$KERNEL" --initrd="$INITRD" \
        --tap="$TAP" --log="$WORK/c1.log" \
        --max-instr=4500000000 > "$WORK/c1.out" 2>&1 || true
    wait || true
    need "\[net\] using NIC: e1000"                "$WORK/c1.log" "lane C: NIC bound"
    need "\[dhcp\] PASS: IP 10.0.2.15, gateway 10.0.2.2" "$WORK/c1.log" \
        "lane C: lease receipt"
    need "\[net\] PASS: ping 10.0.2.2 successful (ICMP echo reply received)" \
        "$WORK/c1.log" "lane C: self-test ping receipt"
    need_no "\[e1000\] TX timeout"                 "$WORK/c1.log" "lane C: D2 receipt gone"
else
    echo "SKIP: frozen guest kernel/initrd missing ($KERNEL / $INITRD)"
    echo "      reproduce with:"
    echo "        KERNEL=$KERNEL INITRD=$INITRD $0 $WORK"
fi

echo "== lane D: negative control -- responder answers nothing (--drop)"
run_lane d1 "$NC_TAP" --port 8081 --drop=arp,icmp,dhcp
need "\[dhcp\] DISCOVER sent"        "$WORK/d1.log" "lane D: the request still went out"
need "\[dhcp\] FAIL: no OFFER received" "$WORK/d1.log" "lane D: first timeout receipt"
need "\[net\] FAIL: no DHCP lease"   "$WORK/d1.log" "lane D: no lease"
need "N3FAIL"                        "$WORK/d1.log" "lane D: payload summary"
need_no "N3OK"                       "$WORK/d1.log" "lane D: payload claimed success"
need_no "\[dhcp\] PASS"              "$WORK/d1.log" "lane D: no lease receipt"
need_no "ARP reply: gateway is"      "$WORK/d1.log" "lane D: never reached ARP"
n=$(wc -l < "$WORK/d1-frames.hex")
[ "$n" -eq 1 ] || { echo "  WIRE: lane D saw $n frames, want exactly 1 DISCOVER"; fail=1; }
need "replies arp=0 dhcp=0 icmp=0 dns=0" "$WORK/d1-resp.out" \
    "lane D: responder withheld every reply"

if [ "$fail" -eq 0 ]; then
    echo "N2 responder receipts: ALL PASS"
else
    echo "N2 responder receipts: FAIL (see $WORK)"
    exit 1
fi
