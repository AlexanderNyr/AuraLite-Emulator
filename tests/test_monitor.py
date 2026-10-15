#!/usr/bin/env python3
"""TOOLING T5 wire tests for --monitor= (no third-party Python packages).

Lanes:
  H  HTTP page + RFC 6455 handshake using the standard sample accept key.
  A  real firmware boot: state, 16/64-bit server frames, break/step/exec,
     framebuffer marker, keyboard commands, quit, and non-perturbation receipts.
  B  endless ROM: async pause, single-step and quit while running/stopped.
  G  GDB owns run control: monitor continue must be rejected; GDB can still quit.
"""
import argparse
import http.client
import json
import os
import socket
import struct
import subprocess
import tempfile
import time

X86EMU = os.environ.get("X86EMU", "./x86emu")
failures = 0
checks = 0


def check(ok, label):
    global failures, checks
    checks += 1
    print(("ok   " if ok else "FAIL ") + label)
    if not ok:
        failures += 1


def free_port():
    with socket.socket() as s:
        s.bind(("127.0.0.1", 0))
        return s.getsockname()[1]


def wait_http(port, timeout=15.0):
    deadline = time.monotonic() + timeout
    last = None
    while time.monotonic() < deadline:
        try:
            c = http.client.HTTPConnection("127.0.0.1", port, timeout=1)
            c.request("GET", "/")
            r = c.getresponse()
            body = r.read()
            c.close()
            return r.status, body
        except OSError as exc:
            last = exc
            time.sleep(0.03)
    raise RuntimeError("monitor HTTP did not start: %r" % last)


