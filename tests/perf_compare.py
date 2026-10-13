#!/usr/bin/env python3
"""PERF P5: paired seven-tape wall comparison with a bootstrap CI.

Never gate on wall time: the gates here are return codes, identical guest
work/clock and non-capped runs. Run from the repository root, with the same
firmware/disk artifacts and O2 compiler flags for both explicit binaries.
The bootstrap percentile interval describes paired *local* timing noise,
not a cross-machine prediction or an independent-replication guarantee.
"""
import argparse
import math
import os
import random
import re
import statistics
import subprocess
import sys
import time
from pathlib import Path

TAPES = (
    ("boot-haswell", 50000000, ("--platform=haswell",)),
    ("boot-broadwell", 50000000, ("--platform=broadwell",)),
    ("boot-baytrail", 50000000, ("--platform=baytrail",)),
    ("text-no-fb", 50000000, ("--platform=haswell", "--no-fb")),
    ("gfx-fb", 50000000, ("--platform=haswell",)),
    ("vbe-800x600x32", 50000000, ("--platform=haswell", "--vbe=800x600x32")),
    ("smp2", 20000000, ("--platform=haswell", "--smp=2", "--cpus=2")),
)
WORK_KEYS = ("executed", "idle_quantum", "instr_count_bsp", "vtime", "n_vcpus")
REQUIRED = WORK_KEYS + ("cap_reached",)


def parse_stats(text):
    """Parse each value exactly once from the opt-in stats lines."""
    stats = {}
    for line in text.splitlines():
        if not line.startswith("[stats] "):
            continue
        for key, val in re.findall(r"\b([a-z_]+)=([0-9]+)\b", line):
            if key not in REQUIRED:
                continue  # vcpu= appears twice on SMP; only global work keys matter
            if key in stats:
                raise ValueError(f"duplicate stats key {key}")
            stats[key] = int(val)
    missing = set(REQUIRED) - stats.keys()
    if missing:
        raise ValueError("missing stats: " + ",".join(sorted(missing)))
    return stats


def check_pair(before, after, tape):
    """P5 correctness gate, shared by real measurements and the NC vector."""
    for label, state in (("before", before), ("after", after)):
        if state["executed"] <= 0:
            raise ValueError(f"{tape}: {label} executed=0")
        if state["cap_reached"] != 0:
            raise ValueError(f"{tape}: {label} hit --max-instr")
    if any(before[k] != after[k] for k in WORK_KEYS):
        raise ValueError(f"{tape}: WORK MISMATCH: " +
                         ", ".join(f"{k}={before[k]}/{after[k]}" for k in WORK_KEYS))


def paired_interval(pairs, draws=10000, seed=20261012):
    """Geometric mean of paired speedup; 2.5/97.5 percentile resampling CI."""
    if len(pairs) < 3 or draws < 100:
        raise ValueError("need >=3 pairs and >=100 bootstrap draws")
    logs = [math.log(pre / post) for pre, post in pairs]
    if not all(math.isfinite(x) for x in logs):
        raise ValueError("non-finite timing")
    point = math.exp(statistics.mean(logs))
    rng = random.Random(seed)
    samples = sorted(math.exp(sum(logs[rng.randrange(len(logs))]
                                  for _ in logs) / len(logs)) for _ in range(draws))
    return point, samples[int(.025 * draws)], samples[int(.975 * draws)]


def run(binary, args, cap):
    start = time.perf_counter_ns()
    p = subprocess.run([str(binary), *args, f"--max-instr={cap}", "--stats"],
                       stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True,
                       check=False)
    elapsed = time.perf_counter_ns() - start
    if p.returncode:
        raise ValueError(f"{binary}: rc={p.returncode}; stderr={p.stderr[-400:]}")
    return elapsed, parse_stats(p.stdout)


def compare(before, after, reps):
    print(f"{'tape':<17} {'work':>9} {'reps':>5} {'ratio':>8} {'95% paired CI':>19}", flush=True)
    for idx, (name, cap, args) in enumerate(TAPES):
        pairs = []
        reference = None
        for rep in range(reps):
            # Reverse order every other pair to reduce linear host drift.
            if rep % 2:
                post_ns, post = run(after, args, cap)
                pre_ns, pre = run(before, args, cap)
            else:
                pre_ns, pre = run(before, args, cap)
                post_ns, post = run(after, args, cap)
            check_pair(pre, post, name)
            state = tuple(pre[k] for k in WORK_KEYS)
            if reference is None:
                reference = state
            elif state != reference:
                raise ValueError(f"{name}: NONDETERMINISTIC work across repetitions")
            pairs.append((pre_ns, post_ns))
        ratio, lo, hi = paired_interval(pairs, seed=20261012 + idx)
        print(f"{name:<17} {reference[0]:>9} {reps:>5} {ratio:>7.3f}x "
              f"[{lo:.3f}x, {hi:.3f}x]", flush=True)
    print("paired bench: identical work/clock, uncapped tapes; wall ratios are reports, NOT CI gates")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--before", required=True, type=Path)
    parser.add_argument("--after", required=True, type=Path)
    parser.add_argument("--reps", type=int, default=9)
    a = parser.parse_args()
    if a.reps < 3:
        parser.error("--reps must be >=3 (a percentile CI from fewer pairs is misleading)")
    for p in (a.before, a.after):
        if not p.is_file() or not os.access(p, os.X_OK):
            parser.error(f"not executable: {p}")
    for artifact in ("firmware/firmware.bin", "disk/disk.img"):
        if not Path(artifact).is_file():
            parser.error(f"missing shared input artifact: {artifact}")
    try:
        compare(a.before.resolve(), a.after.resolve(), a.reps)
    except ValueError as exc:
        print(f"paired bench: {exc}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
