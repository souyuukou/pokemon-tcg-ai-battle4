"""Resource-bounded boundary search agent.

The simulator is used as the rules oracle. Hidden zones are sampled from card
count beliefs, while every simulated line is advanced to the opponent's next
decision boundary before it is evaluated.
"""

from __future__ import annotations

from collections import Counter
from dataclasses import dataclass
import hashlib
import json
import math
import os
import random
import time
from typing import Iterable

from cg.api import (
    AreaType,
    CardData,
    CardType,
    Observation,
    Option,
    OptionType,
    SelectContext,
    all_attack,
    all_card_data,
    search_begin,
    search_end,
    search_step,
)


WIN_SCORE = 1_000_000.0
MAX_SEARCH_STATES = 1_200
MAX_LINE_DEPTH = 36


def _asset_path(name: str) -> str:
    local = os.path.join(os.path.dirname(os.path.abspath(__file__)), name)
    if os.path.exists(local):
        return local
    return os.path.join("/kaggle_simulations/agent", name)


def read_deck() -> list[int]:
    with open(_asset_path("deck.csv"), encoding="utf-8") as file:
        values = [line.strip() for line in file if line.strip()]
    if len(values) < 60:
        raise ValueError("deck.csv must contain 60 card IDs")
    return [int(value) for value in values[:60]]


class CardDatabase:
    def __init__(self) -> None:
        self.cards: dict[int, CardData] = {card.cardId: card for card in all_card_data()}
        self.attacks = {attack.attackId: attack for attack in all_attack()}
        self.policy: dict[str, float] = {}
        try:
            with open(_asset_path("policy_table.json"), encoding="utf-8") as file:
                self.policy = {
                    str(key): float(value) for key, value in json.load(file).items()
                }
        except (OSError, ValueError, TypeError):
            pass

    def card(self, card_id: int | None) -> CardData | None:
        return self.cards.get(card_id) if card_id is not None else None

    def card_value(self, card_id: int | None) -> float:
        card = self.card(card_id)
        if card is None:
            return 0.0
        if card.cardType == CardType.POKEMON:
            value = 18.0 + card.hp * 0.11 + 5.0 * len(card.attacks)
            if card.basic:
                value += 6.0
            if card.stage1:
                value += 8.0
            if card.stage2:
                value += 13.0
            if card.ex:
                value += 14.0
            if card.megaEx:
                value += 8.0
            value += 4.0 * len(card.skills)
            return value
        if card.cardType == CardType.SUPPORTER:
            return 31.0
        if card.cardType == CardType.ITEM:
            return 24.0
        if card.cardType == CardType.TOOL:
            return 20.0
        if card.cardType == CardType.STADIUM:
            return 17.0
        if card.cardType == CardType.SPECIAL_ENERGY:
            return 19.0
        if card.cardType == CardType.BASIC_ENERGY:
            return 13.0
        return 5.0

    def attack_damage(self, attack_id: int | None) -> float:
        attack = self.attacks.get(attack_id)
        if attack is None:
            return 0.0
        # Printed zero-damage attacks are often effect attacks and should not
        # be treated like passing.
        return float(attack.damage if attack.damage > 0 else 22)

    def policy_score(self, context: int, option_type: int,
                     identity: int | None) -> float:
        specific = f"{int(context)}:{int(option_type)}:{identity if identity is not None else -1}"
        generic = f"{int(context)}:{int(option_type)}:*"
        return self.policy.get(specific, self.policy.get(generic, 0.0))


class DeckLibrary:
    def __init__(self, path: str) -> None:
        self.entries: list[tuple[int, Counter[int]]] = []
        try:
            with open(path, encoding="utf-8") as file:
                raw = json.load(file)
            for entry in raw:
                self.entries.append(
                    (int(entry.get("count", 1)),
                     Counter({int(k): int(v) for k, v in entry["cards"].items()}))
                )
        except (OSError, ValueError, KeyError, TypeError):
            self.entries = []

    def candidates(self, public: Counter[int], limit: int = 12) -> list[Counter[int]]:
        ranked: list[tuple[float, Counter[int]]] = []
        for frequency, deck in self.entries:
            missing = sum(max(0, count - deck[card]) for card, count in public.items())
            overlap = sum(min(count, deck[card]) for card, count in public.items())
            score = overlap * 30.0 - missing * 100.0 + math.log1p(frequency)
            ranked.append((score, deck))
        ranked.sort(key=lambda item: item[0], reverse=True)
        return [deck.copy() for _, deck in ranked[:limit]]


