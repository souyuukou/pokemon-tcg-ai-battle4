// SPDX-FileCopyrightText: © Pokémon/Nintendo/Creatures/GAME FREAK TM, ®, and character names are trademarks of Nintendo.
// SPDX-License-Identifier: LicenseRef-PTCG-ABC-Competition-Use-Only
#pragma once

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <limits>
#include <string>
#include <utility>
#include <vector>

#include "ExactCardLivenessV4.h"
#include "ExactPassivePayloadV4.h"
#include "ExactSparseEvaluatorV3.h"
#include "ExactEvaluatorManifest.h"

// V4 = V3 Semantic Trunk + Passive Residual (context-free bias + sparse pairs in V4.0).
class ExactSparseEvaluatorV4 {
public:
	static constexpr int ModelSchemaVersion = 2;
	static constexpr int FeatureSchemaVersion = 2;
	static constexpr int LivenessSchemaVersion = ExactCardLivenessV4::LivenessSchemaVersion;
	static constexpr int ContextHidden = 32;
	static constexpr int PassiveContextScale = 4096;
	static constexpr char Magic[8] = { 'P','T','C','G','E','V','4','\0' };

	struct SemanticForwardResult {
		long long semanticValue = 0;
		std::array<std::int32_t, ContextHidden> context{};
	};

	bool load(const std::string& path, std::string& error) {
		loaded = false; manifestSafe = false; modelHashValue = 0;
		std::ifstream in(path, std::ios::binary);
		if (!in) { error = "unable to open V4 model"; return false; }
		in.seekg(0, std::ios::end);
		const std::uint64_t fileSize = (std::uint64_t)in.tellg();
		in.seekg(0, std::ios::beg);
		Header header{};
		in.read(reinterpret_cast<char*>(&header), sizeof(header));
		if (!in || std::memcmp(header.magic, Magic, 8) != 0
			|| header.modelSchema != ModelSchemaVersion
			|| header.featureSchema != FeatureSchemaVersion
			|| header.livenessSchema != (std::uint32_t)LivenessSchemaVersion
			|| header.contextHidden != ContextHidden
			|| header.passiveContextScale != PassiveContextScale
			|| header.tokenCount == 0
			|| (header.expectedFileSize != 0 && header.expectedFileSize != fileSize)) {
			error = "invalid V4 model header";
			return false;
		}
		// proven bounds verified after payload via recomputeProvenBoundsFromWeights()
		tokens.resize(header.tokenCount);
		in.read(reinterpret_cast<char*>(tokens.data()), (std::streamsize)(tokens.size() * sizeof(std::int32_t)));
		passiveBias.assign(header.tokenCount, 0);
		in.read(reinterpret_cast<char*>(passiveBias.data()), (std::streamsize)(passiveBias.size() * sizeof(std::int32_t)));
		passiveContextWeight.assign(header.tokenCount, {});
		for (std::uint32_t i = 0; i < header.tokenCount; ++i)
			in.read(reinterpret_cast<char*>(passiveContextWeight[i].data()),
				(std::streamsize)(ContextHidden * sizeof(std::int16_t)));
		contextFromGlobal.assign((size_t)ContextHidden * ExactSparseEvaluatorV3::GlobalHiddenCount, 0);
		in.read(reinterpret_cast<char*>(contextFromGlobal.data()),
			(std::streamsize)(contextFromGlobal.size() * sizeof(std::int16_t)));
		contextBias.assign(ContextHidden, 0);
		in.read(reinterpret_cast<char*>(contextBias.data()), (std::streamsize)(ContextHidden * sizeof(std::int32_t)));
		passivePairs.resize(header.pairCount);
		if (header.pairCount)
			in.read(reinterpret_cast<char*>(passivePairs.data()),
				(std::streamsize)(header.pairCount * sizeof(ExactPassivePairWeightV4)));
		if (!in) { error = "truncated V4 model"; return false; }
		in.read(reinterpret_cast<char*>(&manifest), sizeof(manifest));
		if (!in || !ExactEvaluatorManifestMatches(manifest, ModelSchemaVersion)) {
			error = "V4 evaluator manifest mismatch"; return false;
		}
		manifestSafe = true;

		const std::uint64_t payloadChecksum = computePayloadChecksum();
		if (header.payloadChecksum != 0 && header.payloadChecksum != payloadChecksum) {
			error = "V4 payload checksum mismatch";
			return false;
		}
		requiredV3ModelHash = header.requiredV3ModelHash;
		featureSchemaHash = header.featureSchemaHash;
		livenessSchemaHash = header.livenessSchemaHash;
		cardTokenTableHash = header.cardTokenTableHash;
		provenMinOutput = header.provenMinOutput;
		provenMaxOutput = header.provenMaxOutput;
		rebuildTokenIndex();
		recomputeProvenBoundsFromWeights();
		if (header.provenMinOutput != provenMinOutput || header.provenMaxOutput != provenMaxOutput) {
			error = "V4 proven bounds mismatch recomputation";
			return false;
		}
		// Saturating models load, but analyticIntegralAllowed stays false.
		modelHashValue = header.payloadChecksum != 0 ? header.payloadChecksum : header.legacyChecksum;
		modelPath = path;
		standalone = true;
		loaded = true;
		return true;
	}

