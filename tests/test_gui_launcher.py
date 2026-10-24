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
  E. UPKEEP U7 access gate: the API and /ws refuse a missing or wrong
     token, a DNS-rebinding Host and a cross-origin Origin; the page
     carries the token (loopback) and sets the SameSite=Strict cookie;
     the happy paths above run exactly like the browser does -- they
     scrape the token out of the served page and present it back. A
     second launcher on the any-address proves the remote shape: no
     token in the page, a login form, the persistent 0600 access-token
     file, and the plaintext warning.
"""
import base64
import http.client as http_client_mod   # the bare name http is a helper below
import json
import os
import re
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


_TOKENS = {}


def page_token(port):
    """The browser's bootstrap, mirrored (UPKEEP U7): the capability is
    embedded in the page the loopback launcher serves. Scraped once per
    port; empty string when the page carries none -- which is exactly
    the pre-U7 negative-control situation, where every refusal check in
    lane E then fails while the happy paths still run."""
    if port not in _TOKENS:
        _code, raw = http(port, "/", _auth=False)
        m = re.search(rb'X86EMU_TOKEN="([^"]*)"', raw)
        _TOKENS[port] = m.group(1).decode() if m else ""
    return _TOKENS[port]


def http(port, path, method="GET", body=None, _auth=True):
    url = "http://127.0.0.1:%d%s" % (port, path)
    data = json.dumps(body).encode() if body is not None else None
    headers = {"Content-Type": "application/json"}
    if _auth:
        headers["X-X86emu-Token"] = page_token(port)
    req = urllib.request.Request(url, data=data, method=method,
                                 headers=headers)
    try:
        with urllib.request.urlopen(req, timeout=10) as r:
            return r.status, r.read()
    except urllib.error.HTTPError as e:
        return e.code, e.read()


def http_raw(port, path, method="GET", headers=None, body=b""):
    """Hand-set headers (Host/Origin/token) that urllib would normalize
    away; returns (status, headers-dict, body)."""
    conn = http_client_mod.HTTPConnection("127.0.0.1", port, timeout=10)
    conn.request(method, path, body=body,
                 headers=headers if headers is not None
                 else {"Host": "127.0.0.1:%d" % port})
    r = conn.getresponse()
    raw = r.read()
    hd = {k.lower(): v for k, v in r.getheaders()}
    conn.close()
    return r.status, hd, raw


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


class WSock:
    """Raw client socket + bytes already read past the 101 headers.

    Measured flake: the monitor's state push coalesces into the same TCP
    segment as the handshake response about one run in six; a header-only
    reader that discards that tail then "receives" mid-frame garbage and
    the whole lifecycle block fails.  MonitorClient in tools/gui_app.py
    already keeps this prefix -- the test must too."""

    def __init__(self, s, prefix=b""):
        self.s = s
        self.buf = prefix

    def sendall(self, data):
        self.s.sendall(data)

    def close(self):
        self.s.close()

    def recv(self, n):
        while len(self.buf) < n:
            d = self.s.recv(65536)
            if not d:
                break
            self.buf += d
        out, self.buf = self.buf[:n], self.buf[n:]
        return out


def ws_handshake(port):
    s = socket.create_connection(("127.0.0.1", port), timeout=5)
    key = base64.b64encode(os.urandom(16)).decode()
    s.sendall(("GET /ws?token=%s HTTP/1.1\r\nHost: 127.0.0.1:%d\r\n"
               "Upgrade: websocket\r\nConnection: Upgrade\r\n"
               "Sec-WebSocket-Key: %s\r\nSec-WebSocket-Version: 13\r\n\r\n"
               % (page_token(port), port, key)).encode())
    s.settimeout(10)
    buf = b""
    while b"\r\n\r\n" not in buf:
        chunk = s.recv(4096)
        if not chunk:
            break
        buf += chunk
    head, _, rest = buf.partition(b"\r\n\r\n")
    # Frame reads inherit this timeout.  Sanitized emulators are 10-20x
    # slower and one `make test-sanitize` run measured a 10s state-push
    # flake here; 30s keeps the vector honest without masking deadlocks.
    s.settimeout(30)
    return WSock(s, rest), head.decode(errors="replace")


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
    # refusals first: no machine yet -- but an *authorized* request now:
    # pre-U7 this probe sent Host: x and still got its 502 (the defect);
    # the same headers are lane E's 403 evidence.
    try:
        s = socket.create_connection(("127.0.0.1", port), timeout=5)
        key = base64.b64encode(os.urandom(16)).decode()
        s.sendall(("GET /ws?token=%s HTTP/1.1\r\nHost: 127.0.0.1:%d\r\n"
                   "Upgrade: websocket\r\nConnection: Upgrade\r\n"
                   "Sec-WebSocket-Key: %s\r\nSec-WebSocket-Version: 13\r\n\r\n"
                   % (page_token(port), port, key)).encode())
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


# ---------------- E. UPKEEP U7 access gate -----------------------------------


def _ws_probe(port, path="/ws", host=None, origin=None, headers=None):
    s = socket.create_connection(("127.0.0.1", port), timeout=5)
    key = base64.b64encode(os.urandom(16)).decode()
    lines = ["GET %s HTTP/1.1" % path,
             "Host: %s" % (host or "127.0.0.1:%d" % port),
             "Upgrade: websocket", "Connection: Upgrade",
             "Sec-WebSocket-Key: %s" % key, "Sec-WebSocket-Version: 13"]
    if origin is not None:
        lines.append("Origin: %s" % origin)
    if headers:
        lines.extend(headers)
    s.sendall(("\r\n".join(lines) + "\r\n\r\n").encode())
    s.settimeout(5)
    out = b""
    try:
        while b"\r\n" not in out:
            d = s.recv(4096)
            if not d:
                break
            out += d
    except OSError:
        pass
    s.close()
    return out.decode(errors="replace").splitlines()[0] if out else ""


# Symbol lookups are getattr-guarded so the pre-U7 negative control reports
# clean FAILs instead of AttributeError on a launcher module that has no
# access gate yet.
COOKIE_NAME = getattr(launcher, "TOKEN_COOKIE", "x86emu_token")
TOKEN_FILE = getattr(launcher, "ACCESS_TOKEN_FILE", "access-token")


def test_access_gate(port):
    tok = page_token(port)
    good = {"Host": "127.0.0.1:%d" % port}

    code, _h, _b = http_raw(port, "/api/status", headers=good)
    check(code == 401, "U7: API without a token is 401")
    h = dict(good, **{"X-X86emu-Token": "wrong"})
    code, _h, _b = http_raw(port, "/api/status", headers=h)
    check(code == 401, "U7: API with a wrong token is 401")
    h = dict(good, **{"X-X86emu-Token": tok})
    code, _h, _b = http_raw(port, "/api/status", headers=h)
    check(code == 200 and len(tok) >= 32, "U7: API with the page token is 200")

    # DNS rebinding: the attacker's name is not this machine
    h = {"Host": "evil.example", "X-X86emu-Token": tok}
    code, _h, _b = http_raw(port, "/api/status", headers=h)
    check(code == 403, "U7: rebinding Host on the API is 403")
    code, _h, _b = http_raw(port, "/", headers={"Host": "evil.example"})
    check(code == 403, "U7: rebinding Host on the page is 403")

    # cross-origin mutation: a browser cannot omit Origin on POST
    h = dict(good, **{"X-X86emu-Token": tok, "Origin": "http://evil.example"})
    code, _h, _b = http_raw(port, "/api/stop", "POST", h, b"{}")
    check(code == 403, "U7: cross-Origin POST is 403 even with the token")
    h = dict(good, **{"X-X86emu-Token": tok,
                       "Origin": "http://127.0.0.1:%d" % port})
    code, _h, _b = http_raw(port, "/api/stop", "POST", h, b"{}")
    check(code == 200, "U7: same-Origin POST with the token passes")

    # the upgrade: token first, then Origin (checked with a token so the
    # 403 is the Origin branch, not the 401), then Host
    line = _ws_probe(port)
    check("401" in line, "U7: /ws without a token is 401")
    line = _ws_probe(port, "/ws?token=wrong")
    check("401" in line, "U7: /ws with a wrong token is 401")
    line = _ws_probe(port, "/ws?token=%s" % tok, origin="http://evil.example")
    check("403" in line, "U7: /ws with a cross-Origin is 403")
    line = _ws_probe(port, "/ws?token=%s" % tok, host="evil.example")
    check("403" in line, "U7: /ws with a rebinding Host is 403")

    # the bootstrap contract the browser relies on
    code, hd, raw = http_raw(port, "/", headers=good)
    cookie = hd.get("set-cookie", "")
    check(code == 200 and tok != "" and tok.encode() in raw,
          "U7: loopback page carries the token")
    check(COOKIE_NAME in cookie and "HttpOnly" in cookie
          and "SameSite=Strict" in cookie,
          "U7: page sets an HttpOnly SameSite=Strict token cookie")
    check(hd.get("cache-control") == "no-store",
          "U7: token-bearing page is no-store")
    h = dict(good, Cookie="%s=%s" % (COOKIE_NAME, tok))
    code, _h, _b = http_raw(port, "/api/status", headers=h)
    check(code == 200, "U7: the cookie alone authorizes the API")


def test_remote_gate(emu):
    """A non-loopback launcher (bound to the any-address, still reached
    here over 127.0.0.1: Host literals stay allowed): the page embeds
    nothing, login demands the state-dir credential, and startup prints
    the plaintext warning. The remote token file is owner-only."""
    tmp = tempfile.mkdtemp(prefix="gui-remote-")
    s = socket.socket()
    s.bind(("127.0.0.1", 0))
    port = s.getsockname()[1]
    s.close()
    proc = subprocess.Popen(
        [sys.executable, os.path.join(ROOT, "tools", "launcher.py"),
         "--bind", "0.0.0.0", "--port", str(port), "--state-dir", tmp,
         "--emu", emu],
        stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    try:
        good = {"Host": "127.0.0.1:%d" % port}
        for _ in range(50):
            try:
                code, hd, raw = http_raw(port, "/", headers=good)
                if code == 200:
                    break
            except OSError:
                pass
            time.sleep(0.1)
        text = raw.decode("utf-8", errors="replace")
        check("X86EMU_TOKEN" not in text and "Access token" in text,
              "U7 remote: bare / is the login form, no token embedded")

        tok_path = os.path.join(tmp, TOKEN_FILE)
        tok, mode = "", 0
        if os.path.exists(tok_path):
            mode = os.stat(tok_path).st_mode & 0o777
            with open(tok_path) as f:
                tok = f.read().strip()
        check(mode == 0o600 and len(tok) >= 32,
              "U7 remote: access-token file exists, 0600, strong")

        code, _hd, raw = http_raw(port, "/?token=wrong", headers=good)
        check(b"X86EMU_TOKEN" not in raw and "Wrong".encode() in raw,
              "U7 remote: a wrong token stays on the login form")
        code, hd, raw = http_raw(port, "/?token=%s" % tok, headers=good)
        check(code == 200 and b"X86EMU_TOKEN" in raw
              and "SameSite=Strict" in hd.get("set-cookie", ""),
              "U7 remote: the file token unlocks the page + cookie")

        code, _hd, _b = http_raw(
            port, "/api/status",
            headers=dict(good, **{"X-X86emu-Token": "wrong"}))
        check(code == 401, "U7 remote: API rejects a wrong token (401)")
        code, _hd, _b = http_raw(port, "/api/status", headers=good)
        check(code == 401, "U7 remote: API rejects a tokenless call (401)")

        line = _ws_probe(port)
        check("401" in line, "U7 remote: /ws rejects a tokenless upgrade")

        # rotation: the helper the desktop GUI button calls
        rotate = getattr(launcher, "load_or_create_access_token", None)
        rotated_ok = False
        if rotate is not None:
            new, _p, created = rotate(tmp, force_new=True)
            rotated_ok = created and new != tok and \
                open(tok_path).read().strip() == new
        check(rotated_ok, "U7 remote: rotation rewrites the credential file")
    finally:
        proc.terminate()
        try:
            out, _ = proc.communicate(timeout=5)
        except subprocess.TimeoutExpired:
            proc.kill()
            out, _ = proc.communicate()
    check(b"plaintext" in out and b"token" in out.lower(),
          "U7 remote: startup prints the plaintext warning + token URL")


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
        test_access_gate(port)
        test_lifecycle(port)
        test_remote_gate(emu)
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
