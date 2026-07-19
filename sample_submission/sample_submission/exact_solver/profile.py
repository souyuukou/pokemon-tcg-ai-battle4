from __future__ import annotations
from collections import Counter
from dataclasses import dataclass
import hashlib
import json
import os
from pathlib import Path


DEFAULT_PROFILE = Path(__file__).resolve().parent / "decks" / "majkel1337-85795098.json"


@dataclass(frozen=True)
class DeckProfile:
    name: str
    cards: tuple[int, ...]
    evaluator: dict
    sha256: str

    def validate(self) -> None:
        if len(self.cards) != 60: raise ValueError(f"deck must contain 60 cards, got {len(self.cards)}")
        digest = hashlib.sha256(",".join(map(str, sorted(self.cards))).encode()).hexdigest()
        if digest != self.sha256: raise ValueError("deck profile hash mismatch")


def load_profile(path: str | os.PathLike | None = None) -> DeckProfile:
    path = Path(path or os.environ.get("PTCG_DECK_PROFILE", DEFAULT_PROFILE))
    raw = json.loads(path.read_text(encoding="utf-8"))
    cards = tuple(card_id for item in raw["cards"] for card_id in [int(item["id"])] * int(item["count"]))
    profile = DeckProfile(raw["name"], cards, raw["evaluator"], raw["canonical_sha256"])
    profile.validate(); return profile


def remaining_own_deck(profile: DeckProfile, known_cards) -> list[int]:
    remaining = Counter(profile.cards)
    for card in known_cards:
        card_id = getattr(card, "id", card)
        remaining[card_id] -= 1
        if remaining[card_id] < 0: raise ValueError(f"too many known copies of {card_id}")
    return list(remaining.elements())

