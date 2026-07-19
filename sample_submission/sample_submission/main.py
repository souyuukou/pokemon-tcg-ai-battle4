from cg.api import Observation, to_observation_class
from battle_ai import BoundarySearchAgent, read_deck

_agent = BoundarySearchAgent(read_deck())

def agent(obs_dict: dict) -> list[int]:
    """Implement Your Pokémon Trading Card Game Agent.

    Each element in the returned list must be >= 0 and < len(obs.select.option).
    The list length must be between obs.select.minCount and obs.select.maxCount (inclusive), with no duplicate elements.
    
    Returns:
        list[int]: A list of option index.
    """
    obs: Observation = to_observation_class(obs_dict)
    return _agent.choose(obs, obs_dict)
