#!/usr/bin/env python3
"""AuraLite Emulator -- native desktop GUI (GUI G2).

A real desktop application window (tkinter, Python standard library only --
no browser, no third-party packages) that drives the emulator exactly like
the CLI would.  The same option SCHEMA as tools/launcher.py is the single
source of truth: user input can never become an arbitrary flag.

Two user modes, switchable at any time and persisted:

  * Обычный  -- VirtualBox-style: a machine list, a big screen, and a
                three-step "new machine" wizard ending in one-click boot,
                including plain ISO boot ("select ISO -> Start");
  * Эксперт  -- the full option surface (every SCHEMA field), the monitor
                console (exec), artifacts, snapshots, TAP responder.

Ground rules (same as the launcher):
  * the emulator is spawned exactly like the CLI (argv list, no shell),
    cwd = repo root, monitor on 127.0.0.1:<free port>;
  * the GUI speaks the monitor's WebSocket protocol directly over a raw
    TCP socket (RFC 6455 client, stdlib only);
  * state (presets, UI state, runs, snapshots) lives in --state-dir
    (default ~/.x86emu-gui), never in the repo.

ISO boot (the quick route, docs/WINDOWS.md): a stock SeaBIOS `bios.bin`
plus the ISO on a SATA port boots any MBR/isohybrid image.  The app
searches known locations for bios.bin and remembers a user-picked copy.
"""
import argparse
import base64
import json
import os
import shutil
import socket
import struct
import sys
import time
import tkinter as tk
from tkinter import filedialog, messagebox, ttk

TOOLS_DIR = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(TOOLS_DIR)
sys.path.insert(0, TOOLS_DIR)

import launcher  # noqa: E402
from launcher import (BUILTIN_CONFIG, ConfigError, Machine, Presets,  # noqa: E402
                      SCHEMA, validate_config)

APP_TITLE = "AuraLite Emulator"
ISO_MAX_INSTR = 10 ** 9          # docs/WINDOWS.md E uses 1e9 for BIOS boots
MONITOR_POLL_MS = 80
STATUS_POLL_MS = 300
RENDER_MIN_MS = 90               # cap the screen repaint rate


# ---------------------------------------------------------------------------
# UI state persistence (tolerant reader: any corruption falls back to defaults)
# ---------------------------------------------------------------------------

DEFAULT_UI = {
    "mode": "normal",            # "normal" | "expert"
    "autostart": True,
    "last_preset": "",
    "bios": "",
    "geometry": "1180x740",
}


def load_ui_state(state_dir):
    out = dict(DEFAULT_UI)
    try:
        with open(os.path.join(state_dir, "gui_state.json"), "r", encoding="utf-8") as f:
            data = json.load(f)
        if isinstance(data, dict):
            for k in DEFAULT_UI:
                if k in data and isinstance(data[k], (str, bool)):
                    out[k] = data[k]
    except (OSError, ValueError):
        pass
    return out


def save_ui_state(state_dir, ui):
    keep = {k: ui.get(k, DEFAULT_UI[k]) for k in DEFAULT_UI}
    try:
        tmp = os.path.join(state_dir, "gui_state.json.tmp")
        with open(tmp, "w", encoding="utf-8") as f:
            json.dump(keep, f, indent=1)
        os.replace(tmp, os.path.join(state_dir, "gui_state.json"))
    except OSError:
        pass


# ---------------------------------------------------------------------------
# SeaBIOS discovery + normal-mode machine shapes
# ---------------------------------------------------------------------------

def find_seabios(state_dir, explicit=None):
    """Locate a stock SeaBIOS rom.  Returns an absolute path or None.

    Search order: explicit argument (wizard pick), AURALITE_BIOS, the state
    dir, then the repo (root / firmware/).
    """
    cands = []
    if explicit:
        cands.append(explicit)
    env = os.environ.get("AURALITE_BIOS")
    if env:
        cands.append(env)
    for d in (state_dir, ROOT, os.path.join(ROOT, "firmware")):
        cands.append(os.path.join(d, "bios.bin"))
        cands.append(os.path.join(d, "seabios.bin"))
    for p in cands:
        if p and os.path.isfile(p):
            return os.path.abspath(p)
    return None


def _stat_flags():
    return {"stats": True, "log": True, "dump_fb": True}


def normal_config(kind, path=None, bios=None, initrd=None):
    """Build a whole machine config for the usual-mode wizard.

    kind: "iso" | "disk" | "kernel" | "empty"
      iso    -- stock SeaBIOS + the image on an ATAPI CD (--cdrom=, El Torito),
                like VirtualBox/VMware; MBR/isohybrid ISOs also boot from the
                documented SATA recipe (docs/WINDOWS.md E) in expert mode;
      disk   -- the built-in sample firmware booting a disk image;
      kernel -- direct ELF64 kernel boot (no firmware);
      empty  -- the built-in sample firmware demo with no media.
    """
    cfg = {"platform": "haswell"}
    if kind == "iso":
        if not path or not os.path.isfile(path):
            raise ConfigError("ISO-образ не найден: %s" % path)
        if not bios or not os.path.isfile(bios):
            raise ConfigError("нужен bios.bin (стоковый SeaBIOS) — укажите файл в мастере")
        cfg.update({"rom": bios, "lpc": "ich9", "cdrom": path,
                    "max_instr": ISO_MAX_INSTR})
    elif kind == "disk":
        if not path or not os.path.isfile(path):
            raise ConfigError("дисковый образ не найден: %s" % path)
        cfg.update({"rom": os.path.join(ROOT, "firmware", "firmware.bin"),
                    "disk": path, "max_instr": 200 * 1000 * 1000})
    elif kind == "kernel":
        if not path or not os.path.isfile(path):
            raise ConfigError("образ ядра не найден: %s" % path)
        cfg.update({"kernel": path, "max_instr": 500 * 1000 * 1000})
        if initrd:
            if not os.path.isfile(initrd):
                raise ConfigError("initrd не найден: %s" % initrd)
            cfg["initrd"] = initrd
    elif kind == "empty":
        cfg.update({"max_instr": 2 * 1000 * 1000})
    else:
        raise ConfigError("неизвестный тип машины: %s" % kind)
    cfg.update(_stat_flags())
    return cfg


# ---------------------------------------------------------------------------
# Monitor protocol client (RFC 6455 over a raw socket, stdlib only)
# ---------------------------------------------------------------------------

