"""Observation-safe action quotient and deterministic emergency policy."""

from __future__ import annotations
import os
import time
from dataclasses import dataclass, field
from .canonical import canonical_bytes
from .profile import load_profile
from .resources import MatchBudget


_budget = MatchBudget()
_last_turn = None
last_decision = None
_policy_cache = {}
_evaluator_loaded = False
_fallback_cards = None


def _ensure_v3_evaluator() -> None:
    global _evaluator_loaded
    if _evaluator_loaded:
        return
    from pathlib import Path
    from cg.api import exact_load_evaluator_model
    from .nnue_v3 import BELIEF_SCHEMA_HASH, FEATURE_SCHEMA_HASH
    candidates = [Path(__file__).resolve().parents[1] / "exact-evaluator-v3.bin",
                  Path("/kaggle_simulations/agent/exact-evaluator-v3.bin")]
    path = next((candidate for candidate in candidates if candidate.exists()), None)
    if path is None:
        raise RuntimeError("V3 evaluator model is missing")
    info = exact_load_evaluator_model(str(path))
    if (int(info.get("schemaVersion", 0)) != 3
            or not info.get("informationSetSafe")
            or not info.get("boundaryOnly")
            or int(info.get("featureSchemaHash", 0)) != FEATURE_SCHEMA_HASH
            or int(info.get("beliefSchemaHash", 0)) != BELIEF_SCHEMA_HASH):
        raise RuntimeError("V3 evaluator model was not accepted")
    _evaluator_loaded = True


@dataclass
class PolicyContext:
    """Per-player match budget and native turn-policy ownership."""
    budget: MatchBudget = field(default_factory=MatchBudget)
    last_turn: int | None = None
    session_id: int | None = None
    last_decision: dict | None = None
    budget_turn: int | None = None
    turn_search_seconds: float = 0.0

    def reset(self) -> None:
        if self.session_id is not None:
            try:
                from cg.api import exact_turn_release
                exact_turn_release(self.session_id)
            except (RuntimeError, OSError):
                pass
        self.budget.reset()
        self.last_turn = None
        self.session_id = None
        self.last_decision = None
        self.budget_turn = None
        self.turn_search_seconds = 0.0


_default_context = PolicyContext(_budget)


def option_semantic_key(option):
    data = vars(option).copy()
    data.pop("serial", None)
    return canonical_bytes(data)


def _fallback_database():
    """Lazily load only public card metadata and replay action priors."""
    global _fallback_cards
    if _fallback_cards is None:
        from battle_ai import CardDatabase
        _fallback_cards = CardDatabase()
    return _fallback_cards


def _visible_option_card_id(obs, option) -> int | None:
    """Resolve an option identity only from zones visible to the acting player."""
    try:
        option_type = int(option.type)
        if option_type == 13:  # Attack
            return option.attackId
        if option_type in (7, 8, 9):  # Play, attach, evolve from own hand
            hand = obs.current.players[obs.current.yourIndex].hand or ()
            return hand[int(option.index)].id
        if option_type not in (3, 10):  # Card or Ability
            return option.cardId
        player_index = (obs.current.yourIndex if option.playerIndex is None
                        else int(option.playerIndex))
        player = obs.current.players[player_index]
        area = int(option.area)
        index = int(option.index or 0)
        if area == 1 and obs.select.deck is not None:
            card = obs.select.deck[index]
            return None if card is None else card.id
        if area == 2 and player.hand is not None:
            return player.hand[index].id
        if area == 3:
            return player.discard[index].id
        if area == 4:
            pokemon = player.active[index]
            return None if pokemon is None else pokemon.id
        if area == 5:
            return player.bench[index].id
    except (AttributeError, IndexError, TypeError, ValueError):
        return None
    return option.cardId


def _in_play_target(obs, option):
    try:
        player = obs.current.players[obs.current.yourIndex]
        area = int(option.inPlayArea)
        index = int(option.inPlayIndex or 0)
        if area == 4:
            return player.active[index]
        if area == 5:
            return player.bench[index]
    except (AttributeError, IndexError, TypeError, ValueError):
        pass
    return None


