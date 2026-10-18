#!/usr/bin/env python3
"""tests/test_gui_launcher.py -- GUI G1 vectors for tools/launcher.py.

The launcher is the no-command-line back end: it must build the emulator's
argv strictly from its SCHEMA (nothing user-supplied can become a flag),
serve the GUI page, manage the machine lifecycle, and splice the WebSocket
monitor protocol through /ws.

Vectors:
  A. argv construction and schema validation (incl. the rejection NCs).
  B. HTTP surface: page, status, meta, files, presets round trip.
  C. lifecycle + WS proxy against the real emulator: start -> running,
     RFC 6455 handshake through the proxy, first frame is a state push,
     continue -> the run ends with exit 0 and the known receipts, artifacts
     land in the run dir, output tails readable.
  D. refusals: /ws with no machine is 502, unknown API 404, snapshot delete
     of a missing file, bad preset name.
"""
import base64
import json
import os
import socket
import subprocess
import sys
import tempfile
import time
import urllib.error
import urllib.request

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, "tools"))
import launcher  # noqa: E402

fails = 0


def check(cond, label):
    global fails
    if cond:
        print("ok  ", label)
    else:
        print("FAIL", label)
        fails += 1


def http(port, path, method="GET", body=None):
    url = "http://127.0.0.1:%d%s" % (port, path)
    data = json.dumps(body).encode() if body is not None else None
    req = urllib.request.Request(url, data=data, method=method,
                                 headers={"Content-Type": "application/json"})
    try:
        with urllib.request.urlopen(req, timeout=10) as r:
            return r.status, r.read()
    except urllib.error.HTTPError as e:
        return e.code, e.read()


def jhttp(port, path, method="GET", body=None):
    code, raw = http(port, path, method, body)
    try:
        return code, json.loads(raw.decode() or "{}")
    except ValueError:
        return code, {}


# ---------------- A. schema / argv ------------------------------------------

def test_schema():
    cfg = launcher.validate_config({
        "platform": "haswell", "rom": "firmware/firmware.bin",
        "disk": "disk/disk.img", "max_instr": 200000, "stats": True,
        "log": True, "dump_fb": True, "smp": 2, "lpc": "ich9",
        "vbe": "1024x768x32", "tap": "127.0.0.1:8080", "ahci2": True,
        "gdb": "1234", "watch_phys": "0x100000", "keys": "1E,9E",
    })
    argv = launcher.build_argv("./x86emu", cfg, "/tmp/runX", 4242)
    joined = " ".join(argv)
    check(argv[0] == "./x86emu", "argv starts at the emulator")
    check("--monitor=127.0.0.1:4242" in joined, "argv carries exactly the monitor spec")
    check(joined.count("--monitor=") == 1, "monitor spec appears once")
    check("--platform=haswell" in joined and "--smp=2" in joined, "choice + int flags")
    check("--stats" in joined and "--ahci2" in joined, "bool flags")
    check("--log=/tmp/runX/log.txt" in joined and "--dump-fb=/tmp/runX/framebuffer.ppm" in joined,
          "managed outputs point into the run dir")
    check("--vbe=1024x768x32" in joined and "--tap=127.0.0.1:8080" in joined, "regex strings")
    check("--gdb=1234" in joined and "--watch-phys=0x100000" in joined, "debug strings")

    # rejection NCs: every one of these must be refused, not sanitized
    def rejects(cfg, label):
        try:
            launcher.validate_config(cfg)
            check(False, label + " (accepted!)")
        except launcher.ConfigError:
            check(True, label)

    rejects({"platform": "haswell; rm -rf /"}, "reject platform injection")
    rejects({"unknown_flag": 1}, "reject unknown key")
    rejects({"rom": "/nonexistent/nope.bin"}, "reject missing path")
    rejects({"smp": 99}, "reject smp out of range")
    rejects({"max_instr": "abc"}, "reject non-integer")
    rejects({"lpc": "evil"}, "reject lpc choice")
    rejects({"vbe": "99999x99999x7"}, "reject vbe pattern")
    rejects({"tap": "host;cat /etc/passwd"}, "reject tap pattern")
    rejects({"keys": "zz,zz"}, "reject keys pattern")
    rejects({"log": "/etc/passwd"}, "reject managed output override")

