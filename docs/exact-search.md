# Exact turn search

The exhaustive audit and deck-gating rules for hidden opponent information and
opponent decisions during the root turn are recorded in
[`current-turn-opponent-interactions.md`](current-turn-opponent-interactions.md).

## Semantics

The root is an information state, not a guessed complete game state. A leaf is
the state after end-of-turn effects and Pokémon Checkup, immediately before the
opponent's `TurnStart` and draw. Policies are keyed by the acting player's
information state, so a decision cannot depend on a hidden card identity.

Decision alternatives may be merged only when they induce the same normalized
distribution over next information state, observation, and rule state. Decision
multiplicity is discarded; chance multiplicity is accumulated as an integer.
Draws use products of binomial coefficients and a total of `C(N,k)`. Probabilities
are `fractions.Fraction`; there is no sampling, floating point, or probability
cutoff.

## Exact reductions

- An unordered draw is a card-count vector, not a permutation.
- Identical hand copies and identical structured Bench Pokémon form orbits.
- Multi-card choices are count vectors when order has no rule meaning.
- Energy payments are grouped by the final multiset, except energy with distinct
  effects remains distinct.
- Damage-counter placements use integer partitions over Pokémon orbits.
- Coin sequences are grouped by sufficient statistics only when later rules use
  only those statistics.
- A shuffled deck is represented lazily by its remaining multiset until an
  order-sensitive effect observes it.
- Face-down, unobserved, exchangeable prize positions have one representative
  decision. The revealed identity remains an exact chance outcome.
- Trigger order, yes/no, and count choices merge only after successor equality,
  unless a commuting rule has been explicitly audited.

Unknown selection contexts fail closed: all legal actions are retained.

## State identity and hashing

Canonical keys include rule state, turn history and flags, the card/attachment
graph, pending effects and selections, and actor-relative knowledge/belief.
Physical serials are alpha-renamed. Logs, UI option order, and exact RNG state are
excluded. Belief weights are divided by their GCD.

Two fixed-key SipHash-2-4 results provide a stable 128-bit table index. The full
canonical bytes are interned and compared on every digest hit. Hashes are never
used as proof of equality. Namespace fields include engine/canonical-rule/deck/
evaluator versions and the leaf definition.

## Resource policy

Use at most two root workers. Root actions are leases in a shared priority
queue. A lease is bounded to a short slice and returns the task to the queue;
the task retains its own planner, interval, consumed-node count, and chance
cursor, so another worker can continue it without regenerating completed
outcomes. The queue priority combines the initial work estimate, remaining
interval, consumed nodes, and deterministic tie-breakers.

The intended RSS hard stop is 2.7 GB: approximately 1.3 GB keys/TT, 0.8 GB
workers, 0.3 GB beliefs/policy/rational values, and 0.3 GB headroom. On Linux,
admission reads current `/proc/self/statm` resident pages; `ru_maxrss` is used
only for the diagnostic peak field. Keep 30 seconds of the 600-second match
bank.

An interrupted node is stored as an interval, never as an exact value. The
emergency action maximizes the proven lower bound and is marked `certified=false`.
The legacy `SearchBegin` API requires fabricated opponent identities. The agent
does not use that path for turn planning; `ExactDecide` below preserves unknown
zones and reports an interval when an identity is genuinely required.

## Native turn planner

`ExactDecide` is the information-safe replacement for `SearchBegin` in the
submission agent. It accepts the sanitized serialized observation, the fixed
deck, aligned hand-card values, and one wall-clock budget. It returns the chosen
physical option indexes, exact rational lower/upper bounds, certification, and
search metrics.

The planner stores the actor's unknown deck and prizes as one card-count pool.
Draws and prize takes branch by card type with remaining-copy integer weights.
For large multi-card draws, a `MultiDrawCursor` yields one bounded count-vector
outcome at a time and persists its composition stack across interruption;
small continuation spaces may use the bounded legacy vector path.
When an effect legally reveals the deck, prize multisets are enumerated lazily
with hypergeometric weights and the triggering action is replayed in each
resulting information state. A shuffled known deck is a multiset, not a sampled
permutation.

Two root workers own independent `Game` scratch state and partial cursors. They
share a 64-shard table of immutable completed entries; SipHash chooses a shard
and the complete canonical bytes are compared inside the digest bucket. Root
actions are probed fairly, then advanced in bounded node quanta. Actions with
the same canonical successor use one representative while all physical root
intervals remain in the API result. Unknown opponent identities are never
populated with guessed cards: an actual identity dependency produces the full
evaluator interval and `certified=false`.

`ExactCanonicalState` is used at every native node. It explicitly encodes State
scalars, Card state, references, attachments, ordered stacks, and exchangeable
zone multisets. It never hashes C++ padding, unused FixedList storage, physical
serials, or absolute move counters. This is required for resumability: the old
raw serializer produced different keys when the same root action was replayed.
Canonical bytes use lossless zero-run encoding before hashing and storage.

Interrupted Decision nodes retain exact action intervals and a round-robin
cursor. Chance nodes retain certified integer mass. Terminal turn leaves are
evaluated directly and are not inserted into the TT because they are cheap and
almost always unique. RSS is sampled in native enumeration loops; 2.7 GiB
stops further search safely and returns the current proven interval.

## Replay-trained CPU evaluator

V3 separates the lossless `PlayerInformationStateV3` used by search from the
Q8 belief projection used only by the evaluator. The evaluator record contains
global/public state, own hand and knowledge, exact known deck order, and one
structured record per Pokémon. Energy, Tools, evolution cards, persistent
effects, attacks, HP and status remain attached to that entity. Bench records
use a shared 24-unit integer encoder and symmetric pooling; a 64-unit global
layer combines the four owner/location pools with zones and belief features.

