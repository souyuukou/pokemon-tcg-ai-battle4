import os
from cg.api import Observation, to_observation_class
from exact_solver.agent_policy import choose_action
from exact_solver.profile import load_profile

def read_deck_csv() -> list[int]:
    """Load the branch-selectable, hash-verified deck profile."""
    try:
        return list(load_profile().cards)
    except (FileNotFoundError, ValueError):
        file_path = "deck.csv"
        if not os.path.exists(file_path):
            file_path = "/kaggle_simulations/agent/" + file_path
        with open(file_path, "r") as file:
            return [int(v) for v in file.read().splitlines()[:60]]

def agent(obs_dict: dict) -> list[int]:
    """Implement Your Pokémon Trading Card Game Agent.

    Each element in the returned list must be >= 0 and < len(obs.select.option).
    The list length must be between obs.select.minCount and obs.select.maxCount (inclusive), with no duplicate elements.
    
    Returns:
        list[int]: A list of option index.
    """
    obs: Observation = to_observation_class(obs_dict)
    if obs.select == None:
        # In the initial selection, the obs.select is None, and it is necessary to return the deck.
        # The deck is a list of 60 card IDs.
        # The deck must comply with the Pokémon Trading Card Game rules.
        from exact_solver import agent_policy
        agent_policy._default_context.reset()
        agent_policy._last_turn = None
        agent_policy.last_decision = None
        agent_policy._policy_cache.clear()
        return read_deck_csv()

    action, _certified, _reason = choose_action(obs)
    return action
