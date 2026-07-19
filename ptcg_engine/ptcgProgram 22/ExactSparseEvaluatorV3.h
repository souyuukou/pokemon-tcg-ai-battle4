#pragma once

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <limits>
#include <map>
#include <string>
#include <unordered_map>
#include <vector>

#include "ExactEvaluatorAvx2.h"

// V3 keeps the rule-observable record structured.  In particular, attached
// cards and effects live inside their Pokemon entity; they are never reduced
// to player totals.  Belief values are the only lossy boundary (Q8).
class ExactSparseEvaluatorV3 {
public:
	static constexpr int SchemaVersion = 3;
	static constexpr int InformationStateSchemaVersion = 3;
	static constexpr int EffectSchemaVersion = 1;
	static constexpr int ComboSchemaVersion = 1;
	static constexpr int BeliefScale = 256;
	static constexpr int WeightScale = 4096;
	static constexpr int ScoreScale = 100'000'000;
	static constexpr int NonTerminalLimit = 90'000'000;
	static constexpr int GlobalDenseCount = 16;
	static constexpr int EntityDenseCount = 16;
	static constexpr int EntityHiddenCount = 24;
	static constexpr int GlobalHiddenCount = 64;
	static constexpr int PoolCount = 4;
	static constexpr int GlobalRelationCount = 24;
	static constexpr int EntityRelationCount = 16;
	static constexpr int MaxEntities = 2 * (BENCH_SIZE_MAX + 1);
	static constexpr int MaxGlobalSparse = 512;
	static constexpr int MaxEntitySparse = 64;

	// Effect tokens are stable schema identifiers, not physical card ids.
	static constexpr int AttackTokenBase = 1'000'000;
	static constexpr int EffectTokenBase = 2'000'000;
	static constexpr int ComboTokenBase = 3'000'000;

	enum GlobalRelation : std::int16_t {
		OwnHand, OwnTrash, OppTrash, Stadium, OwnHiddenPool,
		OwnDeckExpected, OwnPrizeExpected, OwnDeckExists, OwnPrizeExists,
		OwnKnownDeck, OwnKnownPrize, OwnKnownTop0, OwnKnownTop1, OwnKnownTop2, OwnKnownTop3,
		OwnKnownBottom0, OwnKnownBottom1, OwnKnownBottom2, OwnKnownBottom3,
		GlobalEffect, OwnPlayerEffect, OppPlayerEffect, ComboProbability, PublicHistory
	};
	enum EntityRelation : std::int16_t {
		PokemonIdentity, Evolution0, Evolution1, Evolution2, Evolution3,
		AttachedEnergy, AttachedTool, AbilityUsed, AttackIdentity,
		EntityEffect, AttackExactDamage, AttackKo, AttackPrize, AttackUnavailable,
		AttackBenchDamage, AttackSelfDamage
	};
	enum Pool : std::int32_t { OwnActivePool, OppActivePool, OwnBenchPool, OppBenchPool };

	struct SparseInput {
		std::int32_t token = 0;
		std::int16_t relation = 0;
		std::int16_t reserved = 0;
		std::int32_t value = 0;
	};
	struct ExtractTiming {
		std::uint64_t publicNs = 0;
		std::uint64_t hiddenNs = 0;
		std::uint64_t entityNs = 0;
	};
	template<size_t Capacity>
	struct FixedSparseList {
		std::array<SparseInput, Capacity> values{};
		std::uint16_t count = 0;
		bool push(std::int32_t token, std::int16_t relation, std::int32_t value) {
			if (value == 0) return true;
			if (count >= Capacity) return false;
			values[count++] = { token, relation, 0, value }; return true;
		}
		void canonicalize() {
			std::sort(values.begin(), values.begin() + count, [](const SparseInput& a, const SparseInput& b) {
				if (a.relation != b.relation) return a.relation < b.relation;
				if (a.token != b.token) return a.token < b.token;
				return a.value < b.value;
			});
			std::uint16_t output = 0;
			for (std::uint16_t i = 0; i < count; ++i) {
				if (output > 0 && values[output - 1].relation == values[i].relation
					&& values[output - 1].token == values[i].token) values[output - 1].value += values[i].value;
				else values[output++] = values[i];
			}
			count = output;
		}
	};
	struct EntityRecord {
		std::array<std::int32_t, EntityDenseCount> dense{};
		FixedSparseList<MaxEntitySparse> sparse;
		std::int32_t pool = 0;
	};
	struct FeatureRecord {
		std::array<std::int32_t, GlobalDenseCount> globalDense{};
		FixedSparseList<MaxGlobalSparse> globalSparse;
		std::array<EntityRecord, MaxEntities> entities{};
		std::uint8_t entityCount = 0;
		std::uint8_t opponentInferenceVersion = 0;
		bool overflow = false;
	};
	static_assert(sizeof(FeatureRecord) <= 64 * 1024, "V3 worker feature scratch exceeds 64 KiB");
	struct BeliefInput {
		const std::unordered_map<int, int>* ownDeckQ8 = nullptr;
		const std::unordered_map<int, int>* ownPrizeQ8 = nullptr;
		const std::unordered_map<int, int>* ownDeckExistsQ8 = nullptr;
		const std::unordered_map<int, int>* ownPrizeExistsQ8 = nullptr;
		const std::unordered_map<int, int>* comboQ8 = nullptr;
		const std::map<int, int>* knownDeckCounts = nullptr;
		const std::map<int, int>* knownPrizeCounts = nullptr;
		const std::array<std::vector<int>, 2>* knownTop = nullptr;
		const std::array<std::vector<int>, 2>* knownBottom = nullptr;
	};

	enum class AttackPreviewStatus : std::uint8_t { Exact, Unavailable };
	struct AttackPreview {
		AttackPreviewStatus status = AttackPreviewStatus::Unavailable;
		int unavailableReason = 0; // 1=energy, 2=rule/condition, 3=non-deterministic effect
		int damage = 0;
		int ko = 0;
		int prizes = 0;
	};

