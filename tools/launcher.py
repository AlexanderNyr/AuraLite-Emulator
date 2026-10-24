#!/usr/bin/env python3
"""tools/launcher.py -- the no-command-line GUI back end (GUI G1).

A single stdlib-only Python service that makes every x86emu facility
reachable from `web/gui.html`:

  * machine configuration and process lifecycle (start / stop / restart),
    built strictly from the option SCHEMA below -- user input can never
    become an arbitrary flag;
  * a raw WebSocket proxy (/ws) spliced onto the running machine's live
    monitor (RFC 6455 frames pass through untouched, so the existing
    monitor protocol -- state/reply/framebuffer -- works as-is);
  * server-side file browsing for ROM / disk / kernel / initrd / snapshot
    paths, artifact download (log, trace, dumps, stats), named presets
    and the host TAP responder as a managed side process.

Ground rules:
  * the emulator is spawned exactly like the CLI would (`argv` list, no
    shell) with cwd = the repo root, so every measured receipt of the
    command-line world is byte-identical here;
  * the machine's monitor is bound to 127.0.0.1:<free port> and only the
    launcher's /ws is exposed -- one port for the whole GUI;
  * run state (presets, per-run output, snapshots) lives in a state
    directory (--state-dir, default ~/.x86emu-gui), never in the repo.

Access control (UPKEEP U7; threat model: UPKEEP_PLAN s1.7):
  * a capability token gates every API route and the /ws splice. On a
    loopback bind the token is per-process random and is embedded into
    the served GUI page, so the default flow stays one-click; on a
    non-loopback bind it is the persistent `access-token` file (0600) in
    the state directory -- the desktop GUI shows and rotates it -- and
    the page embeds nothing until the visitor presents it. Carriers:
    `X-X86emu-Token` / `Authorization: Bearer` / `?token=` / the
    SameSite=Strict cookie the page response sets.
  * the Host header must name this machine (loopback names, the bound
    literal, this host's name): a DNS-rebinding page's Host is none of
    those, so it never reaches a route. Mutating routes and /ws also
    require a same-origin `Origin` when a browser sends one.
  * each machine run gets its own monitor token, handed to the emulator
    in its environment (never argv, so it stays out of argv.json and
    process lists); the /ws proxy presents it to the monitor, which is
    therefore never the unauthenticated thing its header once said.
"""
import argparse
import hmac
import html as _html
import ipaddress
import json
import os
import re
import secrets
import signal
import socket
import subprocess
import sys
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import parse_qs, urlparse, unquote

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
GUI_PAGE = os.path.join(ROOT, "web", "gui.html")
DEFAULT_EMU = os.path.join(ROOT, "x86emu")
RESPONDER = os.path.join(ROOT, "tools", "tap_responder.py")

# ---------------------------------------------------------------------------
# Option schema: the single source of truth for what the GUI may start.
# Each key is a GUI field; the value describes the flag it becomes.
# ---------------------------------------------------------------------------

PLATFORMS = ["sandybridge", "ivybridge", "haswell", "broadwell", "baytrail"]
LPCS = ["pantherpoint", "ich9"]

SCHEMA = {
    # boot path
    "rom":            {"flag": "--rom=",           "type": "path"},
    "disk":           {"flag": "--disk=",          "type": "path"},
    "kernel":         {"flag": "--kernel=",        "type": "path"},
    "initrd":         {"flag": "--initrd=",        "type": "path"},
    "snapshot_load":  {"flag": "--snapshot-load=", "type": "path"},
    # cpu / platform
    "platform":       {"flag": "--platform=",      "type": "choice", "choices": PLATFORMS},
    "smp":            {"flag": "--smp=",           "type": "int", "min": 1, "max": 8},
    "cpus":           {"flag": "--cpus=",          "type": "int", "min": 1, "max": 64},
    "smp_probe":      {"flag": "--smp-probe",      "type": "bool"},
    # chipset
    "lpc":            {"flag": "--lpc=",           "type": "choice", "choices": LPCS},
    "no_usb_uhci":    {"flag": "--no-usb-uhci",    "type": "bool"},
    # storage (ports 0..3 per controller; port 0 == the shorthand form)
    "sata":           {"flag": "--sata=",          "type": "path"},
    "sata1":          {"flag": "--sata-port1=",    "type": "path"},
    "sata2p":         {"flag": "--sata-port2=",    "type": "path"},
    "sata3":          {"flag": "--sata-port3=",    "type": "path"},
    "ahci2":          {"flag": "--ahci2",          "type": "bool"},
    "sata2":          {"flag": "--sata2=",         "type": "path"},
    "sata2_port1":    {"flag": "--sata2-port1=",   "type": "path"},
    "sata_writethrough": {"flag": "--sata-writethrough", "type": "bool"},
    "virtio_blk":     {"flag": "--virtio-blk=",     "type": "path"},   # VIRTIO V2
    # video
    "cdrom":          {"flag": "--cdrom=",         "type": "path"},   # STORE S6
    "vbe":            {"flag": "--vbe=",           "type": "str", "regex": r"^(\d{1,5}x\d{1,5}x(4|8|15|16|24|32)|off)$"},
    "no_fb":          {"flag": "--no-fb",          "type": "bool"},
    # network
    "tap":            {"flag": "--tap=",           "type": "str", "regex": r"^[A-Za-z0-9_.\-]{1,64}:\d{1,5}$"},
    "virtio_net":     {"flag": "--virtio-net=",    "type": "str", "regex": r"^[A-Za-z0-9_.\-]{1,64}:\d{1,5}$"},   # VIRTIO V4
    # run
    "max_instr":      {"flag": "--max-instr=",     "type": "int", "min": 0, "max": 10 ** 15},
    "stats":          {"flag": "--stats",          "type": "bool"},
    # outputs (paths the launcher fills with per-run artifact locations)
    "log":            {"flag": "--log=",           "type": "str", "managed": "log.txt"},
    "dump_fb":        {"flag": "--dump-fb=",       "type": "str", "managed": "framebuffer.ppm"},
    "dump_text":      {"flag": "--dump-text=",     "type": "str", "managed": "text-screen.txt"},
    "trace_file":     {"flag": "--trace-file=",    "type": "str", "managed": "trace.txt"},
    "trace_from":     {"flag": "--trace-from=",    "type": "int", "min": 0, "max": 10 ** 15},
    "trace_count":    {"flag": "--trace-count=",   "type": "int", "min": 0, "max": 10 ** 15},
    # debug
    "gdb":            {"flag": "--gdb=",           "type": "str", "regex": r"^([A-Za-z0-9_.\-]{1,64}:)?\d{1,5}$"},
    "watch_phys":     {"flag": "--watch-phys=",    "type": "str", "regex": r"^(0x[0-9a-fA-F]+|\d+)$"},
    "symbols":        {"flag": "--symbols=",       "type": "path"},
    "debug_script":   {"flag": "--debug-script=",  "type": "path"},
    "keys":           {"flag": "--keys=",          "type": "str", "regex": r"^[0-9a-fA-F]{1,4}(,[0-9a-fA-F]{1,4})*$"},
    "keys_at":        {"flag": "--keys-at=",       "type": "str", "regex": r"^(prompt|0x[0-9a-fA-F]+|\d+)$"},
    "snapshot_save":  {"flag": "--snapshot-save=", "type": "path"},
}

