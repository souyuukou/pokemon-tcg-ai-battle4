# Boundary-search submission agent

The runnable agent is `sample_submission/main.py`.

Runtime design:

- card-count determinizations are sampled without replacement;
- opponent decks are selected from a 256-archetype, 47 KB replay prior;
- only Main selections invoke the simulator search API;
- each candidate is advanced through the complete current turn;
- evaluation occurs after control reaches the opponent;
- replay policy statistics affect branch order, not terminal scoring;
- search memory is released after every determinization;
- simulator states are capped at 1,200 per search and line depth at 36.

The submitted deck is a frequently observed Mega Lucario archetype. In the
local replay sample, the closest list recorded 615 wins in 708 games.

Validation:

```powershell
python -m unittest sample_submission/tests/test_agent.py
```

Regenerate the compact replay priors:

```powershell
python tools/build_deck_library.py --files 2000 --decks 256
```
