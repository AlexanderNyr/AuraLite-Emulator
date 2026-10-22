#!/usr/bin/env python3
"""tests/test_tap_responder.py -- NET N2 protocol vectors for the responder.

The wire lane (`tests/integration/n2_responder.sh`) proves the responder works
against a real guest through the real socket, but it cannot say *why* a reply
was accepted.  These vectors close that gap: they rebuild the guest's own
request frames byte for byte from AuraLite-OS@0ed0d29 `kernel/net/net.c`, feed
them to `tools/tap_responder.Responder`, and then accept or reject the reply
using **the guest's filters ported verbatim** -- same comparisons, same order,
same minimum lengths.  So a reply that passes here is a reply the frozen guest
would consume, and the constants that matter (ports 67/68, options 1/3/6/53/54,
`yiaddr`, `xid` echo, `opcode == 2`, `icmp.type == 0`) are each pinned by an
assertion rather than implied by a working run.

Also pinned: the RFC 1071 checksums (the guest computes them with
`checksum()` at net.c:171 and a wrong one is exactly the class of bug that
would pass a lenient peer), the 60-byte minimum frame pad the guest itself
applies in `net_eth_send`, and the `--drop` lever the negative control uses.

Run: python3 tests/test_tap_responder.py   (RC=0 = all passed)
"""
import os
import struct
import sys

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)),
                                "..", "tools"))
import tap_responder as R  # noqa: E402

FAILED = []


def check(cond, label):
    if cond:
        print("ok   %s" % label)
    else:
        print("FAIL %s" % label)
        FAILED.append(label)


# ---------------------------------------------------------------------------
# The guest's side, rebuilt from kernel/net/net.c (0ed0d29).
# ---------------------------------------------------------------------------
OUR_MAC = bytes([0x52, 0x54, 0x00, 0x12, 0x34, 0x56])   # RAL/RAH preload (N0)
BCAST_MAC = b"\xff" * 6
BCAST_IP = b"\xff\xff\xff\xff"


def g_checksum(data: bytes) -> int:
    """net.c:171 `checksum()`: 16-bit BE words, carries folded, complemented.

    The odd tail is `p[0] << 8`, NOT `p[0]` -- RFC 1071 treats a trailing byte
    as the high half of a word whose low half is zero.  Getting this wrong is
    invisible while every length is even (IP header 20, ICMP echo 40) and shows
    up as a sum that is off by 0xFF00 the first time an odd-length segment
    appears, which for this guest is a 49-byte TCP segment carrying a 29-byte
    DNS query.  Both this transcription and the payload's own `checksum()` were
    fixed in N3 for exactly that reason."""
    total = 0
    i = 0
    n = len(data)
    while i + 1 < n:
        total += (data[i] << 8) | data[i + 1]
        i += 2
    if i < n:
        total += data[i] << 8       # the guest's own tail rule (net.c:181)
    while total >> 16:
        total = (total & 0xFFFF) + (total >> 16)
    return (~total) & 0xFFFF


def g_eth(payload: bytes, ethertype: int, dst: bytes = BCAST_MAC) -> bytes:
    """net.c:293 `net_eth_send`: src = our MAC, ethertype BE, pad to 60."""
    frame = dst + OUR_MAC + struct.pack("!H", ethertype) + payload
    if len(frame) < 60:
        frame += b"\x00" * (60 - len(frame))
    return frame


def g_ipv4(proto: int, src: bytes, dst: bytes, body: bytes, ident: int) -> bytes:
    """net.c's ipv4_hdr builder: v4/ihl5, tos 0, flags_frag 0, ttl 64."""
    hdr = struct.pack("!BBHHHBBH", 0x45, 0, 20 + len(body), ident, 0, 64,
                      proto, 0) + src + dst
    return hdr[:10] + struct.pack("!H", g_checksum(hdr)) + hdr[12:] + body


def g_dhcp_option(buf: bytearray, code: int, data: bytes) -> None:
    """net.c:764 `dhcp_add_option` (END carries no length byte)."""
    buf.append(code)
    if code != 255:
        buf.append(len(data))
        buf += data


def g_dhcp_frame(msgtype: int, xid: int, extra=()) -> bytes:
    """net.c:801 net_dhcp's DISCOVER / REQUEST builder, byte for byte:
    236-byte fixed part + options + END, over UDP 68->67 checksum 0, over IP
    0.0.0.0 -> 255.255.255.255 ident 0x1234/0x1235, over a broadcast frame."""
    body = bytearray(236)
    body[0] = 1                                  # BOOTREQUEST
    body[1] = 1                                  # htype = Ethernet
    body[2] = 6                                  # hlen
    struct.pack_into("!I", body, 4, xid)
    struct.pack_into("!H", body, 10, 0x8000)     # flags: broadcast requested
    body[28:34] = OUR_MAC                        # chaddr
    struct.pack_into("!I", body, 232, 0x63825363)  # magic cookie

    opts = bytearray()
    g_dhcp_option(opts, 53, bytes([msgtype]))
    if msgtype == 1:                             # DISCOVER: parameter list
        g_dhcp_option(opts, 55, bytes([1, 3, 6]))
    for code, data in extra:
        g_dhcp_option(opts, code, data)
    g_dhcp_option(opts, 255, b"")

    udp_len = 8 + 236 + len(opts)
    udp = struct.pack("!HHHH", 68, 67, udp_len, 0) + bytes(body) + bytes(opts)
    ip = g_ipv4(17, b"\x00\x00\x00\x00", BCAST_IP, udp,
                0x1234 if msgtype == 1 else 0x1235)
    return g_eth(ip, 0x0800)


def g_arp_frame(target_ip: bytes) -> bytes:
    """net.c:415 net_arp_resolve's request: broadcast, sender = us."""
    payload = struct.pack("!HHBBH", 1, 0x0800, 6, 4, 1)
    payload += OUR_MAC + R.ip4(R.GUEST_IP) + b"\x00" * 6 + target_ip
    return g_eth(payload, 0x0806)


def g_icmp_frame(target_ip: bytes, dst_mac: bytes) -> bytes:
    """net.c:476 net_ping: ident 0x1234, seq 1, payload 0x41..0x60 (32 B)."""
    data = bytes((i + 0x41) & 0xFF for i in range(32))
    icmp = struct.pack("!BBHHH", 8, 0, 0, 0x1234, 1) + data
    icmp = icmp[:2] + struct.pack("!H", g_checksum(icmp)) + icmp[4:]
    ip = g_ipv4(1, R.ip4(R.GUEST_IP), target_ip, icmp, 1)
    return g_eth(ip, 0x0800, dst=dst_mac)


DNS_LOCAL_PORT = 12345      # kernel/net/dns.h:19
DNS_SERVER_IP = (10, 0, 2, 3)


def g_dns_encode_name(name: str) -> bytes:
    """dns_parse.c:37 `dns_encode_name`: lowercased, length-prefixed labels,
    NUL-terminated.  Independent of the responder's own encoder."""
    out = bytearray()
    for label in name.split("."):
        out.append(len(label))
        out += label.lower().encode("ascii")
    out.append(0)
    return bytes(out)


def g_dns_message(qid: int, name: str, qtype: int = 1) -> bytes:
    """dns.c:120 `dns_build_query_type`: id, RD, QDCOUNT=1, encoded QNAME,
    QTYPE, QCLASS=IN."""
    qname = g_dns_encode_name(name)
    return (struct.pack("!HHH", qid, 0x0100, 1) + b"\x00" * 6 + qname
            + struct.pack("!HH", qtype, 1))


def g_dns_frame(qid: int, name: str = "example.com", qtype: int = 1) -> bytes:
    """dns.c:144 `dns_wire_query` -> net.c `net_udp_sendto`: UDP 12345->53,
    checksum 0, ip.ident 2, ttl 64, unicast to the resolved server MAC."""
    msg = g_dns_message(qid, name, qtype)
    udp = struct.pack("!HHHH", DNS_LOCAL_PORT, 53, 8 + len(msg), 0) + msg
    ip = g_ipv4(17, R.ip4(R.GUEST_IP), R.ip4(DNS_SERVER_IP), udp, 2)
    return g_eth(ip, 0x0800, dst=R.mac("52:55:0a:00:02:03"))


# ---------------------------------------------------------------------------
# The guest's accept filters, ported from net.c.  A reply passes only if the
# frozen kernel's own comparisons would pass.
# ---------------------------------------------------------------------------

def guest_accepts_arp_reply(frame: bytes):
    """net.c:441-465 (local-subnet path)."""
    if len(frame) < 14 + 28:
        return False, "too short (%d)" % len(frame)
    if struct.unpack("!H", frame[12:14])[0] != 0x0806:
        return False, "ethertype"
    if struct.unpack("!H", frame[14 + 6:14 + 8])[0] != 2:      # opcode
        return False, "opcode != ARP_REPLY"
    return True, frame[22:28]                                   # sender_mac


def guest_accepts_dhcp_reply(frame: bytes, want_msgtype: int, xid: int):
    """net.c:902-935 (OFFER) and 1046-1075 (ACK).  Same gates, in order."""
    if len(frame) < 14 + 20 + 8 + 236:
        return False, "too short (%d)" % len(frame)
    if struct.unpack("!H", frame[12:14])[0] != 0x0800:
        return False, "ethertype"
    if frame[14 + 9] != 17:
        return False, "ip.protocol != UDP"
    sport, dport = struct.unpack("!HH", frame[34:38])
    if sport != 67:
        return False, "udp.src != 67"
    if dport != 68:
        return False, "udp.dst != 68"
    dhcp = frame[42:]
    opts_len = len(frame) - (14 + 20 + 8 + 236)
    if opts_len < 4:
        return False, "opts_len < 4"
    mt = R.dhcp_find_option(dhcp[236:], 53)
    if not mt or len(mt) != 1:
        return False, "option 53 missing/short"
    if want_msgtype == 5 and struct.unpack("!I", dhcp[4:8])[0] != xid:
        return False, "xid not echoed"          # the ACK path checks xid
    if mt[0] != want_msgtype:
        return False, "message type %d != %d" % (mt[0], want_msgtype)
    return True, dhcp