BUILTIN_CONFIG = {
    "platform": "haswell",
    "rom": "firmware/firmware.bin",
    "disk": "disk/disk.img",
    "max_instr": 200000000,
    "stats": True,
    "log": True,
    "dump_fb": True,
    "lpc": "pantherpoint",
}


class ConfigError(ValueError):
    pass


def _resolve_path(value):
    """Accept absolute paths as-is; resolve anything else against the repo
    root so GUI runs behave exactly like `make demo` from the root."""
    if not isinstance(value, str) or not value.strip():
        raise ConfigError("empty path")
    value = value.strip()
    if "\x00" in value:
        raise ConfigError("bad path")
    return value if os.path.isabs(value) else os.path.join(ROOT, value)


def validate_config(cfg, state_dir=None):
    """Return a sanitized {key: value} dict.  Unknown keys are refused;
    every accepted value is coerced/checked against the SCHEMA."""
    if not isinstance(cfg, dict):
        raise ConfigError("config must be an object")
    out = {}
    for key, value in cfg.items():
        if key not in SCHEMA:
            raise ConfigError("unknown option: %s" % key)
        spec = SCHEMA[key]
        if value is None or value is False or value == "":
            continue
        t = spec["type"]
        if t == "bool":
            if value is not True and value not in (1, "1", "true", "on"):
                raise ConfigError("%s: expected boolean" % key)
            out[key] = True
        elif t == "int":
            try:
                n = int(str(value), 0)
            except ValueError:
                raise ConfigError("%s: expected an integer" % key)
            if n < spec["min"] or n > spec["max"]:
                raise ConfigError("%s: out of range" % key)
            out[key] = n
        elif t == "choice":
            if value not in spec["choices"]:
                raise ConfigError("%s: expected one of %s" % (key, ", ".join(spec["choices"])))
            out[key] = value
        elif t == "path":
            p = _resolve_path(value)
            if not os.path.exists(p):
                raise ConfigError("%s: file not found: %s" % (key, value))
            out[key] = p
        elif t == "str":
            s = str(value)
            if "managed" in spec:
                # managed outputs: the toggle is a boolean-ish request for a
                # per-run artifact path the launcher owns
                if value is True or s in ("1", "true", "on", "yes") or s == spec["managed"]:
                    out[key] = spec["managed"]
                else:
                    raise ConfigError("%s: unexpected value %r" % (key, s))
            else:
                if not re.match(spec["regex"], s):
                    raise ConfigError("%s: bad value %r" % (key, s))
                out[key] = s
        else:  # pragma: no cover - schema bug
            raise ConfigError("internal schema error for %s" % key)
    return out


def build_argv(emu_path, cfg, run_dir, monitor_port):
    """Translate a validated config into the emulator argv list.  Managed
    output options point into run_dir so the GUI can download them."""
    cfg = validate_config(cfg)
    argv = [emu_path, "--monitor=127.0.0.1:%d" % monitor_port]
    for key, value in cfg.items():
        spec = SCHEMA[key]
        if "managed" in spec:
            argv.append(spec["flag"] + os.path.join(run_dir, value))
        elif spec["type"] == "bool":
            argv.append(spec["flag"])
        else:
            argv.append(spec["flag"] + str(value))
    return argv


