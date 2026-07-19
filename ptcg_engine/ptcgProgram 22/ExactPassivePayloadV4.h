// SPDX-FileCopyrightText: © Pokémon/Nintendo/Creatures/GAME FREAK TM, ®, and character names are trademarks of Nintendo.
// SPDX-License-Identifier: LicenseRef-PTCG-ABC-Competition-Use-Only
#pragma once

#include <algorithm>
#include <cstdint>
#include <utility>
#include <vector>

// Passive card multiset that affects evaluation only (never Control State TT keys).
struct ExactPassivePayloadV4 {
	static constexpr int SchemaVersion = 2;
	std::vector<std::pair<int, int>> counts; // sorted (cardId, count)
	std::uint64_t livenessProofHash = 0;
	int totalCount = 0;

	void clear() {
		counts.clear();
		livenessProofHash = 0;
		totalCount = 0;
	}

	void setCounts(std::vector<std::pair<int, int>> values, std::uint64_t proofHash) {
		counts = std::move(values);
		std::sort(counts.begin(), counts.end());
		livenessProofHash = proofHash;
		totalCount = 0;
		for (const auto& item : counts) totalCount += item.second;
	}

	int countOf(int cardId) const {
		auto found = std::lower_bound(counts.begin(), counts.end(), std::make_pair(cardId, 0),
			[](const auto& left, const auto& right) { return left.first < right.first; });
		if (found == counts.end() || found->first != cardId) return 0;
		return found->second;
	}

	bool empty() const { return totalCount <= 0; }
};

// Passive atoms must stay partitioned by their source continuation class unless
// both hand semantics and deck-removal semantics are proven identical.
struct ExactPassivePoolBySourceClassV4 {
	int sourceClassId = -1;
	std::vector<std::pair<int, int>> atoms; // (cardId, copies in that class)
	int count = 0;
	std::uint64_t proofHash = 0;
	bool deckRemovalInvariant = false;
	bool handTargetInvariant = false;
};

struct ExactPassivePairWeightV4 {
	std::uint16_t cardA = 0;
	std::uint16_t cardB = 0;
	std::int32_t weight = 0;
};
