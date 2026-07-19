// SPDX-FileCopyrightText: © Pokémon/Nintendo/Creatures/GAME FREAK TM, ®, and character names are trademarks of Nintendo.
// SPDX-License-Identifier: LicenseRef-PTCG-ABC-Competition-Use-Only
#pragma once

#include "Card.h"
#include "ExactCardPartition.h"
#include "Skill.h"
#include "State.h"
#include "Types.h"

#include <algorithm>
#include <cstdint>
#include <unordered_map>
#include <unordered_set>
#include <string>
#include <vector>

// Conservative turn-remainder liveness for V4 Passive Residual.
// Passive is allowed only when every PassiveProofV4 flag is proven true.
// "Currently unplayable" is NEVER sufficient by itself.
//
// Coverage is per-candidate (proveCandidate), not a single closure.complete() gate.
// Unrelated AttackDamage operators must not block Passive proof for a locked Supporter.
namespace ExactCardLivenessV4 {

static constexpr int LivenessSchemaVersion = 6;

enum class CardLiveness : unsigned char { Active = 0, Passive = 1, Unknown = 2 };

enum class OperatorSourceKind : unsigned char {
	Play = 0,
	Ability = 1,
	Delay = 2,
	AttackPre = 3,
	AttackPost = 4,
	Unknown = 5,
};

// Deferred functionStack entries. Opaque callbacks fail closed; EffectControl
// frames only resume skill resolution already covered by pendingEffectIndex+1
// and reachable-operator scans (TypedDeferredFunction long-term target).
enum class DeferredFunctionKind : unsigned char {
	Opaque = 0,
	EffectControl = 1,
};

struct OperatorSourceKey {
	int cardId = 0;
	int skillId = 0;
	int effectIndex = -1;
	OperatorSourceKind kind = OperatorSourceKind::Unknown;

	bool valid() const { return effectIndex >= 0 && skillId > 0; }

	// Match the concrete Effect slot (kind may differ if inferred).
	bool sameEffect(const OperatorSourceKey& other) const {
		return valid() && other.valid()
			&& cardId == other.cardId
			&& skillId == other.skillId
			&& effectIndex == other.effectIndex;
	}
};

enum class CardObservationKind : unsigned char {
	None = 0,
	CountOnly = 1,
	StaticPredicate = 2,
	CardIdentity = 3,
	CardOrder = 4,
	Unknown = 5
};

enum Reason : std::uint64_t {
	CurrentlyPlayable = 1ull << 0,
	MayBecomePlayable = 1ull << 1,
	ReferencedByReachableTarget = 1ull << 2,
	MayMoveZone = 1ull << 3,
	MayChangeLegality = 1ull << 4,
	MayTriggerEffect = 1ull << 5,
	MayBeDiscarded = 1ull << 6,
	MayBeReturnedToDeck = 1ull << 7,
	MayBeAttached = 1ull << 8,
	MayBeEvolved = 1ull << 9,
	MayBeUsedAsCost = 1ull << 10,
	UnknownEffect = 1ull << 11,
	UnsupportedTarget = 1ull << 12,
	InReachableSet = 1ull << 13,
	PassiveProven = 1ull << 14,
	EnergyOnceConsumed = 1ull << 15,
	SupporterLocked = 1ull << 16,
	StadiumLocked = 1ull << 17,
	EvolutionBlocked = 1ull << 18,
	HandTargetObserved = 1ull << 19,
	DeckIdentityObserved = 1ull << 20,
	ProofIncomplete = 1ull << 21,
	ClosureRequired = 1ull << 22,
	CoverageIncomplete = 1ull << 23,
};

struct PassiveProofV4 {
	bool handActionInvariant = false;
	bool handTargetInvariant = false;
	bool zoneMovementInvariant = false;
	bool deckRemovalInvariant = false;
	bool semanticInvariant = false;
	bool actionIndependentResidual = false;

	std::uint64_t reachableOperatorHash = 0;
	std::uint64_t partitionSchemaHash = 0;
	std::uint64_t proofHash = 0;

	bool allProven() const {
		return handActionInvariant && handTargetInvariant && zoneMovementInvariant
			&& deckRemovalInvariant && semanticInvariant && actionIndependentResidual;
	}
};

struct OperatorFootprint {
	OperatorSourceKey source{};
	int operatorCardId = 0; // == source.cardId (legacy accessors)
	EffectType effectType = EffectType::NoEffect;
	ConditionType conditionType = ConditionType::Always;
	bool isCondition = false;
	CardObservationKind observation = CardObservationKind::Unknown;
	Target target{};
	bool hasTarget = false;
	bool mayTargetHand = false;
	bool mayTargetDeck = false;
	bool mayDiscardHand = false;
	bool mayReturnHandToDeck = false;
	bool mayCountHandByType = false;
	bool mayCountHandTotal = false;
	bool maySearchDeckByIdentity = false;
	bool mayMoveCardZones = false;
};

struct OperatorClosure {
	std::unordered_set<int> reachableCards;
	std::unordered_set<int> reachableEffectTypes; // cast EffectType
	std::vector<OperatorFootprint> footprints;
	std::uint64_t reachableOperatorHash = 0;
	std::uint64_t partitionSchemaHash = 0;
	bool hasUnknown = false;
	// Diagnostic coverage of scanners (not the Passive gate — use proveCandidate).
	bool allCardOperatorsCovered = false;
	bool pendingEffectsCovered = false;
	bool globalEffectsCovered = false;
	bool actionCostsCovered = false;
	bool selectionContextsCovered = false;
	bool conditionsCovered = false;

	bool complete() const {
		return allCardOperatorsCovered && pendingEffectsCovered && globalEffectsCovered
			&& actionCostsCovered && selectionContextsCovered && conditionsCovered
			&& !hasUnknown;
	}
};

// Per-candidate coverage: an unrelated damage-only effect must not poison every card.
struct CandidateCoverageProof {
	bool handIdentitySafe = false;
	bool handTypeSafe = false;
	bool handCountSafe = false;
	bool deckRemovalSafe = false;
	bool zoneMovementSafe = false;
	bool actionCostSafe = false;
	bool selectionSafe = false;
	bool conditionSafe = false;

	bool allSafe() const {
		return handIdentitySafe && handTypeSafe && handCountSafe && deckRemovalSafe
			&& zoneMovementSafe && actionCostSafe && selectionSafe && conditionSafe;
	}

	static CandidateCoverageProof AllTrue() {
		CandidateCoverageProof p;
		p.handIdentitySafe = p.handTypeSafe = p.handCountSafe = true;
		p.deckRemovalSafe = p.zoneMovementSafe = p.actionCostSafe = true;
		p.selectionSafe = p.conditionSafe = true;
		return p;
	}
};

struct CoverageResult {
	bool analyzed = false;
	bool unknown = false;
	bool touchesHandIdentity = false;
	bool touchesHandType = false;
	bool touchesHandCount = false;
	bool touchesDeckIdentity = false;
	bool mayDiscardHand = false;
	bool mayReturnHandToDeck = false;
	bool mayMoveZones = false;
	bool mayObserveCardOrder = false;
	bool impliesFurtherChance = false;

	bool covered() const { return analyzed && !unknown; }
};

// Snapshot of which hand-cost / further-chance operators can actually fire
// after the current pending Draw returns to Main (Moment Passive).
struct PassiveMomentStateV4 {
	std::unordered_set<int> momentPlayableOperatorIds;
	int projectedHandSize = 0;
	bool itemPlayLocked = false;
	bool built = false;

