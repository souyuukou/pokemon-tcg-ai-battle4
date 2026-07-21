// SPDX-FileCopyrightText: © Pokémon/Nintendo/Creatures/GAME FREAK TM, ®, and character names are trademarks of Nintendo.
// SPDX-License-Identifier: LicenseRef-PTCG-ABC-Competition-Use-Only
// Part of the Pokémon TCG AI Battle Challenge. Provided for Competition use only;
// the full license is in the LICENSES/ folder and incorporates the Competition Rules.
// Competition Rules: https://www.kaggle.com/competitions/pokemon-tcg-ai-battle/rules

#pragma once

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#endif

#include "Card.h"
#include "Core.h"
#include "ExactBigRational.h"
#include "State.h"
#include "Types.h"

// Symbolic skeleton DAG shared across multi-draw outcomes that differ only in
// turn-inert card identities.  Structure is expanded once per isomorphism class;
// each concrete outcome then bottom-up sweeps values with its own integer weights
// and leaf evaluations.  If an "inert" card becomes a legal operator during
// expansion, the class is discarded and the caller falls back to per-outcome
// search so exactness is never compromised.
namespace ExactSkeleton {

enum class CertScope : unsigned char { ExactEvaluatorExpectation, Argmax };

inline CertScope CertScopeFromEnvironment() {
	// Default is argmax proof: stop once best.lower >= max(other.upper).
	// Set PTCG_EXACT_CERT_SCOPE=exact to require a point-valued best action.
#ifdef _WIN32
	char configured[64]{};
	DWORD length = GetEnvironmentVariableA("PTCG_EXACT_CERT_SCOPE", configured, (DWORD)std::size(configured));
	if (length == 0 || length >= std::size(configured)) return CertScope::Argmax;
	if (_stricmp(configured, "exact") == 0
		|| _stricmp(configured, "exact_evaluator_expectation") == 0)
		return CertScope::ExactEvaluatorExpectation;
	if (_stricmp(configured, "argmax") == 0) return CertScope::Argmax;
#else
	const char* configured = std::getenv("PTCG_EXACT_CERT_SCOPE");
	if (configured == nullptr) return CertScope::Argmax;
	if (std::strcmp(configured, "exact") == 0
		|| std::strcmp(configured, "exact_evaluator_expectation") == 0)
		return CertScope::ExactEvaluatorExpectation;
	if (std::strcmp(configured, "argmax") == 0) return CertScope::Argmax;
#endif
	return CertScope::Argmax;
}

inline const char* CertScopeName(CertScope scope) {
	return scope == CertScope::Argmax ? "argmax" : "exact_evaluator_expectation";
}

// Conservative turn-inert test for a single card identity given the current
// once-per-turn flags and the actor's public board.  False negatives are safe
// (smaller classes); false positives are caught by the runtime guard.
inline bool IsTurnInertCardId(const State& state, int actor, int cardId) {
	const CardMaster* master = FindCardMaster(cardId);
	if (master == nullptr) return false;
	const PlayerState& player = state.players[actor];
	if (IsEnergy(master->cardType)) {
		// After the once-per-turn attach, every remaining Energy is inert for the
		// rest of this turn (Enriching Energy's own draw lands in this regime).
		if (state.energyPlayed) return true;
		return player.thisTurn.cannotPlaySpecialEnergy
			&& master->cardType == CardType::SpecialEnergy;
	}
	if (master->cardType == CardType::Supporter) {
		return state.supporterPlayed || player.thisTurn.cannotPlaySupporter
			|| (state.turn <= 1 && !master->canPlayFirstTurn);
	}
	if (master->cardType == CardType::Stadium) {
		return state.stadiumPlayed || player.cannotPlayStadium || player.thisTurn.cannotPlayStadium;
	}
	if (master->cardType == CardType::Pokemon) {
		if (master->evolutionType == EvolutionType::Basic) return false;
		if (state.turn <= 2) return true;
		// Stage1/Stage2 with no matching pre-evolution currently in play is inert
		// until a search/bench change puts one there; treating that as inert is
		// conservative only when no matching pokemon is already on the field.
		bool hasMatch = false;
		auto consider = [&](CardRef ref) {
			if (ref.isNull() || hasMatch) return;
			const CardMaster& field = state.getCard(ref).getMaster();
			if (master->evolvesFrom == field.name || master->evolvesFrom == field.nameEn)
				hasMatch = true;
		};
		for (CardRef ref : player.active) consider(ref);
		for (CardRef ref : player.bench) consider(ref);
		return !hasMatch;
	}
	// Items/Tools: moment-inert when board Exist/Switch conditions fail and a
	// hand draw cannot repair them. Runtime guard still fail-closes if wrong.
	if (master->cardType == CardType::Item || master->cardType == CardType::Tool) {
		if (master->play == nullptr) return false;
		for (const Effect& effect : master->play->effects) {
			if (!effect.isCondition) {
				if (effect.effectType == EffectType::Switch
					&& effect.target.targetPlayer == TargetPlayer::Me
					&& player.bench.empty())
					return true;
				break;
			}
			for (AreaType area : effect.target.areas) {
				const bool enemy = effect.target.targetPlayer == TargetPlayer::Enemy
					|| effect.target.targetPlayer == TargetPlayer::Both;
				const bool me = effect.target.targetPlayer == TargetPlayer::Me
					|| effect.target.targetPlayer == TargetPlayer::Both
					|| effect.target.targetPlayer == TargetPlayer::None;
				auto emptyArea = [&](int playerIndex, AreaType a) -> bool {
					const PlayerState& ps = state.players[playerIndex];
					switch (a) {
					case AreaType::Bench: return ps.bench.empty();
					case AreaType::Energy: return ps.energy.empty();
					case AreaType::Tool: return ps.tool.empty();
					case AreaType::Stadium: return state.stadium.empty();
					case AreaType::Active: return ps.active.empty();
					default: return false;
					}
				};
				if (me && emptyArea(actor, area)) return true;
				if (enemy && emptyArea(1 - actor, area)) return true;
			}
		}
		return false;
	}
	return false;
}

// Partition the drawn multiset into (active counts by cardId, inert total count,
// continuation-signature hash of the active multiset). Two outcomes with equal
// keys share a Skeleton class; the runtime guard confirms isomorphism.
inline void BuildOutcomeClassKey(const State& state, int actor,
	const std::vector<std::pair<int, ExactWeight>>& types,
	const std::vector<int>& atomCounts, std::string& key,
	int& activeCards, int& inertCards) {
	key.clear();
	key.append("SKC2");
	activeCards = 0;
	inertCards = 0;
	std::vector<std::pair<int, int>> active;
	active.reserve(atomCounts.size());
	for (int i = 0; i < (int)atomCounts.size() && i < (int)types.size(); ++i) {
		const int take = atomCounts[i];
		if (take <= 0) continue;
		const int cardId = types[i].first;
		if (IsTurnInertCardId(state, actor, cardId)) inertCards += take;
		else {
			active.push_back({ cardId, take });
			activeCards += take;
		}
	}
	std::sort(active.begin(), active.end());
	key.push_back('|');
	for (const auto& item : active) {
		key.append(std::to_string(item.first));
		key.push_back(':');
		key.append(std::to_string(item.second));
		key.push_back(';');
	}
	key.push_back('#');
	key.append(std::to_string(inertCards));
	// Continuation signature: cardType/play structure of active identities so
	// isomorphic Item multisets that differ only by inert fillers can still merge
	// when active sets match (already keyed above). Append a stable type mix hash.
	std::uint64_t sig = 1469598103934665603ULL;
	for (const auto& item : active) {
		const CardMaster* master = FindCardMaster(item.first);
		sig ^= (std::uint64_t)item.first; sig *= 1099511628211ULL;
		sig ^= (std::uint64_t)item.second; sig *= 1099511628211ULL;
		if (master != nullptr) {
			sig ^= (std::uint64_t)master->cardType; sig *= 1099511628211ULL;
			sig ^= (std::uint64_t)master->evolutionType; sig *= 1099511628211ULL;
		}
	}
	key.push_back('@');
	key.append(std::to_string(sig));
}

enum class NodeKind : unsigned char { DecisionMax, DecisionMin, Chance, Leaf, Blocked, SolveBridge };

struct DagEdge {
	std::string label;
	int child = -1;
	// Chance edges: atomCounts over the parent's chanceCardTypes schema.
	// Walk recomputes member-specific mass from the member's remaining deck;
	// `weight` is retained only as a representative diagnostic and must not be
	// used as a certified probability numerator.
	std::vector<int> atomCounts;
	ExactWeight weight;
};

struct DagNode {
	NodeKind kind = NodeKind::Blocked;
	std::vector<DagEdge> edges;
	std::string internKey;
	bool complete = false;
};

struct Dag {
	std::vector<DagNode> nodes;
	std::unordered_map<std::string, int> intern;
	int root = -1;
	bool inertGuardFailed = false;
	bool walkFailed = false;
	unsigned long long expandedNodes = 0;
	unsigned long long interiorChanceNodes = 0;
	unsigned long long macroCollapsed = 0;
	std::unordered_set<int> assumedInert;
};

struct OutcomeClass {
	std::string key;
	std::vector<size_t> memberIndices;
	int activeCards = 0;
	int inertCards = 0;
};

inline std::vector<OutcomeClass> ClassOutcomes(const State& preDrawState, int actor,
	const std::vector<std::pair<int, ExactWeight>>& types,
	const std::vector<std::vector<int>>& atomCountsList) {
	std::vector<OutcomeClass> classes;
	std::unordered_map<std::string, size_t> index;
	for (size_t i = 0; i < atomCountsList.size(); ++i) {
		std::string key;
		int active = 0, inert = 0;
		BuildOutcomeClassKey(preDrawState, actor, types, atomCountsList[i], key, active, inert);
		auto [found, inserted] = index.emplace(key, classes.size());
		if (inserted) {
			OutcomeClass group;
			group.key = key;
			group.activeCards = active;
			group.inertCards = inert;
			group.memberIndices.push_back(i);
			classes.push_back(std::move(group));
		} else {
			classes[found->second].memberIndices.push_back(i);
		}
	}
	return classes;
}

} // namespace ExactSkeleton