def _fallback_score(obs, option) -> float:
    """Observation-safe tactical ordering when the exact policy is unavailable."""
    from battle_ai import option_score
    from cg.api import AreaType, EnergyType, OptionType, SelectContext

    cards = _fallback_database()
    context = SelectContext(int(obs.select.context))
    option_type = OptionType(int(option.type))
    card_id = _visible_option_card_id(obs, option)
    score = float(option_score(
        obs, option, cards, obs.current.yourIndex
    ))

    # This deck needs a Supporter on its first turn more than the one-turn
    # evolution lead.  The replay prior agrees: going second is the stable
    # default for the checked-in Alakazam list.
    if context == SelectContext.IS_FIRST:
        return 1_000_000.0 if option_type == OptionType.NO else -1_000_000.0

    # Do not expose a two-Prize support Pokémon as the opening Active.  Prefer
    # Dunsparce, which can pivot or evolve into the deck's draw engine, followed
    # by Abra, which advances the main attacker.
    if context == SelectContext.SETUP_ACTIVE_POKEMON:
        return score + {
            305: 400.0,  # Dunsparce
            741: 300.0,  # Abra
            343: 100.0,  # Shaymin
            140: -500.0,  # Fezandipiti ex
        }.get(card_id, 0.0)

    # Bench search should establish evolution lines before consuming slots with
    # support Pokémon.  These identities are visible search results, not hidden
    # deck materializations.
    if context in (SelectContext.SETUP_BENCH_POKEMON,
                   SelectContext.TO_BENCH,
                   SelectContext.TO_FIELD):
        score += {
            741: 300.0,  # Abra
            305: 240.0,  # Dunsparce
            343: 80.0,   # Shaymin
            140: -80.0,  # Fezandipiti ex
        }.get(card_id, 0.0)

    # Sacred Ash's TO_DECK selection is recovery, not a hand-reset cost.
    effect_id = getattr(obs.select.effect, "id", None)
    if context == SelectContext.TO_DECK and effect_id == 1129:
        score = cards.card_value(card_id)

    if context == SelectContext.MAIN and option_type == OptionType.ABILITY:
        player = obs.current.players[obs.current.yourIndex]
        # Run Away Draw shuffles Dudunsparce itself away.  If it is the only
        # Pokémon in play, using it loses immediately for having no Active.
        if (card_id == 66 and int(option.area) == int(AreaType.ACTIVE)
                and not player.bench):
            return -10_000_000.0
        # Both draw Abilities are normally strongest before committing to an
        # attack because the new cards can change every later legal choice.
        if card_id in (66, 140):
            score += 160.0

    if context == SelectContext.MAIN and option_type == OptionType.EVOLVE:
        # Kadabra and Alakazam draw on evolution; Dudunsparce unlocks the
        # reusable draw engine.  Resolve these before an already-available
        # low-damage attack.
        score += 170.0 if card_id in (66, 742, 743) else 100.0

    if context == SelectContext.MAIN and option_type == OptionType.PLAY:
        # Deck-specific sequencing for the fixed submission list.  Search and
        # evolution setup belong before attacking; reactive gust is deliberately
        # left below an available attack unless exact search proves the line.
        score += {
            741: 120.0,   # Abra
            305: 110.0,   # Dunsparce
            1079: 190.0,  # Rare Candy
            1081: 80.0,   # Enhanced Hammer
            1086: 150.0,  # Buddy-Buddy Poffin
            1097: 80.0,   # Night Stretcher
            1129: 60.0,   # Sacred Ash
            1152: 140.0,  # Poké Pad
            1184: 90.0,   # Lana's Aid
            1197: 90.0,   # Xerosic's Machinations
            1225: 170.0,  # Hilda
            1231: 180.0,  # Dawn
        }.get(card_id, 0.0)

    if context == SelectContext.MAIN and option_type == OptionType.ATTACH:
        target = _in_play_target(obs, option)
        target_data = cards.card(target.id) if target is not None else None
        player = obs.current.players[obs.current.yourIndex]
        if card_id == 13:  # Enriching Energy: attaching draws four.
            score += 300.0
        elif card_id == 19:  # Telepath Psychic Energy
            if (target_data is not None
                    and int(target_data.energyType) == int(EnergyType.PSYCHIC)
                    and len(player.bench) < player.benchMax):
                score += 260.0
            else:
                score -= 120.0
        if target is not None and target.id == 66:
            # An attached card is shuffled away by Run Away Draw.
            score -= 100.0

    if context == SelectContext.MAIN and option_type == OptionType.ATTACK:
        if option.attackId == 1072:  # Alakazam: Powerful Hand
            # The printed attack has zero base damage because it places
            # counters.  Its actual output is 20 per card in hand.
            score += max(0.0, 40.0 * obs.current.players[
                obs.current.yourIndex
            ].handCount - 44.0)
        elif option.attackId == 183:  # Fezandipiti ex: Cruel Arrow
            score += 156.0  # Treat the printed effect as its actual 100 damage.
    return score


def _force_max_fallback_choices(obs) -> bool:
    from cg.api import SelectContext
    context = SelectContext(int(obs.select.context))
    if context in (
        SelectContext.TO_BENCH,
        SelectContext.TO_FIELD,
        SelectContext.TO_HAND,
        SelectContext.LOOK,
        SelectContext.HEAL,
        SelectContext.REMOVE_DAMAGE_COUNTER,
        SelectContext.TO_HAND_ENERGY,
    ):
        return True
    return (context == SelectContext.TO_DECK
            and getattr(obs.select.effect, "id", None) == 1129)