class WSClient:
    def __init__(self, port, timeout=10):
        self.s = socket.create_connection(("127.0.0.1", port), timeout=timeout)
        self.s.settimeout(timeout)
        self.buf = b""
        self.screen = {}
        self.width = self.height = 0
        self.pixel0 = None
        self.frame_lengths = []
        self.sent_frame_lengths = []
        self.header = ""
        key = "dGhlIHNhbXBsZSBub25jZQ=="
        req = ("GET /ws HTTP/1.1\r\n"
               "Host: 127.0.0.1:%d\r\n"
               "Upgrade: websocket\r\n"
               "Connection: keep-alive, Upgrade\r\n"
               "Sec-WebSocket-Key: %s\r\n"
               "Sec-WebSocket-Version: 13\r\n\r\n" % (port, key))
        self.s.sendall(req.encode("ascii"))
        raw = self._until(b"\r\n\r\n")
        head = raw.split(b"\r\n\r\n", 1)[0]
        # _until already keeps any bytes following the header in self.buf;
        # do not overwrite them (the server may send initial state frames at once).
        self.header = head.decode("latin1") + "\r\n\r\n"

    def _until(self, marker):
        while marker not in self.buf:
            d = self.s.recv(65536)
            if not d:
                raise EOFError("monitor closed before HTTP response")
            self.buf += d
        i = self.buf.index(marker) + len(marker)
        out, self.buf = self.buf[:i], self.buf[i:]
        return out

    def close(self):
        try: self.s.close()
        except OSError: pass

    def send_raw_frame(self, payload, opcode=1, masked=True):
        n = len(payload)
        if n < 126:
            marker = n
            head = bytes((0x80 | opcode, (0x80 if masked else 0) | marker))
        elif n <= 0xFFFF:
            marker = 126
            head = bytes((0x80 | opcode, (0x80 if masked else 0) | marker)) + struct.pack("!H", n)
        else:
            marker = 127
            head = bytes((0x80 | opcode, (0x80 if masked else 0) | marker)) + struct.pack("!Q", n)
        self.sent_frame_lengths.append((opcode, marker, n, masked))
        if masked:
            key = os.urandom(4)
            body = bytes(b ^ key[i & 3] for i, b in enumerate(payload))
            self.s.sendall(head + key + body)
        else:
            self.s.sendall(head + payload)

    def command(self, cmd, **fields):
        obj = {"cmd": cmd}
        obj.update(fields)
        self.send_raw_frame(json.dumps(obj, separators=(",", ":")).encode("utf-8"))

    def _exact(self, n):
        while len(self.buf) < n:
            d = self.s.recv(65536)
            if not d: raise EOFError("monitor WebSocket closed")
            self.buf += d
        out, self.buf = self.buf[:n], self.buf[n:]
        return out

    def recv_event(self, timeout=10):
        self.s.settimeout(timeout)
        first, second = self._exact(2)
        if first & 0x70: raise AssertionError("unexpected WebSocket RSV bits")
        if second & 0x80: raise AssertionError("server-to-client frame was masked")
        marker = second & 0x7F
        if marker == 126: n = struct.unpack("!H", self._exact(2))[0]
        elif marker == 127: n = struct.unpack("!Q", self._exact(8))[0]
        else: n = marker
        if n > 4 * 1024 * 1024: raise AssertionError("monitor frame too large: %d" % n)
        payload = self._exact(n)
        self.frame_lengths.append((first & 0x0F, marker, n))
        if (first & 0x0F) == 1:
            return "text", json.loads(payload.decode("utf-8")), marker
        if (first & 0x0F) == 2:
            self.apply_framebuffer(payload)
            return "binary", payload, marker
        return "other", payload, marker

    def apply_framebuffer(self, b):
        if len(b) < 10 or b[0:3] != b"FB\x01": return
        flags = b[3]
        w, h, rows = struct.unpack_from("<HHH", b, 4)
        if not w or not h or w > 4096 or h > 4096: return
        if flags & 1: self.screen.clear()
        self.width, self.height = w, h
        off = 10
        for _ in range(rows):
            if off + 2 > len(b): raise AssertionError("truncated framebuffer row id")
            y = struct.unpack_from("<H", b, off)[0]; off += 2
            size = w * 3
            if y >= h or off + size > len(b): raise AssertionError("truncated framebuffer row")
            row = b[off:off + size]
            self.screen[y] = row
            if y == 0: self.pixel0 = tuple(row[:3])
            off += size
        if off != len(b): raise AssertionError("extra bytes in framebuffer frame")

    def wait_reply(self, cmd, timeout=15):
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            kind, data, marker = self.recv_event(max(0.1, deadline - time.monotonic()))
            if kind == "text" and data.get("kind") == "reply" and data.get("cmd") == cmd:
                return data
        raise TimeoutError("reply for %s not received" % cmd)

    def wait_state(self, predicate, timeout=30):
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            kind, data, marker = self.recv_event(max(0.1, deadline - time.monotonic()))
            if kind == "text" and data.get("kind") == "state" and predicate(data):
                return data, marker
        raise TimeoutError("matching monitor state not received")

    def wait_pixel(self, rgb, timeout=10):
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            if self.pixel0 == rgb: return True
            self.recv_event(max(0.1, deadline - time.monotonic()))
        return self.pixel0 == rgb


