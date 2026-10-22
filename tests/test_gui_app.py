#!/usr/bin/env python3
"""tests/test_gui_app.py -- GUI G2 vectors for tools/gui_app.py (tkinter).

The desktop app must keep every G1 ground rule (SCHEMA-only argv, state dir,
the monitor WebSocket protocol) while adding the normal/expert modes and the
plain ISO boot flow.

Vectors:
  A. UI-state reader (corrupt file falls back to defaults) and SeaBIOS
     discovery order.
  B. normal_config() machine shapes: iso (SeaBIOS + lpc=ich9 + sata), disk,
     kernel, empty -- each one must pass launcher.validate_config; refusals:
     missing files, missing bios, directory instead of image.
  C. MonitorClient + FrameBuffer against the real emulator: connect over
     RFC 6455, first push is state + framebuffer keyframe, continue ends the
     run with exit 0 and artifacts; FB\x01 delta decoding is byte-exact.
  D. GUI construction lane (skipped when no display): App builds, mode switch
     moves the notebook tabs, collect_config/set_config round-trip.
"""
import json
import os
import shutil
import struct
import sys
import tempfile
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, "tools"))
import gui_app  # noqa: E402
import launcher  # noqa: E402

fails = 0


def check(cond, label):
    global fails
    if cond:
        print("ok  ", label)
    else:
        print("FAIL", label)
        fails += 1


def skip(label):
    print("skip", label)


# ---------------------------------------------------------------------------
# A. ui state + SeaBIOS discovery
# ---------------------------------------------------------------------------

def test_ui_state(tmp):
    os.makedirs(tmp, exist_ok=True)
    st = gui_app.load_ui_state(tmp)
    check(st["mode"] == "normal" and st["autostart"] is True, "A ui defaults intact")
    with open(os.path.join(tmp, "gui_state.json"), "w", encoding="utf-8") as f:
        f.write("{ this is not json")
    st = gui_app.load_ui_state(tmp)
    check(st == gui_app.DEFAULT_UI, "A corrupt gui_state.json falls back to defaults")
    with open(os.path.join(tmp, "gui_state.json"), "w", encoding="utf-8") as f:
        json.dump({"mode": "expert", "geometry": "800x600", "junk": 1}, f)
    st = gui_app.load_ui_state(tmp)
    check(st["mode"] == "expert" and st["geometry"] == "800x600"
          and "junk" not in st, "A partial gui_state.json merges known keys only")


def test_find_seabios(tmp):
    os.makedirs(tmp, exist_ok=True)
    # Hermetic: a real AURALITE_BIOS (or a repo bios.bin) must not shadow
    # the fixtures -- save/restore the env and point ROOT at the sandbox.
    saved_env = os.environ.pop("AURALITE_BIOS", None)
    saved_root = gui_app.ROOT
    gui_app.ROOT = tmp
    try:
        _find_seabios_body(tmp)
    finally:
        gui_app.ROOT = saved_root
        if saved_env is not None:
            os.environ["AURALITE_BIOS"] = saved_env


def _find_seabios_body(tmp):
    bios = os.path.join(tmp, "bios.bin")
    with open(bios, "wb") as f:
        f.write(b"\x00" * 128)
    check(gui_app.find_seabios(tmp) == os.path.abspath(bios),
          "A find_seabios sees the state-dir copy")
    other = os.path.join(tmp, "other.bin")
    with open(other, "wb") as f:
        f.write(b"\x00" * 16)
    check(gui_app.find_seabios(tmp, explicit=other) == os.path.abspath(other),
          "A explicit pick wins the search order")
    os.remove(bios)
    os.environ["AURALITE_BIOS"] = other
    try:
        check(gui_app.find_seabios(tmp) == os.path.abspath(other),
              "A AURALITE_BIOS env is honoured")
    finally:
        os.environ.pop("AURALITE_BIOS", None)
    empty = os.path.join(tmp, "empty")
    os.makedirs(empty, exist_ok=True)
    check(gui_app.find_seabios(empty) is None,
          "A no bios anywhere returns None")


# ---------------------------------------------------------------------------
# B. normal_config machine shapes
# ---------------------------------------------------------------------------