class MonitorClient:
    """Pollable WebSocket client for the emulator's monitor socket.

    Events returned by pump():
      ("text", dict)     -- JSON message (kind: state | reply)
      ("fb",   bytes)    -- binary framebuffer delta ("FB\x01" protocol)
      ("closed", None)   -- the connection is gone
    """

    def __init__(self):
        self.sock = None
        self.buf = b""

    def connect(self, port, timeout=5.0):
        s = socket.create_connection(("127.0.0.1", int(port)), timeout=timeout)
        key = base64.b64encode(os.urandom(16)).decode("ascii")
        req = ("GET /ws HTTP/1.1\r\n"
               "Host: 127.0.0.1:%d\r\n"
               "Upgrade: websocket\r\n"
               "Connection: Upgrade\r\n"
               "Sec-WebSocket-Key: %s\r\n"
               "Sec-WebSocket-Version: 13\r\n\r\n" % (port, key))
        s.sendall(req.encode("ascii"))
        buf = b""
        while b"\r\n\r\n" not in buf:
            d = s.recv(4096)
            if not d:
                raise OSError("monitor closed during handshake")
            buf += d
        head, rest = buf.split(b"\r\n\r\n", 1)
        if b" 101 " not in head.split(b"\r\n", 1)[0]:
            raise OSError("monitor refused the websocket upgrade")
        self.buf = rest
        self.sock = s
        self.sock.settimeout(0.02)
        return self

    def close(self):
        if self.sock is not None:
            try:
                self.sock.close()
            except OSError:
                pass
        self.sock = None

    def send(self, obj):
        if self.sock is None:
            raise OSError("monitor is not connected")
        payload = json.dumps(obj).encode("utf-8")
        mask = os.urandom(4)
        n = len(payload)
        hdr = bytes([0x81])
        if n < 126:
            hdr += bytes([0x80 | n])
        elif n < 65536:
            hdr += bytes([0x80 | 126]) + struct.pack(">H", n)
        else:
            hdr += bytes([0x80 | 127]) + struct.pack(">Q", n)
        body = bytes(b ^ mask[i & 3] for i, b in enumerate(payload))
        self.sock.settimeout(5.0)
        try:
            self.sock.sendall(hdr + mask + body)
        finally:
            self.sock.settimeout(0.02)

    def _parse_frame(self):
        if len(self.buf) < 2:
            return None
        b0, b1 = self.buf[0], self.buf[1]
        op = b0 & 0x0F
        masked = b1 & 0x80
        n = b1 & 0x7F
        off = 2
        if n == 126:
            if len(self.buf) < off + 2:
                return None
            n = struct.unpack(">H", self.buf[off:off + 2])[0]
            off += 2
        elif n == 127:
            if len(self.buf) < off + 8:
                return None
            n = struct.unpack(">Q", self.buf[off:off + 8])[0]
            off += 8
        if n > 16 * 1024 * 1024:
            raise OSError("monitor frame is too large")
        mask = b""
        if masked:
            if len(self.buf) < off + 4:
                return None
            mask = self.buf[off:off + 4]
            off += 4
        if len(self.buf) < off + n:
            return None
        payload = self.buf[off:off + n]
        if masked:
            payload = bytes(b ^ mask[i & 3] for i, b in enumerate(payload))
        self.buf = self.buf[off + n:]
        if op == 0x8:
            return ("closed", None)
        if op == 0x9:                       # ping -> pong
            self._send_raw(0xA, payload)
            return None
        if op == 0x1:
            try:
                return ("text", json.loads(payload.decode("utf-8")))
            except ValueError:
                return None
        if op == 0x2:
            return ("fb", payload)
        return None

    def _send_raw(self, op, payload):
        mask = os.urandom(4)
        n = len(payload)
        hdr = bytes([0x80 | op])
        if n < 126:
            hdr += bytes([0x80 | n])
        else:
            hdr += bytes([0x80 | 126]) + struct.pack(">H", n)
        body = bytes(b ^ mask[i & 3] for i, b in enumerate(payload))
        self.sock.sendall(hdr + mask + body)

    def pump(self):
        events = []
        if self.sock is None:
            return events
        try:
            while True:
                d = self.sock.recv(65536)
                if not d:
                    events.append(("closed", None))
                    self.close()
                    break
                self.buf += d
                while True:
                    ev = self._parse_frame()
                    if ev is None:
                        break
                    events.append(ev)
                    if ev[0] == "closed":
                        self.close()
                        return events
        except socket.timeout:
            pass
        except OSError:
            events.append(("closed", None))
            self.close()
        return events


class FrameBuffer:
    """Accumulates the monitor's FB\x01 row deltas into a full RGB image."""

    def __init__(self):
        self.width = 0
        self.height = 0
        self.buf = bytearray()
        self.dirty = set()

    def apply(self, blob):
        if len(blob) < 10 or blob[0:3] != b"FB\x01":
            return
        flags = blob[3]
        w, h, rows = struct.unpack_from("<HHH", blob, 4)
        if not w or not h or w > 4096 or h > 4096:
            return
        if w != self.width or h != self.height:
            self.width, self.height = w, h
            self.buf = bytearray(w * h * 3)
            self.dirty = set(range(h))
            flags |= 1
        if flags & 1:                       # keyframe: the delta redraws all
            self.buf = bytearray(w * h * 3)
            self.dirty = set(range(h))
        off = 10
        for _ in range(rows):
            if off + 2 > len(blob):
                return
            y = struct.unpack_from("<H", blob, off)[0]
            off += 2
            if y >= h or off + w * 3 > len(blob):
                return
            self.buf[y * w * 3:(y + 1) * w * 3] = blob[off:off + w * 3]
            self.dirty.add(y)
            off += w * 3

    def row_colors(self, y):
        w = self.width
        row = self.buf[y * w * 3:(y + 1) * w * 3]
        return "{" + " ".join("#%02x%02x%02x" % (row[i], row[i + 1], row[i + 2])
                              for i in range(0, len(row), 3)) + "}"


# ---------------------------------------------------------------------------
# Application
# ---------------------------------------------------------------------------

