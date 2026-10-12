#!/bin/sh
# tests/integration/n3_net_e2e.sh -- NET N3 end-to-end close-out harness.
#
# N3's claim is that the responder's DNS and TCP carriages let the guest run the
# online self-tests it actually runs at boot, and that the whole NET lane is
# deterministic end to end.  `kernel/kernel.c:428-430` calls, in this order and
# only when DHCP succeeded:
#
#     net_self_test()      -- ping the gateway              (N2 closed this)
#     net_dns_self_test()  -- resolve 'example.com'         (N3)
#     tcp_x5_self_test()   -- 16 concurrent TCP connections (N3)
#
# Lanes:
#
#   Lane A (wire, always runs, x2): the full tape.  `tests/nic_net.c` runs
#     DHCP -> ARP -> ICMP echo -> ARP for 10.0.2.3 -> a DNS query over UDP ->
#     the same query over the RFC 1035 TCP carriage -> 16 concurrent TCP
#     connections to 10.0.2.3:53, opened in sequence, held, then closed.
#     Receipts required:
#       [dns] PASS: 'example.com' -> 93.184.216.34
#       [dns] PASS: 'example.com' -> 93.184.216.34 over TCP (45-byte message, prefix ok)
#       [tcp-x5] PASS: 16 concurrent connections held and closed cleanly
#       [net] PASS: DNS over UDP + DNS over TCP + 16 concurrent TCP connections
#       N3OK
#     and exactly 76 frames on the wire, byte-identical between the two runs.
#     The count is asserted here rather than in n2_responder.sh because N2's
#     four frames are only the beginning of this tape; that harness pins the
#     first four and leaves the total to this one.
#
#   Lane B (no device, always runs): the same payload WITHOUT --tap=.  N2's pin,
#     re-measured on the longer payload: it must still report the guest's own TX
#     timeout rather than hang, and must not claim any of the N3 receipts.
#
#   Lane C (frozen guest, SKIPs without it): the DoD as the real guest prints
#     it -- `[dns] PASS: example.com -> a.b.c.d (cache verified)` from
#     `dns_self_test`, `[tcp-x5] PASS: 16 concurrent connections held; table
#     full is diagnosed, not fatal`, and `UDPTEST PASS: example.com A .* from
#     10.0.2.3:53` from the guest's `udptest` app, which is what
#     `tests/integration/cases/test_udp_sockets.sh:25` asserts.
#
#   Lane D (negative control, always runs): `--no-tcp`.  The responder answers
#     ARP, DHCP, ICMP and DNS-over-UDP exactly as before and ignores every TCP
#     segment, so the DNS leg still passes and the TCP legs must fail on the
#     guest's own retry ladder:
#       [dns] PASS: 'example.com' -> 93.184.216.34
#       [tcp] RTO waiting for SYN-ACK, retransmitting SYN     (x3)
#       [tcp] timeout waiting for SYN-ACK
#       [net] FAIL: no DNS answer over the TCP carriage
#       N3FAIL
#     with exactly 10 frames on the wire (6 from N2 + the DNS ARP + the DNS
#     query + 4 SYNs) and `tcp=0` in the responder's summary.  `--drop=dns` is
#     NOT a substitute: it would leave the handshakes succeeding and hang the
#     query instead of exercising the connection table.
#
# Usage:
#   tests/integration/n3_net_e2e.sh [workdir]
# Optional env: X86EMU (default ./x86emu), KERNEL, INITRD (lane C),
#   RESP_TIMEOUT (default 60), MAX_INSTR (default 200000000).

set -eu

ROOT=$(cd "$(dirname "$0")/../.." && pwd)
X86EMU=${X86EMU:-$ROOT/x86emu}
WORK=${1:-/tmp/n3-net-e2e}
KERNEL=${KERNEL:-/home/user/AuraLite-OS/build/kernel.elf}
INITRD=${INITRD:-/home/user/AuraLite-OS/build/initrd.tar}
RESP_TIMEOUT=${RESP_TIMEOUT:-60}
MAX_INSTR=${MAX_INSTR:-200000000}
TAP=127.0.0.1:8080
NC_TAP=127.0.0.1:8082
GW_MAC=52:55:0a:00:02:02
DNS_ANSWER=93.184.216.34
# Measured composition of the 76 frames lane A puts on the wire: ARP 2
# (gateway, then the DNS server) + DHCP 2 + ICMP 1 + DNS/UDP 1 + SYN 17 (one
# for the TCP carriage, sixteen for the X5 gate) + bare ACK 34 (two per
# connection: the handshake ACK and the closing ACK) + FIN|ACK 17 (one per
# connection) + ACK|PSH 2 (the DNS-over-TCP length prefix and its answer).
FULL_FRAMES=76
# Lane D: N2's four, plus the DNS ARP and query (both still work -- the lever
# is TCP-only), plus four SYNs (the original and TCP_MAX_RETRIES retransmits).
NC_FRAMES=10