# ---------------------------------------------------------------------------
# Process / run state
# ---------------------------------------------------------------------------

def _terminate(proc, timeout=5):
    """POSIX: signal the whole process group (the emulator may have children).
    Windows: no killpg/getpgid -- fall back to terminate/kill."""
    try:
        os.killpg(os.getpgid(proc.pid), signal.SIGTERM)
    except (AttributeError, OSError, ProcessLookupError, PermissionError):
        try:
            proc.terminate()
        except OSError:
            pass
    try:
        proc.wait(timeout=timeout)
        return proc.returncode
    except subprocess.TimeoutExpired:
        try:
            os.killpg(os.getpgid(proc.pid), signal.SIGKILL)
        except (AttributeError, OSError, ProcessLookupError, PermissionError):
            try:
                proc.kill()
            except OSError:
                pass
        try:
            proc.wait(timeout=timeout)
        except subprocess.TimeoutExpired:
            pass
        return proc.returncode


class Machine:
    def __init__(self, emu_path, state_dir):
        self.emu_path = emu_path
        self.state_dir = state_dir
        self.runs_dir = os.path.join(state_dir, "runs")
        self.snap_dir = os.path.join(state_dir, "snaps")
        for d in (state_dir, self.runs_dir, self.snap_dir):
            os.makedirs(d, exist_ok=True)
        self.lock = threading.RLock()
        self.proc = None
        self.run_dir = None
        self.monitor_port = None
        self.monitor_token = None     # per-run capability for --monitor
        self.config = dict(BUILTIN_CONFIG)
        self.started_at = None
        self.exit_code = None
        self.argv = None
        self.responder = None

    # -- lifecycle ---------------------------------------------------------
    def status(self):
        with self.lock:
            running = self.proc is not None and self.proc.poll() is None
            return {
                "state": "running" if running else ("exited" if self.exit_code is not None else "off"),
                "exit_code": self.exit_code,
                "pid": self.proc.pid if self.proc and self.proc.poll() is None else None,
                "started_at": self.started_at,
                "run_dir": self.run_dir,
                "snap_dir": self.snap_dir,
                "monitor_port": self.monitor_port,
                "emu": self.emu_path,
                "config": self.config,
                "argv": self.argv,
                "responder": self.responder_status(),
            }

    def start(self, cfg):
        with self.lock:
            if self.proc is not None and self.proc.poll() is None:
                raise ConfigError("machine is already running")
            config = validate_config(cfg)
            ts = time.strftime("%Y%m%d-%H%M%S")
            run_dir = os.path.join(self.runs_dir, ts)
            os.makedirs(run_dir, exist_ok=True)
            monitor_port = free_port()
            argv = build_argv(self.emu_path, config, run_dir, monitor_port)
            # UPKEEP U7: a fresh monitor capability per run, carried in the
            # child's environment -- never in argv, so it appears neither in
            # process lists nor in the run's argv.json. Only the owner of
            # this process (the launcher / desktop GUI) can present it.
            monitor_token = new_token()
            env = dict(os.environ)
            env["X86EMU_MONITOR_TOKEN"] = monitor_token
            out = open(os.path.join(run_dir, "stdout.log"), "wb")
            err = open(os.path.join(run_dir, "stderr.log"), "wb")
            self.proc = subprocess.Popen(
                argv, cwd=ROOT, stdout=out, stderr=err, env=env,
                stdin=subprocess.DEVNULL, start_new_session=True)
            out.close(); err.close()
            self.run_dir = run_dir
            self.monitor_port = monitor_port
            self.monitor_token = monitor_token
            self.config = config
            self.argv = argv
            self.started_at = time.time()
            self.exit_code = None
            threading.Thread(target=self._watch, daemon=True).start()
            self._write_meta(run_dir, argv)
            return {"ok": True, "monitor_port": monitor_port, "run_dir": run_dir}

    def _write_meta(self, run_dir, argv):
        with open(os.path.join(run_dir, "argv.json"), "w", encoding="utf-8") as f:
            json.dump({"argv": argv, "time": time.strftime("%Y-%m-%d %H:%M:%S")}, f, indent=1)

    def _watch(self):
        proc = self.proc
        if proc is None:
            return
        rc = proc.wait()
        with self.lock:
            if self.proc is proc:
                self.exit_code = rc

    def stop(self):
        with self.lock:
            if self.proc is None or self.proc.poll() is not None:
                return {"ok": True, "exit_code": self.exit_code, "already": True}
            proc = self.proc
        rc = _terminate(proc)
        with self.lock:
            self.exit_code = rc
        return {"ok": True, "exit_code": rc}

    # -- output / artifacts -------------------------------------------------
    def output(self, which, tail=200):
        with self.lock:
            run_dir = self.run_dir
        if not run_dir:
            return ""
        name = "stderr.log" if which == "stderr" else "stdout.log"
        path = os.path.join(run_dir, name)
        try:
            with open(path, "r", encoding="utf-8", errors="replace") as f:
                lines = f.readlines()
            return "".join(lines[-int(tail):])
        except OSError:
            return ""

    def artifacts(self):
        with self.lock:
            run_dir = self.run_dir
        if not run_dir:
            return []
        out = []
        for name in sorted(os.listdir(run_dir)):
            p = os.path.join(run_dir, name)
            if os.path.isfile(p):
                out.append({"name": name, "size": os.path.getsize(p),
                            "mtime": int(os.path.getmtime(p))})
        return out

    def artifact_path(self, name):
        name = os.path.basename(unquote(name))
        with self.lock:
            run_dir = self.run_dir
        if not run_dir or not name:
            return None
        path = os.path.join(run_dir, name)
        return path if os.path.isfile(path) else None

    # -- snapshots ----------------------------------------------------------
    def snapshots(self):
        out = []
        for name in sorted(os.listdir(self.snap_dir)):
            p = os.path.join(self.snap_dir, name)
            if os.path.isfile(p):
                out.append({"name": name, "size": os.path.getsize(p),
                            "mtime": int(os.path.getmtime(p))})
        return out

    def snapshot_delete(self, name):
        name = os.path.basename(unquote(str(name)))
        path = os.path.join(self.snap_dir, name)
        if name and os.path.isfile(path):
            os.remove(path)
            return {"ok": True}
        return {"ok": False, "error": "no such snapshot"}

    # -- TAP responder -------------------------------------------------------
    def responder_start(self, opts):
        with self.lock:
            if self.responder is not None and self.responder.poll() is None:
                raise ConfigError("responder is already running")
            port = int(opts.get("port", 8080))
            if not (1 <= port <= 65535):
                raise ConfigError("responder port out of range")
            argv = [sys.executable, RESPONDER, "--host", "127.0.0.1", "--port", str(port)]
            drop = opts.get("drop") or ""
            if drop:
                for item in str(drop).split(","):
                    if item.strip() not in ("arp", "icmp", "dhcp", "dns", "tcp", ""):
                        raise ConfigError("bad --drop item: %s" % item)
                argv += ["--drop", ",".join(x.strip() for x in drop.split(",") if x.strip())]
            if opts.get("no_tcp"):
                argv.append("--no-tcp")
            argv += ["--timeout", "0"]
            log = open(os.path.join(self.runs_dir, "responder.log"), "ab")
            self.responder = subprocess.Popen(
                argv, cwd=ROOT, stdout=log, stderr=log,
                stdin=subprocess.DEVNULL, start_new_session=True)
            log.close()
            return {"ok": True, "pid": self.responder.pid}

    def responder_stop(self):
        with self.lock:
            r, self.responder = self.responder, None
        if r is not None and r.poll() is None:
            _terminate(r, timeout=3)
        return {"ok": True}

    def responder_status(self):
        with self.lock:
            r = self.responder
        return {"running": r is not None and r.poll() is None,
                "pid": r.pid if r and r.poll() is None else None}


