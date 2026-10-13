#!/usr/bin/env python3
"""P5 deterministic correctness vectors; intentionally no wall-time assertions."""
import importlib.util
import os
import unittest
from pathlib import Path

MODULE = Path(os.environ.get("PERF_COMPARE_MODULE", Path(__file__).with_name("perf_compare.py")))
spec = importlib.util.spec_from_file_location("perf_compare", MODULE)
p = importlib.util.module_from_spec(spec)
spec.loader.exec_module(p)

SAMPLE = """[stats] vcpu=0 executed_steps=2 instr_count=1026 halted=1 fault=0
[stats] vcpu=1 executed_steps=0 instr_count=512 halted=1 fault=0
[stats] executed=2 idle_quantum=1024 instr_count_bsp=1026 vtime=1026
[stats] cap=50000000 cap_reached=0 n_vcpus=2
[stats] tlb_hits=5 tlb_misses=3 tlb_flushes=2
[stats] decode_hits=1 decode_misses=4
"""


class ComparatorVectors(unittest.TestCase):
    def test_paired_idle_not_cap(self):
        s = p.parse_stats(SAMPLE)
        self.assertEqual((s["executed"], s["vtime"], s["idle_quantum"]),
                         (2, 1026, 1024))
        p.check_pair(s, dict(s), "idle")

    def test_no_phantom_work_or_capped_sample(self):
        s = p.parse_stats(SAMPLE)
        with self.assertRaisesRegex(ValueError, "executed=0"):
            p.check_pair({**s, "executed": 0}, s, "zero")
        with self.assertRaisesRegex(ValueError, "hit --max-instr"):
            p.check_pair(s, {**s, "cap_reached": 1}, "cap")

    def test_missing_and_duplicate_global_stats(self):
        with self.assertRaisesRegex(ValueError, "missing stats"):
            p.parse_stats("[stats] executed=2\n")
        with self.assertRaisesRegex(ValueError, "duplicate stats key executed"):
            p.parse_stats(SAMPLE + "[stats] executed=2\n")

    def test_work_mismatch_rejected(self):
        # The one P5 NC disables precisely this guard in a scratch comparator.
        # Same cap and stdout-like line count cannot hide a changed K6 clock.
        s = p.parse_stats(SAMPLE)
        with self.assertRaisesRegex(ValueError, "WORK MISMATCH: .*vtime=1026/1027"):
            p.check_pair(s, {**s, "vtime": 1027}, "clock")
        with self.assertRaisesRegex(ValueError, "WORK MISMATCH: executed=2/3"):
            p.check_pair(s, {**s, "executed": 3}, "steps")

    def test_bootstrap_seed_and_no_timing_gate(self):
        pairs = [(200, 100)] * 9
        ratio, lo, hi = p.paired_interval(pairs)
        self.assertAlmostEqual(ratio, 2)
        self.assertAlmostEqual(lo, 2)
        self.assertAlmostEqual(hi, 2)
        self.assertEqual((ratio, lo, hi), p.paired_interval(pairs))
        with self.assertRaisesRegex(ValueError, ">=3 pairs"):
            p.paired_interval([(200, 100)])


if __name__ == "__main__":
    unittest.main(verbosity=2)
