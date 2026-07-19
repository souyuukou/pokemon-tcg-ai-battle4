import ctypes
import json

from .sim import Battle, StartData, lib


def _get_battle_data() -> dict:
    """Retrieve the current state.

    Returns:
        dict: Current observation.
    """
    sd = lib.GetBattleData(Battle.battle_ptr)
    Battle.obs = json.loads(sd.json.decode())
    Battle.obs["search_begin_input"] = ctypes.string_at(sd.data, sd.count).decode("ascii")
    return Battle.obs


def battle_start(deck0: list[int], deck1: list[int]) -> tuple[dict, StartData]:
    """Start the battle.

    Args:
        deck0: List of card IDs included in the first player’s deck.
        deck1: List of card IDs included in the second player’s deck.

    Returns:
        tuple: A tuple containing:
            - dict: First observation.
            - StartData: Battle start data.
    """
    if len(deck0) != 60 or len(deck1) != 60:
        raise ValueError("The deck must contain 60 cards.")
    cards = deck0 + deck1
    arg = (ctypes.c_int * len(cards))(*cards)
    start_data = lib.BattleStart(arg)
    Battle.battle_ptr = start_data.battlePtr
    if Battle.battle_ptr == None or Battle.battle_ptr == 0:
        return (None, start_data)
    else:
        return (_get_battle_data(), start_data)


def battle_start_seeded(deck0: list[int], deck1: list[int], seed: int) -> tuple[dict, StartData]:
    """Start a deterministic battle for exact-search regression tests."""
    if len(deck0) != 60 or len(deck1) != 60:
        raise ValueError("The deck must contain 60 cards.")
    if not hasattr(lib, "BattleStartSeeded"):
        raise RuntimeError("BattleStartSeeded is not available in this native library")
    cards = deck0 + deck1
    arg = (ctypes.c_int * len(cards))(*cards)
    start_data = lib.BattleStartSeeded(arg, ctypes.c_uint(seed))
    Battle.battle_ptr = start_data.battlePtr
    if not Battle.battle_ptr:
        return (None, start_data)
    return (_get_battle_data(), start_data)


def battle_start_ordered(deck0: list[int], deck1: list[int], seed: int = 1) -> tuple[dict, StartData]:
    """Start replay from the exact post-shuffle deck order stored in a replay."""
    if len(deck0) != 60 or len(deck1) != 60:
        raise ValueError("The deck must contain 60 cards.")
    if not hasattr(lib, "BattleStartOrdered"):
        raise RuntimeError("BattleStartOrdered is not available in this native library")
    cards = deck0 + deck1
    arg = (ctypes.c_int * len(cards))(*cards)
    start_data = lib.BattleStartOrdered(arg, ctypes.c_uint(seed))
    Battle.battle_ptr = start_data.battlePtr
    if not Battle.battle_ptr:
        return (None, start_data)
    return (_get_battle_data(), start_data)


def battle_finish():
    """End the battle and free the memory used during it."""
    lib.BattleFinish(Battle.battle_ptr)


def battle_select(select_list: list[int]) -> dict:
    """Select option.

    Args:
        select_list:

    Returns:
        dict: Next observation.
    """
    if not isinstance(select_list, list) or not all(isinstance(i, int) for i in select_list):
        raise ValueError("select_list is not list[int]")
    arg = (ctypes.c_int * len(select_list))(*select_list)
    err = lib.Select(Battle.battle_ptr, arg, len(select_list))
    if err != 0:
        if err == 30:
            raise ValueError("battle_ptr broken.")
        else:
            raise IndexError()
    return _get_battle_data()


def exact_replay_trace_begin() -> None:
    """Capture native evaluator features at exact post-checkup turn leaves."""
    if not hasattr(lib, "ExactReplayTraceBegin"):
        raise RuntimeError("ExactReplayTraceBegin is not available in this native library")
    if not Battle.battle_ptr or lib.ExactReplayTraceBegin(Battle.battle_ptr) != 0:
        raise RuntimeError("cannot start exact replay trace")


def exact_replay_set_deck_order(player: int, card_ids: list[int]) -> None:
    """Restore the exact deck order recorded before the next replay action."""
    if not hasattr(lib, "ExactReplaySetDeckOrder") or not Battle.battle_ptr:
        raise RuntimeError("exact replay deck restoration is not available")
    values = (ctypes.c_int * len(card_ids))(*card_ids)
    error = lib.ExactReplaySetDeckOrder(Battle.battle_ptr, int(player), values, len(card_ids))
    if error:
        raise ValueError(f"cannot restore player {player} deck order (error {error})")


def exact_replay_set_hidden_zones(player: int, hand_ids: list[int], deck_ids: list[int]) -> None:
    """Restore recorded hand/deck membership after hidden random outcomes."""
    if not hasattr(lib, "ExactReplaySetHiddenZones") or not Battle.battle_ptr:
        raise RuntimeError("exact replay hidden-zone restoration is not available")
    hand = (ctypes.c_int * len(hand_ids))(*hand_ids)
    deck = (ctypes.c_int * len(deck_ids))(*deck_ids)
    error = lib.ExactReplaySetHiddenZones(Battle.battle_ptr, int(player),
                                          hand, len(hand_ids), deck, len(deck_ids))
    if error:
        raise ValueError(f"cannot restore player {player} hidden zones (error {error})")


def exact_replay_trace_drain() -> list[dict]:
    """Return and clear turn-end samples captured since the previous drain."""
    if not hasattr(lib, "ExactReplayTraceDrain") or not Battle.battle_ptr:
        raise RuntimeError("exact replay trace is not available")
    raw = lib.ExactReplayTraceDrain(Battle.battle_ptr)
    return json.loads(raw.decode()) if raw else []


def exact_replay_trace_end() -> None:
    """Disable native turn-end feature capture."""
    if hasattr(lib, "ExactReplayTraceEnd") and Battle.battle_ptr:
        lib.ExactReplayTraceEnd(Battle.battle_ptr)


def visualize_data() -> str:
    """Retrieve the data to be used by the visualizer.

    Returns:
        str: The data to be used by the visualizer.
    """
    return lib.VisualizeData(Battle.battle_ptr).decode()
