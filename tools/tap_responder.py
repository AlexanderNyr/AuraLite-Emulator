#!/usr/bin/env python3
"""tools/tap_responder.py -- NET N2: the host side of the TAP socket.

Ground rule 1 (fixed, N0 artifact) makes the transport UDP datagrams that ARE
Ethernet frames at `127.0.0.1:8080`, with the emulator *connecting* and the
host side *binding*.  N1 put the first frame on that wire; N2 is the peer that
answers, in the shape of QEMU's `-netdev user` (slirp), because that is the
receipt shape the guest already names:

    [net] FAIL: no ICMP echo reply (is QEMU -netdev user configured?)
    il_assert_grep_fixed "$LOG" "[dhcp] PASS: IP 10.0.2.15, gateway 10.0.2.2"
        -- AuraLite-OS@0ed0d29 tests/integration/cases/test_e1000e.sh:49

Two modes:

  capture (N1's lane)  record every datagram as one hex line, answer nothing.
  slirp   (N2)         answer ARP for the gateway, DHCP DISCOVER/REQUEST with
                       OFFER/ACK, and ICMP echo with an echo reply.

What is measured, and from where
--------------------------------
Everything the guest asserts is measured from AuraLite-OS@0ed0d29
`kernel/net/net.c` -- the x86_64 stack, NOT `kernel/arch/i386/net32.c`, which
is a different port whose `[net] PASS: lease + ARP + echo reply` receipt this
kernel never prints (NET_PLAN N2's DoD quoted the wrong file; corrected here):

  * slirp addressing: net 10.0.2.0/24, guest `10.0.2.15`, gateway/DHCP server
    `10.0.2.2`, DNS `10.0.2.3` (`net.c:39-47` OUR_IP_O*/GW_IP_O*, plus the X3
    comment "the default server list (QEMU SLIRP 10.0.2.3)").
  * the OFFER/ACK filters (`net.c:900-960`, `1043-1105`): ethertype IPv4,
    `ip.protocol == 17`, `udp.src == 67`, `udp.dst == 68`, frame
    >= 14+20+8+236, `opts_len >= 4`, option 53 present with the right message
    type, and -- for the ACK only -- `xid` echoed.  Options 54/1/6 are read
    from the OFFER; option 3 (router) sets `gateway_ip` from the ACK.
  * the ARP reply filter (`net.c:437-465`): ethertype 0x0806, `opcode == 2`,
    length >= 42, and the guest takes `sender_mac` verbatim -- it never checks
    `target_ip`.
  * the ICMP reply filter (`net.c:527-556`): ethertype 0x0800,
    `ip.protocol == 1`, `icmp.type == 0`, frame >= 42.  The request is
    `ident 0x1234`, `seq 1`, 32 payload bytes `0x41..0x60`, `ttl 64`
    (`net.c:476`).

What is NOT measured (NET_PLAN ledger D7): the gateway's MAC address.  No
source reachable from this tree pins it -- libslirp's `bootp.c` was not
fetchable here and AuraLite-OS never records an `[net] ARP reply: gateway is
...` tape.  slirp's published scheme derives the MAC from the host IP
(`52:55:<the four IP bytes>`), so `10.0.2.2` -> `52:55:0a:00:02:02`; that is
the default here and `--gw-mac` exists so anyone holding a real `-netdev user`
tape can pin it.  No guest assertion depends on the value.

Determinism: every byte of every reply is a pure function of the request -- no
clock, no lease bookkeeping, no counters leaking into packets -- so two runs of
the same guest produce identical wires.

UPKEEP U6 closed the receive-path hole this file grew with the TCP carriage:
segments used to be glued onto the stream in *arrival* order (`rcv_nxt +=
seglen` on whatever showed up), so a duplicate or out-of-order segment -- the
wire's normal produce on any loss -- silently reinterpreted the DNS stream
and made every later ACK number arithmetic on fiction.  The receiver now
validates against RCV.NXT and the advertised window, trims overlaps, queues
out-of-order data (bounded), answers with duplicate ACKs carrying RFC 2018
SACK blocks when SACK was negotiated, sequences FINs in order only, re-sends
its FIN|ACK to a duplicate FIN in LAST_ACK instead of forgetting the tuple,
and keeps bounded tombstones for a late duplicate FIN after the close.  The
guest's M6d machinery is the consumer: it decodes up to four SACK blocks
(`tcp_m6d.h`, `TCPM6D_MAX_BLOCKS`) to mark its retransmission queue and pick
the hole for fast retransmit, so what is emitted here is exactly the shape
that decoder reads.
"""

import argparse
import binascii
import socket
import struct
import sys
import time

# ---- slirp's 10.0.2.0/24 (measured, see the module docstring) --------------
GUEST_IP = (10, 0, 2, 15)
GW_IP = (10, 0, 2, 2)
DNS_IP = (10, 0, 2, 3)
GW_MAC_DEFAULT = "52:55:0a:00:02:02"
LEASE_SECONDS = 3600

ETHERTYPE_IPV4 = 0x0800
ETHERTYPE_ARP = 0x0806
IP_PROTO_ICMP = 1
IP_PROTO_UDP = 17
ARP_REQUEST = 1
ARP_REPLY = 2
ICMP_ECHO_REQ = 8
ICMP_ECHO_REP = 0

DHCP_SERVER_PORT = 67
DHCP_CLIENT_PORT = 68
DNS_PORT = 53
DNS_LOCAL_PORT_GUEST = 12345   # kernel/net/dns.h:19 -- the guest's source port
DNS_QTYPE_A = 1
DNS_QTYPE_AAAA = 28
DNS_FLAG_QR = 0x8000
DNS_FLAG_TC = 0x0200
# The A record we hand out.  Any address satisfies the guest's own gates
# (`[dns] PASS: example.com -> a.b.c.d (cache verified)` and the udptest app's
# `UDPTEST PASS: example.com A .* from 10.0.2.3:53` -- both are regexes, no
# fixed address), so this is the real-world value for the name the guest asks
# about, kept behind --dns-ip rather than baked in.
DNS_ANSWER_IP_DEFAULT = "93.184.216.34"
DNS_ANSWER_TTL = 3600

IP_PROTO_TCP = 6
TCP_FIN = 0x01
TCP_SYN = 0x02
TCP_RST = 0x04
TCP_PSH = 0x08
TCP_ACK = 0x10
TCP_WINDOW = 64240          # the guest's own advertised window (tcp.c:44)
TCP_MSS = 1460              # NETL3_V4_MSS (netl3.h:35)
TCP_OPT_END = 0
TCP_OPT_MSS = 2
TCP_OPT_SACK_PERM = 4
TCP_OPT_SACK = 5
# UPKEEP U6 queue bounds.  Both are deterministic *counts*, never ages: this
# file's replies stay a pure function of the frames seen (no clock anywhere
# in the TCP model above).
#   * OOO runs held per connection.  Bytes are bounded by TCP_WINDOW itself
#     (nothing at or beyond RCV.NXT+RCV.WND is accepted), and 32 runs is
#     more than alternating MSS-sized holes can carve out of 64 KiB.
#   * Tombstones kept after a passive close completes, so a late duplicate
#     FIN can still be re-acknowledged.  FIFO eviction by count only.
TCP_OOO_MAX_SEGS = 32
TCP_TOMBSTONE_MAX = 16
# The guest opens TCP_MAX_CONNS (16) concurrent connections to 10.0.2.3:53 in
# tcp_x5_self_test and expects every one of them to reach ESTABLISHED, so the
# table has to be comfortably wider than that.  It is bounded because an
# unbounded table would let a guest that never closes exhaust the host.
TCP_MAX_CONNS = 64
# Our initial sequence number.  A constant, not a clock: the guest never
# validates an ISN (tcp_open only does `conn_ack = rx.seq + 1`), and a
# deterministic one is what keeps two runs of the same guest on an identical
# wire -- which is the property every other reply here already has.
TCP_ISN = 0x5A5A0000
DHCP_BOOTREQUEST = 1
DHCP_BOOTREPLY = 2
DHCP_DISCOVER = 1
DHCP_OFFER = 2
DHCP_REQUEST = 3
DHCP_ACK = 5
DHCP_MAGIC_COOKIE = 0x63825363
OPT_SUBNET_MASK = 1
OPT_ROUTER = 3
OPT_DNS = 6
OPT_REQUESTED_IP = 50
OPT_LEASE_TIME = 51
OPT_MSG_TYPE = 53
OPT_SERVER_ID = 54
OPT_END = 255

