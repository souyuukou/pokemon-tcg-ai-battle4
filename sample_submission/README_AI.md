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
- a cheap native estimate runs at the beginning of each own turn; roots with an
  immediate multi-draw, a large estimated workload, or a wide Main use
  the separate general evaluator instead of starting an unlikely-to-finish
  exhaustive search;
- `exact-evaluator-v3.bin` is trained only on post-checkup turn boundaries;
- `general-evaluator-v3.bin` is a separate 30 MB information-set-safe network
  trained on every non-setup decision state. It applies each semantic candidate
  once and evaluates the resulting intermediate state. Its ranking includes a
  bounded observation-safe tactical term for forcing short sequences;
- the general evaluator also handles unresolved own choices during the
  opponent's turn and replaces an uncertified exact argmax at the hard time
  ceiling. It never reports exact certification;
- only if both native evaluators are unavailable does the policy fail closed to
  the deterministic legal rule heuristic.

The packaged Linux `cg/libcg.so` is built on Ubuntu 22.04 (GLIBC 2.35) so it
also loads in the older glibc used by the Kaggle runner. Do not replace it with
a build from Ubuntu 24.04+, which may require `GLIBC_2.38` or newer.

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

Retrain both Value models from complete, exactly replayable matches. The
official daily Kaggle ZIPs are consumed directly (no multi-gigabyte unzip
tree), and the trainer streams examples instead of retaining the full JSONL:

```powershell
python tools/extract_replay_v3_dataset.py data/kaggle_replays data/train-v3-both.jsonl `
  --mode both --limit 0 --max-replays 0
python tools/train_v3_evaluator.py data/train-v3-both.jsonl `
  sample_submission/sample_submission/exact-evaluator-v3.bin `
  --initial-model sample_submission/sample_submission/exact-evaluator-v3.bin `
  --sample-kind boundary --boundary-only --epochs 5
python tools/train_v3_evaluator.py data/train-v3-both.jsonl `
  sample_submission/sample_submission/general-evaluator-v3.bin `
  --initial-model sample_submission/sample_submission/exact-evaluator-v3.bin `
  --sample-kind intermediate --epochs 5
```

Each replay contributes total loss weight one independently to each dataset.
Any version, random, action, restoration, or terminal mismatch rejects the
entire replay; a valid prefix is never retained.

Run the generic-policy fixed-deck benchmark against the previous emergency
heuristic on both seats:

```powershell
python tools/evaluate_general_selfplay.py --seed-count 50 `
  --opponent heuristic --both-seats --work-threshold 1 `
  --output data/general-vs-heuristic.json
```

The latest all-day extraction (2026-06-16 through 2026-07-19) accepted 21,448
matches and produced 2,720,815 examples. The general export was promoted after
its quantized regression check; the newly trained boundary candidate is kept in
`data/` but is not promoted because quantization regressed its held-out MSE.
The 20-game both-seat verification completed all games, with the general
policy winning 12–8 against the emergency heuristic and peak RSS 242,302,976
bytes.