	bool bindRequiredV3(const ExactSparseEvaluatorV3& trunk, std::string& error) {
		if (requiredV3ModelHash != 0 && requiredV3ModelHash != trunk.modelHash()) {
			error = "requiredV3ModelHash mismatch";
			return false;
		}
		const auto& table = trunk.tokenTable();
		std::uint64_t tokenHash = fnv1a(reinterpret_cast<const unsigned char*>(table.data()),
			table.size() * sizeof(std::int32_t));
		if (cardTokenTableHash != 0 && cardTokenTableHash != tokenHash) {
			error = "cardTokenTableHash mismatch";
			return false;
		}
		if (tokens.size() != table.size()) {
			error = "V4/V3 token table size mismatch";
			return false;
		}
		for (size_t i = 0; i < tokens.size(); ++i) {
			if (tokens[i] != table[i]) {
				error = "V4/V3 token table content mismatch";
				return false;
			}
		}
		return true;
	}

	void attachV3Trunk(const ExactSparseEvaluatorV3* trunk) {
		v3Trunk = trunk;
		if (trunk != nullptr) loaded = true;
	}

	void bootstrapPassiveFromV3(const ExactSparseEvaluatorV3& trunk) {
		v3Trunk = &trunk;
		const auto& table = trunk.tokenTable();
		tokens = table;
		rebuildTokenIndex();
		passiveBias.assign(tokens.size(), 0);
		passiveContextWeight.assign(tokens.size(), {});
		contextFromGlobal.assign((size_t)ContextHidden * ExactSparseEvaluatorV3::GlobalHiddenCount, 0);
		contextBias.assign(ContextHidden, 0);
		passivePairs.clear();
		long long minOut = 0, maxOut = 0;
		for (size_t i = 0; i < tokens.size(); ++i) {
			const int id = tokens[i];
			if (id <= 0 || id >= ExactSparseEvaluatorV3::AttackTokenBase) continue;
			long long score = trunk.estimateOwnHandLinearScore(id);
			if (score > std::numeric_limits<std::int32_t>::max()) score = std::numeric_limits<std::int32_t>::max();
			if (score < std::numeric_limits<std::int32_t>::min()) score = std::numeric_limits<std::int32_t>::min();
			passiveBias[i] = (std::int32_t)score;
			// Worst-case hand of 60 copies (impossible) still used only for static bound.
			long long contrib = score * 10;
			if (contrib < minOut) minOut = contrib;
			if (contrib > maxOut) maxOut = contrib;
		}
		requiredV3ModelHash = trunk.modelHash();
		cardTokenTableHash = fnv1a(reinterpret_cast<const unsigned char*>(tokens.data()),
			tokens.size() * sizeof(std::int32_t));
		featureSchemaHash = FeatureSchemaVersion;
		livenessSchemaHash = LivenessSchemaVersion;
		recomputeProvenBoundsFromWeights();
		standalone = false;
		loaded = true;
		modelHashValue = trunk.modelHash() ^ 0x56345F4254ULL; // V4BT
		(void)minOut; (void)maxOut;
	}

	bool isLoaded() const { return loaded; }
	bool isStandalone() const { return standalone; }
	bool manifestInformationSetSafe() const { return loaded && manifestSafe; }
	bool manifestBoundaryOnly() const { return loaded && manifestSafe && manifest.boundaryOnly != 0; }
	bool hasV3Trunk() const { return v3Trunk != nullptr; }
	int modelSchemaVersion() const { return ModelSchemaVersion; }
	int featureSchemaVersion() const { return FeatureSchemaVersion; }
	std::uint64_t modelHash() const { return modelHashValue; }
	const std::string& path() const { return modelPath; }
	size_t residentBytes() const {
		return tokens.size() * sizeof(tokens[0]) + passiveBias.size() * sizeof(passiveBias[0])
			+ passiveContextWeight.size() * sizeof(passiveContextWeight[0])
			+ contextFromGlobal.size() * sizeof(contextFromGlobal[0])
			+ contextBias.size() * sizeof(contextBias[0])
			+ passivePairs.size() * sizeof(ExactPassivePairWeightV4);
	}

