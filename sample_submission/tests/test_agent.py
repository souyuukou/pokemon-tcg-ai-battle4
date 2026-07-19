from __future__ import annotations

import os
import random
import sys
import unittest

ROOT = os.path.abspath(
    os.path.join(os.path.dirname(__file__), "..", "sample_submission")
)
sys.path.insert(0, ROOT)

from battle_ai import BoundarySearchAgent, legal_selections, read_deck
from cg.api import SelectContext, to_observation_class
from cg.game import battle_finish, battle_select, battle_start


class AgentIntegrationTest(unittest.TestCase):
    def test_deck_is_valid_and_agent_completes_a_game(self) -> None:
        deck = read_deck()
        self.assertEqual(60, len(deck))
        agent = BoundarySearchAgent(deck)
        opponent_policy = BoundarySearchAgent(deck)
        raw, start = battle_start(deck, deck)
        self.assertIsNotNone(raw, (start.errorPlayer, start.errorType))
        try:
            for _ in range(300):
                observation = to_observation_class(raw)
                if observation.current.result >= 0:
                    return
                select = observation.select
                if observation.current.yourIndex == 0:
                    action = agent.choose(
                        observation, {**raw, "remainingOverageTime": 40.0}
                    )
                else:
                    candidates = legal_selections(
                        observation,
                        opponent_policy.cards,
                        observation.current.yourIndex,
                        cap=8,
                    )
                    action = candidates[0]
                self.assertGreaterEqual(len(action), select.minCount)
                self.assertLessEqual(len(action), select.maxCount)
                self.assertEqual(len(action), len(set(action)))
                self.assertTrue(all(0 <= value < len(select.option) for value in action))

                if select.context == SelectContext.MAIN:
                    determinization = agent.belief.sample(observation, 0)
                    me = observation.current.yourIndex
                    enemy = 1 - me
                    if select.deck is None:
                        self.assertEqual(
                            observation.current.players[me].deckCount,
                            len(determinization.own_deck),
                        )
                    self.assertEqual(
                        len(observation.current.players[me].prize),
                        len(determinization.own_prize),
                    )
                    self.assertEqual(
                        observation.current.players[enemy].deckCount,
                        len(determinization.opponent_deck),
                    )
                    self.assertEqual(
                        observation.current.players[enemy].handCount,
                        len(determinization.opponent_hand),
                    )
                    self.assertEqual(
                        len(observation.current.players[enemy].prize),
                        len(determinization.opponent_prize),
                    )
                raw = battle_select(action)
            self.fail("game did not finish within 300 selections")
        finally:
            battle_finish()


if __name__ == "__main__":
    unittest.main()