# ---------------- B. HTTP surface -------------------------------------------


def test_http_surface(port):
    code, raw = http(port, "/")
    check(code == 200 and b"AuraLite" in raw and "GUI".encode() in raw,
          "GET / serves the GUI page")
    code, st = jhttp(port, "/api/status")
    check(code == 200 and st.get("ok") and st.get("state") in ("off", "exited", "running"),
          "status endpoint answers")
    code, meta = jhttp(port, "/api/meta")
    check(code == 200 and "haswell" in meta.get("platforms", []), "meta lists platforms")
    code, files = jhttp(port, "/api/files?path=firmware&ext=.bin")
    check(code == 200 and any(e["name"] == "firmware.bin" for e in files.get("entries", [])),
          "file browser lists the ROM")
    code, _ = jhttp(port, "/api/presets", "POST", {"name": "smoke", "config": {"platform": "haswell"}})
    check(code == 200, "preset save")
    code, p = jhttp(port, "/api/presets")
    check(code == 200 and p.get("presets", {}).get("smoke", {}).get("platform") == "haswell",
          "preset round trip")
    code, _ = jhttp(port, "/api/presets?name=smoke", "DELETE")
    code, p = jhttp(port, "/api/presets")
    check("smoke" not in p.get("presets", {}), "preset delete")
    code, _ = jhttp(port, "/api/presets", "POST", {"name": "bad name!!", "config": {}})
    check(code == 400, "reject bad preset name")
    code, _ = jhttp(port, "/api/nope")
    check(code == 404, "unknown API is 404")

# ---------------- C. lifecycle + WS proxy -----------------------------------


def ws_handshake(port):
    s = socket.create_connection(("127.0.0.1", port), timeout=5)
    key = base64.b64encode(os.urandom(16)).decode()
    s.sendall(("GET /ws HTTP/1.1\r\nHost: 127.0.0.1:%d\r\n"
               "Upgrade: websocket\r\nConnection: Upgrade\r\n"
               "Sec-WebSocket-Key: %s\r\nSec-WebSocket-Version: 13\r\n\r\n"
               % (port, key)).encode())
    s.settimeout(10)
    buf = b""
    while b"\r\n\r\n" not in buf:
        chunk = s.recv(4096)
        if not chunk:
            break
        buf += chunk
    head = buf.split(b"\r\n\r\n")[0].decode(errors="replace")
    return s, head


def ws_frame(s):
    h = s.recv(2)
    if len(h) < 2:
        return None, None
    ln = h[1] & 0x7F
    if ln == 126:
        ln = int.from_bytes(s.recv(2), "big")
    elif ln == 127:
        ln = int.from_bytes(s.recv(8), "big")
    data = b""
    while len(data) < ln:
        data += s.recv(ln - len(data))
    return h[0] & 0x0F, data


