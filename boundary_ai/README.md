# Boundary-evaluated information-set DAG search

This directory contains the independent C++20 search core for the Pokémon TCG
engine. It deliberately has no dependency on physical card serial numbers or
on the engine's random-number generator.

Implemented layers:

- exact rational scores and multivariate hypergeometric outcomes;
- card-type count vectors and correlated deck/prize hidden pools;
- a shared allocation decision diagram (`PrizeMDD`);
- canonical information-state keys and belief interning;
- symmetric Pokémon grouping and conservative partial-order reduction;
- Decision, Chance, Observation, and Boundary DAG nodes;
- transposition sharing, policy-only scheduling, node/time/memory budgets;
- interval propagation and proof of a best root action;
- a boundary-only evaluator API that cannot receive a concrete hidden world.

Build and test:

```powershell
cmake -S boundary_ai -B build/boundary_ai -A x64
cmake --build build/boundary_ai --config Release
ctest --test-dir build/boundary_ai -C Release --output-on-failure
```

`SearchDomain` is the game-specific seam. Its implementation must expose
random results as `ChanceExpansion`, private observations as
`ObservationExpansion`, and return `BoundaryExpansion` only after attack,
knockout, prize, end-of-turn, Pokémon Checkup, and next-player transition have
completed.
