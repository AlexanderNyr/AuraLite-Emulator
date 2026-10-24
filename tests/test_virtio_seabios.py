#!/usr/bin/env python3
"""tests/test_virtio_seabios.py -- VIRTIO V3 integration lane.

Stock SeaBIOS is the second measured consumer of the modern
virtio transport (V0 census section 2.3): its own virtio-blk
driver parses the vendor capabilities, programs the queue and
reads LBA0 through the split ring -- nothing AuraLite-shaped in
the loop.  The boot stub (tests/fixtures/bootstub.nas, shared
with the ISO lanes) sits at LBA0 of a raw image attached ONLY
through --virtio-blk; the USB stick gets a zero image with no
0xAA55, so the marker can come through the virtio queue or not
at all.

Lanes:
  V  SeaBIOS boots the stub from the virtio-blk disk.
  S  snapshot mid-work: save during SeaBIOS (budget-capped,
     marker not yet printed), load, continue -- the marker must
     still arrive, and the end [stats] must be byte-identical
     to the uninterrupted lane V (executed_steps + instr_count:
     the deterministic machine loses nothing across a
     save/load that carries the blk transport and image).
  C  control: the same snapshot pair over plain --sata.  This
     pins the pre-existing defect V3 fixed -- xhci_snap's BAR0
     rebind ran *after* the snapshot MMIO-map comparison, so
     every post-enumeration snapshot refused to load (measured
     on the parent with zero virtio involvement).

Like the ISO lanes, everything SKIPs (never fails) when stock
SeaBIOS or NASM is missing, and V/S skip on builds that predate
--virtio-blk (unknown option -> usage).  C has no such excuse:
on a pre-V3 build it FAILS, which is its job.
"""
import argparse
import os
import re
import shutil
import subprocess
import sys
import tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, "tests"))
import test_iso_boot as iso   # find_bios/build_stub, same fixtures  # noqa: E402

MARKER = iso.MARKER
STATS = re.compile(r"\[stats\] vcpu=0 executed_steps=(\d+) instr_count=(\d+)")
SAVE_BUDGET = "60000000"      # < the ~120M executed steps to the marker
FULL_BUDGET = "2000000000"

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


def run(emu, bios, tmp, tag, extra):
    """One emulator run; returns (rc, marker, stats-pair-or-None, tail)."""
    log = os.path.join(tmp, tag + ".log.txt")
    argv = [emu, "--rom=" + bios, "--lpc=ich9", *extra, "--log=" + log]
    r = subprocess.run(argv, cwd=ROOT, capture_output=True, text=True,
                       timeout=300)
    txt = ""
    try:
        with open(log, "r", encoding="utf-8", errors="replace") as f:
            txt = f.read()
    except OSError:
        pass
    tail = (r.stdout or "") + (r.stderr or "")
    m = STATS.search(tail)
    stats = (int(m.group(1)), int(m.group(2))) if m else None
    return r.returncode, MARKER in txt or MARKER in tail, stats, tail


def snapshot_pair(emu, bios, tmp, tag, extra):
    """save mid-boot, then load and continue.  Returns (ok, stats)."""
    snap = os.path.join(tmp, tag + ".snap")
    rc, marker, _s, _t = run(emu, bios, tmp, tag + "-save",
                             [*extra, "--max-instr=" + SAVE_BUDGET,
                              "--snapshot-save=" + snap, "--stats"])
    if rc != 0 or marker or not os.path.isfile(snap):
        return False, None     # marker this early = the budget lied
    rc, marker, stats, _t = run(emu, bios, tmp, tag + "-load",
                                 [*extra, "--snapshot-load=" + snap,
                                  "--max-instr=" + FULL_BUDGET, "--stats"])
    return rc == 0 and marker, stats


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--bios", default=None)
    ap.add_argument("--emu", default=os.path.join(ROOT, "x86emu"))
    a = ap.parse_args()

    if not os.path.isfile(a.emu):
        print("test_virtio_seabios: %s not found -- run make first" % a.emu,
              file=sys.stderr)
        return 2
    bios = iso.find_bios(a.bios)
    if not bios:
        skip("V/S/C SeaBIOS virtio-blk lanes (no stock SeaBIOS bios.bin)")
        print("test_virtio_seabios: PASS (skipped)")
        return 0

    tmp = tempfile.mkdtemp(prefix="vblk-")
    try:
        stub = iso.build_stub(tmp)
        if not stub:
            skip("V/S/C SeaBIOS virtio-blk lanes (NASM not available)")
            print("test_virtio_seabios: PASS (skipped)")
            return 0
        with open(stub, "rb") as f:
            boot = f.read()
        mbr = os.path.join(tmp, "mbr.img")
        zero = os.path.join(tmp, "zero.img")
        with open(mbr, "wb") as f:
            f.write(boot + b"\x00" * (1048576 - len(boot)))
        with open(zero, "wb") as f:
            f.write(b"\x00" * 1048576)
        disk = ["--disk=" + zero]

        probe = subprocess.run([a.emu, "--virtio-blk=" + mbr,
                                "--max-instr=1"],
                               cwd=ROOT, capture_output=True, text=True,
                               timeout=60)
        have_virtio = not (probe.returncode != 0
                           and "usage:" in (probe.stderr or ""))
        if not have_virtio:
            skip("V SeaBIOS boots from virtio-blk (--virtio-blk: V2 not in this build)")
            skip("S snapshot continuity over virtio-blk (same)")
        else:
            vextra = ["--virtio-blk=" + mbr, *disk]
            rc, marker, vstats, tail = run(
                a.emu, bios, tmp, "v",
                [*vextra, "--max-instr=" + FULL_BUDGET, "--stats"])
            check(rc == 0 and marker,
                  "V SeaBIOS boots the stub from the virtio-blk disk")
            if not marker:
                print(tail[-2000:])
            ok, sstats = snapshot_pair(a.emu, bios, tmp, "s", vextra)
            check(ok, "S snapshot save/load across virtio-blk mid-boot")
            check(vstats is not None and vstats == sstats,
                  "S determinism: end [stats] identical to the "
                  "uninterrupted run (%r vs %r)" % (vstats, sstats))

        ok, _s = snapshot_pair(a.emu, bios, tmp, "c",
                               ["--sata=" + mbr, *disk])
        check(ok, "C control: post-enumeration snapshot loads (--sata pair)")
    finally:
        shutil.rmtree(tmp, ignore_errors=True)
    print("test_virtio_seabios: %s" % ("FAIL" if fails else "PASS"))
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
