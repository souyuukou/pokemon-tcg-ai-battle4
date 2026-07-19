#pragma once

#include "boundary_ai/belief.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <vector>

namespace boundary_ai {

using BoardStateId = std::uint64_t;
using PendingEffectId = std::uint32_t;

struct TurnResources {
    bool supporterAvailable = true;
    bool energyAttachmentAvailable = true;
    bool retreatAvailable = true;
    bool stadiumAvailable = true;
    std::uint8_t openBenchSlots = 0;
    std::vector<std::uint32_t> usedAbilities;

    void canonicalize() {
        std::sort(usedAbilities.begin(), usedAbilities.end());
        usedAbilities.erase(std::unique(usedAbilities.begin(), usedAbilities.end()),
                            usedAbilities.end());
    }
    friend bool operator==(const TurnResources&, const TurnResources&) = default;
};

struct ObservationState {
    std::uint64_t publicFingerprint = 0;
    std::uint64_t privateFingerprint = 0;
    std::uint8_t observingPlayer = 0;
    friend bool operator==(const ObservationState&, const ObservationState&) = default;
};

struct InformationState {
    BoardStateId board = 0;
    PackedCounts ownHand;
    BeliefStateId hiddenInformation = 0;
    TurnResources resources;
    ObservationState observation;
    PendingEffectId pendingEffect = 0;

    void canonicalize() { resources.canonicalize(); }
    friend bool operator==(const InformationState&, const InformationState&) = default;
};

struct InformationStateHash {
    std::size_t operator()(const InformationState& state) const noexcept {
        PackedCountsHash countHash;
        std::size_t seed = static_cast<std::size_t>(state.board);
        auto mix = [&](std::size_t value) {
            seed ^= value + 0x9e3779b97f4a7c15ull + (seed << 6) + (seed >> 2);
        };
        mix(countHash(state.ownHand));
        mix(state.hiddenInformation);
        mix(state.resources.supporterAvailable);
        mix(state.resources.energyAttachmentAvailable);
        mix(state.resources.retreatAvailable);
        mix(state.resources.stadiumAvailable);
        mix(state.resources.openBenchSlots);
        for (const auto ability : state.resources.usedAbilities) mix(ability);
        mix(static_cast<std::size_t>(state.observation.publicFingerprint));
        mix(static_cast<std::size_t>(state.observation.privateFingerprint));
        mix(state.observation.observingPlayer);
        mix(state.pendingEffect);
        return seed;
    }
};

} // namespace boundary_ai
