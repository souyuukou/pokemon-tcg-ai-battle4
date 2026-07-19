#pragma once

#include <algorithm>
#include <array>
#include <cstdint>
#include <iomanip>
#include <limits>
#include <numeric>
#include <sstream>
#include <string>
#include <stdexcept>
#include <vector>

class ExactBigUnsigned {
public:
	static constexpr std::uint32_t Base = 1'000'000'000U;
	std::vector<std::uint32_t> digit;

	ExactBigUnsigned(unsigned long long value = 0) {
		while (value) { digit.push_back((std::uint32_t)(value % Base)); value /= Base; }
	}
	bool zero() const { return digit.empty(); }
	bool fitsUnsignedLongLong() const {
		if (digit.size() > 3) return false;
		unsigned long long value = 0;
		for (size_t i = digit.size(); i-- > 0;) {
			if (value > (std::numeric_limits<unsigned long long>::max() - digit[i]) / Base) return false;
			value = value * Base + digit[i];
		}
		return true;
	}
	unsigned long long unsignedLongLong() const {
		if (!fitsUnsignedLongLong()) throw std::overflow_error("exact integer does not fit uint64");
		unsigned long long value = 0;
		for (size_t i = digit.size(); i-- > 0;) value = value * Base + digit[i];
		return value;
	}
	void trim() { while (!digit.empty() && digit.back() == 0) digit.pop_back(); }
	static int compare(const ExactBigUnsigned& a, const ExactBigUnsigned& b) {
		if (a.digit.size() != b.digit.size()) return a.digit.size() < b.digit.size() ? -1 : 1;
		for (size_t i = a.digit.size(); i-- > 0;) if (a.digit[i] != b.digit[i]) return a.digit[i] < b.digit[i] ? -1 : 1;
		return 0;
	}
	static ExactBigUnsigned add(const ExactBigUnsigned& a, const ExactBigUnsigned& b) {
		ExactBigUnsigned out; out.digit.resize(std::max(a.digit.size(), b.digit.size()));
		std::uint64_t carry = 0;
		for (size_t i = 0; i < out.digit.size(); ++i) {
			std::uint64_t value = carry + (i < a.digit.size() ? a.digit[i] : 0) + (i < b.digit.size() ? b.digit[i] : 0);
			out.digit[i] = (std::uint32_t)(value % Base); carry = value / Base;
		}
		if (carry) out.digit.push_back((std::uint32_t)carry); return out;
	}
	static ExactBigUnsigned subtract(const ExactBigUnsigned& a, const ExactBigUnsigned& b) {
		ExactBigUnsigned out; out.digit.resize(a.digit.size()); std::int64_t borrow = 0;
		for (size_t i = 0; i < a.digit.size(); ++i) {
			std::int64_t value = (std::int64_t)a.digit[i] - (i < b.digit.size() ? b.digit[i] : 0) - borrow;
			if (value < 0) { value += Base; borrow = 1; } else borrow = 0;
			out.digit[i] = (std::uint32_t)value;
		}
		out.trim(); return out;
	}
	static ExactBigUnsigned multiply(const ExactBigUnsigned& a, const ExactBigUnsigned& b) {
		if (a.zero() || b.zero()) return {};
		ExactBigUnsigned out; out.digit.assign(a.digit.size() + b.digit.size(), 0);
		for (size_t i = 0; i < a.digit.size(); ++i) {
			std::uint64_t carry = 0;
			for (size_t j = 0; j < b.digit.size(); ++j) {
				std::uint64_t value = out.digit[i + j] + (std::uint64_t)a.digit[i] * b.digit[j] + carry;
				out.digit[i + j] = (std::uint32_t)(value % Base); carry = value / Base;
			}
			size_t at = i + b.digit.size();
			while (carry) {
				if (at == out.digit.size()) out.digit.push_back(0);
				std::uint64_t value = out.digit[at] + carry;
				out.digit[at++] = (std::uint32_t)(value % Base); carry = value / Base;
			}
		}
		out.trim(); return out;
	}
	static ExactBigUnsigned multiplySmall(const ExactBigUnsigned& a, std::uint32_t factor) {
		if (a.zero() || factor == 0) return {};
		ExactBigUnsigned out; out.digit.resize(a.digit.size());
		std::uint64_t carry = 0;
		for (size_t i = 0; i < a.digit.size(); ++i) {
			std::uint64_t value = (std::uint64_t)a.digit[i] * factor + carry;
			out.digit[i] = (std::uint32_t)(value % Base); carry = value / Base;
		}
		if (carry) out.digit.push_back((std::uint32_t)carry);
		return out;
	}
	std::uint32_t divideSmall(std::uint32_t divisor) {
		if (divisor == 0) throw std::invalid_argument("division by zero");
		std::uint64_t remainder = 0;
		for (size_t i = digit.size(); i-- > 0;) {
			std::uint64_t value = remainder * Base + digit[i];
			digit[i] = (std::uint32_t)(value / divisor); remainder = value % divisor;
		}
		trim(); return (std::uint32_t)remainder;
	}
	bool even() const { return zero() || (digit.front() & 1U) == 0; }
	void shiftRightOne() { divideSmall(2); }
	static std::pair<ExactBigUnsigned, ExactBigUnsigned> divideRemainder(
		const ExactBigUnsigned& dividend, const ExactBigUnsigned& divisor) {
		if (divisor.zero()) throw std::invalid_argument("division by zero");
		if (compare(dividend, divisor) < 0) return { ExactBigUnsigned(), dividend };
		ExactBigUnsigned quotient, remainder = dividend;
		ExactBigUnsigned multiple = divisor, power(1);
		while (compare(multiple, remainder) <= 0) {
			multiple = multiplySmall(multiple, 2);
			power = multiplySmall(power, 2);
		}
		multiple.shiftRightOne(); power.shiftRightOne();
		while (!power.zero()) {
			if (compare(multiple, remainder) <= 0) {
				remainder = subtract(remainder, multiple);
				quotient = add(quotient, power);
			}
			multiple.shiftRightOne(); power.shiftRightOne();
		}
		return { quotient, remainder };
	}
	static ExactBigUnsigned divide(const ExactBigUnsigned& dividend, const ExactBigUnsigned& divisor) {
		return divideRemainder(dividend, divisor).first;
	}
	static ExactBigUnsigned remainder(const ExactBigUnsigned& dividend, const ExactBigUnsigned& divisor) {
		return divideRemainder(dividend, divisor).second;
	}
	static ExactBigUnsigned gcd(ExactBigUnsigned a, ExactBigUnsigned b) {
		if (a.zero()) return b; if (b.zero()) return a;
		unsigned commonTwos = 0;
		while (a.even() && b.even()) { a.shiftRightOne(); b.shiftRightOne(); ++commonTwos; }
		while (a.even()) a.shiftRightOne();
		do {
			while (b.even()) b.shiftRightOne();
			if (compare(a, b) > 0) std::swap(a, b);
			b = subtract(b, a);
		} while (!b.zero());
		while (commonTwos--) a = multiplySmall(a, 2);
		return a;
	}
	unsigned bitLength() const {
		if (zero()) return 0;
		ExactBigUnsigned copy = *this; unsigned bits = 0;
		while (!copy.zero()) { copy.shiftRightOne(); ++bits; }
		return bits;
	}
	std::string text() const {
		if (zero()) return "0";
		std::ostringstream out; out << digit.back();
		for (size_t i = digit.size() - 1; i-- > 0;) out << std::setw(9) << std::setfill('0') << digit[i];
		return out.str();
	}
};