	bool load(const std::string& path, std::string& error) {
		std::ifstream stream(path, std::ios::binary | std::ios::ate);
		if (!stream) { error = "cannot open evaluator model"; return false; }
		const std::streamoff length = stream.tellg();
		if (length < 0 || (std::uint64_t)length > std::numeric_limits<size_t>::max()) {
			error = "invalid evaluator V3 length"; return false;
		}
		std::vector<std::uint8_t> raw((size_t)length);
		stream.seekg(0, std::ios::beg);
		if (!raw.empty() && !stream.read(reinterpret_cast<char*>(raw.data()), length)) {
			error = "cannot read evaluator model"; return false;
		}
		if (raw.size() < sizeof(Header)) { error = "invalid evaluator V3 length"; return false; }
		Header header{}; std::memcpy(&header, raw.data(), sizeof(header));
		if (std::memcmp(header.magic, "PTCGEV3", 7) != 0 || header.version != 3
			|| header.globalDense != GlobalDenseCount || header.entityDense != EntityDenseCount
			|| header.entityHidden != EntityHiddenCount || header.globalHidden != GlobalHiddenCount
			|| header.poolCount != PoolCount || header.globalRelations != GlobalRelationCount
			|| header.entityRelations != EntityRelationCount || header.tokenCount == 0
			|| header.weightScale != WeightScale || header.beliefScale != BeliefScale
			|| header.scoreScale != ScoreScale || header.featureSchema != SchemaVersion
			|| header.informationSchema != InformationStateSchemaVersion
			|| header.effectSchema != EffectSchemaVersion || header.comboSchema != ComboSchemaVersion) {
			error = "invalid evaluator V3 header"; return false;
		}
		size_t expected = sizeof(Header) + (size_t)header.tokenCount * sizeof(std::int32_t)
			+ (size_t)EntityHiddenCount * EntityDenseCount * sizeof(std::int16_t)
			+ (size_t)EntityRelationCount * header.tokenCount * EntityHiddenCount * sizeof(std::int16_t)
			+ (size_t)EntityHiddenCount * sizeof(std::int32_t)
			+ (size_t)GlobalHiddenCount * GlobalDenseCount * sizeof(std::int16_t)
			+ (size_t)GlobalRelationCount * header.tokenCount * GlobalHiddenCount * sizeof(std::int16_t)
			+ (size_t)PoolCount * EntityHiddenCount * GlobalHiddenCount * sizeof(std::int16_t)
			+ (size_t)GlobalHiddenCount * sizeof(std::int32_t)
			+ (size_t)GlobalHiddenCount * sizeof(std::int16_t) + sizeof(std::int64_t);
		if (raw.size() != expected) { error = "invalid evaluator V3 payload length"; return false; }
		size_t offset = sizeof(Header);
		auto read = [&](auto& destination, size_t count) {
			using Value = typename std::decay_t<decltype(destination)>::value_type;
			destination.resize(count); std::memcpy(destination.data(), raw.data() + offset, count * sizeof(Value));
			offset += count * sizeof(Value);
		};
		read(tokens, header.tokenCount);
		if (tokens.front() != 0 || !std::is_sorted(tokens.begin(), tokens.end())
			|| std::adjacent_find(tokens.begin(), tokens.end()) != tokens.end()
			|| checksum(tokens.data(), tokens.size() * sizeof(tokens[0])) != header.tokenChecksum) {
			error = "invalid evaluator V3 token table"; return false;
		}
		read(entityDenseWeight, (size_t)EntityHiddenCount * EntityDenseCount);
		read(entitySparseWeight, (size_t)EntityRelationCount * tokens.size() * EntityHiddenCount);
		read(entityBias, EntityHiddenCount);
		read(globalDenseWeight, (size_t)GlobalHiddenCount * GlobalDenseCount);
		read(globalSparseWeight, (size_t)GlobalRelationCount * tokens.size() * GlobalHiddenCount);
		read(poolWeight, (size_t)PoolCount * EntityHiddenCount * GlobalHiddenCount);
		read(globalBias, GlobalHiddenCount); read(outputWeight, GlobalHiddenCount);
		std::memcpy(&outputBias, raw.data() + offset, sizeof(outputBias));
		if (!accumulatorBoundsSafe()) { error = "evaluator V3 accumulator bound exceeds int32"; return false; }
		entityDenseByInput.resize((size_t)EntityDenseCount * EntityHiddenCount);
		for (int input = 0; input < EntityDenseCount; ++input) for (int hidden = 0; hidden < EntityHiddenCount; ++hidden)
			entityDenseByInput[(size_t)input * EntityHiddenCount + hidden]
				= entityDenseWeight[(size_t)hidden * EntityDenseCount + input];
		globalDenseByInput.resize((size_t)GlobalDenseCount * GlobalHiddenCount);
		for (int input = 0; input < GlobalDenseCount; ++input) for (int hidden = 0; hidden < GlobalHiddenCount; ++hidden)
			globalDenseByInput[(size_t)input * GlobalHiddenCount + hidden]
				= globalDenseWeight[(size_t)hidden * GlobalDenseCount + input];
		int maximum = tokens.back(); tokenIndex.assign((size_t)std::max(0, maximum) + 1, 0);
		for (int i = 1; i < (int)tokens.size(); ++i) if (tokens[i] >= 0) tokenIndex[tokens[i]] = i;
		modelHashValue = checksum(raw.data(), raw.size());
		avx2Enabled = ExactCpuSupportsAvx2();
#if defined(_MSC_VER)
		char* simdMode = nullptr; size_t simdModeLength = 0;
		_dupenv_s(&simdMode, &simdModeLength, "PTCG_EVALUATOR_SIMD");
		if (simdMode != nullptr) {
			if (std::strcmp(simdMode, "scalar") == 0) avx2Enabled = false;
			else if (std::strcmp(simdMode, "avx2") == 0) avx2Enabled = ExactCpuSupportsAvx2();
			std::free(simdMode);
		}
#else
		if (const char* simdMode = std::getenv("PTCG_EVALUATOR_SIMD")) {
			if (std::strcmp(simdMode, "scalar") == 0) avx2Enabled = false;
			else if (std::strcmp(simdMode, "avx2") == 0) avx2Enabled = ExactCpuSupportsAvx2();
		}
#endif
		loaded = true; modelPath = path; return true;
	}

	bool isLoaded() const { return loaded; }
	std::uint64_t modelHash() const { return modelHashValue; }
	const std::string& path() const { return modelPath; }
	size_t residentBytes() const {
		return tokens.size() * sizeof(tokens[0]) + tokenIndex.size() * sizeof(tokenIndex[0])
			+ entityDenseWeight.size() * sizeof(entityDenseWeight[0]) + entitySparseWeight.size() * sizeof(entitySparseWeight[0])
			+ globalDenseWeight.size() * sizeof(globalDenseWeight[0]) + globalSparseWeight.size() * sizeof(globalSparseWeight[0])
			+ poolWeight.size() * sizeof(poolWeight[0]) + entityBias.size() * sizeof(entityBias[0])
			+ globalBias.size() * sizeof(globalBias[0]) + outputWeight.size() * sizeof(outputWeight[0]);
	}

	static AttackPreview tryPreviewAttack(const State& state, CardRef attackerRef,
		const Card& attacker, const AttackEnergy& ae) {
		AttackPreview result;
		if (attackerRef.isNull() || attacker.area != AreaType::Active || state.players[1 - attacker.playerIndex].active.empty()) {
			result.unavailableReason = 2; return result;
		}
		if (ae.insufficientEnergy > 0) { result.unavailableReason = 1; return result; }
		if (!CanUseAttack(state, attackerRef, attacker, ae)) { result.unavailableReason = 2; return result; }
		const Attack& attack = *ae.attack;
		if (!attack.preEffects.empty() || !attack.postEffects.empty() || attack.attackFlags != 0) {
			result.unavailableReason = 3; return result;
		}
		CardRef targetRef = state.players[1 - attacker.playerIndex].getActive();
		const Card& target = state.getCard(targetRef);
		int damage = CalcDamage(state, attack.damage, targetRef, target, attackerRef, attacker, true, &attack);
		result.status = AttackPreviewStatus::Exact; result.damage = damage;
		result.ko = damage >= state.getHp(target);
		result.prizes = result.ko ? state.getPrizeCount(target) : 0;
		return result;
	}

