"""Collision-safe, size-bounded transposition table."""

from __future__ import annotations
from dataclasses import dataclass
from enum import Enum
from fractions import Fraction
from typing import Any
from .canonical import CanonicalKey, KeyArena


class BoundKind(Enum):
    EXACT = "exact"; LOWER = "lower"; UPPER = "upper"; INTERVAL = "interval"


@dataclass(slots=True)
class TTEntry:
    key: CanonicalKey
    node_kind: str
    lower: Fraction
    upper: Fraction
    best_action: Any = None
    certified: bool = True

    @property
    def bound(self):
        if self.lower == self.upper: return BoundKind.EXACT
        return BoundKind.INTERVAL


class TranspositionTable:
    def __init__(self, arena: KeyArena, max_entries: int = 1_000_000):
        self.arena, self.max_entries = arena, max_entries
        self._buckets: dict[tuple[int, int, str], list[TTEntry]] = {}
        self._fifo: list[TTEntry] = []

    def get(self, key: CanonicalKey, node_kind: str) -> TTEntry | None:
        for entry in self._buckets.get((key.lo, key.hi, node_kind), ()):
            if self.arena.equal(entry.key, key): return entry
        return None

    def put(self, entry: TTEntry) -> None:
        existing = self.get(entry.key, entry.node_kind)
        if existing is not None:
            existing.lower, existing.upper = entry.lower, entry.upper
            existing.best_action, existing.certified = entry.best_action, entry.certified
            return
        if len(self._fifo) >= self.max_entries: self._evict(self._fifo.pop(0))
        self._fifo.append(entry)
        self._buckets.setdefault((entry.key.lo, entry.key.hi, entry.node_kind), []).append(entry)

    def _evict(self, entry: TTEntry) -> None:
        bucket_key = (entry.key.lo, entry.key.hi, entry.node_kind)
        bucket = self._buckets[bucket_key]; bucket.remove(entry)
        if not bucket: del self._buckets[bucket_key]

    def __len__(self): return len(self._fifo)

