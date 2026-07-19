from __future__ import annotations

import os
import sys
import unittest
from types import SimpleNamespace
from unittest.mock import patch

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", "sample_submission"))
sys.path.insert(0, ROOT)

import cg.api
from cg.api import AreaType, Card, Option, OptionType, SelectContext
from exact_solver import agent_policy


def _player(*, active=(), bench=(), hand=()):
    return SimpleNamespace(
        active=list(active),
        bench=list(bench),
        benchMax=5,
        hand=list(hand),
        handCount=len(hand),
        discard=[],
        prize=[None] * 6,
        deckCount=40,
        poisoned=False,
        burned=False,
        asleep=False,
        paralyzed=False,
        confused=False,
    )


def _pokemon(card_id: int):
    return SimpleNamespace(
        id=card_id,
        hp=140 if card_id == 66 else 70,
        maxHp=140 if card_id == 66 else 70,
        energies=[],
        energyCards=[],
        tools=[],
        preEvolution=[],
    )


def _observation(context, options, *, minimum=1, maximum=1,
                 active=(), bench=(), hand=(), deck=None, effect=None):
    player = _player(active=active, bench=bench, hand=hand)
    opponent = _player(active=[_pokemon(305)])
    select = SimpleNamespace(
        context=context,
        option=options,
        minCount=minimum,
        maxCount=maximum,
        deck=deck,
        effect=effect,
    )
    current = SimpleNamespace(yourIndex=0, players=[player, opponent])
    return SimpleNamespace(select=select, current=current)


class AgentPolicyFallbackTest(unittest.TestCase):
    def test_lone_active_dudunsparce_never_uses_run_away_draw(self) -> None:
        options = [
            Option(OptionType.ABILITY, area=AreaType.ACTIVE, index=0, playerIndex=0),
            Option(OptionType.END),
        ]
        obs = _observation(
            SelectContext.MAIN,
            options,
            active=[_pokemon(66)],
        )
        self.assertEqual([1], agent_policy._fallback_action(obs))

    def test_optional_bench_search_takes_visible_cards_instead_of_zero(self) -> None:
        deck = [Card(741, 1, 0), Card(305, 2, 0)]
        options = [
            Option(OptionType.CARD, area=AreaType.DECK, index=0, playerIndex=0),
            Option(OptionType.CARD, area=AreaType.DECK, index=1, playerIndex=0),
        ]
        obs = _observation(
            SelectContext.TO_BENCH,
            options,
            minimum=0,
            maximum=2,
            deck=deck,
        )
        self.assertEqual([0, 1], agent_policy._fallback_action(obs))

    def test_hilda_takes_visible_energy_despite_low_replay_frequency(self) -> None:
        deck = [Card(5, 1, 0), Card(19, 2, 0)]
        options = [
            Option(OptionType.CARD, area=AreaType.DECK, index=0, playerIndex=0),
            Option(OptionType.CARD, area=AreaType.DECK, index=1, playerIndex=0),
        ]
        obs = _observation(
            SelectContext.TO_HAND,
            options,
            minimum=0,
            maximum=1,
            deck=deck,
            effect=Card(1225, 3, 0),
        )
        self.assertEqual(1, len(agent_policy._fallback_action(obs)))

    def test_setup_prefers_dunsparce_over_two_prize_support(self) -> None:
        hand = [Card(140, 1, 0), Card(305, 2, 0)]
        options = [
            Option(OptionType.CARD, area=AreaType.HAND, index=0, playerIndex=0),
            Option(OptionType.CARD, area=AreaType.HAND, index=1, playerIndex=0),
        ]
        obs = _observation(
            SelectContext.SETUP_ACTIVE_POKEMON,
            options,
            hand=hand,
        )
        self.assertEqual([1], agent_policy._fallback_action(obs))

    def test_default_deck_chooses_to_go_second(self) -> None:
        options = [Option(OptionType.YES), Option(OptionType.NO)]
        obs = _observation(SelectContext.IS_FIRST, options)
        self.assertEqual([1], agent_policy._fallback_action(obs))

    def test_spent_turn_budget_reserves_a_reroot_selection_slice(self) -> None:
        context = agent_policy.PolicyContext()
        context.turn_search_seconds = 10.0
        old = os.environ.get("PTCG_EXACT_TURN_MS")
        os.environ["PTCG_EXACT_TURN_MS"] = "1"
        try:
            self.assertEqual(
                100,
                agent_policy._turn_slice_milliseconds(
                    context, False, 100, allow_reroot=True
                ),
            )
            with self.assertRaises(RuntimeError):
                agent_policy._turn_slice_milliseconds(
                    context, False, 100, allow_reroot=False
                )
        finally:
            if old is None:
                os.environ.pop("PTCG_EXACT_TURN_MS", None)
            else:
                os.environ["PTCG_EXACT_TURN_MS"] = old

    def test_unproven_native_root_action_is_not_replaced_by_fallback(self) -> None:
        hand = [Card(741, 1, 0)]
        options = [
            Option(OptionType.END),
            Option(OptionType.PLAY, index=0),
        ]
        obs = _observation(
            SelectContext.MAIN,
            options,
            active=[_pokemon(305)],
            hand=hand,
        )
        obs.current.turn = 1
        obs.current.firstPlayer = 0
        context = agent_policy.PolicyContext()
        native = {
            "selected": [0],
            "certified": False,
            "bestActionCertified": False,
            "memoryLimitReached": True,
            "sessionId": 99,
        }
        try:
            with patch.object(agent_policy, "_ensure_v3_evaluator"), \
                    patch.object(cg.api, "exact_turn_begin", return_value=native):
                action, certified, reason = agent_policy.choose_action(
                    obs, context=context
                )
            self.assertEqual([0], action)
            self.assertFalse(certified)
            self.assertEqual("native-exact-budget-exhausted", reason)
        finally:
            # The native begin call was mocked, so there is no real session to
            # release from the global engine registry.
            context.session_id = None


if __name__ == "__main__":
    unittest.main()