	static void extractFeaturesInto(FeatureRecord& out, const State& state, int actor,
		const std::unordered_map<int, int>* actorProfile, const BeliefInput* belief,
		unsigned long long* hiddenFeatureCacheHits, ExtractTiming* timing,
		const std::vector<std::pair<int, int>>* sortedActorProfile,
		bool canonicalizeOutput = true) {
		auto stageStarted = timing != nullptr ? std::chrono::steady_clock::now()
			: std::chrono::steady_clock::time_point{};
		out.globalSparse.count = 0;
		out.entityCount = 0;
		out.opponentInferenceVersion = 0;
		out.overflow = false;
		if (state.phase == GamePhase::PokemonCheckupEnd) {
			if (state.turnState != 0) out.overflow = true;
			for (const PlayerState& player : state.players)
				if (player.thisTurn.value != 0 || player.turnState != 0) out.overflow = true;
			for (const Card& card : state.allCard) if (card.cardId != 0
				&& (card.area == AreaType::Active || card.area == AreaType::Bench || card.area == AreaType::Stadium)
				&& (card.thisTurn.value != std::array<unsigned, 4>{}
					|| card.thisTurnEnemy.value != std::array<unsigned, 1>{}
					|| card.turnState != std::array<unsigned, 3>{} || !card.abilityUsed.empty())) out.overflow = true;
		}
		const int enemy = 1 - actor;
		const PlayerState& me = state.players[actor]; const PlayerState& opp = state.players[enemy];
		out.globalDense[0] = actor;
		out.globalDense[1] = state.firstPlayer == actor ? 1 : 0;
		out.globalDense[2] = state.turn;
		out.globalDense[3] = (int)me.prize.size(); out.globalDense[4] = (int)opp.prize.size();
		out.globalDense[5] = (int)me.deck.size(); out.globalDense[6] = (int)opp.deck.size();
		out.globalDense[7] = (int)opp.hand.size(); out.globalDense[8] = (int)state.gameResult;
		out.globalDense[9] = (int)state.finishReason; out.globalDense[10] = state.turnHistories[0].ko ? 1 : 0;
		out.globalDense[11] = state.turnHistories[0].takePrizeCountTurnPlayer;
		out.globalDense[12] = 0; out.globalDense[13] = 0;
		out.globalDense[14] = (me.koPrizeOnceChanged ? 1 : 0) | (opp.koPrizeOnceChanged ? 2 : 0);
		out.globalDense[15] = 1;
		auto addGlobal = [&](int token, int relation, int value) {
			if (!out.globalSparse.push(token, (std::int16_t)relation, value)) out.overflow = true;
		};
		if (state.turnHistories[0].turnAttackId > 0)
			addGlobal(AttackTokenBase + state.turnHistories[0].turnAttackId, PublicHistory, BeliefScale);
		if (state.turnHistories[1].turnAttackId > 0)
			addGlobal(AttackTokenBase + state.turnHistories[1].turnAttackId, PublicHistory, 2 * BeliefScale);
		for (CardRef ref : me.hand) if (!ref.isNull()) addGlobal(state.getCard(ref).cardId, OwnHand, BeliefScale);
		for (CardRef ref : me.trash) if (!ref.isNull()) addGlobal(state.getCard(ref).cardId, OwnTrash, BeliefScale);
		for (CardRef ref : opp.trash) if (!ref.isNull()) addGlobal(state.getCard(ref).cardId, OppTrash, BeliefScale);
		for (CardRef ref : state.stadium) if (!ref.isNull()) addGlobal(state.getCard(ref).cardId, Stadium, BeliefScale);
		appendPlayerEffects(me, OwnPlayerEffect, addGlobal); appendPlayerEffects(opp, OppPlayerEffect, addGlobal);
		appendGlobalEffects(state, addGlobal);
		if (timing != nullptr) {
			auto now = std::chrono::steady_clock::now();
			timing->publicNs += (std::uint64_t)std::chrono::duration_cast<std::chrono::nanoseconds>(now - stageStarted).count();
			stageStarted = now;
		}
		if (actorProfile != nullptr) {
			// Remaining hidden counts are used by every belief and combo feature.
			// Build them once; the former implementation rescanned allCard for every
			// card ID and again inside every evolution-combo query.
			static thread_local std::vector<int> remainingById;
			static thread_local std::vector<std::uint32_t> remainingGeneration;
			static thread_local std::uint32_t generation = 0;
			if (++generation == 0) { std::fill(remainingGeneration.begin(), remainingGeneration.end(), 0); generation = 1; }
			static thread_local std::vector<std::pair<int, int>> profileScratch;
			if (sortedActorProfile == nullptr) {
				profileScratch.clear(); profileScratch.reserve(actorProfile->size());
				for (const auto& item : *actorProfile) profileScratch.push_back(item);
				std::sort(profileScratch.begin(), profileScratch.end());
			}
			const auto& profile = sortedActorProfile != nullptr ? *sortedActorProfile : profileScratch;
			int maximumId = profile.empty() ? 0 : profile.back().first;
			if (maximumId >= 0 && (size_t)maximumId >= remainingById.size()) {
				remainingById.resize((size_t)maximumId + 1);
				remainingGeneration.resize((size_t)maximumId + 1);
			}
			for (const auto& item : profile) if (item.first >= 0) {
				remainingById[(size_t)item.first] = 0;
				remainingGeneration[(size_t)item.first] = generation;
			}
			bool concreteHiddenPool = true;
			auto addHidden = [&](const auto& zone) {
				for (CardRef ref : zone) {
					if (ref.isNull()) { concreteHiddenPool = false; continue; }
					const Card& card = state.getCard(ref);
					if (card.cardId >= 0 && card.playerIndex == actor
						&& (size_t)card.cardId < remainingGeneration.size()
						&& remainingGeneration[(size_t)card.cardId] == generation)
						++remainingById[(size_t)card.cardId];
				}
			};
			addHidden(me.deck); addHidden(me.prize);
			if (!concreteHiddenPool) {
				for (const auto& item : profile) if (item.first >= 0)
					remainingById[(size_t)item.first] = item.second;
				for (const Card& card : state.allCard) if (card.cardId >= 0 && card.playerIndex == actor
					&& card.area != AreaType::Deck && card.area != AreaType::Prize
					&& (size_t)card.cardId < remainingGeneration.size()
					&& remainingGeneration[(size_t)card.cardId] == generation)
					--remainingById[(size_t)card.cardId];
			}
			auto remainingCount = [&](int id) {
				return id >= 0 && (size_t)id < remainingGeneration.size()
					&& remainingGeneration[(size_t)id] == generation
					? std::max(0, remainingById[(size_t)id]) : 0;
			};
			struct HiddenFeatureCacheEntry {
				bool valid = false;
				std::uint64_t hash = 0;
				std::uint8_t count = 0, deckSize = 0, prizeSize = 0;
				std::array<int, DECK_SIZE> ids{};
				std::array<std::uint8_t, DECK_SIZE> remaining{};
				FixedSparseList<192> features;
			};
			static thread_local std::array<HiddenFeatureCacheEntry, 4096> hiddenFeatureCache;
			HiddenFeatureCacheEntry* hiddenCacheSlot = nullptr;
			bool hiddenCacheHit = false;
			if (belief == nullptr && profile.size() <= DECK_SIZE) {
				std::uint64_t hash = 1469598103934665603ULL;
				auto mix = [&](std::uint32_t value) {
					for (int byte = 0; byte < 4; ++byte) {
						hash ^= (std::uint8_t)(value >> (byte * 8));
						hash *= 1099511628211ULL;
					}
				};
				mix((std::uint32_t)me.deck.size()); mix((std::uint32_t)me.prize.size());
				for (const auto& item : profile) { mix((std::uint32_t)item.first); mix((std::uint32_t)remainingCount(item.first)); }
				hiddenCacheSlot = &hiddenFeatureCache[hash & (hiddenFeatureCache.size() - 1)];
				hiddenCacheHit = hiddenCacheSlot->valid && hiddenCacheSlot->hash == hash
					&& hiddenCacheSlot->count == profile.size()
					&& hiddenCacheSlot->deckSize == me.deck.size() && hiddenCacheSlot->prizeSize == me.prize.size();
				if (hiddenCacheHit) for (size_t i = 0; i < profile.size(); ++i)
					hiddenCacheHit = hiddenCacheHit && hiddenCacheSlot->ids[i] == profile[i].first
						&& hiddenCacheSlot->remaining[i] == remainingCount(profile[i].first);
				if (hiddenCacheHit) {
					for (int i = 0; i < hiddenCacheSlot->features.count; ++i) {
						const SparseInput& cached = hiddenCacheSlot->features.values[i];
						addGlobal(cached.token, cached.relation, cached.value);
					}
					if (hiddenFeatureCacheHits != nullptr) (*hiddenFeatureCacheHits)++;
				}
			}
			const std::uint16_t hiddenFeatureStart = out.globalSparse.count;
			if (!hiddenCacheHit) {
			int hiddenTotal = (int)me.deck.size() + (int)me.prize.size();
			for (const auto& item : profile) {
				int remaining = remainingCount(item.first);
				if (remaining > 0) {
					addGlobal(item.first, OwnHiddenPool, remaining * BeliefScale);
					if (belief == nullptr && hiddenTotal > 0) {
						auto expected = [&](int size) { return (remaining * size * BeliefScale + hiddenTotal / 2) / hiddenTotal; };
						auto exists = [&](int size) {
							unsigned long long denominator = choose64(hiddenTotal, size);
							unsigned long long absent = choose64(hiddenTotal - remaining, size);
							return ratioQ8(denominator - absent, denominator);
						};
						addGlobal(item.first, OwnDeckExpected, expected((int)me.deck.size()));
						addGlobal(item.first, OwnPrizeExpected, expected((int)me.prize.size()));
						addGlobal(item.first, OwnDeckExists, exists((int)me.deck.size()));
						addGlobal(item.first, OwnPrizeExists, exists((int)me.prize.size()));
					}
				}
			}
			if (belief == nullptr && hiddenTotal > 0) {
				struct EvolutionRelation {
					int evolvedId = 0;
					EvolutionType type = EvolutionType::NoEvolutionType;
					std::vector<int> preIds, basicIds;
				};
				struct EvolutionProfileCache {
					std::vector<std::pair<int, int>> profile;
					std::vector<EvolutionRelation> relations;
				};
				static thread_local EvolutionProfileCache evolutionCache;
				if (profile != evolutionCache.profile) {
					evolutionCache.profile = profile; evolutionCache.relations.clear();
					for (const auto& item : evolutionCache.profile) {
						const CardMaster* evolved = FindCardMaster(item.first);
						if (evolved == nullptr || evolved->evolutionType == EvolutionType::Basic
							|| evolved->evolutionType == EvolutionType::NoEvolutionType) continue;
						EvolutionRelation relation; relation.evolvedId = item.first; relation.type = evolved->evolutionType;
						for (const auto& candidate : evolutionCache.profile) {
							const CardMaster* pre = FindCardMaster(candidate.first);
							if (pre != nullptr && (pre->name == evolved->evolvesFrom
								|| pre->nameEn == evolved->evolvesFrom)) relation.preIds.push_back(candidate.first);
						}
						if (relation.type == EvolutionType::Stage2) for (const auto& basicCandidate : evolutionCache.profile) {
							const CardMaster* basic = FindCardMaster(basicCandidate.first); if (basic == nullptr) continue;
							bool required = false;
							for (const auto& stageCandidate : evolutionCache.profile) {
								const CardMaster* stage = FindCardMaster(stageCandidate.first);
								if (stage != nullptr && stage->evolutionType == EvolutionType::Stage1
									&& (stage->name == evolved->evolvesFrom || stage->nameEn == evolved->evolvesFrom)
									&& (basic->name == stage->evolvesFrom || basic->nameEn == stage->evolvesFrom)) {
									required = true; break;
								}
							}
							if (required) relation.basicIds.push_back(basicCandidate.first);
						}
						evolutionCache.relations.push_back(std::move(relation));
					}
				}
				unsigned long long denominator = choose64(hiddenTotal, (int)me.deck.size());
				for (const EvolutionRelation& relation : evolutionCache.relations) {
					if (remainingCount(relation.evolvedId) <= 0) continue;
					int preCount = 0; for (int id : relation.preIds) preCount += remainingCount(id);
					if (preCount <= 0 || denominator == 0) continue;
					int evoCount = remainingCount(relation.evolvedId), size = (int)me.deck.size();
					unsigned long long neitherEvo = choose64(hiddenTotal - evoCount, size);
					unsigned long long neitherPre = choose64(hiddenTotal - preCount, size);
					unsigned long long neitherBoth = choose64(hiddenTotal - evoCount - preCount, size);
					long long favorable = (long long)denominator - (long long)neitherEvo - (long long)neitherPre + (long long)neitherBoth;
					addGlobal(ComboTokenBase + relation.evolvedId, ComboProbability,
						ratioQ8((unsigned long long)std::max<long long>(0, favorable), denominator));
					if (relation.type == EvolutionType::Stage2) {
						int basicCount = 0; for (int id : relation.basicIds) basicCount += remainingCount(id);
						if (basicCount > 0) {
							auto absent = [&](int count) { return choose64(hiddenTotal - count, size); };
							long long all = (long long)denominator - (long long)absent(evoCount) - (long long)absent(preCount) - (long long)absent(basicCount)
								+ (long long)absent(evoCount + preCount) + (long long)absent(evoCount + basicCount)
								+ (long long)absent(preCount + basicCount) - (long long)absent(evoCount + preCount + basicCount);
							addGlobal(ComboTokenBase + 250'000 + relation.evolvedId, ComboProbability,
								ratioQ8((unsigned long long)std::max<long long>(0, all), denominator));
						}
					}
				}
			}
				if (belief == nullptr && hiddenCacheSlot != nullptr
					&& out.globalSparse.count - hiddenFeatureStart <= hiddenCacheSlot->features.values.size()) {
					hiddenCacheSlot->valid = true;
					hiddenCacheSlot->hash = 1469598103934665603ULL;
					auto mix = [&](std::uint32_t value) {
						for (int byte = 0; byte < 4; ++byte) {
							hiddenCacheSlot->hash ^= (std::uint8_t)(value >> (byte * 8));
							hiddenCacheSlot->hash *= 1099511628211ULL;
						}
					};
					mix((std::uint32_t)me.deck.size()); mix((std::uint32_t)me.prize.size());
					hiddenCacheSlot->count = (std::uint8_t)profile.size();
					hiddenCacheSlot->deckSize = (std::uint8_t)me.deck.size(); hiddenCacheSlot->prizeSize = (std::uint8_t)me.prize.size();
					for (size_t i = 0; i < profile.size(); ++i) {
						hiddenCacheSlot->ids[i] = profile[i].first;
						hiddenCacheSlot->remaining[i] = (std::uint8_t)remainingCount(profile[i].first);
						mix((std::uint32_t)profile[i].first); mix(hiddenCacheSlot->remaining[i]);
					}
					hiddenCacheSlot->features.count = out.globalSparse.count - hiddenFeatureStart;
					for (int i = 0; i < hiddenCacheSlot->features.count; ++i)
						hiddenCacheSlot->features.values[i] = out.globalSparse.values[hiddenFeatureStart + i];
				}
			}
		}
		if (belief != nullptr) {
			appendMap(belief->ownDeckQ8, OwnDeckExpected, addGlobal);
			appendMap(belief->ownPrizeQ8, OwnPrizeExpected, addGlobal);
			appendMap(belief->ownDeckExistsQ8, OwnDeckExists, addGlobal);
			appendMap(belief->ownPrizeExistsQ8, OwnPrizeExists, addGlobal);
			appendMap(belief->comboQ8, ComboProbability, addGlobal);
			appendMap(belief->knownDeckCounts, OwnKnownDeck, addGlobal, BeliefScale);
			appendMap(belief->knownPrizeCounts, OwnKnownPrize, addGlobal, BeliefScale);
			if (belief->knownTop != nullptr) for (int i = 0; i < std::min(4, (int)(*belief->knownTop)[actor].size()); ++i)
				addGlobal((*belief->knownTop)[actor][i], OwnKnownTop0 + i, BeliefScale);
			if (belief->knownBottom != nullptr) for (int i = 0; i < std::min(4, (int)(*belief->knownBottom)[actor].size()); ++i)
				addGlobal((*belief->knownBottom)[actor][i], OwnKnownBottom0 + i, BeliefScale);
		}
		if (timing != nullptr) {
			auto now = std::chrono::steady_clock::now();
			timing->hiddenNs += (std::uint64_t)std::chrono::duration_cast<std::chrono::nanoseconds>(now - stageStarted).count();
			stageStarted = now;
		}
		auto entity = [&](CardRef ref, int owner, bool active) {
			if (ref.isNull() || out.entityCount >= MaxEntities) { if (!ref.isNull()) out.overflow = true; return; }
			EntityRecord& e = out.entities[out.entityCount++]; const Card& card = state.getCard(ref);
			e.sparse.count = 0;
			e.pool = owner == actor ? (active ? OwnActivePool : OwnBenchPool) : (active ? OppActivePool : OppBenchPool);
			e.dense[0] = owner == actor ? 1 : -1; e.dense[1] = active ? 1 : 0;
			e.dense[2] = card.damage; e.dense[3] = state.getHp(card); e.dense[4] = state.retreatCost(card);
			const PlayerState& ps = state.players[owner]; e.dense[5] = active ? (int)ps.badStatus : 0;
			e.dense[6] = active ? ps.poisonDamageCounter : 0; e.dense[7] = active && ps.burned ? 1 : 0;
			e.dense[8] = 0; e.dense[9] = state.getPrizeCount(card);
			e.dense[10] = card.takeAttackDamagePreTurn; e.dense[11] = card.cannotUseAttackIdNonActive;
			e.dense[12] = 0; e.dense[13] = card.reverse ? 1 : 0; e.dense[14] = 0; e.dense[15] = 1;
			auto add = [&](int token, int relation, int value) {
				if (!e.sparse.push(token, (std::int16_t)relation, value)) out.overflow = true;
			};
			add(card.cardId, PokemonIdentity, BeliefScale);
			int evolution = 0;
			for (CardRef child : state.players[owner].preEvolution) if (!child.isNull()
				&& state.getCard(child).attachMoveCounter == card.moveCounter)
				add(state.getCard(child).cardId, Evolution0 + std::min(evolution++, 3), BeliefScale);
			for (CardRef child : state.players[owner].energy) if (!child.isNull()
				&& state.getCard(child).attachMoveCounter == card.moveCounter)
				add(state.getCard(child).cardId, AttachedEnergy, BeliefScale);
			for (CardRef child : state.players[owner].tool) if (!child.isNull()
				&& state.getCard(child).attachMoveCounter == card.moveCounter)
				add(state.getCard(child).cardId, AttachedTool, BeliefScale);
			for (short skill : card.abilityUsed) add(EffectTokenBase + 400 + skill, AbilityUsed, BeliefScale);
			appendCardEffects(card, add);
			auto& energies = state.game->energyList; state.getEnergies(owner, ref, energies);
			SetAttackEnergy(state, card, energies, true);
			for (const AttackEnergy& attack : state.game->attackEnergyList) {
				int token = AttackTokenBase + attack.attack->attackId;
				add(token, AttackIdentity, std::max(0, 8 - std::clamp(attack.insufficientEnergy, 0, 7)) * BeliefScale);
				AttackPreview preview = tryPreviewAttack(state, ref, card, attack);
				if (preview.status == AttackPreviewStatus::Exact) {
					add(token, AttackExactDamage, preview.damage * BeliefScale / 10);
					if (preview.ko) add(token, AttackKo, BeliefScale);
					if (preview.prizes) add(token, AttackPrize, preview.prizes * BeliefScale);
				} else add(token, AttackUnavailable, std::max(1, preview.unavailableReason) * BeliefScale);
			}
			if (canonicalizeOutput) e.sparse.canonicalize();
		};
		for (CardRef ref : me.active) entity(ref, actor, true); for (CardRef ref : opp.active) entity(ref, enemy, true);
		for (CardRef ref : me.bench) entity(ref, actor, false); for (CardRef ref : opp.bench) entity(ref, enemy, false);
		if (canonicalizeOutput) {
			std::sort(out.entities.begin(), out.entities.begin() + out.entityCount, [](const EntityRecord& a, const EntityRecord& b) {
				if (a.pool != b.pool) return a.pool < b.pool;
				if (a.dense != b.dense) return a.dense < b.dense;
				return std::lexicographical_compare(a.sparse.values.begin(), a.sparse.values.begin() + a.sparse.count,
					b.sparse.values.begin(), b.sparse.values.begin() + b.sparse.count, [](const SparseInput& x, const SparseInput& y) {
						if (x.relation != y.relation) return x.relation < y.relation;
						if (x.token != y.token) return x.token < y.token; return x.value < y.value;
					});
			});
			out.globalSparse.canonicalize();
		}
		if (timing != nullptr) timing->entityNs += (std::uint64_t)std::chrono::duration_cast<std::chrono::nanoseconds>(
			std::chrono::steady_clock::now() - stageStarted).count();
	}