def free_port():
    s = socket.socket()
    s.bind(("127.0.0.1", 0))
    port = s.getsockname()[1]
    s.close()
    return port


# ---------------------------------------------------------------------------
# Access control (UPKEEP U7)
# ---------------------------------------------------------------------------

ACCESS_TOKEN_FILE = "access-token"
TOKEN_COOKIE = "x86emu_token"


def new_token():
    """A fresh URL-safe capability (256 bits of entropy)."""
    return secrets.token_urlsafe(32)


def is_loopback_bind(bind):
    """True when `bind` keeps the launcher on this machine only."""
    b = (bind or "").strip().lower()
    if b in ("localhost",):
        return True
    try:
        return ipaddress.ip_address(b).is_loopback
    except ValueError:
        return False            # a name we cannot prove local: treat remote


def load_or_create_access_token(state_dir, force_new=False):
    """The persistent remote-access credential: `<state_dir>/access-token`,
    created 0600 on first use.  -> (token, path, created).

    This is the secret the desktop GUI displays and rotates (same file),
    so obtaining or replacing it never needs a terminal.  A running
    launcher keeps its in-memory copy, so rotation takes effect at the
    next launcher start.  A token someone can read from this file is a
    token that same user could read from the runs/ state anyway -- the
    file's owner-only mode is the honest boundary, not a cipher."""
    path = os.path.join(state_dir, ACCESS_TOKEN_FILE)
    if not force_new:
        try:
            with open(path, "r", encoding="utf-8") as f:
                tok = f.read().strip()
            if len(tok) >= 16 and re.match(r"^[A-Za-z0-9_\-]+$", tok):
                return tok, path, False
        except OSError:
            pass
    tok = new_token()
    fd = os.open(path, os.O_WRONLY | os.O_CREAT | os.O_TRUNC, 0o600)
    try:
        os.write(fd, (tok + "\n").encode("ascii"))
    finally:
        os.close(fd)
    try:                                # a pre-existing loose file keeps its
        os.chmod(path, 0o600)           # mode through O_TRUNC; tighten it
    except OSError:
        pass
    return tok, path, True


def _host_part(value):
    """A Host header/authority reduced to its hostname: lowercased,
    brackets and any :port stripped."""
    v = (value or "").strip().lower()
    if v.startswith("["):
        end = v.find("]")
        return v[1:end] if end > 0 else ""
    return v.rsplit(":", 1)[0] if v.count(":") == 1 else v


