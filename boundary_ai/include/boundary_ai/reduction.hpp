#pragma once

#include "boundary_ai/counts.hpp"

#include <algorithm>
#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace boundary_ai {

struct PokemonSignature {
    CardTypeId card = 0;
    std::uint16_t hpRemaining = 0;
    PackedCounts attachedEnergy;
    CardTypeId tool = 0;
    std::uint64_t evolutionFingerprint = 0;
    std::uint64_t stateFingerprint = 0;
    friend bool operator==(const PokemonSignature&, const PokemonSignature&) = default;
    friend bool operator<(const PokemonSignature& a, const PokemonSignature& b) {
        if (a.card != b.card) return a.card < b.card;
        if (a.hpRemaining != b.hpRemaining) return a.hpRemaining < b.hpRemaining;
        if (a.attachedEnergy != b.attachedEnergy) return a.attachedEnergy < b.attachedEnergy;
        if (a.tool != b.tool) return a.tool < b.tool;
        if (a.evolutionFingerprint != b.evolutionFingerprint) {
            return a.evolutionFingerprint < b.evolutionFingerprint;
        }
        return a.stateFingerprint < b.stateFingerprint;
    }
};

struct PokemonClass {
    PokemonSignature signature;
    std::uint8_t multiplicity = 0;
};

inline std::vector<PokemonClass> groupSymmetricPokemon(
    std::vector<PokemonSignature> pokemon) {
    std::sort(pokemon.begin(), pokemon.end());
    std::vector<PokemonClass> result;
    for (auto& signature : pokemon) {
        if (!result.empty() && result.back().signature == signature) {
            ++result.back().multiplicity;
        } else {
            result.push_back({std::move(signature), 1});
        }
    }
    return result;
}

struct ActionFootprint {
    std::uint64_t reads = 0;
    std::uint64_t writes = 0;
    bool observes = false;
    bool randomizes = false;
    bool changesDeck = false;
    bool changesHand = false;
    bool changesBenchCapacity = false;
};

struct ReducibleAction {
    std::uint64_t canonicalId = 0;
    ActionFootprint footprint;
};

inline bool commute(const ReducibleAction& lhs, const ReducibleAction& rhs) {
    const auto& a = lhs.footprint;
    const auto& b = rhs.footprint;
    if (a.observes || b.observes || a.randomizes || b.randomizes ||
        a.changesDeck || b.changesDeck || a.changesHand || b.changesHand ||
        a.changesBenchCapacity || b.changesBenchCapacity) {
        return false;
    }
    return (a.writes & (b.reads | b.writes)) == 0 &&
           (b.writes & (a.reads | a.writes)) == 0;
}

// Keep only the canonical representative of adjacent independent actions.
inline bool violatesCanonicalOrder(
    const ReducibleAction& previous, const ReducibleAction& candidate) {
    return commute(previous, candidate) && candidate.canonicalId < previous.canonicalId;
}

} // namespace boundary_ai