	static FeatureRecord extractFeatures(const State& state, int actor,
		const std::unordered_map<int, int>* actorProfile = nullptr, const BeliefInput* belief = nullptr,
		unsigned long long* hiddenFeatureCacheHits = nullptr, ExtractTiming* timing = nullptr,
		const std::vector<std::pair<int, int>>* sortedActorProfile = nullptr) {
		FeatureRecord out;
		extractFeaturesInto(out, state, actor, actorProfile, belief, hiddenFeatureCacheHits, timing,
			sortedActorProfile, true);
		return out;
	}

	long long evaluate(const State& state, int actor,
		const std::unordered_map<int, int>* actorProfile = nullptr, const BeliefInput* belief = nullptr) const {
		return evaluate(extractFeatures(state, actor, actorProfile, belief));
	}
	long long evaluate(const FeatureRecord& feature, unsigned long long* accumulatorHits = nullptr) const {
		std::array<std::int32_t, GlobalHiddenCount> global{};
		for (int h = 0; h < GlobalHiddenCount; ++h) global[h] = globalBias[h];
		if (avx2Enabled) ExactAddDenseI16ToI32Avx2(globalDenseByInput.data(), feature.globalDense.data(),
			global.data(), GlobalDenseCount, GlobalHiddenCount);
		else for (int d = 0; d < GlobalDenseCount; ++d) for (int h = 0; h < GlobalHiddenCount; ++h)
			global[h] += (std::int32_t)globalDenseWeight[(size_t)h * GlobalDenseCount + d] * feature.globalDense[d];
		for (int i = 0; i < feature.globalSparse.count; ++i)
			addGlobalSparse(feature.globalSparse.values[i], global);
		struct EntityCacheEntry {
			std::uint64_t model = 0, hash = 0;
			bool valid = false;
			EntityRecord record;
			std::array<std::int32_t, GlobalHiddenCount> projection{};
		};
		static thread_local std::array<EntityCacheEntry, 2048> entityCache;
		auto entityHash = [](const EntityRecord& entity) {
			std::uint64_t hash = 1469598103934665603ULL;
			auto bytes = [&](const void* data, size_t count) {
				const auto* input = static_cast<const std::uint8_t*>(data);
				for (size_t i = 0; i < count; ++i) { hash ^= input[i]; hash *= 1099511628211ULL; }
			};
			bytes(&entity.pool, sizeof(entity.pool)); bytes(entity.dense.data(), sizeof(entity.dense));
			bytes(&entity.sparse.count, sizeof(entity.sparse.count));
			bytes(entity.sparse.values.data(), (size_t)entity.sparse.count * sizeof(SparseInput));
			return hash;
		};
		auto sameEntity = [](const EntityRecord& left, const EntityRecord& right) {
			return left.pool == right.pool && left.dense == right.dense
				&& left.sparse.count == right.sparse.count
				&& (left.sparse.count == 0 || std::memcmp(left.sparse.values.data(), right.sparse.values.data(),
					(size_t)left.sparse.count * sizeof(SparseInput)) == 0);
		};
		for (int ei = 0; ei < feature.entityCount; ++ei) {
			const EntityRecord& entity = feature.entities[ei];
			const std::uint64_t hash = entityHash(entity);
			EntityCacheEntry& cached = entityCache[hash & (entityCache.size() - 1)];
			if (cached.valid && cached.model == modelHashValue && cached.hash == hash && sameEntity(cached.record, entity)) {
				for (int gh = 0; gh < GlobalHiddenCount; ++gh) global[gh] += cached.projection[gh];
				if (accumulatorHits != nullptr) (*accumulatorHits)++;
				continue;
			}
			std::array<std::int32_t, EntityHiddenCount> acc{};
			for (int h = 0; h < EntityHiddenCount; ++h) acc[h] = entityBias[h];
			if (avx2Enabled) ExactAddDenseI16ToI32Avx2(entityDenseByInput.data(), entity.dense.data(),
				acc.data(), EntityDenseCount, EntityHiddenCount);
			else for (int d = 0; d < EntityDenseCount; ++d) for (int h = 0; h < EntityHiddenCount; ++h)
				acc[h] += (std::int32_t)entityDenseWeight[(size_t)h * EntityDenseCount + d] * entity.dense[d];
			for (int i = 0; i < entity.sparse.count; ++i) addEntitySparse(entity.sparse.values[i], acc);
			cached.projection.fill(0);
			for (int gh = 0; gh < GlobalHiddenCount; ++gh) {
				long long projection = 0;
				for (int eh = 0; eh < EntityHiddenCount; ++eh) {
					long long activation = std::clamp<std::int64_t>(acc[eh], 0, (std::int64_t)127 * WeightScale);
					size_t at = ((size_t)entity.pool * EntityHiddenCount + eh) * GlobalHiddenCount + gh;
					projection += (long long)poolWeight[at] * activation;
				}
				cached.projection[gh] = (std::int32_t)roundedDivide(projection, WeightScale);
				global[gh] += cached.projection[gh];
			}
			cached.model = modelHashValue; cached.hash = hash; cached.record = entity; cached.valid = true;
		}
		long long output = outputBias;
		for (int h = 0; h < GlobalHiddenCount; ++h) {
			long long activation = std::clamp<std::int64_t>(global[h], 0, (std::int64_t)127 * WeightScale);
			output += (long long)outputWeight[h] * activation;
		}
		constexpr long long divisor = (long long)WeightScale * WeightScale;
		long long magnitude = output >= 0 ? output : -output;
		long long score = (magnitude / divisor) * ScoreScale
			+ ((magnitude % divisor) * ScoreScale + divisor / 2) / divisor;
		if (output < 0) score = -score;
		return std::clamp(score, (long long)-NonTerminalLimit, (long long)NonTerminalLimit);
	}