// Integer probability mass with a uint64 hot path.  Unlike ExactFraction,
// this type is used before division, where combinations of individually small
// binomial coefficients can already exceed 64 bits.
class ExactWeight {
public:
	ExactWeight(unsigned long long value = 0) : small(value) {}
	explicit ExactWeight(ExactBigUnsigned value) { assign(std::move(value)); }

	bool zero() const { return !large && small == 0; }
	bool isLarge() const { return large; }
	bool fitsUnsignedLongLong() const { return !large; }
	unsigned long long unsignedLongLong() const {
		if (large) throw std::overflow_error("exact weight does not fit uint64");
		return small;
	}
	ExactBigUnsigned magnitude() const { return large ? big : ExactBigUnsigned(small); }
	std::string text() const { return large ? big.text() : std::to_string(small); }
	unsigned bitLength() const {
		if (large) return big.bitLength();
		unsigned bits = 0; for (auto value = small; value; value >>= 1) ++bits; return bits;
	}

	static int compare(const ExactWeight& a, const ExactWeight& b) {
		if (!a.large && !b.large) return a.small == b.small ? 0 : (a.small < b.small ? -1 : 1);
		return ExactBigUnsigned::compare(a.magnitude(), b.magnitude());
	}
	static ExactWeight add(const ExactWeight& a, const ExactWeight& b) {
		if (!a.large && !b.large && b.small <= std::numeric_limits<unsigned long long>::max() - a.small)
			return ExactWeight(a.small + b.small);
		return ExactWeight(ExactBigUnsigned::add(a.magnitude(), b.magnitude()));
	}
	static ExactWeight subtract(const ExactWeight& a, const ExactWeight& b) {
		if (compare(a, b) < 0) throw std::underflow_error("negative exact weight");
		if (!a.large && !b.large) return ExactWeight(a.small - b.small);
		return ExactWeight(ExactBigUnsigned::subtract(a.magnitude(), b.magnitude()));
	}
	static ExactWeight multiply(const ExactWeight& a, const ExactWeight& b) {
		if (a.zero() || b.zero()) return ExactWeight();
		if (!a.large && !b.large && b.small <= std::numeric_limits<unsigned long long>::max() / a.small)
			return ExactWeight(a.small * b.small);
		return ExactWeight(ExactBigUnsigned::multiply(a.magnitude(), b.magnitude()));
	}
	static ExactWeight gcd(const ExactWeight& a, const ExactWeight& b) {
		if (!a.large && !b.large) return ExactWeight(std::gcd(a.small, b.small));
		return ExactWeight(ExactBigUnsigned::gcd(a.magnitude(), b.magnitude()));
	}
	static std::pair<ExactWeight, ExactWeight> divideRemainder(const ExactWeight& a, const ExactWeight& b) {
		if (b.zero()) throw std::invalid_argument("division by zero");
		if (!a.large && !b.large) return { ExactWeight(a.small / b.small), ExactWeight(a.small % b.small) };
		auto result = ExactBigUnsigned::divideRemainder(a.magnitude(), b.magnitude());
		return { ExactWeight(std::move(result.first)), ExactWeight(std::move(result.second)) };
	}
	static ExactWeight divide(const ExactWeight& a, const ExactWeight& b) {
		return divideRemainder(a, b).first;
	}
	static ExactWeight remainder(const ExactWeight& a, const ExactWeight& b) {
		return divideRemainder(a, b).second;
	}