class Auth:
    """One launcher's gate: the token, whether the page may embed it
    (loopback only), and the Host allowlist.  Built by `make_auth`."""

    def __init__(self, token, embed, allowed_hosts, any_literal):
        self.token = token
        self.embed = embed                  # loopback: page carries it
        self.allowed_hosts = allowed_hosts  # set of lowercase host parts
        self.any_literal = any_literal      # 0.0.0.0 bind: any IP literal

    def host_ok(self, host_header):
        h = _host_part(host_header)
        if not h:
            return False
        if h in self.allowed_hosts:
            return True
        if self.any_literal:
            try:
                ipaddress.ip_address(h)
                return True
            except ValueError:
                return False
        return False

    def token_ok(self, presented):
        return bool(presented) and hmac.compare_digest(
            str(presented), self.token)

    def origin_ok(self, origin_header, host_header):
        """Same-origin check for browser-driven mutations and the upgrade:
        browsers always send Origin on POST and WebSocket handshakes and
        cannot forge it; our page is same-origin by construction."""
        if not origin_header:
            return True                     # a non-browser client
        u = urlparse(origin_header)
        if u.scheme not in ("http", "https") or not u.hostname:
            return False                    # includes Origin: null
        return (u.hostname.lower() == _host_part(host_header)
                and self.host_ok(u.netloc))


def make_auth(bind, state_dir):
    """Build the gate for one listener.

    Loopback:  per-process token, embedded in the page -- the browser
    carries it, no step for the operator.  Non-loopback: the persistent
    access-token file, embedded in NO page; the visitor presents it once
    (form / ?token=) and the resulting cookie carries it from there."""
    hosts = {"127.0.0.1", "localhost", "::1"}
    any_literal = False
    b = (bind or "").strip().lower()
    try:
        ip = ipaddress.ip_address(b)
        if ip.is_unspecified:
            any_literal = True
        else:
            hosts.add(str(ip))
    except ValueError:
        if b and b != "localhost":
            hosts.add(b)
    if not is_loopback_bind(bind):
        hn = socket.gethostname().lower()
        if hn:
            hosts.add(hn)
            hosts.add(hn.split(".")[0])
        token, _path, _created = load_or_create_access_token(state_dir)
        return Auth(token, False, hosts, any_literal)
    return Auth(new_token(), True, hosts, any_literal)


_LOGIN_PAGE = """<!doctype html>
<html lang="ru"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>AuraLite Emulator — token access</title>
<style>body{background:#0b1020;color:#e7efff;font:15px/1.5 ui-monospace,Consolas,monospace;
display:flex;min-height:100vh;align-items:center;justify-content:center;margin:0}
.box{background:#121a2d;border:1px solid #263451;border-radius:10px;padding:26px 30px;max-width:520px}
h1{color:#58d6ff;font-size:19px;margin:0 0 10px}input{width:100%%;box-sizing:border-box;
padding:9px;border:1px solid #263451;border-radius:6px;background:#0b1324;color:#e7efff;font:inherit}
button{margin-top:12px;padding:9px 16px;border:1px solid #34758a;border-radius:7px;
background:#15557a;color:#d9f3ff;font:inherit;cursor:pointer}
.err{color:#ff8585}.dim{color:#8fa2c2}</style></head><body>
<div class="box"><h1>AuraLite Emulator — remote access</h1>
<p>The interface is served on a network address and requires an access token.</p>
<p class="dim">The token is shown in the AuraLite Emulator desktop app
(Expert mode → "Network" tab) and stored in the GUI state directory,
in the <code>access-token</code> file. It can also be replaced there.</p>
%(error)s
<form method="get" action="/">
<label>Access token<br><input name="token" type="password" autocomplete="off" autofocus></label>
<br><button type="submit">Sign in</button></form>
<p class="dim">Warning: the connection is not encrypted (HTTP). Do not use
this access on untrusted networks.</p>
</div></body></html>
"""


# ---------------------------------------------------------------------------
# Presets
# ---------------------------------------------------------------------------

class Presets:
    def __init__(self, state_dir):
        self.path = os.path.join(state_dir, "presets.json")
        self.lock = threading.RLock()   # save() calls load() under the lock

    def load(self):
        with self.lock:
            try:
                with open(self.path, "r", encoding="utf-8") as f:
                    data = json.load(f)
                return data if isinstance(data, dict) else {}
            except (OSError, ValueError):
                return {}

    def save(self, name, config):
        name = str(name).strip()
        if not re.match(r"^[\w .\-]{1,64}$", name):
            raise ConfigError("bad preset name")
        validate_config(config)  # presets store only schema-clean configs
        with self.lock:
            data = self.load()
            data[name] = config
            tmp = self.path + ".tmp"
            with open(tmp, "w", encoding="utf-8") as f:
                json.dump(data, f, indent=1)
            os.replace(tmp, self.path)
        return {"ok": True}

    def delete(self, name):
        with self.lock:
            data = self.load()
            data.pop(name, None)
            with open(self.path, "w", encoding="utf-8") as f:
                json.dump(data, f, indent=1)
        return {"ok": True}


# ---------------------------------------------------------------------------
# WebSocket proxy: after the RFC 6455 handshake the connection is a raw
# byte pipe to the emulator's monitor socket.  Frames stay untouched
# (client frames remain masked, server frames unmasked), so no re-framing
# is needed and the monitor protocol works verbatim.
# ---------------------------------------------------------------------------