DHCP_FIXED_LEN = 236   # op..file(128) + cookie(4): the guest's sizeof(dhcp_pkt)
BROADCAST_MAC = b"\xff" * 6
BROADCAST_IP = b"\xff\xff\xff\xff"


def mac(s: str) -> bytes:
    return bytes(int(b, 16) for b in s.split(":"))


def macstr(b: bytes) -> str:
    return ":".join("%02x" % x for x in b)


def ip4(t) -> bytes:
    return bytes(t)


def on_subnet(ip: bytes, net: bytes, prefix: int) -> bool:
    """`ip` inside `net`/`prefix`.  The guest decides "local subnet" exactly
    this way (net.c:358: `(target_ip & subnet_mask) != (our_ip & subnet_mask)`
    means remote), and for a local target it ARPs for that target rather than
    for the gateway -- so a responder that only answers who-has-gateway makes
    DNS unreachable, which is what the N3 wire run measured."""
    mask = (0xFFFFFFFF << (32 - prefix)) & 0xFFFFFFFF if prefix else 0
    a = struct.unpack("!I", ip)[0] & mask
    b = struct.unpack("!I", net)[0] & mask
    return a == b


def ip4parse(s: str):
    """Dotted quad -> the tuple `ip4()` takes.  Rejects anything else rather
    than silently answering from a wrong address."""
    parts = s.split(".")
    if len(parts) != 4:
        raise ValueError("not a dotted quad: %r" % s)
    out = []
    for part in parts:
        v = int(part, 10)
        if not 0 <= v <= 255 or (len(part) > 1 and part[0] == "0"):
            raise ValueError("bad octet %r in %r" % (part, s))
        out.append(v)
    return tuple(out)


def ipstr(b: bytes) -> str:
    return ".".join(str(x) for x in b)


def csum(data: bytes) -> int:
    """RFC 1071 one's complement -- the same `checksum()` the guest builds at
    kernel/net/net.c:171: 16-bit words, carries folded, result complemented,
    an odd tail padded with a zero byte."""
    if len(data) % 2:
        data += b"\x00"
    acc = 0
    for i in range(0, len(data), 2):
        acc += (data[i] << 8) | data[i + 1]
    while acc >> 16:
        acc = (acc & 0xFFFF) + (acc >> 16)
    return (~acc) & 0xFFFF


def eth_pad(frame: bytes) -> bytes:
    """`net_eth_send` pads to the 60-byte minimum, so the guest's own frames
    arrive padded; replies are padded too, keeping the wire symmetric."""
    return frame + b"\x00" * (60 - len(frame)) if len(frame) < 60 else frame


# ============================ request parsing ==============================

def parse_eth(frame: bytes):
    if len(frame) < 14:
        return None
    et = struct.unpack("!H", frame[12:14])[0]
    return frame[0:6], frame[6:12], et, frame[14:]


def build_eth(dst: bytes, src: bytes, ethertype: int, payload: bytes) -> bytes:
    return eth_pad(dst + src + struct.pack("!H", ethertype) + payload)


def parse_arp(payload: bytes):
    """-> (opcode, sender_mac, sender_ip, target_ip) or None."""
    if len(payload) < 28:
        return None
    hw, proto, hlen, plen, op = struct.unpack("!HHBBH", payload[0:8])
    if hw != 1 or proto != ETHERTYPE_IPV4 or hlen != 6 or plen != 4:
        return None
    return op, payload[8:14], payload[14:18], payload[24:28]


def build_arp_reply(dst_mac: bytes, src_mac: bytes,
                    sender_ip: bytes, target_ip: bytes) -> bytes:
    payload = struct.pack("!HHBBH", 1, ETHERTYPE_IPV4, 6, 4, ARP_REPLY)
    payload += src_mac + sender_ip + dst_mac + target_ip
    return build_eth(dst_mac, src_mac, ETHERTYPE_ARP, payload)


def parse_ipv4(payload: bytes):
    """-> (proto, src, dst, body, ident) or None.

    `body` runs to the end of the *frame*, so it can carry Ethernet padding:
    `net_eth_send` pads every short frame to 60 bytes.  Callers that need the
    L4 segment exactly (TCP, where a checksum is computed over it) have to
    trim it to `ip.total_length` themselves -- `ipv4_body()` below does that.
    DHCP deliberately keeps the padded view because the guest's own filter
    measures `opts_len` from the frame length (net.c:911)."""
    if len(payload) < 20 or (payload[0] >> 4) != 4:
        return None
    ihl = (payload[0] & 0xF) * 4
    if ihl < 20 or len(payload) < ihl:
        return None
    total_len = struct.unpack("!H", payload[2:4])[0]
    ident = struct.unpack("!H", payload[4:6])[0]
    proto = payload[9]
    end = min(total_len, len(payload)) if total_len >= ihl else len(payload)
    return proto, payload[12:16], payload[16:20], payload[ihl:end], ident


def ipv4_body(payload: bytes, body: bytes):
    """`body` trimmed to what ip.total_length actually covers -- the L4 segment
    with no Ethernet padding on the end.  This is the length the guest's own
    receiver uses for TCP (tcp.c:326) and the length a checksum is over."""
    if len(payload) < 4:
        return body
    ihl = (payload[0] & 0x0F) * 4
    total = struct.unpack("!H", payload[2:4])[0]
    if total >= ihl and total - ihl < len(body):
        return body[0:total - ihl]
    return body


def build_ipv4(proto: int, src: bytes, dst: bytes, body: bytes,
               ident: int = 0, ttl: int = 255) -> bytes:
    hdr = struct.pack("!BBHHHBBH", (4 << 4) | 5, 0, 20 + len(body), ident,
                      0, ttl, proto, 0) + src + dst
    return hdr[0:10] + struct.pack("!H", csum(hdr)) + hdr[12:20] + body


def parse_udp(body: bytes):
    if len(body) < 8:
        return None
    sport, dport, _ulen, _ck = struct.unpack("!HHHH", body[0:8])
    return sport, dport, body[8:]


def build_udp(sport: int, dport: int, data: bytes,
              src_ip: bytes, dst_ip: bytes, compute_csum: bool) -> bytes:
    length = 8 + len(data)
    if compute_csum:
        pseudo = src_ip + dst_ip + struct.pack("!BBH", 0, IP_PROTO_UDP, length)
        ck = csum(pseudo + struct.pack("!HHHH", sport, dport, length, 0) + data)
        if ck == 0:
            ck = 0xFFFF            # RFC 768: zero means "no checksum"
    else:
        ck = 0                     # legal over IPv4, and what the guest sends
    return struct.pack("!HHHH", sport, dport, length, ck) + data


def tcp_build_options(mss: int = TCP_MSS, sack_perm: bool = True) -> bytes:
    """MSS then SACK-permitted, in the order the guest's own
    `tcpm6c_build_syn_opts` emits them (tcp.c:175 -- its SYN carries both and
    it prints `[tcp] SYN options: 8 bytes, hdr=28, mss=1460 sack-perm`)."""
    opts = bytearray()
    if mss:
        opts += bytes((TCP_OPT_MSS, 4)) + struct.pack("!H", mss)
    if sack_perm:
        opts += bytes((TCP_OPT_SACK_PERM, 2))
    while len(opts) % 4:
        opts.append(TCP_OPT_END)
    return bytes(opts)


def tcp_parse_options(opts: bytes):
    """-> (mss, sack_perm).  Tolerant walker: a truncated TLV stops it rather
    than reading past the header."""
    mss, sack = 0, False
    i = 0
    while i < len(opts):
        kind = opts[i]
        if kind == TCP_OPT_END:
            break
        if kind == 1:                       # NOP
            i += 1
            continue
        if i + 1 >= len(opts):
            break
        ln = opts[i + 1]
        if ln < 2 or i + ln > len(opts):
            break
        if kind == TCP_OPT_MSS and ln == 4:
            mss = struct.unpack("!H", opts[i + 2:i + 4])[0]
        elif kind == TCP_OPT_SACK_PERM and ln == 2:
            sack = True
        i += ln
    return mss, sack


def seq_diff(a: int, b: int) -> int:
    """Signed (a - b) in modulo-2^32 sequence space, in [-2^31, 2^31).

    TCP sequence numbers wrap, so plain subtraction misjudges order at the
    seam: 24 follows 0xFFFFFFF9, it does not precede it by four billion.
    This is the serial-number arithmetic RFC 9293's comparisons assume, and
    the same trick the guest's SACK decoder already trusts
    (`(int32_t)(e - s) <= 0` rejects a nonsense block, tcp_m6d.h)."""
    d = (a - b) & 0xFFFFFFFF
    return d - 0x100000000 if d >= 0x80000000 else d