class App:
    def __init__(self, root, emu_path, state_dir):
        self.root = root
        self.state_dir = os.path.abspath(state_dir)
        os.makedirs(self.state_dir, exist_ok=True)
        self.machine = Machine(os.path.abspath(emu_path), self.state_dir)
        self.presets = Presets(self.state_dir)
        self.ui = load_ui_state(self.state_dir)
        self.mon = MonitorClient()
        self.fb = FrameBuffer()
        self.photo = None
        self.last_render = 0.0
        self.machine_state = "off"
        self.exit_code = None
        self.last_state_msg = {}
        self.log_lines = []
        self.reply_text = "—"
        self.connect_deadline = 0.0
        self.stop_deadline = 0.0

        root.title(APP_TITLE)
        try:
            root.geometry(self.ui["geometry"])
        except Exception:
            root.geometry("1180x740")
        self._build_style()
        self._build_menu()
        self._build_layout()
        self.set_mode(self.ui["mode"])
        self.refresh_presets()
        self.root.after(STATUS_POLL_MS, self._poll_status)
        self.root.after(MONITOR_POLL_MS, self._poll_monitor)

    # -- construction ------------------------------------------------------
    def _build_style(self):
        style = ttk.Style(self.root)
        try:
            style.theme_use("clam")
        except Exception:
            pass
        style.configure("Big.TButton", padding=(12, 6))
        style.configure("Mode.TRadiobutton", padding=(8, 4))
        style.configure("Status.TLabel", padding=(6, 3))

    def _build_menu(self):
        menubar = tk.Menu(self.root)
        m_file = tk.Menu(menubar, tearoff=0)
        m_file.add_command(label="Новая машина…", command=self.on_new_machine)
        m_file.add_separator()
        m_file.add_command(label="Открыть папку состояния", command=self.open_state_dir)
        m_file.add_separator()
        m_file.add_command(label="Выход", command=self.on_close)
        m_view = tk.Menu(menubar, tearoff=0)
        self.mode_var = tk.StringVar(value=self.ui["mode"])
        m_view.add_radiobutton(label="Обычный режим", variable=self.mode_var,
                               value="normal", command=lambda: self.set_mode("normal"))
        m_view.add_radiobutton(label="Экспертный режим", variable=self.mode_var,
                               value="expert", command=lambda: self.set_mode("expert"))
        m_help = tk.Menu(menubar, tearoff=0)
        m_help.add_command(label="О программе", command=self.on_about)
        menubar.add_cascade(label="Файл", menu=m_file)
        menubar.add_cascade(label="Вид", menu=m_view)
        menubar.add_cascade(label="Справка", menu=m_help)
        self.root.config(menu=menubar)

    def _build_layout(self):
        top = ttk.Frame(self.root, padding=(8, 6))
        top.pack(fill="x")
        ttk.Label(top, text="Режим:").pack(side="left")
        ttk.Radiobutton(top, text="Обычный", value="normal", variable=self.mode_var,
                        style="Mode.TRadiobutton",
                        command=lambda: self.set_mode("normal")).pack(side="left")
        ttk.Radiobutton(top, text="Экспертный", value="expert", variable=self.mode_var,
                        style="Mode.TRadiobutton",
                        command=lambda: self.set_mode("expert")).pack(side="left")
        ttk.Separator(top, orient="vertical").pack(side="left", fill="y", padx=10)
        self.autostart_var = tk.BooleanVar(value=bool(self.ui["autostart"]))
        ttk.Checkbutton(top, text="Автозапуск после старта", variable=self.autostart_var,
                        command=self._save_ui).pack(side="left")
        self.badge_state = tk.Label(top, text="машина выключена", bg="#5b6470", fg="white",
                                    padx=8, pady=2)
        self.badge_state.pack(side="right", padx=4)
        self.badge_mon = tk.Label(top, text="монитор: —", bg="#5b6470", fg="white",
                                  padx=8, pady=2)
        self.badge_mon.pack(side="right", padx=4)

        main = ttk.PanedWindow(self.root, orient="horizontal")
        main.pack(fill="both", expand=True, padx=8, pady=4)

        # -- sidebar: machine (preset) list + controls --------------------
        side = ttk.Frame(main, padding=4)
        main.add(side, weight=0)
        ttk.Label(side, text="Машины").pack(anchor="w")
        self.preset_list = tk.Listbox(side, height=18, exportselection=False, width=28)
        self.preset_list.pack(fill="both", expand=True, pady=4)
        self.preset_list.bind("<<ListboxSelect>>", lambda e: self.on_preset_select())
        btns = ttk.Frame(side)
        btns.pack(fill="x")
        self.btn_start = ttk.Button(btns, text="▶ Пуск", style="Big.TButton",
                                    command=self.on_start)
        self.btn_start.pack(fill="x", pady=2)
        row = ttk.Frame(side)
        row.pack(fill="x")
        self.btn_pause = ttk.Button(row, text="⏸ Пауза", command=self.on_pause)
        self.btn_pause.pack(side="left", fill="x", expand=True, padx=1)
        self.btn_cont = ttk.Button(row, text="▶ Пуск ▶", command=self.on_continue)
        self.btn_cont.pack(side="left", fill="x", expand=True, padx=1)
        row2 = ttk.Frame(side)
        row2.pack(fill="x")
        self.btn_step = ttk.Button(row2, text="Шаг", command=self.on_step)
        self.btn_step.pack(side="left", fill="x", expand=True, padx=1)
        self.btn_stop = ttk.Button(row2, text="⏹ Стоп", command=self.on_stop)
        self.btn_stop.pack(side="left", fill="x", expand=True, padx=1)
        ttk.Button(side, text="↻ Перезапуск", command=self.on_restart).pack(fill="x", pady=2)
        ttk.Button(side, text="＋ Новая машина…", command=self.on_new_machine).pack(fill="x", pady=2)
        row3 = ttk.Frame(side)
        row3.pack(fill="x", pady=(6, 0))
        ttk.Button(row3, text="Изменить", command=self.on_preset_edit).pack(
            side="left", fill="x", expand=True, padx=1)
        ttk.Button(row3, text="Удалить", command=self.on_preset_delete).pack(
            side="left", fill="x", expand=True, padx=1)

        # -- notebook -----------------------------------------------------
        self.nb = ttk.Notebook(main)
        main.add(self.nb, weight=1)
        self.tab_screen = ttk.Frame(self.nb, padding=4)
        self.tab_config = ttk.Frame(self.nb, padding=4)
        self.tab_mon = ttk.Frame(self.nb, padding=4)
        self.tab_art = ttk.Frame(self.nb, padding=4)
        self.tab_snap = ttk.Frame(self.nb, padding=4)
        self.tab_net = ttk.Frame(self.nb, padding=4)
        self.tab_log = ttk.Frame(self.nb, padding=4)
        self.nb.add(self.tab_screen, text=" Экран ")
        self.nb.add(self.tab_config, text=" Конфигурация ")
        self.nb.add(self.tab_mon, text=" Монитор ")
        self.nb.add(self.tab_art, text=" Артефакты ")
        self.nb.add(self.tab_snap, text=" Снапшоты ")
        self.nb.add(self.tab_net, text=" Сеть ")
        self.nb.add(self.tab_log, text=" Журнал ")

        self._build_screen_tab()
        self._build_config_tab()
        self._build_monitor_tab()
        self._build_artifacts_tab()
        self._build_snapshots_tab()
        self._build_net_tab()
        self._build_log_tab()

        self.status = ttk.Label(self.root, text="готов", style="Status.TLabel", anchor="w")
        self.status.pack(fill="x", side="bottom")

    def _build_screen_tab(self):
        bar = ttk.Frame(self.tab_screen)
        bar.pack(fill="x")
        ttk.Label(bar, text="Масштаб:").pack(side="left")
        self.scale_var = tk.StringVar(value="fit")
        cb = ttk.Combobox(bar, textvariable=self.scale_var, state="readonly", width=8,
                          values=("fit", "1", "1.5", "2"))
        cb.pack(side="left", padx=4)
        cb.bind("<<ComboboxSelected>>", lambda e: self._apply_scale())
        self.fb_info = ttk.Label(bar, text="фреймбуфер: —")
        self.fb_info.pack(side="right")
        wrap = ttk.Frame(self.tab_screen)
        wrap.pack(fill="both", expand=True)
        self.canvas = tk.Canvas(wrap, bg="black", highlightthickness=0)
        self.canvas.pack(fill="both", expand=True)

    def _apply_scale(self):
        if self.photo is None:
            return
        v = self.scale_var.get()
        if v == "fit":
            self.canvas.coords("fb", 0, 0)
            self.canvas.itemconfigure("fb", width=self.fb.width, height=self.fb.height)
        else:
            z = float(v)
            self.canvas.itemconfigure("fb", width=int(self.fb.width * z),
                                      height=int(self.fb.height * z))

    def _build_config_tab(self):
        outer = ttk.Frame(self.tab_config)
        outer.pack(fill="both", expand=True)
        canvas = tk.Canvas(outer, highlightthickness=0)
        sb = ttk.Scrollbar(outer, orient="vertical", command=canvas.yview)
        form = ttk.Frame(canvas, padding=8)
        form.bind("<Configure>",
                  lambda e: canvas.configure(scrollregion=canvas.bbox("all")))
        canvas.create_window((0, 0), window=form, anchor="nw")
        canvas.configure(yscrollcommand=sb.set)
        canvas.pack(side="left", fill="both", expand=True)
        sb.pack(side="right", fill="y")

        self.cfg_vars = {}
        self.path_vars = {}

        def section(title):
            ttk.Separator(form, orient="horizontal").pack(fill="x", pady=6)
            ttk.Label(form, text=title, font=("TkDefaultFont", 10, "bold")).pack(anchor="w")

        def row_path(key, label, browse_kind="file"):
            r = ttk.Frame(form)
            r.pack(fill="x", pady=1)
            ttk.Label(r, text=label, width=22, anchor="w").pack(side="left")
            var = tk.StringVar()
            e = ttk.Entry(r, textvariable=var)
            e.pack(side="left", fill="x", expand=True, padx=4)
            def browse():
                if browse_kind == "save":
                    p = filedialog.asksaveasfilename(parent=self.root)
                else:
                    p = filedialog.askopenfilename(parent=self.root)
                if p:
                    var.set(p)
            ttk.Button(r, text="…", width=3, command=browse).pack(side="left")
            self.cfg_vars[key] = var

        def row_choice(key, label, values):
            r = ttk.Frame(form)
            r.pack(fill="x", pady=1)
            ttk.Label(r, text=label, width=22, anchor="w").pack(side="left")
            var = tk.StringVar()
            ttk.Combobox(r, textvariable=var, values=values, width=18).pack(
                side="left", padx=4)
            self.cfg_vars[key] = var

        def row_str(key, label):
            r = ttk.Frame(form)
            r.pack(fill="x", pady=1)
            ttk.Label(r, text=label, width=22, anchor="w").pack(side="left")
            var = tk.StringVar()
            ttk.Entry(r, textvariable=var).pack(side="left", fill="x", expand=True, padx=4)
            self.cfg_vars[key] = var

        def row_int(key, label):
            row_str(key, label)

        def row_bool(key, label):
            r = ttk.Frame(form)
            r.pack(fill="x", pady=1)
            var = tk.BooleanVar()
            ttk.Checkbutton(r, text=label, variable=var).pack(side="left")
            self.cfg_vars[key] = var

        section("Загрузка")
        row_path("rom", "Прошивка (ROM)")
        row_path("disk", "Диск (USB-образ)")
        row_path("kernel", "Ядро (ELF64)")
        row_path("initrd", "Initrd")
        row_path("snapshot_load", "Снапшот для загрузки")
        section("ЦП / платформа")
        row_choice("platform", "Платформа", launcher.PLATFORMS)
        row_int("smp", "vCPU (smp)")
        row_int("cpus", "Ядер (cpus)")
        row_bool("smp_probe", "smp-probe")
        section("Чипсет")
        row_choice("lpc", "LPC", launcher.LPCS)
        row_bool("no_usb_uhci", "Без USB UHCI")
        section("Накопители (SATA)")
        row_path("sata", "Порт 0 (диск/ISO)")
        row_path("sata1", "Порт 1")
        row_path("sata2p", "Порт 2")
        row_path("sata3", "Порт 3")
        row_bool("ahci2", "Второй контроллер (ahci2)")
        row_path("sata2", "ahci2: порт 0")
        row_path("sata2_port1", "ahci2: порт 1")
        row_bool("sata_writethrough", "Write-through")
        section("Видео")
        row_str("vbe", "Режим VBE (например 1024x768x32 / off)")
        row_bool("no_fb", "Без фреймбуфера")
        section("Сеть")
        row_str("tap", "TAP (host:port)")
        section("Прогон")
        row_int("max_instr", "Лимит инструкций")
        row_bool("stats", "Статистика")
        row_bool("log", "Журнал (log)")
        row_bool("dump_fb", "Скриншот fb")
        row_bool("dump_text", "Текстовый экран")
        row_bool("trace_file", "Трасса")
        row_int("trace_from", "Трасса: с инструкции")
        row_int("trace_count", "Трасса: сколько")
        section("Отладка")
        row_str("gdb", "GDB (порт или host:port)")
        row_str("watch_phys", "Следить за физ. адресом")
        row_path("symbols", "Файл символов")
        row_path("debug_script", "Скрипт отладчика")
        row_str("keys", "Скан-коды (hex, через запятую)")
        row_str("keys_at", "Когда вводить (prompt/адрес)")
        row_path("snapshot_save", "Сохранить снапшот в")

        self._config_from_dict(BUILTIN_CONFIG)

    def _build_monitor_tab(self):
        bar = ttk.Frame(self.tab_mon)
        bar.pack(fill="x")
        ttk.Label(bar, text="Команда монитора:").pack(side="left")
        self.mon_entry = ttk.Entry(bar)
        self.mon_entry.pack(side="left", fill="x", expand=True, padx=4)
        self.mon_entry.bind("<Return>", lambda e: self.on_exec())
        ttk.Button(bar, text="Выполнить", command=self.on_exec).pack(side="left")
        for label, line in (("regs", "regs"), ("breaks", "info break"),
                            ("ahci", "ahci"), ("pic", "pic"), ("log", "log")):
            ttk.Button(bar, text=label,
                       command=lambda l=line: self.exec_line(l)).pack(side="left", padx=1)
        self.mon_out = tk.Text(self.tab_mon, height=18, wrap="word",
                               state="normal", bg="#101418", fg="#d7e0ea")
        self.mon_out.pack(fill="both", expand=True, pady=4)
        self.mon_out.insert("end", "—")
        self.mon_out.configure(state="disabled")

    def _build_artifacts_tab(self):
        bar = ttk.Frame(self.tab_art)
        bar.pack(fill="x")
        ttk.Button(bar, text="Обновить", command=self.refresh_artifacts).pack(side="left")
        ttk.Button(bar, text="Сохранить как…", command=self.on_artifact_save).pack(
            side="left", padx=4)
        ttk.Button(bar, text="Открыть папку", command=self.on_artifact_dir).pack(side="left")
        self.art_tree = ttk.Treeview(self.tab_art, columns=("size", "mtime"),
                                     show="headings", height=14)
        self.art_tree.heading("size", text="Размер")
        self.art_tree.heading("mtime", text="Изменён")
        self.art_tree.pack(fill="both", expand=True, pady=4)

    def _build_snapshots_tab(self):
        bar = ttk.Frame(self.tab_snap)
        bar.pack(fill="x")
        ttk.Label(bar, text="Имя:").pack(side="left")
        self.snap_name = tk.StringVar(value="gui.snap")
        ttk.Entry(bar, textvariable=self.snap_name, width=24).pack(side="left", padx=4)
        ttk.Button(bar, text="Сохранить (savevm)", command=self.on_snap_save).pack(side="left")
        ttk.Button(bar, text="Загрузить (loadvm)", command=self.on_snap_load).pack(
            side="left", padx=4)
        ttk.Button(bar, text="Удалить", command=self.on_snap_delete).pack(side="left", padx=4)
        ttk.Button(bar, text="Обновить", command=self.refresh_snapshots).pack(side="left", padx=4)
        ttk.Label(self.tab_snap, foreground="#666",
                  text="Снапшоты работают только на остановленной машине (пауза/шаг/точка)."
                  ).pack(anchor="w", pady=2)
        self.snap_tree = ttk.Treeview(self.tab_snap, columns=("size", "mtime"),
                                      show="headings", height=14)
        self.snap_tree.heading("size", text="Размер")
        self.snap_tree.heading("mtime", text="Изменён")
        self.snap_tree.pack(fill="both", expand=True, pady=4)

    def _build_net_tab(self):
        bar = ttk.Frame(self.tab_net, padding=4)
        bar.pack(fill="x")
        ttk.Label(bar, text="Порт респондера:").pack(side="left")
        self.tap_port = tk.StringVar(value="8080")
        ttk.Entry(bar, textvariable=self.tap_port, width=8).pack(side="left", padx=4)
        ttk.Label(bar, text="drop:").pack(side="left")
        self.tap_drop = tk.StringVar(value="")
        ttk.Entry(bar, textvariable=self.tap_drop, width=16).pack(side="left", padx=4)
        self.tap_no_tcp = tk.BooleanVar(value=False)
        ttk.Checkbutton(bar, text="no-tcp", variable=self.tap_no_tcp).pack(side="left", padx=4)
        ttk.Button(bar, text="Старт", command=self.on_tap_start).pack(side="left", padx=4)
        ttk.Button(bar, text="Стоп", command=self.on_tap_stop).pack(side="left")
        self.tap_status = ttk.Label(self.tab_net, text="респондер остановлен")
        self.tap_status.pack(anchor="w", pady=4)
        ttk.Label(self.tab_net, foreground="#666", wraplength=640, justify="left", text=(
            "TAP-респондер отвечает на ARP/ICMP/DHCP/DNS/TCP для адреса host:port из "
            "поля «TAP» в Конфигурации. Порт респондера выбирается из семейства "
            "8080/8081/8082 (AURALITE_TAP_PORT) — см. docs/plans/NET_PLAN.md.")).pack(
            anchor="w", pady=8)

    def _build_log_tab(self):
        bar = ttk.Frame(self.tab_log)
        bar.pack(fill="x")
        ttk.Label(bar, text="Фильтр:").pack(side="left")
        self.log_filter = tk.StringVar(value="")
        ttk.Entry(bar, textvariable=self.log_filter, width=24).pack(side="left", padx=4)
        self.log_filter.trace_add("write", lambda *_: self._paint_log())
        self.log_scroll = tk.BooleanVar(value=True)
        ttk.Checkbutton(bar, text="автопрокрутка", variable=self.log_scroll).pack(side="left")
        ttk.Button(bar, text="Обновить", command=self.refresh_output).pack(side="left", padx=4)
        self.log_box = tk.Text(self.tab_log, height=20, wrap="none",
                               bg="#101418", fg="#d7e0ea")
        self.log_box.pack(fill="both", expand=True, pady=4)

    # -- config <-> form --------------------------------------------------
    def _config_from_dict(self, cfg):
        for key, var in self.cfg_vars.items():
            v = cfg.get(key, "")
            if isinstance(var, tk.BooleanVar):
                var.set(bool(v))
            else:
                var.set("" if v is None or v is False else str(v))

    def collect_config(self):
        cfg = {}
        for key, var in self.cfg_vars.items():
            spec = SCHEMA[key]
            if isinstance(var, tk.BooleanVar):
                if var.get():
                    cfg[key] = True
                continue
            val = var.get().strip()
            if not val:
                continue
            if spec["type"] == "int":
                try:
                    cfg[key] = int(val, 0)
                except ValueError:
                    raise ConfigError("%s: ожидается целое число" % key)
            elif spec["type"] == "bool":
                cfg[key] = True
            else:
                cfg[key] = val
        return cfg

    def set_config(self, cfg):
        self._config_from_dict(cfg)

    # -- modes ------------------------------------------------------------
    def set_mode(self, mode):
        mode = "expert" if mode == "expert" else "normal"
        self.ui["mode"] = mode
        self.mode_var.set(mode)
        keep = {self.tab_screen, self.tab_log}
        if mode == "expert":
            keep |= {self.tab_config, self.tab_mon, self.tab_art,
                     self.tab_snap, self.tab_net}
        for tab in (self.tab_config, self.tab_mon, self.tab_art,
                    self.tab_snap, self.tab_net):
            if tab in keep:
                if str(tab) not in self.nb.tabs():
                    self.nb.add(tab)
            else:
                if str(tab) in self.nb.tabs():
                    self.nb.forget(tab)
        self._save_ui()

    def _save_ui(self):
        self.ui["autostart"] = bool(self.autostart_var.get())
        self.ui["geometry"] = self.root.geometry()
        save_ui_state(self.state_dir, self.ui)

    # -- presets (machines) ------------------------------------------------
    def refresh_presets(self):
        self.preset_list.delete(0, "end")
        for name in sorted(self.presets.load()):
            self.preset_list.insert("end", name)
        last = self.ui.get("last_preset", "")
        names = sorted(self.presets.load())
        if last in names:
            self.preset_list.selection_set(names.index(last))

    def current_preset(self):
        sel = self.preset_list.curselection()
        if not sel:
            return None
        return self.preset_list.get(sel[0])

    def on_preset_select(self):
        name = self.current_preset()
        if not name:
            return
        self.ui["last_preset"] = name
        self._save_ui()
        cfg = self.presets.load().get(name)
        if cfg:
            self.set_config(cfg)

    def on_preset_edit(self):
        name = self.current_preset()
        if not name:
            messagebox.showinfo(APP_TITLE, "Сначала выберите машину в списке.")
            return
        self.set_mode("expert")
        self.nb.select(self.tab_config)

    def on_preset_delete(self):
        name = self.current_preset()
        if not name:
            return
        if not messagebox.askyesno(APP_TITLE, "Удалить машину «%s»?" % name):
            return
        self.presets.delete(name)
        self.refresh_presets()

    def save_current_preset(self, name, cfg):
        validate_config(cfg)
        self.presets.save(name, cfg)
        self.refresh_presets()
        self.ui["last_preset"] = name
        self._save_ui()

    # -- wizard ------------------------------------------------------------
    def on_new_machine(self):
        Wizard(self)

    # -- lifecycle ---------------------------------------------------------
    def on_start(self):
        if self.machine_state == "running":
            return
        try:
            cfg = self.collect_config()
            validate_config(cfg)
        except ConfigError as e:
            messagebox.showerror(APP_TITLE, "Конфигурация не запущена:\n%s" % e)
            return
        try:
            res = self.machine.start(cfg)
        except ConfigError as e:
            messagebox.showerror(APP_TITLE, "Запуск отклонён:\n%s" % e)
            return
        self.log_lines = []
        self._paint_log()
        self.mon.close()
        self.connect_deadline = time.time() + 20.0
        self.status.configure(text="монитор подключается… (порт %s)" % res["monitor_port"])
        self.refresh_artifacts()

    def on_pause(self):
        self._send({"cmd": "pause"})

    def on_continue(self):
        self._send({"cmd": "continue"})

    def on_step(self):
        self._send({"cmd": "step"})

    def on_stop(self):
        """End-stop semantics (TOOLING T5): `quit` prints stats and artifacts."""
        if self.machine_state == "running":
            self._send({"cmd": "quit"})
            self.stop_deadline = time.time() + 5.0
        else:
            self.machine.stop()
            self.refresh_artifacts()

    def on_restart(self):
        cfg = None
        try:
            cfg = self.collect_config()
            validate_config(cfg)
        except ConfigError as e:
            messagebox.showerror(APP_TITLE, "Конфигурация некорректна:\n%s" % e)
            return
        self.machine.stop()
        self.mon.close()
        self.on_start()

    def _send(self, obj):
        try:
            self.mon.send(obj)
        except OSError as e:
            self.status.configure(text="монитор недоступен: %s" % e)

    def _connect_monitor(self):
        port = self.machine.monitor_port
        if not port:
            return
        try:
            self.mon.connect(port, timeout=1.0)
        except OSError:
            return
        self.badge_mon.configure(text="монитор: на связи", bg="#2f7d32")
        self.status.configure(text="монитор подключён")
        if self.autostart_var.get():
            self._send({"cmd": "continue"})

    # -- polling -----------------------------------------------------------
    def _poll_status(self):
        st = self.machine.status()
        self.machine_state = st["state"]
        self.exit_code = st["exit_code"]
        if st["state"] == "running":
            if self.last_state_msg.get("stop_reason") == "end":
                self.badge_state.configure(
                    text="прогон завершён (end) — «Стоп» сохранит результаты",
                    bg="#2f7d32")
            else:
                self.badge_state.configure(text="машина работает", bg="#2f7d32")
            if self.mon.sock is None and time.time() < self.connect_deadline:
                self._connect_monitor()
        elif st["state"] == "exited":
            txt = "прогон завершён (код %s)" % st["exit_code"]
            self.badge_state.configure(text=txt,
                                       bg="#2f7d32" if st["exit_code"] == 0 else "#a33")
            if self.mon.sock is not None:
                self.mon.close()
                self.badge_mon.configure(text="монитор: —", bg="#5b6470")
                self.refresh_artifacts()
                self.refresh_output()
        else:
            self.badge_state.configure(text="машина выключена", bg="#5b6470")
        if self.stop_deadline and time.time() > self.stop_deadline:
            self.stop_deadline = 0.0
            if self.machine.status()["state"] == "running":
                self.machine.stop()
                self.refresh_artifacts()
        st_txt = "готов"
        if self.last_state_msg:
            st_txt = "%s | %s | инстр: %s" % (
                self.last_state_msg.get("stop_reason", "—"),
                self.last_state_msg.get("mode", "—"),
                self.last_state_msg.get("executed", "—"))
        taps = st.get("responder") or {}
        self.status.configure(text=st_txt + (
            " | TAP-респондер: %s" % ("pid %s" % taps["pid"] if taps.get("running") else "стоп")))
        self.root.after(STATUS_POLL_MS, self._poll_status)

    def _poll_monitor(self):
        if self.mon.sock is not None:
            for kind, payload in self.mon.pump():
                if kind == "text":
                    if payload.get("kind") == "state":
                        self.last_state_msg = payload
                        self._paint_state()
                    elif payload.get("kind") == "reply":
                        msg = "%s %s: %s" % ("OK" if payload.get("ok") else "ОШИБКА",
                                             payload.get("cmd", ""),
                                             payload.get("message", ""))
                        if payload.get("output"):
                            msg += "\n" + payload["output"]
                        self.reply_text = msg
                        self._paint_mon()
                elif kind == "fb":
                    self.fb.apply(payload)
                    self._render_fb()
                elif kind == "closed":
                    self.badge_mon.configure(text="монитор: отключён", bg="#a33")
        self.root.after(MONITOR_POLL_MS, self._poll_monitor)

    def _paint_state(self):
        s = self.last_state_msg
        self.fb_info.configure(text="фреймбуфер: %sx%s" % (self.fb.width or "—",
                                                           self.fb.height or "—"))
        regs = s.get("registers") or {}
        self._paint_mon()

    def _paint_mon(self):
        s = self.last_state_msg
        regs = s.get("registers") or {}
        body = [
            "режим: %s   останов: %s   инстр: %s" % (
                s.get("mode", "—"), s.get("stop_reason", "—"), s.get("executed", "—")),
            "дизассемблер: %s" % s.get("disassembly", "—"),
            "",
        ]
        for k, v in list(regs.items()):
            body.append("%s = %s" % (k, v))
        body.append("")
        body.append("ответ: " + self.reply_text)
        txt = "\n".join(body)
        self.mon_out.configure(state="normal")
        self.mon_out.delete("1.0", "end")
        self.mon_out.insert("end", txt)
        self.mon_out.configure(state="disabled")

    def _render_fb(self):
        if not self.fb.width or not self.fb.height:
            return
        now = time.time()
        if (now - self.last_render) * 1000.0 < RENDER_MIN_MS:
            return
        self.last_render = now
        if (self.photo is None or int(self.photo.width()) != self.fb.width
                or int(self.photo.height()) != self.fb.height):
            self.photo = tk.PhotoImage(width=self.fb.width, height=self.fb.height)
            self.canvas.delete("all")
            self.canvas.create_image(0, 0, image=self.photo, anchor="nw", tags="fb")
            self._apply_scale()
        for y in sorted(self.fb.dirty):
            try:
                self.photo.put(self.fb.row_colors(y), to=(0, y))
            except tk.TclError:
                return
        self.fb.dirty.clear()
        self.fb_info.configure(text="фреймбуфер: %sx%s" % (self.fb.width, self.fb.height))

    # -- tabs actions ------------------------------------------------------
    def on_exec(self):
        line = self.mon_entry.get().strip()
        if line:
            self.exec_line(line)

    def exec_line(self, line):
        self._send({"cmd": "exec", "line": line})

    def refresh_output(self):
        out = self.machine.output("stdout", tail=400)
        err = self.machine.output("stderr", tail=100)
        self.log_lines = [l.rstrip("\n") for l in out.splitlines()]
        if err:
            self.log_lines += ["--- stderr ---"] + [l.rstrip("\n") for l in err.splitlines()]
        self._paint_log()

    def _paint_log(self):
        f = (self.log_filter.get() or "").lower()
        txt = "\n".join(l for l in self.log_lines if not f or f in l.lower()) or "(пусто)"
        self.log_box.configure(state="normal")
        self.log_box.delete("1.0", "end")
        self.log_box.insert("end", txt)
        if self.log_scroll.get():
            self.log_box.see("end")
        self.log_box.configure(state="disabled")

    def refresh_artifacts(self):
        for i in self.art_tree.get_children():
            self.art_tree.delete(i)
        for a in self.machine.artifacts():
            self.art_tree.insert("", "end", iid=a["name"],
                                 values=(a["size"],
                                         time.strftime("%Y-%m-%d %H:%M:%S",
                                                       time.localtime(a["mtime"]))))

    def on_artifact_save(self):
        sel = self.art_tree.selection()
        if not sel:
            return
        name = sel[0]
        src = self.machine.artifact_path(name)
        if not src:
            return
        dst = filedialog.asksaveasfilename(parent=self.root, initialfile=name)
        if dst:
            shutil.copyfile(src, dst)

    def on_artifact_dir(self):
        if not self.machine.run_dir:
            messagebox.showinfo(APP_TITLE, "Прогон ещё не запускался.")
            return
        path = self.machine.run_dir
        try:
            if sys.platform.startswith("win"):
                os.startfile(path)  # noqa: S606
            else:
                subprocess.Popen(["xdg-open", path])
        except OSError as e:
            messagebox.showerror(APP_TITLE, "Не удалось открыть папку: %s" % e)

    def refresh_snapshots(self):
        for i in self.snap_tree.get_children():
            self.snap_tree.delete(i)
        for a in self.machine.snapshots():
            self.snap_tree.insert("", "end", iid=a["name"],
                                  values=(a["size"],
                                          time.strftime("%Y-%m-%d %H:%M:%S",
                                                        time.localtime(a["mtime"]))))

    def _snap_path(self):
        name = self.snap_name.get().strip()
        if not name:
            raise ConfigError("укажите имя снапшота")
        return os.path.join(self.machine.snap_dir,
                            os.path.basename(name.replace("/", "_")))

    def on_snap_save(self):
        try:
            path = self._snap_path()
        except ConfigError as e:
            messagebox.showerror(APP_TITLE, str(e))
            return
        self.exec_line("savevm " + path)
        self.root.after(400, self.refresh_snapshots)

    def on_snap_load(self):
        sel = self.snap_tree.selection()
        name = sel[0] if sel else self.snap_name.get().strip()
        if not name:
            return
        self.exec_line("loadvm " + os.path.join(self.machine.snap_dir,
                                                os.path.basename(name)))
        self.root.after(400, self.refresh_snapshots)

    def on_snap_delete(self):
        sel = self.snap_tree.selection()
        if not sel:
            return
        self.machine.snapshot_delete(sel[0])
        self.refresh_snapshots()

    def on_tap_start(self):
        try:
            self.machine.responder_start({"port": int(self.tap_port.get()),
                                          "drop": self.tap_drop.get(),
                                          "no_tcp": self.tap_no_tcp.get()})
        except (ConfigError, ValueError) as e:
            messagebox.showerror(APP_TITLE, "Респондер не запущен:\n%s" % e)
            return
        self.tap_status.configure(text="респондер работает")

    def on_tap_stop(self):
        self.machine.responder_stop()
        self.tap_status.configure(text="респондер остановлен")

    def open_state_dir(self):
        try:
            if sys.platform.startswith("win"):
                os.startfile(self.state_dir)  # noqa: S606
            else:
                subprocess.Popen(["xdg-open", self.state_dir])
        except OSError:
            pass

    def on_about(self):
        messagebox.showinfo(APP_TITLE, (
            "AuraLite Emulator — графический запуск машин.\n\n"
            "Обычный режим: мастер создания машин и загрузка с ISO.\n"
            "Экспертный режим: все опции командной строки, монитор,\n"
            "артефакты, снапшоты и TAP-респондер.\n\n"
            "Все запуски строятся строго по SCHEMA — как в CLI."))

    def on_close(self):
        self._save_ui()
        try:
            self.machine.responder_stop()
        except Exception:
            pass
        if self.machine_state == "running":
            self._send({"cmd": "quit"})
            deadline = time.time() + 3.0
            while time.time() < deadline and self.machine.status()["state"] == "running":
                time.sleep(0.05)
        self.machine.stop()
        self.mon.close()
        self.root.destroy()


