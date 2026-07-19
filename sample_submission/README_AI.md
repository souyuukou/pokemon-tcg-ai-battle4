# Boundary-search submission agent

The runnable agent is `sample_submission/main.py`.  Its default deck is the
hash-verified battle3 profile `majkel1337-85795098` (60 cards, canonical SHA
`3f4515092dc59df397f365a9b79c7cf0c1cb73b9aa38bc47c1b18e9df4c2fdaf`).

Runtime design:

- the native `ExactPlanner` is called through the exported simulator API;
- hidden deck/prize information is kept as correlated weighted worlds;
- draw, prize, reveal, and pending effects are resumable chance/observation
  transitions, and actions are selected only at the common turn boundary;
- canonical keys omit card serials, logs, and RNG state;
- per-turn sessions resume across selections and return exact score intervals;
- if an information-set proof cannot be completed, the policy fails closed to a
  deterministic legal fallback rather than using hidden identities.

The default deck is fixed to the battle3 list in
`sample_submission/sample_submission/deck.csv` and the matching profile under
`exact_solver/decks/`.

Validation:

```powershell
python -m unittest discover -s sample_submission/tests -v
```

Regenerate the compact replay priors:

```powershell
python tools/build_deck_library.py --files 10000 --decks 256
```

The checked-in `deck_library.json` and `policy_table.json` were regenerated
from 10,000 replay JSON files with Laplace-smoothed action frequencies.  The
learner is streaming: it keeps only one replay and compact counters in memory,
so it remains suitable for the 3 GB runtime limit.