def tcp_sack_options(rcv_nxt: int, ooo, trigger_seq=None) -> bytes:
    """RFC 2018 SACK option bytes describing the out-of-order queue `ooo`
    (entries [seq, data, fin], all inside (RCV.NXT, RCV.NXT+RCV.WND)), or
    b"" when nothing uncontiguous is held.

    Only data bytes are described -- a queued FIN occupies sequence space
    but SACK blocks are about bytes the sender need not retransmit, and
    advertising the FIN would tell the sender a byte exists there.  Runs
    are merged when adjacent (a filled hole between two queued runs makes
    one block), then ordered per RFC 2018 s4: the first block MUST be the
    one containing the segment that triggered this ACK (the most recent
    change to the receive queue), the rest follow in sequence order.  At
    most four blocks: 8n+2 bytes plus two pad bytes is what the 40-byte
    option space holds with no timestamp option in play, and four is
    exactly the guest decoder's TCPM6D_MAX_BLOCKS.  Everything is computed
    in offsets from RCV.NXT, so a block straddling the 2^32 seam comes out
    with a right edge numerically below its left edge -- precisely the
    shape the guest's `(int32_t)(e - s) > 0` sanity check accepts."""
    spans = []
    for seq, data, _fin in ooo:
        if not data:
            continue                      # a queued FIN alone SACKs nothing
        lo = seq_diff(seq, rcv_nxt)
        if lo > 0:
            spans.append((lo, lo + len(data)))
    if not spans:
        return b""
    spans.sort()
    blocks = []
    for lo, hi in spans:
        if blocks and lo <= blocks[-1][1]:        # adjacent or overlapping
            if hi > blocks[-1][1]:
                blocks[-1] = (blocks[-1][0], hi)
        else:
            blocks.append((lo, hi))
    if trigger_seq is not None:
        t = seq_diff(trigger_seq, rcv_nxt)
        for i, (lo, hi) in enumerate(blocks):
            if lo <= t < hi:
                blocks.insert(0, blocks.pop(i))
                break
    blocks = blocks[:4]
    opts = bytearray((TCP_OPT_SACK, 2 + 8 * len(blocks)))
    for lo, hi in blocks:
        opts += struct.pack("!II", (rcv_nxt + lo) & 0xFFFFFFFF,
                            (rcv_nxt + hi) & 0xFFFFFFFF)
    while len(opts) % 4:
        opts.append(TCP_OPT_END)
    return bytes(opts)


def parse_tcp(body: bytes):
    """-> dict or None.  The header length comes from `data_offset`, never from
    a constant: the guest's SYN is 28 bytes (options) while its data segments
    are 20, and `tcp_recv_segment_timeout` itself rejects anything outside
    5..15 words (tcp.c:337).  Payload length is taken from what the IP header
    said the segment was, because the NIC pads short frames to 60 bytes and
    counting the pad as TCP payload is the exact bug that comment names."""
    if len(body) < 20:
        return None
    sport, dport, seq, ack = struct.unpack("!HHII", body[0:12])
    off_flags, window, ck, urg = struct.unpack("!HHHH", body[12:20])
    words = off_flags >> 12
    if words < 5 or words > 15:
        return None
    hdr = words * 4
    if len(body) < hdr:
        return None
    return {"sport": sport, "dport": dport, "seq": seq, "ack": ack,
            "flags": off_flags & 0x1FF, "window": window, "checksum": ck,
            "hdr": hdr, "opts": body[20:hdr], "data": body[hdr:]}


def tcp_checksum(src_ip: bytes, dst_ip: bytes, seg: bytes) -> int:
    """RFC 793 pseudo-header sum.  `netl3`'s output path fills this in for the
    guest, so a segment that arrives with a bad sum is a bug worth refusing."""
    pseudo = src_ip + dst_ip + struct.pack("!BBH", 0, IP_PROTO_TCP, len(seg))
    return csum(pseudo + seg)


def build_tcp(sport: int, dport: int, seq: int, ack: int, flags: int,
              src_ip: bytes, dst_ip: bytes, opts: bytes = b"",
              data: bytes = b"", window: int = TCP_WINDOW,
              compute_csum: bool = True) -> bytes:
    hdr = struct.pack("!HHIIBBHHH", sport, dport, seq & 0xFFFFFFFF,
                      ack & 0xFFFFFFFF, ((20 + len(opts)) >> 2) << 4, flags,
                      window, 0, 0) + opts + data
    if compute_csum:
        ck = tcp_checksum(src_ip, dst_ip, hdr)
        hdr = hdr[0:16] + struct.pack("!H", ck) + hdr[18:]
    return hdr


def dns_parse_query(msg: bytes):
    """-> dict(id, flags, qname, qtype, qclass, question) or None.

    `question` is the message's own question section bytes, kept verbatim so
    the reply echoes them instead of re-encoding a name -- one less place to
    diverge from what was asked.  Label lengths are validated as the guest's
    `dns_encode_name` produces them (1..63, NUL-terminated), because
    `dns_name_read` on the guest side rejects anything else.
    """
    if len(msg) < 12:
        return None
    qid, flags, qd = struct.unpack("!HHH", msg[0:6])
    if qd != 1:
        return None                      # the guest's builder always sends one
    pos = 12
    while pos < len(msg):
        n = msg[pos]
        if n == 0:
            pos += 1
            break
        if (n & 0xC0) != 0 or n > 63:
            return None                  # no compression in a question
        pos += 1 + n
    else:
        return None                      # ran off the end: no root label
    if pos + 4 > len(msg):
        return None
    qtype, qclass = struct.unpack("!HH", msg[pos:pos + 4])
    pos += 4
    labels = []
    i = 12
    while i < len(msg) and msg[i]:
        labels.append(msg[i + 1:i + 1 + msg[i]].decode("ascii", "replace"))
        i += 1 + msg[i]
    return {"id": qid, "flags": flags, "qname": ".".join(labels).lower(),
            "qtype": qtype, "qclass": qclass, "question": msg[12:pos]}


def dns_encode_name(name: str) -> bytes:
    """`dns_encode_name` (dns_parse.c:37): lowercased labels, each prefixed by
    its length, NUL-terminated."""
    out = bytearray()
    for label in name.split("."):
        if not label or len(label) > 63:
            raise ValueError("bad DNS label %r" % label)
        out.append(len(label))
        out += label.lower().encode("ascii")
    out.append(0)
    return bytes(out)


def dns_build_reply(q: dict, answer_ip: bytes, ttl: int = DNS_ANSWER_TTL) -> bytes:
    """A minimal NOERROR answer that `dns_parse_response` (dns_parse.c:132)
    accepts: echoed id, QR+RD+RA, QDCOUNT/ANCOUNT 1, the question echoed
    verbatim, then one A RR whose owner is a compression pointer to it.

    The guest compares the RR owner against the asked name case-insensitively
    (`ci_equal`), requires `class == 1`, `type == 1` and `rdlen == 4`, and
    stops at the first matching A -- so a pointer owner is both legal and the
    shortest correct thing to send."""
    hdr = struct.pack("!HHHHHH", q["id"], 0x8180, 1, 1, 0, 0)
    ans = struct.pack("!HHHIH", 0xC00C, DNS_QTYPE_A, 1, ttl, 4) + answer_ip
    return hdr + q["question"] + ans


def dhcp_find_option(opts: bytes, code: int):
    """The guest's own walker (net.c:777): pad (0) skipped, END (255) stops, a
    truncated TLV stops.  Replies are validated with the same code path the
    guest uses, not a more forgiving one."""
    i = 0
    while i < len(opts):
        c = opts[i]
        if c == OPT_END:
            return None
        if c == 0:
            i += 1
            continue
        if i + 1 >= len(opts):
            return None
        ln = opts[i + 1]
        if c == code:
            return opts[i + 2:i + 2 + ln]
        i += 2 + ln
    return None


