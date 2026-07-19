from __future__ import annotations

import os
import sys
import unittest
from collections import Counter

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", "sample_submission"))
sys.path.insert(0, ROOT)

from cg.api import to_observation_class
from cg.game import battle_finish, battle_select, battle_start_seeded
from exact_solver import agent_policy
from exact_solver.profile import load_profile
import main


class WorstCaseFixtureTest(unittest.TestCase):
    def test_yamaguchi_draw_chain_fixture_stays_safe_and_bounded(self) -> None:
        deck = list(load_profile().cards)
        counts = Counter(deck)
        # The fixed profile contains the requested stress ingredients:
        # Dudunsparce x2, Kadabra/Alakazam evolution draw, Enriching Energy,
        # search/recovery effects, and a full six-card hidden prize pool.
        self.assertEqual(2, counts[66])
        self.assertGreaterEqual(counts[742], 2)
        self.assertGreaterEqual(counts[743], 2)
        self.assertEqual(1, counts[13])
        self.assertGreaterEqual(counts[1079], 3)

        old_turn = os.environ.get("PTCG_EXACT_TURN_MS")
        old_selection = os.environ.get("PTCG_EXACT_SELECTION_MS")
        os.environ["PTCG_EXACT_TURN_MS"] = "1000"
        os.environ["PTCG_EXACT_SELECTION_MS"] = "200"
        # This fixture starts directly from a seeded native battle rather than
        # passing through main.agent(None), so explicitly clear any session
        # retained by an earlier integration test.
        agent_policy._default_context.reset()
        agent_policy._last_turn = None
        agent_policy.last_decision = None
        raw, start = battle_start_seeded(deck, deck, 17)
        self.assertEqual(-1, start.errorPlayer, start.errorType)
        try:
            decision = None
            streaming_decision = None
            for _ in range(80):
                obs = to_observation_class(raw)
                if obs.current.result >= 0:
                    break
                raw = battle_select(list(range(obs.select.minCount)))
                obs = to_observation_class(raw)
                if (obs.current.turn > 0
                        and agent_policy._turn_owner(obs.current) == obs.current.yourIndex):
                    action = main.agent(raw)
                    decision = agent_policy.last_decision
                    self.assertTrue(action)
                    exact_diagnostic = decision.get("exactSearch", decision)
                    if int(exact_diagnostic.get("streamingCursorGenerated", 0)) > 0:
                        streaming_decision = exact_diagnostic
                        break
            self.assertIsNotNone(decision)
            self.assertIsNotNone(streaming_decision, "rich fixture never reached streaming draw path")
            decision = streaming_decision
            self.assertTrue(decision["informationSetSafe"])
            self.assertFalse(decision["hiddenInformationLeakDetected"])
            self.assertLess(int(decision["peakRssBytes"]), 2_700_000_000)
            self.assertGreater(int(decision["currentRssBytes"]), 0)
            self.assertGreaterEqual(int(decision["rootWorkers"]), 1)
            self.assertLessEqual(int(decision["rootWorkers"]), 2)
            self.assertGreaterEqual(int(decision["rootQueueLeases"]), 1)
            self.assertLessEqual(int(decision["maxConcurrentSearchThreads"]), 2)
            self.assertGreater(int(decision["streamingCursorGenerated"]), 0)
            self.assertGreater(int(decision["streamingCursorHits"]), 0)
            self.assertGreater(int(decision["streamingCursorPeakBytes"]), 0)
            self.assertEqual(0, int(decision["chanceMassMismatches"]))
            # The integrated native path must fail closed on every opaque or
            # interrupted transition encountered by this rich fixture.
            self.assertEqual(0, int(decision.get("opaqueNodes", 0)))
            self.assertEqual(0, int(decision.get("unsupportedConcreteReferenceNodes", 0)))
            self.assertEqual(0, int(decision.get("interruptedTransitionNodes", 0)))
            self.assertEqual(0, int(decision.get("chanceMassMismatches", 0)))
            # A time-sliced rich search may legitimately leave the argmax
            # interval open; the field is nevertheless required and is tested
            # for a positive proof in the deterministic boundary fixture.
            self.assertIn("bestActionCertified", decision)
            self.assertIn("exactValueCertified", decision)
        finally:
            battle_finish()
            if old_turn is None:
                os.environ.pop("PTCG_EXACT_TURN_MS", None)
            else:
                os.environ["PTCG_EXACT_TURN_MS"] = old_turn
            if old_selection is None:
                os.environ.pop("PTCG_EXACT_SELECTION_MS", None)
            else:
                os.environ["PTCG_EXACT_SELECTION_MS"] = old_selection


if __name__ == "__main__":
    unittest.main()
