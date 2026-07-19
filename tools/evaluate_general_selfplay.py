"""Run deterministic mirror self-play and emit compact decision traces."""

from __future__ import annotations

import argparse
import json
import os
import sys
import time
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
SUBMISSION = ROOT / "sample_submission" / "sample_submission"
sys.path.insert(0, str(SUBMISSION))

from cg.api import to_observation_class  # noqa: E402
from cg.game import battle_finish, battle_select, battle_start_seeded  # noqa: E402
from exact_solver.agent_policy import (  # noqa: E402
    PolicyContext,
    _fallback_action,
    choose_action,
)
from exact_solver.profile import load_profile  # noqa: E402

try:
    import psutil
except ImportError:  # pragma: no cover - optional local diagnostic
    psutil = None


def visible_card_id(obs, option):
    try:
        option_type = int(option.type)
        if option_type in (7, 8, 9):
            return int(obs.current.players[obs.current.yourIndex].hand[int(option.index)].id)
        if option_type == 13:
            return int(option.attackId)
        return None if option.cardId is None else int(option.cardId)
    except (AttributeError, IndexError, TypeError, ValueError):
        return None


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--seeds", default="17,23,41")
    parser.add_argument("--seed-count", type=int, default=0,
                        help="when positive, replace --seeds with 1..N")
    parser.add_argument("--max-decisions", type=int, default=1000)
    parser.add_argument("--work-threshold", type=int, default=1,
                        help="1 exercises the general policy on every own turn")
    parser.add_argument("--opponent", choices=("mirror", "heuristic"),
                        default="mirror")
    parser.add_argument("--both-seats", action="store_true",
                        help="with --opponent heuristic, play each seed twice")
    parser.add_argument("--output", type=Path)
    args = parser.parse_args()
    old_threshold = os.environ.get("PTCG_EXACT_WORK_THRESHOLD")
    os.environ["PTCG_EXACT_WORK_THRESHOLD"] = str(args.work_threshold)
    profile = load_profile()
    deck = list(profile.cards)
    games = []
    started = time.monotonic()
    process = psutil.Process() if psutil is not None else None
    peak_rss = process.memory_info().rss if process is not None else 0
    try:
        seeds = (list(range(1, args.seed_count + 1)) if args.seed_count > 0
                 else [int(value) for value in args.seeds.split(",") if value])
        matches = [
            (seed, general_player)
            for seed in seeds
            for general_player in (
                (0, 1) if args.opponent == "heuristic" and args.both_seats
                else ((0,) if args.opponent == "heuristic" else (None,))
            )
        ]
        for seed, general_player in matches:
            contexts = [PolicyContext(), PolicyContext()]
            raw, start = battle_start_seeded(deck, deck, seed)
            if start.errorPlayer != -1:
                raise RuntimeError(f"seed {seed} start error {start.errorType}")
            trace = []
            try:
                for decision_index in range(args.max_decisions):
                    obs = to_observation_class(raw)
                    if obs.current.result >= 0:
                        break
                    actor = int(obs.current.yourIndex)
                    if general_player is not None and actor != general_player:
                        action = _fallback_action(obs)
                        certified = False
                        reason = "legacy-heuristic"
                    else:
                        action, certified, reason = choose_action(
                            obs, context=contexts[actor])
                    trace.append({
                        "index": decision_index,
                        "turn": int(obs.current.turn),
                        "actor": actor,
                        "context": int(obs.select.context),
                        "reason": reason,
                        "certified": certified,
                        "handCount": int(obs.current.players[actor].handCount),
                        "ownPrizes": len(obs.current.players[actor].prize),
                        "opponentPrizes": len(obs.current.players[1 - actor].prize),
                        "active": [
                            None if card is None else int(card.id)
                            for card in obs.current.players[actor].active
                        ],
                        "bench": [
                            int(card.id)
                            for card in obs.current.players[actor].bench
                        ],
                        "availableAttacks": [
                            int(option.attackId)
                            for option in obs.select.option
                            if int(option.type) == 13
                            and option.attackId is not None
                        ],
                        "selected": [
                            {
                                "index": index,
                                "type": int(obs.select.option[index].type),
                                "cardId": visible_card_id(
                                    obs, obs.select.option[index]),
                            }
                            for index in action
                        ],
                    })
                    raw = battle_select(action)
                    if process is not None:
                        peak_rss = max(peak_rss, process.memory_info().rss)
                final = to_observation_class(raw)
                if final.current.result < 0:
                    raise RuntimeError(
                        f"seed {seed} did not finish in {args.max_decisions} decisions")
                games.append({
                    "seed": seed,
                    "generalPlayer": general_player,
                    "result": int(final.current.result),
                    "decisions": len(trace),
                    "generalDecisions": sum(
                        item["reason"].startswith("general") for item in trace),
                    "certifiedDecisions": sum(item["certified"] for item in trace),
                    "trace": trace,
                })
            finally:
                for context in contexts:
                    context.reset()
                battle_finish()
    finally:
        if old_threshold is None:
            os.environ.pop("PTCG_EXACT_WORK_THRESHOLD", None)
        else:
            os.environ["PTCG_EXACT_WORK_THRESHOLD"] = old_threshold
    report = {
        "deckSha256": profile.sha256,
        "workThreshold": args.work_threshold,
        "opponent": args.opponent,
        "bothSeats": args.both_seats,
        "elapsedSeconds": time.monotonic() - started,
        "peakRssBytes": peak_rss,
        "generalWins": sum(
            game["generalPlayer"] is not None
            and game["result"] == game["generalPlayer"]
            for game in games
        ),
        "generalLosses": sum(
            game["generalPlayer"] is not None
            and game["result"] != game["generalPlayer"]
            for game in games
        ),
        "games": games,
    }
    text = json.dumps(report, ensure_ascii=False, indent=2)
    if args.output:
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_text(text + "\n", encoding="utf-8")
    print(text)


if __name__ == "__main__":
    main()
