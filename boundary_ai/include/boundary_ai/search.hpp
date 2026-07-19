#pragma once

#include "boundary_ai/information_state.hpp"
#include "boundary_ai/rational.hpp"
#include "boundary_ai/runtime.hpp"

#include <algorithm>
#include <cstdint>
#include <functional>
#include <iterator>
#include <limits>
#include <optional>
#include <memory>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <utility>
#include <variant>
#include <vector>

namespace boundary_ai {

enum class NodeKind : std::uint8_t {
    Decision,
    Chance,
    Observation,
    Boundary,
};

template<class State, class Action>
struct DecisionBranch {
    Action action;
    State next;
    double priority = 0.0;
};

template<class State>
struct StochasticBranch {
    std::uint64_t outcomeId = 0;
    Rational probability;
    State next;
    double priority = 0.0;
};

template<class State, class Action>
struct DecisionExpansion {
    std::vector<DecisionBranch<State, Action>> branches;
};

template<class State>
struct ChanceExpansion {
    std::vector<StochasticBranch<State>> branches;
};

template<class State>
struct ObservationExpansion {
    std::vector<StochasticBranch<State>> branches;
};

struct BoundaryExpansion {};

template<class State, class Action>
using Expansion = std::variant<
    DecisionExpansion<State, Action>,
    ChanceExpansion<State>,
    ObservationExpansion<State>,
    BoundaryExpansion>;

template<class State, class Action>
class SearchDomain {
public:
    virtual ~SearchDomain() = default;
    [[nodiscard]] virtual InformationState canonicalKey(const State& state) const = 0;
    [[nodiscard]] virtual Expansion<State, Action> expand(const State& state) const = 0;
};

template<class State>
class BoundaryEvaluator {
public:
    virtual ~BoundaryEvaluator() = default;
    // This is intentionally the only evaluator entry point in the solver.
    [[nodiscard]] virtual Rational evaluateBoundary(const State& state) const = 0;
};

template<class Action>
struct ActionScore {
    Action action;
    ExactScore score;
};

template<class Action>
struct SearchResult {
    std::optional<Action> selectedAction;
    ExactScore score;
    std::vector<ActionScore<Action>> actions;
    bool bestActionProven = false;
    SearchMetrics metrics;
};

template<class State, class Action>
class BoundaryDagSolver {
public:
    BoundaryDagSolver(const SearchDomain<State, Action>& domain,
                      const BoundaryEvaluator<State>& evaluator,
                      Rational minimumValue = Rational(-1),
                      Rational maximumValue = Rational(1))
        : domain_(domain), evaluator_(evaluator),
          minimumValue_(std::move(minimumValue)),
          maximumValue_(std::move(maximumValue)) {
        if (maximumValue_ < minimumValue_) {
            throw std::invalid_argument("invalid value bounds");
        }
    }

    [[nodiscard]] SearchResult<Action> solve(
        const State& root, SearchBudget budget = {}) {
        budget_ = budget;
        metrics_ = {};
        table_.clear();

        auto expansion = domain_.expand(root);
        if (auto* decision = std::get_if<DecisionExpansion<State, Action>>(&expansion)) {
            return solveRootDecision(*decision);
        }

        SearchResult<Action> result;
        result.score = solveState(root);
        result.bestActionProven = result.score.complete;
        result.metrics = metrics_;
        return result;
    }

private:
    struct TableEntry {
        ExactScore score;
        bool inProgress = false;
    };

    const SearchDomain<State, Action>& domain_;
    const BoundaryEvaluator<State>& evaluator_;
    Rational minimumValue_;
    Rational maximumValue_;
    SearchBudget budget_;
    SearchMetrics metrics_;
    std::unordered_map<
        InformationState, std::shared_ptr<TableEntry>, InformationStateHash> table_;

    [[nodiscard]] ExactScore unknown() const {
        return {minimumValue_, maximumValue_, false};
    }

    SearchResult<Action> solveRootDecision(DecisionExpansion<State, Action> decision) {
        if (decision.branches.empty()) throw std::logic_error("decision has no actions");
        sortByPriority(decision.branches);
        SearchResult<Action> result;
        for (const auto& branch : decision.branches) {
            result.actions.push_back({branch.action, solveState(branch.next)});
        }

        auto best = result.actions.begin();
        for (auto it = std::next(result.actions.begin()); it != result.actions.end(); ++it) {
            if (it->score.lower > best->score.lower) best = it;
        }
        result.selectedAction = best->action;
        result.score = best->score;
        result.bestActionProven = true;
        for (const auto& action : result.actions) {
            if (&action != &*best && action.score.upper > best->score.lower) {
                result.bestActionProven = false;
                break;
            }
        }
        result.metrics = metrics_;
        return result;
    }

