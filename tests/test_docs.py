#!/usr/bin/env python3
"""tests/test_docs.py -- documentation invariants (audit A7).

Three claims the tree makes about its own documentation are cheap to state and
expensive to keep true by hand, so they are checked here:

  1. **The tree is English.** A byte-level sweep over every tracked file
     (`.git` excluded) must find no Cyrillic codepoints.  This is the
     post-`0111` state ("the whole tree in English, no Cyrillic anywhere")
     holding as the tree keeps changing -- `0111` itself was a one-off
     translation pass, and nothing stopped a later patch from adding a Russian
     comment back into a source file, a plan, or a GUI string.
  2. **The architecture document exists where it is linked.** `0112` removed
     `PROJECT_ANALYSIS.md` (a hand-copied audit note that had drifted three
     phases out of date) in favour of `docs/architecture.md` +
     `docs/memory_map.md` + `docs/README.md`, and redirected every reference to
     it.  A dangling link in the README or in a plan is the failure mode.
  3. **The removed document is not referenced as if it still existed.** The
     archive (`patches/`) and the chronological record (`CHANGELOG.md`) are
     allowed to name it -- they record history -- but no other live document or
     source file may.

The link check is deliberately scoped to the four documents that carry the
reading path (README, architecture, memory map, docs index), not to the whole
archive: `patches/*.patch` legitimately contain diffs of files that no longer
exist, and validating those would fight the history.
"""
from __future__ import annotations

import os
import re
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

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


def tracked_files() -> list[str]:
    """Every file git knows about; falls back to a walk for a bare tree."""
    try:
        out = subprocess.run(["git", "ls-files", "-z"], cwd=ROOT,
                             capture_output=True, check=True).stdout
        names = [n for n in out.decode("utf-8", "replace").split("\0") if n]
        if names:
            return names
    except (OSError, subprocess.CalledProcessError):
        pass
    found = []
    for base, dirs, files in os.walk(ROOT):
        dirs[:] = [d for d in dirs if d not in (".git", "__pycache__")]
        for fn in files:
            found.append(os.path.relpath(os.path.join(base, fn), ROOT))
    return found


CYRILLIC = re.compile("[\u0400-\u04FF\u0500-\u052F\u2DE0-\u2DFF\uA640-\uA69F]")


def check_no_cyrillic(files: list[str]) -> None:
    bad: list[str] = []
    for rel in files:
        path = os.path.join(ROOT, rel)
        try:
            with open(path, "rb") as f:
                data = f.read()
        except OSError:
            continue
        if b"\xd0" not in data and b"\xd1" not in data and b"\xd2" not in data:
            continue                      # no UTF-8 two-byte lead: cannot be Cyrillic
        try:
            text = data.decode("utf-8")
        except UnicodeDecodeError:
            text = data.decode("latin-1")
        hits = CYRILLIC.findall(text)
        if hits:
            sample = "".join(sorted(set(hits)))[:24]
            bad.append("%s (%d chars: %s)" % (rel, len(hits), sample))
    report(not bad,
           "A7 no Cyrillic in any tracked file (%d scanned)" % len(files),
           "; ".join(bad[:6]))


REQUIRED_DOCS = {
    "docs/architecture.md": ["## Layers", "## CPU core", "## Interrupts",
                             "## Boot paths", "## Invariants that must not move",
                             "## Source map"],
    "docs/memory_map.md": ["## Physical memory", "## I/O port space",
                           "## PCI topology"],
    "docs/README.md": ["## Recommended reading order",
                       "## Documentation conventions"],
}


def check_architecture_docs() -> None:
    for rel, sections in REQUIRED_DOCS.items():
        path = os.path.join(ROOT, rel)
        if not os.path.exists(path):
            report(False, "A7 %s exists" % rel)
            continue
        with open(path, encoding="utf-8") as f:
            text = f.read()
        missing = [s for s in sections if s not in text]
        report(not missing, "A7 %s exists with its required sections" % rel,
               "missing: " + ", ".join(missing) if missing else "")


LINK = re.compile(r"\[[^\]]*\]\(([^)]+)\)")
LINK_SCOPE = ["README.md", "docs/architecture.md", "docs/memory_map.md", "docs/README.md"]


def check_links() -> None:
    for rel in LINK_SCOPE:
        path = os.path.join(ROOT, rel)
        if not os.path.exists(path):
            report(False, "A7 %s exists (link scope)" % rel)
            continue
        base = os.path.dirname(path)
        with open(path, encoding="utf-8") as f:
            text = f.read()
        broken = []
        for m in LINK.finditer(text):
            target = m.group(1).split("#")[0].strip()
            if not target or target.startswith(("http://", "https://", "mailto:")):
                continue
            if not os.path.exists(os.path.normpath(os.path.join(base, target))):
                broken.append(target)
        report(not broken, "A7 every relative link in %s resolves" % rel,
               ", ".join(sorted(set(broken))))


# files allowed to name the removed audit note: the patch archive and the
# chronological record.  Everything else must have been redirected.
ALLOWED = ("patches/", "CHANGELOG.md", "tests/test_docs.py")


# a mention is harmless when the same line says the note is gone, or names what
# replaced it -- those are exactly the redirects this lane exists to require.
REMOVAL_MARKERS = ("removed", "replaced", "replaces", "no longer", "gone",
                   "since `0112`", "that note is")


def check_no_dangling_project_analysis(files: list[str]) -> None:
    offenders = []
    for rel in files:
        if rel.startswith(ALLOWED) or not rel.endswith((".md", ".c", ".h", ".py")):
            continue
        path = os.path.join(ROOT, rel)
        try:
            with open(path, encoding="utf-8", errors="replace") as f:
                text = f.read()
        except OSError:
            continue
        for line in text.splitlines():
            if "PROJECT_ANALYSIS" not in line:
                continue
            if any(mark in line for mark in REMOVAL_MARKERS):
                continue
            offenders.append("%s: %s" % (rel, line.strip()[:72]))
    report(not offenders,
           "A7 no live document references the removed PROJECT_ANALYSIS.md",
           ", ".join(offenders))
    report(not os.path.exists(os.path.join(ROOT, "PROJECT_ANALYSIS.md")),
           "A7 PROJECT_ANALYSIS.md is gone from the repository root")


def main() -> int:
    files = tracked_files()
    check_no_cyrillic(files)
    check_architecture_docs()
    check_links()
    check_no_dangling_project_analysis(files)
    print("test-docs: %d checks, %d failures" % (checks, failures))
    return 0 if failures == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
