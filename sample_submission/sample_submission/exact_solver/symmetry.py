"""Proven-safe action and outcome symmetry reductions."""

from __future__ import annotations
from collections import defaultdict
from dataclasses import dataclass
from itertools import combinations
from typing import Callable, Hashable, Iterable, Sequence


@dataclass(frozen=True, slots=True)
class ActionClass:
    representative: tuple[int, ...]
    members: tuple[tuple[int, ...], ...]


def selections(option_count: int, minimum: int, maximum: int):
    for count in range(minimum, maximum + 1):
        yield from combinations(range(option_count), count)


def quotient_actions(actions: Iterable[tuple[int, ...]], semantic_key: Callable[[tuple[int, ...]], Hashable]) -> list[ActionClass]:
    groups: dict[Hashable, list[tuple[int, ...]]] = defaultdict(list)
    for action in actions: groups[semantic_key(action)].append(action)
    return [ActionClass(min(v), tuple(sorted(v))) for v in groups.values()]


def facedown_prize_actions(option_count: int, take: int, *, prizes_exchangeable: bool,
                           known_indices: Sequence[int] = (), lucky_bonus_possible: bool = False):
    """Collapse prize indices only under the audited face-down/exchangeable rule."""
    all_actions = list(combinations(range(option_count), take))
    if not prizes_exchangeable or known_indices or lucky_bonus_possible or not all_actions:
        return [ActionClass(a, (a,)) for a in all_actions]
    return [ActionClass(all_actions[0], tuple(all_actions))]


SAFE_CONTEXTS = frozenset({"TO_HAND", "DISCARD_ENERGY", "DAMAGE_COUNTER", "EVOLVE", "BENCH"})


def context_is_audited(context: object) -> bool:
    name = getattr(context, "name", str(context)).upper()
    return name in SAFE_CONTEXTS


def bounded_count_vectors(capacities: Sequence[int], minimum: int, maximum: int):
    """Exact unordered selections of identical classes (energy/hand copies)."""
    if minimum < 0 or maximum < minimum: raise ValueError("invalid count range")
    vector = [0] * len(capacities)
    def rec(index: int, total: int):
        if index == len(capacities):
            if minimum <= total <= maximum: yield tuple(vector)
            return
        for count in range(min(capacities[index], maximum - total) + 1):
            vector[index] = count
            yield from rec(index + 1, total + count)
        vector[index] = 0
    yield from rec(0, 0)


def orbit_allocations(size: int, units: int, per_member_cap: int):
    """Integer partitions across exchangeable Pokémon, canonicalized by sorting."""
    if size < 0 or units < 0 or per_member_cap < 0: raise ValueError("negative allocation")
    current: list[int] = []
    def rec(left_members: int, left_units: int, ceiling: int):
        if left_members == 0:
            if left_units == 0: yield tuple(current)
            return
        for value in range(min(ceiling, per_member_cap, left_units), -1, -1):
            current.append(value)
            yield from rec(left_members - 1, left_units - value, value)
            current.pop()
    yield from rec(size, units, per_member_cap)