def test_normal_config(tmp):
    iso = os.path.join(tmp, "test.iso")
    with open(iso, "wb") as f:
        f.write(b"\x00" * 4096)
    bios = os.path.join(tmp, "bios.bin")
    with open(bios, "wb") as f:
        f.write(b"\x00" * 128)
    disk = os.path.join(ROOT, "disk", "disk.img")

    cfg = gui_app.normal_config("iso", path=iso, bios=bios)
    check(cfg["rom"] == bios and cfg["lpc"] == "ich9" and cfg["cdrom"] == iso
          and cfg["max_instr"] == gui_app.ISO_MAX_INSTR,
          "B iso shape = SeaBIOS + ich9 + cdrom (ATAPI/El Torito)")
    try:
        launcher.validate_config(cfg)
        check(True, "B iso config passes validate_config")
    except launcher.ConfigError as e:
        check(False, "B iso config passes validate_config (%s)" % e)

    cfg = gui_app.normal_config("disk", path=disk)
    check(cfg["disk"] == disk and cfg.get("sata") is None
          and cfg["rom"].endswith("firmware.bin"),
          "B disk shape = built-in firmware + disk image")
    launcher.validate_config(cfg)

    elf = os.path.join(tmp, "kernel.elf")
    with open(elf, "wb") as f:
        f.write(b"\x7fELF" + b"\x00" * 60)
    initrd = os.path.join(tmp, "initrd.tar")
    with open(initrd, "wb") as f:
        f.write(b"\x00" * 16)
    cfg = gui_app.normal_config("kernel", path=elf, initrd=initrd)
    check(cfg["kernel"] == elf and cfg["initrd"] == initrd and "rom" not in cfg,
          "B kernel shape = direct ELF boot without firmware")
    launcher.validate_config(cfg)

    cfg = gui_app.normal_config("empty")
    check("rom" not in cfg and "disk" not in cfg and "sata" not in cfg,
          "B empty shape carries no media")
    launcher.validate_config(cfg)

    # negative controls
    try:
        gui_app.normal_config("iso", path=iso)          # no bios
        check(False, "B NC iso without bios is refused")
    except launcher.ConfigError:
        check(True, "B NC iso without bios is refused")
    try:
        gui_app.normal_config("iso", path=os.path.join(tmp, "nope.iso"), bios=bios)
        check(False, "B NC missing iso file is refused")
    except launcher.ConfigError:
        check(True, "B NC missing iso file is refused")
    try:
        gui_app.normal_config("disk", path=tmp)          # a directory, not an image
        check(False, "B NC directory instead of image is refused")
    except launcher.ConfigError:
        check(True, "B NC directory instead of image is refused")
    try:
        gui_app.normal_config("banana")
        check(False, "B NC unknown machine kind is refused")
    except launcher.ConfigError:
        check(True, "B NC unknown machine kind is refused")


# ---------------------------------------------------------------------------
# C. MonitorClient + FrameBuffer against the real emulator
# ---------------------------------------------------------------------------

def fb_keyframe(w=4, h=2):
    rows = b"".join(struct.pack("<H", y) + bytes([10 * y + x for x in range(w * 3)])
                    for y in range(h))
    return b"FB\x01" + struct.pack("<BHHH", 1, w, h, h) + rows


def test_framebuffer():
    fb = gui_app.FrameBuffer()
    fb.apply(fb_keyframe())
    check(fb.width == 4 and fb.height == 2 and fb.dirty == {0, 1},
          "C keyframe decodes w/h and marks all rows dirty")
    check(fb.row_colors(0) == "{#000102 #030405 #060708 #090a0b}",
          "C row_colors renders one #rrggbb per pixel")
    delta = b"FB\x01" + struct.pack("<BHHH", 0, 4, 2, 1) + \
        struct.pack("<H", 1) + bytes([0xFF]) * 12
    fb.dirty.clear()
    fb.apply(delta)
    check(fb.dirty == {1} and fb.row_colors(1) == "{" + " ".join(["#ffffff"] * 4) + "}",
          "C delta updates exactly its rows byte-exact")
    fb2 = gui_app.FrameBuffer()
    fb2.apply(b"XXXX garbage")
    check(fb2.width == 0, "C non-FB blobs are ignored")


