#!/usr/bin/env python3
"""tests/test_cli_inputs.py -- the command-line input contract (audit A1-A6).

README s"Run" states the exit-code contract this file pins:

    exit 0  normal stop or instruction-limit stop
    exit 1  input-file errors
    exit 2  the CPU entered a fault state

Before the audit the file-media and numeric options did not honour it, and
each defect below was reproduced on the built binary first:

  A1  `--rom=<directory>` / `--rom=<empty file>` returned a 0-byte "ROM", the
      machine executed the empty flash and died with `#UD group5 ext=7`
      (exit 2), i.e. an input error was reported as a CPU fault.  The same
      reads gave `--disk=<directory>` and `--disk=/dev/zero` a zero-byte disk
      that was logged as attached.
  A2  `--max-instr=abc` parsed as 0 (exit 0 after ZERO instructions),
      `--smp=-5` silently meant one vCPU, `--keys-at=abc` injected at once,
      `--watch-phys=zzz` watched address 0, and `--sata-portfoo=img` used
      atoi("foo") == 0, attaching the image to PORT 0 -- a silent change of
      machine shape.  `--sata-port99=img` was dropped with exit 0.
  A3  `--vbe=800x600x32junk` was accepted (bare sscanf without %n).
  A4  `--rom=<256 KiB image>` kept the FIRST 128 KiB of the image, so the
      half holding the reset vector was the half that got dropped: a stock
      SeaBIOS 256 KiB build produced `#UD`/exit 2 while the 128 KiB build of
      the same firmware booted.
  A5  `--gdb=abc` and `--gdb=99999` printed a message and then continued
      WITHOUT a debugger, exit 0 -- a scripted lane silently lost its
      debugger.
  A6  the shipped binary carried AuraLite-OS-specific bring-up diagnostics
      (three hard-coded kernel RIPs compared once per instruction, /tmp probe
      writes from inside the interpreter).  They are a compile-time option
      now (`make DEBUG_HOOKS=1`), and the default binary must be clean.

Every vector is paired with a control that must KEEP working, so a fix that
simply rejects everything cannot pass.
"""
from __future__ import annotations

import os
import subprocess
import sys
import tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
EMU = os.environ.get("X86EMU") or os.path.join(ROOT, "x86emu")
ROM = os.path.join(ROOT, "firmware", "firmware.bin")
DISK = os.path.join(ROOT, "disk", "disk.img")
SEABIOS_256 = "/usr/share/seabios/bios-256k.bin"

checks = 0
failures = 0


def report(ok: bool, what: str, detail: str = "") -> None:
    global checks, failures
    checks += 1
    if ok:
        print("ok   " + what)
    else:
        failures += 1
        print("FAIL " + what + (("  -- " + detail) if detail else ""))


def run(args: list[str], timeout: int = 60):
    p = subprocess.run([EMU, *args], cwd=ROOT, capture_output=True, text=True, timeout=timeout)
    return p.returncode, (p.stdout + p.stderr)


def expect_rc(args: list[str], want: int, needle: str = "", timeout: int = 60) -> None:
    label = " ".join(args)
    try:
        rc, out = run(args, timeout)
    except subprocess.TimeoutExpired:
        report(False, label, "timed out")
        return
    if rc != want:
        report(False, label, "exit %d, expected %d" % (rc, want))
        return
    if needle and needle not in out:
        report(False, label, "message %r missing from output" % needle)
        return
    report(True, label)


