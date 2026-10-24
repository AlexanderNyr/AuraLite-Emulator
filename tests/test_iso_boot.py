#!/usr/bin/env python3
"""tests/test_iso_boot.py -- plain ISO boot acceptance.

"Select an ISO and start" must actually boot it, like VirtualBox/VMware.

Two lanes share one test ISO (tools/mkiso.py + tests/fixtures/bootstub.nas):
  Q  quick route   --rom=bios.bin --lpc=ich9 --sata=test.iso
     SeaBIOS reads the MBR at LBA 0 of the SATA-attached image (the
     documented recipe, docs/WINDOWS.md E / GUI "Boot from ISO");
  C  CD route      --rom=bios.bin --lpc=ich9 --cdrom=test.iso
     SeaBIOS reads the El Torito catalog from an ATAPI CD (STORE S6).

Both must print AURA_ISO_BOOT_OK from the boot stub to COM1 (src/serial.c
flushes it into the machine log).  The lanes SKIP -- they never fail --
when stock SeaBIOS (bios.bin) or NASM is not available on this host.
"""
import argparse
import os
import shutil
import subprocess
import sys
import tempfile
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, "tools"))
import mkiso  # noqa: E402

MARKER = "AURA_ISO_BOOT_OK"
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


def find_bios(explicit=None):
    if explicit and os.path.isfile(explicit):
        return explicit
    env = os.environ.get("AURALITE_BIOS")
    if env and os.path.isfile(env):
        return env
    for d in (os.path.expanduser("~/.x86emu-gui"), ROOT,
              os.path.join(ROOT, "firmware")):
        for name in ("bios.bin", "seabios.bin"):
            p = os.path.join(d, name)
            if os.path.isfile(p):
                return p
    return None


def build_stub(tmp):
    nasm = shutil.which("nasm")
    if not nasm:
        return None
    src = os.path.join(ROOT, "tests", "fixtures", "bootstub.nas")
    out = os.path.join(tmp, "bootstub.bin")
    r = subprocess.run([nasm, "-f", "bin", "-o", out, src],
                       capture_output=True, text=True)
    if r.returncode != 0 or not os.path.isfile(out):
        print(r.stderr)
        return None
    return out


def run_boot(emu, bios, iso, tag, tmp, extra=()):
    log = os.path.join(tmp, tag + ".log.txt")
    argv = [emu, "--rom=" + bios, "--lpc=ich9", *extra,
            "--max-instr=2000000000", "--log=" + log, "--stats"]
    t0 = time.time()
    proc = subprocess.Popen(argv, cwd=ROOT, stdout=subprocess.PIPE,
                            stderr=subprocess.PIPE, stdin=subprocess.DEVNULL)
    marker = False
    deadline = time.time() + 180
    while time.time() < deadline:
        for path in (log, None):
            try:
                if path:
                    with open(path, "r", encoding="utf-8", errors="replace") as f:
                        txt = f.read()
                else:
                    break
                if MARKER in txt:
                    marker = True
                    break
            except OSError:
                pass
        if marker:
            break
        if proc.poll() is not None:
            break
        time.sleep(0.2)
    if proc.poll() is None:
        proc.terminate()
        try:
            out, err = proc.communicate(timeout=10)
        except subprocess.TimeoutExpired:
            proc.kill()
            out, err = proc.communicate()
    else:
        out, err = proc.communicate()
    tail = ((out or b"") + (err or b"")).decode("utf-8", "replace")
    if not marker:
        try:
            with open(log, "r", encoding="utf-8", errors="replace") as f:
                marker = MARKER in f.read()
        except OSError:
            pass
    if not marker:
        marker = MARKER in tail
    return marker, time.time() - t0, tail


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--bios", default=None, help="path to stock SeaBIOS bios.bin")
    ap.add_argument("--emu", default=os.path.join(ROOT, "x86emu"))
    a = ap.parse_args()

    if not os.path.isfile(a.emu):
        print("test_iso_boot: %s not found -- run make first" % a.emu, file=sys.stderr)
        return 2
    bios = find_bios(a.bios)
    if not bios:
        skip("Q ISO-as-disk boot (no stock SeaBIOS bios.bin -- see README)")
        skip("C ISO-as-CD boot (no stock SeaBIOS bios.bin)")
        print("test_iso_boot: PASS (skipped)")
        return 0

    tmp = tempfile.mkdtemp(prefix="isoboot-")
    try:
        stub = build_stub(tmp)
        if not stub:
            skip("Q ISO-as-disk boot (NASM not available)")
            skip("C ISO-as-CD boot (NASM not available)")
            print("test_iso_boot: PASS (skipped)")
            return 0
        with open(stub, "rb") as f:
            boot = f.read()
        iso_path = os.path.join(tmp, "test.iso")
        with open(iso_path, "wb") as f:
            f.write(mkiso.build_iso(boot))

        # NC: unknown CLI options must be refused (measured defect: they
        # used to be silently ignored -- --cdrom=... booted a disk-less
        # machine and the lane-C skip detector could not see the difference).
        bogus = subprocess.run([a.emu, "--definitely-not-a-flag"],
                               cwd=ROOT, capture_output=True, text=True,
                               timeout=60)
        check(bogus.returncode != 0 and "usage:" in (bogus.stderr or ""),
              "NC unknown CLI option is refused with usage")

        marker, secs, tail = run_boot(a.emu, bios, iso_path, "q", tmp,
                                      extra=["--sata=" + iso_path])
        check(marker, "Q SeaBIOS boots the ISO as a disk (MBR) -- %s" %
              ("%.1fs" % secs))
        if not marker:
            print(tail[-2000:])

        # Lane C: --cdrom= exists only after STORE S6; pre-S6 the CLI prints
        # its usage and exits 1 -- that is a skip, not a failure.
        probe = subprocess.run([a.emu, "--cdrom=" + iso_path, "--max-instr=1"],
                               cwd=ROOT, capture_output=True, text=True,
                               timeout=60)
        if probe.returncode != 0 and "usage:" in (probe.stderr or ""):
            skip("C ISO-as-CD El Torito boot (--cdrom: STORE S6 not in this build)")
        else:
            marker, secs, tail = run_boot(a.emu, bios, iso_path, "c", tmp,
                                          extra=["--cdrom=" + iso_path])
            check(marker, "C SeaBIOS boots the ISO as an ATAPI CD (El Torito) -- %s"
                  % ("%.1fs" % secs))
            if not marker:
                print(tail[-2000:])
    finally:
        shutil.rmtree(tmp, ignore_errors=True)
    print("test_iso_boot: %s" % ("FAIL" if fails else "PASS"))
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
