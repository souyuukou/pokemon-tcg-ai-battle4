#include "boundary_ai/boundary_ai.hpp"

#include <chrono>
#include <iostream>

using namespace boundary_ai;

struct GridState { int x; int y; };
enum class GridAction { X, Y };

class GridDomain final : public SearchDomain<GridState, GridAction> {
public:
    InformationState canonicalKey(const GridState& state) const override {
        InformationState key;
        key.board = static_cast<std::uint64_t>(state.x * 100 + state.y);
        return key;
    }
    Expansion<GridState, GridAction> expand(const GridState& state) const override {
        if (state.x + state.y == 18) return BoundaryExpansion{};
        DecisionExpansion<GridState, GridAction> result;
        if (state.x < 12) result.branches.push_back({GridAction::X, {state.x + 1, state.y}, 1});
        if (state.y < 12) result.branches.push_back({GridAction::Y, {state.x, state.y + 1}, 0});
        return result;
    }
};

class GridValue final : public BoundaryEvaluator<GridState> {
public:
    Rational evaluateBoundary(const GridState& state) const override {
        return Rational(state.x - state.y, 18);
    }
};

int main() {
    std::vector<CountEntry> entries;
    for (CardTypeId card = 1; card <= 15; ++card) entries.push_back({card, 4});
    const PackedCounts deck(entries);

    const auto started = std::chrono::steady_clock::now();
    PrizeMDD prizes(deck, 6);
    const auto outcomes = prizes.outcomes();
    requireNormalized(outcomes);

    GridDomain domain;
    GridValue value;
    BoundaryDagSolver<GridState, GridAction> solver(domain, value);
    const auto result = solver.solve({0, 0});
    const auto elapsed = std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - started).count();

    std::cout << "prize_mdd_nodes=" << prizes.nodeCount()
              << " prize_count_vectors=" << outcomes.size()
              << " expanded_dag_nodes=" << result.metrics.expandedNodes
              << " tt_hits=" << result.metrics.transpositionHits
              << " elapsed_ms=" << elapsed << '\n';
}
