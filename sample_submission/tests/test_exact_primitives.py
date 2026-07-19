from __future__ import annotations

import os
import sys
import unittest
from fractions import Fraction
from unittest.mock import patch

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", "sample_submission"))
sys.path.insert(0, ROOT)

from exact_solver.canonical import KeyArena, observation_key
from exact_solver.chance import exact_draw_outcomes
from exact_solver.resources import current_rss_bytes, peak_rss_bytes


class ExactPrimitiveTest(unittest.TestCase):
    def test_observation_key_ignores_logs_and_physical_serials(self) -> None:
        namespace = {"rules": 1}
        first = {"logs": [1], "current": {"hand": [{"id": 5, "serial": 91}]}}
        second = {"logs": [999], "current": {"hand": [{"id": 5, "serial": 7}]}}
        self.assertEqual(observation_key(first, namespace), observation_key(second, namespace))

    def test_digest_collision_still_compares_full_key(self) -> None:
        arena = KeyArena(hash_fn=lambda _: (0, 0))
        first, second = arena.intern(b"a"), arena.intern(b"b")
        self.assertFalse(arena.equal(first, second))

    def test_multiset_draw_probability_is_exact(self) -> None:
        outcomes = list(exact_draw_outcomes(["a"] * 4 + ["b"] * 2, 3))
        self.assertEqual(20, sum(item.weight for item in outcomes))
        self.assertEqual(
            {
                (("a", 1), ("b", 2)): 4,
                (("a", 2), ("b", 1)): 12,
                (("a", 3),): 4,
            },
            {item.value: item.weight for item in outcomes},
        )
        self.assertEqual(Fraction(1), sum((item.probability for item in outcomes), Fraction()))

    def test_linux_rss_uses_current_statm_not_peak_rss(self) -> None:
        if os.name != "posix":
            self.skipTest("Linux/procfs-specific assertion")
        with patch("exact_solver.resources.Path.read_text", return_value="100 7 0 0"), \
             patch("exact_solver.resources.os.sysconf", return_value=4096):
            self.assertEqual(7 * 4096, current_rss_bytes())
        self.assertGreaterEqual(peak_rss_bytes(), 0)


if __name__ == "__main__":
    unittest.main()
