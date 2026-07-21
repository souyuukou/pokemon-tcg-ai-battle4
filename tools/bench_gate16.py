from __future__ import annotations

import argparse
import csv
import json
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
SYS_ROOT = ROOT / "sample_submission" / "sample_submission"
sys.path.insert(0, str(SYS_ROOT))

from cg.api import exact_decide_v2, to_observation_class  # noqa: E402
from exact_solver import agent_policy  # noqa: E402
from exact_solver.profile import load_profile  # noqa: E402


FIXTURE_DIR = ROOT / "sample_submission" / "tests" / "fixtures" / "replay_87096749"


def approx_fraction(num_key: str, den_key: str, row: dict) -> float:
    try:
        num = float(row.get(num_key, 0))
        den = float(row.get(den_key, 1))
        if den == 0:
            return 0.0
        return num / den
    except (TypeError, ValueError):
        return 0.0


def main() -> int:
    parser = argparse.ArgumentParser(description="Gate16 bestActionCertified bench")
    parser.add_argument("--budget-ms", type=int, default=30000)
    parser.add_argument("--out", type=Path, default=ROOT / "artifacts" / "gate16_argmax.json")
    parser.add_argument("--csv", type=Path, default=ROOT / "artifacts" / "gate16_proofgap.csv")
    args = parser.parse_args()

    if not FIXTURE_DIR.exists():
        print(f"missing fixtures: {FIXTURE_DIR}", file=sys.stderr)
        return 2

    manifest = json.loads((FIXTURE_DIR / "manifest.json").read_text(encoding="utf-8"))
    agent_policy._ensure_v3_evaluator()
    profile = load_profile()
    deck = list(profile.cards)
    hand = [100] * len(deck)

    rows = []
    best_count = 0
    exact_count = 0
    for item in manifest:
        path = FIXTURE_DIR / item["file"]
        obs = to_observation_class(json.loads(path.read_text(encoding="utf-8")))
        t0 = time.monotonic()
        decision = exact_decide_v2(obs, deck, hand, args.budget_ms, opponent_deck=deck)
        elapsed = time.monotonic() - t0
        best = bool(decision.get("bestActionCertified"))
        exact = bool(decision.get("exactValueCertified"))
        if best:
            best_count += 1
        if exact:
            exact_count += 1
        best_lower = approx_fraction("bestLowerNumerator", "bestLowerDenominator", decision)
        challenger = approx_fraction(
            "maxChallengerUpperNumerator", "maxChallengerUpperDenominator", decision
        )
        proof_gap = decision.get("proofGap")
        if proof_gap is None and decision.get("hasProofGap"):
            proof_gap = challenger - best_lower
        row = {
            "file": item["file"],
            "agent": item.get("agent"),
            "step": item.get("step"),
            "turn": item.get("turn"),
            "options": item.get("options"),
            "elapsedSec": round(elapsed, 3),
            "selected": decision.get("selected"),
            "bestActionCertified": best,
            "exactValueCertified": exact,
            "actionValueCertified": bool(decision.get("actionValueCertified")),
            "hasProofGap": bool(decision.get("hasProofGap")),
            "bestLower": best_lower,
            "maxChallengerUpper": challenger,
            "proofGap": proof_gap,
            "informationSetSafe": bool(decision.get("informationSetSafe")),
            "probabilityExact": bool(decision.get("probabilityExact")),
            "hiddenInformationLeakDetected": bool(
                decision.get("hiddenInformationLeakDetected")
            ),
            "boundContradictions": decision.get("boundContradictions", 0),
            "maxConcurrentSearchThreads": decision.get("maxConcurrentSearchThreads"),
            "peakRssBytes": decision.get("peakRssBytes"),
            "expandedNodes": decision.get("expandedNodes"),
            "rootQueueEliminations": decision.get("rootQueueEliminations"),
            "argmaxDominatedCuts": decision.get("argmaxDominatedCuts"),
            "partialChanceNodes": decision.get("partialChanceNodes"),
            "enumeratedHiddenWorlds": decision.get("enumeratedHiddenWorlds"),
            "certificationScope": decision.get("certificationScope"),
            "error": decision.get("error"),
        }
        rows.append(row)
        print(
            f"{item['file']}: best={best} exact={exact} gap={proof_gap} "
            f"el={elapsed:.2f}s selected={decision.get('selected')}"
        )

    summary = {
        "budgetMs": args.budget_ms,
        "bestActionCertified": f"{best_count}/{len(rows)}",
        "exactValueCertified": f"{exact_count}/{len(rows)}",
        "bestCount": best_count,
        "exactCount": exact_count,
        "rows": rows,
    }
    args.out.parent.mkdir(parents=True, exist_ok=True)
    args.out.write_text(json.dumps(summary, indent=2), encoding="utf-8")

    fieldnames = list(rows[0].keys()) if rows else []
    with args.csv.open("w", encoding="utf-8", newline="") as fh:
        writer = csv.DictWriter(fh, fieldnames=fieldnames)
        writer.writeheader()
        writer.writerows(rows)

    print(f"\nSUMMARY bestActionCertified={best_count}/{len(rows)} "
          f"exactValueCertified={exact_count}/{len(rows)}")
    print(f"wrote {args.out}")
    print(f"wrote {args.csv}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
