#pragma once

#include "boundary_ai/rational.hpp"

#include <algorithm>
#include <cstdint>
#include <functional>
#include <limits>
#include <numeric>
#include <span>
#include <stdexcept>
#include <utility>
#include <vector>

namespace boundary_ai {

using CardTypeId = std::uint32_t;

struct CountEntry {
    CardTypeId card = 0;
    std::uint16_t count = 0;
    friend bool operator==(const CountEntry&, const CountEntry&) = default;
};

class PackedCounts {
public:
    PackedCounts() = default;
    explicit PackedCounts(std::vector<CountEntry> entries) : entries_(std::move(entries)) {
        canonicalize();
    }

    [[nodiscard]] std::uint16_t get(CardTypeId card) const {
        const auto it = lowerBound(card);
        return it == entries_.end() || it->card != card ? 0 : it->count;
    }

    void set(CardTypeId card, std::uint16_t count) {
        auto it = lowerBound(card);
        if (it != entries_.end() && it->card == card) {
            if (count == 0) entries_.erase(it);
            else it->count = count;
        } else if (count != 0) {
            entries_.insert(it, {card, count});
        }
    }

    void add(CardTypeId card, int delta) {
        const int next = static_cast<int>(get(card)) + delta;
        if (next < 0 || next > std::numeric_limits<std::uint16_t>::max()) {
            throw std::out_of_range("card count out of range");
        }
        set(card, static_cast<std::uint16_t>(next));
    }

    [[nodiscard]] int total() const {
        return std::accumulate(entries_.begin(), entries_.end(), 0,
            [](int total, const CountEntry& entry) { return total + entry.count; });
    }

    [[nodiscard]] bool contains(const PackedCounts& subset) const {
        for (const auto& entry : subset.entries_) {
            if (get(entry.card) < entry.count) return false;
        }
        return true;
    }

    void subtract(const PackedCounts& other) {
        if (!contains(other)) throw std::invalid_argument("count underflow");
        for (const auto& entry : other.entries_) add(entry.card, -entry.count);
    }

    void merge(const PackedCounts& other) {
        for (const auto& entry : other.entries_) add(entry.card, entry.count);
    }

    [[nodiscard]] std::span<const CountEntry> entries() const noexcept { return entries_; }
    friend bool operator==(const PackedCounts&, const PackedCounts&) = default;
    friend bool operator<(const PackedCounts& lhs, const PackedCounts& rhs) {
        return std::lexicographical_compare(
            lhs.entries_.begin(), lhs.entries_.end(), rhs.entries_.begin(), rhs.entries_.end(),
            [](const CountEntry& a, const CountEntry& b) {
                return a.card < b.card || (a.card == b.card && a.count < b.count);
            });
    }

private:
    std::vector<CountEntry> entries_;

    std::vector<CountEntry>::iterator lowerBound(CardTypeId card) {
        return std::lower_bound(entries_.begin(), entries_.end(), card,
            [](const CountEntry& entry, CardTypeId id) { return entry.card < id; });
    }
    std::vector<CountEntry>::const_iterator lowerBound(CardTypeId card) const {
        return std::lower_bound(entries_.begin(), entries_.end(), card,
            [](const CountEntry& entry, CardTypeId id) { return entry.card < id; });
    }

    void canonicalize() {
        std::sort(entries_.begin(), entries_.end(),
            [](const CountEntry& a, const CountEntry& b) { return a.card < b.card; });
        std::vector<CountEntry> merged;
        for (const auto& entry : entries_) {
            if (entry.count == 0) continue;
            if (!merged.empty() && merged.back().card == entry.card) {
                const auto sum = static_cast<unsigned>(merged.back().count) + entry.count;
                if (sum > std::numeric_limits<std::uint16_t>::max()) {
                    throw std::overflow_error("card count overflow");
                }
                merged.back().count = static_cast<std::uint16_t>(sum);
            } else {
                merged.push_back(entry);
            }
        }
        entries_ = std::move(merged);
    }
};

struct PackedCountsHash {
    std::size_t operator()(const PackedCounts& value) const noexcept {
        std::size_t seed = 1469598103934665603ull;
        for (const auto& entry : value.entries()) {
            seed ^= entry.card;
            seed *= 1099511628211ull;
            seed ^= entry.count;
            seed *= 1099511628211ull;
        }
        return seed;
    }
};

inline std::uint64_t combination(unsigned n, unsigned k) {
    if (k > n) return 0;
    k = std::min(k, n - k);
    std::uint64_t result = 1;
    for (unsigned i = 1; i <= k; ++i) {
        auto numerator = static_cast<std::uint64_t>(n - k + i);
        auto denominator = static_cast<std::uint64_t>(i);
        const auto g1 = std::gcd(numerator, denominator);
        numerator /= g1;
        denominator /= g1;
        const auto g2 = std::gcd(result, denominator);
        result /= g2;
        denominator /= g2;
        if (denominator != 1 ||
            result > std::numeric_limits<std::uint64_t>::max() / numerator) {
            throw std::overflow_error("combination overflow");
        }
        result *= numerator;
    }
    return result;
}

struct CountOutcome {
    PackedCounts cards;
    Rational probability;
};

inline std::vector<CountOutcome> enumerateHypergeometric(
    const PackedCounts& pool, unsigned draws) {
    if (draws > static_cast<unsigned>(pool.total())) {
        throw std::invalid_argument("draw count exceeds pool");
    }
    const auto denominator = combination(static_cast<unsigned>(pool.total()), draws);
    std::vector<CountOutcome> result;
    std::vector<CountEntry> current;
    const auto entries = pool.entries();

    std::function<void(std::size_t, unsigned, std::uint64_t)> visit =
        [&](std::size_t index, unsigned remaining, std::uint64_t weight) {
            if (index == entries.size()) {
                if (remaining == 0) {
                    result.push_back({
                        PackedCounts(current),
                        Rational(static_cast<std::int64_t>(weight),
                                 static_cast<std::int64_t>(denominator))
                    });
                }
                return;
            }
            const auto& entry = entries[index];
            const unsigned maxTake = std::min<unsigned>(entry.count, remaining);
            for (unsigned take = 0; take <= maxTake; ++take) {
                const auto ways = combination(entry.count, take);
                if (ways != 0 && weight > std::numeric_limits<std::uint64_t>::max() / ways) {
                    throw std::overflow_error("hypergeometric weight overflow");
                }
                if (take != 0) current.push_back({entry.card, static_cast<std::uint16_t>(take)});
                visit(index + 1, remaining - take, weight * ways);
                if (take != 0) current.pop_back();
            }
        };
    visit(0, draws, 1);
    std::sort(result.begin(), result.end(),
        [](const CountOutcome& a, const CountOutcome& b) { return a.cards < b.cards; });
    return result;
}

} // namespace boundary_ai