def test_monitor_client():
    emu = os.path.join(ROOT, "x86emu")
    if not os.path.isfile(emu):
        check(False, "C x86emu is built (run make first)")
        return
    state = tempfile.mkdtemp(prefix="guiapp-state-")
    try:
        m = launcher.Machine(emu, state)
        cfg = {"platform": "haswell", "rom": "firmware/firmware.bin",
               "disk": "disk/disk.img", "max_instr": 3000000,
               "stats": True, "log": True, "dump_fb": True}
        m.start(cfg)
        # UPKEEP U7: the run's monitor demands the per-run capability the
        # Machine generated. A tokenless client must be refused at the
        # upgrade itself (loop until the monitor is listening and the
        # refusal is the *upgrade* error, not connection timing).
        bad = gui_app.MonitorClient()
        refused = False
        deadline = time.time() + 10
        while time.time() < deadline and not refused:
            try:
                bad.connect(m.monitor_port, timeout=1.0)   # no token, on purpose
                bad.close()
                break                     # handshake accepted: pre-U7 defect
            except OSError as e:
                refused = "refused" in str(e)
                if not refused:
                    time.sleep(0.1)       # monitor not bound yet; retry
        check(refused and getattr(m, "monitor_token", None),
              "C U7 tokenless WebSocket is refused; Machine issues a capability")
        mon = gui_app.MonitorClient()
        deadline = time.time() + 20
        connected = False
        while time.time() < deadline and not connected:
            try:
                kw = {}
                if getattr(m, "monitor_token", None):
                    kw["token"] = m.monitor_token   # pre-U7: connect() has no kw
                mon.connect(m.monitor_port, timeout=1.0, **kw)
                connected = True
            except OSError:
                time.sleep(0.1)
        check(connected, "C MonitorClient handshakes with the live monitor")

        saw_state = saw_fb = False
        deadline = time.time() + 15
        while time.time() < deadline and not (saw_state and saw_fb):
            for kind, payload in mon.pump():
                if kind == "text" and isinstance(payload, dict) \
                        and payload.get("kind") == "state":
                    saw_state = True
                elif kind == "fb":
                    if payload[:3] == b"FB\x01":
                        saw_fb = True
        check(saw_state, "C first pushes include a state message")
        check(saw_fb, "C framebuffer keyframe arrives as a binary event")

        mon.send({"cmd": "continue"})
        # T5 end-stop semantics: with a monitor attached the machine lives on
        # the post-mortem stop (stop_reason: end); a clean exit needs `quit`.
        deadline = time.time() + 60
        saw_end = False
        while time.time() < deadline and not saw_end:
            for kind, payload in mon.pump():
                if kind == "text" and isinstance(payload, dict) \
                        and payload.get("kind") == "state" \
                        and payload.get("stop_reason") == "end":
                    saw_end = True
                    break
            if m.status()["state"] == "exited":
                break
            time.sleep(0.05)
        check(saw_end, "C run reaches the end stop through the GUI client")
        if m.status()["state"] == "running":
            mon.send({"cmd": "quit"})
        deadline = time.time() + 30
        exited = False
        while time.time() < deadline:
            if m.status()["state"] == "exited":
                exited = True
                break
            time.sleep(0.05)
        check(exited and m.status()["exit_code"] == 0,
              "C quit turns the end stop into a clean exit 0")
        arts = {a["name"] for a in m.artifacts()}
        check("stdout.log" in arts and "argv.json" in arts,
              "C artifacts land in the run dir")
        if getattr(m, "monitor_token", None) and "argv.json" in arts:
            with open(os.path.join(m.run_dir, "argv.json")) as f:
                argv_txt = f.read()
            check(m.monitor_token not in argv_txt,
                  "C U7 monitor capability stays out of argv.json (env, not argv)")
        mon.close()
        try:
            mon.send({"cmd": "state"})
            check(False, "C NC send on a closed client raises")
        except OSError:
            check(True, "C NC send on a closed client raises")
    finally:
        m.stop()
        shutil.rmtree(state, ignore_errors=True)


# ---------------------------------------------------------------------------
# D. GUI construction (needs a display; skipped headless)
# ---------------------------------------------------------------------------

def test_gui_construct(tmp):
    try:
        import tkinter as tk
        root = tk.Tk()
    except Exception as e:
        skip("D GUI construction (no display: %s)" % e.__class__.__name__)
        return
    try:
        emu = os.path.join(ROOT, "x86emu")
        app = gui_app.App(root, emu, tmp)
        check(app.ui["mode"] == "normal", "D App builds in normal mode")
        app.set_mode("expert")
        nb_tabs = set(app.nb.tabs())
        check(str(app.tab_config) in nb_tabs and str(app.tab_mon) in nb_tabs,
              "D expert mode exposes the full notebook")
        app.set_mode("normal")
        nb_tabs = set(app.nb.tabs())
        check(str(app.tab_config) not in nb_tabs and str(app.tab_screen) in nb_tabs,
              "D normal mode keeps just screen + log")
        iso = os.path.join(tmp, "x.iso")
        open(iso, "wb").write(b"\x00" * 512)
        cfg = gui_app.normal_config("iso", path=iso, bios=iso)
        app.set_config(cfg)
        got = app.collect_config()
        check(got.get("sata") == iso and got.get("lpc") == "ich9",
              "D set_config/collect_config round-trips")
        root.update()
    finally:
        try:
            app.on_close()
        except Exception:
            root.destroy()


# ---------------------------------------------------------------------------

def main():
    tmp = tempfile.mkdtemp(prefix="guiapp-ui-")
    try:
        test_ui_state(tmp)
        test_find_seabios(tmp)
        test_normal_config(tmp)
        test_framebuffer()
        test_monitor_client()
        test_gui_construct(tmp)
    finally:
        shutil.rmtree(tmp, ignore_errors=True)
    print("test_gui_app: %s" % ("FAIL" if fails else "PASS"))
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