	bool mayActivateOperator(int cardId) const {
		return built && momentPlayableOperatorIds.contains(cardId);
	}
};

inline int SkillLeadingHandDiscardCount(const Skill* skill) {
	if (skill == nullptr) return 0;
	for (const Effect& effect : skill->effects) {
		if (effect.isCondition) continue;
		if (effect.effectType != EffectType::ToTrash) return 0;
		bool hand = false;
		for (AreaType area : effect.target.areas) if (area == AreaType::Hand) hand = true;
		if (!hand) return 0;
		int count = (int)effect.selectCount;
		if (count <= 0) count = 1;
		return count;
	}
	return 0;
}

// Board-only Exist failures that a hand draw cannot repair (bench/energy/stadium).
inline bool ItemBoardConditionsUnsatisfiable(const State& state, int actor, const CardMaster& master) {
	if (master.play == nullptr) return false;
	for (const Effect& effect : master.play->effects) {
		if (!effect.isCondition) {
			// Switch-self with empty bench cannot become legal from a hand draw alone.
			if (effect.effectType == EffectType::Switch
				&& effect.target.targetPlayer == TargetPlayer::Me
				&& state.players[actor].bench.empty())
				return true;
			break;
		}
		for (AreaType area : effect.target.areas) {
			const bool enemy = effect.target.targetPlayer == TargetPlayer::Enemy
				|| effect.target.targetPlayer == TargetPlayer::Both;
			const bool me = effect.target.targetPlayer == TargetPlayer::Me
				|| effect.target.targetPlayer == TargetPlayer::Both
				|| effect.target.targetPlayer == TargetPlayer::None;
			auto emptyArea = [&](int player, AreaType a) -> bool {
				const PlayerState& ps = state.players[player];
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

inline PassiveMomentStateV4 BuildPassiveMomentState(const State& state, int actor) {
	PassiveMomentStateV4 moment;
	moment.built = true;
	const PlayerState& ps = state.players[actor];
	moment.itemPlayLocked = ps.thisTurn.cannotPlayItem || ps.cannotPlayItem;
	int handSize = 0;
	for (CardRef ref : ps.hand) if (!ref.isNull()) ++handSize;
	int pendingAdd = 0;
	if (state.exact.pending == ExactPendingType::Draw && state.exact.pendingPlayer == actor)
		pendingAdd = (int)state.exact.pendingCount;
	moment.projectedHandSize = handSize + pendingAdd;

	auto considerHandCard = [&](int cardId) {
		const CardMaster* master = FindCardMaster(cardId);
		if (master == nullptr) return;
		if (master->cardType == CardType::Item || master->cardType == CardType::Tool) {
			if (moment.itemPlayLocked) return;
			if (ItemBoardConditionsUnsatisfiable(state, actor, *master)) return;
			const int trash = SkillLeadingHandDiscardCount(master->play);
			// Need `trash` other cards besides the Item itself after the pending draw.
			if (trash > 0 && moment.projectedHandSize - 1 < trash) return;
			moment.momentPlayableOperatorIds.insert(cardId);
			return;
		}
		if (master->cardType == CardType::Supporter) {
			if (state.supporterPlayed || ps.thisTurn.cannotPlaySupporter) return;
			if (state.turn <= 1 && !master->canPlayFirstTurn) return;
			moment.momentPlayableOperatorIds.insert(cardId);
			return;
		}
		if (master->cardType == CardType::Stadium) {
			if (state.stadiumPlayed || ps.cannotPlayStadium || ps.thisTurn.cannotPlayStadium) return;
			moment.momentPlayableOperatorIds.insert(cardId);
			return;
		}
		if (IsEnergy(master->cardType)) {
			if (state.energyPlayed) return;
			moment.momentPlayableOperatorIds.insert(cardId);
			return;
		}
		// Basics / evolutions may become operators; treat as moment-active.
		moment.momentPlayableOperatorIds.insert(cardId);
	};

	if (state.selectType == SelectType::Main) {
		for (const SelectOption& option : state.options) {
			if (option.type == SelectOptionType::Play) {
				if (option.param0 < 0 || option.param0 >= (int)ps.hand.size()) continue;
				CardRef ref = ps.hand[option.param0];
				if (!ref.isNull()) considerHandCard(state.getCard(ref).cardId);
			} else if (option.type == SelectOptionType::Attach
				|| option.type == SelectOptionType::Evolve
				|| option.type == SelectOptionType::Ability) {
				try {
					CardRef ref = state.getCardRef((AreaType)option.param0, option.param1, actor);
					if (!ref.isNull()) considerHandCard(state.getCard(ref).cardId);
				} catch (...) {}
			}
		}
	}
	for (CardRef ref : ps.hand) {
		if (ref.isNull()) continue;
		considerHandCard(state.getCard(ref).cardId);
	}
	// In-play operators (abilities / stadium) can always fire without a draw.
	if (!state.stadium.empty()) {
		const Card& card = state.getCard(state.stadium[0]);
		moment.momentPlayableOperatorIds.insert(card.cardId);
	}
	for (int p = 0; p < 2; ++p) {
		const PlayerState& board = state.players[p];
		auto addRef = [&](CardRef ref) {
			if (ref.isNull()) return;
			moment.momentPlayableOperatorIds.insert(state.getCard(ref).cardId);
		};
		if (!board.active.empty()) addRef(board.active[0]);
		for (CardRef ref : board.bench) addRef(ref);
		for (CardRef ref : board.tool) addRef(ref);
		for (CardRef ref : board.energy) addRef(ref);
	}
	return moment;
}

inline void MergeCoverageHazardFromResult(CoverageResult& into, const CoverageResult& src) {
	if (!src.analyzed) return;
	into.analyzed = true;
	if (src.unknown) into.unknown = true;
	if (src.touchesHandIdentity) into.touchesHandIdentity = true;
	if (src.touchesHandType) into.touchesHandType = true;
	if (src.touchesHandCount) into.touchesHandCount = true;
	if (src.touchesDeckIdentity) into.touchesDeckIdentity = true;
	if (src.mayDiscardHand) into.mayDiscardHand = true;
	if (src.mayReturnHandToDeck) into.mayReturnHandToDeck = true;
	if (src.mayMoveZones) into.mayMoveZones = true;
	if (src.mayObserveCardOrder) into.mayObserveCardOrder = true;
	if (src.impliesFurtherChance) into.impliesFurtherChance = true;
}

// Function indices for skill-pipeline frames that only resume effect resolution.
// Populated by ExactEnsureDeferredFunctionRegistry() once GameProc symbols exist.
inline std::unordered_set<int>& EffectControlFunctionIndices() {
	static std::unordered_set<int> indices;
	return indices;
}

inline void RegisterEffectControlFunction(int functionIndex) {
	if (functionIndex >= 0) EffectControlFunctionIndices().insert(functionIndex);
}

inline void RegisterDeferredFunctionSemantics(int functionIndex, DeferredArgSemantics semantics) {
	RegisterDeferredFunctionSemanticTableEntry(functionIndex, semantics);
}

// Short-term TypedDeferredFunction: EffectControl frames are safe relative to
// Passive hand/deck reads (remaining effects are scanned separately). Any other
// callback is Opaque → unknown + furtherChance.
inline CoverageResult ScanDeferredFunctionStack(const State& state) {
	CoverageResult r;
	r.analyzed = true;
	if (state.functionStack.empty()) return r;
	const auto& ok = EffectControlFunctionIndices();
	if (ok.empty()) {
		r.unknown = true;
		r.impliesFurtherChance = true;
		return r;
	}
	for (const GameFunction& gf : state.functionStack) {
		const DeferredArgSemantics semantics = DeferredSemanticsFor(gf);
		const int argumentCount = gf.argType == ArgType::None ? 0
			: (gf.argType == ArgType::I || gf.argType == ArgType::B ? 1
				: (gf.argType == ArgType::II ? 2 : 3));
		for (int i = 0; i < argumentCount; ++i) {
			if (semantics[(size_t)i] == DeferredArgSemantic::Unknown
				|| semantics[(size_t)i] == DeferredArgSemantic::None) {
				r.unknown = true;
				r.impliesFurtherChance = true;
				return r;
			}
		}
		if (!ok.count(gf.functionIndex)) {
			r.unknown = true;
			r.impliesFurtherChance = true;
			return r;
		}
	}
	return r;
}

struct CardLivenessResult {
	CardLiveness liveness = CardLiveness::Unknown;
	std::uint64_t reasonMask = 0;
	PassiveProofV4 proof{};
	CandidateCoverageProof coverage{};
};

inline CardObservationKind ObservationKindForEffect(EffectType type) {
	switch (type) {
	case EffectType::NoEffect:
	case EffectType::AttackDamage:
	case EffectType::AttackDamageChange:
	case EffectType::AttackDamageMulti:
	case EffectType::AttackDamageCoin:
	case EffectType::Coin:
	case EffectType::CoinUntilTail:
	case EffectType::NotMove:
	case EffectType::Switch:
	case EffectType::Paralyze:
	case EffectType::Heal:
	case EffectType::HealAll:
	case EffectType::HealSand:
	case EffectType::ResetHp:
	case EffectType::Drain:
	case EffectType::DamageCounter:
	case EffectType::DamageCounterRemoved:
	case EffectType::DamageCounterDamaged:
	case EffectType::DamageCounterAny:
	case EffectType::DamageCounterDouble:
	case EffectType::DamageCounterHp:
	case EffectType::RemoveDamageCounter:
	case EffectType::RemoveDamageCounterAll:
	case EffectType::AttackDamageChangeTargetCount:
	case EffectType::EffectDamageChangeTargetCount:
	case EffectType::AttackDamageChangeEnergyCount:
	case EffectType::EffectDamageChangeEnergyCount:
	case EffectType::AttackDamageChangeTypeEnergyCount:
	case EffectType::EffectDamageChangeTypeEnergyCount:
	case EffectType::AttackDamageChangeEnergyCountCoin:
	case EffectType::AttackDamageChangeTypeEnergyCountCoin:
	case EffectType::AttackDamageChangeCoin:
	case EffectType::AttackDamageChangeCoinUntilTail:
	case EffectType::AttackDamageChangeTargetCountCoin:
	case EffectType::DelayEffect:
		return CardObservationKind::None;
	case EffectType::Draw:
	case EffectType::DrawTargetCount:
	case EffectType::DrawPrizeCount:
	case EffectType::DrawUntil:
	case EffectType::DrawUntilPsychic:
	case EffectType::DrawMirror:
		return CardObservationKind::CountOnly;
	case EffectType::LookDeck:
	case EffectType::LookDeckReverse:
	case EffectType::LookDeckBottom:
	case EffectType::LookAndReturn:
	case EffectType::SwitchDeck:
	case EffectType::DeckToTrash:
	case EffectType::DeckBottomToTrash:
	case EffectType::ToDeck:
	case EffectType::ToDeckAndShuffle:
	case EffectType::ToHand:
	case EffectType::ToTrash:
	case EffectType::SelectCard:
	case EffectType::ForEach:
	case EffectType::SelectEvolvesFrom:
	case EffectType::SelectEvolvesTo:
	case EffectType::SelectAttachFrom:
	case EffectType::SelectAttachTo:
	case EffectType::Devolve:
	case EffectType::DevolveAny:
	case EffectType::TransformDeck:
	case EffectType::TransformTrash:
		return CardObservationKind::CardIdentity;
	case EffectType::ToDeckBottomClose:
	case EffectType::ToDeckBottomReverse:
		return CardObservationKind::CardOrder;
	case EffectType::BreakIfCoinHead:
	case EffectType::BreakIfCoinTail:
	case EffectType::BreakIfCoinTailMulti:
	case EffectType::SkipIfCoinTail:
	case EffectType::PostEffectActivate:
	case EffectType::BreakIfNotPostEffectActivated:
	case EffectType::FailAttack:
	case EffectType::CancelFailAttack:
	case EffectType::SelectActivate:
	case EffectType::SelectPoisonBurnConfuse:
	case EffectType::SelectSpecialCondition:
		return CardObservationKind::None;
	default:
		// Continual field modifiers (HP/cost/damage changes) do not observe hand IDs.
		if (type > EffectType::ContinualEffectSeparator)
			return CardObservationKind::None;
		return CardObservationKind::Unknown;
	}
}

inline CardObservationKind ObservationKindForCondition(ConditionType type) {
	switch (type) {
	case ConditionType::Always:
	case ConditionType::MyTurn:
	case ConditionType::Turn:
	case ConditionType::CoinHeadCount:
	case ConditionType::NotFullBench:
	case ConditionType::NoSameNameSkillThisTurn:
	case ConditionType::SameAttackPreMyTurn:
	case ConditionType::AttachActive:
	case ConditionType::KoPreEnemyTurn:
	case ConditionType::KoPreEnemyTurnTeamRocket:
	case ConditionType::KoAttackDamagePreEnemyTurn:
	case ConditionType::KoAttackDamageEthanPreEnemyTurn:
	case ConditionType::KoAttackDamageHopPreEnemyTurn:
		return CardObservationKind::None;
	case ConditionType::MysteryGarden:
		return CardObservationKind::CountOnly; // hand-size vs field count
	case ConditionType::CountEnergy:
	case ConditionType::CountEnergyType:
	case ConditionType::CompareCountEnergyMeEnemy:
	case ConditionType::AttackEnergyExtra:
	case ConditionType::CountTarget:
	case ConditionType::CountTarget2:
	case ConditionType::CountTargetMeOrEnemy:
	case ConditionType::CompareCountTargetMeEnemy:
	case ConditionType::AnyTargetAfterEffect:
		return CardObservationKind::StaticPredicate;
	case ConditionType::LoveBall:
		return CardObservationKind::CardIdentity;
	default:
		return CardObservationKind::Unknown;
	}
}

inline bool EffectKindBlocksPassive(CardObservationKind kind) {
	return kind == CardObservationKind::CardIdentity
		|| kind == CardObservationKind::CardOrder
		|| kind == CardObservationKind::Unknown;
}

inline bool TargetIncludesHand(const Target& target) {
	for (AreaType area : target.areas)
		if (area == AreaType::Hand) return true;
	return false;
}

inline bool TargetIncludesDeck(const Target& target) {
	for (AreaType area : target.areas)
		if (area == AreaType::Deck) return true;
	return false;
}

inline std::uint64_t MixHash(std::uint64_t hash, std::uint64_t value) {
	hash ^= value;
	hash *= 1099511628211ULL;
	return hash;
}

inline std::uint64_t StableHashBytes(const void* data, size_t size) {
	std::uint64_t hash = 1469598103934665603ULL;
	const auto* bytes = static_cast<const unsigned char*>(data);
	for (size_t i = 0; i < size; ++i) {
		hash ^= bytes[i];
		hash *= 1099511628211ULL;
	}
	return hash;
}

inline std::uint64_t StableHashString(const std::string& value) {
	return StableHashBytes(value.data(), value.size());
}

inline bool EffectImpliesFurtherChanceOrZoneMove(EffectType type) {
	switch (type) {
	case EffectType::Draw:
	case EffectType::DrawTargetCount:
	case EffectType::DrawPrizeCount:
	case EffectType::DrawUntil:
	case EffectType::DrawUntilPsychic:
	case EffectType::DrawMirror:
	case EffectType::ToHand:
	case EffectType::ToDeck:
	case EffectType::ToDeckAndShuffle:
	case EffectType::ToDeckBottomClose:
	case EffectType::ToDeckBottomReverse:
	case EffectType::ToTrash:
	case EffectType::DeckToTrash:
	case EffectType::DeckBottomToTrash:
	case EffectType::LookDeck:
	case EffectType::LookDeckReverse:
	case EffectType::LookDeckBottom:
	case EffectType::LookAndReturn:
	case EffectType::SwitchDeck:
	case EffectType::SelectCard:
	case EffectType::ForEach:
	case EffectType::SelectEvolvesFrom:
	case EffectType::SelectEvolvesTo:
	case EffectType::SelectAttachFrom:
	case EffectType::SelectAttachTo:
		return true;
	default:
		return false;
	}
}

inline OperatorFootprint FootprintFromEffect(const OperatorSourceKey& source, const Effect& effect) {
	OperatorFootprint fp;
	fp.source = source;
	fp.operatorCardId = source.cardId;
	fp.effectType = effect.effectType;
	fp.conditionType = effect.conditionType;
	fp.isCondition = effect.isCondition;
	fp.target = effect.target;
	fp.hasTarget = true;
	fp.mayTargetHand = TargetIncludesHand(effect.target);
	fp.mayTargetDeck = TargetIncludesDeck(effect.target);
	if (effect.isCondition) {
		fp.observation = ObservationKindForCondition(effect.conditionType);
		if (effect.conditionType == ConditionType::MysteryGarden)
			fp.mayCountHandTotal = true;
		if (fp.mayTargetHand && !effect.target.conditions.empty())
			fp.mayCountHandByType = true;
		if (fp.mayTargetHand && (fp.observation == CardObservationKind::CountOnly
			|| fp.observation == CardObservationKind::StaticPredicate))
			fp.mayCountHandTotal = fp.mayCountHandTotal
				|| effect.conditionType == ConditionType::CountTarget
				|| effect.conditionType == ConditionType::CountTarget2;
		return fp;
	}
	fp.observation = ObservationKindForEffect(effect.effectType);
	fp.mayMoveCardZones = effect.effectType == EffectType::ToTrash
		|| effect.effectType == EffectType::ToDeck
		|| effect.effectType == EffectType::ToDeckAndShuffle
		|| effect.effectType == EffectType::ToHand
		|| effect.effectType == EffectType::DeckToTrash
		|| effect.effectType == EffectType::SelectCard
		|| effect.effectType == EffectType::ForEach;
	fp.mayDiscardHand = fp.mayTargetHand
		&& (effect.effectType == EffectType::ToTrash
			|| effect.effectType == EffectType::SelectCard
			|| effect.effectType == EffectType::ForEach);
	fp.mayReturnHandToDeck = fp.mayTargetHand
		&& (effect.effectType == EffectType::ToDeck
			|| effect.effectType == EffectType::ToDeckAndShuffle
			|| effect.effectType == EffectType::ToDeckBottomClose
			|| effect.effectType == EffectType::ToDeckBottomReverse);
	fp.mayCountHandByType = fp.mayTargetHand && !effect.target.conditions.empty();
	fp.maySearchDeckByIdentity = fp.mayTargetDeck && EffectKindBlocksPassive(fp.observation);
	return fp;
}

// Legacy helper used by diagnostics that only know a card id.
inline OperatorFootprint FootprintFromEffect(int cardId, const Effect& effect) {
	OperatorSourceKey source;
	source.cardId = cardId;
	source.skillId = effect.skillId > 0 ? effect.skillId : 0;
	source.effectIndex = 0;
	source.kind = OperatorSourceKind::Unknown;
	return FootprintFromEffect(source, effect);
}

inline OperatorSourceKind KindFromSkillType(SkillType type) {
	switch (type) {
	case SkillType::Ability: return OperatorSourceKind::Ability;
	case SkillType::Play:
	case SkillType::Attach: return OperatorSourceKind::Play;
	default: return OperatorSourceKind::Unknown;
	}
}

inline OperatorSourceKind KindForSkillOnCard(const CardMaster* master, const Skill& skill) {
	if (master != nullptr) {
		if (master->play != nullptr && master->play->skillId == skill.skillId)
			return OperatorSourceKind::Play;
		if (master->ability != nullptr && master->ability->skillId == skill.skillId)
			return OperatorSourceKind::Ability;
		if (master->delay != nullptr && master->delay->skillId == skill.skillId)
			return OperatorSourceKind::Delay;
	}
	return KindFromSkillType(skill.skillType);
}

// Concrete Effect currently owning the pending chance (Draw etc.).
inline OperatorSourceKey PendingEffectSourceKey(const State& state) {
	OperatorSourceKey key;
	if (state.exact.pendingSkillId <= 0 || state.exact.pendingEffectIndex < 0)
		return key;
	auto skill = SkillTable.find(state.exact.pendingSkillId);
	if (skill == SkillTable.end()) return key;
	key.cardId = skill->second.cardId;
	key.skillId = state.exact.pendingSkillId;
	key.effectIndex = state.exact.pendingEffectIndex;
	key.kind = KindForSkillOnCard(FindCardMaster(key.cardId), skill->second);
	return key;
}

// True if the target filter may select this candidate (fail closed on unsupported).
inline bool targetMayMatchCandidate(const Target& target, int candidateCardId) {
	const CardMaster* master = FindCardMaster(candidateCardId);
	if (master == nullptr) return true;
	if (target.conditions.empty()) return true;
	ExactStaticTargetResult result = ExactStaticTargetMatches(*master, target);
	if (!result.supported) return true;
	return result.matches;
}

inline bool footprintMayAffectCandidate(const OperatorFootprint& fp, int candidateCardId) {
	if (!fp.hasTarget) return true;
	// Zone-agnostic unknown footprints always apply.
	if (fp.observation == CardObservationKind::Unknown) return true;
	// Hand/deck hazards only matter when the target filter can hit the candidate.
	if (fp.mayTargetHand || fp.mayTargetDeck || fp.mayDiscardHand || fp.mayReturnHandToDeck
		|| fp.mayCountHandByType || fp.mayCountHandTotal || fp.maySearchDeckByIdentity)
		return targetMayMatchCandidate(fp.target, candidateCardId);
	return true;
}

inline void MergeCoverageHazard(CoverageResult& into, const OperatorFootprint& fp) {
	if (fp.observation == CardObservationKind::Unknown) into.unknown = true;
	if (fp.mayTargetHand && EffectKindBlocksPassive(fp.observation))
		into.touchesHandIdentity = true;
	if (fp.mayCountHandByType) into.touchesHandType = true;
	if (fp.mayCountHandTotal) into.touchesHandCount = true;
	if (fp.maySearchDeckByIdentity
		|| (fp.mayTargetDeck && EffectKindBlocksPassive(fp.observation)))
		into.touchesDeckIdentity = true;
	if (fp.mayDiscardHand) into.mayDiscardHand = true;
	if (fp.mayReturnHandToDeck) into.mayReturnHandToDeck = true;
	if (fp.mayMoveCardZones) into.mayMoveZones = true;
	if (fp.observation == CardObservationKind::CardOrder) into.mayObserveCardOrder = true;
	if (!fp.isCondition && EffectImpliesFurtherChanceOrZoneMove(fp.effectType))
		into.impliesFurtherChance = true;
}

inline void ApplyHazardToCandidate(CandidateCoverageProof& proof, const CoverageResult& haz) {
	if (!haz.analyzed || haz.unknown) {
		proof.handIdentitySafe = false;
		proof.handTypeSafe = false;
		proof.handCountSafe = false;
		proof.deckRemovalSafe = false;
		proof.zoneMovementSafe = false;
		proof.actionCostSafe = false;
		proof.selectionSafe = false;
		proof.conditionSafe = false;
		return;
	}
	if (haz.touchesHandIdentity) proof.handIdentitySafe = false;
	if (haz.touchesHandType) proof.handTypeSafe = false;
	if (haz.touchesHandCount) proof.handCountSafe = false;
	if (haz.touchesDeckIdentity || haz.mayObserveCardOrder) proof.deckRemovalSafe = false;
	if (haz.mayMoveZones) proof.zoneMovementSafe = false;
	if (haz.mayDiscardHand || haz.mayReturnHandToDeck) proof.actionCostSafe = false;
}

inline CoverageResult ScanPendingEffects(const State& state) {
	CoverageResult r;
	r.analyzed = true;
	const ExactHiddenState& exact = state.exact;
	if (exact.pending == ExactPendingType::Opaque) {
		r.unknown = true;
		r.impliesFurtherChance = true;
		return r;
	}
	// The draw chance currently under analysis is not an extra Passive hazard —
	// Passive pools describe identities inside this draw.
	if (exact.pending == ExactPendingType::Draw)
		return r;
	if (exact.pending == ExactPendingType::TakePrize
		|| exact.pending == ExactPendingType::RevealDeck) {
		r.impliesFurtherChance = true;
		if (exact.pendingIntent == ExactQueryIntent::ConcreteCards)
			r.touchesDeckIdentity = true;
	}
	if (exact.pendingSkillId > 0 && exact.pendingEffectIndex >= 0) {
		auto skill = SkillTable.find(exact.pendingSkillId);
		if (skill == SkillTable.end()
			|| exact.pendingEffectIndex >= (int)skill->second.effects.size()) {
			r.unknown = true;
			return r;
		}
		const Effect& effect = skill->second.effects[exact.pendingEffectIndex];
		// Skip the Draw effect that owns this pending chance.
		if (effect.effectType == EffectType::Draw
			|| effect.effectType == EffectType::DrawTargetCount
			|| effect.effectType == EffectType::DrawPrizeCount
			|| effect.effectType == EffectType::DrawUntil
			|| effect.effectType == EffectType::DrawUntilPsychic
			|| effect.effectType == EffectType::DrawMirror)
			return r;
		OperatorFootprint fp = FootprintFromEffect(skill->second.cardId, effect);
		MergeCoverageHazard(r, fp);
	}
	return r;
}

inline CoverageResult ScanSelectionContexts(const State& state, int candidateCardId = 0) {
	CoverageResult r;
	r.analyzed = true;

	auto markRefIfCandidate = [&](CardRef ref) {
		if (ref.isNull()) return;
		const Card& card = state.getCard(ref);
		if (candidateCardId != 0 && card.cardId != candidateCardId) return;
		if (card.area == AreaType::Hand) {
			r.touchesHandIdentity = true;
			r.mayMoveZones = true;
		} else if (card.area == AreaType::Deck || card.area == AreaType::Looking
			|| card.area == AreaType::Prize) {
			r.touchesDeckIdentity = true;
		}
	};
	auto markAreaRef = [&](const AreaRef& areaRef) { markRefIfCandidate(areaRef.card); };

	auto tryHandCard = [&](int handIndex) -> CardRef {
		const int player = state.activePlayerIndex();
		if (handIndex < 0 || handIndex >= (int)state.players[player].hand.size())
			return {};
		return state.players[player].hand[handIndex];
	};

	for (const SelectOption& option : state.options) {
		switch (option.type) {
		case SelectOptionType::Card:
		case SelectOptionType::ToolCard:
		case SelectOptionType::EnergyCard: {
			CardPosition pos = option.getCardPosition();
			CardRef ref{};
			const int player = pos.playerIndex;
			if (player >= 0 && player < 2) {
				const PlayerState& ps = state.players[player];
				auto inRange = [&](const auto& list) {
					return pos.areaIndex >= 0 && pos.areaIndex < (int)list.size();
				};
				if (pos.area == AreaType::Hand && inRange(ps.hand)) ref = ps.hand[pos.areaIndex];
				else if (pos.area == AreaType::Deck && inRange(ps.deck)) ref = ps.deck[pos.areaIndex];
				else if (pos.area == AreaType::Prize && inRange(ps.prize)) ref = ps.prize[pos.areaIndex];
				else if (pos.area == AreaType::Looking && inRange(state.looking))
					ref = state.looking[pos.areaIndex];
			}
			if (!ref.isNull()) {
				markRefIfCandidate(ref);
			} else if (candidateCardId == 0) {
				if (pos.area == AreaType::Hand) {
					r.touchesHandIdentity = true;
					r.mayMoveZones = true;
				}
				if (pos.area == AreaType::Deck || pos.area == AreaType::Looking
					|| pos.area == AreaType::Prize)
					r.touchesDeckIdentity = true;
			} else {
				// Unresolved option with a concrete candidate: fail closed for that zone.
				if (pos.area == AreaType::Hand) {
					r.touchesHandIdentity = true;
					r.mayMoveZones = true;
				}
				if (pos.area == AreaType::Deck || pos.area == AreaType::Looking
					|| pos.area == AreaType::Prize)
					r.touchesDeckIdentity = true;
			}
			break;
		}
		case SelectOptionType::Play: {
			CardRef ref = tryHandCard(option.param0);
			if (!ref.isNull()) {
				markRefIfCandidate(ref);
			} else {
				r.touchesHandIdentity = true;
			}
			break;
		}
		case SelectOptionType::Attach:
		case SelectOptionType::Evolve:
		case SelectOptionType::Ability: {
			// param0 = AreaType, param1 = area index (see SelectOption construction).
			CardRef ref{};
			try {
				ref = state.getCardRef((AreaType)option.param0, option.param1,
					state.activePlayerIndex());
			} catch (...) {}
			if (!ref.isNull()) {
				markRefIfCandidate(ref);
			} else {
				r.touchesHandIdentity = true;
			}
			break;
		}
		case SelectOptionType::Discard: {
			CardRef ref = tryHandCard(option.param0);
			if (!ref.isNull()) {
				if (candidateCardId == 0 || state.getCard(ref).cardId == candidateCardId) {
					r.touchesHandIdentity = true;
					r.mayDiscardHand = true;
					r.mayMoveZones = true;
				}
			} else {
				r.touchesHandIdentity = true;
				r.mayDiscardHand = true;
				r.mayMoveZones = true;
			}
			break;
		}
		case SelectOptionType::Energy:
			r.mayMoveZones = true;
			break;
		default:
			break;
		}
	}

	if (state.selectType != SelectType::None && state.selectType != SelectType::Main
		&& state.selectType != SelectType::Attack && state.selectType != SelectType::YesNo
		&& state.options.empty()) {
		r.unknown = true;
	}

	for (CardRef ref : state.selectedList) markRefIfCandidate(ref);
	for (CardRef ref : state.looking) markRefIfCandidate(ref);
	for (CardRef ref : state.playing) markRefIfCandidate(ref);
	markRefIfCandidate(state.contextCard);
	for (const AreaRef& areaRef : state.preTargetList) markAreaRef(areaRef);
	for (const AreaRef& areaRef : state.targetList) markAreaRef(areaRef);
	return r;
}

inline CoverageResult ScanActionCosts(const State& /*state*/, const OperatorClosure& closure) {
	CoverageResult r;
	r.analyzed = true;
	for (const OperatorFootprint& fp : closure.footprints) {
		if (fp.isCondition) continue;
		if (fp.mayDiscardHand || fp.mayReturnHandToDeck || fp.mayCountHandByType
			|| fp.mayCountHandTotal) {
			MergeCoverageHazard(r, fp);
		}
	}
	return r;
}

inline CoverageResult ScanReachableConditions(const State& /*state*/, const OperatorClosure& closure) {
	CoverageResult r;
	r.analyzed = true;
	for (const OperatorFootprint& fp : closure.footprints) {
		if (!fp.isCondition) continue;
		MergeCoverageHazard(r, fp);
	}
	return r;
}

inline void AppendSkillFootprints(const Skill* skill, OperatorSourceKind kind,
	CoverageResult& into, int candidateCardId = 0) {
	if (skill == nullptr) return;
	for (int i = 0; i < (int)skill->effects.size(); ++i) {
		OperatorSourceKey source;
		source.cardId = skill->cardId;
		source.skillId = skill->skillId;
		source.effectIndex = i;
		source.kind = kind;
		OperatorFootprint fp = FootprintFromEffect(source, skill->effects[i]);
		if (candidateCardId != 0 && !footprintMayAffectCandidate(fp, candidateCardId))
			continue;
		MergeCoverageHazard(into, fp);
	}
}

inline void AppendCardMasterOperators(int cardId, const CardMaster* master, CoverageResult& into,
	bool includeAttacks, int candidateCardId = 0) {
	if (master == nullptr) { into.unknown = true; return; }
	AppendSkillFootprints(master->play, OperatorSourceKind::Play, into, candidateCardId);
	AppendSkillFootprints(master->ability, OperatorSourceKind::Ability, into, candidateCardId);
	AppendSkillFootprints(master->delay, OperatorSourceKind::Delay, into, candidateCardId);
	if (!includeAttacks) return;
	for (const Attack* attack : master->attacks) {
		if (attack == nullptr) continue;
		for (int i = 0; i < (int)attack->preEffects.size(); ++i) {
			OperatorSourceKey source;
			source.cardId = cardId;
			source.skillId = attack->attackId;
			source.effectIndex = i;
			source.kind = OperatorSourceKind::AttackPre;
			OperatorFootprint fp = FootprintFromEffect(source, attack->preEffects[i]);
			if (candidateCardId != 0 && !footprintMayAffectCandidate(fp, candidateCardId))
				continue;
			MergeCoverageHazard(into, fp);
		}
		for (int i = 0; i < (int)attack->postEffects.size(); ++i) {
			OperatorSourceKey source;
			source.cardId = cardId;
			source.skillId = attack->attackId;
			source.effectIndex = i;
			source.kind = OperatorSourceKind::AttackPost;
			OperatorFootprint fp = FootprintFromEffect(source, attack->postEffects[i]);
			if (candidateCardId != 0 && !footprintMayAffectCandidate(fp, candidateCardId))
				continue;
			MergeCoverageHazard(into, fp);
		}
	}
}

inline CoverageResult ScanGlobalEffects(const State& state, int candidateCardId = 0) {
	CoverageResult r;
	r.analyzed = true;
	const int active = state.activePlayerIndex();

	auto scanCard = [&](CardRef ref, bool includeAttacks) {
		if (ref.isNull()) return;
		const Card& card = state.getCard(ref);
		AppendCardMasterOperators(card.cardId, FindCardMaster(card.cardId), r, includeAttacks,
			candidateCardId);
	};
	auto scanTriggered = [&](const std::vector<TriggeredAbility>& stack) {
		for (const TriggeredAbility& ta : stack) {
			auto skill = SkillTable.find(ta.activateInfo.skillId);
			if (skill == SkillTable.end()) { r.unknown = true; continue; }
			AppendSkillFootprints(&skill->second, KindFromSkillType(skill->second.skillType),
				r, candidateCardId);
		}
	};

	if (!state.stadium.empty()) scanCard(state.stadium[0], false);
	for (int p = 0; p < 2; ++p) {
		const bool includeAttacks = (p == active);
		const PlayerState& ps = state.players[p];
		if (!ps.active.empty()) scanCard(ps.active[0], includeAttacks);
		for (int b = 0; b < (int)ps.bench.size(); ++b) scanCard(ps.bench[b], includeAttacks);
		for (CardRef ref : ps.tool) scanCard(ref, false);
		for (CardRef ref : ps.energy) scanCard(ref, false);
	}
	scanTriggered(state.delayTriggerStack);
	scanTriggered(state.temporaryTriggerStack);
	scanTriggered(state.triggerStack);
	MergeCoverageHazardFromResult(r, ScanDeferredFunctionStack(state));
	return r;
}

inline CandidateCoverageProof proveCandidate(
	const State& state,
	int candidateCardId,
	const OperatorClosure& closure,
	const PassiveMomentStateV4& moment = {}) {
	CandidateCoverageProof proof = CandidateCoverageProof::AllTrue();

	CoverageResult pending = ScanPendingEffects(state);
	CoverageResult global = ScanGlobalEffects(state, candidateCardId);
	CoverageResult selection = ScanSelectionContexts(state, candidateCardId);

	CoverageResult costs;
	costs.analyzed = true;
	CoverageResult conditions;
	conditions.analyzed = true;
	CoverageResult ops;
	ops.analyzed = true;
	bool anyUnknownFp = false;
	const OperatorSourceKey currentDraw = PendingEffectSourceKey(state);
	for (const OperatorFootprint& fp : closure.footprints) {
		if (currentDraw.valid() && fp.source.sameEffect(currentDraw))
			continue; // exclude only the pending Draw Effect itself
		if (!footprintMayAffectCandidate(fp, candidateCardId)) continue;
		// Moment Passive: deck-only discard/search operators are not costs until playable.
		if (moment.built && (fp.mayDiscardHand || fp.mayReturnHandToDeck)
			&& !moment.mayActivateOperator(fp.operatorCardId))
			continue;
		if (fp.observation == CardObservationKind::Unknown) anyUnknownFp = true;
		if (fp.isCondition) MergeCoverageHazard(conditions, fp);
		else if (fp.mayDiscardHand || fp.mayReturnHandToDeck || fp.mayCountHandByType
			|| fp.mayCountHandTotal) {
			MergeCoverageHazard(costs, fp);
		}
		MergeCoverageHazard(ops, fp);
	}
	if (anyUnknownFp || closure.hasUnknown) {
		ApplyHazardToCandidate(proof, CoverageResult{ true, true });
		return proof;
	}

	ApplyHazardToCandidate(proof, pending);
	ApplyHazardToCandidate(proof, global);
	ApplyHazardToCandidate(proof, costs);
	ApplyHazardToCandidate(proof, selection);
	ApplyHazardToCandidate(proof, conditions);
	ApplyHazardToCandidate(proof, ops);

	const bool selectionHazard = selection.unknown || selection.touchesHandIdentity
		|| selection.touchesDeckIdentity || selection.mayDiscardHand
		|| selection.mayReturnHandToDeck;
	if (selectionHazard) proof.selectionSafe = false;

	const bool conditionHazard = conditions.unknown || conditions.touchesHandIdentity
		|| conditions.touchesHandType || conditions.touchesHandCount;
	if (conditionHazard) proof.conditionSafe = false;

	return proof;
}

inline void ApplyStateCoverageScanners(OperatorClosure& closure, const State& state) {
	const CoverageResult pending = ScanPendingEffects(state);
	const CoverageResult global = ScanGlobalEffects(state);
	const CoverageResult costs = ScanActionCosts(state, closure);
	const CoverageResult selection = ScanSelectionContexts(state);
	const CoverageResult conditions = ScanReachableConditions(state, closure);
	closure.pendingEffectsCovered = pending.covered();
	closure.globalEffectsCovered = global.covered();
	closure.actionCostsCovered = costs.covered();
	closure.selectionContextsCovered = selection.covered();
	closure.conditionsCovered = conditions.covered();
	if (pending.unknown || global.unknown || costs.unknown || selection.unknown || conditions.unknown)
		closure.hasUnknown = true;
}

// Nested-chance safety for the CURRENT pending skill resolution path.
// Does NOT skip remaining effects by cardId — only pendingEffectIndex+1..end are scanned.
inline bool FurtherChanceUntilTurnEnd(const OperatorClosure& /*closure*/, const State& state) {
	if (ScanDeferredFunctionStack(state).impliesFurtherChance) return true;
	CoverageResult pending = ScanPendingEffects(state);
	if (pending.unknown) return true;
	if (state.exact.pendingSkillId > 0 && state.exact.pendingEffectIndex >= 0) {
		auto skill = SkillTable.find(state.exact.pendingSkillId);
		if (skill == SkillTable.end()) return true;
		const int opCard = skill->second.cardId;
		for (int i = state.exact.pendingEffectIndex + 1; i < (int)skill->second.effects.size(); ++i) {
			const Effect& effect = skill->second.effects[i];
			if (effect.isCondition) {
				if (ObservationKindForCondition(effect.conditionType) == CardObservationKind::Unknown)
					return true;
				continue;
			}
			if (EffectImpliesFurtherChanceOrZoneMove(effect.effectType)) return true;
			OperatorSourceKey source;
			source.cardId = opCard;
			source.skillId = skill->second.skillId;
			source.effectIndex = i;
			source.kind = KindForSkillOnCard(FindCardMaster(opCard), skill->second);
			OperatorFootprint fp = FootprintFromEffect(source, effect);
			if (fp.observation == CardObservationKind::Unknown) return true;
			if (fp.mayTargetDeck || fp.mayReturnHandToDeck) return true;
		}
	} else if (pending.impliesFurtherChance
		&& state.exact.pending != ExactPendingType::Draw
		&& state.exact.pending != ExactPendingType::None) {
		return true;
	}
	CoverageResult selection = ScanSelectionContexts(state);
	if (selection.unknown) return true;
	if (selection.touchesDeckIdentity) return true;
	return false;
}

// Footprint-only overload: exclude a single Effect source key (not a whole card).
inline bool FurtherChanceUntilTurnEnd(const OperatorClosure& closure,
	const OperatorSourceKey& excludeEffect = {}) {
	for (const OperatorFootprint& fp : closure.footprints) {
		if (excludeEffect.valid() && fp.source.sameEffect(excludeEffect))
			continue;
		if (fp.isCondition) continue;
		if (fp.observation == CardObservationKind::Unknown) return true;
		if (EffectImpliesFurtherChanceOrZoneMove(fp.effectType)) return true;
		if (fp.mayTargetDeck || fp.mayMoveCardZones) return true;
		if (fp.mayReturnHandToDeck || fp.mayDiscardHand) return true;
	}
	return false;
}

inline OperatorClosure BuildOperatorClosure(
	const std::unordered_set<int>& reachableCards,
	std::uint64_t partitionSchemaHash = 0) {
	OperatorClosure closure;
	closure.reachableCards = reachableCards;
	closure.partitionSchemaHash = partitionSchemaHash;
	closure.reachableOperatorHash = 1469598103934665603ULL;
	closure.reachableOperatorHash = MixHash(closure.reachableOperatorHash, LivenessSchemaVersion);
	closure.reachableOperatorHash = MixHash(closure.reachableOperatorHash, partitionSchemaHash);
	std::vector<int> sorted(reachableCards.begin(), reachableCards.end());
	std::sort(sorted.begin(), sorted.end());
	for (int cardId : sorted) {
		closure.reachableOperatorHash = MixHash(closure.reachableOperatorHash, (std::uint64_t)cardId);
		const CardMaster* master = FindCardMaster(cardId);
		if (master == nullptr) {
			OperatorFootprint unknown;
			unknown.operatorCardId = cardId;
			unknown.observation = CardObservationKind::Unknown;
			unknown.mayTargetHand = true;
			unknown.mayTargetDeck = true;
			unknown.mayDiscardHand = true;
			unknown.mayReturnHandToDeck = true;
			unknown.maySearchDeckByIdentity = true;
			unknown.mayMoveCardZones = true;
			closure.footprints.push_back(unknown);
			closure.hasUnknown = true;
			continue;
		}
		auto considerSkill = [&](const Skill* skill, OperatorSourceKind kind) {
			if (skill == nullptr) return;
			for (int i = 0; i < (int)skill->effects.size(); ++i) {
				OperatorSourceKey source;
				source.cardId = cardId;
				source.skillId = skill->skillId;
				source.effectIndex = i;
				source.kind = kind;
				OperatorFootprint fp = FootprintFromEffect(source, skill->effects[i]);
				closure.reachableEffectTypes.insert((int)skill->effects[i].effectType);
				closure.footprints.push_back(fp);
				if (fp.observation == CardObservationKind::Unknown)
					closure.hasUnknown = true;
			}
		};
		auto considerAttack = [&](const Attack* attack) {
			if (attack == nullptr) return;
			for (int i = 0; i < (int)attack->preEffects.size(); ++i) {
				OperatorSourceKey source;
				source.cardId = cardId;
				source.skillId = attack->attackId;
				source.effectIndex = i;
				source.kind = OperatorSourceKind::AttackPre;
				OperatorFootprint fp = FootprintFromEffect(source, attack->preEffects[i]);
				closure.reachableEffectTypes.insert((int)attack->preEffects[i].effectType);
				closure.footprints.push_back(fp);
				if (fp.observation == CardObservationKind::Unknown)
					closure.hasUnknown = true;
			}
			for (int i = 0; i < (int)attack->postEffects.size(); ++i) {
				OperatorSourceKey source;
				source.cardId = cardId;
				source.skillId = attack->attackId;
				source.effectIndex = i;
				source.kind = OperatorSourceKind::AttackPost;
				OperatorFootprint fp = FootprintFromEffect(source, attack->postEffects[i]);
				closure.reachableEffectTypes.insert((int)attack->postEffects[i].effectType);
				closure.footprints.push_back(fp);
				if (fp.observation == CardObservationKind::Unknown)
					closure.hasUnknown = true;
			}
		};
		considerSkill(master->play, OperatorSourceKind::Play);
		considerSkill(master->ability, OperatorSourceKind::Ability);
		considerSkill(master->delay, OperatorSourceKind::Delay);
		for (const Attack* attack : master->attacks) considerAttack(attack);
	}
	for (const auto& fp : closure.footprints)
		if (fp.observation == CardObservationKind::Unknown) closure.hasUnknown = true;
	closure.allCardOperatorsCovered = true;
	// Scanners fill the remaining coverage flags when State is available.
	if (reachableCards.empty() && !closure.hasUnknown) {
		closure.pendingEffectsCovered = true;
		closure.globalEffectsCovered = true;
		closure.actionCostsCovered = true;
		closure.selectionContextsCovered = true;
		closure.conditionsCovered = true;
	}
	return closure;
}

inline void SealCoverageForTerminalChance(OperatorClosure& /*closure*/) {
	// Intentionally empty: FurtherChanceUntilTurnEnd==false does not prove costs/conditions.
}

inline OperatorClosure BuildOperatorClosureEx(
	const std::unordered_set<int>& reachableCards,
	std::uint64_t partitionSchemaHash,
	bool pendingCovered,
	bool costsCovered) {
	OperatorClosure closure = BuildOperatorClosure(reachableCards, partitionSchemaHash);
	if (!reachableCards.empty()) {
		closure.pendingEffectsCovered = pendingCovered;
		closure.actionCostsCovered = costsCovered;
	}
	return closure;
}

inline CardLivenessResult ClassifyCardId(
	const State& state, int actor, int cardId,
	const OperatorClosure& closure,
	const PassiveMomentStateV4& moment = {}) {
	CardLivenessResult result;
	result.proof.reachableOperatorHash = closure.reachableOperatorHash;
	result.proof.partitionSchemaHash = closure.partitionSchemaHash;
	result.proof.actionIndependentResidual = true;
	result.coverage = proveCandidate(state, cardId, closure, moment);

	const CardMaster* master = FindCardMaster(cardId);
	if (master == nullptr) {
		result.liveness = CardLiveness::Unknown;
		result.reasonMask |= UnknownEffect | UnsupportedTarget | ClosureRequired;
		return result;
	}

	// Operator-reachable identities remain Active.
	if (closure.reachableCards.contains(cardId)) {
		result.liveness = CardLiveness::Active;
		result.reasonMask |= InReachableSet | MayBecomePlayable;
		result.proof.handActionInvariant = false;
		return result;
	}

	if (!result.coverage.allSafe()) {
		result.reasonMask |= CoverageIncomplete | ProofIncomplete;
		if (!result.coverage.handIdentitySafe || !result.coverage.handTypeSafe
			|| !result.coverage.handCountSafe)
			result.reasonMask |= HandTargetObserved;
		if (!result.coverage.deckRemovalSafe) result.reasonMask |= DeckIdentityObserved;
		if (!result.coverage.zoneMovementSafe) result.reasonMask |= MayMoveZone;
		if (!result.coverage.actionCostSafe)
			result.reasonMask |= MayBeDiscarded | MayBeUsedAsCost;
	}

	bool handTargetOk = result.coverage.handIdentitySafe
		&& result.coverage.handTypeSafe && result.coverage.handCountSafe
		&& result.coverage.selectionSafe && result.coverage.conditionSafe;
	bool zoneOk = result.coverage.zoneMovementSafe;
	bool deckRemovalOk = result.coverage.deckRemovalSafe;
	bool semanticOk = result.coverage.handIdentitySafe && result.coverage.deckRemovalSafe;
	bool actionCostOk = result.coverage.actionCostSafe;

	for (const OperatorFootprint& fp : closure.footprints) {
		if (!footprintMayAffectCandidate(fp, cardId)) continue;
		if (moment.built && (fp.mayDiscardHand || fp.mayReturnHandToDeck)
			&& !moment.mayActivateOperator(fp.operatorCardId))
			continue;
		if (fp.observation == CardObservationKind::Unknown) {
			handTargetOk = zoneOk = deckRemovalOk = semanticOk = actionCostOk = false;
			result.reasonMask |= UnknownEffect;
			break;
		}
		if (fp.mayTargetHand && EffectKindBlocksPassive(fp.observation)) {
			handTargetOk = false;
			result.reasonMask |= HandTargetObserved | ReferencedByReachableTarget;
		}
		if (fp.mayDiscardHand || fp.mayReturnHandToDeck || fp.mayCountHandByType
			|| fp.mayCountHandTotal) {
			handTargetOk = false;
			actionCostOk = false;
			result.reasonMask |= MayBeDiscarded | MayBeUsedAsCost | HandTargetObserved;
		}
		if (fp.mayMoveCardZones && fp.mayTargetHand) {
			zoneOk = false;
			result.reasonMask |= MayMoveZone;
		}
		if (fp.maySearchDeckByIdentity || (fp.mayTargetDeck && EffectKindBlocksPassive(fp.observation))) {
			if (!moment.built || moment.mayActivateOperator(fp.operatorCardId)) {
				deckRemovalOk = false;
				semanticOk = false;
				result.reasonMask |= DeckIdentityObserved;
			}
		}
	}

	const PlayerState& player = state.players[actor];
	bool handActionOk = true;

	if (IsEnergy(master->cardType)) {
		if (!(state.energyPlayed
			|| (player.thisTurn.cannotPlaySpecialEnergy
				&& master->cardType == CardType::SpecialEnergy))) {
			handActionOk = false;
			result.reasonMask |= CurrentlyPlayable | MayBeAttached;
		} else {
			result.reasonMask |= EnergyOnceConsumed;
		}
	} else if (master->cardType == CardType::Supporter) {
		if (!(state.supporterPlayed || player.thisTurn.cannotPlaySupporter
			|| (state.turn <= 1 && !master->canPlayFirstTurn))) {
			handActionOk = false;
			result.reasonMask |= CurrentlyPlayable | MayTriggerEffect;
		} else {
			result.reasonMask |= SupporterLocked;
		}
	} else if (master->cardType == CardType::Stadium) {
		if (!(state.stadiumPlayed || player.cannotPlayStadium || player.thisTurn.cannotPlayStadium)) {
			handActionOk = false;
			result.reasonMask |= CurrentlyPlayable;
		} else {
			result.reasonMask |= StadiumLocked;
		}
	} else if (master->cardType == CardType::Pokemon) {
		if (master->evolutionType == EvolutionType::Basic) {
			handActionOk = false;
			result.reasonMask |= CurrentlyPlayable | MayChangeLegality;
		} else if (state.turn <= 2) {
			result.reasonMask |= EvolutionBlocked;
		} else {
			handActionOk = false;
			result.reasonMask |= MayBecomePlayable | MayBeEvolved;
		}
	} else {
		if (!(player.thisTurn.cannotPlayItem || player.cannotPlayItem)) {
			bool turnLocked = false;
			if (master->play != nullptr) {
				for (const Effect& effect : master->play->effects) {
					if (!effect.isCondition) break;
					if (effect.conditionType != ConditionType::Turn) continue;
					const int need = effect.values[0];
					if (effect.comparatorType == ComparatorType::GreaterEqual && state.turn < need) turnLocked = true;
					if (effect.comparatorType == ComparatorType::Greater && state.turn <= need) turnLocked = true;
					if (effect.comparatorType == ComparatorType::Equal && state.turn != need) turnLocked = true;
				}
			}
			if (!turnLocked) {
				handActionOk = false;
				result.reasonMask |= CurrentlyPlayable | MayTriggerEffect | MayMoveZone;
			}
		}
	}

	if (!actionCostOk) handTargetOk = false;

	result.proof.handActionInvariant = handActionOk;
	result.proof.handTargetInvariant = handTargetOk;
	result.proof.zoneMovementInvariant = zoneOk;
	result.proof.deckRemovalInvariant = deckRemovalOk;
	result.proof.semanticInvariant = semanticOk && result.coverage.allSafe();
	result.proof.actionIndependentResidual = true;

	std::uint64_t proofHash = 1469598103934665603ULL;
	proofHash = MixHash(proofHash, LivenessSchemaVersion);
	proofHash = MixHash(proofHash, (std::uint64_t)cardId);
	proofHash = MixHash(proofHash, closure.reachableOperatorHash);
	proofHash = MixHash(proofHash, handActionOk ? 1ull : 0ull);
	proofHash = MixHash(proofHash, handTargetOk ? 1ull : 0ull);
	proofHash = MixHash(proofHash, zoneOk ? 1ull : 0ull);
	proofHash = MixHash(proofHash, deckRemovalOk ? 1ull : 0ull);
	proofHash = MixHash(proofHash, semanticOk ? 1ull : 0ull);
	proofHash = MixHash(proofHash, result.coverage.allSafe() ? 1ull : 0ull);
	result.proof.proofHash = proofHash;

	if (result.proof.allProven() && result.coverage.allSafe()) {
		result.liveness = CardLiveness::Passive;
		result.reasonMask |= PassiveProven;
		return result;
	}
	if (!handActionOk || !handTargetOk || !zoneOk || !deckRemovalOk || !semanticOk
		|| !result.coverage.allSafe()) {
		result.liveness = CardLiveness::Active;
		result.reasonMask |= ProofIncomplete;
		return result;
	}
	result.liveness = CardLiveness::Unknown;
	result.reasonMask |= ProofIncomplete;
	return result;
}

struct HandSplit {
	std::vector<std::pair<int, int>> activeCounts;
	std::vector<std::pair<int, int>> passiveCounts;
	int unknownCount = 0;
	std::uint64_t proofHash = 0;
};

inline HandSplit SplitHandCounts(const State& state, int actor,
	const std::unordered_map<int, int>& handCounts,
	const OperatorClosure& closure,
	const PassiveMomentStateV4& moment = {}) {
	HandSplit split;
	std::uint64_t hash = 1469598103934665603ULL;
	hash = MixHash(hash, LivenessSchemaVersion);
	hash = MixHash(hash, closure.reachableOperatorHash);
	for (const auto& item : handCounts) {
		if (item.second <= 0) continue;
		CardLivenessResult classified = ClassifyCardId(state, actor, item.first, closure, moment);
		CardLiveness live = classified.liveness;
		if (live == CardLiveness::Unknown) {
			live = CardLiveness::Active;
			++split.unknownCount;
		}
		hash = MixHash(hash, (std::uint64_t)item.first);
		hash = MixHash(hash, (std::uint64_t)item.second);
		hash = MixHash(hash, (std::uint64_t)live);
		hash = MixHash(hash, classified.reasonMask);
		hash = MixHash(hash, classified.proof.proofHash);
		if (live == CardLiveness::Passive)
			split.passiveCounts.push_back(item);
		else
			split.activeCounts.push_back(item);
	}
	std::sort(split.activeCounts.begin(), split.activeCounts.end());
	std::sort(split.passiveCounts.begin(), split.passiveCounts.end());
	split.proofHash = hash;
	return split;
}

// True if any moment-playable (or in-play) operator can open another Draw / zone
// chance later this turn. Deck-only search Items are excluded — Active enumeration
// covers draws of those identities.
inline bool AnyMomentFurtherChance(const OperatorClosure& closure, const State& state,
	const PassiveMomentStateV4& moment,
	const OperatorSourceKey& excludeEffect = {}) {
	if (ScanDeferredFunctionStack(state).impliesFurtherChance) return true;
	for (const OperatorFootprint& fp : closure.footprints) {
		if (fp.isCondition) continue;
		if (excludeEffect.valid() && fp.source.sameEffect(excludeEffect))
			continue;
		if (!EffectImpliesFurtherChanceOrZoneMove(fp.effectType)) continue;
		if (moment.built && !moment.mayActivateOperator(fp.operatorCardId))
			continue;
		return true;
	}
	return false;
}

inline bool AnyReachableFurtherChance(const OperatorClosure& closure,
	const OperatorSourceKey& excludeEffect = {}) {
	for (const OperatorFootprint& fp : closure.footprints) {
		if (fp.isCondition) continue;
		if (excludeEffect.valid() && fp.source.sameEffect(excludeEffect))
			continue;
		if (EffectImpliesFurtherChanceOrZoneMove(fp.effectType)) return true;
	}
	return false;
}

inline bool AnyReachableFurtherChance(const OperatorClosure& closure, const State& state,
	const OperatorSourceKey& excludeEffect = {}) {
	if (ScanDeferredFunctionStack(state).impliesFurtherChance) return true;
	return AnyReachableFurtherChance(closure, excludeEffect);
}

inline const char* LivenessName(CardLiveness live) {
	switch (live) {
	case CardLiveness::Active: return "Active";
	case CardLiveness::Passive: return "Passive";
	default: return "Unknown";
	}
}

} // namespace ExactCardLivenessV4
