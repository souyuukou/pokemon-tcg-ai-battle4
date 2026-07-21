from __future__ import annotations

import argparse
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


def approx(num, den) -> float:
    try:
        n = float(num)
        d = float(den or 1)
        return 0.0 if d == 0 else n / d
    except (TypeError, ValueError):
        return 0.0


def action_approx(action: dict, side: str) -> float:
    return approx(action.get(f"{side}Numerator"), action.get(f"{side}Denominator"))


def top_contributors(decision: dict, limit: int = 20) -> list[dict]:
    rows = []
    for action in decision.get("rootActions") or []:
        selected = action.get("selected") or []
        lo = action_approx(action, "lower")
        hi = action_approx(action, "upper")
        contrib = action.get("rootWidthContribution")
        if contrib is None:
            best = approx(
                decision.get("bestLowerNumerator"),
                decision.get("bestLowerDenominator"),
            )
            selected_best = decision.get("selected") or []
            if selected == selected_best:
                chal = approx(
                    decision.get("maxChallengerUpperNumerator"),
                    decision.get("maxChallengerUpperDenominator"),
                )
                contrib = max(0.0, chal - best)
            else:
                contrib = max(0.0, hi - best)
        local_width = action.get("localWidth")
        if local_width is None:
            local_width = max(0.0, hi - lo)
        reason = "certified" if action.get("certified") else "open_interval"
        if action.get("eliminated"):
            reason = "eliminated"
        elif not action.get("hasBeenVisited", True):
            reason = "unvisited"
        elif local_width >= 150_000_000:
            reason = "wide_pole_or_chance"
        elif not action.get("certified"):
            reason = "partial_decision_or_chance"
        rows.append(
            {
                "type": "RootAction",
                "rootAction": selected[0] if selected else None,
                "reachProbability": 1.0,
                "localLower": lo,
                "localUpper": hi,
                "localWidth": local_width,
                "rootWidthContribution": contrib,
                "estimatedRemainingWork": action.get("estimate") or 0,
                "consumedNodes": action.get("consumedNodes") or 0,
                "certified": bool(action.get("certified")),
                "eliminated": bool(action.get("eliminated")),
                "boundsSound": bool(action.get("boundsSound", True)),
                "pendingEffect": "",
                "reason": reason,
            }
        )
    rows.sort(key=lambda r: float(r["rootWidthContribution"] or 0), reverse=True)
    return rows[:limit]