def _fallback_action(obs) -> list[int]:
    """Choose a legal deterministic action without enumerating combinations."""
    select = obs.select
    option_count = len(select.option)
    ranked = sorted(range(option_count), key=lambda index: (
        -_fallback_score(obs, select.option[index]),
        option_semantic_key(select.option[index]),
        index,
    ))
    # These are benefit selections reached only after the card/effect was
    # already committed.  Replay log-odds are useful for ranking identities,
    # but must not turn a low-frequency Energy or Basic into "take zero".
    if _force_max_fallback_choices(obs):
        return sorted(ranked[:select.maxCount])
    prefix_score = 0.0
    candidates = []
    for count in range(0, select.maxCount + 1):
        if count > 0:
            prefix_score += _fallback_score(obs, select.option[ranked[count - 1]])
        if count < select.minCount:
            continue
        action = sorted(ranked[:count])
        candidates.append((
            -prefix_score,
            count,
            tuple(sorted(option_semantic_key(select.option[i]) for i in action)),
            action,
        ))
    if not candidates:
        raise ValueError("observation has no legal fallback cardinality")
    return min(candidates)[-1]


def _turn_slice_milliseconds(ctx: PolicyContext, is_new_turn: bool, usable_ms: int,
                             allow_reroot: bool = False) -> int:
    """Return this call's slice while enforcing one absolute per-turn cap."""
    turn_cap = int(os.environ.get("PTCG_EXACT_TURN_MS", "90000"))
    selection_cap = int(os.environ.get("PTCG_EXACT_SELECTION_MS", "5000"))
    remaining_turn_ms = turn_cap - int(ctx.turn_search_seconds * 1000)
    if remaining_turn_ms <= 0:
        # A retained policy lookup is cheap, but the observed continuation may
        # be an unfinished branch that was not reached during the root slice.
        # Give that re-root one normal selection slice instead of 1 ms.  The
        # process-wide MatchBudget remains the hard 600-second ceiling.
        if allow_reroot:
            return max(1, min(selection_cap, usable_ms))
        raise RuntimeError("exact turn search budget reached")
    # Small slices make the native root queue resumable and let Python stop as
    # soon as exact-value certification is reached instead of reserving the
    # whole turn allowance for a single opaque call.
    return max(1, min(remaining_turn_ms, selection_cap, usable_ms))


def _turn_owner(current) -> int | None:
    """Return the player whose turn it is; setup has no turn owner."""
    if current is None or current.turn <= 0 or current.firstPlayer not in (0, 1):
        return None
    return current.firstPlayer if current.turn % 2 == 1 else 1 - current.firstPlayer


