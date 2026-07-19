#pragma once

#include <cstdint>
#include <limits>
#include <numeric>
#include <stdexcept>
#include <string>

namespace boundary_ai {

class Rational {
public:
    constexpr Rational() = default;
    constexpr explicit Rational(std::int64_t integer) : numerator_(integer) {}

    Rational(std::int64_t numerator, std::int64_t denominator)
        : numerator_(numerator), denominator_(denominator) {
        normalize();
    }

    [[nodiscard]] constexpr std::int64_t numerator() const noexcept { return numerator_; }
    [[nodiscard]] constexpr std::int64_t denominator() const noexcept { return denominator_; }
    [[nodiscard]] double toDouble() const noexcept {
        return static_cast<double>(numerator_) / static_cast<double>(denominator_);
    }
    [[nodiscard]] std::string toString() const {
        if (denominator_ == 1) return std::to_string(numerator_);
        return std::to_string(numerator_) + "/" + std::to_string(denominator_);
    }

    Rational& operator+=(const Rational& rhs) {
        const auto common = std::gcd(denominator_, rhs.denominator_);
        const auto leftScale = rhs.denominator_ / common;
        const auto rightScale = denominator_ / common;
        numerator_ = checkedAdd(checkedMultiply(numerator_, leftScale),
                                checkedMultiply(rhs.numerator_, rightScale));
        denominator_ = checkedMultiply(denominator_, leftScale);
        normalize();
        return *this;
    }

    Rational& operator-=(const Rational& rhs) {
        return *this += Rational(checkedNegate(rhs.numerator_), rhs.denominator_);
    }

    Rational& operator*=(const Rational& rhs) {
        auto a = numerator_;
        auto b = denominator_;
        auto c = rhs.numerator_;
        auto d = rhs.denominator_;
        const auto g1 = std::gcd(absUnsigned(a), static_cast<std::uint64_t>(d));
        const auto g2 = std::gcd(absUnsigned(c), static_cast<std::uint64_t>(b));
        a /= static_cast<std::int64_t>(g1);
        d /= static_cast<std::int64_t>(g1);
        c /= static_cast<std::int64_t>(g2);
        b /= static_cast<std::int64_t>(g2);
        numerator_ = checkedMultiply(a, c);
        denominator_ = checkedMultiply(b, d);
        normalize();
        return *this;
    }

    Rational& operator/=(const Rational& rhs) {
        if (rhs.numerator_ == 0) throw std::domain_error("division by zero");
        return *this *= Rational(rhs.denominator_, rhs.numerator_);
    }

    friend Rational operator+(Rational lhs, const Rational& rhs) { return lhs += rhs; }
    friend Rational operator-(Rational lhs, const Rational& rhs) { return lhs -= rhs; }
    friend Rational operator*(Rational lhs, const Rational& rhs) { return lhs *= rhs; }
    friend Rational operator/(Rational lhs, const Rational& rhs) { return lhs /= rhs; }
    friend Rational operator-(const Rational& value) {
        return Rational(checkedNegate(value.numerator_), value.denominator_);
    }

    friend bool operator==(const Rational&, const Rational&) = default;
    friend bool operator<(const Rational& lhs, const Rational& rhs) {
        // Reduction before cross multiplication prevents most avoidable overflow.
        const auto common = std::gcd(lhs.denominator_, rhs.denominator_);
        return checkedMultiply(lhs.numerator_, rhs.denominator_ / common)
             < checkedMultiply(rhs.numerator_, lhs.denominator_ / common);
    }
    friend bool operator>(const Rational& lhs, const Rational& rhs) { return rhs < lhs; }
    friend bool operator<=(const Rational& lhs, const Rational& rhs) { return !(rhs < lhs); }
    friend bool operator>=(const Rational& lhs, const Rational& rhs) { return !(lhs < rhs); }

private:
    std::int64_t numerator_ = 0;
    std::int64_t denominator_ = 1;

    static std::uint64_t absUnsigned(std::int64_t value) {
        if (value >= 0) return static_cast<std::uint64_t>(value);
        return static_cast<std::uint64_t>(-(value + 1)) + 1;
    }

    static std::int64_t checkedNegate(std::int64_t value) {
        if (value == std::numeric_limits<std::int64_t>::min()) {
            throw std::overflow_error("rational integer overflow");
        }
        return -value;
    }

    static std::int64_t checkedAdd(std::int64_t lhs, std::int64_t rhs) {
        if ((rhs > 0 && lhs > std::numeric_limits<std::int64_t>::max() - rhs) ||
            (rhs < 0 && lhs < std::numeric_limits<std::int64_t>::min() - rhs)) {
            throw std::overflow_error("rational integer overflow");
        }
        return lhs + rhs;
    }

    static std::int64_t checkedMultiply(std::int64_t lhs, std::int64_t rhs) {
        if (lhs == 0 || rhs == 0) return 0;
        if (lhs == -1 && rhs == std::numeric_limits<std::int64_t>::min()) {
            throw std::overflow_error("rational integer overflow");
        }
        if (rhs == -1 && lhs == std::numeric_limits<std::int64_t>::min()) {
            throw std::overflow_error("rational integer overflow");
        }
        if (lhs > 0) {
            if ((rhs > 0 && lhs > std::numeric_limits<std::int64_t>::max() / rhs) ||
                (rhs < 0 && rhs < std::numeric_limits<std::int64_t>::min() / lhs)) {
                throw std::overflow_error("rational integer overflow");
            }
        } else {
            if ((rhs > 0 && lhs < std::numeric_limits<std::int64_t>::min() / rhs) ||
                (rhs < 0 && lhs < std::numeric_limits<std::int64_t>::max() / rhs)) {
                throw std::overflow_error("rational integer overflow");
            }
        }
        return lhs * rhs;
    }

    void normalize() {
        if (denominator_ == 0) throw std::domain_error("zero rational denominator");
        if (denominator_ < 0) {
            numerator_ = checkedNegate(numerator_);
            denominator_ = checkedNegate(denominator_);
        }
        if (numerator_ == 0) {
            denominator_ = 1;
            return;
        }
        const auto divisor = std::gcd(absUnsigned(numerator_),
                                      static_cast<std::uint64_t>(denominator_));
        numerator_ /= static_cast<std::int64_t>(divisor);
        denominator_ /= static_cast<std::int64_t>(divisor);
    }
};

struct ExactScore {
    Rational lower;
    Rational upper;
    bool complete = false;

    [[nodiscard]] static ExactScore exact(Rational value) {
        return {value, value, true};
    }
    friend bool operator==(const ExactScore&, const ExactScore&) = default;
};

} // namespace boundary_ai