def guest_accepts_icmp_reply(frame: bytes):
    """net.c:534-556."""
    if len(frame) < 14 + 20 + 8:
        return False, "too short (%d)" % len(frame)
    if struct.unpack("!H", frame[12:14])[0] != 0x0800:
        return False, "ethertype"
    if frame[14 + 9] != 1:
        return False, "ip.protocol != ICMP"
    if frame[34] != 0:
        return False, "icmp.type != ECHO_REPLY"
    return True, frame


def guest_dns_name_read(msg: bytes, pos: int):
    """dns_parse.c:65 `dns_name_read`: labels plus compression pointers with a
    jump budget.  -> (dotted lowercase name, offset just past the name at the
    starting level) or None."""
    out = []
    jumps = 0
    first_end = None
    cur = pos
    while True:
        if cur >= len(msg):
            return None
        b = msg[cur]
        if (b & 0xC0) == 0xC0:
            if cur + 1 >= len(msg):
                return None
            ptr = ((b & 0x3F) << 8) | msg[cur + 1]
            if ptr >= len(msg):
                return None
            if first_end is None:
                first_end = cur + 2
            jumps += 1
            if jumps > 16:
                return None
            cur = ptr
            continue
        if b == 0:
            if first_end is None:
                first_end = cur + 1
            return ".".join(out).lower(), first_end
        if cur + 1 + b > len(msg):
            return None
        out.append(msg[cur + 1:cur + 1 + b].decode("ascii", "replace").lower())
        cur += 1 + b


def guest_dns_parse_response(msg: bytes, qname: str, expect_id: int):
    """dns_parse.c:132 `dns_parse_response`, transcribed.  -> (verdict, a_ip,
    a_ttl) where verdict is "ANSWER", "TRUNCATED", "NXDOMAIN", "NODATA" or
    "BAD".  Only the A path matters here, but the gates are all reproduced in
    order because a reply that slips past a missing one is exactly the bug this
    suite exists to prevent."""
    if len(msg) < 12:
        return "BAD", 0, 0
    if struct.unpack("!H", msg[0:2])[0] != expect_id:
        return "BAD", 0, 0
    flags = struct.unpack("!H", msg[2:4])[0]
    if not flags & 0x8000:
        return "BAD", 0, 0                       # QR
    if flags & 0x0200:
        return "TRUNCATED", 0, 0                 # TC -> the TCP retry
    rcode = flags & 0x000F
    qd, an, ns = struct.unpack("!HHH", msg[4:10])
    pos = 12
    for _ in range(qd):
        r = guest_dns_name_read(msg, pos)
        if not r:
            return "BAD", 0, 0
        pos = r[1] + 4
        if pos > len(msg):
            return "BAD", 0, 0
    expected = qname.lower()
    chain_min = 0xFFFFFFFF
    chained = False
    for _ in range(an):
        r = guest_dns_name_read(msg, pos)
        if not r:
            return "BAD", 0, 0
        owner, pos = r
        if pos + 10 > len(msg):
            return "BAD", 0, 0
        rtype, klass, ttl, rdlen = struct.unpack("!HHIH", msg[pos:pos + 10])
        rdata = pos + 10
        if rdata + rdlen > len(msg):
            return "BAD", 0, 0
        if klass == 1 and rtype == 5 and owner == expected:      # CNAME
            t = guest_dns_name_read(msg, rdata)
            if not t:
                return "BAD", 0, 0
            chain_min = min(chain_min, ttl)
            expected = t[0]
            chained = True
        elif klass == 1 and rtype == 1 and rdlen == 4 and owner == expected:
            a_ip = struct.unpack("!I", msg[rdata:rdata + 4])[0]
            return "ANSWER", a_ip, min(ttl, chain_min if chained else ttl)
        pos = rdata + rdlen
    if rcode == 3:
        return "NXDOMAIN", 0, 0
    if chained:
        return "CNAME", 0, 0
    return "NODATA", 0, 0


def guest_accepts_dns_reply(frame: bytes, qid: int, qname: str):
    """net.c `net_udp_recvfrom` gates + dns.c:172 (`got_ip`/`got_port`) +
    dns.c:177 (the id must be ours).  -> (ok, reason, a_ip, a_ttl)."""
    if len(frame) < 14 + 20 + 8:
        return False, "too short (%d)" % len(frame), 0, 0
    if struct.unpack("!H", frame[12:14])[0] != 0x0800:
        return False, "ethertype", 0, 0
    if frame[14 + 9] != 17:
        return False, "ip.protocol != UDP", 0, 0
    ip_total = struct.unpack("!H", frame[16:18])[0]
    if ip_total != len(frame) - 14:
        return False, "ip.total_length %d != %d" % (ip_total, len(frame) - 14), 0, 0
    sport, dport, ulen = struct.unpack("!HHH", frame[34:40])
    if dport != DNS_LOCAL_PORT:
        return False, "udp.dst %d != %d" % (dport, DNS_LOCAL_PORT), 0, 0
    if sport != 53:
        return False, "udp.src %d != 53 (dns.c:172 got_port)", 0, 0
    if ulen < 8:
        return False, "udp.length < 8", 0, 0
    if 14 + 20 + ulen > len(frame):
        return False, "udp.length past the frame", 0, 0
    if frame[26:30] != R.ip4(DNS_SERVER_IP):
        return False, "ip.src != 10.0.2.3 (dns.c:172 got_ip)", 0, 0
    msg = frame[42:34 + ulen]
    verdict, a_ip, a_ttl = guest_dns_parse_response(msg, qname, qid)
    if verdict != "ANSWER":
        return False, "dns_parse_response -> %s" % verdict, 0, 0
    return True, "ANSWER", a_ip, a_ttl


TCP_WINDOW_GUEST = 64240      # tcp.c:44
TCP_MSS_GUEST = 1460          # NETL3_V4_MSS, netl3.h:35
TCP_X5_CONNS = 16             # tcp.h:53
DNS_SERVER_MAC = R.mac("52:55:0a:00:02:03")


def g_tcp_segment(sport, dport, seq, ack, flags, opts=b"", data=b""):
    """tcp.c:161 `tcp_send_segment_at`: window TCP_WINDOW, urgent 0, options
    ride the SYN only, and the checksum is the pseudo-header sum the L3 output
    path fills in.  Built with `g_ipv4`/`g_eth` so it goes through the guest's
    own (corrected) checksum and its 60-byte pad."""
    hdr = struct.pack("!HHIIBBHHH", sport, dport, seq & 0xFFFFFFFF,
                      ack & 0xFFFFFFFF, ((20 + len(opts)) >> 2) << 4, flags,
                      TCP_WINDOW_GUEST, 0, 0) + opts + data
    seg = hdr[0:16] + struct.pack("!H", g_tcp_csum(hdr)) + hdr[18:]
    ip = g_ipv4(6, R.ip4(R.GUEST_IP), R.ip4(DNS_SERVER_IP), seg, 2)
    return g_eth(ip, 0x0800, dst=DNS_SERVER_MAC)


def g_tcp_csum(seg: bytes) -> int:
    pseudo = (R.ip4(R.GUEST_IP) + R.ip4(DNS_SERVER_IP)
              + struct.pack("!BBH", 0, 6, len(seg)))
    return g_checksum(pseudo + seg)


def g_tcp_syn_opts() -> bytes:
    """`tcpm6c_build_syn_opts`: MSS 1460 then SACK-permitted, padded to a
    4-byte edge -- tcp.c prints `[tcp] SYN options: 8 bytes, hdr=28,
    mss=1460 sack-perm`, so 8 bytes is the measured length."""
    return (bytes((2, 4)) + struct.pack("!H", TCP_MSS_GUEST)
            + bytes((4, 2)) + bytes(2))


def g_parse_tcp(frame: bytes):
    """Read a reply frame back into a TCP dict, options included.  The
    guest's filter below is what *accepts* a reply; this is what lets a
    vector say which bytes were in it."""
    return R.parse_tcp(R.ipv4_body(frame[14:], R.parse_ipv4(frame[14:])[3]))