def test_lifecycle(port):
    # refusals first: no machine yet
    try:
        s = socket.create_connection(("127.0.0.1", port), timeout=5)
        key = base64.b64encode(os.urandom(16)).decode()
        s.sendall(("GET /ws HTTP/1.1\r\nHost: x\r\nUpgrade: websocket\r\n"
                   "Connection: Upgrade\r\nSec-WebSocket-Key: %s\r\n"
                   "Sec-WebSocket-Version: 13\r\n\r\n" % key).encode())
        s.settimeout(5)
        head = s.recv(200).decode(errors="replace")
        s.close()
        check("502" in head, "/ws without a machine is 502")
    except OSError:
        check(False, "/ws without a machine is 502")

    code, r = jhttp(port, "/api/start", "POST", {"config": {
        "platform": "haswell", "rom": "firmware/firmware.bin",
        "disk": "disk/disk.img", "max_instr": 60000, "stats": True,
        "log": True, "dump_fb": True}})
    check(code == 200 and r.get("ok"), "machine starts")
    time.sleep(0.6)
    code, st = jhttp(port, "/api/status")
    check(st.get("state") == "running", "status reports running")

    s, head = ws_handshake(port)
    check("101" in head.splitlines()[0] and "Sec-WebSocket-Accept" in head,
          "WS proxy: 101 + Sec-WebSocket-Accept")
    op, data = ws_frame(s)
    first = json.loads(data) if op == 1 else {}
    check(op == 1 and first.get("kind") == "state", "first WS frame is a state push")
    check("platform" in first and "registers" in first, "state carries registers")

    # unmasked frames from a raw client are refused by the monitor codec --
    # the proxy must pass MASKED client frames through untouched
    raw = json.dumps({"cmd": "continue"}).encode()
    mask = os.urandom(4)
    masked = bytes(b ^ mask[i % 4] for i, b in enumerate(raw))
    s.sendall(bytes([0x81, 0x80 | len(raw)]) + mask + masked)
    saw_reply = False
    end_stop = False
    for _ in range(40):
        op, data = ws_frame(s)
        if op == 1:
            m = json.loads(data)
            if m.get("kind") == "reply":
                saw_reply = True
            if m.get("kind") == "state" and m.get("stop_reason") == "end":
                end_stop = True
                break
    check(saw_reply, "continue reaches the machine through the proxy")
    check(end_stop, "run reaches the post-mortem end stop (T5 semantics)")

    # the polite way down: quit makes the emulator print stats/receipts and
    # write the artifacts (SIGTERM would skip the teardown block)
    raw = json.dumps({"cmd": "quit"}).encode()
    mask = os.urandom(4)
    masked = bytes(b ^ mask[i % 4] for i, b in enumerate(raw))
    s.sendall(bytes([0x81, 0x80 | len(raw)]) + mask + masked)
    acked = False
    for _ in range(20):
        op, data = ws_frame(s)
        if op == 1 and json.loads(data).get("kind") == "reply":
            acked = True
            break
    check(acked, "quit is acknowledged at the end stop")
    s.close()

    deadline = time.time() + 20
    while time.time() < deadline:
        code, st = jhttp(port, "/api/status")
        if st.get("state") == "exited":
            break
        time.sleep(0.3)
    check(st.get("state") == "exited" and st.get("exit_code") == 0,
          "run ends with exit code 0 after quit")

    code, arts = jhttp(port, "/api/artifacts")
    names = [e["name"] for e in arts.get("entries", [])]
    check("framebuffer.ppm" in names and "log.txt" in names and "stdout.log" in names,
          "artifacts land in the run dir")
    code, out = jhttp(port, "/api/output?which=stdout&tail=40")
    check("GUEST_OK" in out.get("output", ""), "stdout tail shows the guest receipt")
    code, raw = http(port, "/api/artifact/framebuffer.ppm")
    check(code == 200 and raw[:2] == b"P6", "artifact download returns the PPM")
    code, raw = http(port, "/api/artifact/../../etc/passwd")
    check(code in (404, 400), "artifact path traversal is refused")

    code, r = jhttp(port, "/api/snapshots?name=missing.snap", "DELETE")
    check(code == 200 and not r.get("ok"), "missing snapshot delete is refused")

    code, r = jhttp(port, "/api/stop", "POST", {})
    check(code == 200, "stop on an exited machine is harmless")


def main():
    emu = os.path.join(ROOT, "x86emu")
    if not os.path.isfile(emu):
        print("SKIP: build x86emu first")
        return 0
    tmp = tempfile.mkdtemp(prefix="gui-test-")
    s = socket.socket()
    s.bind(("127.0.0.1", 0))
    port = s.getsockname()[1]
    s.close()
    proc = subprocess.Popen(
        [sys.executable, os.path.join(ROOT, "tools", "launcher.py"),
         "--bind", "127.0.0.1", "--port", str(port), "--state-dir", tmp,
         "--emu", emu],
        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    try:
        for _ in range(50):
            try:
                code, _ = jhttp(port, "/api/status")
                if code == 200:
                    break
            except OSError:
                pass
            time.sleep(0.1)
        test_schema()
        test_http_surface(port)
        test_lifecycle(port)
    finally:
        proc.terminate()
        try:
            proc.wait(timeout=5)
        except subprocess.TimeoutExpired:
            proc.kill()
    if fails:
        print("test-gui-launcher: %d FAILED" % fails)
        return 1
    print("test-gui-launcher: all vectors passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
