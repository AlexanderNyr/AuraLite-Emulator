#!/usr/bin/env python3
"""Use QMP/HMP pmemsave to capture boot_info_t from a running QEMU guest."""
from __future__ import annotations
import argparse
import json
from pathlib import Path
import socket

SIZE_DEFAULT = 7776
PHYS_DEFAULT = 0x10000


def recv_json(stream) -> dict:
    while True:
        line = stream.readline()
        if not line:
            raise SystemExit("QMP socket closed before a response")
        msg = json.loads(line)
        # QMP may interleave asynchronous event messages with replies.
        if "event" not in msg:
            return msg


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("socket", type=Path, help="QMP UNIX socket path")
    ap.add_argument("output", type=Path, help="output path for the physical memory block")
    ap.add_argument("--phys", type=lambda s: int(s, 0), default=PHYS_DEFAULT)
    ap.add_argument("--size", type=int, default=SIZE_DEFAULT)
    args = ap.parse_args()
    if '"' in str(args.output) or "\\" in str(args.output):
        raise SystemExit("output path must not contain a quote or backslash")

    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.unlink(missing_ok=True)
    with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as sock:
        sock.settimeout(10)
        sock.connect(str(args.socket))
        stream = sock.makefile("rwb", buffering=0)
        greeting = recv_json(stream)
        if "QMP" not in greeting:
            raise SystemExit(f"not a QMP greeting: {greeting}")
        stream.write(b'{"execute":"qmp_capabilities"}\r\n')
        caps = recv_json(stream)
        if "return" not in caps:
            raise SystemExit(f"QMP capability negotiation failed: {caps}")
        command = f'pmemsave {args.phys:#x} {args.size} "{args.output}"'
        request = {"execute": "human-monitor-command", "arguments": {"command-line": command}}
        stream.write((json.dumps(request) + "\r\n").encode("utf-8"))
        result = recv_json(stream)
        if "error" in result:
            raise SystemExit(f"QMP memory save failed: {result['error']}")

    if not args.output.is_file() or args.output.stat().st_size != args.size:
        raise SystemExit(f"expected {args.size} bytes at {args.output}")
    qver = greeting["QMP"]["version"]
    print(f"QEMU QMP version: {qver}")
    print(f"Saved {args.size} bytes from physical {args.phys:#x} to {args.output}")
    print(f"HMP/QMP return: {result}")


if __name__ == "__main__":
    main()