# ---------------------------------------------------------------------------
# New-machine wizard (normal mode)
# ---------------------------------------------------------------------------

KIND_LABELS = [
    ("iso", "Загрузка с ISO / CD-образа (как в VirtualBox)"),
    ("disk", "Дисковый образ (USB/SATA диск)"),
    ("kernel", "Ядро ELF64 напрямую (без прошивки)"),
    ("empty", "Демо-машина (встроенная прошивка)"),
]


class Wizard(tk.Toplevel):
    def __init__(self, app):
        super().__init__(app.root)
        self.app = app
        self.title("Новая машина")
        self.resizable(False, False)
        self.transient(app.root)
        self.kind = tk.StringVar(value="iso")
        self.name = tk.StringVar(value="")
        self.path = tk.StringVar(value="")
        self.initrd = tk.StringVar(value="")
        self.bios = tk.StringVar(value=find_seabios(app.state_dir,
                                                    app.ui.get("bios") or None) or "")
        self.run_now = tk.BooleanVar(value=True)
        self.page = 0
        self.body = ttk.Frame(self, padding=12)
        self.body.pack(fill="both", expand=True)
        self.nav = ttk.Frame(self, padding=(12, 8))
        self.nav.pack(fill="x")
        self.btn_back = ttk.Button(self.nav, text="← Назад", command=self.on_back)
        self.btn_back.pack(side="left")
        self.btn_next = ttk.Button(self.nav, text="Далее →", command=self.on_next)
        self.btn_next.pack(side="right")
        self.render()

    def render(self):
        for w in self.body.winfo_children():
            w.destroy()
        app = self.app
        if self.page == 0:
            ttk.Label(self.body, text="Шаг 1. Имя и тип машины",
                      font=("TkDefaultFont", 11, "bold")).pack(anchor="w", pady=(0, 8))
            r = ttk.Frame(self.body)
            r.pack(fill="x", pady=4)
            ttk.Label(r, text="Имя:").pack(side="left")
            ttk.Entry(r, textvariable=self.name, width=32).pack(side="left", padx=6)
            for kind, label in KIND_LABELS:
                ttk.Radiobutton(self.body, text=label, value=kind,
                                variable=self.kind).pack(anchor="w", pady=2)
            self.btn_next.configure(text="Далее →")
            self.btn_back.configure(state="disabled")
        elif self.page == 1:
            kind = self.kind.get()
            titles = {"iso": "Шаг 2. Выберите ISO-образ",
                      "disk": "Шаг 2. Выберите дисковый образ",
                      "kernel": "Шаг 2. Выберите ядро (ELF64)",
                      "empty": "Шаг 2. Носитель не нужен"}
            ttk.Label(self.body, text=titles[kind],
                      font=("TkDefaultFont", 11, "bold")).pack(anchor="w", pady=(0, 8))
            if kind == "empty":
                ttk.Label(self.body, text=(
                    "Демо-машина запускает встроенную прошивку без носителей.")).pack(
                    anchor="w", pady=8)
            else:
                r = ttk.Frame(self.body)
                r.pack(fill="x", pady=4)
                filetypes = [("Все файлы", "*.*")]
                if kind == "iso":
                    filetypes = [("ISO / дисковые образы", "*.iso *.img *.bin"),
                                 ("Все файлы", "*.*")]
                elif kind == "kernel":
                    filetypes = [("ELF", "*.elf *.bin"), ("Все файлы", "*.*")]
                ttk.Label(r, text="Файл:").pack(side="left")
                ttk.Entry(r, textvariable=self.path, width=46).pack(side="left", padx=6)

                def browse():
                    p = filedialog.askopenfilename(parent=self, filetypes=filetypes)
                    if p:
                        self.path.set(p)
                ttk.Button(r, text="Обзор…", command=browse).pack(side="left")
                if kind == "kernel":
                    r2 = ttk.Frame(self.body)
                    r2.pack(fill="x", pady=4)
                    ttk.Label(r2, text="Initrd:").pack(side="left")
                    ttk.Entry(r2, textvariable=self.initrd, width=46).pack(side="left", padx=6)
                    ttk.Button(r2, text="Обзор…", command=lambda: (
                        self.initrd.set(filedialog.askopenfilename(parent=self))
                        or None)).pack(side="left")
                if kind == "iso":
                    ttk.Separator(self.body, orient="horizontal").pack(fill="x", pady=8)
                    ttk.Label(self.body, text="Прошивка SeaBIOS (bios.bin):",
                              font=("TkDefaultFont", 10, "bold")).pack(anchor="w")
                    r3 = ttk.Frame(self.body)
                    r3.pack(fill="x", pady=4)
                    ttk.Entry(r3, textvariable=self.bios, width=46).pack(
                        side="left", padx=(0, 6))
                    ttk.Button(r3, text="Обзор…", command=lambda: (
                        self.bios.set(filedialog.askopenfilename(parent=self))
                        or None)).pack(side="left")
                    if not self.bios.get():
                        ttk.Label(self.body, foreground="#a33", wraplength=520, text=(
                            "Для загрузки с ISO нужен стоковый SeaBIOS (файл bios.bin, "
                            "например из пакета Debian «seabios» или с www.seabios.org). "
                            "Обычный режим подставит его автоматически.")).pack(anchor="w", pady=4)
            self.btn_back.configure(state="normal")
            self.btn_next.configure(text="Далее →")
        else:
            kind = self.kind.get()
            try:
                cfg = self.preview_config()
            except ConfigError as e:
                cfg = None
                err = str(e)
            ttk.Label(self.body, text="Шаг 3. Проверьте и создайте",
                      font=("TkDefaultFont", 11, "bold")).pack(anchor="w", pady=(0, 8))
            if cfg is None:
                ttk.Label(self.body, foreground="#a33", wraplength=520,
                          text="Не удалось собрать конфигурацию: %s" % err).pack(
                    anchor="w", pady=8)
            else:
                lines = ["Имя: %s" % self.name.get().strip(),
                         "Тип: %s" % dict(KIND_LABELS)[kind]]
                for k in sorted(cfg):
                    lines.append("  %s = %s" % (k, cfg[k]))
                ttk.Label(self.body, text="Будет запущено:",
                          font=("TkDefaultFont", 10, "bold")).pack(anchor="w")
                box = tk.Text(self.body, width=64, height=16, wrap="none")
                box.pack(fill="both", expand=True, pady=4)
                box.insert("end", "\n".join(lines))
                box.configure(state="disabled")
            ttk.Checkbutton(self.body, text="Запустить сразу после создания",
                            variable=self.run_now).pack(anchor="w", pady=6)
            self.btn_back.configure(state="normal")
            self.btn_next.configure(text="Создать ✓")

    def preview_config(self):
        kind = self.kind.get()
        path = self.path.get().strip() or None
        bios = self.bios.get().strip() or None
        initrd = self.initrd.get().strip() or None
        return normal_config(kind, path=path, bios=bios, initrd=initrd)

    def on_back(self):
        if self.page > 0:
            self.page -= 1
            self.render()

    def on_next(self):
        if self.page < 2:
            self.page += 1
            self.render()
            return
        name = self.name.get().strip()
        if not name:
            messagebox.showerror(APP_TITLE, "Укажите имя машины.", parent=self)
            return
        try:
            cfg = self.preview_config()
            self.app.save_current_preset(name, cfg)
        except ConfigError as e:
            messagebox.showerror(APP_TITLE, "Машина не создана:\n%s" % e, parent=self)
            return
        if self.kind.get() == "iso" and self.bios.get().strip():
            self.app.ui["bios"] = self.bios.get().strip()
            self.app._save_ui()
        self.destroy()
        if self.run_now.get():
            self.app.set_config(cfg)
            self.app.on_start()


