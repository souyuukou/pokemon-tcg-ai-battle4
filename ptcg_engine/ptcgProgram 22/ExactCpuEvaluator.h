#pragma once

#include <cstdint>
#include <cstdlib>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "ExactCardLivenessV4.h"
#include "ExactFeatureRecordV4.h"
#include "ExactPassivePayloadV4.h"
#include "ExactSparseEvaluatorV3.h"
#include "ExactSparseEvaluatorV4.h"
#include "ExactEvaluatorManifest.h"

enum class ExactEvaluatorVersion : unsigned char { V3 = 0, V4 = 1, Dual = 2 };

#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable: 4996)
#endif
inline const char* ExactGetenv(const char* name) { return std::getenv(name); }
#ifdef _MSC_VER
#pragma warning(pop)
#endif

inline ExactEvaluatorVersion ExactEvaluatorVersionFromEnvironment() {
	const char* configured = ExactGetenv("PTCG_EXACT_EVALUATOR_VERSION");
	if (configured == nullptr) return ExactEvaluatorVersion::V3;
	std::string value(configured);
	if (value == "V4" || value == "v4") return ExactEvaluatorVersion::V4;
	if (value == "Dual" || value == "dual" || value == "DUAL") return ExactEvaluatorVersion::Dual;
	return ExactEvaluatorVersion::V3;
}

inline bool ExactEnvFlagDisabled(const char* name) {
	const char* configured = ExactGetenv(name);
	return configured != nullptr && (configured[0] == '0' || configured[0] == 'n' || configured[0] == 'N');
}

inline bool ExactEnvFlagEnabled(const char* name) {
	const char* configured = ExactGetenv(name);
	return configured != nullptr && (configured[0] == '1' || configured[0] == 'y' || configured[0] == 'Y');
}

class ExactCpuEvaluator {
public:
	// Explicit dual-file load (P0-7). Filename guess is not used.
	bool loadV3AndV4(const std::string& v3Path, const std::string& v4Path, std::string& error) {
		if (!sparseV3.load(v3Path, error)) return false;
		if (!sparseV4.load(v4Path, error)) return false;
		if (!sparseV4.bindRequiredV3(sparseV3, error)) return false;
		sparseV4.attachV3Trunk(&sparseV3);
		version = ExactEvaluatorVersionFromEnvironment();
		if (ExactEnvFlagDisabled("PTCG_EXACT_EVALUATOR_V4_ENABLE_PASSIVE")) sparseV4.setEnablePassive(false);
		if (ExactEnvFlagDisabled("PTCG_EXACT_EVALUATOR_V4_ENABLE_PAIRS")) sparseV4.setEnablePairs(false);
		fallbackToV3 = !ExactEnvFlagDisabled("PTCG_EXACT_EVALUATOR_V4_FALLBACK_TO_V3");
		loaded = true;
		return true;
	}

	bool load(const std::string& path, std::string& error) {
		const char* v3Env = ExactGetenv("ExactEvaluatorV3File");
		const char* v4Env = ExactGetenv("ExactEvaluatorV4File");
		if (v3Env != nullptr && v4Env != nullptr && v3Env[0] && v4Env[0])
			return loadV3AndV4(v3Env, v4Env, error);
		if (v4Env != nullptr && v4Env[0]) {
			std::string v3Path = (v3Env != nullptr && v3Env[0]) ? std::string(v3Env) : path;
			return loadV3AndV4(v3Path, v4Env, error);
		}
		// Default: V3 only. Do not guess companion V4/V3 by filename rewrite.
		if (!sparseV3.load(path, error)) return false;
		sparseV4.bootstrapPassiveFromV3(sparseV3);
		version = ExactEvaluatorVersionFromEnvironment();
		if (ExactEnvFlagDisabled("PTCG_EXACT_EVALUATOR_V4_ENABLE_PASSIVE")) sparseV4.setEnablePassive(false);
		if (ExactEnvFlagDisabled("PTCG_EXACT_EVALUATOR_V4_ENABLE_PAIRS")) sparseV4.setEnablePairs(false);
		fallbackToV3 = !ExactEnvFlagDisabled("PTCG_EXACT_EVALUATOR_V4_FALLBACK_TO_V3");
		loaded = true;
		return true;
	}
	bool isLoaded() const { return loaded; }
	const std::string& path() const { return sparseV3.path(); }
	int schemaVersion() const {
		if (!loaded) return 0;
		return usesV4Search() ? ExactSparseEvaluatorV4::ModelSchemaVersion : ExactSparseEvaluatorV3::SchemaVersion;
	}

