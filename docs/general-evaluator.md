# General exception evaluator

Exact turn search and exception handling use different learned objectives.

`exact-evaluator-v3.bin` evaluates only the common boundary after attack,
knockout, prizes, end-of-turn effects, Pokémon Checkup, and control transfer.
Its manifest requires `informationSetSafe=1` and `boundaryOnly=1`.

`general-evaluator-v3.bin` is an independent V3 entity/global network. Its
manifest requires `informationSetSafe=1` and `boundaryOnly=0`; the two native
loaders reject swapping these files. Its replay target is the eventual
actor-relative match result at every non-setup decision state. Complete matches
have total loss weight one so long games do not dominate.

At the beginning of an own turn, `ExactEstimateRoot` applies each root option
to an isolated information state and estimates immediate draw combinations and
selection width. If the configured work threshold is exceeded, or an early
Main has more than four options with a substantial deck remaining, the agent
does not spend the turn budget on an unlikely-to-finish proof.

`ExactGeneralDecide` quotients physically equivalent options, applies each
semantic candidate once, and evaluates the successor. An immediate unresolved
draw or prize is projected from `OwnDeckExpected` or `OwnPrizeExpected` into an
expected hand feature. It never reads the concrete hidden card references.
The same path handles choices requested during the opponent's turn and exact
searches whose best action remains open at the hard time ceiling.

The runtime rank is the learned successor Value plus a bounded,
observation-safe tactical term. The learned network supplies the long-horizon
match estimate; the tactical term protects forcing local sequencing such as
attacking now, taking a mandatory beneficial search target, and not using Run
Away Draw from the only Pokémon in play. This term consumes only the same
public observation available to the player and never changes exact-search
certification.

The result is intentionally marked `certified=false` with
`decisionMode="general_one_step_value"`. It is a robust approximate exception
policy, not an exact boundary result. Runtime concurrency remains under the
process-wide two-thread permit budget, and the two resident V3 models require
well below the 3 GB limit.

The latest all-day run used the 178,025 replay candidates from 2026-06-16
through 2026-07-19 (including the official Kaggle daily ZIPs for 2026-07-03
through 2026-07-19). It accepted 21,448 complete matches and rejected 156,577
without retaining mismatched prefixes, producing 253,721 boundary and
2,467,094 intermediate examples. Training is streamed to keep the resident
working set below the 3 GB runtime budget.

The boundary candidate's float network improved (test MSE 0.6321), but its
quantized export regressed to MSE 0.9070 versus the deployed baseline 0.8051;
the production boundary file therefore remains the last quantized model that
passed the regression gate. The new general V3 export passed the gate: its
quantized test MSE is 0.9285 versus 0.9312 for its boundary-initialized input,
with sign accuracy 0.6277 versus 0.6341. Both candidate artifacts and their
reports remain in `data/` for further quantization-aware tuning.

In the final fixed-deck benchmark, the general exception policy played the
legacy emergency heuristic on both seats for seeds 1–50. All 100 games
terminated; the new policy won 57 and lost 43, with 258,084,864 peak RSS and
at most 219 decisions. Three hybrid mirror games (seeds 17, 23, and 41) also
terminated with 133–185 decisions, zero fail-closed selections, zero immediate
End choices while an attack was legal, and 269,221,888 peak RSS. The traces
showed evolution-draw engines being resolved before board development,
attachments, and legal attacks. These are regression and tactical sanity
checks; they demonstrate a stronger exception path but do not by themselves
constitute a statistical proof of professional competitive strength.
