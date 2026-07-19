"""Build a compact opponent deck prior from replay files."""

from __future__ import annotations

from collections import Counter
import argparse
import glob
import json
import os
import math


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--replays", default="data/kaggle_replays")
    parser.add_argument("--output",
                        default="sample_submission/sample_submission/deck_library.json")
    parser.add_argument("--files", type=int, default=1000)
    parser.add_argument("--decks", type=int, default=256)
    parser.add_argument(
        "--policy-output",
        default="sample_submission/sample_submission/policy_table.json",
    )
    args = parser.parse_args()

    paths = glob.glob(os.path.join(args.replays, "**", "*.json"), recursive=True)
    paths.sort(key=os.path.getsize)
    frequency: Counter[tuple[int, ...]] = Counter()
    selected_counts: Counter[str] = Counter()
    offered_counts: Counter[str] = Counter()
    for path in paths[:args.files]:
        try:
            with open(path, encoding="utf-8") as file:
                replay = json.load(file)
            if len(replay.get("steps", [])) < 2:
                continue
            for seat in replay["steps"][1]:
                action = seat.get("action")
                if isinstance(action, list) and len(action) == 60:
                    frequency[tuple(sorted(int(card) for card in action))] += 1
            collect_policy(replay, selected_counts, offered_counts)
        except (OSError, ValueError, TypeError):
            continue

    output = []
    for deck, count in frequency.most_common(args.decks):
        cards = Counter(deck)
        output.append({
            "count": count,
            "cards": {str(card): amount for card, amount in sorted(cards.items())},
        })
    with open(args.output, "w", encoding="utf-8") as file:
        json.dump(output, file, separators=(",", ":"))
    print(f"wrote {len(output)} deck archetypes to {args.output}")

    policy: dict[str, float] = {}
    for key, offered in offered_counts.items():
        selected = selected_counts[key]
        # Smoothed centered log odds. Clipping prevents a rare replay from
        # dominating the rules-based ordering.
        probability = (selected + 2.0) / (offered + 4.0)
        value = max(-2.5, min(2.5, math.log(probability / (1.0 - probability))))
        policy[key] = round(value, 5)
    with open(args.policy_output, "w", encoding="utf-8") as file:
        json.dump(policy, file, separators=(",", ":"))
    print(f"wrote {len(policy)} policy priors to {args.policy_output}")


def option_identity(observation: dict, option: dict) -> int | None:
    option_type = int(option.get("type", -1))
    if option_type == 13:
        return option.get("attackId")
    current = observation.get("current") or {}
    players = current.get("players") or []
    your_index = current.get("yourIndex", 0)
    if option_type in (7, 8, 9):
        try:
            hand = players[your_index].get("hand") or []
            return int(hand[int(option["index"])]["id"])
        except (IndexError, KeyError, TypeError, ValueError):
            return None
    if option_type in (3, 10):
        player_index = option.get("playerIndex", your_index)
        try:
            player = players[int(player_index)]
            area = int(option.get("area", -1))
            index = int(option.get("index", 0))
            if area == 1:
                deck = (observation.get("select") or {}).get("deck") or []
                return int(deck[index]["id"])
            if area == 2:
                return int((player.get("hand") or [])[index]["id"])
            if area == 3:
                return int(player["discard"][index]["id"])
            if area == 4:
                return int(player["active"][index]["id"])
            if area == 5:
                return int(player["bench"][index]["id"])
        except (IndexError, KeyError, TypeError, ValueError):
            return None
    return option.get("cardId")


def collect_policy(replay: dict, selected: Counter[str], offered: Counter[str]) -> None:
    for step in replay.get("steps", [])[2:]:
        for seat_index, seat in enumerate(step):
            rewards = replay.get("rewards") or []
            if seat_index >= len(rewards) or rewards[seat_index] != 1:
                continue
            observation = seat.get("observation") or {}
            select = observation.get("select")
            action = seat.get("action")
            if not select or not isinstance(action, list):
                continue
            context = int(select.get("context", -1))
            selected_indices = set(int(index) for index in action)
            for index, option in enumerate(select.get("option", [])):
                option_type = int(option.get("type", -1))
                identity = option_identity(observation, option)
                specific = f"{context}:{option_type}:{identity if identity is not None else -1}"
                generic = f"{context}:{option_type}:*"
                offered[specific] += 1
                offered[generic] += 1
                if index in selected_indices:
                    selected[specific] += 1
                    selected[generic] += 1


if __name__ == "__main__":
    main()
