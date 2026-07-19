# Implementation status

The implementation is split at the existing engine's randomness boundary so
that unsupported randomness fails closed rather than being reported as exact.

## Implemented

- `belief/`: `PackedCounts`, exact multivariate hypergeometric enumeration,
  correlated deck/prize `HiddenPool`, shared `PrizeMDD`, and belief interning.
- `search/`: canonical information keys, Decision/Chance/Observation/Boundary
  expansions, a transposition DAG, exact score intervals, best-action proof,
  policy-only branch ordering, and time/node/memory budgets.
- `reduction/`: same-Pokémon target classes and conservative partial-order
  checks that forbid reduction around draw/search/shuffle/observation/bench
  capacity changes.
- `evaluation/`: a boundary feature type with no concrete hidden-world field,
  a bounded model interface, and a visible-state baseline value.
- Existing-engine integration: `BoundarySearch.h` advances `State::step()`,
  enumerates all legal selection combinations, exposes manual coins as exact
  chance nodes, and recognizes the post-checkup next-player boundary.
- Tests: probability normalization, compressed-vs-physical draw equivalence,
  MDD sharing, observation-contingent policy, TT merging, boundary-only Value,
  interval interruption, partial-order safety, and adapter compilation.

## Next engine refactor

`Draw` and `ShuffleDeck` currently execute synchronously inside card-effect
functions. Exact hidden-information branching cannot safely suspend there
because the current effect function would restart and duplicate side effects.
The next integration step is therefore to convert those primitives into
resumable pending effects. Until that is done, the fully observed adapter
throws when it sees a shuffle and never labels that branch exact.

After resumable draw/search/shuffle primitives are available:

1. bind each pending primitive to `HiddenPool`/`PrizeMDD`;
2. emit an `ObservationExpansion` when the acting player learns its outcome;
3. keep unobserved outcomes in a `ChanceExpansion` without adding them to the
   information-state key;
4. add full-engine exhaustive-oracle tests on reduced mini decks;
5. connect the trained boundary model and competition-agent API.