mkdir -p "$WORK"
fail=0
need()    { grep -q -e "$1" "$2"    || { echo "  MISSING: $3"; fail=1; }; }
need_no() { grep -q -e "$1" "$2" && { echo "  PRESENT: $3"; fail=1; } || :; }
counteq() { # counteq <file> <want> <label>
    n=$(wc -l < "$1")
    [ "$n" -eq "$2" ] || { echo "  COUNT: $3 -- got $n, want $2"; fail=1; }
}

command -v python3 >/dev/null 2>&1 || { echo "SKIP: python3 missing"; exit 0; }
[ -x "$X86EMU" ] || { echo "ERROR: build x86emu first (make x86emu)"; exit 1; }

CC=${CC:-cc}
"$CC" -std=c11 -O1 -Wall -Wextra -Werror -ffreestanding -fno-stack-protector -fno-pie \
    -mcmodel=kernel -mno-sse -mno-sse2 -mno-mmx -mno-red-zone -nostdlib -no-pie \
    -Wl,-z,max-page-size=0x1000 -T "$ROOT/tests/nic_net.ld" \
    "$ROOT/tests/nic_net.c" -o "$WORK/nic_net.elf"

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

echo "== protocol vectors: DNS + TCP against the guest's own parsers"
python3 "$ROOT/tests/test_tap_responder.py" > "$WORK/unit.out" 2>&1 \
    || { echo "  FAIL: responder protocol vectors (see $WORK/unit.out)"; fail=1; }
need "test-tap-responder: all tests passed" "$WORK/unit.out" \
    "responder protocol vectors"
for v in test_dns_query_answer test_dns_over_tcp test_tcp_open_and_close \
         test_tcp_sixteen_concurrent test_tcp_behaviours \
         test_arp_for_the_whole_subnet; do
    need "--- $v" "$WORK/unit.out" "vector $v ran"
done

echo "== lane A: DHCP + ARP + ICMP + DNS + TCP end to end (x2 determinism)"
run_lane a1 "$TAP"
run_lane a2 "$TAP"
for t in a1 a2; do
    need "\[dhcp\] PASS: IP 10.0.2.15, gateway 10.0.2.2" "$WORK/$t.log" \
        "lane $t: the N2 lease receipt is still there"
    need "\[net\] PASS: lease + ARP + echo reply (payload verified)" "$WORK/$t.log" \
        "lane $t: the N2 composite receipt is still there"
    need "\[net\] ARP request sent for 10.0.2.3" "$WORK/$t.log" \
        "lane $t: the DNS server was ARPed for (on-link, so not routed)"
    need "\[net\] ARP reply: 10.0.2.3 is at $GW_MAC" "$WORK/$t.log" \
        "lane $t: the responder answered who-has for a non-gateway address"
    need "\[dns\] cache MISS 'example.com'" "$WORK/$t.log" \
        "lane $t: the resolver's own log convention (dns.c:9)"
    need "\[dns\] PASS: 'example.com' -> $DNS_ANSWER" "$WORK/$t.log" \
        "lane $t: DNS over UDP, accepted by dns_parse_response"
    need "\[tcp\] SYN options: 8 bytes, hdr=28, mss=1460 sack-perm" "$WORK/$t.log" \
        "lane $t: the SYN carries MSS + SACK-permitted (tcp.c:178)"
    need "\[tcp\] ESTABLISHED (peer_wnd=" "$WORK/$t.log" \
        "lane $t: the handshake completed"
    need "\[dns\] PASS: 'example.com' -> $DNS_ANSWER over TCP" "$WORK/$t.log" \
        "lane $t: DNS over the RFC 1035 TCP carriage"
    need "\[tcp-x5\] probing 16 concurrent TCP connections" "$WORK/$t.log" \
        "lane $t: the X5 gate ran"
    need "\[tcp-x5\] PASS: 16 concurrent connections held and closed cleanly" \
        "$WORK/$t.log" "lane $t: all 16 held and closed"
    need_no "\[tcp-x5\] FAIL" "$WORK/$t.log" "lane $t: the X5 gate failed"
    need_no "\[tcp\] timeout waiting for SYN-ACK" "$WORK/$t.log" \
        "lane $t: a handshake timed out"
    need_no "\[tcp\] connection refused" "$WORK/$t.log" \
        "lane $t: a connection was refused"
    need_no "\[dns\] FAIL" "$WORK/$t.log" "lane $t: a DNS leg failed"
    need "\[net\] PASS: DNS over UDP + DNS over TCP + 16 concurrent TCP connections" \
        "$WORK/$t.log" "lane $t: the N3 composite receipt"
    need "N3OK"    "$WORK/$t.log" "lane $t: payload summary"
    need_no "N3FAIL" "$WORK/$t.log" "lane $t: payload reported failure"
    need "TCP SYN from 10.0.2.15:40000 -> 53" "$WORK/$t-resp.out" \
        "lane $t: responder saw the DNS-over-TCP SYN"
    need "TCP DNS A? example.com" "$WORK/$t-resp.out" \
        "lane $t: responder answered DNS over TCP"
    need "TCP SYN from 10.0.2.15:40015 -> 53" "$WORK/$t-resp.out" \
        "lane $t: responder saw the sixteenth connection"
    need "on-subnet" "$WORK/$t-resp.out" \
        "lane $t: responder logged the non-gateway ARP"
    counteq "$WORK/$t-frames.hex" "$FULL_FRAMES" "lane $t wire frames"