The explicit token vocabulary contains every registered card, attack, effect
and generated combo token. Belief inputs include expected Deck/Prize counts and
the probability of at least one copy, plus exact evolution and attack-energy
supply events, all rounded to Q8 only at the model boundary. Materialized
opponent hand, Deck, and Prize identities are forbidden. Weights are int16,
accumulators are checked integer arithmetic, and the output path is int64 with
clipped ReLU. Inference has no floating point, global lock, or per-leaf heap
allocation. Terminal scores remain `+/-100,000,000`; nonterminal output is
clamped to `+/-90,000,000`.

`ExactReplayTraceBegin` makes the normal engine pause after turn-end effects and
Pokémon Checkup but before the next `TurnStart`. `BattleStartOrdered` loads the
post-shuffle deck order recorded by Kaggle, because its environment seed does
not reproduce the engine's use of device randomness. The extractor replays the
complete action stream and rejects the entire match on a version mismatch,
illegal action, random divergence, truncation, or nonterminal ending. It never
keeps a valid prefix from a rejected match. Samples are actor-relative turn
leaves with final reward `{-1,0,1}` and per-match total loss weight one.

Train a replacement model with:

```powershell
python tools/extract_turn_end_dataset.py data/kaggle_replays data/turn-leaves.jsonl
python tools/train_turn_end_evaluator.py data/turn-leaves.jsonl exact-evaluator-v3.bin `
  --manifest data/turn-leaves.jsonl.manifest.json --epochs 30 --qat-epochs 8
```

Replays are split as complete matches by `(date, replayId)`: oldest 80% train,
next 10% validation, latest 10% test. The adoption report checks the zero
baseline, sign accuracy, quantization degradation, native/reference bit
identity, and an optional paired replay-level bootstrap against saved V2
baseline predictions.
`--require-gates` refuses to publish a model unless every supplied gate passes.

V1 and V2 evaluator formats and their model-specific public APIs are removed.
`ExactDecideV2` remains because it is a search API, not a model schema. V3
certification means only that the exact expectation of the fixed quantized
evaluator was computed over all chance outcomes and legal information-set
policies; it is exposed as
`certificationScope="exact_evaluator_expectation"` and does not claim the learned
evaluator is a perfect estimate of eventual match outcome.

`ExactLoadEvaluatorModel` and `ExactUnloadEvaluatorModel` expose explicit model
lifetime control. Every model payload ends with a versioned manifest containing
the feature-schema hash, belief-schema hash, `informationSetSafe`, and
`boundaryOnly`. The native loader rejects a missing or mismatched manifest;
active turn sessions retain an immutable shared model after it is detached from
future searches.

Windows x64 `cg.dll` and Linux x86-64 `libcg.so` include `ExactDecide`. The
Python wrapper feature-detects the symbol so the unchanged ARM64 library uses a
legal deterministic fallback.

`ExactDecideV2` additionally accepts an optional known opponent deck for
closed-world validation. Production calls omit it; an identity-dependent read
of an unknown opponent zone therefore still fails closed. Detailed metrics
separate unknown-opponent reads, unsupported concrete reads, interrupted
transitions, depth guards, raw choices, and quotient-merged choices.

`ExactTurnBegin` retains the selected root worker's transposition table and a
compact contingent policy for the rest of the turn.  Actor decision nodes are
indexed by a serial-independent semantic observation key; actions are stored as
semantic option descriptors and remapped to the current physical option array.
`ExactTurnAdvance` conditions on the next observation and returns a certified
policy hit without expanding nodes when the information state is unambiguous.
If multiple hidden beliefs produce different actions or values for the same
observable key, lookup fails closed and resumes exact search from the live
observation. `ExactTurnRelease` frees all native session memory at turn end.
`ExactTurnProgress` is read-only and reports the current root action, depth,
canonical/successor merges, resumed work, elapsed time, and memory.

The Python policy owns a `PolicyContext` per player.  Each context has its own
600-second chess clock, native session, and decision metrics; only time spent in
that player's action calls is charged.  This prevents self-play from sharing a
single budget or charging one player for the opponent's search.

Count-only and existence-only conditions on a hidden deck use the zone size and
do not request card identities. Concrete searches suspend the transition,
enumerate bounded card-count allocations with combination weights, materialize
the selected world, and replay from the pre-transition checkpoint. Identical
copies in an exchangeable searched deck share one semantic action.

Before an own-deck search, the remaining turn's reachable Skills, attacks, and
static target predicates are analysed into an `ExactCardPartition`. Card IDs
that an operator can distinguish remain singleton classes; all other IDs retain
their atoms but use one class count in the multivariate-hypergeometric reveal.
Refinement is monotone, so a later search can split a class conditionally without
changing total mass. Unsupported dynamic predicates conservatively expose every
identity and fall back to the generic exact enumerator. The regression suite also
runs this path with a non-submission deck to prevent fixed-ID specialization.

An unknown opponent list is a structural absence of a probability model, not a
slow subtree. Such a root action returns `searchStatus="blocked"` immediately;
time-sliced but fully specified work returns `"resumable"`. Completed searches
return `"certified"`. The production scheduler does not retry a blocked root,
while other legal root actions continue normally.
## Git deck workflow

`main` contains generic engine/search code. Deck/evaluator changes belong in
`deck/<slug>` branches and a profile under `decks/`. Select a profile with
`PTCG_DECK_PROFILE`; every profile carries a canonical SHA-256 checked at load.

