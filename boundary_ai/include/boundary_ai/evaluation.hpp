#pragma once

#include "boundary_ai/rational.hpp"

#include <array>
#include <cstdint>
#include <stdexcept>
#include <vector>

namespace boundary_ai {

// All fields are derived from player-visible state or a belief distribution.
// No concrete hidden-zone assignment has a slot in this type.
struct BoundaryFeatures {
    std::array<std::int16_t, 2> prizesRemaining{};
    std::array<std::int16_t, 2> handSize{};
    std::array<std::int16_t, 2> deckSize{};
    std::vector<std::int16_t> publicBoard;
    std::vector<Rational> beliefProbabilities;
    Rational nextTurnAttackProbability;
    Rational lossRisk;
};

class LinearBoundaryValue {
public:
    LinearBoundaryValue(std::vector<Rational> weights, Rational bias,
                        Rational minimum = Rational(-1),
                        Rational maximum = Rational(1))
        : weights_(std::move(weights)), bias_(std::move(bias)),
          minimum_(std::move(minimum)), maximum_(std::move(maximum)) {}

    [[nodiscard]] Rational operator()(const std::vector<Rational>& encoded) const {
        if (encoded.size() != weights_.size()) {
            throw std::invalid_argument("boundary feature size mismatch");
        }
        Rational value = bias_;
        for (std::size_t i = 0; i < encoded.size(); ++i) {
            value += encoded[i] * weights_[i];
        }
        if (value < minimum_) return minimum_;
        if (value > maximum_) return maximum_;
        return value;
    }

private:
    std::vector<Rational> weights_;
    Rational bias_;
    Rational minimum_;
    Rational maximum_;
};

} // namespace boundary_ai