def _iter_pokemon_cards(pokemon) -> Iterable[int]:
    if pokemon is None:
        return
    yield pokemon.id
    for card in pokemon.energyCards:
        yield card.id
    for card in pokemon.tools:
        yield card.id
    for card in pokemon.preEvolution:
        yield card.id


def visible_cards(observation: Observation, player_index: int,
                  include_hand: bool = True) -> Counter[int]:
    state = observation.current
    player = state.players[player_index]
    result: Counter[int] = Counter()
    for pokemon in player.active:
        result.update(_iter_pokemon_cards(pokemon))
    for pokemon in player.bench:
        result.update(_iter_pokemon_cards(pokemon))
    result.update(card.id for card in player.discard)
    if include_hand and player.hand is not None:
        result.update(card.id for card in player.hand)
    result.update(card.id for card in player.prize if card is not None)
    result.update(card.id for card in state.stadium if card.playerIndex == player_index)
    if state.looking:
        result.update(
            card.id for card in state.looking
            if card is not None and card.playerIndex == player_index
        )
    return result


def _counter_items(counter: Counter[int]) -> list[int]:
    return [card for card, count in counter.items() for _ in range(max(0, count))]


def _repair_deck(hypothesis: Counter[int], required: Counter[int],
                 fallback_card: int) -> Counter[int]:
    deck = hypothesis.copy()
    for card, count in required.items():
        if deck[card] < count:
            deck[card] = count
    while sum(deck.values()) > 60:
        removable = [
            card for card, count in deck.items()
            if count > required[card] and card != fallback_card
        ]
        if not removable:
            break
        card = max(removable, key=lambda value: deck[value] - required[value])
        deck[card] -= 1
    while sum(deck.values()) < 60:
        deck[fallback_card] += 1
    return +deck


@dataclass
class Determinization:
    own_deck: list[int]
    own_prize: list[int]
    opponent_deck: list[int]
    opponent_prize: list[int]
    opponent_hand: list[int]
    opponent_active: list[int]