WS_GUID = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"


def _ws_handshake_key():
    import base64, os as _os
    return base64.b64encode(_os.urandom(16)).decode("ascii")


def ws_proxy(client_sock, client_request_bytes, host, port, on_done=None):
    backend = None
    try:
        try:
            backend = socket.create_connection((host, port), timeout=10)
        except OSError:
            try:
                client_sock.sendall(b"HTTP/1.1 502 Bad Gateway\r\nContent-Length: 0\r\n\r\n")
            except OSError:
                pass
            return
        backend.sendall(client_request_bytes)
        backend.settimeout(10)
        # read the backend's 101 response headers
        buf = b""
        while b"\r\n\r\n" not in buf:
            chunk = backend.recv(4096)
            if not chunk:
                raise OSError("backend closed during handshake")
            buf += chunk
        head, rest = buf.split(b"\r\n\r\n", 1)
        if b" 101 " not in head.split(b"\r\n", 1)[0]:
            client_sock.sendall(head + b"\r\n\r\n" + rest)
            return
        client_sock.sendall(head + b"\r\n\r\n")
        if rest:
            client_sock.sendall(rest)
        backend.settimeout(None)
        client_sock.settimeout(None)

        def pump(src, dst):
            try:
                while True:
                    data = src.recv(65536)
                    if not data:
                        return
                    dst.sendall(data)
            except OSError:
                return

        # Two independent directions: a client-side send failure (browser
        # tab closed mid-keyframe) must not drop already-received client
        # data such as a pending `quit` -- measured, the single-loop splice
        # lost exactly that frame.
        t1 = threading.Thread(target=pump, args=(client_sock, backend), daemon=True)
        t2 = threading.Thread(target=pump, args=(backend, client_sock), daemon=True)
        t1.start(); t2.start()
        t1.join(); t2.join()
    except OSError:
        pass
    finally:
        if backend is not None:
            try:
                backend.close()
            except OSError:
                pass
        try:
            client_sock.close()
        except OSError:
            pass
        if on_done:
            on_done()


# ---------------------------------------------------------------------------
# HTTP API
# ---------------------------------------------------------------------------