	void setEnablePassive(bool enabled) { enablePassive = enabled; }
	void setEnablePairs(bool enabled) { enablePairs = enabled; }
	bool passiveEnabled() const { return enablePassive; }

	SemanticForwardResult forwardSemantic(const ExactSparseEvaluatorV3::FeatureRecord& features,
		unsigned long long* accumulatorHits = nullptr) const {
		SemanticForwardResult out;
		if (v3Trunk != nullptr && !features.overflow)
			out.semanticValue = v3Trunk->evaluate(features, accumulatorHits);
		// V4.0: frozen/context-free residual — do not build leaf context for residual.
		out.context.fill(0);
		for (int i = 0; i < ContextHidden && i < (int)contextBias.size(); ++i)
			out.context[(size_t)i] = 0;
		(void)contextFromGlobal;
		(void)contextBias;
		return out;
	}

	long long passiveCardValueContextFree(int cardId) const {
		const int index = indexFor(cardId);
		if (index < 0 || index >= (int)passiveBias.size()) return 0;
		return passiveBias[(size_t)index];
	}

	bool hasPassiveToken(int cardId) const { return indexFor(cardId) >= 0; }
	bool analyticIntegralSafe() const { return analyticIntegralAllowed; }
	std::int64_t provenMin() const { return provenMinOutput; }
	std::int64_t provenMax() const { return provenMaxOutput; }

	long long evaluatePassiveResidual(const ExactPassivePayloadV4& passive) const {
		if (!enablePassive || passive.empty()) return 0;
		long long value = 0;
		for (const auto& item : passive.counts)
			value += (long long)item.second * passiveCardValueContextFree(item.first);
		if (enablePairs) {
			for (const auto& pair : passivePairs) {
				const int a = passive.countOf(pair.cardA);
				const int b = passive.countOf(pair.cardB);
				if (a <= 0 || b <= 0) continue;
				if (pair.cardA == pair.cardB)
					value += (long long)pair.weight * ((long long)a * (a - 1) / 2);
				else
					value += (long long)pair.weight * (long long)a * (long long)b;
			}
		}
		return value;
	}

	long long evaluateV4(const ExactSparseEvaluatorV3::FeatureRecord& features,
		const ExactPassivePayloadV4& passive,
		unsigned long long* accumulatorHits = nullptr) const {
		const auto semantic = forwardSemantic(features, accumulatorHits);
		long long value = semantic.semanticValue + evaluatePassiveResidual(passive);
		if (value > ExactSparseEvaluatorV3::NonTerminalLimit) value = ExactSparseEvaluatorV3::NonTerminalLimit;
		if (value < -ExactSparseEvaluatorV3::NonTerminalLimit) value = -ExactSparseEvaluatorV3::NonTerminalLimit;
		return value;
	}

	// Exact Search path: no final clamp. Models must prove residual bounds in int64.
	long long evaluateV4ExactUnclamped(const ExactSparseEvaluatorV3::FeatureRecord& features,
		const ExactPassivePayloadV4& passive,
		unsigned long long* accumulatorHits = nullptr) const {
		const auto semantic = forwardSemantic(features, accumulatorHits);
		return semantic.semanticValue + evaluatePassiveResidual(passive);
	}

	std::vector<std::pair<int, long long>> passiveValueTableContextFree() const {
		std::vector<std::pair<int, long long>> out;
		out.reserve(tokens.size());
		for (size_t i = 0; i < tokens.size(); ++i) {
			const int id = tokens[i];
			if (id <= 0 || id >= ExactSparseEvaluatorV3::AttackTokenBase) continue;
			out.push_back({ id, passiveCardValueContextFree(id) });
		}
		return out;
	}