class BeliefTracker:
    def __init__(self, deck: list[int], cards: CardDatabase,
                 library: DeckLibrary) -> None:
        self.deck = Counter(deck)
        self.cards = cards
        self.library = library
        self.game_nonce = 0

    def reset(self) -> None:
        self.game_nonce += 1

    def sample(self, observation: Observation, sample_index: int) -> Determinization:
        state = observation.current
        me = state.yourIndex
        enemy = 1 - me
        seed_material = (
            f"{self.game_nonce}:{state.turn}:{state.turnActionCount}:"
            f"{sample_index}:{state.players[me].deckCount}:"
            f"{state.players[enemy].deckCount}"
        ).encode()
        seed = int.from_bytes(hashlib.blake2b(seed_material, digest_size=8).digest(), "little")
        rng = random.Random(seed)

        own_public = visible_cards(observation, me, include_hand=True)
        own_pool = self.deck - own_public
        own_prize_count = len(state.players[me].prize)
        own_known_prize = [
            card.id for card in state.players[me].prize if card is not None
        ]
        own_unknown_prize_count = own_prize_count - len(own_known_prize)
        own_deck_count = state.players[me].deckCount

        if observation.select.deck is not None:
            revealed_deck = [card.id for card in observation.select.deck]
            revealed_counts = Counter(revealed_deck)
            own_pool -= revealed_counts
            own_unknown_prize = _counter_items(own_pool)
            rng.shuffle(own_unknown_prize)
            own_prize = own_known_prize + own_unknown_prize[:own_unknown_prize_count]
            own_prize = own_prize[:own_prize_count]
            own_deck: list[int] = []
        else:
            own_hidden = _counter_items(own_pool)
            rng.shuffle(own_hidden)
            needed = own_unknown_prize_count + own_deck_count
            own_hidden = self._pad(own_hidden, needed, self._fallback_energy(self.deck))
            own_prize = own_known_prize + own_hidden[:own_unknown_prize_count]
            own_deck = own_hidden[own_unknown_prize_count:needed]
            rng.shuffle(own_deck)

        enemy_public = visible_cards(observation, enemy, include_hand=False)
        hypotheses = self.library.candidates(enemy_public)
        if hypotheses:
            hypothesis = hypotheses[sample_index % len(hypotheses)]
        else:
            hypothesis = self.deck.copy()
        fallback = self._fallback_energy(hypothesis)
        hypothesis = _repair_deck(hypothesis, enemy_public, fallback)
        enemy_pool = hypothesis - enemy_public
        enemy_player = state.players[enemy]
        opponent_active: list[int] = []
        if enemy_player.active and enemy_player.active[0] is None:
            basics = [
                card for card in _counter_items(enemy_pool)
                if (self.cards.card(card) is not None and self.cards.card(card).basic)
            ]
            if basics:
                active_id = basics[sample_index % len(basics)]
                opponent_active = [active_id]
                enemy_pool[active_id] -= 1
                if enemy_pool[active_id] <= 0:
                    del enemy_pool[active_id]

        enemy_hidden = _counter_items(enemy_pool)
        rng.shuffle(enemy_hidden)
        enemy_known_prize = [
            card.id for card in enemy_player.prize if card is not None
        ]
        enemy_unknown_prize_count = len(enemy_player.prize) - len(enemy_known_prize)
        enemy_needed = (
            enemy_player.handCount + enemy_unknown_prize_count + enemy_player.deckCount
        )
        enemy_hidden = self._pad(enemy_hidden, enemy_needed, fallback)
        hand_end = enemy_player.handCount
        prize_end = hand_end + enemy_unknown_prize_count
        enemy_hand = enemy_hidden[:hand_end]
        enemy_prize = enemy_known_prize + enemy_hidden[hand_end:prize_end]
        enemy_deck = enemy_hidden[prize_end:enemy_needed]
        rng.shuffle(enemy_deck)

        return Determinization(
            own_deck=own_deck,
            own_prize=own_prize,
            opponent_deck=enemy_deck,
            opponent_prize=enemy_prize,
            opponent_hand=enemy_hand,
            opponent_active=opponent_active,
        )

    def _fallback_energy(self, deck: Counter[int]) -> int:
        for card in deck:
            data = self.cards.card(card)
            if data and data.cardType == CardType.BASIC_ENERGY:
                return card
        return next(iter(deck), 1)

    @staticmethod
    def _pad(cards: list[int], count: int, fallback: int) -> list[int]:
        if len(cards) < count:
            cards.extend([fallback] * (count - len(cards)))
        return cards[:count]


def pokemon_strength(pokemon, cards: CardDatabase) -> float:
    if pokemon is None:
        return -500.0
    card = cards.card(pokemon.id)
    hp_fraction = pokemon.hp / max(1, pokemon.maxHp)
    value = pokemon.hp * 0.38 + pokemon.maxHp * 0.10
    value += len(pokemon.energies) * 18.0 + len(pokemon.tools) * 9.0
    value += len(pokemon.preEvolution) * 7.0
    value += hp_fraction * 20.0
    if card:
        value += (18.0 if card.ex else 0.0) + (10.0 if card.megaEx else 0.0)
        for attack_id in card.attacks:
            attack = cards.attacks.get(attack_id)
            if attack and len(attack.energies) <= len(pokemon.energies):
                value += min(80.0, cards.attack_damage(attack_id) * 0.20)
    return value


def boundary_value(observation: Observation, root_player: int,
                   cards: CardDatabase) -> float:
    state = observation.current
    if state.result >= 0:
        if state.result == root_player:
            return WIN_SCORE
        if state.result == 1 - root_player:
            return -WIN_SCORE
        return 0.0

    me = state.players[root_player]
    enemy = state.players[1 - root_player]
    score = (len(enemy.prize) - len(me.prize)) * 520.0
    score += (me.handCount - enemy.handCount) * 11.0
    score += (me.deckCount - enemy.deckCount) * 0.8

    my_board = sum(pokemon_strength(pokemon, cards) for pokemon in me.active + me.bench)
    enemy_board = sum(
        pokemon_strength(pokemon, cards) for pokemon in enemy.active + enemy.bench
    )
    score += my_board - enemy_board

    if me.active:
        score += pokemon_strength(me.active[0], cards) * 0.15
    if enemy.active:
        score -= pokemon_strength(enemy.active[0], cards) * 0.15

    score -= 38.0 * sum((me.poisoned, me.burned, me.asleep, me.paralyzed, me.confused))
    score += 38.0 * sum(
        (enemy.poisoned, enemy.burned, enemy.asleep, enemy.paralyzed, enemy.confused)
    )

    # At the unified boundary the opponent owns the next decision. Their
    # immediately available attacks represent concrete loss risk.
    if state.yourIndex == 1 - root_player and observation.select:
        for option in observation.select.option:
            if option.type == OptionType.ATTACK:
                score -= cards.attack_damage(option.attackId) * 0.55
    return score