	ExactWeight& operator+=(const ExactWeight& other) { *this = add(*this, other); return *this; }
	friend bool operator==(const ExactWeight& a, const ExactWeight& b) { return compare(a, b) == 0; }
	friend bool operator!=(const ExactWeight& a, const ExactWeight& b) { return !(a == b); }
	friend bool operator<(const ExactWeight& a, const ExactWeight& b) { return compare(a, b) < 0; }
	friend bool operator>=(const ExactWeight& a, const ExactWeight& b) { return compare(a, b) >= 0; }

private:
	unsigned long long small = 0;
	bool large = false;
	ExactBigUnsigned big;
	void assign(ExactBigUnsigned value) {
		if (value.fitsUnsignedLongLong()) { small = value.unsignedLongLong(); large = false; big = {}; }
		else { small = 0; large = true; big = std::move(value); }
	}
};

struct ExactBigSigned {
	int sign = 0;
	ExactBigUnsigned magnitude;
	ExactBigSigned(long long value = 0) {
		if (value != 0) {
			sign = value < 0 ? -1 : 1;
			unsigned long long mag = value < 0 ? (unsigned long long)(-(value + 1)) + 1 : (unsigned long long)value;
			magnitude = ExactBigUnsigned(mag);
		}
	}
	void multiply(const ExactBigUnsigned& factor) {
		magnitude = ExactBigUnsigned::multiply(magnitude, factor); if (magnitude.zero()) sign = 0;
	}
	static ExactBigSigned add(const ExactBigSigned& a, const ExactBigSigned& b) {
		if (a.sign == 0) return b; if (b.sign == 0) return a;
		ExactBigSigned out;
		if (a.sign == b.sign) { out.sign = a.sign; out.magnitude = ExactBigUnsigned::add(a.magnitude, b.magnitude); }
		else {
			int cmp = ExactBigUnsigned::compare(a.magnitude, b.magnitude);
			if (cmp == 0) return out;
			out.sign = cmp > 0 ? a.sign : b.sign;
			out.magnitude = cmp > 0 ? ExactBigUnsigned::subtract(a.magnitude, b.magnitude)
				: ExactBigUnsigned::subtract(b.magnitude, a.magnitude);
		}
		return out;
	}
	std::string text() const { return sign < 0 ? "-" + magnitude.text() : magnitude.text(); }
};