	const std::vector<ExactPassivePairWeightV4>& pairs() const { return passivePairs; }

private:
#pragma pack(push, 1)
	struct Header {
		char magic[8];
		std::uint32_t modelSchema, featureSchema, livenessSchema;
		std::uint32_t contextHidden, passiveContextScale, tokenCount, pairCount;
		std::uint64_t legacyChecksum;
		std::uint64_t requiredV3ModelHash;
		std::uint64_t featureSchemaHash;
		std::uint64_t livenessSchemaHash;
		std::uint64_t cardTokenTableHash;
		std::uint64_t payloadChecksum;
		std::uint64_t expectedFileSize;
		std::int64_t provenMinOutput;
		std::int64_t provenMaxOutput;
		std::uint8_t reserved[16];
	};
#pragma pack(pop)

	static std::uint64_t fnv1a(const unsigned char* data, size_t size) {
		std::uint64_t hash = 1469598103934665603ULL;
		for (size_t i = 0; i < size; ++i) {
			hash ^= data[i];
			hash *= 1099511628211ULL;
		}
		return hash;
	}

	std::uint64_t computePayloadChecksum() const {
		std::uint64_t hash = 1469598103934665603ULL;
		auto mix = [&](const void* ptr, size_t bytes) {
			const auto* data = reinterpret_cast<const unsigned char*>(ptr);
			for (size_t i = 0; i < bytes; ++i) {
				hash ^= data[i];
				hash *= 1099511628211ULL;
			}
		};
		mix(tokens.data(), tokens.size() * sizeof(std::int32_t));
		mix(passiveBias.data(), passiveBias.size() * sizeof(std::int32_t));
		for (const auto& row : passiveContextWeight)
			mix(row.data(), row.size() * sizeof(std::int16_t));
		mix(contextFromGlobal.data(), contextFromGlobal.size() * sizeof(std::int16_t));
		mix(contextBias.data(), contextBias.size() * sizeof(std::int32_t));
		mix(passivePairs.data(), passivePairs.size() * sizeof(ExactPassivePairWeightV4));
		return hash;
	}

	void recomputeProvenBoundsFromWeights() {
		constexpr int MaxHandCopies = 60; // DECK_SIZE; never under-bound residual proof
		long long lo = 0, hi = 0;
		for (std::int32_t bias : passiveBias) {
			if (bias >= 0) hi += (long long)bias * MaxHandCopies;
			else lo += (long long)bias * MaxHandCopies;
		}
		for (const auto& pair : passivePairs) {
			const long long w = pair.weight;
			const long long maxTerm = (pair.cardA == pair.cardB)
				? (long long)MaxHandCopies * (MaxHandCopies - 1) / 2
				: (long long)MaxHandCopies * MaxHandCopies;
			if (w >= 0) hi += w * maxTerm;
			else lo += w * maxTerm;
		}
		const long long lim = ExactSparseEvaluatorV3::NonTerminalLimit;
		provenMinOutput = lo;
		provenMaxOutput = hi;
		analyticIntegralAllowed = (lo > -lim) && (hi < lim);
	}

	void rebuildTokenIndex() {
		tokenIndex.clear();
		int maximum = 0;
		for (int token : tokens) if (token > maximum) maximum = token;
		tokenIndex.assign((size_t)maximum + 1, -1);
		for (size_t i = 0; i < tokens.size(); ++i)
			if (tokens[i] >= 0 && tokens[i] < (int)tokenIndex.size()) tokenIndex[(size_t)tokens[i]] = (int)i;
	}

	int indexFor(int token) const {
		if (token < 0 || token >= (int)tokenIndex.size()) return -1;
		return tokenIndex[(size_t)token];
	}

	bool loaded = false;
	bool standalone = false;
	bool enablePassive = true;
	bool enablePairs = true;
	bool analyticIntegralAllowed = false;
	std::uint64_t modelHashValue = 0;
	ExactEvaluatorManifestDisk manifest{};
	bool manifestSafe = false;
	std::uint64_t requiredV3ModelHash = 0;
	std::uint64_t featureSchemaHash = 0;
	std::uint64_t livenessSchemaHash = 0;
	std::uint64_t cardTokenTableHash = 0;
	std::int64_t provenMinOutput = 0;
	std::int64_t provenMaxOutput = 0;
	std::string modelPath;
	const ExactSparseEvaluatorV3* v3Trunk = nullptr;
	std::vector<std::int32_t> tokens;
	std::vector<int> tokenIndex;
	std::vector<std::int32_t> passiveBias;
	std::vector<std::array<std::int16_t, ContextHidden>> passiveContextWeight;
	std::vector<std::int16_t> contextFromGlobal;
	std::vector<std::int32_t> contextBias;
	std::vector<ExactPassivePairWeightV4> passivePairs;
};