def g_parse_sack(opts: bytes):
    """`tcpm6d_decode` (guest kernel/net/tcp_m6d.h), transcribed: END stops
    the walk, NOP skips a byte, a SACK option needs olen >= 10 and
    (olen - 2) % 8 == 0, at most TCPM6D_MAX_BLOCKS=4 blocks are taken, and
    a block whose end does not follow its start in *signed* sequence space
    ("accepting it would mark arbitrary queue entries acked") is skipped.
    -> [(left, right), ...] in wire order."""
    out = []
    i = 0
    while i < len(opts):
        kind = opts[i]
        if kind == 0:                       # TCPOPT_END
            break
        if kind == 1:                       # TCPOPT_NOP
            i += 1
            continue
        if i + 1 >= len(opts):
            break
        olen = opts[i + 1]
        if olen < 2 or i + olen > len(opts):
            break
        if kind == 5 and olen >= 10 and (olen - 2) % 8 == 0:
            q = i + 2
            for _ in range((olen - 2) // 8):
                if len(out) >= 4:
                    break
                s, e = struct.unpack("!II", opts[q:q + 8])
                q += 8
                d = (e - s) & 0xFFFFFFFF
                if d >= 0x80000000:
                    d -= 0x100000000
                if d <= 0:
                    continue
                out.append((s, e))
        i += olen
    return out


def guest_accepts_tcp_segment(frame: bytes, sport: int, dport: int):
    """tcp.c:290 `tcp_recv_segment_timeout`, transcribed.  -> (ok, reason,
    flags, seq, ack, window, data).  The 4-tuple and the header length are all
    it checks -- no seq/ack validation -- and the payload length comes from the
    IP total, not the frame, because the NIC pads short frames to 60."""
    if len(frame) < 14 + 20 + 20:
        return False, "too short (%d)" % len(frame), 0, 0, 0, 0, b""
    if struct.unpack("!H", frame[12:14])[0] != 0x0800:
        return False, "ethertype", 0, 0, 0, 0, b""
    if frame[14 + 9] != 6:
        return False, "ip.protocol != TCP", 0, 0, 0, 0, b""
    ip_total = struct.unpack("!H", frame[16:18])[0]
    ip_ihl = (frame[14] & 0x0F) * 4
    if ip_total < ip_ihl + 20 or 14 + ip_total > len(frame):
        return False, "ip.total_length %d" % ip_total, 0, 0, 0, 0, b""
    if frame[26:30] != R.ip4(DNS_SERVER_IP):
        return False, "ip.src != the peer", 0, 0, 0, 0, b""
    t = frame[34:14 + ip_total]
    tsport, tdport, seq, ack = struct.unpack("!HHII", t[0:12])
    off_flags, window = struct.unpack("!HH", t[12:16])
    if tsport != dport:
        return False, "tcp.src_port %d != %d" % (tsport, dport), 0, 0, 0, 0, b""
    if tdport != sport:
        return False, "tcp.dst_port %d != %d" % (tdport, sport), 0, 0, 0, 0, b""
    words = off_flags >> 12
    if words < 5 or words > 15:
        return False, "data_offset %d words" % words, 0, 0, 0, 0, b""
    hdr = words * 4
    if hdr > len(t):
        return False, "header past the segment", 0, 0, 0, 0, b""
    if g_tcp_csum(t[0:16] + b"\x00\x00" + t[18:]) != struct.unpack("!H", t[16:18])[0]:
        return False, "tcp checksum", 0, 0, 0, 0, b""
    return True, "ok", off_flags & 0x1FF, seq, ack, window, t[hdr:]


def tcp_handshake(r, sport, isn, dport=53):
    """Run the guest's `tcp_open_addr` against `r`.  -> (ok, reason, our_isn)."""
    out = r.handle_many(g_tcp_segment(sport, dport, isn, 0, R.TCP_SYN,
                                      opts=g_tcp_syn_opts()))
    if len(out) != 1:
        return False, "SYN got %d replies" % len(out), 0
    ok, why, flags, seq, ack, window, _d = \
        guest_accepts_tcp_segment(out[0], sport, dport)
    if not ok:
        return False, "SYN-ACK rejected: " + why, 0
    if (flags & (R.TCP_SYN | R.TCP_ACK)) != (R.TCP_SYN | R.TCP_ACK):
        return False, "flags 0x%02x are not SYN-ACK" % flags, 0
    if ack != (isn + 1) & 0xFFFFFFFF:
        return False, "SYN-ACK acks %d, wanted %d" % (ack, isn + 1), 0
    if window == 0:
        return False, "zero window", 0
    # the guest's final ACK -- it must be met with silence
    out = r.handle_many(g_tcp_segment(sport, dport, isn + 1, seq + 1, R.TCP_ACK))
    if out:
        return False, "the final ACK was answered (%d frame(s))" % len(out), 0
    return True, "ESTABLISHED", seq


def tcp_close(r, sport, our_isn, isn, dport=53):
    """Run the guest's `tcp_close` from ESTABLISHED.  -> (ok, reason)."""
    out = r.handle_many(g_tcp_segment(sport, dport, isn + 1, our_isn + 1,
                                      R.TCP_FIN | R.TCP_ACK))
    if len(out) != 1:
        return False, "FIN got %d replies, wanted one FIN-ACK" % len(out)
    ok, why, flags, seq, ack, _w, _d = \
        guest_accepts_tcp_segment(out[0], sport, dport)
    if not ok:
        return False, "FIN-ACK rejected: " + why
    if (flags & (R.TCP_FIN | R.TCP_ACK)) != (R.TCP_FIN | R.TCP_ACK):
        return False, "flags 0x%02x are not FIN-ACK (tcp_close needs both: " \
                      "ACK -> FIN_WAIT_2, FIN -> TIME_WAIT)" % flags
    if ack != (isn + 2) & 0xFFFFFFFF:
        return False, "FIN-ACK acks %d, wanted %d" % (ack, isn + 2)
    out = r.handle_many(g_tcp_segment(sport, dport, isn + 2, seq + 1, R.TCP_ACK))
    if out:
        return False, "the closing ACK was answered (%d frame(s))" % len(out)
    return True, "closed"


# ---------------------------------------------------------------------------
# Vectors
# ---------------------------------------------------------------------------

def test_frames_match_the_guest_builder():
    """Sanity on the test's own side: the frames it feeds are what net.c
    builds, including the pad and the header checksums.  Without this the
    vectors below could be passing against a request no guest ever sends."""
    d = g_dhcp_frame(1, 0x12345678)
    # 14 eth + 20 ip + 8 udp + 236 dhcp fixed + options 53(3) 55(5) END(1)
    check(len(d) == 14 + 20 + 8 + 236 + 3 + 5 + 1,
          "DISCOVER frame length %d (236 fixed + 53/55/END)" % len(d))
    ip = d[14:34]
    check(g_checksum(ip) == 0, "DISCOVER IP header checksum verifies to 0")
    check(struct.unpack("!H", d[40:42])[0] == 0,
          "DISCOVER UDP checksum is 0 (what the guest sends)")
    check(struct.unpack("!HH", d[34:38]) == (68, 67),
          "DISCOVER is UDP 68 -> 67 (net.c:866)")
    arp = g_arp_frame(R.ip4(R.GW_IP))
    check(len(arp) == 60, "ARP request padded to the 60-byte minimum (%d)"
          % len(arp))
    ic = g_icmp_frame(R.ip4(R.GW_IP), bytes(6))
    check(len(ic) == 14 + 20 + 8 + 32, "ICMP request is 74 bytes")
    check(g_checksum(ic[34:34 + 40]) == 0,
          "ICMP request checksum verifies to 0")
    check(ic[42:74] == bytes((i + 0x41) for i in range(32)),
          "ICMP request payload is 0x41..0x60 as net.c:481 builds it")


def test_arp_reply():
    r = R.Responder(R.mac(R.GW_MAC_DEFAULT), quiet=True)
    reply = r.handle(g_arp_frame(R.ip4(R.GW_IP)))
    check(reply is not None, "ARP request for the gateway gets a reply")
    ok, why = guest_accepts_arp_reply(reply)
    check(ok, "ARP reply passes the guest's filter (%s)" % why)
    if ok:
        check(why == R.mac(R.GW_MAC_DEFAULT),
              "ARP reply sender_mac is the gateway MAC %s" % why)
        check(reply[14 + 24:14 + 28] == R.ip4(R.GUEST_IP),
              "ARP reply target_ip is the requester's address")
        check(reply[0:6] == BCAST_MAC,
              "ARP reply is broadcast (slirp's shape; the guest never checks)")
        check(len(reply) == 60, "ARP reply padded to 60 bytes (%d)" % len(reply))
    # An ARP request for something else is still answered (slirp answers for
    # the addresses it owns); a non-request is not.
    req_only = bytearray(g_arp_frame(R.ip4(R.GW_IP)))
    struct.pack_into("!H", req_only, 14 + 6, 2)       # opcode = reply
    check(r.handle(bytes(req_only)) is None,
          "an ARP *reply* from the guest is not answered (no loop)")


def test_dhcp_discover_offer():
    r = R.Responder(R.mac(R.GW_MAC_DEFAULT), quiet=True)
    frame = g_dhcp_frame(1, 0x12345678)
    reply = r.handle(frame)
    check(reply is not None, "DISCOVER gets an OFFER")
    ok, why = guest_accepts_dhcp_reply(reply, 2, 0)
    check(ok, "OFFER passes the guest's filter (%s)" % why)
    if not ok:
        return
    dhcp = why
    check(dhcp[0] == 2, "OFFER op = BOOTREPLY")
    check(dhcp[28:34] == OUR_MAC, "OFFER echoes chaddr")
    check(struct.unpack("!I", dhcp[4:8])[0] == 0x12345678,
          "OFFER echoes xid 0x12345678")
    yiaddr = dhcp[16:20]
    check(yiaddr == R.ip4(R.GUEST_IP),
          "OFFER yiaddr = %s (the receipt the guest's own integration test "
          "asserts)" % R.ipstr(yiaddr))
    opts = dhcp[236:]
    check(opts[0] == 53, "option 53 (message type) leads the list, as in slirp")
    sid = R.dhcp_find_option(opts, 54)
    check(sid == R.ip4(R.GW_IP),
          "option 54 server id = %s (net.c:929 reads it)" % R.ipstr(sid))
    sm = R.dhcp_find_option(opts, 1)
    check(sm == bytes([255, 255, 255, 0]),
          "option 1 subnet mask = 255.255.255.0 (net.c:933 -> subnet_mask, "
          "which is what makes the gateway on-link)")
    rtr = R.dhcp_find_option(opts, 3)
    check(rtr == R.ip4(R.GW_IP),
          "option 3 router = %s" % R.ipstr(rtr))
    dns = R.dhcp_find_option(opts, 6)
    check(dns == R.ip4(R.DNS_IP),
          "option 6 DNS = %s (net.c:938 -> dns_set_servers)" % R.ipstr(dns))
    lease = R.dhcp_find_option(opts, 51)
    check(lease is not None and len(lease) == 4,
          "option 51 lease time present (%s s)"
          % (struct.unpack("!I", lease)[0] if lease else "?"))
    check(opts[-1] == 255, "the option list ends with END (255)")
    # The reply is L2+L3 broadcast: the client holds no address yet, which is
    # the exact failure net.c:1010's comment names.
    check(reply[0:6] == BCAST_MAC and reply[30:34] == BCAST_IP,
          "OFFER is broadcast at L2 and L3 (a unicast ACK to an unconfigured "
          "address is the measured WHPX failure mode)")
    ip = reply[14:34]
    check(g_checksum(ip) == 0, "OFFER IP header checksum verifies to 0")
    total = struct.unpack("!H", ip[2:4])[0]
    check(total == len(reply) - 14,
          "OFFER ip.total_length %d == the %d bytes behind the Ethernet header"
          % (total, len(reply) - 14))
    ulen = struct.unpack("!H", reply[38:40])[0]
    check(ulen == total - 20, "OFFER udp.length %d == total_length - 20" % ulen)


def test_dhcp_request_ack():
    r = R.Responder(R.mac(R.GW_MAC_DEFAULT), quiet=True)
    xid = 0x12345678
    offer = r.handle(g_dhcp_frame(1, xid))
    ok, dhcp = guest_accepts_dhcp_reply(offer, 2, 0)
    check(ok, "OFFER accepted before the REQUEST is built")
    # The guest's REQUEST carries option 50 (the offered address) and 54 (the
    # server id it read), both big-endian on the wire.
    req = g_dhcp_frame(3, xid, extra=((50, dhcp[16:20]),
                                      (54, R.ip4(R.GW_IP))))
    reply = r.handle(req)
    check(reply is not None, "REQUEST gets an ACK")
    ok, why = guest_accepts_dhcp_reply(reply, 5, xid)
    check(ok, "ACK passes the guest's filter, xid included (%s)" % why)
    if ok:
        ack = why
        check(ack[16:20] == R.ip4(R.GUEST_IP),
              "ACK yiaddr = 10.0.2.15 -> our_ip")
        rtr = R.dhcp_find_option(ack[236:], 3)
        check(rtr == R.ip4(R.GW_IP),
              "ACK option 3 = 10.0.2.2 -> gateway_ip (net.c:1073)")
    # A mismatched xid must be ignored by the *guest*, and the responder must
    # still answer the xid it was given (it echoes, never invents).
    bad = r.handle(g_dhcp_frame(3, 0xDEADBEEF, extra=((50, dhcp[16:20]),)))
    ok2, _ = guest_accepts_dhcp_reply(bad, 5, xid)
    check(not ok2, "an ACK to a different xid is rejected by the guest filter")
    ok3, _ = guest_accepts_dhcp_reply(bad, 5, 0xDEADBEEF)
    check(ok3, "the responder echoes whatever xid it was given (no lease state)")


def test_icmp_echo_reply():
    gw_mac = R.mac(R.GW_MAC_DEFAULT)
    r = R.Responder(gw_mac, quiet=True)
    req = g_icmp_frame(R.ip4(R.GW_IP), gw_mac)
    reply = r.handle(req)
    check(reply is not None, "ICMP echo request gets a reply")
    ok, why = guest_accepts_icmp_reply(reply)
    check(ok, "echo reply passes the guest's filter (%s)" % why)
    if not ok:
        return
    ricmp = reply[34:]
    check(ricmp[0] == 0 and ricmp[1] == 0, "echo reply type 0 code 0")
    check(struct.unpack("!H", ricmp[4:6])[0] == 0x1234,
          "echo reply keeps ident 0x1234")
    check(struct.unpack("!H", ricmp[6:8])[0] == 1, "echo reply keeps seq 1")
    check(ricmp[8:40] == bytes((i + 0x41) for i in range(32)),
          "echo reply returns the 32 payload bytes unchanged (0x41..0x60)")
    check(g_checksum(ricmp[:40]) == 0,
          "echo reply ICMP checksum verifies to 0")
    rip = reply[14:34]
    check(g_checksum(rip) == 0, "echo reply IP header checksum verifies to 0")
    check(rip[12:16] == R.ip4(R.GW_IP) and rip[16:20] == R.ip4(R.GUEST_IP),
          "echo reply swapped src/dst (10.0.2.2 -> 10.0.2.15)")
    check(reply[6:12] == gw_mac and reply[0:6] == OUR_MAC,
          "echo reply is unicast to us from the gateway MAC")
    check(struct.unpack("!H", rip[4:6])[0] == 1,
          "echo reply keeps the request's ip.ident (1)")


def test_determinism_and_the_drop_lever():
    a = R.Responder(R.mac(R.GW_MAC_DEFAULT), quiet=True)
    b = R.Responder(R.mac(R.GW_MAC_DEFAULT), quiet=True)
    frames = [g_arp_frame(R.ip4(R.GW_IP)),
              g_dhcp_frame(1, 0x12345678),
              g_dhcp_frame(3, 0x12345678, extra=((50, R.ip4(R.GUEST_IP)),)),
              g_icmp_frame(R.ip4(R.GW_IP), R.mac(R.GW_MAC_DEFAULT))]
    check(all(a.handle(f) == b.handle(f) for f in frames),
          "two responder instances answer identically (no hidden state)")
    check(all(a.handle(f) == a.handle(f) for f in frames),
          "replaying the same frame answers identically (idempotent)")

    silent = R.Responder(R.mac(R.GW_MAC_DEFAULT), quiet=True,
                         drop="arp,icmp,dhcp")
    check(all(silent.handle(f) is None for f in frames),
          "--drop=arp,icmp,dhcp withholds every reply (the NC lever)")
    only_arp = R.Responder(R.mac(R.GW_MAC_DEFAULT), quiet=True, drop="icmp,dhcp")
    check(only_arp.handle(frames[0]) is not None and
          only_arp.handle(frames[3]) is None,
          "--drop is per-protocol, not all-or-nothing")


def test_junk_does_not_answer():
    r = R.Responder(R.mac(R.GW_MAC_DEFAULT), quiet=True)
    junk = [b"", b"\x00" * 13, b"\xff" * 14,
            g_eth(b"\x00" * 20, 0x86DD),                 # IPv6: not N2 scope
            g_eth(struct.pack("!BBHHHBBH", 0x45, 0, 40, 0, 0, 64, 6, 0)
                  + b"\x00" * 32, 0x0800),               # TCP: ignored
            g_dhcp_frame(1, 1)[:60]]                     # truncated DHCP
    check(all(r.handle(f) is None for f in junk),
          "junk / out-of-scope frames get no reply (nothing invented)")


ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def test_dns_query_answer():
    """`net_dns_self_test` runs at boot right after `net_self_test`
    (kernel/kernel.c:428-429) and the guest's `udptest` app asserts
    `UDPTEST PASS: example.com A .* from 10.0.2.3:53`, so the answer has to
    come back unicast, from 10.0.2.3:53, to the query's own source port --
    `net_udp_recvfrom` matches the *destination* port against 12345 and
    `dns_wire_query` rejects any other `got_ip`/`got_port`."""
    qid = 0xA5F1
    frame = g_dns_frame(qid)
    r = R.Responder(R.mac(R.GW_MAC_DEFAULT))
    reply = r.handle(frame)
    check(reply is not None, "an A query gets an answer")
    ok, why, a_ip, a_ttl = guest_accepts_dns_reply(reply, qid, "example.com")
    check(ok, "the reply passes the guest's UDP + DNS filters (%s)" % why)
    check(a_ip == 0x5DB8D822, "A record is 93.184.216.34 (0x%08x)" % a_ip)
    check(a_ttl == R.DNS_ANSWER_TTL, "TTL %d survives the CNAME/chain min()"
          % a_ttl)

    msg = reply[42:]
    check(struct.unpack("!H", msg[0:2])[0] == qid,
          "the id is echoed (dns.c:177 rejects an off-path answer)")
    flags = struct.unpack("!H", msg[2:4])[0]
    check(flags & 0x8000 and not flags & 0x0200,
          "QR set, TC clear (TC would send the guest to the TCP carriage)")
    check(flags & 0x000F == 0, "rcode NOERROR")
    qd, an, ns, ar = struct.unpack("!HHHH", msg[4:12])
    check((qd, an, ns, ar) == (1, 1, 0, 0), "counts 1/1/0/0 (%d/%d/%d/%d)"
          % (qd, an, ns, ar))
    q = R.dns_parse_query(g_dns_message(qid, "example.com"))
    check(msg[12:12 + len(q["question"])] == q["question"],
          "the question is echoed verbatim, not re-encoded")
    check(msg[12 + len(q["question"]):][:2] == b"\xc0\x0c",
          "the answer's owner is a compression pointer to it")
    check(r.stats["dns"] == 1, "the dns counter moved")

    sport, dport = struct.unpack("!HH", reply[34:38])
    check((sport, dport) == (53, DNS_LOCAL_PORT),
          "UDP 53 -> %d (the query's own source port)" % dport)
    check(reply[6:12] == R.mac("52:55:0a:00:02:02") and
          reply[0:6] == OUR_MAC,
          "unicast: gateway MAC -> us, not broadcast")
    check(struct.unpack("!H", reply[38:40])[0] == 8 + len(msg),
          "udp.length covers the message")
    check(g_checksum(reply[14:34]) == 0, "reply IP header checksum verifies")


def test_dns_only_answers_what_was_asked():
    """Nothing invented: a type the responder does not serve, a question count
    it never sees, and a runt all have to be left alone -- a wrong answer here
    is worse than no answer, because the guest caches it."""
    r = R.Responder(R.mac(R.GW_MAC_DEFAULT))
    aaaa = g_dns_frame(0x1111, "example.com", qtype=28)
    check(r.handle(aaaa) is None, "a type-28 (AAAA) query is not answered")
    check(r.stats["ignored"] == 1, "AAAA counted as ignored, not answered")

    qid = 0x2222
    good = g_dns_message(qid, "example.com")
    two_q = struct.pack("!HHH", qid, 0x0100, 2) + b"\x00" * 6 + \
        good[12:] + good[12:]
    udp = struct.pack("!HHHH", DNS_LOCAL_PORT, 53, 8 + len(two_q), 0) + two_q
    ip = g_ipv4(17, R.ip4(R.GUEST_IP), R.ip4(DNS_SERVER_IP), udp, 2)
    check(r.handle(g_eth(ip, 0x0800, dst=R.mac("52:55:0a:00:02:03"))) is None,
          "QDCOUNT=2 is not answered (the guest's builder always sends 1)")

    runt = b"\x00" * 11                                   # < 12-byte header
    udp = struct.pack("!HHHH", DNS_LOCAL_PORT, 53, 8 + len(runt), 0) + runt
    ip = g_ipv4(17, R.ip4(R.GUEST_IP), R.ip4(DNS_SERVER_IP), udp, 2)
    check(r.handle(g_eth(ip, 0x0800, dst=R.mac("52:55:0a:00:02:03"))) is None,
          "an 11-byte runt is not answered")

    # a query to some other UDP port is not DNS
    udp = struct.pack("!HHHH", DNS_LOCAL_PORT, 5353, 8 + len(good), 0) + good
    ip = g_ipv4(17, R.ip4(R.GUEST_IP), R.ip4(DNS_SERVER_IP), udp, 2)
    check(r.handle(g_eth(ip, 0x0800, dst=R.mac("52:55:0a:00:02:03"))) is None,
          "dport 5353 (mDNS) is not answered as DNS")
    check(r.stats["dns"] == 0, "no answer was counted for any of the above")


def test_dns_is_deterministic_and_droppable():
    """Same query, same answer, twice -- and `--drop=dns` withholds it while
    leaving the other three protocols alone (the lever N3's NC uses)."""
    qid = 0x3333
    frame = g_dns_frame(qid)
    r1 = R.Responder(R.mac(R.GW_MAC_DEFAULT))
    r2 = R.Responder(R.mac(R.GW_MAC_DEFAULT))
    check(r1.handle(frame) == r2.handle(frame),
          "two responder instances answer a DNS query identically")
    check(r1.handle(frame) == r1.handle(frame),
          "replaying the query answers identically (no transaction state)")

    dropped = R.Responder(R.mac(R.GW_MAC_DEFAULT), drop="dns")
    check(dropped.handle(frame) is None, "--drop=dns withholds the answer")
    check(dropped.handle(g_arp_frame(R.ip4(R.GW_IP))) is not None,
          "--drop=dns leaves ARP alone")
    check(dropped.handle(g_dhcp_frame(1, 0x12345678)) is not None,
          "--drop=dns leaves DHCP alone")
    check(dropped.handle(g_icmp_frame(R.ip4(R.GW_IP),
                                      R.mac(R.GW_MAC_DEFAULT))) is not None,
          "--drop=dns leaves ICMP alone")


def test_tcp_open_and_close():
    """`tcp_open_addr` (tcp.c:675) blocks on the handshake: SYN, up to
    TCP_MAX_RETRIES x TCP_RTO_TICKS waiting for SYN|ACK, then a bare ACK, then
    ESTABLISHED.  `tcp_close` (tcp.c:1343) from ESTABLISHED sends FIN|ACK and
    accepts a single FIN|ACK back -- its ACK branch moves it to FIN_WAIT_2 and
    its FIN branch to TIME_WAIT in the same segment -- so one reply closes the
    connection without burning an RTO."""
    r = R.Responder(R.mac(R.GW_MAC_DEFAULT), quiet=True)
    ok, why, our_isn = tcp_handshake(r, 40000, 0x1000)
    check(ok, "the guest's handshake reaches ESTABLISHED (%s)" % why)
    check(len(r.tcp) == 1, "one 4-tuple in the table")
    check(r.stats["tcp"] == 1, "the tcp counter moved")

    out = r.handle_many(g_tcp_segment(40000, 53, 0x1000, 0, R.TCP_SYN,
                                      opts=g_tcp_syn_opts()))
    sa = out[0]
    okf, whyf, flags, seq, ack, window, _d = \
        guest_accepts_tcp_segment(sa, 40000, 53)
    check(okf, "the SYN-ACK passes tcp_recv_segment_timeout's filter (%s)" % whyf)
    check(window == TCP_WINDOW_GUEST or window != 0,
          "the SYN-ACK advertises a usable window (%d)" % window)
    check(seq == our_isn, "our ISN is echoed consistently (%d)" % seq)
    t = R.parse_tcp(R.ipv4_body(sa[14:], R.parse_ipv4(sa[14:])[3]))
    check(t["hdr"] == 28 and R.tcp_parse_options(t["opts"]) == (1460, True),
          "the SYN-ACK carries MSS 1460 + SACK-permitted, so the guest's "
          "tcpm6c_parse_opts runs (hdr=%d)" % t["hdr"])

    ok, why = tcp_close(r, 40000, our_isn, 0x1000)
    check(ok, "the guest's close completes in one round trip (%s)" % why)
    check(r.tcp == {}, "the 4-tuple is freed once the guest ACKs our FIN")


def test_tcp_sixteen_concurrent():
    """tcp.c:1496 `tcp_x5_self_test` opens TCP_MAX_CONNS(16) connections to
    10.0.2.3:53 in sequence, holds them ALL open, then closes them all -- so
    the responder needs 16 independent 4-tuples alive at once, each with its
    own sequence numbers, and the ephemeral ports come from the guest's
    40000..64999 wheel with ISN 0x1000 + h*0x100."""
    r = R.Responder(R.mac(R.GW_MAC_DEFAULT), quiet=True)
    conns = []
    for h in range(TCP_X5_CONNS):
        ok, why, our_isn = tcp_handshake(r, 40000 + h, 0x1000 + h * 0x100)
        if not ok:
            check(False, "connection %d: %s" % (h, why))
            break
        conns.append((40000 + h, 0x1000 + h * 0x100, our_isn))
    check(len(conns) == TCP_X5_CONNS,
          "%d/%d connections reached ESTABLISHED and stayed open"
          % (len(conns), TCP_X5_CONNS))
    check(len(r.tcp) == TCP_X5_CONNS,
          "the table holds %d live tuples at once" % len(r.tcp))
    check(r.stats["tcp"] == TCP_X5_CONNS, "the tcp counter counts handshakes")

    isn_set = set()
    for sport, isn, our_isn in conns:
        t = R.parse_tcp(R.ipv4_body(
            r.handle_many(g_tcp_segment(sport, 53, isn, 0, R.TCP_SYN,
                                        opts=g_tcp_syn_opts()))[0][14:],
            R.parse_ipv4(r.handle_many(g_tcp_segment(
                sport, 53, isn, 0, R.TCP_SYN,
                opts=g_tcp_syn_opts()))[0][14:])[3]))
        isn_set.add((sport, t["seq"]))
    check(len(isn_set) == TCP_X5_CONNS,
          "each tuple keeps its own sequence numbers (%d distinct)"
          % len(isn_set))

    closed = 0
    for sport, isn, our_isn in conns:
        ok, _why = tcp_close(r, sport, our_isn, isn)
        closed += 1 if ok else 0
    check(closed == TCP_X5_CONNS, "%d/%d closed cleanly" % (closed, TCP_X5_CONNS))
    check(r.tcp == {}, "the table is empty after every close")


def test_tcp_behaviours():
    """The paths that are not the happy one, each of which the guest can reach:
    a retransmitted SYN after an RTO, a segment for a tuple that was never
    opened, a RST, and a full table."""
    r = R.Responder(R.mac(R.GW_MAC_DEFAULT), quiet=True)
    a = r.handle_many(g_tcp_segment(41000, 53, 0x2000, 0, R.TCP_SYN,
                                    opts=g_tcp_syn_opts()))
    b = r.handle_many(g_tcp_segment(41000, 53, 0x2000, 0, R.TCP_SYN,
                                    opts=g_tcp_syn_opts()))
    check(len(a) == 1 and a == b,
          "a retransmitted SYN is answered again, identically (tcp.c:766 "
          "retransmits after TCP_RTO_TICKS)")
    check(len(r.tcp) == 1, "the retransmit did not open a second tuple")

    stray = r.handle_many(g_tcp_segment(41999, 53, 0x3000, 0,
                                        R.TCP_FIN | R.TCP_ACK))
    check(stray == [], "a FIN for a tuple that was never opened gets no reply")
    stray = r.handle_many(g_tcp_segment(41999, 53, 0x3000, 0x4000, R.TCP_ACK))
    check(stray == [], "a bare ACK for an unknown tuple gets no reply")

    ok, why, our_isn = tcp_handshake(r, 42000, 0x4000)
    check(ok, "a second connection opens (%s)" % why)
    rst = r.handle_many(g_tcp_segment(42000, 53, 0x4001, our_isn + 1, R.TCP_RST))
    check(rst == [], "a RST is not answered")
    check((R.ip4(R.GUEST_IP), 42000, R.ip4(R.DNS_IP), 53) not in r.tcp,
          "a RST drops the tuple")

    full = R.Responder(R.mac(R.GW_MAC_DEFAULT), quiet=True)
    for i in range(R.TCP_MAX_CONNS):
        full.handle_many(g_tcp_segment(50000 + i, 53, 0x1000 + i, 0, R.TCP_SYN,
                                       opts=g_tcp_syn_opts()))
    check(len(full.tcp) == R.TCP_MAX_CONNS,
          "the table is bounded at TCP_MAX_CONNS=%d" % R.TCP_MAX_CONNS)
    over = full.handle_many(g_tcp_segment(59999, 53, 0x9999, 0, R.TCP_SYN,
                                          opts=g_tcp_syn_opts()))
    check(len(over) == 1, "a SYN past the bound is answered")
    okf, whyf, flags, _s, ack, _w, _d = \
        guest_accepts_tcp_segment(over[0], 59999, 53)
    check(okf and (flags & (R.TCP_RST | R.TCP_ACK)) == (R.TCP_RST | R.TCP_ACK)
          and ack == 0x999A,
          "...with RST|ACK acking seq+1 (%s, flags=0x%02x ack=%d)"
          % (whyf, flags, ack))

    no_tcp = R.Responder(R.mac(R.GW_MAC_DEFAULT), quiet=True, no_tcp=True)
    check(no_tcp.handle_many(g_tcp_segment(43000, 53, 0x5000, 0, R.TCP_SYN,
                                           opts=g_tcp_syn_opts())) == [],
          "--no-tcp withholds the SYN-ACK (the N3 NC lever)")
    check(no_tcp.handle(g_arp_frame(R.ip4(R.GW_IP))) is not None,
          "--no-tcp leaves ARP alone")


def test_dns_over_tcp():
    """dns.c:187 `dns_wire_query_tcp`: a fresh id, the 2-byte length prefix and
    the message sent as two separate `tcp_send_h` calls, then the reply read
    back across segment boundaries.  This sends it that way -- prefix first,
    query second -- and checks the answer is the same one the UDP leg gives."""
    r = R.Responder(R.mac(R.GW_MAC_DEFAULT), quiet=True)
    ok, why, our_isn = tcp_handshake(r, 44000, 0x6000)
    check(ok, "the TCP carriage opens (%s)" % why)

    qid = 0xBEEF
    msg = g_dns_message(qid, "example.com")
    pfx = struct.pack("!H", len(msg))
    gseq = 0x6001
    out = r.handle_many(g_tcp_segment(44000, 53, gseq, our_isn + 1,
                                      R.TCP_ACK | R.TCP_PSH, data=pfx))
    check(len(out) == 1 and not R.parse_tcp(
        R.ipv4_body(out[0][14:], R.parse_ipv4(out[0][14:])[3]))["data"],
          "the bare length prefix is ACKed but not answered yet")
    out = r.handle_many(g_tcp_segment(44000, 53, gseq + 2, our_isn + 1,
                                      R.TCP_ACK | R.TCP_PSH, data=msg))
    data_frames = [f for f in out
                   if R.parse_tcp(R.ipv4_body(f[14:],
                                              R.parse_ipv4(f[14:])[3]))["data"]]
    check(len(out) == 1 and len(data_frames) == 1,
          "the completed message gets exactly ONE segment -- the answer "
          "acknowledges the data instead of a bare ACK going out first "
          "(%d segment(s), %d with data)" % (len(out), len(data_frames)))
    t = R.parse_tcp(R.ipv4_body(data_frames[0][14:],
                                R.parse_ipv4(data_frames[0][14:])[3]))
    check((t["flags"] & (R.TCP_ACK | R.TCP_PSH)) == (R.TCP_ACK | R.TCP_PSH),
          "the answer rides ACK|PSH (flags=0x%02x)" % t["flags"])
    check(t["seq"] == our_isn + 1,
          "the answer's seq follows our SYN-ACK (%d)" % t["seq"])
    body = t["data"]
    check(len(body) >= 2, "the answer carries the RFC 1035 length prefix")
    mlen = struct.unpack("!H", body[0:2])[0]
    check(mlen == len(body) - 2,
          "prefix %d == the %d bytes behind it" % (mlen, len(body) - 2))
    verdict, a_ip, a_ttl = guest_dns_parse_response(body[2:], "example.com", qid)
    check(verdict == "ANSWER", "the guest's parser accepts it (%s)" % verdict)
    check(a_ip == 0x5DB8D822, "same A record as the UDP leg (0x%08x)" % a_ip)
    check(r.stats["dns"] == 1, "counted as a DNS answer, not a TCP one")

    # and the close still works after data has flowed
    fin = r.handle_many(g_tcp_segment(44000, 53, gseq + 2 + len(msg),
                                      our_isn + 1 + len(body),
                                      R.TCP_FIN | R.TCP_ACK))
    check(len(fin) == 1, "the connection still closes after carrying data")
    check(r.stats["tcp"] == 1, "one handshake for the whole exchange")


def test_tcp_sequencing():
    """UPKEEP U6: the receiver validates sequence numbers instead of gluing
    arrivals onto the stream.  The guest's own receiver never checks seq/ack
    (see `guest_accepts_tcp_segment`), so the *consumer* of sequencing here
    is the guest as a SENDER: its M6d code decodes SACK blocks to mark its
    retransmission queue (`tcpm6d_mark`), picks the hole
    (`tcpm6d_next_hole`), and counts duplicate ACKs per RFC 5681 s2 -- a
    duplicate ACK that carries data or moves the window is NOT counted
    (`tcpm6_on_ack` takes both as exclusions), which pins the shape of our
    dup ACKs as firmly as any filter above.

    RFC 9293 s3.10.7.4 is the rule: trim to the window (FIN included),
    deliver only what begins at RCV.NXT, hold the rest, acknowledge the
    unacceptable with <SEQ=SND.NXT><ACK=RCV.NXT>.  RFC 2018 s4 adds: SACK
    blocks only when the peer's SYN offered SACK-permitted, first block the
    one holding the segment that triggered the ACK.  Every assertion below
    is on wire bytes (ack numbers, block edges, header lengths) because the
    guest's accept filter would pass any of them -- it is the sequence
    arithmetic itself that is on trial."""
    M = 0xFFFFFFFF
    qid = 0x51AC
    msg = g_dns_message(qid, "example.com")
    stream = struct.pack("!H", len(msg)) + msg
    L = len(stream)
    check(L == 31, "the framed test query is 31 bytes (prefix + message)")

    r = R.Responder(R.mac(R.GW_MAC_DEFAULT), quiet=True)
    DATA = R.TCP_ACK | R.TCP_PSH

    def feed(sport, seq, ack, flags, data=b""):
        return r.handle_many(g_tcp_segment(sport, 53, seq, ack, flags,
                                           data=data))

    def acked(frame):
        # Tolerant on purpose: the negative control runs this same file
        # against the pre-U6 responder, where several replies below are
        # shorter (or absent) -- each shortened reply must record as a
        # FAIL, not abort the file before the later signatures print.
        try:
            t = g_parse_tcp(frame)
        except Exception:
            t = None
        if not t:
            return {"flags": 0, "ack": 0, "seq": 0, "data": b"", "hdr": 0,
                    "window": 0, "opts": b""}, []
        return t, g_parse_sack(t["opts"])

    def key(sport):
        return (R.ip4(R.GUEST_IP), sport, R.ip4(R.DNS_IP), 53)

    # ---- out-of-order tail: held, SACKed, contiguous-only delivery ------
    ok, why, oisn = tcp_handshake(r, 45001, 0x10000)
    check(ok, "sequencing connection opens (%s)" % why)
    base, dns0 = 0x10001, r.stats["dns"]
    out = feed(45001, base + 8, oisn + 1, DATA, stream[8:])
    check(len(out) == 1, "an out-of-order segment gets exactly one dup ACK")
    t, sack = acked(out[0])
    okg, why_g = guest_accepts_tcp_segment(out[0], 45001, 53)[:2]
    check(okg, "the SACK-carrying ACK passes the guest's filter, checksum "
               "and header length included (%s)" % why_g)
    check(t["flags"] == R.TCP_ACK and t["data"] == b"",
          "the dup ACK carries no data (RFC 5681 s2: the guest would not "
          "count it as a duplicate otherwise)")
    check(t["ack"] == base, "the cumulative ACK did not move into the gap")
    check(t["window"] == 64240,
          "the advertised window is unchanged (a moved window un-counts a "
          "duplicate ACK on the guest)")
    check(sack == [(base + 8, base + L)],
          "one SACK block covers exactly the held bytes (%s)" % (sack,))
    check(t["hdr"] == 32,
          "the 10-byte SACK option is padded to a 4-byte edge (hdr=%d)"
          % t["hdr"])
    check(r.stats["dns"] == dns0, "nothing reached DNS before the gap filled")
    out = feed(45001, base, oisn + 1, DATA, stream[:8])
    t, sack = acked(out[0])
    check(len(out) == 1 and t["ack"] == base + L,
          "filling the gap acknowledges the whole query in one step")
    check(sack == [], "the queue is empty: no SACK rides the answer")
    check(bool(t["data"]), "the gap fill produced the answer at last")
    verdict, _a, _t = guest_dns_parse_response(t["data"][2:], "example.com",
                                               qid)
    check(verdict == "ANSWER", "the assembled answer parses (%s)" % verdict)
    check(r.stats["dns"] == dns0 + 1, "the query was answered exactly once")
    out = feed(45001, base, oisn + 1, DATA, stream[:8])
    t, _s = acked(out[0])
    check(t["flags"] == R.TCP_ACK and t["ack"] == base + L
          and t["data"] == b"" and r.stats["dns"] == dns0 + 1,
          "a retransmitted head is re-ACKed, never re-delivered")

    # ---- partial overlap: only the new tail advances --------------------
    ok, why, oisn = tcp_handshake(r, 45002, 0x20000)
    check(ok, "overlap connection opens (%s)" % why)
    base, dns0 = 0x20001, r.stats["dns"]
    out = feed(45002, base, oisn + 1, DATA, stream[:6])
    t, _s = acked(out[0])
    check(t["ack"] == base + 6 and t["data"] == b"",
          "the first six bytes are ACKed and held (incomplete message)")
    out = feed(45002, base + 2, oisn + 1, DATA, stream[2:])
    t, _s = acked(out[0])
    check(t["ack"] == base + L and bool(t["data"])
          and r.stats["dns"] == dns0 + 1,
          "a straddling segment contributes only its new tail; the query "
          "assembles exactly once")

    # ---- two gaps: first-block rule, drain order ------------------------
    ok, why, oisn = tcp_handshake(r, 45003, 0x30000)
    check(ok, "two-gap connection opens (%s)" % why)
    base = 0x30001
    pieces = [(0, stream[0:6]), (6, stream[6:12]), (12, stream[12:18]),
              (18, stream[18:24]), (24, stream[24:])]
    out = feed(45003, base + 6, oisn + 1, DATA, pieces[1][1])
    t, sack = acked(out[0])
    check(sack == [(base + 6, base + 12)] and t["ack"] == base,
          "the first gap piece is SACKed on its own")
    out = feed(45003, base + 18, oisn + 1, DATA, pieces[3][1])
    t, sack = acked(out[0])
    check(sack == [(base + 18, base + 24), (base + 6, base + 12)],
          "the first block holds the segment that triggered this ACK "
          "(RFC 2018 s4), the older block follows (%s)" % (sack,))
    out = feed(45003, base + 6, oisn + 1, DATA, pieces[1][1])
    t, sack = acked(out[0])
    check(sack[:1] == [(base + 6, base + 12)] and t["ack"] == base,
          "a duplicate of queued data leads its dup ACK and changes nothing")
    out = feed(45003, base, oisn + 1, DATA, pieces[0][1])
    t, sack = acked(out[0])
    check(t["ack"] == base + 12 and sack == [(base + 18, base + 24)]
          and t["data"] == b"",
          "filling gap one drains exactly what became contiguous")
    out = feed(45003, base + 12, oisn + 1, DATA, pieces[2][1])
    t, sack = acked(out[0])
    check(t["ack"] == base + 24 and sack == [] and t["data"] == b"",
          "filling gap two drains the rest of the queue, still no answer")
    out = feed(45003, base + 24, oisn + 1, DATA, pieces[4][1])
    t, _s = acked(out[0])
    check(t["ack"] == base + L and bool(t["data"]),
          "the final piece completes the query and gets the answer")

    # ---- at most four blocks, however many runs are held -----------------
    ok, why, oisn = tcp_handshake(r, 45004, 0x40000)
    check(ok, "block-cap connection opens (%s)" % why)
    base = 0x40001
    out = []
    for k in range(6):
        out = feed(45004, base + 4 + 4 * k, oisn + 1, DATA, b"xx")
    t, sack = acked(out[0])
    check(len(sack) == 4 and sack == [(base + 24, base + 26),
                                      (base + 4, base + 6),
                                      (base + 8, base + 10),
                                      (base + 12, base + 14)],
          "six held runs are reported as four blocks: trigger first, then "
          "ascending (%s)" % (sack,))
    check(t["hdr"] == 56,
          "four blocks + 2 pad bytes fit the 40-byte option space (hdr=%d)"
          % t["hdr"])
    check(t["ack"] == base, "none of the six runs moved the cumulative ACK")
    r.handle_many(g_tcp_segment(45004, 53, base, oisn + 1, R.TCP_RST))

    # ---- no SACK without the peer's SACK-permitted ----------------------
    mss_only = bytes((2, 4)) + struct.pack("!H", 1460) + bytes(4)
    out = r.handle_many(g_tcp_segment(45005, 53, 0x50000, 0, R.TCP_SYN,
                                      opts=mss_only))
    check(len(out) == 1, "an MSS-only SYN still gets an answer")
    t = g_parse_tcp(out[0])
    oisn5 = t["seq"]
    check(R.tcp_parse_options(t["opts"]) == (1460, True),
          "our SYN-ACK still offers SACK-permitted (that offer licenses "
          "the *guest's* SACK blocks, not ours)")
    check(feed(45005, 0x50001, oisn5 + 1, R.TCP_ACK) == [],
          "the MSS-only handshake completes in silence")
    base = 0x50001
    out = feed(45005, base + 8, oisn5 + 1, DATA, stream[8:])
    t, sack = acked(out[0])
    check(t["hdr"] == 20 and sack == [] and t["ack"] == base,
          "an unnegotiated peer gets a plain 20-byte dup ACK (RFC 2018 s4 "
          "MUST NOT) -- queueing still happens, signalling does not")
    out = feed(45005, base, oisn5 + 1, DATA, stream[:8])
    t, _s = acked(out[0])
    check(t["ack"] == base + L and bool(t["data"]),
          "the unnegotiated connection still assembles and answers")

    # ---- sequence space wraps mid-query ---------------------------------
    ok, why, oisn = tcp_handshake(r, 45006, 0xFFFFFFF8)
    check(ok, "wrap connection opens just below the seam (%s)" % why)
    base = 0xFFFFFFF9
    out = feed(45006, 0xFFFFFFFD, oisn + 1, DATA, stream[4:16])
    t, sack = acked(out[0])
    check(sack == [(0xFFFFFFFD, 9)],
          "a block straddling the seam has right edge < left edge and the "
          "guest's signed-length check still accepts it (%s)" % (sack,))
    check(t["ack"] == base, "the ACK stayed below the seam")
    out = feed(45006, base, oisn + 1, DATA, stream[:4])
    t, _s = acked(out[0])
    check(t["ack"] == 9 and t["data"] == b"",
          "the fill drains across the seam to ack=%d" % t["ack"])
    out = feed(45006, 9, oisn + 1, DATA, stream[16:])
    t, _s = acked(out[0])
    check(t["ack"] == (base + L) & M and bool(t["data"]),
          "post-seam bytes complete the query (ack=%d)" % t["ack"])

    # ---- an out-of-order FIN is sequenced, not consumed -----------------
    ok, why, oisn = tcp_handshake(r, 45007, 0x60000)
    check(ok, "OOO-FIN connection opens (%s)" % why)
    base = 0x60001
    out = feed(45007, base, oisn + 1, DATA, stream[:10])
    t, _s = acked(out[0])
    check(t["ack"] == base + 10, "ten contiguous bytes in")
    out = feed(45007, base + L, oisn + 1, R.TCP_FIN | R.TCP_ACK)
    t, sack = acked(out[0])
    check(len(out) == 1 and t["flags"] == R.TCP_ACK
          and t["ack"] == base + 10 and sack == [],
          "a FIN past a gap earns a plain dup ACK, no close handshake, "
          "and is not SACKed (SACK describes data bytes)")
    check(key(45007) in r.tcp, "the tuple stays open while its FIN waits")
    out = feed(45007, base + 10, oisn + 1, DATA, stream[10:])
    check(len(out) == 2, "the gap fill yields the answer *and* the "
                          "close (%d frames)" % len(out))
    t1, _s = acked(out[0])
    t2, _s = acked((out + [b""])[1])
    check(bool(t1["data"]) and (t2["flags"] & (R.TCP_FIN | R.TCP_ACK))
          == (R.TCP_FIN | R.TCP_ACK) and t2["ack"] == base + L + 1,
          "the queued FIN is consumed exactly when the last data byte "
          "lands (ack covers data + FIN)")
    check(feed(45007, base + L + 1, (t2["seq"] + 1) & M, R.TCP_ACK) == [],
          "the final ACK is met with silence")
    check(key(45007) not in r.tcp, "the tuple is freed after the FIN dance")

    # ---- duplicate FIN in LAST_ACK, then the tombstone ------------------
    ok, why, oisn = tcp_handshake(r, 45008, 0x70000)
    check(ok, "LAST_ACK connection opens (%s)" % why)
    base = 0x70001
    their_fin = g_tcp_segment(45008, 53, base, oisn + 1,
                              R.TCP_FIN | R.TCP_ACK)
    out = r.handle_many(their_fin)
    t, _s = acked(out[0])
    check(len(out) == 1 and (t["flags"] & (R.TCP_FIN | R.TCP_ACK))
          == (R.TCP_FIN | R.TCP_ACK) and t["ack"] == base + 1,
          "the close yields one FIN-ACK (the pinned pre-U6 shape)")
    fseq = t["seq"]
    for again in (1, 2):
        out = r.handle_many(their_fin)
        t, _s = acked(out[0]) if out else ({}, [])
        check(len(out) == 1 and (t.get("flags", 0)
                                 & (R.TCP_FIN | R.TCP_ACK))
              == (R.TCP_FIN | R.TCP_ACK) and t.get("seq") == fseq
              and t.get("ack") == base + 1,
              "a duplicate FIN in LAST_ACK re-sends the same FIN-ACK "
              "(attempt %d): the peer retransmits because it never saw "
              "ours, and only acking our FIN may end this dance" % again)
    check(key(45008) in r.tcp, "the tuple survives its duplicate FINs")
    check(feed(45008, base + 1, (fseq + 1) & M, R.TCP_ACK) == [],
          "the real final ACK closes quietly")
    check(key(45008) not in r.tcp, "the tuple is freed by the final ACK")
    out = r.handle_many(their_fin)
    t, _s = acked(out[0]) if out else ({}, [])
    check(len(out) == 1 and t.get("flags") == R.TCP_ACK
          and t.get("seq") == (fseq + 1) & M and t.get("ack") == base + 1,
          "a late duplicate FIN meets the tombstone: the final ACK again, "
          "a plain ACK this time -- the FIN side was already proven seen")
    check(feed(45008, base + 1, (fseq + 1) & M, DATA, b"x") == [],
          "the tombstone answers only the FIN duplicate, nothing else")

    # ---- beyond the window: discarded and honestly unSACKed -------------
    ok, why, oisn = tcp_handshake(r, 45009, 0x80000)
    check(ok, "window connection opens (%s)" % why)
    base = 0x80001
    out = feed(45009, base + 70000, oisn + 1, DATA, b"12345678")
    t, sack = acked(out[0])
    check(t["ack"] == base and sack == [],
          "a wholly out-of-window segment is dup-ACKed, dropped, unSACKed")
    out = feed(45009, base + 64200, oisn + 1, DATA, bytes(80))
    t, sack = acked(out[0])
    check(sack == [(base + 64200, base + 64240)],
          "a segment straddling the right edge is trimmed to RCV.WND "
          "before it is held (%s)" % (sack,))
    out = feed(45009, base, oisn + 1, DATA, stream)
    t, sack = acked(out[0])
    check(bool(t["data"]) and t["ack"] == base + L
          and sack == [(base + 64200, base + 64240)],
          "the answer still advertises the run held across the remaining "
          "gap (RFC 2018: SACK rides every ACK while data is held)")
    r.handle_many(g_tcp_segment(45009, 53, base + L, oisn + 1, R.TCP_RST))

    # ---- the queue bound is an honest refusal, not a silent merge --------
    ok, why, oisn = tcp_handshake(r, 45010, 0x90000)
    check(ok, "bound connection opens (%s)" % why)
    base = 0x90001
    t32 = t33 = None
    for i in range(33):
        out = feed(45010, base + 4 + 4 * i, oisn + 1, DATA, b"ab")
        if i == 31:
            t32 = acked(out[0])
        if i == 32:
            t33 = acked(out[0])
    check(t32[1][:1] == [(base + 128, base + 130)],
          "run 32 is held and leads its dup ACK (%s)" % (t32[1][:1],))
    check(t33[0]["ack"] == base and (base + 132, base + 134) not in t33[1]
          and t33[1][:1] == [(base + 4, base + 6)],
          "run 33 breaks the 32-run bound: it is discarded whole and its "
          "bytes are NOT SACKed (the sender must retransmit them later)")
    out = feed(45010, base, oisn + 1, DATA, b"wxyz")
    t, _s = acked(out[0])
    check(t["ack"] == base + 6,
          "four in-order bytes drain exactly the one run that became "
          "contiguous (ack=%#x)" % t["ack"])
    r.handle_many(g_tcp_segment(45010, 53, base + 6, oisn + 1, R.TCP_RST))

    # ---- FIN piggybacked on the last data: pinned two-frame close -------
    ok, why, oisn = tcp_handshake(r, 45011, 0xA0000)
    check(ok, "FIN-with-data connection opens (%s)" % why)
    base = 0xA0001
    out = feed(45011, base, oisn + 1, DATA | R.TCP_FIN, stream)
    check(len(out) == 2, "query + FIN in one segment: answer then FIN-ACK")
    t1, _s = acked(out[0])
    t2, _s = acked((out + [b""])[1])
    check(bool(t1["data"]) and (t2["flags"] & (R.TCP_FIN | R.TCP_ACK))
          == (R.TCP_FIN | R.TCP_ACK) and t2["ack"] == base + L + 1,
          "the FIN behind data is acked only after the data, in order")
    check(feed(45011, base + L + 1, (t2["seq"] + 1) & M, R.TCP_ACK) == [],
          "and the closing ACK is silent")
    check(key(45011) not in r.tcp, "the FIN-with-data tuple is freed")

    # ---- the whole model replays deterministically (no clock) -----------
    def script(rr):
        _ok, _why, oi = tcp_handshake(rr, 45100, 0xB0000)
        bx = 0xB0001
        outs = [rr.handle_many(g_tcp_segment(45100, 53, bx + 10, oi + 1,
                                             DATA, data=stream[10:])),
                rr.handle_many(g_tcp_segment(45100, 53, bx, oi + 1,
                                             DATA, data=stream[:10]))]
        a0 = g_parse_tcp(outs[1][0])
        snd = (a0["seq"] + len(a0["data"])) & M
        fin = g_tcp_segment(45100, 53, bx + L, snd, R.TCP_FIN | R.TCP_ACK)
        outs.append(rr.handle_many(fin))
        outs.append(rr.handle_many(fin))                 # LAST_ACK dup
        fsq = g_parse_tcp(outs[2][0])["seq"]
        outs.append(rr.handle_many(g_tcp_segment(
            45100, 53, bx + L + 1, (fsq + 1) & M, R.TCP_ACK)))
        outs.append(rr.handle_many(fin))                 # tombstone
        return outs

    ra = R.Responder(R.mac(R.GW_MAC_DEFAULT), quiet=True)
    rb = R.Responder(R.mac(R.GW_MAC_DEFAULT), quiet=True)
    check(script(ra) == script(rb),
          "two instances replay OOO + SACK + dup-FIN + tombstone "
          "byte-identically: retention is a count, never a clock")


def test_arp_for_the_whole_subnet():
    """`net_arp_resolve` ARPs the TARGET for an on-link address (net.c:405+)
    and only routes through the gateway for an off-subnet one, so the responder
    has to answer who-has 10.0.2.3 -- otherwise DNS is unreachable even though
    the gateway resolves fine.  Measured: the first N3 wire run stopped at
    `[dns] FAIL: could not resolve the server's MAC`."""
    def arp_for(tip):
        payload = struct.pack("!HHBBH", 1, 0x0800, 6, 4, 1)
        payload += OUR_MAC + R.ip4(R.GUEST_IP) + bytes(6) + R.ip4(tip)
        return g_eth(payload, 0x0806)

    r = R.Responder(R.mac(R.GW_MAC_DEFAULT), quiet=True)
    for tip in [(10, 0, 2, 2), (10, 0, 2, 3), (10, 0, 2, 99)]:
        rep = r.handle(arp_for(tip))
        ok = rep is not None and guest_accepts_arp_reply(rep)[0]
        check(ok, "who-has %s is answered and passes the guest's filter"
              % R.ipstr(R.ip4(tip)))
        if rep:
            check(guest_accepts_arp_reply(rep)[1] == R.mac(R.GW_MAC_DEFAULT),
                  "...with the single interface MAC (%s)"
                  % R.ipstr(R.ip4(tip)))
    check(r.handle(arp_for(R.GUEST_IP)) is None,
          "a probe for the guest's OWN address is not answered")
    check(r.handle(arp_for((8, 8, 8, 8))) is None,
          "an off-subnet target is not answered (the guest routes it)")
    check(r.handle(arp_for((10, 0, 3, 1))) is None,
          "a neighbouring subnet is not answered either")
    check(r.stats["arp"] == 3, "only the on-subnet probes counted")


def test_reply_goes_to_the_peer_not_to_the_endpoint():
    """Regression vector for the bug the first N2 wire run exposed.

    The responder *binds* the endpoint the emulator *connects to* (see
    src/devices.c: `--tap=` does a UDP connect).  So `sendto(reply,
    (host, port))` sends the reply to the responder's own socket and the
    emulator never sees it -- measured as a wire log full of correct OFFERs
    next to a guest printing `[dhcp] FAIL: no OFFER received`.  The reply has
    to go to the peer address `recvfrom` reported.

    Protocol vectors above cannot see this (they call `Responder.handle`
    directly), so it gets its own socket-level vector: run the real tool as a
    subprocess, send one DISCOVER from a *connected* client socket, and assert
    the OFFER arrives on that client -- and on nothing else.
    """
    import socket
    import subprocess
    import time

    endpoint = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    endpoint.bind(("127.0.0.1", 0))
    port = endpoint.getsockname()[1]
    endpoint.close()                       # freed for the responder to bind

    sentinel = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sentinel.bind(("127.0.0.1", 0))        # must stay silent for the whole run
    sentinel.settimeout(0.5)
    sentinel_port = sentinel.getsockname()[1]

    proc = subprocess.Popen(
        [sys.executable, os.path.join(ROOT, "tools", "tap_responder.py"),
         "--port", str(port), "--timeout", "5", "--quiet"],
        stdout=subprocess.DEVNULL, stderr=subprocess.STDOUT)

    client = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    client.connect(("127.0.0.1", port))    # exactly what the emulator does
    client.settimeout(4.0)
    reply = b""
    try:
        deadline = time.monotonic() + 4.0
        while time.monotonic() < deadline:
            try:
                sentinel.recvfrom(65536)
            except socket.timeout:
                pass
            client.send(g_dhcp_frame(1, 0x12345678))
            try:
                reply = client.recv(65536)
                break
            except socket.timeout:
                continue
    finally:
        client.close()
        proc.terminate()
        proc.wait(timeout=10)

    check(bool(reply), "a connected client receives the OFFER (not only the "
                       "responder's own socket)")
    check(len(reply) > 42 and reply[14 + 20 + 8 + 0] == 2,
          "what the client received is a BOOTREPLY (%d bytes)" % len(reply))
    opts = reply[14 + 20 + 8 + 236:]          # DHCP fixed part ends here
    check(len(opts) >= 3 and opts[0] == 53 and opts[2] == 2,
          "the client's reply is an OFFER (option 53 = 2)")
    silent = True
    try:
        sentinel.settimeout(0.2)
        sentinel.recvfrom(65536)
        silent = False
    except socket.timeout:
        pass
    sentinel.close()
    check(silent, "no reply leaked to any other socket on the host (port %d)"
          % sentinel_port)


def main():
    for fn in (test_frames_match_the_guest_builder,
               test_arp_reply,
               test_dhcp_discover_offer,
               test_dhcp_request_ack,
               test_icmp_echo_reply,
               test_dns_query_answer,
               test_dns_only_answers_what_was_asked,
               test_dns_is_deterministic_and_droppable,
               test_tcp_open_and_close,
               test_tcp_sixteen_concurrent,
               test_tcp_behaviours,
               test_dns_over_tcp,
               test_tcp_sequencing,
               test_arp_for_the_whole_subnet,
               test_reply_goes_to_the_peer_not_to_the_endpoint,
               test_determinism_and_the_drop_lever,
               test_junk_does_not_answer):
        print("--- %s" % fn.__name__)
        fn()
    if FAILED:
        print("test-tap-responder: %d FAILURE(S): %s"
              % (len(FAILED), ", ".join(FAILED)))
        return 1
    print("test-tap-responder: all tests passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
