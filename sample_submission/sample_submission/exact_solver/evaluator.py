from __future__ import annotations


def evaluate_observation(state, profile) -> int:
    """Small integer leaf evaluator; no floating point and no hidden identities."""
    me = state.yourIndex; opp = 1 - me
    mine, theirs = state.players[me], state.players[opp]
    if state.result >= 0:
        return (1 if state.result == me else -1 if state.result == opp else 0) * 100_000_000
    cfg = profile.evaluator
    value = 1_000_000 * (len(theirs.prize) - len(mine.prize))
    value += 2_000 * (len(mine.active) + len(mine.bench) - len(theirs.active) - len(theirs.bench))
    value += 500 * (_energy_count(mine) - _energy_count(theirs))
    value += _hand_value(mine.hand or (), cfg.get("hand_values", {}))
    value -= 80 * theirs.handCount
    value -= 2_000 * max(0, 4 - mine.deckCount) ** 2
    value += 100 * (_damage(theirs) - _damage(mine))
    return value


def _energy_count(player):
    return sum(len(p.energyCards) for p in player.active + player.bench if p is not None)


def _damage(player):
    return sum(max(0, p.maxHp - p.hp) for p in player.active + player.bench if p is not None)


def _hand_value(cards, values):
    return sum(int(values.get(str(card.id), values.get("default", 100))) for card in cards)