class Handler(BaseHTTPRequestHandler):
    server_version = "x86emu-launcher/1"
    machine = None      # injected
    presets = None
    auth = None         # injected (UPKEEP U7): the Auth gate

    # -- UPKEEP U7 access gate ------------------------------------------------
    def _presented_token(self):
        h = self.headers.get("X-X86emu-Token")
        if h:
            return h.strip()
        a = self.headers.get("Authorization") or ""
        if a.lower().startswith("bearer "):
            return a[7:].strip()
        q = parse_qs(urlparse(self.path).query)
        if q.get("token"):
            return q["token"][0]
        for part in (self.headers.get("Cookie") or "").split(";"):
            if "=" in part:
                k, v = part.strip().split("=", 1)
                if k == TOKEN_COOKIE:
                    return v.strip().strip('"')
        return ""

    def _refuse(self, code, error):
        self._json({"ok": False, "error": error}, code)
        return False

    def _gate(self, need_token=True, mutation=False):
        """Authorize before any route body runs. Host first (rebinding),
        then the capability, then Origin for state-changing routes."""
        a = self.auth
        if a is None:                       # miswired server: fail closed
            return self._refuse(403, "access gate not configured")
        if not a.host_ok(self.headers.get("Host")):
            return self._refuse(403, "forbidden host")
        if need_token and not a.token_ok(self._presented_token()):
            return self._refuse(401, "unauthorized: token required")
        if mutation and not a.origin_ok(self.headers.get("Origin"),
                                         self.headers.get("Host")):
            return self._refuse(403, "forbidden origin")
        return True

    # -- helpers -----------------------------------------------------------
    def _json(self, obj, code=200):
        body = json.dumps(obj).encode("utf-8")
        self.send_response(code)
        self.send_header("Content-Type", "application/json; charset=utf-8")
        self.send_header("Cache-Control", "no-store")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def _file(self, path, download=None):
        try:
            with open(path, "rb") as f:
                data = f.read()
        except OSError:
            self._json({"ok": False, "error": "file not found"}, 404)
            return
        self.send_response(200)
        self.send_header("Content-Type", "application/octet-stream")
        if download:
            self.send_header("Content-Disposition",
                             "attachment; filename=\"%s\"" % os.path.basename(download))
        self.send_header("Content-Length", str(len(data)))
        self.end_headers()
        self.wfile.write(data)

    def _body(self):
        length = int(self.headers.get("Content-Length") or 0)
        if length > 1 << 20:
            raise ConfigError("body too large")
        raw = self.rfile.read(length) if length else b"{}"
        try:
            return json.loads(raw.decode("utf-8") or "{}")
        except ValueError:
            raise ConfigError("invalid JSON body")

    def log_message(self, fmt, *args):  # quiet
        pass

    # -- GET ---------------------------------------------------------------
    def _serve_gui_page(self, authorized):
        """The one public route. Authorized (loopback embed, or a valid
        presented token): the real page with the capability injected for
        its JS plus a SameSite=Strict cookie so navigations (artifact
        downloads) and the upgrade carry it too. Otherwise: the login
        form. A cross-origin page can never read either body (SOP), and
        the Host gate has already refused rebinding names."""
        if not authorized:
            error = ""
            if any(k == "token" for k in parse_qs(urlparse(self.path).query)):
                error = '<p class="err">Wrong token. Please try again.</p>'
            body = (_LOGIN_PAGE % {"error": error}).encode("utf-8")
            self.send_response(200)
            self.send_header("Content-Type", "text/html; charset=utf-8")
            self.send_header("Cache-Control", "no-store")
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)
            return
        with open(GUI_PAGE, "rb") as f:
            page = f.read()
        inject = ("<script>window.X86EMU_TOKEN=\"%s\";</script>"
                  % _html.escape(self.auth.token, quote=True)).encode("utf-8")
        if b"</head>" in page:
            body = page.replace(b"</head>", inject + b"</head>", 1)
        else:
            body = inject + page
        self.send_response(200)
        self.send_header("Content-Type", "text/html; charset=utf-8")
        self.send_header("Cache-Control", "no-store")
        self.send_header("Set-Cookie",
                         "%s=%s; HttpOnly; SameSite=Strict; Path=/"
                         % (TOKEN_COOKIE, self.auth.token))
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def do_GET(self):
        u = urlparse(self.path)
        path = u.path
        try:
            if path == "/ws":
                return self._upgrade_ws()
            if path in ("/", "/index.html"):
                if self.auth is not None and not self.auth.host_ok(
                        self.headers.get("Host")):
                    return self._refuse(403, "forbidden host")
                ok = (self.auth is not None
                      and (self.auth.embed
                           or self.auth.token_ok(self._presented_token())))
                return self._serve_gui_page(ok)
            if not self._gate(need_token=True):
                return
            if path == "/api/status":
                return self._json({"ok": True, **self.machine.status()})
            if path == "/api/meta":
                return self._json({"ok": True, "platforms": PLATFORMS, "lpcs": LPCS,
                                   "schema": sorted(SCHEMA.keys()),
                                   "builtin": BUILTIN_CONFIG})
            if path == "/api/output":
                q = parse_qs(u.query)
                which = (q.get("which") or ["stdout"])[0]
                tail = (q.get("tail") or ["200"])[0]
                return self._json({"ok": True, "which": which,
                                   "output": self.machine.output(which, tail)})
            if path == "/api/artifacts":
                return self._json({"ok": True, "entries": self.machine.artifacts()})
            if path.startswith("/api/artifact/"):
                name = path[len("/api/artifact/"):]
                p = self.machine.artifact_path(name)
                if not p:
                    return self._json({"ok": False, "error": "no such artifact"}, 404)
                return self._file(p, download=name)
            if path == "/api/snapshots":
                return self._json({"ok": True, "entries": self.machine.snapshots()})
            if path == "/api/presets":
                return self._json({"ok": True, "presets": self.presets.load()})
            if path == "/api/files":
                q = parse_qs(u.query)
                raw = (q.get("path") or [ROOT])[0]
                exts = [e.lower() for e in (q.get("ext") or [""])[0].split(",") if e]
                d = raw if os.path.isabs(raw) else os.path.join(ROOT, raw)
                d = os.path.abspath(d)
                if not os.path.isdir(d):
                    return self._json({"ok": False, "error": "not a directory: %s" % d}, 404)
                entries = []
                try:
                    for name in sorted(os.listdir(d)):
                        p = os.path.join(d, name)
                        if os.path.isdir(name) or True:
                            pass
                        is_dir = os.path.isdir(p)
                        if not is_dir and exts and not any(name.lower().endswith(e) for e in exts):
                            continue
                        entries.append({"name": name, "dir": is_dir,
                                        "size": 0 if is_dir else os.path.getsize(p)})
                except OSError as e:
                    return self._json({"ok": False, "error": str(e)}, 403)
                return self._json({"ok": True, "path": d, "entries": entries[:500],
                                   "parent": os.path.dirname(d)})
            return self._json({"ok": False, "error": "not found"}, 404)
        except ConfigError as e:
            return self._json({"ok": False, "error": str(e)}, 400)

    # -- POST / DELETE -------------------------------------------------------
    def do_POST(self):
        u = urlparse(self.path)
        if not self._gate(need_token=True, mutation=True):
            return
        try:
            body = self._body()
            if u.path == "/api/start":
                cfg = body.get("config", body)
                res = self.machine.start(cfg)
                return self._json({"ok": True, **res})
            if u.path == "/api/stop":
                return self._json(self.machine.stop())
            if u.path == "/api/restart":
                self.machine.stop()
                res = self.machine.start(body.get("config") or self.machine.config)
                return self._json({"ok": True, **res})
            if u.path == "/api/presets":
                return self._json(self.presets.save(body.get("name"), body.get("config")))
            if u.path == "/api/responder":
                if body.get("on"):
                    return self._json(self.machine.responder_start(body))
                return self._json(self.machine.responder_stop())
            return self._json({"ok": False, "error": "not found"}, 404)
        except ConfigError as e:
            return self._json({"ok": False, "error": str(e)}, 400)
        except Exception as e:                       # pragma: no cover
            return self._json({"ok": False, "error": str(e)}, 500)

    def do_DELETE(self):
        if not self._gate(need_token=True, mutation=True):
            return
        u = urlparse(self.path)
        q = parse_qs(u.query)
        if u.path == "/api/presets":
            return self._json(self.presets.delete((q.get("name") or [""])[0]))
        if u.path == "/api/snapshots":
            return self._json(self.machine.snapshot_delete((q.get("name") or [""])[0]))
        return self._json({"ok": False, "error": "not found"}, 404)

    # -- WebSocket upgrade ----------------------------------------------------
    def _upgrade_ws(self):
        # The splice is a state-changing channel (run control rides it),
        # so it takes the mutation gate: Host + token + same-Origin.
        if not self._gate(need_token=True, mutation=True):
            return
        if (self.headers.get("Upgrade") or "").lower() != "websocket":
            return self._json({"ok": False, "error": "websocket upgrade required"}, 426)
        status = self.machine.status()
        port = status.get("monitor_port")
        if status["state"] != "running" or not port:
            return self._json({"ok": False, "error": "no running machine"}, 502)
        key = self.headers.get("Sec-WebSocket-Key")
        if not key:
            return self._json({"ok": False, "error": "missing Sec-WebSocket-Key"}, 400)
        # rebuild the request for the backend monitor, presenting the
        # per-run monitor capability this process alone knows (UPKEEP U7)
        lines = ["GET /ws?token=%s HTTP/1.1" % (self.machine.monitor_token or ""),
                 "Host: 127.0.0.1:%d" % port,
                 "Upgrade: websocket",
                 "Connection: Upgrade",
                 "Sec-WebSocket-Key: %s" % key,
                 "Sec-WebSocket-Version: 13"]
        if self.headers.get("Sec-WebSocket-Protocol"):
            lines.append("Sec-WebSocket-Protocol: %s" % self.headers["Sec-WebSocket-Protocol"])
        req = ("\r\n".join(lines) + "\r\n\r\n").encode("ascii")
        sock = self.connection
        self.server.hijack(self.request)   # the proxy thread owns the socket now
        threading.Thread(target=ws_proxy,
                         args=(sock, req, "127.0.0.1", port,
                               lambda: self.server.unhijack(self.request)),
                         daemon=True).start()
        self.close_connection = True


