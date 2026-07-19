// SPDX-FileCopyrightText: © Pokémon/Nintendo/Creatures/GAME FREAK TM, ®, and character names are trademarks of Nintendo.
// SPDX-License-Identifier: LicenseRef-PTCG-ABC-Competition-Use-Only
#pragma once

#include "ExactBigRational.h"
#include "ExactPassivePayloadV4.h"

#include <stdexcept>
#include <unordered_map>
#include <utility>
#include <vector>

// Exact hypergeometric expectations for Passive Residual integration.
// Floating point is forbidden. V4.0 residual is context-free (beta + pairs only).
namespace ExactPassiveExpectationV4 {

inline ExactWeight Choose(int n, int k) {
	if (k < 0 || n < 0 || k > n) return ExactWeight();
	if (k == 0 || k == n) return ExactWeight(1);
	if (k > n - k) k = n - k;
	ExactWeight result(1);
	for (int i = 1; i <= k; ++i) {
		result = ExactWeight::multiply(result, ExactWeight((unsigned long long)(n - k + i)));
		auto div = ExactWeight::divideRemainder(result, ExactWeight((unsigned long long)i));
		if (!div.second.zero()) throw std::runtime_error("choose not divisible");
		result = div.first;
	}
	return result;
}

inline ExactBigRational Ratio(ExactWeight numerator, ExactWeight denominator) {
	if (denominator.zero()) throw std::runtime_error("zero denominator");
	if (numerator.zero()) return ExactBigRational(0, 1);
	ExactBigRational value(1, 1);
	value.scale(numerator, denominator);
	return value;
}

inline ExactBigRational ExpectedCount(int poolSize, int take, int cardCopies) {
	if (poolSize <= 0 || take <= 0 || cardCopies <= 0) return ExactBigRational(0, 1);
	ExactWeight num = ExactWeight::multiply(ExactWeight((unsigned long long)take),
		ExactWeight((unsigned long long)cardCopies));
	return Ratio(std::move(num), ExactWeight((unsigned long long)poolSize));
}

inline ExactBigRational ExpectedProductDistinct(int poolSize, int take, int copiesI, int copiesJ) {
	if (poolSize <= 1 || take <= 1 || copiesI <= 0 || copiesJ <= 0) return ExactBigRational(0, 1);
	ExactWeight num = ExactWeight::multiply(ExactWeight((unsigned long long)take),
		ExactWeight((unsigned long long)(take - 1)));
	num = ExactWeight::multiply(num, ExactWeight((unsigned long long)copiesI));
	num = ExactWeight::multiply(num, ExactWeight((unsigned long long)copiesJ));
	ExactWeight den = ExactWeight::multiply(ExactWeight((unsigned long long)poolSize),
		ExactWeight((unsigned long long)(poolSize - 1)));
	return Ratio(std::move(num), std::move(den));
}

inline ExactBigRational ExpectedChoose2(int poolSize, int take, int copiesI) {
	if (poolSize <= 1 || take <= 1 || copiesI <= 1) return ExactBigRational(0, 1);
	ExactWeight num = ExactWeight::multiply(Choose(take, 2), Choose(copiesI, 2));
	ExactWeight den = Choose(poolSize, 2);
	return Ratio(std::move(num), std::move(den));
}

inline ExactBigRational ScaleRational(ExactBigRational value, long long coeff) {
	if (coeff == 0 || value.numerator.sign == 0) return ExactBigRational(0, 1);
	const unsigned long long mag = (unsigned long long)(coeff < 0 ? -coeff : coeff);
	value.scale(ExactWeight(mag), ExactWeight(1));
	if (coeff < 0) value.numerator.sign = -value.numerator.sign;
	return value;
}

inline ExactBigRational IntegerRational(long long value) {
	if (value == 0) return ExactBigRational(0, 1);
	ExactBigRational out(value < 0 ? -1 : 1, 1);
	ExactWeight mag((unsigned long long)(value < 0 ? -value : value));
	out.scale(mag, ExactWeight(1));
	return out;
}

inline long long ValueOf(const std::vector<std::pair<int, long long>>& values, int cardId) {
	for (const auto& item : values) if (item.first == cardId) return item.second;
	return 0;
}

inline int CountOf(const std::vector<std::pair<int, int>>& counts, int cardId) {
	for (const auto& item : counts) if (item.first == cardId) return item.second;
	return 0;
}

// Draw-only residual (legacy helper). Prefer ExpectedPassiveResidualWithBase.
inline ExactBigRational ExpectedPassiveResidual(
	int passivePool, int passiveTake,
	const std::vector<std::pair<int, long long>>& cardValueById,
	const std::vector<ExactPassivePairWeightV4>& pairs,
	const std::vector<std::pair<int, int>>& passiveCopies) {
	ExactBigRational total(0, 1);
	if (passivePool <= 0 || passiveTake <= 0) return total;
	for (const auto& item : passiveCopies) {
		long long ri = ValueOf(cardValueById, item.first);
		if (ri == 0) continue;
		total = ExactBigRational::add(total,
			ScaleRational(ExpectedCount(passivePool, passiveTake, item.second), ri));
	}
	for (const auto& pair : pairs) {
		if (pair.weight == 0) continue;
		int na = CountOf(passiveCopies, pair.cardA), nb = CountOf(passiveCopies, pair.cardB);
		ExactBigRational expected = (pair.cardA == pair.cardB)
			? ExpectedChoose2(passivePool, passiveTake, na)
			: ExpectedProductDistinct(passivePool, passiveTake, na, nb);
		total = ExactBigRational::add(total, ScaleRational(std::move(expected), pair.weight));
	}
	return total;
}

// P0-4: base hand counts e_i plus draw X_i.
// E[e+X]=e+E[X]
// E[(e_i+X_i)(e_j+X_j)] = e_i e_j + e_i E[X_j] + e_j E[X_i] + E[X_i X_j]
// E[C(e+X,2)] = C(e,2) + e E[X] + E[C(X,2)]
inline ExactBigRational ExpectedPassiveResidualWithBase(
	const std::vector<std::pair<int, int>>& basePassiveCounts,
	const std::vector<std::pair<int, int>>& drawPopulationCounts,
	int drawCount,
	const std::vector<std::pair<int, long long>>& cardValueById,
	const std::vector<ExactPassivePairWeightV4>& pairs) {
	ExactBigRational total(0, 1);
	int pool = 0;
	for (const auto& item : drawPopulationCounts) pool += item.second;

	auto expectedXi = [&](int cardId) -> ExactBigRational {
		return ExpectedCount(pool, drawCount, CountOf(drawPopulationCounts, cardId));
	};
	auto expectedXiXj = [&](int a, int b) -> ExactBigRational {
		if (a == b) return ExactBigRational(0, 1);
		return ExpectedProductDistinct(pool, drawCount,
			CountOf(drawPopulationCounts, a), CountOf(drawPopulationCounts, b));
	};
	auto expectedChoose2Xi = [&](int cardId) -> ExactBigRational {
		return ExpectedChoose2(pool, drawCount, CountOf(drawPopulationCounts, cardId));
	};

	// Singleton: sum_i beta_i * E[e_i + X_i]
	std::unordered_map<int, bool> seen;
	auto considerCard = [&](int cardId) {
		if (seen[cardId]) return;
		seen[cardId] = true;
		long long beta = ValueOf(cardValueById, cardId);
		if (beta == 0) return;
		const int e = CountOf(basePassiveCounts, cardId);
		ExactBigRational term = IntegerRational(e);
		term = ExactBigRational::add(term, expectedXi(cardId));
		total = ExactBigRational::add(total, ScaleRational(std::move(term), beta));
	};
	for (const auto& item : basePassiveCounts) considerCard(item.first);
	for (const auto& item : drawPopulationCounts) considerCard(item.first);

	for (const auto& pair : pairs) {
		if (pair.weight == 0) continue;
		const int ei = CountOf(basePassiveCounts, pair.cardA);
		const int ej = CountOf(basePassiveCounts, pair.cardB);
		if (pair.cardA == pair.cardB) {
			// E[C(e+X,2)] = C(e,2) + e E[X] + E[C(X,2)]
			ExactBigRational term = IntegerRational((long long)ei * (ei - 1) / 2);
			term = ExactBigRational::add(term,
				ScaleRational(expectedXi(pair.cardA), ei));
			term = ExactBigRational::add(term, expectedChoose2Xi(pair.cardA));
			total = ExactBigRational::add(total, ScaleRational(std::move(term), pair.weight));
		} else {
			ExactBigRational term = IntegerRational((long long)ei * ej);
			term = ExactBigRational::add(term, ScaleRational(expectedXi(pair.cardB), ei));
			term = ExactBigRational::add(term, ScaleRational(expectedXi(pair.cardA), ej));
			term = ExactBigRational::add(term, expectedXiXj(pair.cardA, pair.cardB));
			total = ExactBigRational::add(total, ScaleRational(std::move(term), pair.weight));
		}
	}
	return total;
}

inline ExactBigRational MultiplyRational(const ExactBigRational& a, const ExactBigRational& b) {
	if (a.numerator.sign == 0 || b.numerator.sign == 0) return ExactBigRational(0, 1);
	ExactBigRational out = a;
	out.numerator.multiply(b.numerator.magnitude);
	out.numerator.sign *= b.numerator.sign;
	for (size_t i = 0; i < ExactBigRational::Primes.size(); ++i)
		out.denominator[i] = (std::uint16_t)(out.denominator[i] + b.denominator[i]);
	return out;
}

struct PassiveDrawPool {
	std::vector<std::pair<int, int>> copies;
	int take = 0;
};

// Multiple source-class Passive pools with fixed class takes (conditionally independent).
inline ExactBigRational ExpectedPassiveResidual(
	const std::vector<std::pair<int, int>>& basePassiveCounts,
	const std::vector<PassiveDrawPool>& pools,
	const std::vector<std::pair<int, long long>>& cardValueById,
	const std::vector<ExactPassivePairWeightV4>& pairs) {
	if (pools.empty()) {
		return ExpectedPassiveResidualWithBase(basePassiveCounts, {}, 0, cardValueById, pairs);
	}
	if (pools.size() == 1) {
		return ExpectedPassiveResidualWithBase(basePassiveCounts, pools[0].copies, pools[0].take,
			cardValueById, pairs);
	}

	auto expectedXi = [&](int cardId) -> ExactBigRational {
		ExactBigRational sum(0, 1);
		for (const auto& pool : pools) {
			int size = 0;
			for (const auto& item : pool.copies) size += item.second;
			sum = ExactBigRational::add(sum,
				ExpectedCount(size, pool.take, CountOf(pool.copies, cardId)));
		}
		return sum;
	};
	auto expectedXiXj = [&](int a, int b) -> ExactBigRational {
		if (a == b) return ExactBigRational(0, 1);
		ExactBigRational sum(0, 1);
		std::vector<ExactBigRational> meanA, meanB;
		meanA.reserve(pools.size());
		meanB.reserve(pools.size());
		for (const auto& pool : pools) {
			int size = 0;
			for (const auto& item : pool.copies) size += item.second;
			sum = ExactBigRational::add(sum, ExpectedProductDistinct(size, pool.take,
				CountOf(pool.copies, a), CountOf(pool.copies, b)));
			meanA.push_back(ExpectedCount(size, pool.take, CountOf(pool.copies, a)));
			meanB.push_back(ExpectedCount(size, pool.take, CountOf(pool.copies, b)));
		}
		for (size_t i = 0; i < pools.size(); ++i)
			for (size_t j = i + 1; j < pools.size(); ++j) {
				sum = ExactBigRational::add(sum, MultiplyRational(meanA[i], meanB[j]));
				sum = ExactBigRational::add(sum, MultiplyRational(meanB[i], meanA[j]));
			}
		return sum;
	};
	auto expectedChoose2Xi = [&](int cardId) -> ExactBigRational {
		ExactBigRational sum(0, 1);
		std::vector<ExactBigRational> means;
		means.reserve(pools.size());
		for (const auto& pool : pools) {
			int size = 0;
			for (const auto& item : pool.copies) size += item.second;
			sum = ExactBigRational::add(sum,
				ExpectedChoose2(size, pool.take, CountOf(pool.copies, cardId)));
			means.push_back(ExpectedCount(size, pool.take, CountOf(pool.copies, cardId)));
		}
		for (size_t i = 0; i < means.size(); ++i)
			for (size_t j = i + 1; j < means.size(); ++j)
				sum = ExactBigRational::add(sum, MultiplyRational(means[i], means[j]));
		return sum;
	};

	ExactBigRational total(0, 1);
	std::unordered_map<int, bool> seen;
	auto considerCard = [&](int cardId) {
		if (seen[cardId]) return;
		seen[cardId] = true;
		long long beta = ValueOf(cardValueById, cardId);
		if (beta == 0) return;
		const int e = CountOf(basePassiveCounts, cardId);
		ExactBigRational term = IntegerRational(e);
		term = ExactBigRational::add(term, expectedXi(cardId));
		total = ExactBigRational::add(total, ScaleRational(std::move(term), beta));
	};
	for (const auto& item : basePassiveCounts) considerCard(item.first);
	for (const auto& pool : pools)
		for (const auto& item : pool.copies) considerCard(item.first);

	for (const auto& pair : pairs) {
		if (pair.weight == 0) continue;
		const int ei = CountOf(basePassiveCounts, pair.cardA);
		const int ej = CountOf(basePassiveCounts, pair.cardB);
		if (pair.cardA == pair.cardB) {
			ExactBigRational term = IntegerRational((long long)ei * (ei - 1) / 2);
			term = ExactBigRational::add(term, ScaleRational(expectedXi(pair.cardA), ei));
			term = ExactBigRational::add(term, expectedChoose2Xi(pair.cardA));
			total = ExactBigRational::add(total, ScaleRational(std::move(term), pair.weight));
		} else {
			ExactBigRational term = IntegerRational((long long)ei * ej);
			term = ExactBigRational::add(term, ScaleRational(expectedXi(pair.cardB), ei));
			term = ExactBigRational::add(term, ScaleRational(expectedXi(pair.cardA), ej));
			term = ExactBigRational::add(term, expectedXiXj(pair.cardA, pair.cardB));
			total = ExactBigRational::add(total, ScaleRational(std::move(term), pair.weight));
		}
	}
	return total;
}

} // namespace ExactPassiveExpectationV4