def start_monitor(args, monitor_port=None, gdb_port=None, cwd=None, host="127.0.0.1"):
    mp = monitor_port or free_port()
    full = [X86EMU] + list(args)
    if gdb_port is not None: full.append("--gdb=127.0.0.1:%d" % gdb_port)
    spec = "%s:%d" % (host, mp) if host else str(mp)
    full.append("--monitor=" + spec)
    p = subprocess.Popen(full, cwd=cwd, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    return p, mp


def assert_http_and_handshake(port, require_accept=True):
    status, body = wait_http(port)
    check(status == 200 and b"Live connection and run control" in body and b"WebSocket" in body,
          "HTTP GET / serves the live monitor and offline file controls")
    c = WSClient(port)
    lines = {line.split(":", 1)[0].lower(): line.split(":", 1)[1].strip()
             for line in c.header.split("\r\n") if ":" in line}
    check(c.header.startswith("HTTP/1.1 101 Switching Protocols"), "WebSocket upgrade returns 101")
    accept_ok = lines.get("sec-websocket-accept") == "s3pPLMBiTxaQ9kYGzzhZRbK+xOo="
    check(accept_ok, "RFC 6455 sample key -> Sec-WebSocket-Accept")
    if require_accept and not accept_ok:
        c.close()
        return None
    return c


def lane_handshake():
    # Exercise the host-omitted CLI spelling; it must bind to loopback.
    p, port = start_monitor(["--max-instr=1000000000"], host=None)
    c = None
    try:
        c = assert_http_and_handshake(port)
        if not c:
            p.kill()
            p.wait()
            return
        kind, state, marker = c.recv_event(5)
        check(kind == "text" and state.get("stop_reason") == "start",
              "handshake lane receives initial stopped state")
        if kind == "text":
            c.recv_event(5)  # initial framebuffer keyframe
        c.command("quit")
        c.wait_reply("quit", 5)
        try: p.wait(timeout=10)
        except subprocess.TimeoutExpired: p.kill(); p.wait()
        check(p.returncode == 0, "monitor quit exits cleanly")
    finally:
        if c: c.close()
        if p.poll() is None: p.kill(); p.wait()
        p.communicate()


def lane_boot():
    with tempfile.TemporaryDirectory(prefix="t5-monitor-") as td:
        fb_path = os.path.join(td, "framebuffer.ppm")
        plain_log = os.path.join(td, "plain.log")
        mon_log = os.path.join(td, "monitor.log")
        args = ["--platform=haswell", "--max-instr=600000", "--stats",
                "--dump-fb=" + fb_path, "--log=" + plain_log]
        plain = subprocess.run([X86EMU] + args, capture_output=True, timeout=60)
        check(plain.returncode == 0, "plain firmware control run exits 0")
        try:
            with open(fb_path, "rb") as f:
                plain_fb = f.read()
        except OSError:
            plain_fb = None

        margs = ["--platform=haswell", "--max-instr=600000", "--stats",
                 "--dump-fb=" + fb_path, "--log=" + mon_log]
        p, port = start_monitor(margs)
        c = None
        mon_out = None
        try:
            c = assert_http_and_handshake(port)
            if not c:
                return
            kind, start, _ = c.recv_event(10)
            check(kind == "text" and start.get("stop_reason") == "start" and start.get("mode") == "real16",
                  "A start state contains mode, counters and reset-vector stop")
            check(start.get("platform", "").lower().startswith("haswell") and
                  isinstance(start.get("registers"), dict) and len(start.get("vcpus", [])) == 1,
                  "A JSON state includes platform, registers and vCPUs")
            kind, fb, fb_marker = c.recv_event(10)
            if kind == "binary":
                check(fb[:3] == b"FB\x01" and fb[3] & 1 and c.width == 800 and c.height == 600,
                      "A initial full framebuffer keyframe is 800x600")
                check(fb_marker == 127 and len(fb) > 65535,
                      "A full framebuffer uses the RFC 64-bit payload length")
            else:
                check(False, "A initial framebuffer keyframe arrives after state")
            state_event = next((x for x in c.frame_lengths if x[0] == 1 and x[1] == 126), None)
            check(state_event is not None, "A JSON state uses the RFC 16-bit payload length")

            c.command("exec", line="info regs")
            r = c.wait_reply("exec")
            check(r.get("ok") and "rip" in r.get("output", ""), "A exec runs read-only debugger REPL command")
            c.command("break", addr="0x100000")
            br = c.wait_reply("break")
            check(br.get("ok") and "0x0000000000100000" in br.get("message", ""),
                  "A breakpoint is set through the T2 run-control core")
            c.command("continue")
            check(c.wait_reply("continue").get("ok"), "A continue from the reset stop")
            stop, _ = c.wait_state(lambda s: s.get("stop_reason") == "breakpoint", 15)
            check(stop.get("registers", {}).get("rip") == "0x0000000000100000",
                  "A breakpoint stops at the guest entry")
            c.command("step")
            check(c.wait_reply("step").get("ok"), "A one-instruction step request")
            step, _ = c.wait_state(lambda s: s.get("stop_reason") == "step", 10)
            check(step.get("registers", {}).get("rip") == "0x0000000000100001",
                  "A step advances exactly one instruction")
            c.command("exec", line="info regs")
            check(c.wait_reply("exec").get("ok"), "A exec works at a debugger stop")
            c.command("continue")
            check(c.wait_reply("continue").get("ok"), "A continue resumes the stepped guest")
            end, _ = c.wait_state(lambda s: s.get("stop_reason") == "end", 30)
            check(end.get("executed") == "600000", "A final stop reports the completed work count")
            check(c.wait_pixel((0x20, 0, 0), 10),
                  "A framebuffer delta carries the guest's 0x00200000 marker pixel")
            c.command("keys", codes="1E,9E")
            check(c.wait_reply("keys").get("ok"), "A keys inject Set-1 scancode bytes")
            c.command("type", text="A\n")
            tr = c.wait_reply("type")
            check(tr.get("ok") and "6 scancode" in tr.get("message", ""),
                  "A type maps ASCII make/break and Shift keys")
            c.command("quit")
            check(c.wait_reply("quit").get("ok"), "A quit command is acknowledged")
            try:
                mon_out, _ = p.communicate(timeout=30)
            except subprocess.TimeoutExpired:
                p.kill()
                mon_out, _ = p.communicate()
                check(False, "A monitor process terminates after quit")
            check(p.returncode == 0, "A monitor process exits 0")
        finally:
            if c:
                c.close()
            if p.poll() is None:
                p.kill()
                p.communicate()

        check(mon_out is not None and mon_out == plain.stdout,
              "A monitored and plain stdout (including --stats) are byte-identical")
        try:
            with open(plain_log, "rb") as f:
                plain_log_bytes = f.read()
            with open(mon_log, "rb") as f:
                mon_log_bytes = f.read()
            logs_match = plain_log_bytes == mon_log_bytes
        except OSError:
            logs_match = False
        check(logs_match, "A monitored and plain log rings are byte-identical")
        try:
            with open(fb_path, "rb") as f:
                mon_fb = f.read()
        except OSError:
            mon_fb = None
        check(plain_fb is not None and mon_fb is not None and plain_fb == mon_fb,
              "A monitored and plain framebuffer dumps are byte-identical")


def lane_pause_step_quit():
    with tempfile.TemporaryDirectory(prefix="t5-monitor-loop-") as td:
        rom_path = os.path.join(td, "loop.rom")
        rom = bytearray(b"\xf4" * 131072)
        rom[0x1FFF0:0x1FFF2] = b"\xeb\xfe"  # endless jmp $ at reset vector
        open(rom_path, "wb").write(rom)
        p, port = start_monitor(["--rom=" + rom_path, "--max-instr=100000000000"])
        c = None
        try:
            c = assert_http_and_handshake(port)
            if not c: return
            c.recv_event(5); c.recv_event(5)
            c.command("continue")
            check(c.wait_reply("continue").get("ok"), "B continue starts the endless loop")
            c.command("pause")
            check(c.wait_reply("pause").get("ok"), "B pause is accepted asynchronously while running")
            paused, _ = c.wait_state(lambda s: s.get("stop_reason") == "pause", 10)
            check(paused.get("mode") == "real16", "B pause stops at an instruction boundary")
            c.command("step")
            check(c.wait_reply("step").get("ok"), "B step resumes from the pause stop")
            stepped, _ = c.wait_state(lambda s: s.get("stop_reason") == "step", 10)
            check(stepped.get("registers", {}).get("rip") == paused.get("registers", {}).get("rip"),
                  "B one `jmp $` step returns to the same RIP")
            c.command("quit")
            check(c.wait_reply("quit").get("ok"), "B quit requests a clean stop")
            try: p.communicate(timeout=10)
            except subprocess.TimeoutExpired: p.kill(); p.communicate()
            check(p.returncode == 0, "B emulator exits cleanly after quit")
        finally:
            if c: c.close()
            if p.poll() is None: p.kill(); p.wait()
            p.communicate()


def rsp_send(s, data):
    b = data.encode("ascii")
    s.sendall(b"$" + b + b"#%02x" % (sum(b) & 0xFF))


def lane_gdb_readonly():
    gport, mport = free_port(), free_port()
    while mport == gport: mport = free_port()
    args = [X86EMU, "--rom=/dev/zero", "--max-instr=100000000000",
            "--gdb=127.0.0.1:%d" % gport, "--monitor=127.0.0.1:%d" % mport]
    # Replace /dev/zero with a valid 128-KiB loop ROM so the reset vector executes safely.
    with tempfile.TemporaryDirectory(prefix="t5-monitor-gdb-") as td:
        rom_path = os.path.join(td, "loop.rom")
        rom = bytearray(b"\xf4" * 131072); rom[0x1FFF0:0x1FFF2] = b"\xeb\xfe"
        open(rom_path, "wb").write(rom); args[1] = "--rom=" + rom_path
        p = subprocess.Popen(args, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        c = None; g = None
        try:
            deadline = time.monotonic() + 15
            while g is None and time.monotonic() < deadline:
                try: g = socket.create_connection(("127.0.0.1", gport), timeout=1)
                except OSError: time.sleep(0.03)
            check(g is not None, "G GDB endpoint accepts its owner before monitor startup stop")
            if g is None: return
            g.settimeout(5)
            status, body = wait_http(mport)
            check(status == 200 and b"WebSocket" in body, "G monitor HTTP remains available with GDB attached")
            c = WSClient(mport)
            check("HTTP/1.1 101" in c.header and "s3pPLMBiTxaQ9kYGzzhZRbK+xOo=" in c.header,
                  "G monitor WebSocket handshake with GDB attached")
            c.recv_event(5); c.recv_event(5)
            c.command("continue")
            ro = c.wait_reply("continue")
            check(not ro.get("ok") and "read-only" in ro.get("message", ""),
                  "G monitor run control is read-only while GDB is attached")
            rsp_send(g, "k")
            try:
                ack = g.recv(1)
                check(ack == b"+", "G GDB kill packet is accepted")
            except OSError:
                check(False, "G GDB kill packet is accepted")
            try: p.communicate(timeout=10)
            except subprocess.TimeoutExpired: p.kill(); p.communicate()
            check(p.returncode == 0, "G GDB-owned emulator exits cleanly")
        finally:
            if c: c.close()
            if g:
                try: g.close()
                except OSError: pass
            if p.poll() is None: p.kill(); p.wait()
            p.communicate()


def lane_framing():
    # A live server accepts masked client text frames with short, 16-bit and
    # 64-bit lengths. JSON whitespace after the command is legal and makes
    # the extended-length cases exercise the actual network parser.
    p, port = start_monitor(["--max-instr=1000000000"])
    c = None
    try:
        c = assert_http_and_handshake(port)
        if not c: return
        c.recv_event(5); c.recv_event(5)
        base = b'{"cmd":"exec","line":"info regs"}'
        for n, marker in ((len(base), len(base)), (126, 126), (65536, 127)):
            payload = base + b" " * (n - len(base))
            c.send_raw_frame(payload, masked=True)
            observed = c.sent_frame_lengths[-1]
            if n <= 126:
                reply = c.wait_reply("exec", 10)
                ok = reply.get("ok")
            else:
                # 64-bit frame lengths are parsed, then rejected by the
                # deliberately smaller command-size policy (not as bad framing).
                reply = c.wait_reply("", 10)
                ok = not reply.get("ok") and reply.get("message") == "text command is too long"
            check(ok and observed[1] == marker and observed[2] == n and observed[3],
                  "codec masked client frame length=%d parsed" % n)
        # Client frames are required to be masked; the server closes a bad peer.
        c.send_raw_frame(b"x", masked=False)
        try:
            kind, payload, _ = c.recv_event(5)
            closed = kind == "other" and payload[:2] == struct.pack("!H", 1002)
        except (EOFError, OSError, socket.timeout):
            closed = True
        check(closed, "codec rejects an unmasked client frame")
    finally:
        if c: c.close()
        if p.poll() is None: p.kill(); p.wait()
        p.communicate()


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--lane", choices=("all", "handshake", "boot", "pause", "gdb", "framing"), default="all")
    args = parser.parse_args()
    lanes = {
        "handshake": lane_handshake,
        "boot": lane_boot,
        "pause": lane_pause_step_quit,
        "gdb": lane_gdb_readonly,
        "framing": lane_framing,
    }
    selected = list(lanes) if args.lane == "all" else [args.lane]
    for name in selected:
        try:
            lanes[name]()
        except Exception as exc:
            check(False, "%s raised %r" % (name, exc))
    print("PASS test-monitor (%d checks)" % checks if not failures else
          "FAIL test-monitor (%d failures / %d checks)" % (failures, checks))
    return 1 if failures else 0


if __name__ == "__main__":
    raise SystemExit(main())
