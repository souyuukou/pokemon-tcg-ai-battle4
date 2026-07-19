#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <limits>

namespace boundary_ai {

struct SearchBudget {
    std::uint64_t maxExpandedNodes = std::numeric_limits<std::uint64_t>::max();
    std::size_t maxTranspositionEntries = 1'000'000;
    std::chrono::steady_clock::time_point deadline =
        std::chrono::steady_clock::time_point::max();

    [[nodiscard]] bool expired(std::uint64_t expandedNodes) const {
        return expandedNodes >= maxExpandedNodes ||
               std::chrono::steady_clock::now() >= deadline;
    }
};

struct SearchMetrics {
    std::uint64_t expandedNodes = 0;
    std::uint64_t boundaryEvaluations = 0;
    std::uint64_t transpositionHits = 0;
    std::uint64_t cycleCuts = 0;
    std::uint64_t decisionNodes = 0;
    std::uint64_t chanceNodes = 0;
    std::uint64_t observationNodes = 0;
    std::uint64_t boundaryNodes = 0;
    std::uint64_t evictedEntries = 0;
    std::uint64_t probabilityChecks = 0;
};

} // namespace boundary_ai