def main() -> int:
    if not os.path.exists(EMU):
        print("skip: %s is not built" % EMU, file=sys.stderr)
        return 0
    # The control checks boot the sample firmware and the sample guest kernel,
    # so a fresh clone needs `make firmware/firmware.bin disk/disk.img` first
    # (the `test-cli` target depends on both).  Say so instead of dying inside
    # the first control with a bare traceback.
    missing = [p for p in (ROM, DISK) if not os.path.exists(p)]
    if missing:
        print("skip: missing build fixture(s): %s\n"
              "      run `make firmware/firmware.bin disk/disk.img`"
              % ", ".join(missing), file=sys.stderr)
        return 0

    work = tempfile.mkdtemp(prefix="cli-inputs-")
    empty = os.path.join(work, "empty.bin")
    open(empty, "wb").close()

    # ---------------- A1: media files must be real, non-empty regular files
    expect_rc(["--rom=" + work], 1, "not a regular file")
    expect_rc(["--rom=" + empty], 1, "empty file")
    expect_rc(["--disk=" + work, "--max-instr=10"], 1, "not a regular file")
    expect_rc(["--disk=/dev/zero", "--max-instr=10"], 1, "not a regular file")
    expect_rc(["--sata=" + os.path.join(work, "nope.img"), "--max-instr=10"],
              1, "No such file or directory")
    expect_rc(["--sata2=" + os.path.join(work, "nope.img"), "--max-instr=10"],
              1, "No such file or directory")
    expect_rc(["--cdrom=" + os.path.join(work, "nope.iso"), "--max-instr=10"],
              1, "No such file or directory")
    # control: the real media keep working
    expect_rc(["--rom=" + ROM, "--max-instr=200000"], 0, "GUEST_OK")
    if os.path.exists(DISK):
        expect_rc(["--disk=" + DISK, "--max-instr=200000"], 0)
    # control: the built-in default disk stays optional (the no-media lane)
    expect_rc(["--rom=" + ROM, "--disk=" + os.path.join(work, "absent.img"),
              "--max-instr=10"], 1, "absent.img")
    ctrl = subprocess.run([EMU, "--rom=" + ROM, "--max-instr=10"],
                          cwd=work, capture_output=True, text=True)
    report(ctrl.returncode == 0,
           "A1 control: absent default disk/disk.img is still not fatal",
           "exit %d" % ctrl.returncode)

    # ---------------- A2: every numeric option is parsed strictly
    for bad in ("abc", "12x", "", "0x", "-1", "1e9"):
        expect_rc(["--max-instr=" + bad], 1, "--max-instr")
    for bad in ("abc", "12x", "", "-1", "999"):
        expect_rc(["--smp=" + bad], 1, "--smp")
    expect_rc(["--smp=8", "--max-instr=10"], 0)
    for bad in ("abc", "", "-3"):
        expect_rc(["--cpus=" + bad], 1, "--cpus")
    for bad in ("abc", "12x", "-1"):
        expect_rc(["--keys-at=" + bad, "--max-instr=10"], 1, "--keys-at")
    expect_rc(["--keys-at=prompt", "--max-instr=10"], 0)
    for bad in ("zzz", "0x", "-1"):
        expect_rc(["--watch-phys=" + bad, "--max-instr=10"], 1, "--watch-phys")
    expect_rc(["--trace-from=abc", "--max-instr=10"], 1, "--trace-from")
    expect_rc(["--trace-count=xyz", "--max-instr=10"], 1, "--trace-count")
    # the port digit of --sata-portN / --sata2-portN: atoi("foo") used to be 0
    expect_rc(["--sata-portfoo=" + DISK, "--max-instr=10"], 1, "sata-portN")
    expect_rc(["--sata-port99=" + DISK, "--max-instr=10"], 1, "sata-portN")
    expect_rc(["--sata2-portfoo=" + DISK, "--max-instr=10"], 1, "sata2-portN")
    expect_rc(["--sata-port5=" + DISK, "--max-instr=10"], 0)
    expect_rc(["--sata-port0=" + DISK, "--max-instr=10"], 0)

    # ---------------- A3: --vbe is an exact grammar
    for bad in ("bogus", "800x600x32junk", "800x600", "", "0x600x32",
                "800x0x32", "800x600x0", "99999999x1x32"):
        expect_rc(["--vbe=" + bad, "--max-instr=10"], 1, "--vbe")
    for good in ("800x600x32", "1024x768x16", "off"):
        expect_rc(["--vbe=" + good, "--max-instr=10"], 0)

    # ---------------- A4: a ROM larger than the window is refused, not halved
    if os.path.exists(SEABIOS_256) and os.path.getsize(SEABIOS_256) > 131072:
        expect_rc(["--rom=" + SEABIOS_256, "--max-instr=10"], 1, "ROM window")
    else:
        big = os.path.join(work, "big.rom")
        with open(ROM, "rb") as src:
            image = src.read()          # read ONCE: a second read() is empty
        with open(big, "wb") as dst:
            dst.write(image)
            dst.write(image)            # 2x the window, so it must be refused
        expect_rc(["--rom=" + big, "--max-instr=10"], 1, "ROM window")

    # ---------------- A5: --gdb is validated like --monitor
    for bad in ("abc", "99999", "0", ":1234", "host:abc", "1.2.3.4:"):
        expect_rc(["--gdb=" + bad, "--max-instr=10"], 1, "--gdb")
    # control: --monitor keeps its own (already strict) contract
    expect_rc(["--monitor=abc", "--max-instr=10"], 1, "--monitor")

    # ---------------- A6: no guest-specific diagnostics in the shipped binary
    try:
        with open(EMU, "rb") as f:
            blob = f.read()
    except OSError as exc:
        report(False, "A6 read the binary", str(exc))
        blob = b""
    for needle in (b"k4-probe", b"[dbg-u]", b"AURALITE_IVT_LOG", b"801907be"):
        report(needle not in blob,
               "A6 release binary carries no %r" % needle.decode(),
               "found in %s" % EMU)
    # control: the debug-hook build does carry them (proves the grep can see them)
    dbg = os.path.join(ROOT, "x86emu-dbg")
    if os.path.exists(dbg):
        with open(dbg, "rb") as f:
            dblob = f.read()
        report(b"k4-probe" in dblob,
               "A6 control: DEBUG_HOOKS build does carry the probe sink")
    else:
        print("skip A6 control (no x86emu-dbg build; `make DEBUG_HOOKS=1`)")

    print("test-cli-inputs: %d checks, %d failures" % (checks, failures))
    return 0 if failures == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
