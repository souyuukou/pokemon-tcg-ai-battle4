# Implementation status

The runnable submission is now connected to the native exact-turn planner in
`ptcg_engine/ptcgProgram 22`.  The standalone `boundary_ai` core remains the
small, engine-independent reference implementation; the competition path uses
the native planner because it can suspend engine effects at hidden-information
boundaries.

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
- Existing-engine integration: `ExactHiddenState` is carried in the real
  `State`, and `ExactPlanner` drives `Draw`, `TakePrize`, `RevealDeck`, and
  opaque pending effects through resumable pending transitions.  `Export.cpp`
  exposes `ExactTurnBegin`, `ExactTurnAdvance`, `ExactDecide`, progress, and
  release APIs used by `sample_submission/main.py`.
- Information-set safety: hidden deck/prize assignments are represented as
  correlated weighted worlds, are conditioned only after observations, and are
  merged by serial-independent canonical keys.  Native metrics fail closed on
  illegal information-set splits or hidden-information leaks.
- Resource limits: the planner uses two workers, a bounded state/key arena,
  exact rational weights, resumable per-turn sessions, and interval results
  when the 600-second/3 GB runtime budget is reached.
- Linux admission uses `/proc/self/statm` current RSS; `ru_maxrss` is retained
  only for the diagnostic peak field, so releasing a session permits later
  turns to search again.
- Root branches are leased from a shared priority queue in bounded slices.
  Each task owns its planner and cursor, so a lease can move between workers
  without regenerating completed chance outcomes.
- The process-wide `ExactThreadBudget(2)` uses one blocking root permit and an
  opportunistic second permit, so concurrent sessions cannot deadlock while
  waiting for a pair. Nested Chance async is disabled in root-parallel mode
  and every one-shot Chance batch acquires permits before spawning workers.
- Large multi-card draws use a `MultiDrawCursor` count-vector generator. Only
  a bounded composition stack and the current outcome are retained; the legacy
  continuation vector is reserved for small spaces where it is memory-bounded.
- Evaluator payloads carry a versioned manifest with feature/belief schema
  hashes, `informationSetSafe`, and `boundaryOnly`; load succeeds only after
  exact code-side validation.
- Deferred function frames carry explicit argument semantics. Card-reference
  arguments are canonicalized through semantic card tokens, while unknown
  argument meanings fail closed during deferred-effect coverage scans.
- `FixedList` boundary operations are checked before reads/writes, and State
  snapshots use memberwise C++ assignment instead of raw object-representation
  copies. Root results expose independent `bestActionCertified` and
  `exactValueCertified` proofs.
- Tests: probability normalization, compressed-vs-physical draw equivalence,
  MDD sharing, observation-contingent policy, TT merging, boundary-only Value,
  interval interruption, partial-order safety, adapter compilation, the
  battle3 deck profile hash, a native hidden-world smoke test, and a seeded
  Dudunsparce/Kadabra/Alakazam draw-chain fixture with coverage and RSS guards.
- CI runs GCC/Clang-style release and ASan/UBSan/TSan jobs plus MSVC Debug STL
  and native Windows release builds.