def _card_for_option(observation: Observation, option: Option):
    state = observation.current
    if option.type in (OptionType.PLAY, OptionType.ATTACH, OptionType.EVOLVE):
        player = state.players[state.yourIndex]
        if player.hand is not None and option.index is not None:
            if 0 <= option.index < len(player.hand):
                return player.hand[option.index]
    if option.type == OptionType.CARD and option.area == AreaType.DECK:
        if observation.select.deck is not None and option.index is not None:
            if 0 <= option.index < len(observation.select.deck):
                return observation.select.deck[option.index]
    if option.type == OptionType.CARD:
        player_index = option.playerIndex
        if player_index is not None:
            player = state.players[player_index]
            if option.area == AreaType.HAND and player.hand is not None:
                if option.index is not None and 0 <= option.index < len(player.hand):
                    return player.hand[option.index]
            if option.area == AreaType.DISCARD and option.index is not None:
                if 0 <= option.index < len(player.discard):
                    return player.discard[option.index]
    return None


def option_score(observation: Observation, option: Option, cards: CardDatabase,
                 root_player: int) -> float:
    context = observation.select.context
    card_ref = _card_for_option(observation, option)
    card_id = card_ref.id if card_ref is not None else option.cardId
    if card_id is None and option.type == OptionType.ATTACK:
        card_id = option.attackId
    if card_id is None and option.type == OptionType.ABILITY:
        target = _target_pokemon(observation, option)
        card_id = target.id if target is not None else None
    value = cards.card_value(card_id)
    score = 0.0

    if option.type == OptionType.ATTACK:
        score = 180.0 + cards.attack_damage(option.attackId) * 2.0
    elif option.type == OptionType.ABILITY:
        score = 150.0
    elif option.type == OptionType.EVOLVE:
        score = 125.0 + value
    elif option.type == OptionType.ATTACH:
        score = 105.0 + value
        if option.inPlayArea == AreaType.ACTIVE:
            score += 25.0
    elif option.type == OptionType.PLAY:
        score = 90.0 + value
        data = cards.card(card_id)
        if data and data.cardType == CardType.SUPPORTER:
            score += 25.0
    elif option.type == OptionType.RETREAT:
        score = 30.0
        player = observation.current.players[observation.current.yourIndex]
        if player.active and (
            player.poisoned or player.burned or player.asleep
            or player.paralyzed or player.confused
        ):
            score += 100.0
    elif option.type == OptionType.END:
        score = -80.0
    elif option.type == OptionType.YES:
        score = 15.0
    elif option.type == OptionType.NO:
        score = 0.0
    elif option.type == OptionType.NUMBER:
        score = float(option.number or 0)
    elif option.type == OptionType.CARD:
        score = value
    elif option.type in (OptionType.ENERGY, OptionType.ENERGY_CARD, OptionType.TOOL_CARD):
        score = 10.0 - float(option.energyIndex or option.toolIndex or 0)

    if context in (
        SelectContext.DISCARD,
        SelectContext.DISCARD_ENERGY,
        SelectContext.DISCARD_ENERGY_CARD,
        SelectContext.DISCARD_TOOL_CARD,
        SelectContext.TO_DECK,
        SelectContext.TO_DECK_BOTTOM,
    ):
        score = -value
    elif context in (
        SelectContext.TO_HAND,
        SelectContext.ATTACH_FROM,
        SelectContext.EVOLVES_TO,
    ):
        score = value
    elif context in (SelectContext.SWITCH, SelectContext.TO_ACTIVE):
        score += _target_pokemon_score(observation, option, cards)
    elif context in (
        SelectContext.DAMAGE,
        SelectContext.DAMAGE_COUNTER,
        SelectContext.DAMAGE_COUNTER_ANY,
    ):
        target_owner = option.playerIndex
        target = _target_pokemon(observation, option)
        if target is not None:
            ko_margin = 200.0 if target.hp <= max(
                10, observation.select.remainDamageCounter * 10
            ) else 0.0
            score += ko_margin + (1000.0 - target.hp)
            if target_owner == root_player:
                score = -score
    elif context in (SelectContext.HEAL, SelectContext.REMOVE_DAMAGE_COUNTER):
        target = _target_pokemon(observation, option)
        if target is not None:
            score += target.maxHp - target.hp
    elif context == SelectContext.COIN_HEAD:
        score = 0.0
    score += 24.0 * cards.policy_score(context, option.type, card_id)
    return score