	// Exact model-side part of a turn continuation signature.  A card that is
	// proven rule-inert for the remainder of the turn may share a draw class only
	// when every global sparse relation (including generated combo aliases) has
	// the same integer pre-activation contribution.  Returning the full vector,
	// rather than a digest, prevents evaluator collisions from merging outcomes.
	std::vector<std::int16_t> cardContinuationSignature(int cardId) const {
		std::array<int, 3> aliases{ cardId, ComboTokenBase + cardId,
			ComboTokenBase + 250'000 + cardId };
		std::vector<std::int16_t> signature;
		signature.reserve(aliases.size() * GlobalRelationCount * GlobalHiddenCount);
		for (int alias : aliases) {
			int token = indexFor(alias);
			for (int relation = 0; relation < GlobalRelationCount; ++relation) {
				size_t base = ((size_t)relation * tokens.size() + token) * GlobalHiddenCount;
				for (int hidden = 0; hidden < GlobalHiddenCount; ++hidden)
					signature.push_back(globalSparseWeight[base + hidden]);
			}
		}
		return signature;
	}

	// First-order OwnHand contribution used to bootstrap V4 Passive bias from V3.
	// Matches tools/bootstrap_evaluator_v4.py / nnue_v4.own_hand_linear_score.
	long long estimateOwnHandLinearScore(int cardId) const {
		if (!loaded) return 0;
		const int token = indexFor(cardId);
		size_t base = ((size_t)OwnHand * tokens.size() + token) * GlobalHiddenCount;
		long long output = 0;
		for (int h = 0; h < GlobalHiddenCount; ++h) {
			long long delta = (long long)globalSparseWeight[base + h] * BeliefScale;
			long long activation = std::clamp<std::int64_t>(delta, 0, (std::int64_t)127 * WeightScale);
			output += (long long)outputWeight[h] * activation;
		}
		constexpr long long divisor = (long long)WeightScale * WeightScale;
		long long magnitude = output >= 0 ? output : -output;
		long long score = (magnitude / divisor) * ScoreScale
			+ ((magnitude % divisor) * ScoreScale + divisor / 2) / divisor;
		if (output < 0) score = -score;
		return std::clamp(score, (long long)-NonTerminalLimit, (long long)NonTerminalLimit);
	}

	const std::vector<std::int32_t>& tokenTable() const { return tokens; }

private:
#pragma pack(push, 1)
	struct Header {
		char magic[8];
		std::uint32_t version, globalDense, entityDense, entityHidden, globalHidden, poolCount;
		std::uint32_t globalRelations, entityRelations, tokenCount;
		std::uint32_t weightScale, beliefScale, scoreScale;
		std::uint32_t featureSchema, informationSchema, effectSchema, comboSchema;
		std::uint64_t tokenChecksum, cardChecksum, effectChecksum, comboChecksum;
		std::uint8_t datasetHash[32];
	};
#pragma pack(pop)
	std::vector<std::int32_t> tokens;
	std::vector<int> tokenIndex;
	std::vector<std::int16_t> entityDenseWeight, entityDenseByInput, entitySparseWeight, globalDenseWeight, globalDenseByInput;
	std::vector<std::int16_t> globalSparseWeight, poolWeight, outputWeight;
	std::vector<std::int32_t> entityBias, globalBias;
	std::int64_t outputBias = 0;
	std::uint64_t modelHashValue = 0;
	bool loaded = false;
	bool avx2Enabled = false;
	std::string modelPath;

	bool accumulatorBoundsSafe() const {
		constexpr std::array<long long, EntityDenseCount> entityMax{
			1, 1, 10'000, 10'000, 100, 3, 100, 1, 0, 6, 10'000, 32'767, 0, 1, 0, 1 };
		constexpr std::array<long long, EntityRelationCount> entitySparseMax{
			256, 256, 256, 256, 256, 15'360, 1'024, 2'048, 32'768,
			2'100'000, 409'600, 4'096, 24'576, 12'288, 409'600, 409'600 };
		for (int h = 0; h < EntityHiddenCount; ++h) {
			long long bound = std::llabs((long long)entityBias[h]);
			for (int d = 0; d < EntityDenseCount; ++d)
				bound += std::llabs((long long)entityDenseWeight[(size_t)h * EntityDenseCount + d]) * entityMax[d];
			for (int relation = 0; relation < EntityRelationCount; ++relation) {
				long long maximum = 0;
				for (size_t token = 0; token < tokens.size(); ++token)
					maximum = std::max(maximum, std::llabs((long long)entitySparseWeight[((size_t)relation * tokens.size() + token) * EntityHiddenCount + h]));
				bound += maximum * entitySparseMax[relation];
			}
			if (bound > std::numeric_limits<std::int32_t>::max()) return false;
		}
		constexpr std::array<long long, GlobalDenseCount> globalMax{
			1, 1, 10'000, 60, 60, 60, 60, 60, 4, 32, 1, 60, 0, 0, 3, 1 };
		constexpr std::array<long long, GlobalRelationCount> globalSparseMax{
			15'360, 15'360, 15'360, 256, 15'360, 15'360, 15'360, 15'360, 15'360,
			15'360, 15'360, 256, 256, 256, 256, 256, 256, 256, 256,
			2'100'000, 2'100'000, 2'100'000, 15'360, 512 };
		for (int gh = 0; gh < GlobalHiddenCount; ++gh) {
			long long bound = std::llabs((long long)globalBias[gh]);
			for (int d = 0; d < GlobalDenseCount; ++d)
				bound += std::llabs((long long)globalDenseWeight[(size_t)gh * GlobalDenseCount + d]) * globalMax[d];
			for (int relation = 0; relation < GlobalRelationCount; ++relation) {
				long long maximum = 0;
				for (size_t token = 0; token < tokens.size(); ++token)
					maximum = std::max(maximum, std::llabs((long long)globalSparseWeight[((size_t)relation * tokens.size() + token) * GlobalHiddenCount + gh]));
				bound += maximum * globalSparseMax[relation];
			}
			for (int pool = 0; pool < PoolCount; ++pool) {
				int entityCount = pool < 2 ? 1 : BENCH_SIZE_MAX;
				for (int eh = 0; eh < EntityHiddenCount; ++eh) {
					size_t at = ((size_t)pool * EntityHiddenCount + eh) * GlobalHiddenCount + gh;
					bound += std::llabs((long long)poolWeight[at]) * 127LL * entityCount;
				}
			}
			if (bound > std::numeric_limits<std::int32_t>::max()) return false;
		}
		return true;
	}

	static std::uint64_t checksum(const void* data, size_t count) {
		const auto* bytes = static_cast<const std::uint8_t*>(data); std::uint64_t value = 1469598103934665603ULL;
		for (size_t i = 0; i < count; ++i) { value ^= bytes[i]; value *= 1099511628211ULL; } return value;
	}
	static unsigned long long choose64(int n, int k) {
		if (k < 0 || k > n) return 0; k = std::min(k, n - k);
		unsigned long long value = 1;
		for (int i = 1; i <= k; ++i) value = value * (unsigned long long)(n - k + i) / (unsigned long long)i;
		return value;
	}
	static int ratioQ8(unsigned long long numerator, unsigned long long denominator) {
		if (denominator == 0 || numerator == 0) return 0;
		if (numerator >= denominator) return BeliefScale;
		unsigned long long remainder = numerator; int result = 0;
		for (int bit = 0; bit < 8; ++bit) {
			result <<= 1;
			if (remainder >= denominator - remainder) { remainder = remainder - (denominator - remainder); result++; }
			else remainder += remainder;
		}
		if (remainder >= denominator - remainder && result < BeliefScale) result++;
		return result;
	}
	int indexFor(int token) const { return token >= 0 && token < (int)tokenIndex.size() ? tokenIndex[token] : 0; }
	static long long roundedDivide(long long value, long long divisor) {
		return value >= 0 ? (value + divisor / 2) / divisor : -((-value + divisor / 2) / divisor);
	}
	void addEntitySparse(const SparseInput& in, std::array<std::int32_t, EntityHiddenCount>& acc) const {
		if (in.relation < 0 || in.relation >= EntityRelationCount) return;
		size_t base = ((size_t)in.relation * tokens.size() + indexFor(in.token)) * EntityHiddenCount;
		if (avx2Enabled) {
			ExactAddScaledI16ToI32Avx2(entitySparseWeight.data() + base, in.value, acc.data(), EntityHiddenCount);
			return;
		}
		for (int h = 0; h < EntityHiddenCount; ++h) acc[h] += (std::int32_t)entitySparseWeight[base + h] * in.value;
	}
	void addGlobalSparse(const SparseInput& in, std::array<std::int32_t, GlobalHiddenCount>& acc) const {
		if (in.relation < 0 || in.relation >= GlobalRelationCount) return;
		size_t base = ((size_t)in.relation * tokens.size() + indexFor(in.token)) * GlobalHiddenCount;
		if (avx2Enabled) {
			ExactAddScaledI16ToI32Avx2(globalSparseWeight.data() + base, in.value, acc.data(), GlobalHiddenCount);
			return;
		}
		for (int h = 0; h < GlobalHiddenCount; ++h) acc[h] += (std::int32_t)globalSparseWeight[base + h] * in.value;
	}
	template<class Map, class Callback>
	static void appendMap(const Map* source, int relation, Callback&& add, int scale = 1) {
		if (source != nullptr) for (const auto& item : *source) if (item.second != 0) add(item.first, relation, item.second * scale);
	}
	template<class Callback>
	static void appendPlayerEffects(const PlayerState& p, int relation, Callback&& add) {
		auto effect = [&](int id, int value) { if (value) add(EffectTokenBase + id, relation, value); };
		effect(1, p.thisTurn.metalDamageChange); effect(2, p.thisTurn.cannotPlayItem);
		effect(3, p.thisTurn.cannotPlaySupporter); effect(4, p.thisTurn.cannotPlayStadium);
		effect(5, p.thisTurn.cannotPlaySpecialEnergy); effect(6, p.thisTurn.cannotEvolve);
		effect(7, p.thisTurn.cannotRetreatPoison); effect(8, p.nextTurn.metalDamageChange);
		effect(9, p.nextTurn.cannotPlayItem); effect(10, p.nextTurn.cannotPlaySupporter);
		effect(11, p.nextTurn.cannotPlayStadium); effect(12, p.nextTurn.cannotPlaySpecialEnergy);
		effect(13, p.nextTurn.cannotEvolve); effect(14, p.nextTurn.cannotRetreatPoison);
		effect(15, p.poisonDamageChange); effect(16, p.burnDamageChange);
		effect(17, p.poisonDamageChangeNotDarkness); effect(18, p.benchCapacity);
		effect(19, p.cannotPlayItem); effect(20, p.cannotPlayStadium); effect(21, p.cannotPlayTool);
		effect(22, p.cannotPlayAceSpec); effect(23, p.cannotPlayAbilityPokemonNotRocket);
		effect(24, p.cannotTrashToHandAbilityOrTrainers); effect(25, p.playerDamageChange);
		effect(26, p.playerDamageChangeEx); effect(27, p.playerDamageChangeMyFighting);
		effect(28, p.takePrizeCountChangeTerastalAttackKoActive); effect(29, p.takePrizeCountChangeNAttackKoActive);
	}
	template<class Callback>
	static void appendGlobalEffects(const State& s, Callback&& add) {
		auto effect = [&](int id, int value) { if (value) add(EffectTokenBase + 100 + id, GlobalEffect, value); };
		effect(1, s.turnState); effect(2, s.continualState); effect(3, s.turnHistories[0].koTeamRocket);
		effect(4, s.turnHistories[0].koAttackDamage); effect(5, s.turnHistories[0].koAttackDamageEthan);
		effect(6, s.turnHistories[0].koAttackDamageHop);
	}
	template<class Callback>
	static void appendCardEffects(const Card& c, Callback&& add) {
		auto effect = [&](int id, int value) { if (value) add(EffectTokenBase + 200 + id, EntityEffect, value); };
		effect(1, c.thisTurn.cannotUseAttackId); effect(2, c.thisTurn.cannotUseAttackId2);
		effect(3, c.thisTurn.damageChange); effect(4, c.thisTurn.damageChangeActive);
		effect(5, c.thisTurn.damageChangeMyAttack); effect(6, c.thisTurn.attackCostChange);
		effect(7, c.thisTurn.retreatCostChange); effect(8, c.thisTurn.cannotRetreat);
		effect(9, c.thisTurn.cannotHandAttachEnergy); effect(10, c.thisTurn.cannotAttack);
		effect(11, c.nextTurn.cannotUseAttackId); effect(12, c.nextTurn.cannotUseAttackId2);
		effect(13, c.nextTurn.damageChange); effect(14, c.nextTurn.damageChangeActive);
		effect(15, c.nextTurn.damageChangeMyAttack); effect(16, c.nextTurn.attackCostChange);
		effect(17, c.nextTurn.retreatCostChange); effect(18, c.nextTurn.cannotRetreat);
		effect(19, c.nextTurn.cannotHandAttachEnergy); effect(20, c.nextTurn.cannotAttack);
		effect(21, c.hpChange); effect(22, c.damageChange); effect(23, c.damageChangeActive);
		effect(24, c.damageChangeEx); effect(25, c.damageChangeAbility); effect(26, c.damageChangeEvolved);
		effect(27, c.damageChangeEnemyTakenPrize); effect(28, c.takeDamageChange);
		effect(29, c.takeEnemyAttackDamageChange); effect(30, c.takeEnemyAbilityPokemonAttackDamageChange);
		effect(31, c.takeEnemyFireOrWaterPokemonAttackDamageChange); effect(32, c.takeEnemy4TypePokemonAttackDamageChange);
		effect(33, c.noDamageGreaterEqual); effect(34, c.retreatCostChange); effect(35, c.attackCostChangeColorless);
		effect(36, c.attackCostDown); effect(37, c.attackCostDownColorlessOwnAttack); effect(38, c.typeIndex);
		effect(39, c.weaknessIndex); effect(40, c.noAbility); effect(41, c.noKoMeAbility);
		effect(42, c.noDamageEnemyAttack); effect(43, c.noEffectEnemyAttack); effect(44, c.noEffectEnemyItem);
		effect(45, c.noEffectEnemySupporter); effect(46, c.noSpecialCondition); effect(47, c.noSleepParalyzeConfuse);
		effect(48, c.noSleep); effect(49, c.noRetreatCost); effect(50, c.noPrizeEx);
		effect(51, c.canUsePreEvolutionAttack); effect(52, c.canEvolveAppearTurn); effect(53, c.canAttackFirst);
		effect(54, c.cannotRetreat); effect(55, c.cannotAttack); effect(56, c.cannotToHand);
		effect(57, c.attackEnergyColoressOne); effect(58, c.attackEnergyPsychicOne);
		effect(59, c.doubleGrassEnergy); effect(60, c.basicPrizePlus1); effect(61, c.doubleAttack);
		effect(62, c.tool2); effect(63, c.tool4); effect(64, c.technicalMachine);
		effect(65, c.takeDamageChangeNextEnemyTurn); effect(66, c.noDamageLessEqualAttackNextEnemyTurn);
		effect(67, c.noDamageAndEffectAttackNextEnemyTurn); effect(68, c.noWeaknessNextEnemyTurn);
	}
};