def parse_dhcp(payload: bytes):
    """-> dict or None.  Requires the 236-byte fixed part plus >= 4 option
    bytes, which is the guest's own `opts_len < 4` gate."""
    if len(payload) < DHCP_FIXED_LEN + 4:
        return None
    if payload[0] != DHCP_BOOTREQUEST:
        return None
    if struct.unpack("!I", payload[232:236])[0] != DHCP_MAGIC_COOKIE:
        return None
    chaddr = payload[28:44]
    hlen = payload[2]
    opts = payload[DHCP_FIXED_LEN:]
    mt = dhcp_find_option(opts, OPT_MSG_TYPE)
    if not mt or len(mt) != 1:
        return None
    return {
        "htype": payload[1], "hlen": hlen,
        "xid": struct.unpack("!I", payload[4:8])[0],
        "flags": struct.unpack("!H", payload[10:12])[0],
        "chaddr": chaddr,
        "chaddr_mac": chaddr[0:hlen] if 0 < hlen <= 16 else chaddr[0:6],
        "msgtype": mt[0],
        "requested_ip": dhcp_find_option(opts, OPT_REQUESTED_IP),
        "server_id": dhcp_find_option(opts, OPT_SERVER_ID),
    }


def build_dhcp_reply(req: dict, msgtype: int, yiaddr: bytes) -> bytes:
    """One BOOTREPLY the way slirp's dhcp_reply builds it: the 236-byte fixed
    part (xid/chaddr echoed, sname/file zero) then options in slirp's order --
    53 message type, 54 server id, 51 lease time, 1 mask, 3 router, 6 DNS,
    END.  The guest reads 53+54+1+6 from the OFFER and 53+3 (+yiaddr, xid)
    from the ACK, so one option list serves both."""
    body = bytearray(DHCP_FIXED_LEN)
    body[0] = DHCP_BOOTREPLY
    body[1] = req["htype"] or 1
    body[2] = req["hlen"] or 6
    body[3] = 0                                    # hops
    struct.pack_into("!I", body, 4, req["xid"])    # xid echoed
    struct.pack_into("!H", body, 8, 0)             # secs
    struct.pack_into("!H", body, 10, 0)            # flags
    body[12:16] = b"\x00\x00\x00\x00"              # ciaddr
    body[16:20] = yiaddr                           # yiaddr
    body[20:24] = ip4(GW_IP)                       # siaddr
    body[24:28] = b"\x00\x00\x00\x00"              # giaddr
    body[28:44] = req["chaddr"]                    # chaddr, all 16 bytes
    struct.pack_into("!I", body, 232, DHCP_MAGIC_COOKIE)

    opts = bytearray()
    opts += bytes([OPT_MSG_TYPE, 1, msgtype])
    opts += bytes([OPT_SERVER_ID, 4]) + ip4(GW_IP)
    opts += bytes([OPT_LEASE_TIME, 4]) + struct.pack("!I", LEASE_SECONDS)
    opts += bytes([OPT_SUBNET_MASK, 4]) + ip4((255, 255, 255, 0))
    opts += bytes([OPT_ROUTER, 4]) + ip4(GW_IP)
    opts += bytes([OPT_DNS, 4]) + ip4(DNS_IP)
    opts += bytes([OPT_END])
    return bytes(body) + bytes(opts)


# ================================ the loop =================================