def _target_pokemon(observation: Observation, option: Option):
    if option.playerIndex is None or option.index is None:
        return None
    player = observation.current.players[option.playerIndex]
    if option.area == AreaType.ACTIVE:
        return player.active[option.index] if option.index < len(player.active) else None
    if option.area == AreaType.BENCH:
        return player.bench[option.index] if option.index < len(player.bench) else None
    return None


def _target_pokemon_score(observation: Observation, option: Option,
                          cards: CardDatabase) -> float:
    target = _target_pokemon(observation, option)
    return pokemon_strength(target, cards) if target is not None else 0.0


def legal_selections(observation: Observation, cards: CardDatabase,
                     root_player: int, cap: int = 8) -> list[list[int]]:
    select = observation.select
    if select is None:
        return []
    count = len(select.option)
    if count == 0:
        return [[]]
    ranked = sorted(
        range(count),
        key=lambda index: option_score(
            observation, select.option[index], cards, root_player
        ),
        reverse=True,
    )
    if select.maxCount == 1:
        selections = [[index] for index in ranked[:cap]]
        if select.minCount == 0:
            selections.append([])
        return selections

    # Full combination enumeration is capped. Include the greedy selection and
    # one-swap variants, which captures almost all multi-card effect choices.
    target_count = select.maxCount
    greedy = ranked[:target_count]
    selections: list[list[int]] = [sorted(greedy)]
    pool = ranked[:min(count, target_count + cap)]
    for remove_index in range(min(target_count, 4)):
        for replacement in pool[target_count:target_count + cap]:
            variant = greedy.copy()
            variant[remove_index] = replacement
            variant = sorted(set(variant))
            if select.minCount <= len(variant) <= select.maxCount:
                selections.append(variant)
                if len(selections) >= cap:
                    break
        if len(selections) >= cap:
            break
    if select.minCount == 0:
        selections.append([])
    unique: list[list[int]] = []
    seen = set()
    for selection in selections:
        key = tuple(selection)
        if key not in seen:
            seen.add(key)
            unique.append(selection)
    return unique[:cap]


