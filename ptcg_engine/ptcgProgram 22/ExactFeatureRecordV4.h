// SPDX-FileCopyrightText: © Pokémon/Nintendo/Creatures/GAME FREAK TM, ®, and character names are trademarks of Nintendo.
// SPDX-License-Identifier: LicenseRef-PTCG-ABC-Competition-Use-Only
#pragma once

#include "ExactPassivePayloadV4.h"
#include "ExactSparseEvaluatorV3.h"

#include <cstdint>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

// Explicit V4 feature split: Passive hand identities leave Semantic OwnHand only.
struct SemanticFeaturesV4 {
	ExactSparseEvaluatorV3::FeatureRecord features{};
	std::int32_t passiveHandTotal = 0;
	std::int32_t passivePokemonCount = 0;
	std::int32_t passiveTrainerCount = 0;
	std::int32_t passiveEnergyCount = 0;
	std::int32_t passiveDeckTotal = 0;
};

struct FeatureRecordV4 {
	SemanticFeaturesV4 semantic;
	ExactPassivePayloadV4 passive;
	bool overflow = false;
};

namespace ExactFeatureV4 {

static constexpr int SchemaVersion = 2;

// Full FeatureRecord bytes for representative-invariance guards and tests.
inline std::string SerializeSemanticFeatures(const ExactSparseEvaluatorV3::FeatureRecord& features) {
	std::string out;
	out.reserve(256 + (size_t)features.globalSparse.count * 16
		+ (size_t)features.entityCount * 64);
	out.append(reinterpret_cast<const char*>(features.globalDense.data()),
		features.globalDense.size() * sizeof(std::int32_t));
	out.append(reinterpret_cast<const char*>(&features.globalSparse.count),
		sizeof(features.globalSparse.count));
	for (int i = 0; i < features.globalSparse.count; ++i) {
		const auto& item = features.globalSparse.values[i];
		out.append(reinterpret_cast<const char*>(&item.token), sizeof(item.token));
		out.append(reinterpret_cast<const char*>(&item.relation), sizeof(item.relation));
		out.append(reinterpret_cast<const char*>(&item.value), sizeof(item.value));
	}
	out.push_back((char)features.entityCount);
	out.push_back((char)features.opponentInferenceVersion);
	out.push_back(features.overflow ? 1 : 0);
	for (int ei = 0; ei < features.entityCount; ++ei) {
		const auto& e = features.entities[ei];
		out.append(reinterpret_cast<const char*>(&e.pool), sizeof(e.pool));
		out.append(reinterpret_cast<const char*>(e.dense.data()),
			e.dense.size() * sizeof(std::int32_t));
		out.append(reinterpret_cast<const char*>(&e.sparse.count), sizeof(e.sparse.count));
		for (int si = 0; si < e.sparse.count; ++si) {
			const auto& item = e.sparse.values[si];
			out.append(reinterpret_cast<const char*>(&item.token), sizeof(item.token));
			out.append(reinterpret_cast<const char*>(&item.relation), sizeof(item.relation));
			out.append(reinterpret_cast<const char*>(&item.value), sizeof(item.value));
		}
	}
	return out;
}

inline ExactPassivePayloadV4 MergePassiveCounts(
	const ExactPassivePayloadV4& base,
	const std::vector<std::pair<int, int>>& drawn) {
	std::unordered_map<int, int> merged;
	for (const auto& item : base.counts) merged[item.first] += item.second;
	for (const auto& item : drawn) if (item.second > 0) merged[item.first] += item.second;
	std::vector<std::pair<int, int>> sorted(merged.begin(), merged.end());
	ExactPassivePayloadV4 out;
	out.setCounts(std::move(sorted), base.livenessProofHash);
	return out;
}

// V4.0: strip Passive copies from OwnHand only. Deck/prize/combo/hidden features that
// share the same card ID must remain — otherwise deck residual information vanishes.
inline FeatureRecordV4 BuildFromV3(
	const ExactSparseEvaluatorV3::FeatureRecord& source,
	const ExactPassivePayloadV4& passive,
	const std::unordered_set<int>* /*passiveIds*/ = nullptr) {
	FeatureRecordV4 out;
	out.semantic.features = source;
	out.passive = passive;
	out.overflow = source.overflow;
	out.semantic.passiveHandTotal = passive.totalCount;

	ExactSparseEvaluatorV3::FixedSparseList<ExactSparseEvaluatorV3::MaxGlobalSparse> kept;
	for (int i = 0; i < source.globalSparse.count; ++i) {
		const auto& item = source.globalSparse.values[i];
		if (item.relation != ExactSparseEvaluatorV3::OwnHand) {
			kept.push(item.token, item.relation, item.value);
			continue;
		}
		const int passiveCopies = passive.countOf(item.token);
		if (passiveCopies <= 0) {
			kept.push(item.token, item.relation, item.value);
			continue;
		}
		int copies = item.value / ExactSparseEvaluatorV3::BeliefScale;
		if (copies <= 0) copies = 1;
		const int keep = copies - passiveCopies;
		if (keep > 0)
			kept.push(item.token, item.relation, keep * ExactSparseEvaluatorV3::BeliefScale);
	}
	out.semantic.features.globalSparse = kept;
	return out;
}

} // namespace ExactFeatureV4