class Responder:
    """Answers one frame; returns the reply frame or None.  A pure function of
    the request -- no clock, no lease table, no counters -- so two runs of the
    same guest see an identical wire."""

    def __init__(self, gw_mac: bytes, udp_checksum: bool = False,
                 quiet: bool = False, drop: str = "",
                 dns_ip: str = DNS_ANSWER_IP_DEFAULT, no_tcp: bool = False):
        self.gw_mac = gw_mac
        self.udp_checksum = udp_checksum
        self.quiet = quiet
        self.drop = frozenset(d for d in drop.split(",") if d)
        self.dns_answer_ip = ip4(ip4parse(dns_ip))
        self.no_tcp = no_tcp
        self.stats = {"arp": 0, "dhcp": 0, "icmp": 0, "dns": 0, "tcp": 0,
                      "ignored": 0}
        # TCP is the one protocol here that cannot be answered statelessly --
        # a 4-tuple needs its own sequence numbers.  Keyed by the tuple, holds
        # {snd_nxt, rcv_nxt, state, buf, isn, mac, sack_perm, ooo, fin_seq};
        # bounded at TCP_MAX_CONNS, freed when the connection finishes, and
        # derived only from the segments seen, so two runs of the same guest
        # still produce the same wire.
        self.tcp = {}
        # Bounded retained state after a passive close completes (UPKEEP U6):
        # the final ACK we owe a late duplicate FIN, keyed by the same tuple.
        # The guest is the active closer and therefore the side in TIME_WAIT;
        # this is not a passive-side TIME_WAIT (2MSL is a wall clock and this
        # file has none) -- it keeps exactly {rcv_nxt, snd_nxt, mac} so the
        # final ACK stays answerable, evicted FIFO past TCP_TOMBSTONE_MAX.
        self.tcp_closed = {}

    def log(self, msg: str) -> None:
        if not self.quiet:
            print(msg, flush=True)

    def handle(self, frame: bytes):
        p = parse_eth(frame)
        if not p:
            return None
        _dst, src, et, payload = p

        if et == ETHERTYPE_ARP:
            a = parse_arp(payload)
            if not a:
                return None
            op, smac, sip, tip = a
            if op != ARP_REQUEST:
                return None
            # slirp is one user-mode stack on one interface, so it answers
            # who-has for its own gateway *and* for anything else on the
            # 10.0.2.0/24 it hands out -- with the same MAC, because there is
            # only one.  The guest needs this: `net_arp_resolve` ARPs the
            # target itself for an on-link address (net.c:405+), so a DNS query
            # to 10.0.2.3 is preceded by who-has 10.0.2.3, not by who-has
            # 10.0.2.2.  Off-subnet targets are never ARPed for (they route via
            # the gateway), and a probe for the guest's own address is not ours
            # to answer.
            gw = ip4(GW_IP)
            if tip != gw and not (on_subnet(tip, ip4(GUEST_IP), 24)
                                  and tip != ip4(GUEST_IP)):
                return None
            if "arp" in self.drop:
                self.log("  [drop] ARP request for %s (negative control)"
                         % ipstr(tip))
                return None
            self.stats["arp"] += 1
            self.log("  ARP  who-has %s tell %s -> reply %s is at %s"
                     % (ipstr(tip), ipstr(sip), ipstr(tip),
                        macstr(self.gw_mac))
                     if tip == gw else
                     "  ARP  who-has %s tell %s -> reply %s is at %s (on-subnet)"
                     % (ipstr(tip), ipstr(sip), ipstr(tip),
                        macstr(self.gw_mac)))
            # slirp broadcasts the reply -- during DHCP the request comes from
            # 0.0.0.0, and the guest only ever checks `opcode == 2`.
            return build_arp_reply(BROADCAST_MAC, self.gw_mac, tip, sip)

        if et != ETHERTYPE_IPV4:
            return None
        v = parse_ipv4(payload)
        if not v:
            return None
        proto, ipsrc, _ipdst, body, ident = v

        if proto == IP_PROTO_ICMP:
            if len(body) < 8 or body[0] != ICMP_ECHO_REQ:
                return None
            if "icmp" in self.drop:
                self.log("  [drop] ICMP echo request (negative control)")
                return None
            self.stats["icmp"] += 1
            icid, icseq = struct.unpack("!HH", body[4:8])
            self.log("  ICMP echo request from %s (id 0x%04x seq %u, %d payload"
                     " bytes) -> echo reply"
                     % (ipstr(ipsrc), icid, icseq, len(body) - 8))
            # slirp's icmp_input echo path: swap addresses, type 0, keep
            # ident/seq/payload byte for byte, recompute both checksums.
            icmp = bytearray(body)
            icmp[0] = ICMP_ECHO_REP
            icmp[1] = 0
            icmp[2:4] = b"\x00\x00"
            icmp[2:4] = struct.pack("!H", csum(bytes(icmp)))
            return build_eth(src, self.gw_mac, ETHERTYPE_IPV4,
                             build_ipv4(IP_PROTO_ICMP, ip4(GW_IP), ipsrc,
                                        bytes(icmp), ident=ident))

        if proto != IP_PROTO_UDP:
            return None
        u = parse_udp(body)
        if not u:
            return None
        sport, dport, data = u

        if dport == DNS_PORT:
            return self.handle_dns(src, ipsrc, sport, data, ident)
        if sport != DHCP_CLIENT_PORT or dport != DHCP_SERVER_PORT:
            return None
        req = parse_dhcp(data)
        if not req:
            self.stats["ignored"] += 1
            self.log("  DHCP: malformed request ignored")
            return None
        if "dhcp" in self.drop:
            self.log("  [drop] DHCP message type %d (negative control)"
                     % req["msgtype"])
            return None
        if req["msgtype"] == DHCP_DISCOVER:
            self.log("  DHCP DISCOVER xid=0x%08x chaddr=%s -> OFFER %s"
                     % (req["xid"], macstr(req["chaddr_mac"]),
                        ipstr(ip4(GUEST_IP))))
            reply = build_dhcp_reply(req, DHCP_OFFER, ip4(GUEST_IP))
        elif req["msgtype"] == DHCP_REQUEST:
            want = req["requested_ip"] or ip4(GUEST_IP)
            self.log("  DHCP REQUEST xid=0x%08x for %s -> ACK %s"
                     % (req["xid"], ipstr(want), ipstr(want)))
            reply = build_dhcp_reply(req, DHCP_ACK, want)
        else:
            self.stats["ignored"] += 1
            self.log("  DHCP message type %d ignored" % req["msgtype"])
            return None
        self.stats["dhcp"] += 1

        # Broadcast at L2 and L3.  The client set the RFC 2131 broadcast flag
        # and still holds no address -- exactly the failure its own comment
        # names ("OFFER-received/ACK-never" when the server unicasts the ACK to
        # the still-unconfigured address), so unicasting is not an option.
        udp = build_udp(DHCP_SERVER_PORT, DHCP_CLIENT_PORT, reply,
                        ip4(GW_IP), BROADCAST_IP, self.udp_checksum)
        ip = build_ipv4(IP_PROTO_UDP, ip4(GW_IP), BROADCAST_IP, udp,
                        ident=0x1234)
        return build_eth(BROADCAST_MAC, self.gw_mac, ETHERTYPE_IPV4, ip)


    def handle_dns(self, src_mac: bytes, ipsrc: bytes, sport: int,
                   data: bytes, ident: int):
        """DNS over UDP/53 -- the carriage `net_dns_self_test` uses at boot
        (`kernel/kernel.c:429`) and the one the guest's `udptest` app uses
        (`UDPTEST PASS: example.com A .* from 10.0.2.3:53`).

        Unicast, from 10.0.2.3:53 to the query's own source port: the guest's
        `dns_wire_query` rejects an answer where `got_ip != server_ip or
        got_port != 53` (dns.c:172), and `net_udp_recvfrom` matches on the
        *destination* port, so the reply has to come back to
        `DNS_LOCAL_PORT_GUEST` (12345) rather than to 53.  Only type A is
        answered; anything else is left alone rather than invented."""
        q = dns_parse_query(data)
        if not q:
            self.stats["ignored"] += 1
            self.log("  DNS: malformed query ignored")
            return None
        if q["qtype"] != DNS_QTYPE_A:
            self.stats["ignored"] += 1
            self.log("  DNS query for '%s' type %d ignored (only A is answered)"
                     % (q["qname"], q["qtype"]))
            return None
        if "dns" in self.drop:
            self.log("  [drop] DNS A query for '%s' (negative control)"
                     % q["qname"])
            return None
        self.stats["dns"] += 1
        msg = dns_build_reply(q, self.dns_answer_ip)
        self.log("  DNS A? %s (id 0x%04x, from %s:%d) -> %s ttl %d"
                 % (q["qname"], q["id"], ipstr(ipsrc), sport,
                    ipstr(self.dns_answer_ip), DNS_ANSWER_TTL))
        udp = build_udp(DNS_PORT, sport, msg, ip4(DNS_IP), ipsrc,
                        self.udp_checksum)
        ip = build_ipv4(IP_PROTO_UDP, ip4(DNS_IP), ipsrc, udp, ident=ident)
        return build_eth(src_mac, self.gw_mac, ETHERTYPE_IPV4, ip)


    # ---- TCP ------------------------------------------------------------

    def tcp_segment(self, c, tuple_key, flags, data=b"", ident=0, opts=b"",
                    seq=None):
        """One segment for an existing 4-tuple, 20-byte header (options ride
        SYNs only -- the guest's own `tcp_send_segment_at` says so: "Options
        ride the SYN only; data segments stay 20 bytes").  The one exception
        is ours, since UPKEEP U6: ACKs may carry RFC 2018 SACK blocks in
        `opts`, which is what the guest negotiates SACK-permitted *for*.
        `seq` overrides SND.NXT for the one retransmission this timerless
        responder ever makes -- its FIN in LAST_ACK.

        The TCP checksum is ALWAYS computed.  Unlike UDP over IPv4, where a
        zero checksum legally means "not computed" (and is what the guest
        itself sends), RFC 793 makes the TCP checksum mandatory -- a segment
        with checksum 0 is not a valid segment, so `--udp-checksum` has no
        business reaching this path."""
        cip, cport, sip, sport = tuple_key
        seg = build_tcp(sport, cport, c["snd_nxt"] if seq is None else seq,
                        c["rcv_nxt"], flags, sip, cip, opts=opts, data=data,
                        compute_csum=True)
        ip = build_ipv4(IP_PROTO_TCP, sip, cip, seg, ident=ident)
        return build_eth(c["mac"], self.gw_mac, ETHERTYPE_IPV4, ip)

    def tcp_sack_opts(self, c, trigger_seq=None):
        """The SACK option bytes for an ACK on connection `c`, or b"" --
        either because nothing uncontiguous is held, or because the peer
        never sent SACK-permitted in its SYN, in which case RFC 2018 s4 says
        the receiver MUST NOT send SACK options at all.  (Our own SYN-ACK
        always offers SACK-permitted, but that licenses the *guest* to SACK
        our data, not us to SACK theirs.)"""
        if not c.get("sack_perm"):
            return b""
        return tcp_sack_options(c["rcv_nxt"], c["ooo"], trigger_seq)

    def tcp_ack_frame(self, c, key, ident, trigger_seq=None):
        """A bare acknowledgment <SEQ=SND.NXT><ACK=RCV.NXT><CTL=ACK>, with
        SACK blocks for whatever out-of-order data is held.  Data-less and
        at the constant advertised window, so the guest counts it as a true
        duplicate ACK under RFC 5681 s2: its `tcpm6_on_ack` is handed
        `data_len > 0` and `window != snd_wnd` as exclusions, and either
        would silently un-count a dup ACK that fast retransmit waits for."""
        return self.tcp_segment(c, key, TCP_ACK,
                                opts=self.tcp_sack_opts(c, trigger_seq),
                                ident=ident)

    def tcp_forget(self, key, tombstone=False):
        """Drop a 4-tuple.  With `tombstone`, keep the final sequence
        positions (UPKEEP U6) bounded FIFO at TCP_TOMBSTONE_MAX entries, so
        a late duplicate of the peer's FIN can be re-acknowledged instead
        of meeting the silence an unknown tuple gets."""
        c = self.tcp.pop(key, None)
        if tombstone and c is not None:
            self.tcp_closed[key] = {"rcv_nxt": c["rcv_nxt"],
                                    "snd_nxt": c["snd_nxt"], "mac": c["mac"]}
            while len(self.tcp_closed) > TCP_TOMBSTONE_MAX:
                self.tcp_closed.pop(next(iter(self.tcp_closed)))

    def tcp_receive(self, c, t):
        """Sequence one segment against RCV.NXT (UPKEEP U6).

        -> (delivered, fin_now, want_ack, trigger_seq):
            delivered    contiguous bytes were appended to c["buf"]
            fin_now      the peer's FIN became the next sequence position
                         and was consumed; the caller owes our FIN|ACK
            want_ack     an immediate acknowledgment is owed -- the
                         pre-U6 cadence ACKed everything data-bearing, and
                         that cadence is kept so the in-order wire is
                         unchanged byte for byte
            trigger_seq  where the triggering segment began (after
                         trimming), for the RFC 2018 first-block rule;
                         None when this segment advanced the ACK number

        The pre-U6 code was `rcv_nxt += seglen; buf += data`
        unconditionally: a duplicate advanced the cumulative ACK over
        bytes never sent, an out-of-order segment was glued straight onto
        the stream, and a FIN after a gap consumed the gap's sequence
        space along with its own.  The rule here is RFC 9293 s3.10.7.4's
        own: "One could tailor actual segments to fit this assumption by
        trimming off any portions that lie outside the window (including
        SYN and FIN) and only processing further if the segment then
        begins at RCV.NXT.  Segments with higher beginning sequence
        numbers SHOULD be held for later processing."  What is discarded
        (old, or beyond the window's right edge) is acknowledged with the
        current RCV.NXT and never SACKed -- SACK advertises what is held,
        and inventing holdings would retire the guest's retransmission
        queue over bytes we threw away.
        """
        data = t["data"]
        seq = t["seq"]
        fin = bool(t["flags"] & TCP_FIN)
        dlen = len(data)
        if not dlen and not fin:
            return False, False, False, None    # a pure ACK: see the rule
                                                # in handle_tcp's docstring
        rcv = c["rcv_nxt"]
        off = seq_diff(seq, rcv)                # first byte's position
        fin_off = off + dlen                    # the FIN byte's position
        if fin_off < 0 or (fin_off == 0 and not fin):
            return False, False, True, None     # an old duplicate, whole
        if off >= TCP_WINDOW:
            return False, False, True, None     # entirely beyond RCV.WND
        # The FIN survives trimming only if its own position lies inside
        # the window -- the acceptability tests examine SEQ+SEG.LEN-1 with
        # the FIN counted in SEG.LEN.  A FIN trimmed off here is not lost:
        # the sender's retransmission brings it back once RCV.NXT moves.
        fin = fin and fin_off < TCP_WINDOW
        if off < 0:                             # straddles RCV.NXT: only
            data = data[-off:]                  # the new part is processed
            seq = rcv
            dlen = len(data)
            off = 0
        if off + dlen > TCP_WINDOW:             # straddles the right edge
            data = data[:TCP_WINDOW - off]
            dlen = len(data)

        if off > 0:                             # beyond a gap: hold it
            self.tcp_ooo_insert(c, seq, data, fin)
            return False, False, True, seq

        delivered = False
        if dlen:
            c["buf"] += data
            c["rcv_nxt"] = (rcv + dlen) & 0xFFFFFFFF
            delivered = True
        if fin:
            c["rcv_nxt"] = (c["rcv_nxt"] + 1) & 0xFFFFFFFF
            c["ooo"] = []         # nothing may follow a FIN: purge the queue
            return delivered, True, True, None
        fin_now, more = self.tcp_drain_ooo(c)
        return delivered or more, fin_now, True, None

    def tcp_ooo_insert(self, c, seq, data, fin):
        """Queue one in-window segment that starts past RCV.NXT.

        Portions already queued are carved out of the newcomer, never the
        other way around, so a retransmission with different boundaries
        cannot double-count bytes (and the bytes kept are the first copy
        seen, which every later segment is measured against).  The FIN, if
        it survived window trimming, rides the piece that ends at its
        position; if the data was wholly queued already but the FIN is new
        -- a retransmitted tail whose FIN stood outside the window the
        first time -- a zero-length marker holds its place.

        The queue is bounded at TCP_OOO_MAX_SEGS runs; a segment that
        would break the bound is discarded whole rather than partially
        (a partial accept would need SACK to describe shards, and the
        sender's retransmission finds room once the left gap fills).
        All interval work is in offsets from RCV.NXT, where every run lies
        in (0, TCP_WINDOW), so plain integer comparisons stay correct
        across the 2^32 seam."""
        rcv = c["rcv_nxt"]
        pieces = [(seq_diff(seq, rcv), data)]
        for eseq, edata, _efin in c["ooo"]:
            if not edata:
                continue
            elo = seq_diff(eseq, rcv)
            ehi = elo + len(edata)
            carved = []
            for plo, pdata in pieces:
                phi = plo + len(pdata)
                if ehi <= plo or elo >= phi:
                    carved.append((plo, pdata))
                    continue
                if elo > plo:
                    carved.append((plo, pdata[:elo - plo]))
                if ehi < phi:
                    carved.append((ehi, pdata[ehi - plo:]))
            pieces = carved
            if not pieces:
                break

        fin_pos = seq_diff(seq, rcv) + len(data) if fin else None
        if fin_pos is not None:
            for eseq, edata, efin in c["ooo"]:
                eoff = seq_diff(eseq, rcv)
                if efin and eoff + len(edata) == fin_pos:
                    fin_pos = None     # this FIN is already queued
                    break
                if edata and eoff <= fin_pos < eoff + len(edata):
                    fin_pos = None     # the position is claimed as data:
                    break              # a contradictory peer; first wins
        entries = []
        for plo, pdata in pieces:
            take_fin = fin_pos is not None and plo + len(pdata) == fin_pos
            entries.append([(rcv + plo) & 0xFFFFFFFF, pdata, take_fin])
            if take_fin:
                fin_pos = None
        if fin_pos is not None:
            entries.append([(rcv + fin_pos) & 0xFFFFFFFF, b"", True])
        if not entries:
            return
        if len(c["ooo"]) + len(entries) > TCP_OOO_MAX_SEGS:
            return               # discarded whole: honestly unSACKed
        c["ooo"].extend(entries)

    def tcp_drain_ooo(self, c):
        """Deliver queued runs RCV.NXT has caught up with, in SEQ order.

        A run may straddle RCV.NXT -- the segment that just landed
        re-covered its head -- so only its new tail is delivered.  A run
        wholly behind RCV.NXT is a duplicate's duplicate and is dropped,
        except that a FIN positioned exactly at RCV.NXT is consumed
        (sequence space is sequence space; the caller owes the close
        handshake for it).  -> (fin_now, delivered_anything)."""
        delivered = False
        while True:
            progressed = False
            for e in list(c["ooo"]):
                eoff = seq_diff(e[0], c["rcv_nxt"])
                span = len(e[1]) + (1 if e[2] else 0)
                if eoff + span <= 0:
                    c["ooo"].remove(e)          # wholly old: dead
                    progressed = True
                    continue
                if eoff > 0:
                    continue                     # still across a gap
                data = e[1][-eoff:] if eoff < 0 else e[1]
                c["ooo"].remove(e)
                progressed = True
                if data:
                    c["buf"] += data
                    c["rcv_nxt"] = (c["rcv_nxt"] + len(data)) & 0xFFFFFFFF
                    delivered = True
                if e[2]:
                    c["rcv_nxt"] = (c["rcv_nxt"] + 1) & 0xFFFFFFFF
                    c["ooo"] = []       # nothing may follow a FIN
                    return True, delivered
            if not progressed:
                return False, delivered

    def handle_tcp(self, src_mac, ipsrc, ipdst, body, ident):
        """-> list of reply frames (usually 0 or 1).

        The state machine is the minimum that satisfies the frozen guest's
        tcp.c, measured rather than assumed:

        * `tcp_open_addr` sends SYN, then *blocks* up to TCP_MAX_RETRIES(3) x
          TCP_RTO_TICKS(20) waiting for SYN|ACK, answers with a bare ACK and
          only then reports ESTABLISHED -- so 16 concurrent opens
          (`tcp_x5_self_test`) need 16 handshakes, not one.
        * `tcp_recv_segment_timeout` filters on the 4-tuple only: src ip ==
          peer, `src_port == conn_dst_port`, `dst_port == conn_src_port`, and a
          data_offset in 5..15 words.  It never checks seq/ack, and it takes the
          payload length from the IP total, not the frame length (the NIC pads
          to 60).  So the reply's ports and its ip.total_length are what have to
          be right.
        * `tcp_close` from ESTABLISHED sends FIN|ACK and accepts a single
          FIN|ACK back: the ACK branch moves it to FIN_WAIT_2 and the FIN
          branch (same segment) to TIME_WAIT after it ACKs us.  Answering with
          one FIN|ACK therefore closes in a single round trip with no RTO.
        * A bare ACK is never answered.  The guest's closer consumes *any*
          segment matching the tuple, so an ACK we sent in reply to its final
          ACK would be eaten by the next `tcp_close` and misread as a FIN ACK.
          Silence is both correct TCP and the deterministic choice.
        * Sequence numbers are validated (UPKEEP U6, RFC 9293 s3.10.7.4 --
          `tcp_receive`): only contiguous bytes reach the DNS stream, gaps
          are held in a bounded queue and reported with RFC 2018 SACK blocks
          on duplicate ACKs, and a FIN counts only in sequence order.  The
          guest's receiver never checks our seq/ack either, but *its* M6d
          fast-retransmit machinery reads SACK blocks and counts duplicate
          ACKs, so the stimuli it was built against have to be real.
        * The close is as small as the guest makes it: we are always the
          passive closer (our FIN answers the peer's), so CLOSING and
          TIME_WAIT are *guest-side* states here and unreachable on ours --
          no code fabricates them.  What a passive closer does owe: a
          duplicate FIN in LAST_ACK gets our FIN|ACK again (the
          retransmission timer's job, keyed to the peer's retransmission,
          since determinism forbids us a clock), and after the tuple is
          freed a bounded tombstone still answers a late duplicate FIN
          with the final ACK.
        """
        if self.no_tcp:
            self.stats["ignored"] += 1
            self.log("  [no-tcp] TCP segment flags=0x%02x from port %d ignored "
                     "(negative control)" % (body[13] if len(body) > 13 else 0,
                                             struct.unpack("!H", body[0:2])[0]
                                             if len(body) >= 2 else 0))
            return []
        t = parse_tcp(body)
        if not t:
            self.stats["ignored"] += 1
            return []
        # RFC 793 verification: the one's-complement sum over the pseudo-header
        # and the WHOLE segment, checksum field included, comes out 0 when the
        # segment is good.  (Zeroing the field first and comparing against the
        # received value is the same arithmetic, but it is one more place to get
        # the order wrong -- this form was measured against a corrupted byte.)
        # A zero checksum means "not computed" and is accepted as-is: the
        # guest's `tcp_send_segment_at` leaves it 0 and lets the L3 output path
        # fill it in, so this only ever fires on a segment that claims to carry
        # a sum and does not.
        if t["checksum"] and tcp_checksum(ipsrc, ipdst, body) != 0:
            self.stats["ignored"] += 1
            self.log("  TCP: bad checksum ignored")
            return []
        key = (ipsrc, t["sport"], ipdst, t["dport"])
        flags = t["flags"]
        c = self.tcp.get(key)

        if flags & TCP_RST:
            # An aborted connection keeps no tombstone either: a reset means
            # both sides have wiped the slate, and answering some late
            # duplicate FIN from before the RST would resurrect it.
            self.tcp.pop(key, None)
            self.tcp_closed.pop(key, None)
            return []

        if (flags & TCP_SYN) and not (flags & TCP_ACK):
            if c is None:
                if len(self.tcp) >= TCP_MAX_CONNS:
                    self.stats["ignored"] += 1
                    self.log("  TCP: connection table full (%d), SYN from "
                             "port %d reset" % (TCP_MAX_CONNS, t["sport"]))
                    rst = build_tcp(t["dport"], t["sport"], 0, t["seq"] + 1,
                                    TCP_RST | TCP_ACK, ipdst, ipsrc)
                    ip = build_ipv4(IP_PROTO_TCP, ipdst, ipsrc, rst,
                                    ident=ident)
                    return [build_eth(src_mac, self.gw_mac, ETHERTYPE_IPV4, ip)]
                mss, sack = tcp_parse_options(t["opts"])
                c = {"snd_nxt": TCP_ISN + 1, "rcv_nxt": (t["seq"] + 1) & 0xFFFFFFFF,
                     "state": "SYN_RCVD", "buf": b"", "mac": src_mac,
                     "isn": TCP_ISN,
                     # SACK may only ever be *sent* to a peer that offered
                     # SACK-permitted in its SYN (RFC 2018 s4).  The flag is
                     # retained here because everything after the handshake
                     # depends on it; pre-U6 it was parsed, logged, and lost.
                     "sack_perm": sack, "ooo": [], "fin_seq": None}
                self.tcp[key] = c
                # A fresh SYN supersedes any tombstone for this tuple: the
                # guest's ephemeral wheel (40000..64999) wraps, and the new
                # connection's sequence space must not be answered with the
                # old one's final ACK.
                self.tcp_closed.pop(key, None)
                self.stats["tcp"] += 1
                self.log("  TCP SYN from %s:%d -> %d (peer mss=%d sack=%s) "
                         "-> SYN-ACK isn=0x%08x"
                         % (ipstr(ipsrc), t["sport"], t["dport"], mss,
                            "yes" if sack else "no", TCP_ISN))
            else:
                # A retransmitted SYN: answer it again, identically.  The
                # guest only retransmits after an RTO, so this is the path
                # that turns a lost SYN-ACK into a connection instead of a
                # `[tcp] timeout waiting for SYN-ACK`.
                c["snd_nxt"] = c["isn"] + 1
                c["rcv_nxt"] = (t["seq"] + 1) & 0xFFFFFFFF
                c["state"] = "SYN_RCVD"
                # The sequencing state restarts with the handshake: queued
                # runs were measured against the old RCV.NXT and a stale
                # FIN position would retransmit a FIN that no longer
                # belongs to this incarnation of the tuple.
                c["ooo"] = []
                c["fin_seq"] = None
                c["sack_perm"] = tcp_parse_options(t["opts"])[1]
                self.log("  TCP SYN (retransmit) from port %d -> SYN-ACK again"
                         % t["sport"])
            seg = build_tcp(t["dport"], t["sport"], c["isn"], c["rcv_nxt"],
                            TCP_SYN | TCP_ACK, ipdst, ipsrc,
                            opts=tcp_build_options())
            ip = build_ipv4(IP_PROTO_TCP, ipdst, ipsrc, seg, ident=ident)
            return [build_eth(src_mac, self.gw_mac, ETHERTYPE_IPV4, ip)]

        if c is None:
            tomb = self.tcp_closed.get(key)
            if tomb is not None and (flags & TCP_FIN) and \
                    seq_diff((t["seq"] + len(t["data"])) & 0xFFFFFFFF,
                             tomb["rcv_nxt"]) == -1:
                # A late duplicate of the FIN that closed this tuple, after
                # the tuple was freed: answer the final ACK once more and
                # keep the tombstone.  This is the treatment RFC 9293
                # prescribes in TIME_WAIT ("the only thing that can arrive
                # is a retransmission of the remote FIN.  Acknowledge it"),
                # minus the 2MSL clock -- the retention bound is a count,
                # because this file's replies stay a pure function of the
                # frames seen.  A plain ACK, not our FIN again: the final
                # ACK we already received proves the guest saw the FIN, so
                # what it is missing must be the ACK.
                self.log("  TCP FIN (late duplicate) from port %d -> final "
                         "ACK again" % t["sport"])
                seg = build_tcp(t["dport"], t["sport"], tomb["snd_nxt"],
                                tomb["rcv_nxt"], TCP_ACK, ipdst, ipsrc)
                ip = build_ipv4(IP_PROTO_TCP, ipdst, ipsrc, seg, ident=ident)
                return [build_eth(tomb["mac"], self.gw_mac, ETHERTYPE_IPV4,
                                  ip)]
            # No connection: whatever this is, it gets no answer (see the bare
            # ACK rule above -- inventing an RST here would hand the guest's
            # closer a segment it would misread).
            return []

        if c["state"] == "LAST_ACK":
            # RFC 9293: "The only thing that can arrive in this state is an
            # acknowledgment of our FIN.  If our FIN is now acknowledged,
            # delete the TCB."  Everything else is a retransmission from a
            # peer that has not seen (or has not yet had answered) some part
            # of our close -- and forgetting the tuple is the one response
            # that can help none of those peers.
            if (flags & TCP_ACK) and not (flags & TCP_FIN) \
                    and t["ack"] == c["snd_nxt"]:
                # The guest ACKed our FIN: it is in TIME_WAIT now and will
                # send nothing more except possibly a retransmission of its
                # FIN.  Forget the tuple -- keeping it would make a later
                # stray segment look like it belonged to a live connection --
                # but keep a tombstone, so that FIN retransmission is met
                # with the final ACK rather than with unknown-tuple silence.
                self.log("  TCP close complete: port %d (tuple freed)"
                         % t["sport"])
                self.tcp_forget(key, tombstone=True)
                return []
            if (flags & TCP_FIN) and \
                    seq_diff((t["seq"] + len(t["data"])) & 0xFFFFFFFF,
                             c["rcv_nxt"]) == -1:
                # A retransmission of the FIN we already consumed: the peer
                # never saw our FIN|ACK (its ack predates our FIN -- had it
                # seen the FIN, this segment would be the final ACK above).
                # Pre-U6 this freed the tuple, stranding the guest in
                # FIN_WAIT_2 until its close timed out.  A stack with a
                # retransmission timer would simply resend its FIN; we have
                # no timers by design, so the peer's retransmission is our
                # clock: send the same FIN|ACK again and stay in LAST_ACK.
                self.log("  TCP FIN (duplicate) from port %d -> FIN-ACK again"
                         % t["sport"])
                return [self.tcp_segment(c, key, TCP_FIN | TCP_ACK,
                                         seq=c["fin_seq"], ident=ident)]
            if t["data"] or (flags & TCP_FIN):
                # Text after the peer's FIN, or a FIN somewhere unexpected:
                # the text is ignored (the stream has ended), but an
                # out-of-place segment still earns the challenge ACK
                # <SEQ=SND.NXT><ACK=RCV.NXT> of the first acceptability step.
                return [self.tcp_ack_frame(c, key, ident)]
            return []          # a stale ACK that does not ack our FIN: the
                               # bare-ACK rule (silence) applies as anywhere

        if (flags & TCP_ACK) and c["state"] == "SYN_RCVD":
            c["state"] = "ESTAB"
            self.log("  TCP handshake complete: %s:%d <-> :%d ESTABLISHED "
                     "(%d connection(s) open)"
                     % (ipstr(ipsrc), t["sport"], t["dport"], len(self.tcp)))

        delivered, fin_now, want_ack, trigger = self.tcp_receive(c, t)

        out = []
        answered = False
        if delivered and t["dport"] == DNS_PORT:
            answered, frames = self.dns_over_tcp(
                c, key, ident, opts=self.tcp_sack_opts(c, trigger))
            out += frames
        # A bare ACK goes out only when there is nothing to piggyback it
        # on: an answer to the data acknowledges the data itself (its
        # rcv_nxt already advanced in tcp_receive), and sending both would
        # spend two segments -- and two of the guest's RX descriptors -- on
        # one round trip.  The same holds when our FIN|ACK below *is* the
        # acknowledgment and no data came with the peer's FIN: the guest's
        # closer is built for a single FIN-ACK (see the docstring).
        if want_ack and not answered and not (fin_now and not delivered):
            out.append(self.tcp_ack_frame(c, key, ident, trigger))

        if fin_now:
            c["state"] = "LAST_ACK"
            fin = self.tcp_segment(c, key, TCP_FIN | TCP_ACK, ident=ident)
            c["fin_seq"] = c["snd_nxt"]
            c["snd_nxt"] = (c["snd_nxt"] + 1) & 0xFFFFFFFF
            self.log("  TCP FIN from port %d -> FIN-ACK (closing)" % t["sport"])
            out.append(fin)
        return out

    def dns_over_tcp(self, c, key, ident, opts=b""):
        """RFC 1035 s4.2.2 carriage: a 2-byte length prefix, then the same
        message.  The guest's `dns_wire_query_tcp` sends the prefix and the
        query as two separate `tcp_send_h` calls and accumulates the reply
        across segment boundaries, so both directions have to be framed rather
        than assumed to be one segment per message.

        -> (answered_anything, frames).  The flag tells the caller whether an
        ACK already went out piggybacked on an answer."""
        out = []
        answered = False
        while len(c["buf"]) >= 2:
            mlen = struct.unpack("!H", c["buf"][0:2])[0]
            if mlen < 12 or mlen > 4096:
                self.log("  TCP DNS: implausible message length %d, dropping "
                         "the stream" % mlen)
                c["buf"] = b""
                return answered, out
            if len(c["buf"]) < 2 + mlen:
                return answered, out          # incomplete: wait for more
            msg = c["buf"][2:2 + mlen]
            c["buf"] = c["buf"][2 + mlen:]
            q = dns_parse_query(msg)
            if not q:
                self.stats["ignored"] += 1
                self.log("  TCP DNS: malformed query ignored")
                continue
            if q["qtype"] != DNS_QTYPE_A:
                self.stats["ignored"] += 1
                self.log("  TCP DNS query for '%s' type %d ignored (only A is "
                         "answered)" % (q["qname"], q["qtype"]))
                continue
            if "dns" in self.drop:
                self.log("  [drop] TCP DNS A query for '%s' (negative control)"
                         % q["qname"])
                continue
            reply = dns_build_reply(q, self.dns_answer_ip)
            body = struct.pack("!H", len(reply)) + reply
            self.stats["dns"] += 1
            self.log("  TCP DNS A? %s (id 0x%04x) -> %s (%d-byte message)"
                     % (q["qname"], q["id"], ipstr(self.dns_answer_ip),
                        len(reply)))
            seg = self.tcp_segment(c, key, TCP_ACK | TCP_PSH, data=body,
                                   ident=ident, opts=opts)
            c["snd_nxt"] = (c["snd_nxt"] + len(body)) & 0xFFFFFFFF
            out.append(seg)
            answered = True
        return answered, out

    def handle_many(self, frame: bytes):
        """Like `handle`, but a list: a TCP segment can legitimately need more
        than one reply (an ACK for the data plus the answer it carried)."""
        p = parse_eth(frame)
        if not p:
            return []
        _dst, src, et, payload = p
        if et != ETHERTYPE_IPV4:
            r = self.handle(frame)
            return [r] if r else []
        v = parse_ipv4(payload)
        if not v or v[0] != IP_PROTO_TCP:
            r = self.handle(frame)
            return [r] if r else []
        # Trim to ip.total_length before parsing: a 20-byte-header segment
        # plus Ethernet padding is not the segment the checksum was over.
        return self.handle_tcp(src, v[1], v[2], ipv4_body(payload, v[3]), v[4])


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(
        description="the host side of the emulator's TAP socket (NET N2)")
    ap.add_argument("--mode", choices=("slirp", "capture"), default="slirp",
                    help="capture = N1's record-only lane")
    ap.add_argument("--host", default="127.0.0.1")
    ap.add_argument("--port", type=int, default=8080)
    ap.add_argument("--gw-mac", default=GW_MAC_DEFAULT,
                    help="gateway/DHCP-server MAC (ledger D7: unmeasured)")
    ap.add_argument("--out", help="hex frame log of everything received")
    ap.add_argument("--count", type=int, default=0,
                    help="stop after N received frames (0 = until --timeout)")
    ap.add_argument("--timeout", type=float, default=0.0,
                    help="seconds to run; 0 = until interrupted")
    ap.add_argument("--dns-ip", default=DNS_ANSWER_IP_DEFAULT,
                    help="A record handed out for every DNS query")
    ap.add_argument("--udp-checksum", action="store_true",
                    help="compute the DHCP reply's UDP checksum (the guest "
                         "never validates it and sends 0 itself)")
    ap.add_argument("--drop", default="",
                    help="comma list of replies to withhold: arp,icmp,dhcp,dns "
                         "(the negative-control lever)")
    ap.add_argument("--no-tcp", action="store_true",
                    help="ignore TCP entirely -- no SYN-ACK, no FIN-ACK (the "
                         "negative-control lever for the connection table; "
                         "--drop=dns is not enough, because the handshakes "
                         "would still succeed and only the query would hang)")
    ap.add_argument("--quiet", action="store_true")
    a = ap.parse_args(argv)

    r = Responder(mac(a.gw_mac), a.udp_checksum, a.quiet, a.drop, a.dns_ip,
                  a.no_tcp)

    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    s.bind((a.host, a.port))
    if not a.quiet:
        print("responder: mode=%s bound %s:%d gw-ip=%s gw-mac=%s"
              % (a.mode, a.host, a.port, ipstr(ip4(GW_IP)), a.gw_mac),
              flush=True)

    out = open(a.out, "w", encoding="ascii") if a.out else None
    seen = 0
    deadline = (time.monotonic() + a.timeout) if a.timeout else None
    try:
        while True:
            if deadline is not None:
                remain = deadline - time.monotonic()
                if remain <= 0:
                    break
                s.settimeout(remain)
            try:
                frame, peer = s.recvfrom(65536)
            except socket.timeout:
                break
            if not frame:
                continue
            seen += 1
            if out:
                out.write(binascii.hexlify(frame).decode("ascii") + "\n")
                out.flush()
            if a.mode != "capture":
                for reply in r.handle_many(frame):
                    # Back to the PEER, not to (host, port): the endpoint we
                    # bound is the one the emulator connected *to*, so sending
                    # there delivers the reply to ourselves.  Measured the hard
                    # way -- the first N2 wire run had the responder answering
                    # its own OFFER while the guest printed
                    # `[dhcp] FAIL: no OFFER received`.
                    s.sendto(reply, peer)
            if a.count and seen >= a.count:
                break
    except KeyboardInterrupt:
        pass
    finally:
        if out:
            out.close()
        s.close()
    if not a.quiet:
        print("responder: %d frame(s) in, replies arp=%d dhcp=%d icmp=%d "
              "dns=%d tcp=%d ignored=%d"
              % (seen, r.stats["arp"], r.stats["dhcp"], r.stats["icmp"],
                 r.stats["dns"], r.stats["tcp"], r.stats["ignored"]),
              flush=True)
    return 0


if __name__ == "__main__":
    sys.exit(main())