def choose_action(obs, *, context: PolicyContext | None = None,
                  opponent_deck: list[int] | None = None) -> tuple[list[int], bool, str]:
    """Return action, certification flag, reason.

    Full turn search is only certified when a transition provider can resolve all
    hidden chance variables. The bundled API demands guessed opponent cards, so
    this policy never calls it with fabricated identities.
    """
    ctx = context or _default_context
    observed_turn = obs.current.turn if obs.current is not None else None
    if observed_turn != ctx.budget_turn:
        ctx.budget_turn = observed_turn
        ctx.turn_search_seconds = 0.0
    call_started = time.monotonic()
    def finish(action, certified, reason):
        elapsed = time.monotonic() - call_started
        ctx.budget.charge(elapsed)
        ctx.turn_search_seconds += elapsed
        return action, certified, reason

    select = obs.select
    if select is None: raise ValueError("deck request is not an action")
    option_count = len(select.option)
    if not 0 <= select.minCount <= select.maxCount <= option_count:
        raise ValueError("invalid observation")

    # Setup is not a turn-end-boundary search problem. Keep it out of the
    # emergency path and name the deterministic deck-specific policy explicitly.
    if obs.current is None or obs.current.turn <= 0:
        return finish(_fallback_action(obs), False, "setup-policy")

    # Effects resolved during the opponent's turn can ask this process to choose
    # a promotion, discard, switch target, and similar options.  They are outside
    # the own-turn planning objective.  Starting a 90-second ExactTurnSession for
    # them consumed most of the match clock in real episodes, so use the
    # observation-safe tactical policy without starting a native session.
    owner = _turn_owner(obs.current)
    if owner is not None and owner != obs.current.yourIndex:
        if ctx.session_id is not None:
            try:
                from cg.api import exact_turn_release
                exact_turn_release(ctx.session_id)
            except (RuntimeError, OSError):
                pass
            ctx.session_id = None
        ctx.last_turn = None
        return finish(_fallback_action(obs), False,
                      "heuristic-opponent-turn-selection")
    if select.minCount == select.maxCount == 0: return finish([], True, "forced-empty")
    if select.minCount == select.maxCount == option_count:
        return finish(list(range(option_count)), True, "forced-all")
    global _last_turn, last_decision
    native_failure = "native exact chance provider unavailable"
    try:
        if not ctx.budget.can_expand():
            raise RuntimeError("exact search resource reserve reached")
        from cg.api import exact_decide, exact_turn_begin, exact_turn_advance, exact_turn_release
        _ensure_v3_evaluator()
        profile = load_profile()
        values = profile.evaluator.get("hand_values", {})
        hand_values = [int(values.get(str(card_id), values.get("default", 100))) for card_id in profile.cards]
        is_new_turn = obs.current is not None and obs.current.turn != ctx.last_turn
        usable_ms = max(1, int((ctx.budget.remaining - ctx.budget.limits.reserve_seconds) * 1000))
        requested_ms = _turn_slice_milliseconds(
            ctx, is_new_turn, usable_ms,
            allow_reroot=ctx.session_id is not None and not is_new_turn,
        )
        if is_new_turn:
            if ctx.session_id is not None:
                exact_turn_release(ctx.session_id)
                ctx.session_id = None
            try:
                native = exact_turn_begin(obs, list(profile.cards), hand_values,
                                          requested_ms, opponent_deck=opponent_deck)
                ctx.session_id = int(native["sessionId"])
                reason = "native-exact-turn-begin"
            except RuntimeError:
                native = exact_decide(obs, list(profile.cards), hand_values, requested_ms)
                reason = "native-exact-turn-search"
        elif ctx.session_id is not None:
            native = exact_turn_advance(ctx.session_id, obs, requested_ms)
            reason = "exact-policy-reroot" if native.get("policyHits", 0) else "exact-policy-resume"
        else:
            native = exact_decide(obs, list(profile.cards), hand_values, requested_ms)
            reason = "native-exact-turn-search"

        # Continue the same observable root until its exact value and argmax are
        # certified, or until the explicit turn/match resource ceiling is
        # reached. ExactTurnAdvance now retains every root task, so these calls
        # monotonically refine all competing action intervals.
        turn_cap_ms = int(os.environ.get("PTCG_EXACT_TURN_MS", "90000"))
        selection_cap_ms = int(os.environ.get("PTCG_EXACT_SELECTION_MS", "5000"))
        while (ctx.session_id is not None
               and not bool(native.get("exactValueCertified", native.get("certified", False)))
               and not bool(native.get("structurallyBlocked", False))
               and not bool(native.get("memoryLimitReached", False))
               and int(native.get("boundContradictions", 0)) == 0
               and int(native.get("sessionInvalidations", 0)) == 0):
            elapsed_ms = int((time.monotonic() - call_started) * 1000)
            remaining_turn_ms = turn_cap_ms - int(ctx.turn_search_seconds * 1000) - elapsed_ms
            remaining_match_ms = int(
                (ctx.budget.remaining - ctx.budget.limits.reserve_seconds) * 1000
            ) - elapsed_ms
            remaining_ms = min(remaining_turn_ms, remaining_match_ms)
            if remaining_ms <= 0:
                break
            native = exact_turn_advance(
                ctx.session_id, obs,
                max(1, min(selection_cap_ms, remaining_ms)),
            )
            reason = ("native-exact-certified"
                      if native.get("exactValueCertified", native.get("certified", False))
                      else "native-exact-resume")

        action = [int(index) for index in native["selected"]]
        if select.minCount <= len(action) <= select.maxCount and len(set(action)) == len(action) \
                and all(0 <= index < option_count for index in action):
            ctx.last_turn = obs.current.turn if obs.current is not None else ctx.last_turn
            ctx.last_decision = native
            if context is None:
                _last_turn = ctx.last_turn
                last_decision = native
            if not bool(native.get("exactValueCertified", native.get("certified", False))):
                reason = "native-exact-budget-exhausted"
            # At a hard resource ceiling, return the exact solver's documented
            # highest-lower-bound action and interval. Never replace an
            # information-set-safe native result with the emergency heuristic.
            return finish(action, bool(native.get("exactValueCertified",
                                                   native.get("certified", False))),
                          reason)
    except Exception as error:
        native_failure = f"{type(error).__name__}: {error}"
    # O(n log n), including variable-cardinality selections.  Enumerating every
    # combination here used to make the emergency path itself exceed the clock
    # on large search lists.
    best = _fallback_action(obs)
    return finish(best, False, f"fail-closed-policy: {native_failure}")

