"""Generic exact expectimax over a transition adapter."""

from __future__ import annotations
from dataclasses import dataclass
from fractions import Fraction
import time
from typing import Any, Protocol, Sequence

from .canonical import KeyArena, KeyMemoryLimit, canonical_bytes
from .transposition import TTEntry, TranspositionTable


class TransitionModel(Protocol):
    def node_kind(self, state: Any) -> str: ...  # leaf, max, chance, forced
    def canonical_state(self, state: Any) -> bytes: ...
    def evaluate(self, state: Any) -> int | Fraction: ...
    def actions(self, state: Any) -> Sequence[Any]: ...
    def step(self, state: Any, action: Any) -> Any: ...
    def outcomes(self, state: Any) -> Sequence[tuple[Any, int]]: ...


@dataclass(frozen=True, slots=True)
class SearchResult:
    lower: Fraction
    upper: Fraction
    action: Any = None
    certified: bool = True

    @property
    def value(self) -> Fraction:
        if self.lower != self.upper: raise ValueError("result is an interval")
        return self.lower


class ExactSolver:
    """Exact max/chance propagation; interruption produces explicit intervals."""
    def __init__(self, model: TransitionModel, *, namespace: dict[str, Any],
                 deadline: float | None = None, value_floor: int = -(10**18),
                 value_ceiling: int = 10**18, max_entries: int = 250_000,
                 arena: KeyArena | None = None):
        self.model, self.deadline = model, deadline
        self.floor, self.ceiling = Fraction(value_floor), Fraction(value_ceiling)
        self.arena = arena or KeyArena()
        self.namespace = canonical_bytes(namespace)
        self.tt = TranspositionTable(self.arena, max_entries=max_entries)
        self.expanded = self.merged = 0

    def solve(self, state: Any) -> SearchResult:
        return self._solve(state, set())

    def _solve(self, state: Any, path: set[int]) -> SearchResult:
        if self.deadline is not None and time.monotonic() >= self.deadline:
            return SearchResult(self.floor, self.ceiling, certified=False)
        kind = self.model.node_kind(state)
        raw = canonical_bytes((self.namespace, kind, self.model.canonical_state(state)))
        try:
            key = self.arena.intern(raw)
        except KeyMemoryLimit:
            return SearchResult(self.floor, self.ceiling, certified=False)
        cached = self.tt.get(key, kind)
        if cached:
            self.merged += 1
            return SearchResult(cached.lower, cached.upper, cached.best_action, cached.certified)
        # Full-key identity is represented by key_id; cycles are conservatively bounded.
        if key.key_id in path:
            return SearchResult(self.floor, self.ceiling, certified=False)
        path.add(key.key_id); self.expanded += 1
        try:
            if kind == "leaf":
                value = Fraction(self.model.evaluate(state)); result = SearchResult(value, value)
            elif kind == "forced":
                actions = self.model.actions(state)
                result = self._solve(self.model.step(state, actions[0]), path)
            elif kind == "max":
                result = self._max(state, path)
            elif kind == "chance":
                result = self._chance(state, path)
            else:
                raise ValueError(f"unknown node kind {kind!r}; fail-closed")
        finally:
            path.remove(key.key_id)
        self.tt.put(TTEntry(key, kind, result.lower, result.upper, result.action, result.certified))
        return result

    def _max(self, state: Any, path: set[int]) -> SearchResult:
        best: SearchResult | None = None
        upper = self.floor
        certified = True
        for action in self.model.actions(state):
            child = self._solve(self.model.step(state, action), path)
            candidate = SearchResult(child.lower, child.upper, action, child.certified)
            upper = max(upper, child.upper)
            certified &= child.certified
            if best is None or candidate.lower > best.lower or (candidate.lower == best.lower and repr(action) < repr(best.action)):
                best = candidate
        if best is None: raise ValueError("max node has no actions")
        return SearchResult(best.lower, upper, best.action, certified and best.lower == upper)

    def _chance(self, state: Any, path: set[int]) -> SearchResult:
        outcomes = self.model.outcomes(state)
        if any(weight < 0 for _, weight in outcomes): raise ValueError("negative chance weight")
        total = sum(weight for _, weight in outcomes)
        if total <= 0: raise ValueError("chance node has no positive weight")
        low = high = Fraction(0); certified = True
        # Equal canonical successors are merged before recursion, preserving integer mass.
        grouped: dict[tuple[str, bytes], tuple[Any, int]] = {}
        for child_state, weight in outcomes:
            if weight <= 0: continue
            group_key = (self.model.node_kind(child_state), self.model.canonical_state(child_state))
            prior = grouped.get(group_key)
            grouped[group_key] = (child_state, weight + (prior[1] if prior else 0))
        for child_state, weight in grouped.values():
            child = self._solve(child_state, path)
            low += child.lower * weight; high += child.upper * weight
            certified &= child.certified
        return SearchResult(low / total, high / total, certified=certified)