def classify(decision: dict, contributors: list[dict]) -> str:
    native = decision.get("proofFailureClass")
    if decision.get("bestActionCertified"):
        return "none"
    if native and native != "none":
        return native
    if (
        decision.get("structurallyBlocked")
        or int(decision.get("opaqueNodes") or 0) > 0
        or decision.get("memoryLimitReached")
        or int(decision.get("unsupportedConcreteReference") or 0) > 0
    ):
        return "D_structural"
    chance_w = float(decision.get("unresolvedChanceWidthContribution") or 0)
    decision_w = float(decision.get("unresolvedDecisionWidthContribution") or 0)
    partial_c = int(decision.get("partialChanceNodes") or 0)
    partial_d = int(decision.get("partialDecisionNodes") or 0)
    wide = sum(1 for c in contributors if float(c.get("localWidth") or 0) >= 150_000_000)
    if wide >= 1 or chance_w >= decision_w or partial_c >= max(1, partial_d) * 2:
        return "A_unresolved_chance"
    if partial_d > partial_c:
        return "B_decision_branching"
    tt = int(decision.get("ttReadHits") or 0)
    merges = int(decision.get("successorMerges") or 0)
    expanded = int(decision.get("expandedNodes") or 0)
    if merges + tt < max(1, expanded // 100):
        return "C_reuse_shortage"
    return "A_unresolved_chance"


def build_report(file_name: str, meta: dict, decision: dict, budget_ms: int, elapsed: float) -> dict:
    contributors = top_contributors(decision)
    total_contrib = sum(float(c["rootWidthContribution"] or 0) for c in contributors) or 1.0
    for c in contributors:
        c["gapSharePct"] = round(
            100.0 * float(c["rootWidthContribution"] or 0) / total_contrib, 2
        )
    best_lower = approx(
        decision.get("bestLowerNumerator"), decision.get("bestLowerDenominator")
    )
    chal_upper = approx(
        decision.get("maxChallengerUpperNumerator"),
        decision.get("maxChallengerUpperDenominator"),
    )
    proof_gap = decision.get("proofGap")
    if proof_gap is None:
        proof_gap = chal_upper - best_lower
    report = {
        "positionId": file_name,
        "agent": meta.get("agent"),
        "step": meta.get("step"),
        "turn": meta.get("turn"),
        "options": meta.get("options"),
        "budgetMs": budget_ms,
        "elapsedSec": round(elapsed, 3),
        "selected": decision.get("selected"),
        "bestActionCertified": bool(decision.get("bestActionCertified")),
        "exactValueCertified": bool(decision.get("exactValueCertified")),
        "bestLower": best_lower,
        "challengerUpper": chal_upper,
        "proofGap": proof_gap,
        "expandedNodes": decision.get("expandedNodes"),
        "leaves": decision.get("leaves"),
        "evaluatorCalls": decision.get("evaluatorCalls"),
        "partialChanceNodes": decision.get("partialChanceNodes"),
        "partialDecisionNodes": decision.get("partialDecisionNodes"),
        "beliefNodes": decision.get("beliefNodes"),
        "enumeratedHiddenWorlds": decision.get("enumeratedHiddenWorlds"),
        "ttHits": decision.get("ttReadHits"),
        "successorMerges": decision.get("successorMerges"),
        "canonicalStateMerges": decision.get("canonicalStateMerges"),
        "opaqueNodes": decision.get("opaqueNodes"),
        "unsupportedNodes": decision.get("unsupportedConcreteReference"),
        "memoryStops": 1 if decision.get("memoryLimitReached") else 0,
        "structurallyBlocked": bool(decision.get("structurallyBlocked")),
        "unresolvedChanceWidthContribution": decision.get(
            "unresolvedChanceWidthContribution"
        ),
        "unresolvedDecisionWidthContribution": decision.get(
            "unresolvedDecisionWidthContribution"
        ),
        "proofFailureClass": classify(decision, contributors),
        "informationSetSafe": bool(decision.get("informationSetSafe")),
        "probabilityExact": bool(decision.get("probabilityExact")),
        "boundContradictions": decision.get("boundContradictions", 0),
        "hiddenInformationLeakDetected": bool(
            decision.get("hiddenInformationLeakDetected")
        ),
        "maxConcurrentSearchThreads": decision.get("maxConcurrentSearchThreads"),
        "peakRssBytes": decision.get("peakRssBytes"),
        "topContributors": contributors,
    }
    return report


def main() -> int:
    parser = argparse.ArgumentParser(description="Phase-0 proof failure reports")
    parser.add_argument(
        "--budgets",
        default="20000,60000",
        help="Comma-separated budgets in ms (default 20s,60s). Add 300000 for long curve.",
    )
    parser.add_argument(
        "--long-budget-files",
        default="",
        help="Optional comma-separated fixture names to also run at --long-budget-ms",
    )
    parser.add_argument("--long-budget-ms", type=int, default=300000)
    parser.add_argument(
        "--out",
        type=Path,
        default=ROOT / "artifacts" / "proof_failure_reports.json",
    )
    args = parser.parse_args()

    budgets = [int(x) for x in args.budgets.split(",") if x.strip()]
    long_files = {x.strip() for x in args.long_budget_files.split(",") if x.strip()}

    manifest = json.loads((FIXTURE_DIR / "manifest.json").read_text(encoding="utf-8"))
    agent_policy._ensure_v3_evaluator()
    profile = load_profile()
    deck = list(profile.cards)
    hand = [100] * len(deck)

    reports: list[dict] = []
    curves: dict[str, list[dict]] = {}

    for item in manifest:
        name = item["file"]
        path = FIXTURE_DIR / name
        obs = to_observation_class(json.loads(path.read_text(encoding="utf-8")))
        curves[name] = []
        run_budgets = list(budgets)
        if name in long_files:
            run_budgets.append(args.long_budget_ms)
        for ms in run_budgets:
            t0 = time.monotonic()
            decision = exact_decide_v2(obs, deck, hand, ms, opponent_deck=deck)
            elapsed = time.monotonic() - t0
            report = build_report(name, item, decision, ms, elapsed)
            reports.append(report)
            curves[name].append(
                {
                    "budgetMs": ms,
                    "elapsedSec": report["elapsedSec"],
                    "proofGap": report["proofGap"],
                    "bestActionCertified": report["bestActionCertified"],
                    "bestLower": report["bestLower"],
                    "challengerUpper": report["challengerUpper"],
                    "expandedNodes": report["expandedNodes"],
                    "class": report["proofFailureClass"],
                }
            )
            top = report["topContributors"][:3]
            top_txt = ", ".join(
                f"{c['rootAction']}:{c['gapSharePct']}%" for c in top
            )
            print(
                f"{name}@{ms}ms class={report['proofFailureClass']} "
                f"best={report['bestActionCertified']} gap={report['proofGap']} "
                f"top=[{top_txt}] el={elapsed:.2f}s"
            )

    # Classification summary at the largest common budget.
    primary_budget = max(budgets)
    primary = [r for r in reports if r["budgetMs"] == primary_budget]
    by_class: dict[str, list[str]] = {}
    for r in primary:
        by_class.setdefault(r["proofFailureClass"], []).append(r["positionId"])

    summary = {
        "primaryBudgetMs": primary_budget,
        "bestActionCertified": sum(1 for r in primary if r["bestActionCertified"]),
        "total": len(primary),
        "byClass": {k: {"count": len(v), "positions": v} for k, v in by_class.items()},
        "curves": curves,
        "reports": reports,
    }
    args.out.parent.mkdir(parents=True, exist_ok=True)
    args.out.write_text(json.dumps(summary, indent=2), encoding="utf-8")

    md = args.out.with_suffix(".md")
    lines = [
        f"# Proof failure report (budget={primary_budget}ms)",
        "",
        f"bestActionCertified: **{summary['bestActionCertified']}/{summary['total']}**",
        "",
        "## Class counts",
    ]
    for cls, info in sorted(summary["byClass"].items()):
        lines.append(f"- `{cls}`: {info['count']} — {', '.join(info['positions'])}")
    lines.append("")
    lines.append("## Per-position gap ownership (top contributors)")
    for r in sorted(primary, key=lambda x: float(x["proofGap"] or 0)):
        if r["bestActionCertified"]:
            continue
        lines.append(
            f"### {r['positionId']} — class `{r['proofFailureClass']}` gap={r['proofGap']}"
        )
        for c in r["topContributors"][:8]:
            lines.append(
                f"- rootAction={c['rootAction']} share={c['gapSharePct']}% "
                f"lo={round(c['localLower'],1)} hi={round(c['localUpper'],1)} "
                f"reason={c['reason']} nodes={c['consumedNodes']}"
            )
        lines.append("")
    md.write_text("\n".join(lines), encoding="utf-8")
    print(f"\nwrote {args.out}")
    print(f"wrote {md}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