# ---------------------------------------------------------------------------
# main
# ---------------------------------------------------------------------------

def main(argv=None):
    ap = argparse.ArgumentParser(description="AuraLite-Emulator desktop GUI")
    ap.add_argument("--state-dir", default=os.path.expanduser("~/.x86emu-gui"))
    ap.add_argument("--emu", default=os.path.join(ROOT, "x86emu"),
                    help="path to the x86emu binary")
    ap.add_argument("--preset", default="",
                    help="machine (preset) to load and start immediately")
    a = ap.parse_args(argv)

    if not os.path.isfile(a.emu) and os.path.isfile(a.emu + ".exe"):
        a.emu += ".exe"
    if not os.path.isfile(a.emu):
        print("gui_app: %s not found -- run `make` first" % a.emu, file=sys.stderr)
        return 2

    if sys.platform.startswith("win"):      # crisp text on high-DPI screens
        try:
            import ctypes
            ctypes.windll.shcore.SetProcessDpiAwareness(1)
        except Exception:
            pass

    root = tk.Tk()
    app = App(root, a.emu, a.state_dir)
    root.protocol("WM_DELETE_WINDOW", app.on_close)
    if a.preset:
        app.ui["last_preset"] = a.preset
        app.refresh_presets()
        cfg = app.presets.load().get(a.preset)
        if cfg:
            app.set_config(cfg)
            app.on_start()
    root.mainloop()
    return 0


if __name__ == "__main__":
    sys.exit(main())