    ExactScore solveState(const State& state) {
        if (budget_.expired(metrics_.expandedNodes)) return unknown();

        InformationState key = domain_.canonicalKey(state);
        key.canonicalize();
        if (auto found = table_.find(key); found != table_.end()) {
            if (found->second->inProgress) {
                ++metrics_.cycleCuts;
                return unknown();
            }
            ++metrics_.transpositionHits;
            return found->second->score;
        }
        makeRoom();
        auto entry = std::make_shared<TableEntry>(TableEntry{unknown(), true});
        auto [position, inserted] = table_.emplace(
            std::move(key), entry);
        (void)inserted;
        (void)position;
        ++metrics_.expandedNodes;

        ExactScore score;
        const auto expansion = domain_.expand(state);
        if (const auto* decision =
                std::get_if<DecisionExpansion<State, Action>>(&expansion)) {
            ++metrics_.decisionNodes;
            score = solveDecision(*decision);
        } else if (const auto* chance =
                       std::get_if<ChanceExpansion<State>>(&expansion)) {
            ++metrics_.chanceNodes;
            score = solveStochastic(chance->branches);
        } else if (const auto* observation =
                       std::get_if<ObservationExpansion<State>>(&expansion)) {
            ++metrics_.observationNodes;
            score = solveStochastic(observation->branches);
        } else {
            ++metrics_.boundaryNodes;
            ++metrics_.boundaryEvaluations;
            const Rational value = evaluator_.evaluateBoundary(state);
            if (value < minimumValue_ || value > maximumValue_) {
                throw std::logic_error("boundary evaluator exceeded declared bounds");
            }
            score = ExactScore::exact(value);
        }
        *entry = {score, false};
        return score;
    }

    ExactScore solveDecision(DecisionExpansion<State, Action> decision) {
        if (decision.branches.empty()) throw std::logic_error("decision has no actions");
        sortByPriority(decision.branches);
        auto result = ExactScore{minimumValue_, minimumValue_, true};
        bool first = true;
        for (const auto& branch : decision.branches) {
            const auto child = solveState(branch.next);
            if (first || child.lower > result.lower) result.lower = child.lower;
            if (first || child.upper > result.upper) result.upper = child.upper;
            result.complete = result.complete && child.complete;
            first = false;
            if (result.lower == maximumValue_) {
                // No unexplored action can improve on the declared global max.
                result.upper = maximumValue_;
                return {result.lower, result.upper,
                        result.complete || result.lower == result.upper};
            }
        }
        return result;
    }

    ExactScore solveStochastic(std::vector<StochasticBranch<State>> branches) {
        if (branches.empty()) throw std::logic_error("stochastic node has no outcomes");
        ++metrics_.probabilityChecks;
        Rational mass;
        for (const auto& branch : branches) {
            if (branch.probability < Rational(0)) {
                throw std::logic_error("negative probability");
            }
            mass += branch.probability;
        }
        if (mass != Rational(1)) throw std::logic_error("probability mass is not one");

        sortByPriority(branches);
        ExactScore result{Rational(0), Rational(0), true};
        for (const auto& branch : branches) {
            const auto child = solveState(branch.next);
            result.lower += branch.probability * child.lower;
            result.upper += branch.probability * child.upper;
            result.complete = result.complete && child.complete;
        }
        return result;
    }

    template<class Branch>
    static void sortByPriority(std::vector<Branch>& branches) {
        std::stable_sort(branches.begin(), branches.end(),
            [](const Branch& lhs, const Branch& rhs) {
                return lhs.priority > rhs.priority;
            });
    }

    void makeRoom() {
        if (table_.size() < budget_.maxTranspositionEntries) return;
        for (auto it = table_.begin(); it != table_.end(); ++it) {
            if (!it->second->inProgress && !it->second->score.complete) {
                table_.erase(it);
                ++metrics_.evictedEntries;
                return;
            }
        }
        // Complete entries are still safe to evict; correctness is unchanged.
        for (auto it = table_.begin(); it != table_.end(); ++it) {
            if (!it->second->inProgress) {
                table_.erase(it);
                ++metrics_.evictedEntries;
                return;
            }
        }
    }
};

} // namespace boundary_ai
