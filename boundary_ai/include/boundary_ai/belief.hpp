#pragma once

#include "boundary_ai/counts.hpp"

#include <cstdint>
#include <map>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace boundary_ai {

using BeliefStateId = std::uint32_t;

// A layered decision diagram for exact zone allocations. Nodes are shared by
// (card-type layer, remaining zone capacity), so physical card permutations
// are never materialized.
class AllocationMDD {
public:
    struct Edge {
        std::uint16_t take = 0;
        std::uint64_t ways = 0;
        std::uint32_t child = 0;
    };
    struct Node {
        std::uint32_t layer = 0;
        std::uint16_t remaining = 0;
        std::vector<Edge> edges;
        bool terminal = false;
    };

    AllocationMDD() = default;
    AllocationMDD(PackedCounts pool, unsigned zoneSize)
        : pool_(std::move(pool)), zoneSize_(zoneSize) {
        if (zoneSize > static_cast<unsigned>(pool_.total())) {
            throw std::invalid_argument("zone exceeds hidden pool");
        }
        root_ = build(0, zoneSize);
    }

    [[nodiscard]] const Node& root() const { return nodes_.at(root_); }
    [[nodiscard]] std::uint32_t rootId() const noexcept { return root_; }
    [[nodiscard]] const Node& node(std::uint32_t id) const { return nodes_.at(id); }
    [[nodiscard]] std::size_t nodeCount() const noexcept { return nodes_.size(); }

    [[nodiscard]] std::vector<CountOutcome> outcomes() const {
        std::vector<CountOutcome> result;
        std::vector<CountEntry> selected;
        const auto denominator = combination(static_cast<unsigned>(pool_.total()), zoneSize_);
        enumerate(root_, 1, selected, denominator, result);
        return result;
    }

private:
    PackedCounts pool_;
    unsigned zoneSize_ = 0;
    std::uint32_t root_ = 0;
    std::vector<Node> nodes_;
    std::map<std::pair<std::size_t, unsigned>, std::uint32_t> memo_;

    std::uint32_t build(std::size_t layer, unsigned remaining) {
        const auto key = std::make_pair(layer, remaining);
        if (const auto found = memo_.find(key); found != memo_.end()) return found->second;
        const auto id = static_cast<std::uint32_t>(nodes_.size());
        memo_.emplace(key, id);
        nodes_.push_back(Node{static_cast<std::uint32_t>(layer),
                              static_cast<std::uint16_t>(remaining), {}, false});

        const auto entries = pool_.entries();
        if (layer == entries.size()) {
            nodes_[id].terminal = remaining == 0;
            return id;
        }

        const auto& entry = entries[layer];
        const auto maxTake = std::min<unsigned>(entry.count, remaining);
        for (unsigned take = 0; take <= maxTake; ++take) {
            const auto child = build(layer + 1, remaining - take);
            if (nodes_[child].terminal || !nodes_[child].edges.empty()) {
                nodes_[id].edges.push_back({
                    static_cast<std::uint16_t>(take),
                    combination(entry.count, take),
                    child
                });
            }
        }
        return id;
    }

    void enumerate(std::uint32_t nodeId, std::uint64_t weight,
                   std::vector<CountEntry>& selected, std::uint64_t denominator,
                   std::vector<CountOutcome>& out) const {
        const auto& current = nodes_.at(nodeId);
        if (current.terminal) {
            out.push_back({
                PackedCounts(selected),
                Rational(static_cast<std::int64_t>(weight),
                         static_cast<std::int64_t>(denominator))
            });
            return;
        }
        const auto entries = pool_.entries();
        for (const auto& edge : current.edges) {
            if (edge.ways != 0 &&
                weight > std::numeric_limits<std::uint64_t>::max() / edge.ways) {
                throw std::overflow_error("MDD path weight overflow");
            }
            if (edge.take != 0) {
                selected.push_back({entries[current.layer].card, edge.take});
            }
            enumerate(edge.child, weight * edge.ways, selected, denominator, out);
            if (edge.take != 0) selected.pop_back();
        }
    }
};

using PrizeMDD = AllocationMDD;

struct HiddenPool {
    PackedCounts unobserved;
    std::uint16_t deckSize = 0;
    std::uint16_t prizeSize = 0;