	bool usesV4Search() const { return version == ExactEvaluatorVersion::V4; }
	bool usesV4PassiveStrip() const {
		return version == ExactEvaluatorVersion::Dual || ExactEnvFlagEnabled("PTCG_EXACT_V4_PASSIVE_STRIP");
	}
	ExactEvaluatorVersion evaluatorVersion() const { return version; }
	bool informationSetSafe() const {
		return loaded && sparseV3.manifestInformationSetSafe()
			&& (!sparseV4.isLoaded() || !sparseV4.isStandalone() || sparseV4.manifestInformationSetSafe());
	}
	// Keep the two safety claims independent in diagnostics.  A model may have
	// an information-set-safe feature projection while still being trained for
	// intermediate states; callers must not infer boundary-only from the former.
	bool boundaryOnly() const {
		return loaded && sparseV3.manifestBoundaryOnly()
			&& (!sparseV4.isLoaded() || !sparseV4.isStandalone() || sparseV4.manifestBoundaryOnly());
	}
	std::uint64_t modelHash() const {
		return usesV4Search() ? sparseV4.modelHash() : sparseV3.modelHash();
	}
	std::uint64_t residentBytes() const {
		return sparseV3.residentBytes() + sparseV4.residentBytes();
	}

	bool evaluateV3Features(const ExactSparseEvaluatorV3::FeatureRecord& features, long long& value,
		unsigned long long* accumulatorHits = nullptr) const {
		if (!loaded || features.overflow) return false;
		value = sparseV3.evaluate(features, accumulatorHits); return true;
	}

	bool evaluateV4Features(const ExactSparseEvaluatorV3::FeatureRecord& features,
		const ExactPassivePayloadV4& passive, long long& value,
		unsigned long long* accumulatorHits = nullptr) const {
		if (!loaded || features.overflow || !sparseV4.isLoaded()) return false;
		value = sparseV4.evaluateV4(features, passive, accumulatorHits); return true;
	}

	bool evaluateV4FeaturesUnclamped(const ExactSparseEvaluatorV3::FeatureRecord& features,
		const ExactPassivePayloadV4& passive, long long& value,
		unsigned long long* accumulatorHits = nullptr) const {
		if (!loaded || features.overflow || !sparseV4.isLoaded()) return false;
		value = sparseV4.evaluateV4ExactUnclamped(features, passive, accumulatorHits); return true;
	}

	// Strip Passive identities into payload using mandatory operator closure.
	static void splitOwnHandFeatures(ExactSparseEvaluatorV3::FeatureRecord& features,
		const State& state, int actor, ExactPassivePayloadV4& passive,
		const ExactCardLivenessV4::OperatorClosure& closure) {
		std::unordered_map<int, int> handCounts;
		for (int i = 0; i < features.globalSparse.count; ++i) {
			const auto& item = features.globalSparse.values[i];
			if (item.relation != ExactSparseEvaluatorV3::OwnHand) continue;
			int copies = item.value / ExactSparseEvaluatorV3::BeliefScale;
			if (copies <= 0) copies = 1;
			handCounts[item.token] += copies;
		}
		auto split = ExactCardLivenessV4::SplitHandCounts(state, actor, handCounts, closure);
		passive.setCounts(split.passiveCounts, split.proofHash);
		std::unordered_set<int> passiveIds;
		for (const auto& item : split.passiveCounts) passiveIds.insert(item.first);
		auto record = ExactFeatureV4::BuildFromV3(features, passive, &passiveIds);
		features = record.semantic.features;
	}

	std::vector<std::int16_t> cardContinuationSignature(int cardId) const {
		return loaded ? sparseV3.cardContinuationSignature(cardId)
			: std::vector<std::int16_t>{ (std::int16_t)(cardId & 0x7fff),
				(std::int16_t)((unsigned)cardId >> 15) };
	}
	long long evaluate(const State& state, int actor,
		const std::unordered_map<int, int>* actorProfile = nullptr,
		const ExactSparseEvaluatorV3::BeliefInput* belief = nullptr) const {
		return sparseV3.evaluate(state, actor, actorProfile, belief);
	}
	const ExactSparseEvaluatorV4& v4() const { return sparseV4; }
	bool allowV3Fallback() const { return fallbackToV3; }
private:
	bool loaded = false;
	bool fallbackToV3 = true;
	ExactEvaluatorVersion version = ExactEvaluatorVersion::V3;
	ExactSparseEvaluatorV3 sparseV3;
	ExactSparseEvaluatorV4 sparseV4;
};