done
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
need_no "\[dns\]" "$WORK/b1.log" "lane B: no DNS was attempted without a lease"
need_no "\[tcp"  "$WORK/b1.log" "lane B: no TCP was attempted without a lease"

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
        "$WORK/c1.log" "lane C: net_self_test receipt"
    need "\[dns\] PASS: example.com -> " "$WORK/c1.log" \
        "lane C: dns_self_test receipt (cache verified)"
    need "\[tcp-x5\] PASS: 16 concurrent connections held" "$WORK/c1.log" \
        "lane C: tcp_x5_self_test receipt"
    need_no "\[e1000\] TX timeout"                 "$WORK/c1.log" "lane C: D2 receipt gone"
    need_no "\[tcp-x5\] FAIL"                      "$WORK/c1.log" "lane C: X5 did not fail"
    echo "      (the guest's own case test_udp_sockets.sh:25 also wants"
    echo "       'UDPTEST PASS: example.com A ... from 10.0.2.3:53', which needs"
    echo "       'run udptest' at the shell prompt; assert it there.)"
else
    echo "SKIP: frozen guest kernel/initrd missing ($KERNEL / $INITRD)"
    echo "      reproduce with:"
    echo "        KERNEL=$KERNEL INITRD=$INITRD $0 $WORK"
fi

echo "== lane D: negative control -- TCP refused entirely (--no-tcp)"
run_lane d1 "$NC_TAP" --port 8082 --no-tcp
need "\[dns\] PASS: 'example.com' -> $DNS_ANSWER" "$WORK/d1.log" \
    "lane D: DNS over UDP still works (the lever is TCP-only)"
need "\[tcp\] SYN options: 8 bytes" "$WORK/d1.log" \
    "lane D: the SYN still went out"
rto=$(grep -c "\[tcp\] RTO waiting for SYN-ACK, retransmitting SYN" "$WORK/d1.log")
[ "$rto" -eq 3 ] || { echo "  COUNT: lane D SYN retransmits -- got $rto, want 3 (TCP_MAX_RETRIES)"; fail=1; }
need "\[tcp\] timeout waiting for SYN-ACK" "$WORK/d1.log" \
    "lane D: the guest's own timeout receipt"
need "\[net\] FAIL: no DNS answer over the TCP carriage" "$WORK/d1.log" \
    "lane D: the TCP carriage failed"
need "N3FAIL"   "$WORK/d1.log" "lane D: payload summary"
need_no "N3OK"  "$WORK/d1.log" "lane D: payload claimed success"
need_no "\[tcp-x5\]" "$WORK/d1.log" "lane D: never reached the X5 gate"
need_no "\[tcp\] ESTABLISHED" "$WORK/d1.log" "lane D: no handshake completed"
need "\[no-tcp\] TCP segment flags=0x02" "$WORK/d1-resp.out" \
    "lane D: the responder logged the withheld SYNs"
need "tcp=0" "$WORK/d1-resp.out" "lane D: no TCP connection was counted"
counteq "$WORK/d1-frames.hex" "$NC_FRAMES" "lane D wire frames"

if [ "$fail" -eq 0 ]; then
    echo "N3 end-to-end receipts: ALL PASS"
else
    echo "N3 end-to-end receipts: FAIL (see $WORK)"
    exit 1
fi