    HiddenPool() = default;
    HiddenPool(PackedCounts cards, unsigned deck, unsigned prizes)
        : unobserved(std::move(cards)),
          deckSize(static_cast<std::uint16_t>(deck)),
          prizeSize(static_cast<std::uint16_t>(prizes)) {
        validate();
    }

    void validate() const {
        if (unobserved.total() != static_cast<int>(deckSize + prizeSize)) {
            throw std::invalid_argument("hidden zone sizes do not match card counts");
        }
    }

    [[nodiscard]] PrizeMDD prizeAllocations() const {
        return PrizeMDD(unobserved, prizeSize);
    }

    [[nodiscard]] std::vector<CountOutcome> drawOutcomes(unsigned count) const {
        if (count > deckSize) throw std::invalid_argument("draw exceeds deck");
        // Exchangeability makes the deck marginal hypergeometric while the
        // prize/deck correlation remains in the posterior HiddenPool.
        return enumerateHypergeometric(unobserved, count);
    }

    [[nodiscard]] HiddenPool afterDeckObservation(const PackedCounts& observed) const {
        HiddenPool next = *this;
        if (!next.unobserved.contains(observed) ||
            observed.total() > static_cast<int>(next.deckSize)) {
            throw std::invalid_argument("impossible deck observation");
        }
        next.unobserved.subtract(observed);
        next.deckSize = static_cast<std::uint16_t>(next.deckSize - observed.total());
        next.validate();
        return next;
    }

    [[nodiscard]] HiddenPool afterPrizeObservation(const PackedCounts& observed) const {
        HiddenPool next = *this;
        if (!next.unobserved.contains(observed) ||
            observed.total() > static_cast<int>(next.prizeSize)) {
            throw std::invalid_argument("impossible prize observation");
        }
        next.unobserved.subtract(observed);
        next.prizeSize = static_cast<std::uint16_t>(next.prizeSize - observed.total());
        next.validate();
        return next;
    }

    friend bool operator==(const HiddenPool&, const HiddenPool&) = default;
};

struct BeliefState {
    HiddenPool ownHidden;
    HiddenPool opponentHidden;
    PackedCounts publiclyKnownOpponentHand;
    std::uint64_t observationFingerprint = 0;
    friend bool operator==(const BeliefState&, const BeliefState&) = default;
};

class BeliefStore {
public:
    BeliefStateId intern(BeliefState state) {
        // Belief states are relatively few compared with game states. A
        // deterministic linear collision check keeps the representation exact.
        const auto hash = hashState(state);
        if (const auto range = byHash_.equal_range(hash); range.first != range.second) {
            for (auto it = range.first; it != range.second; ++it) {
                if (states_[it->second] == state) return it->second;
            }
        }
        const auto id = static_cast<BeliefStateId>(states_.size());
        states_.push_back(std::move(state));
        byHash_.emplace(hash, id);
        return id;
    }

    [[nodiscard]] const BeliefState& get(BeliefStateId id) const { return states_.at(id); }
    [[nodiscard]] std::size_t size() const noexcept { return states_.size(); }

private:
    std::vector<BeliefState> states_;
    std::unordered_multimap<std::size_t, BeliefStateId> byHash_;

    static std::size_t hashState(const BeliefState& state) {
        PackedCountsHash hashCounts;
        std::size_t seed = hashCounts(state.ownHidden.unobserved);
        auto mix = [&](std::size_t value) {
            seed ^= value + 0x9e3779b97f4a7c15ull + (seed << 6) + (seed >> 2);
        };
        mix(state.ownHidden.deckSize);
        mix(state.ownHidden.prizeSize);
        mix(hashCounts(state.opponentHidden.unobserved));
        mix(state.opponentHidden.deckSize);
        mix(state.opponentHidden.prizeSize);
        mix(hashCounts(state.publiclyKnownOpponentHand));
        mix(static_cast<std::size_t>(state.observationFingerprint));
        return seed;
    }
};

inline void requireNormalized(std::span<const CountOutcome> outcomes) {
    Rational sum;
    for (const auto& outcome : outcomes) {
        if (outcome.probability < Rational(0)) {
            throw std::logic_error("negative probability");
        }
        sum += outcome.probability;
    }
    if (sum != Rational(1)) throw std::logic_error("probability mass is not one");
}

} // namespace boundary_ai