struct ExactBigRational {
	static constexpr std::array<unsigned, 17> Primes{ 2,3,5,7,11,13,17,19,23,29,31,37,41,43,47,53,59 };
	ExactBigSigned numerator;
	std::array<std::uint16_t, Primes.size()> denominator{};

	ExactBigRational(long long n = 0, unsigned long long d = 1) : numerator(n) { addDenominator(d); }
	void addDenominator(unsigned long long value) {
		for (size_t i = 0; i < Primes.size(); ++i) while (value % Primes[i] == 0) {
			value /= Primes[i]; denominator[i]++;
		}
		// Every probability denominator is composed from counts no greater than
		// DECK_SIZE. Reaching this branch indicates corrupted arithmetic input.
		if (value != 1) throw std::runtime_error("unsupported exact denominator prime");
	}
	void addDenominator(const ExactBigUnsigned& input) {
		ExactBigUnsigned value = input;
		for (size_t i = 0; i < Primes.size(); ++i) {
			while (!value.zero()) {
				ExactBigUnsigned quotient = value;
				if (quotient.divideSmall(Primes[i]) != 0) break;
				value = std::move(quotient); denominator[i]++;
			}
		}
		if (!value.zero() && !(value.digit.size() == 1 && value.digit[0] == 1))
			throw std::runtime_error("unsupported exact denominator prime");
	}
	static ExactBigUnsigned factorProduct(const std::array<std::uint16_t, Primes.size()>& exponent) {
		ExactBigUnsigned out(1);
		for (size_t i = 0; i < Primes.size(); ++i) {
			ExactBigUnsigned factor(Primes[i]), power(1);
			unsigned n = exponent[i];
			while (n) {
				if (n & 1U) power = ExactBigUnsigned::multiply(power, factor);
				n >>= 1U; if (n) factor = ExactBigUnsigned::multiply(factor, factor);
			}
			out = ExactBigUnsigned::multiply(out, power);
		}
		return out;
	}
	void scale(unsigned long long weight, unsigned long long total) {
		numerator.multiply(ExactBigUnsigned(weight)); addDenominator(total);
	}
	void scale(const ExactWeight& weight, const ExactWeight& total) {
		if (total.zero()) throw std::runtime_error("zero exact probability mass");
		numerator.multiply(weight.magnitude()); addDenominator(total.magnitude());
	}
	static ExactBigRational add(const ExactBigRational& a, const ExactBigRational& b) {
		ExactBigRational out; out.denominator.fill(0);
		std::array<std::uint16_t, Primes.size()> am{}, bm{};
		for (size_t i = 0; i < Primes.size(); ++i) {
			out.denominator[i] = std::max(a.denominator[i], b.denominator[i]);
			am[i] = out.denominator[i] - a.denominator[i]; bm[i] = out.denominator[i] - b.denominator[i];
		}
		ExactBigSigned an = a.numerator, bn = b.numerator;
		an.multiply(factorProduct(am)); bn.multiply(factorProduct(bm)); out.numerator = ExactBigSigned::add(an, bn); return out;
	}
	static int compare(const ExactBigRational& a, const ExactBigRational& b) {
		if (a.numerator.sign != b.numerator.sign) return a.numerator.sign < b.numerator.sign ? -1 : 1;
		if (a.numerator.sign == 0) return 0;
		std::array<std::uint16_t, Primes.size()> am{}, bm{};
		for (size_t i = 0; i < Primes.size(); ++i) {
			std::uint16_t common = std::max(a.denominator[i], b.denominator[i]);
			am[i] = common - a.denominator[i]; bm[i] = common - b.denominator[i];
		}
		ExactBigUnsigned av = ExactBigUnsigned::multiply(a.numerator.magnitude, factorProduct(am));
		ExactBigUnsigned bv = ExactBigUnsigned::multiply(b.numerator.magnitude, factorProduct(bm));
		int cmp = ExactBigUnsigned::compare(av, bv); return a.numerator.sign < 0 ? -cmp : cmp;
	}
	std::string denominatorText() const { return factorProduct(denominator).text(); }
};