# ---------------------------------------------------------------------------
# main
# ---------------------------------------------------------------------------

def make_server(bind, port, machine, presets, auth=None):
    if auth is None:
        auth = make_auth(bind, machine.state_dir)
    handler = type("BoundHandler", (Handler,),
                   {"machine": machine, "presets": presets, "auth": auth})

    class ProxyHTTPServer(ThreadingHTTPServer):
        daemon_threads = True

        def __init__(self, *a, **kw):
            super().__init__(*a, **kw)
            self._hijacked = {}          # id(socket) -> socket owned by ws_proxy

        def hijack(self, sock):
            self._hijacked[id(sock)] = sock

        def unhijack(self, sock):
            self._hijacked.pop(id(sock), None)

        def shutdown_request(self, request):
            if id(request) in self._hijacked:
                return                  # the ws_proxy thread owns this socket
            super().shutdown_request(request)

        def close_request(self, request):
            if id(request) in self._hijacked:
                return
            super().close_request(request)

    return ProxyHTTPServer((bind, port), handler)


def main(argv=None):
    ap = argparse.ArgumentParser(description="AuraLite-Emulator GUI launcher")
    ap.add_argument("--bind", default="127.0.0.1",
                    help="bind address (0.0.0.0 exposes the GUI on the network)")
    ap.add_argument("--port", type=int, default=8080)
    ap.add_argument("--state-dir", default=os.path.expanduser("~/.x86emu-gui"))
    ap.add_argument("--emu", default=DEFAULT_EMU, help="path to the x86emu binary")
    a = ap.parse_args(argv)

    if not os.path.isfile(a.emu) and os.path.isfile(a.emu + ".exe"):
        a.emu += ".exe"                     # Windows build.bat output
    if not os.path.isfile(a.emu):
        print("launcher: %s not found -- run `make` first" % a.emu, file=sys.stderr)
        return 2
    machine = Machine(os.path.abspath(a.emu), os.path.abspath(a.state_dir))
    presets = Presets(machine.state_dir)
    auth = make_auth(a.bind, machine.state_dir)
    srv = make_server(a.bind, a.port, machine, presets, auth)
    print("x86emu GUI launcher: http://%s:%d/  (state: %s)" %
          (a.bind, a.port, machine.state_dir), flush=True)
    if is_loopback_bind(a.bind):
        print("access: token embedded in the served page (loopback only); "
              "the API and /ws require it", flush=True)
    else:
        # Plaintext warning is a product requirement, not a footnote: the
        # token below is the *only* credential and the wire is bare HTTP.
        print("WARNING: bound beyond loopback over plaintext HTTP -- the "
              "token below is the only credential and crosses the network "
              "in the clear.", flush=True)
        print("access token URL: http://%s:%d/?token=%s"
              % (a.bind, a.port, auth.token), flush=True)
        print("(the token lives in %s -- the desktop GUI shows and rotates "
              "it)" % os.path.join(machine.state_dir, ACCESS_TOKEN_FILE),
              flush=True)
    try:
        srv.serve_forever()
    except KeyboardInterrupt:
        machine.stop()
        machine.responder_stop()
    return 0


if __name__ == "__main__":
    sys.exit(main())
