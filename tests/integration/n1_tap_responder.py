#!/usr/bin/env python3
"""tests/integration/n1_tap_responder.py -- NET N1 host-side TAP endpoint.

The N0 ground rule fixes the transport: TAP datagrams ARE Ethernet frames and
the endpoint is UDP 127.0.0.1:8080, with the emulator as the connecting side.
N1 is the first phase where a frame leaves the guest, so something has to be
bound at that endpoint to prove it -- this is the smallest honest thing that
can be: it records every datagram, one hex frame per line, and exits after
`--count` frames or `--timeout` seconds.

It deliberately does NOT answer (N2 is the host-responder phase), and it
deliberately writes nothing that could leak back into the emulator's own log:
the frames land in a separate file so the determinism pair in n1_tx_tap.sh
compares emulator output only.

Usage:
    n1_tap_responder.py --out frames.hex [--port 8080] [--count 1]
                        [--timeout 30]
"""
import argparse
import binascii
import socket
import sys
import time


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--out", required=True, help="hex frame log to write")
    ap.add_argument("--port", type=int, default=8080)
    ap.add_argument("--host", default="127.0.0.1")
    ap.add_argument("--count", type=int, default=1, help="frames to capture")
    ap.add_argument("--timeout", type=float, default=30.0, help="seconds")
    a = ap.parse_args()

    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    s.bind((a.host, a.port))
    s.settimeout(a.timeout)

    got = 0
    deadline = time.monotonic() + a.timeout
    with open(a.out, "w", encoding="ascii") as f:
        while got < a.count:
            s.settimeout(max(0.05, deadline - time.monotonic()))
            try:
                data, _peer = s.recvfrom(65536)
            except socket.timeout:
                break
            if not data:
                continue
            f.write(binascii.hexlify(data).decode("ascii") + "\n")
            f.flush()
            got += 1
    s.close()
    print("responder: captured %d frame(s) -> %s" % (got, a.out))
    return 0 if got >= a.count else 2


if __name__ == "__main__":
    sys.exit(main())
