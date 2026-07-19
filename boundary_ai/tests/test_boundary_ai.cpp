#include "boundary_ai/boundary_ai.hpp"

#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <string>
#include <utility>

using namespace boundary_ai;

namespace {

void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

void testRational() {
    require(Rational(1, 3) + Rational(1, 6) == Rational(1, 2),
            "rational addition");
    require(Rational(2, 3) * Rational(9, 4) == Rational(3, 2),
            "rational multiplication");
    require(Rational(-1, 2) < Rational(0), "rational comparison");
}

void testCountsAndMdd() {
    PackedCounts pool({{10, 3}, {20, 2}});
    const auto outcomes = enumerateHypergeometric(pool, 2);
    require(outcomes.size() == 3, "draw compositions are compressed");
    requireNormalized(outcomes);
    require(outcomes[0].probability == Rational(1, 10) ||
            outcomes[0].probability == Rational(3, 10) ||
            outcomes[0].probability == Rational(3, 5), "valid exact probability");

    PrizeMDD mdd(pool, 2);
    const auto prizeOutcomes = mdd.outcomes();
    require(prizeOutcomes.size() == 3, "MDD allocation count");
    requireNormalized(prizeOutcomes);
    require(mdd.nodeCount() < 10, "MDD shares suffix states");

    HiddenPool hidden(pool, 3, 2);
    const auto observed = PackedCounts({{10, 1}});
    const auto posterior = hidden.afterDeckObservation(observed);
    require(posterior.deckSize == 2 && posterior.prizeSize == 2,
            "observation updates zone sizes");
    require(posterior.unobserved.get(10) == 2, "observation updates counts");

    // Compare compressed count-vector expectation with physical-card
    // enumeration. This is the reference check required before enabling a
    // reduction in the real game adapter.
    Rational compressedExpectation;
    for (const auto& outcome : outcomes) {
        compressedExpectation += outcome.probability *
            Rational(outcome.cards.get(10));
    }
    int physicalTotal = 0;
    int physicalCases = 0;
    const int physicalCards[] = {10, 10, 10, 20, 20};
    for (int i = 0; i < 5; ++i) {
        for (int j = i + 1; j < 5; ++j) {
            physicalTotal += (physicalCards[i] == 10) + (physicalCards[j] == 10);
            ++physicalCases;
        }
    }
    require(compressedExpectation == Rational(physicalTotal, physicalCases),
            "compressed draw equals physical enumeration");
}

void testReductions() {
    PokemonSignature a{1, 100, PackedCounts({{50, 2}}), 0, 7, 9};
    PokemonSignature b = a;
    PokemonSignature c{2, 70, {}, 0, 0, 0};
    const auto groups = groupSymmetricPokemon({a, c, b});
    require(groups.size() == 2 && groups[0].multiplicity == 2,
            "pokemon symmetry");

    ReducibleAction high{2, {.reads = 1, .writes = 1}};
    ReducibleAction low{1, {.reads = 2, .writes = 2}};
    require(commute(high, low), "independent actions commute");
    require(violatesCanonicalOrder(high, low), "partial-order canonicalization");
    low.footprint.observes = true;
    require(!commute(high, low), "observations are order-sensitive");
}

struct ToyState {
    int id = 0;
};

enum class ToyAction {
    Safe,
    Inspect,
    ChooseA,
    ChooseB,
};

class ToyDomain final : public SearchDomain<ToyState, ToyAction> {
public:
    InformationState canonicalKey(const ToyState& state) const override {
        InformationState key;
        key.board = static_cast<BoardStateId>(state.id);
        return key;
    }

    Expansion<ToyState, ToyAction> expand(const ToyState& state) const override {
        switch (state.id) {
        case 0:
            return DecisionExpansion<ToyState, ToyAction>{{
                {ToyAction::Safe, {10}, 0.0},
                {ToyAction::Inspect, {1}, 1.0},
            }};
        case 1:
            return ObservationExpansion<ToyState>{{
                {100, Rational(1, 2), {2}, 1.0},
                {200, Rational(1, 2), {3}, 1.0},
            }};
        case 2:
            return DecisionExpansion<ToyState, ToyAction>{{
                {ToyAction::ChooseA, {11}, 1.0},
                {ToyAction::ChooseB, {12}, 0.0},
            }};
        case 3:
            return DecisionExpansion<ToyState, ToyAction>{{
                {ToyAction::ChooseA, {12}, 0.0},
                {ToyAction::ChooseB, {11}, 1.0},
            }};
        case 4:
            return ChanceExpansion<ToyState>{{
                {1, Rational(1, 2), {11}, 0.0},
                {2, Rational(1, 2), {12}, 0.0},
            }};
        case 10:
        case 11:
        case 12:
            return BoundaryExpansion{};
        default:
            throw std::logic_error("unknown toy state");
        }
    }
};

class ToyValue final : public BoundaryEvaluator<ToyState> {
public:
    Rational evaluateBoundary(const ToyState& state) const override {
        if (state.id == 10) return Rational(1, 4);
        if (state.id == 11) return Rational(1);
        if (state.id == 12) return Rational(-1);
        throw std::logic_error("value requested before boundary");
    }
};

void testObservationPolicyAndBoundaryOnlyValue() {
    ToyDomain domain;
    ToyValue value;
    BoundaryDagSolver<ToyState, ToyAction> solver(domain, value);
    const auto result = solver.solve({0});
    require(result.selectedAction == ToyAction::Inspect, "observation-contingent policy");
    require(result.score == ExactScore::exact(Rational(1)), "boundary expectation");
    require(result.bestActionProven, "best action proof");
    require(result.metrics.boundaryEvaluations == 2,
            "only boundary states are evaluated and proven branches are skipped");
    require(result.metrics.transpositionHits >= 1, "transposition sharing");
}

void testBudgetIntervals() {
    ToyDomain domain;
    ToyValue value;
    BoundaryDagSolver<ToyState, ToyAction> solver(domain, value);
    SearchBudget budget;
    budget.maxExpandedNodes = 1;
    const auto result = solver.solve({4}, budget);
    require(!result.score.complete, "budgeted search is incomplete");
    require(result.score.lower == Rational(-1) &&
            result.score.upper == Rational(1), "unexplored mass uses global bounds");
}

} // namespace

int main() {
    try {
        testRational();
        testCountsAndMdd();
        testReductions();
        testObservationPolicyAndBoundaryOnlyValue();
        testBudgetIntervals();
        std::cout << "boundary_ai_tests: all checks passed\n";
        return EXIT_SUCCESS;
    } catch (const std::exception& error) {
        std::cerr << "boundary_ai_tests: " << error.what() << '\n';
        return EXIT_FAILURE;
    }
}
