from __future__ import annotations

import os
import sys
import unittest
from pathlib import Path

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", "sample_submission"))
sys.path.insert(0, ROOT)

from cg.api import (
    exact_decide,
    exact_load_evaluator_model,
    exact_turn_advance,
    exact_turn_begin,
    exact_turn_progress,
    exact_turn_release,
    to_observation_class,
)
from cg.game import battle_finish, battle_select, battle_start_seeded
from exact_solver.profile import load_profile
from exact_solver import agent_policy
import main


class NativeExactSmokeTest(unittest.TestCase):
    def test_same_root_resume_retains_every_competing_action(self) -> None:
        profile = load_profile()
        deck = list(profile.cards)
        exact_load_evaluator_model(str(
            Path(ROOT, "exact-evaluator-v3.bin").resolve()
        ))
        raw, start = battle_start_seeded(deck, deck, 17)
        self.assertEqual(-1, start.errorPlayer, start.errorType)
        session_id = None
        try:
            target = None
            for _ in range(96):
                obs = to_observation_class(raw)
                if (obs.current.turn > 0
                        and agent_policy._turn_owner(obs.current) == obs.current.yourIndex
                        and len(obs.select.option) > 1):
                    target = obs
                    break
                raw = battle_select(list(range(obs.select.minCount)))
            self.assertIsNotNone(target)

            decision = exact_turn_begin(target, deck, [100] * 60, 1)
            session_id = int(decision["sessionId"])
            initial_leases = int(decision["rootQueueLeases"])
            maximum_leases = initial_leases
            for _ in range(10):
                if decision["exactValueCertified"]:
                    break
                decision = exact_turn_advance(session_id, target, 50)
                maximum_leases = max(
                    maximum_leases, int(decision["rootQueueLeases"])
                )
                self.assertEqual(
                    len(target.select.option), len(decision["rootActions"])
                )
            self.assertGreater(maximum_leases, initial_leases)
            self.assertTrue(decision["bestActionCertified"])
            self.assertTrue(decision["exactValueCertified"])
        finally:
            if session_id is not None:
                exact_turn_release(session_id)
            battle_finish()

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
        raw, start = battle_start_seeded(list(profile.cards), list(profile.cards), 17)
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
                    if agent_policy._default_context.session_id is not None:
                        progress = exact_turn_progress(agent_policy._default_context.session_id)
                        for key in ("actionValueCertified", "bestActionCertified",
                                    "exactValueCertified", "resumable", "rootWorkers"):
                            self.assertIn(key, progress)
                        self.assertLessEqual(int(progress["rootWorkers"]), 2)
                    one_shot = exact_decide(obs_after, list(profile.cards),
                                           [100] * len(profile.cards), 100)
                    self.assertGreaterEqual(int(one_shot["rootQueueLeases"]), 1)
                    self.assertLessEqual(int(one_shot["maxConcurrentSearchThreads"]), 2)
                    if len(obs_after.select.option) > 1:
                        # A deliberately tiny slice may leave root tasks
                        # untouched; that must never certify an action.
                        tiny = exact_decide(obs_after, list(profile.cards),
                                            [100] * len(profile.cards), 1)
                        self.assertFalse(tiny["bestActionCertified"])
                        self.assertGreaterEqual(
                            len(tiny["selected"]), obs_after.select.minCount
                        )
                        self.assertLessEqual(
                            len(tiny["selected"]), obs_after.select.maxCount
                        )
                        self.assertEqual(
                            len(tiny["selected"]), len(set(tiny["selected"]))
                        )
                        self.assertTrue(all(
                            0 <= int(index) < len(obs_after.select.option)
                            for index in tiny["selected"]
                        ))
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

    def test_single_legal_boundary_proves_best_action(self) -> None:
        profile = load_profile()
        raw, start = battle_start_seeded(list(profile.cards), list(profile.cards), 17)
        self.assertEqual(-1, start.errorPlayer, start.errorType)
        try:
            for _ in range(96):
                obs = to_observation_class(raw)
                if obs.current.result >= 0:
                    break
                raw = battle_select(list(range(obs.select.minCount)))
                obs = to_observation_class(raw)
                if (obs.current.turn > 0
                        and agent_policy._turn_owner(obs.current) == obs.current.yourIndex):
                    if len(obs.select.option) == 1:
                        decision = exact_decide(obs, list(profile.cards), [100] * len(profile.cards), 300)
                        self.assertTrue(decision["bestActionCertified"])
                        self.assertTrue(decision["exactValueCertified"])
                        return
                    raw = battle_select(list(range(obs.select.minCount)))
            self.fail("did not reach a one-action boundary fixture")
        finally:
            battle_finish()


if __name__ == "__main__":
    unittest.main()
