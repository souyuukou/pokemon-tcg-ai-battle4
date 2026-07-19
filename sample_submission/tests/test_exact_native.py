from __future__ import annotations

import os
import sys
import unittest

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", "sample_submission"))
sys.path.insert(0, ROOT)

from cg.api import exact_decide, to_observation_class
from cg.game import battle_finish, battle_select, battle_start
from exact_solver.profile import load_profile
from exact_solver import agent_policy
import main


class NativeExactSmokeTest(unittest.TestCase):
    def test_battle3_profile_is_default_and_native_api_is_connected(self) -> None:
        profile = load_profile()
        self.assertEqual(60, len(profile.cards))
        self.assertEqual(
            "3f4515092dc59df397f365a9b79c7cf0c1cb73b9aa38bc47c1b18e9df4c2fdaf",
            profile.sha256,
        )

        old_turn_ms = os.environ.get("PTCG_EXACT_TURN_MS")
        old_selection_ms = os.environ.get("PTCG_EXACT_SELECTION_MS")
        os.environ["PTCG_EXACT_TURN_MS"] = "1000"
        os.environ["PTCG_EXACT_SELECTION_MS"] = "200"
        raw, start = battle_start(list(profile.cards), list(profile.cards))
        self.assertEqual(-1, start.errorPlayer, start.errorType)
        try:
            # Progress through setup and one own-turn decision.  This reaches
            # the exported exact planner without supplying hidden opponent
            # identities, so clairvoyant determinization would fail here.
            reached_native_boundary = False
            for _ in range(64):
                obs = to_observation_class(raw)
                if obs.current.result >= 0:
                    break
                action = list(range(obs.select.minCount))
                raw = battle_select(action)
                obs_after = to_observation_class(raw)
                owner = agent_policy._turn_owner(obs_after.current)
                if obs_after.current.turn > 0 and owner == obs_after.current.yourIndex:
                    reached_native_boundary = True
                    native_action = main.agent(raw)
                    self.assertGreaterEqual(len(native_action), obs_after.select.minCount)
                    self.assertLessEqual(len(native_action), obs_after.select.maxCount)
                    self.assertEqual(len(native_action), len(set(native_action)))
                    self.assertTrue(all(0 <= i < len(obs_after.select.option) for i in native_action))
                    self.assertIsNotNone(agent_policy.last_decision)
                    self.assertTrue(agent_policy.last_decision["informationSetSafe"])
                    self.assertFalse(agent_policy.last_decision["hiddenInformationLeakDetected"])
                    self.assertGreater(agent_policy.last_decision["expandedNodes"], 0)
                    one_shot = exact_decide(obs_after, list(profile.cards),
                                           [100] * len(profile.cards), 100)
                    self.assertGreaterEqual(int(one_shot["rootQueueLeases"]), 1)
                    self.assertLessEqual(int(one_shot["maxConcurrentSearchThreads"]), 2)
                    break
                if obs_after.current.turn > 0:
                    # Consume the other side's decision without inspecting its
                    # hidden cards, then wait for our own turn.
                    raw = battle_select(list(range(obs_after.select.minCount)))
            self.assertTrue(reached_native_boundary)
        finally:
            battle_finish()
            if old_turn_ms is None:
                os.environ.pop("PTCG_EXACT_TURN_MS", None)
            else:
                os.environ["PTCG_EXACT_TURN_MS"] = old_turn_ms
            if old_selection_ms is None:
                os.environ.pop("PTCG_EXACT_SELECTION_MS", None)
            else:
                os.environ["PTCG_EXACT_SELECTION_MS"] = old_selection_ms


if __name__ == "__main__":
    unittest.main()
