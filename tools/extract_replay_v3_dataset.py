"""Replay complete matches into information-safe native V3 feature records.

Boundary and intermediate datasets intentionally use different capture points:
boundary samples are post-checkup turn leaves, while intermediate samples are
every non-setup decision state. A rejected replay contributes no prefix.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import sys
import zipfile
from dataclasses import dataclass
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
SUBMISSION = ROOT / "sample_submission" / "sample_submission"
sys.path.insert(0, str(SUBMISSION))

from cg.api import to_observation_class  # noqa: E402
from cg.game import (  # noqa: E402
    battle_finish,
    battle_select,
    battle_start_ordered,
    exact_replay_dual_trace_begin,
    exact_replay_intermediate_trace_begin,
    exact_replay_set_deck_order,
    exact_replay_set_all_hidden_zones,
    exact_replay_trace_begin,
    exact_replay_trace_drain,
    exact_replay_trace_end,
)


def card_ids(cards) -> list[int]:
    return [int(card["id"]) for card in cards or ()]


@dataclass(frozen=True)
class ReplaySource:
    container: Path
    member: str | None
    date: str
    replay_id: str

    def read_text(self) -> str:
        if self.member is None:
            return self.container.read_text(encoding="utf-8")
        archive = _zip_cache.get(self.container)
        if archive is None:
            archive = zipfile.ZipFile(self.container)
            _zip_cache[self.container] = archive
        with archive.open(self.member) as stream:
            return stream.read().decode("utf-8")


_zip_cache: dict[Path, zipfile.ZipFile] = {}


def replay_paths(source: Path, candidate_limit: int) -> list[ReplaySource]:
    paths: list[ReplaySource] = []
    for path in source.rglob("*"):
        if not path.is_file() or path.name.startswith("."):
            continue
        if any(part.startswith("_") for part in path.parts):
            continue
        if path.suffix.lower() == ".json":
            paths.append(ReplaySource(path, None, path.parent.name, path.stem))
        elif path.suffix.lower() == ".zip":
            try:
                with zipfile.ZipFile(path) as archive:
                    for member in archive.namelist():
                        member_path = Path(member)
                        if (member_path.suffix.lower() == ".json"
                                and not member_path.name.startswith(".")):
                            paths.append(ReplaySource(
                                path, member, path.parent.name, member_path.stem))
            except (OSError, zipfile.BadZipFile):
                continue
    zip_dates = {
        path.date for path in paths if path.container.suffix.lower() == ".zip"
    }
    if zip_dates:
        paths = [
            path for path in paths
            if path.date not in zip_dates
            or path.container.suffix.lower() == ".zip"
        ]
    # Stable chronological split is preserved by the path layout and replay id.
    paths.sort(key=lambda path: (
        path.date,
        int(path.replay_id) if path.replay_id.isdigit() else path.replay_id,
    ))
    if candidate_limit > 0 and len(paths) > candidate_limit:
        # Cover the entire available time span rather than taking only the oldest
        # agent population.
        indices = [
            round(index * (len(paths) - 1) / (candidate_limit - 1))
            for index in range(candidate_limit)
        ] if candidate_limit > 1 else [len(paths) - 1]
        paths = [paths[index] for index in indices]
    return paths


def extract_one(path: ReplaySource, mode: str) -> tuple[list[dict], str | None]:
    battle_open = False
    try:
        replay = json.loads(path.read_text())
        rewards = replay.get("rewards")
        if (not isinstance(rewards, list) or len(rewards) != 2
                or not all(isinstance(value, (int, float)) for value in rewards)):
            return [], "invalid_reward"
        frames = replay.get("steps", [[{}]])[0][0].get("visualize", [])
        if len(frames) < 2 or not frames[0].get("current"):
            return [], "missing_visual_trace"
        initial = frames[0]["current"]["players"]
        decks = [card_ids(initial[player].get("deck")) for player in range(2)]
        if any(len(deck) != 60 for deck in decks):
            return [], "invalid_deck"
        raw, start = battle_start_ordered(decks[0], decks[1], 1)
        battle_open = True
        if start.errorPlayer != -1:
            return [], f"start_{start.errorType}"
        if mode == "boundary":
            exact_replay_trace_begin()
        elif mode == "intermediate":
            exact_replay_intermediate_trace_begin()
        else:
            exact_replay_dual_trace_begin()

        # The first saved post-action frame contains the actual opening hands.
        # Draw() removes from the back and appends to Hand, so reconstruct the
        # exact pre-draw deck before applying the IsFirst selection. This also
        # reproduces which player has a Basic and therefore the setup stack.
        first_players = frames[1]["current"]["players"]
        for player in range(2):
            opening_hand = card_ids(first_players[player].get("hand"))
            opening_deck = card_ids(first_players[player].get("deck"))
            opening_prize = card_ids(first_players[player].get("prize"))
            if not opening_prize and len(opening_hand) + len(opening_deck) == 60:
                exact_replay_set_deck_order(
                    player, opening_deck + list(reversed(opening_hand)))

        # Frame zero is the deck/start snapshot. Every later frame records the
        # physical selection that produced that frame and the complete hidden
        # hand/deck state needed to reproduce subsequent random transitions.
        for frame in frames[1:]:
            selected = frame.get("selected")
            if not isinstance(selected, list):
                return [], "missing_selection"
            raw = battle_select([int(value) for value in selected])
            current = frame.get("current")
            if current is None:
                return [], "missing_frame_state"
            for player in range(2):
                physical = current["players"][player]
                exact_replay_set_all_hidden_zones(
                    player,
                    card_ids(physical.get("hand")),
                    card_ids(physical.get("deck")),
                    card_ids(physical.get("prize")),
                )

        observation = to_observation_class(raw)
        if observation.current.result < 0:
            return [], "nonterminal"
        samples = exact_replay_trace_drain()
        if not samples:
            return [], "empty_trace"
        replay_id = str(replay.get("id", path.replay_id))
        kind_counts = {
            kind: sum(sample.get("sampleKind") == kind for sample in samples)
            for kind in ("boundary", "intermediate")
        }
        for index, sample in enumerate(samples):
            actor = int(sample["actor"])
            sample["reward"] = float(rewards[actor])
            sample["matchWeight"] = 1.0 / max(
                1, kind_counts.get(sample.get("sampleKind"), 0))
            sample["replayId"] = replay_id
            sample["date"] = path.date
            sample["sampleIndex"] = index
        return samples, None
    except (IndexError, KeyError, OSError, RuntimeError, TypeError, ValueError) as error:
        return [], f"{type(error).__name__}:{error}"
    finally:
        if battle_open:
            try:
                exact_replay_trace_end()
            except Exception:
                pass
            try:
                battle_finish()
            except Exception:
                pass


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("source", type=Path)
    parser.add_argument("output", type=Path)
    parser.add_argument("--mode", choices=("boundary", "intermediate", "both"),
                        required=True)
    parser.add_argument("--limit", type=int, default=2000,
                        help="target number of fully accepted replays")
    parser.add_argument("--max-replays", type=int, default=15000,
                        help="maximum candidates examined to reach --limit")
    parser.add_argument("--progress-every", type=int, default=50)
    parser.add_argument("--date-from", default="",
                        help="inclusive YYYY-MM-DD source date filter")
    parser.add_argument("--date-to", default="",
                        help="inclusive YYYY-MM-DD source date filter")
    args = parser.parse_args()

    selected = replay_paths(args.source, args.max_replays)
    if args.date_from or args.date_to:
        selected = [
            path for path in selected
            if (not args.date_from or path.date >= args.date_from)
            and (not args.date_to or path.date <= args.date_to)
        ]
    args.output.parent.mkdir(parents=True, exist_ok=True)
    temporary = args.output.with_suffix(args.output.suffix + ".tmp")
    accepted = rejected = examples = 0
    reasons: dict[str, int] = {}
    digest = hashlib.sha256()
    with temporary.open("w", encoding="utf-8", newline="\n") as stream:
        for index, path in enumerate(selected, 1):
            samples, reason = extract_one(path, args.mode)
            if reason is not None:
                rejected += 1
                reasons[reason] = reasons.get(reason, 0) + 1
            else:
                accepted += 1
                examples += len(samples)
                for sample in samples:
                    line = json.dumps(sample, ensure_ascii=False, separators=(",", ":"))
                    stream.write(line + "\n")
                    digest.update((line + "\n").encode("utf-8"))
                if args.limit > 0 and accepted >= args.limit:
                    break
            if index % args.progress_every == 0:
                print(
                    f"{args.mode}: {index}/{len(selected)} "
                    f"accepted={accepted} rejected={rejected} examples={examples}",
                    flush=True,
                )
    os.replace(temporary, args.output)
    manifest = {
        "mode": args.mode,
        "source": str(args.source.resolve()),
        "candidateReplays": len(selected),
        "examinedReplays": accepted + rejected,
        "acceptedReplays": accepted,
        "rejectedReplays": rejected,
        "examples": examples,
        "sha256": digest.hexdigest(),
        "rejectionReasons": dict(sorted(reasons.items(), key=lambda item: (-item[1], item[0]))),
        "featureSchemaVersion": 3,
        "informationSetSafe": True,
        "boundaryOnly": args.mode == "boundary" if args.mode != "both" else None,
        "lossWeighting": "one_total_weight_per_match",
    }
    manifest_path = args.output.with_suffix(args.output.suffix + ".manifest.json")
    manifest_path.write_text(
        json.dumps(manifest, ensure_ascii=False, indent=2) + "\n",
        encoding="utf-8",
    )
    print(json.dumps(manifest, ensure_ascii=False, indent=2))


if __name__ == "__main__":
    main()
