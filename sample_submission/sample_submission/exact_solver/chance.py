"""Exact combinatorial chance expansion without floating point or sampling."""

from __future__ import annotations
from collections import Counter
from dataclasses import dataclass
from fractions import Fraction
from math import comb, gcd
from typing import Hashable, Iterable, Iterator


@dataclass(frozen=True, slots=True)
class WeightedOutcome:
    value: tuple[tuple[Hashable, int], ...]
    weight: int
    total: int

    @property
    def probability(self) -> Fraction:
        return Fraction(self.weight, self.total)


def exact_draw_outcomes(population: Iterable[Hashable], count: int) -> Iterator[WeightedOutcome]:
    """Yield unordered multiset draws with hypergeometric integer weights."""
    counts = sorted(Counter(population).items(), key=lambda item: repr(item[0]))
    total_cards = sum(n for _, n in counts)
    if count < 0 or count > total_cards: raise ValueError("invalid draw count")
    total = comb(total_cards, count)
    chosen: list[tuple[Hashable, int]] = []
    def rec(index: int, left: int, weight: int):
        if index == len(counts):
            if left == 0: yield WeightedOutcome(tuple(chosen), weight, total)
            return
        value, available = counts[index]
        for take in range(min(available, left) + 1):
            if take: chosen.append((value, take))
            yield from rec(index + 1, left - take, weight * comb(available, take))
            if take: chosen.pop()
    yield from rec(0, count, 1)


def normalize_integer_weights(weighted: Iterable[tuple[object, int]]) -> tuple[tuple[object, int], ...]:
    values = [(v, w) for v, w in weighted if w > 0]
    divisor = 0
    for _, weight in values: divisor = gcd(divisor, weight)
    return tuple((v, w // divisor) for v, w in values) if divisor else ()