class BoundarySearchAgent:
    def __init__(self, deck: list[int]) -> None:
        self.deck = deck
        self.cards = CardDatabase()
        self.library = DeckLibrary(_asset_path("deck_library.json"))
        self.belief = BeliefTracker(deck, self.cards, self.library)
        self.calls = 0

    def reset(self) -> None:
        self.calls = 0
        self.belief.reset()

    def choose(self, observation: Observation, raw: dict) -> list[int]:
        self.calls += 1
        select = observation.select
        if select is None:
            self.reset()
            return self.deck.copy()

        if select.context == SelectContext.IS_FIRST:
            # Going second gains the first attack and is the robust default for
            # unknown matchups in this ruleset.
            return [next(
                (i for i, option in enumerate(select.option)
                 if option.type == OptionType.NO), 0
            )]
        if select.context == SelectContext.COIN_HEAD:
            return [self.calls & 1]

        candidates = legal_selections(observation, self.cards,
                                      observation.current.yourIndex, cap=12)
        fallback = candidates[0] if candidates else list(range(select.maxCount))
        if select.context != SelectContext.MAIN:
            return fallback
        if len(candidates) <= 1:
            return fallback

        remaining = float(raw.get("remainingOverageTime", 600.0))
        budget = self._time_budget(observation, remaining)
        if budget < 0.08:
            return fallback
        try:
            return self._search_main(observation, candidates, budget)
        except Exception:
            # A legal deterministic fallback is more important than losing the
            # match to an unexpected simulator/search state.
            try:
                search_end()
            except Exception:
                pass
            return fallback

    def _time_budget(self, observation: Observation, remaining: float) -> float:
        reserve = 35.0
        usable = max(0.0, remaining - reserve)
        prizes = sum(len(player.prize) for player in observation.current.players)
        expected_main_calls = max(10, prizes * 2 + 4)
        per_call = usable / expected_main_calls
        return min(3.5, max(0.10, per_call))

    def _search_main(self, observation: Observation,
                     real_candidates: list[list[int]], budget: float) -> list[int]:
        deadline = time.monotonic() + budget
        sample_count = 1 if budget < 0.5 else (2 if budget < 1.5 else 4)
        totals = {tuple(candidate): 0.0 for candidate in real_candidates}
        counts = {tuple(candidate): 0 for candidate in real_candidates}

        for sample_index in range(sample_count):
            if time.monotonic() >= deadline:
                break
            determinization = self.belief.sample(observation, sample_index)
            try:
                root = search_begin(
                    observation,
                    determinization.own_deck,
                    determinization.own_prize,
                    determinization.opponent_deck,
                    determinization.opponent_prize,
                    determinization.opponent_hand,
                    determinization.opponent_active,
                    manual_coin=True,
                )
                values = self._evaluate_root(
                    root, observation.current.yourIndex,
                    real_candidates, deadline
                )
                for action, value in values.items():
                    totals[action] += value
                    counts[action] += 1
            finally:
                search_end()

        best = max(
            totals,
            key=lambda action: (
                totals[action] / max(1, counts[action]),
                -real_candidates.index(list(action)),
            ),
        )
        return list(best)

    def _evaluate_root(self, root, root_player: int,
                       candidates: list[list[int]],
                       deadline: float) -> dict[tuple[int, ...], float]:
        result: dict[tuple[int, ...], float] = {}
        expanded = 0
        for candidate in candidates:
            if time.monotonic() >= deadline or expanded >= MAX_SEARCH_STATES:
                break
            try:
                child = search_step(root.searchId, candidate)
                expanded += 1
                values = []
                rollout_count = 3
                for variant in range(rollout_count):
                    if time.monotonic() >= deadline or expanded >= MAX_SEARCH_STATES:
                        break
                    value, used = self._roll_to_boundary(
                        child, root_player, deadline,
                        MAX_SEARCH_STATES - expanded, variant
                    )
                    expanded += used
                    values.append(value)
                if values:
                    # Future decisions remain under our control, so selecting
                    # the best observation-contingent continuation is valid.
                    result[tuple(candidate)] = max(values)
            except (ValueError, RuntimeError):
                continue

        # Missing branches retain a conservative score and cannot win merely
        # because the time budget expired before they were examined.
        for index, candidate in enumerate(candidates):
            result.setdefault(tuple(candidate), -WIN_SCORE / 2.0 - index)
        return result

    def _roll_to_boundary(self, search_state, root_player: int,
                          deadline: float, node_budget: int,
                          variant: int = 0) -> tuple[float, int]:
        current = search_state
        root_turn = current.observation.current.turn
        used = 0
        previous_value = boundary_value(current.observation, root_player, self.cards)
        own_main_decisions = 0

        for depth in range(MAX_LINE_DEPTH):
            observation = current.observation
            state = observation.current
            if state.result >= 0:
                return boundary_value(observation, root_player, self.cards), used
            if (
                state.turn > root_turn
                and state.yourIndex != root_player
                and observation.select is not None
            ):
                return boundary_value(observation, root_player, self.cards), used
            if (
                time.monotonic() >= deadline
                or used >= node_budget
                or observation.select is None
            ):
                return previous_value, used

            selections = legal_selections(
                observation, self.cards, root_player, cap=4
            )
            if not selections:
                return previous_value, used

            # Search decisions after an observation are made from the new
            # information state. Opponent-owned selections use the reverse
            # ordering. Coin branches alternate deterministically across calls,
            # giving balanced outcomes over determinizations.
            if observation.select.context == SelectContext.COIN_HEAD:
                chosen = selections[(depth + self.calls) % len(selections)]
            elif state.yourIndex == root_player:
                choice_index = 0
                if observation.select.context == SelectContext.MAIN:
                    if variant > 0 and own_main_decisions == variant - 1:
                        choice_index = min(1, len(selections) - 1)
                    own_main_decisions += 1
                elif variant == 3 and depth < 4:
                    choice_index = min(1, len(selections) - 1)
                chosen = selections[choice_index]
            else:
                chosen = selections[-1]
            current = search_step(current.searchId, chosen)
            used += 1
            previous_value = boundary_value(current.observation, root_player, self.cards)

        return previous_value, used
