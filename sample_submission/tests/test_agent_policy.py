from __future__ import annotations

import os
import sys
import tempfile
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
    def test_second_call_survives_agent_directory_leaving_import_path(self) -> None:
        options = [Option(OptionType.YES), Option(OptionType.NO)]
        obs = _observation(SelectContext.IS_FIRST, options)
        original_cwd = os.getcwd()
        original_path = list(sys.path)
        try:
            with tempfile.TemporaryDirectory() as temporary_directory:
                with patch.dict(sys.modules, {"battle_ai": None}):
                    sys.path[:] = [
                        path for path in sys.path
                        if os.path.abspath(path or original_cwd) != ROOT
                    ]
                    try:
                        os.chdir(temporary_directory)
                        self.assertEqual([1], agent_policy._fallback_action(obs))
                    finally:
                        os.chdir(original_cwd)
        finally:
            sys.path[:] = original_path

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

    def test_unproven_native_root_action_uses_general_time_fallback(self) -> None:
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
                    patch.object(agent_policy, "_ensure_general_evaluator"), \
                    patch.object(cg.api, "exact_estimate_root",
                                 return_value={"recommendedGeneral": False}), \
                    patch.object(cg.api, "exact_turn_begin", return_value=native), \
                    patch.object(agent_policy, "_general_action",
                                 return_value=([1], {
                                     "selected": [1],
                                     "informationSetSafe": True,
                                 })):
                action, certified, reason = agent_policy.choose_action(
                    obs, context=context
                )
            self.assertEqual([1], action)
            self.assertFalse(certified)
            self.assertEqual("general-exact-time-exhausted", reason)
        finally:
            # The native begin call was mocked, so there is no real session to
            # release from the global engine registry.
            context.session_id = None

    def test_predicted_explosion_uses_general_one_step_value(self) -> None:
        options = [Option(OptionType.END), Option(OptionType.PLAY, index=0)]
        obs = _observation(
            SelectContext.MAIN, options,
            active=[_pokemon(305)], hand=[Card(741, 1, 0)])
        obs.current.turn = 1
        obs.current.firstPlayer = 0
        obs.search_begin_input = "opaque"
        context = agent_policy.PolicyContext()
        estimate = {
            "recommendedGeneral": True,
            "totalEstimatedWork": 1_000_001,
        }
        general = {
            "selected": [1],
            "informationSetSafe": True,
            "candidateSetComplete": True,
        }
        with patch.object(agent_policy, "_ensure_v3_evaluator"), \
                patch.object(agent_policy, "_ensure_general_evaluator"), \
                patch.object(agent_policy, "_profile_inputs",
                             return_value=(SimpleNamespace(cards=[741] * 60), [100] * 60)), \
                patch.object(cg.api, "exact_estimate_root", return_value=estimate), \
                patch.object(agent_policy, "_general_action",
                             return_value=([1], general)), \
                patch.object(cg.api, "exact_turn_begin") as exact_begin:
            action, certified, reason = agent_policy.choose_action(
                obs, context=context)
        self.assertEqual([1], action)
        self.assertFalse(certified)
        self.assertEqual("general-predicted-explosion", reason)
        self.assertIs(general, context.last_decision)
        exact_begin.assert_not_called()

    def test_opponent_turn_selection_uses_general_value(self) -> None:
        obs = _observation(
            SelectContext.TO_HAND,
            [Option(OptionType.CARD, area=AreaType.DISCARD, index=0)],
            active=[_pokemon(305)])
        obs.current.turn = 2
        obs.current.firstPlayer = 0
        obs.current.yourIndex = 0
        general = {"selected": [0], "informationSetSafe": True}
        with patch.object(agent_policy, "_general_action",
                          return_value=([0], general)):
            action, certified, reason = agent_policy.choose_action(
                obs, context=agent_policy.PolicyContext())
        self.assertEqual([0], action)
        self.assertFalse(certified)
        self.assertEqual("general-opponent-turn-selection", reason)

    def test_general_value_blends_observation_safe_tactical_ranking(self) -> None:
        options = [
            Option(OptionType.END),
            Option(OptionType.ATTACK, index=0),
        ]
        obs = _observation(
            SelectContext.MAIN, options, active=[_pokemon(305)])
        native = {
            "selected": [0],
            "informationSetSafe": True,
            "actions": [
                {"selected": [0], "value": 10_000_000},
                {"selected": [1], "value": 9_000_000},
            ],
        }
        with patch.object(agent_policy, "_ensure_general_evaluator"), \
                patch.object(agent_policy, "_profile_inputs",
                             return_value=(SimpleNamespace(cards=[741] * 60),
                                           [100] * 60)), \
                patch.object(cg.api, "exact_general_decide",
                             return_value=native), \
                patch.object(agent_policy, "_fallback_score",
                             side_effect=[0.0, 10.0]), \
                patch.dict(os.environ,
                           {"PTCG_GENERAL_TACTICAL_BLEND": "250000"}):
            action, decision = agent_policy._general_action(obs)
        self.assertEqual([1], action)
        self.assertEqual(9_000_000, decision["networkValue"])
        self.assertEqual(10.0, decision["tacticalValue"])
        self.assertEqual(11_500_000, decision["combinedValue"])


if __name__ == "__main__":
    unittest.main()
