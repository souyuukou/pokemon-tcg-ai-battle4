from __future__ import annotations

import os
import sys
import unittest

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", "sample_submission"))
sys.path.insert(0, ROOT)

from cg.api import exact_decide, to_observation_class
from cg.game import battle_finish, battle_select, battle_start_seeded
from exact_solver import agent_policy
from exact_solver.profile import load_profile


class V4IdentityOracleGateTest(unittest.TestCase):
    def test_passive_draw_env_is_fail_closed_before_identity_oracle_certification(self) -> None:
        old_flag = os.environ.get("PTCG_EXACT_V4_PASSIVE_DRAW")
        os.environ["PTCG_EXACT_V4_PASSIVE_DRAW"] = "1"
        profile = load_profile()
        raw, start = battle_start_seeded(list(profile.cards), list(profile.cards), 17)
        self.assertEqual(-1, start.errorPlayer, start.errorType)
        reached = False
        try:
            for _ in range(80):
                obs = to_observation_class(raw)
                if obs.current.result >= 0:
                    break
                raw = battle_select(list(range(obs.select.minCount)))
                obs = to_observation_class(raw)
                if (obs.current.turn > 0
                        and agent_policy._turn_owner(obs.current) == obs.current.yourIndex):
                    decision = exact_decide(obs, list(profile.cards), [100] * len(profile.cards), 100)
                    self.assertTrue(decision["v4PassiveDrawExperimental"])
                    self.assertFalse(decision["actionValueCertified"])
                    self.assertFalse(decision["bestActionCertified"])
                    self.assertFalse(decision["exactValueCertified"])
                    reached = True
                    break
                if obs.current.turn > 0:
                    raw = battle_select(list(range(obs.select.minCount)))
            self.assertTrue(reached, "did not reach an own-turn boundary")
        finally:
            battle_finish()
            if old_flag is None:
                os.environ.pop("PTCG_EXACT_V4_PASSIVE_DRAW", None)
            else:
                os.environ["PTCG_EXACT_V4_PASSIVE_DRAW"] = old_flag


if __name__ == "__main__":
    unittest.main()
