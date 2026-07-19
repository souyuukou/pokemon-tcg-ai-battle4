#pragma once

// Exact search state canonicalisation.  The engine's wire serialisation is
// intentionally physical: it contains CardRef indices, absolute move counters
// and container order.  Those are useful to replay a game, but are not a sound
// transposition key because two commuting action sequences can allocate the
// same logical cards differently.  This builder keeps a reference-free scalar
// image of State and appends a semantic graph of all card-bearing fields.

#include "State.h"

#include <map>
#include <set>

class ExactCanonicalState {
public:
	static std::string Build(const State& state) {
		ExactCanonicalState builder(state);
		return compressZeros(builder.build());
	}
	static std::string Compress(const std::string& bytes) { return compressZeros(bytes); }

private:
	const State& state;
	std::map<int, int> skillRank;
	std::map<int, CardRef> moveTarget;
	mutable std::array<std::string, 128> cardCache;
	mutable std::array<bool, 128> cardCached = {};

	explicit ExactCanonicalState(const State& input) : state(input) {
		std::set<int> orders;
		for (const Card& card : state.allCard)
			if (card.cardId != 0 && card.skillOrder > 0) orders.insert(card.skillOrder);
		for (int i = 1; i < (int)state.allCard.size(); ++i)
			if (state.allCard[i].cardId != 0) moveTarget.emplace(state.allCard[i].moveCounter, CardRef(i));
		int rank = 1;
		for (int order : orders) skillRank.emplace(order, rank++);
	}

	static void appendInt(std::string& out, long long value) {
		out += std::to_string(value);
		out.push_back(';');
	}

	static void appendBlob(std::string& out, const std::string& blob) {
		appendInt(out, (long long)blob.size());
		out.append(blob);
	}

	// Lossless zero-run encoding keeps full-byte collision checks while avoiding
	// hashing/copying the cleared allCard padding in every main-state key.
	static std::string compressZeros(const std::string& input) {
		std::string out;
		out.reserve(input.size() / 2);
		for (size_t i = 0; i < input.size();) {
			unsigned char byte = (unsigned char)input[i];
			if (byte != 0 && byte != 0xff) { out.push_back((char)byte); ++i; continue; }
			if (byte == 0xff) { out.push_back((char)0xff); out.push_back((char)0); ++i; continue; }
			size_t end = i + 1;
			while (end < input.size() && input[end] == 0 && end - i < 255) ++end;
			out.push_back((char)0xff); out.push_back((char)(end - i)); i = end;
		}
		return out;
	}

	static AreaRef nullAreaRef() { return { CardRef(0), 0 }; }

	std::string rawBytes(const void* data, size_t size) const {
		return std::string((const char*)data, size);
	}

	std::string shallowCard(CardRef ref) const {
		if (ref.isNull()) return "N";
		const Card& source = state.getCard(ref);
		std::string token = "C";
		appendInt(token, source.cardId);
		appendInt(token, source.attachMoveCounter == 0 ? 0 : 1);
		appendInt(token, source.skillOrder > 0 && skillRank.contains(source.skillOrder)
			? skillRank.at(source.skillOrder) : 0);
		appendInt(token, source.damage);
		appendBlob(token, rawBytes(&source.thisTurn.value, sizeof(source.thisTurn.value)));
		appendBlob(token, rawBytes(&source.nextTurn.value, sizeof(source.nextTurn.value)));
		appendBlob(token, rawBytes(&source.thisTurnEnemy.value, sizeof(source.thisTurnEnemy.value)));
		appendBlob(token, rawBytes(&source.nextTurnEnemy.value, sizeof(source.nextTurnEnemy.value)));
		appendInt(token, source.takeAttackDamageThisTurn); appendInt(token, source.takeAttackDamagePreTurn);
		appendInt(token, source.playerIndex); appendInt(token, (int)source.area); appendInt(token, (int)source.preArea);
		appendInt(token, source.reverse ? 1 : 0); appendInt(token, source.cannotUseAttackIdNonActive);
		std::vector<short> used(source.abilityUsed.begin(), source.abilityUsed.end());
		std::sort(used.begin(), used.end());
		appendBlob(token, rawBytes(used.data(), used.size() * sizeof(short)));
		appendInt(token, source.nextEnemyTurnEndStateBattleField); appendInt(token, source.nextEnemyTurnEndState);
		Card turnCopy = source;
		unsigned char cause = turnCopy.koCauseRef;
		turnCopy.koCauseRef = 0;
		appendBlob(token, rawBytes(turnCopy.turnState.data(), sizeof(turnCopy.turnState)));
		appendBlob(token, rawBytes(source.continualState.data(), sizeof(source.continualState)));
		if (cause != 0 && cause < state.allCard.size() && state.allCard[cause].cardId != 0) {
			const Card& causeCard = state.allCard[cause];
			appendInt(token, causeCard.cardId);
			appendInt(token, causeCard.playerIndex);
			appendInt(token, (int)causeCard.area);
		} else {
			appendInt(token, 0);
		}
		return token;
	}

	std::string cardToken(CardRef ref) const {
		if (!ref.isNull() && cardCached[ref.cardIndex]) return cardCache[ref.cardIndex];
		std::string token = shallowCard(ref);
		if (ref.isNull()) return token;
		const Card& card = state.getCard(ref);
		CardRef target(0);
		if (card.attachMoveCounter != 0) {
			auto found = moveTarget.find(card.attachMoveCounter);
			if (found != moveTarget.end()) target = found->second;
		}
		appendBlob(token, shallowCard(target));
		cardCached[ref.cardIndex] = true;
		cardCache[ref.cardIndex] = token;
		return cardCache[ref.cardIndex];
	}

	std::string areaToken(const AreaRef& ref) const {
		std::string token;
		if (ref.card.isNull()) return "N";
		appendInt(token, state.getCard(ref.card).moveCounter == ref.moveCounter ? 1 : 0);
		appendBlob(token, cardToken(ref.card));
		return token;
	}

	template<class List>
	void appendCardList(std::string& out, const List& list, bool unordered) const {
		std::vector<std::string> tokens;
		tokens.reserve(list.size());
		for (CardRef ref : list) tokens.push_back(cardToken(ref));
		if (unordered) std::sort(tokens.begin(), tokens.end());
		appendInt(out, (long long)tokens.size());
		for (const std::string& token : tokens) appendBlob(out, token);
	}

	void appendAreaList(std::string& out, const std::vector<AreaRef>& list, bool unordered) const {
		std::vector<std::string> tokens;
		tokens.reserve(list.size());
		for (const AreaRef& ref : list) tokens.push_back(areaToken(ref));
		if (unordered) std::sort(tokens.begin(), tokens.end());
		appendInt(out, (long long)tokens.size());
		for (const std::string& token : tokens) appendBlob(out, token);
	}

	std::string triggerToken(const TriggeredAbility& source) const {
		std::string token;
		appendInt(token, source.activateInfo.skillId); appendInt(token, source.activateInfo.usePlayerIndex);
		appendInt(token, source.activateInfo.isEffectStack ? 1 : 0); appendInt(token, source.activateInfo.effectStackIndex);
		appendInt(token, source.activateInfo.isSpecialCondition ? 1 : 0);
		appendInt(token, (int)source.trigger.type); appendInt(token, source.trigger.depth); appendInt(token, source.trigger.value);
		appendBlob(token, areaToken(source.activateInfo.effectCard));
		appendBlob(token, areaToken(source.trigger.subject));
		appendBlob(token, areaToken(source.trigger.object));
		return token;
	}

	void appendTriggerStack(std::string& out, const std::vector<TriggeredAbility>& stack) const {
		appendInt(out, (long long)stack.size());
		for (const TriggeredAbility& trigger : stack) appendBlob(out, triggerToken(trigger));
	}

	std::string optionToken(const SelectOption& option) const {
		std::string token;
		appendInt(token, (int)option.type);
		auto appendPosition = [&](AreaType area, int index, int player) {
			appendInt(token, (int)area);
			appendInt(token, player);
			try { appendBlob(token, cardToken(state.getCardRef(area, index, player))); }
			catch (...) { appendInt(token, index); }
		};
		switch (option.type) {
		case SelectOptionType::Card:
		case SelectOptionType::ToolCard:
		case SelectOptionType::EnergyCard:
		case SelectOptionType::Energy:
			appendPosition((AreaType)option.param0, option.param1, option.param2);
			appendInt(token, option.param3); appendInt(token, option.param4); break;
		case SelectOptionType::Play:
			appendPosition(AreaType::Hand, option.param0, state.selectPlayer); break;
		case SelectOptionType::Attach:
		case SelectOptionType::Evolve:
			appendPosition((AreaType)option.param0, option.param1, state.selectPlayer);
			appendPosition((AreaType)option.param2, option.param3, state.selectPlayer); break;
		case SelectOptionType::Ability:
		case SelectOptionType::Discard:
			appendPosition((AreaType)option.param0, option.param1, state.selectPlayer); break;
		case SelectOptionType::Skill:
			appendInt(token, option.param0); break;
		default:
			appendInt(token, option.param0); appendInt(token, option.param1);
			appendInt(token, option.param2); appendInt(token, option.param3);
			appendInt(token, option.param4); break;
		}
		return token;
	}

	static void clearCardList(auto& list) { list.allClear(); }

	std::string build() const {
		State scalar = state;
		scalar.logs.clear(); scalar.logIndex = {}; scalar.selected.clear();
		scalar.contextCard = CardRef(0);
		scalar.selectingEnergyPokemonRef = CardRef(0);
		scalar.attacker = CardRef(0);
		scalar.effectState.ability.effectCard = nullAreaRef();
		scalar.triggerInfo.subject = nullAreaRef();
		scalar.triggerInfo.object = nullAreaRef();
		for (TurnHistory& history : scalar.turnHistories) history.turnAttackCard = CardRef(0);
		scalar.moveCounter = 0;
		scalar.currentSkillOrder = (int)skillRank.size() + 1;
		scalar.reserved = 0;
		clearCardList(scalar.stadium); clearCardList(scalar.looking);
		clearCardList(scalar.selectedList); clearCardList(scalar.eachList);
		clearCardList(scalar.playing); clearCardList(scalar.checkList);
		for (PlayerState& player : scalar.players) {
			clearCardList(player.active); clearCardList(player.bench); clearCardList(player.prize);
			clearCardList(player.hand); clearCardList(player.deck); clearCardList(player.trash);
			clearCardList(player.energy); clearCardList(player.tool); clearCardList(player.preEvolution);
			clearCardList(player.temporary); player.reserved = 0;
		}
		scalar.allCard = {};
		scalar.options.clear(); scalar.preTargetList.clear(); scalar.targetList.clear(); scalar.koList.clear();
		scalar.delayTriggerStack.clear(); scalar.temporaryTriggerStack.clear(); scalar.triggerStack.clear();
		scalar.turnUsedSkill.clear(); scalar.turnPlay.clear(); scalar.turnHeal.clear(); scalar.turnEvolve.clear();

		// Hidden count-vector order is not semantic.
		for (int player = 0; player < 2; ++player) {
			std::vector<std::pair<int, unsigned char>> counts;
			for (int i = 0; i < scalar.exact.typeCount[player]; ++i)
				counts.push_back({ scalar.exact.cardId[player][i], scalar.exact.cardCount[player][i] });
			std::sort(counts.begin(), counts.end());
			scalar.exact.cardId[player] = {};
			scalar.exact.cardCount[player] = {};
			for (int i = 0; i < (int)counts.size(); ++i) {
				scalar.exact.cardId[player][i] = counts[i].first;
				scalar.exact.cardCount[player][i] = counts[i].second;
			}
		}

		BinaryWriter writer;
		auto pod = [&](const auto& value) { writer.set(&value, sizeof(value)); };
		pod(scalar.turn); pod(scalar.turnActionCount); pod(scalar.effectActionCount); pod(scalar.turnAttackCount);
		pod(scalar.phase); pod(scalar.gameResult); pod(scalar.finishReason);
		pod(scalar.setupDone); pod(scalar.mulligan); pod(scalar.mulliganCount); pod(scalar.firstPlayer);
		pod(scalar.lookingPlayer); pod(scalar.lookingReverse); pod(scalar.isBreak); pod(scalar.effectLoopStop);
		pod(scalar.changed); pod(scalar.stateChanged); pod(scalar.updateOrder); pod(scalar.turnState);
		pod(scalar.continualState); pod(scalar.currentCardEffectIndex); pod(scalar.coinHeadCount);
		pod(scalar.lastStadiumPlayer); pod(scalar.failRetreat); pod(scalar.selectType); pod(scalar.selectContext);
		pod(scalar.selectDeck); pod(scalar.selectPlayer); pod(scalar.selectMin); pod(scalar.selectMax);
		pod(scalar.remainDamageCounter); pod(scalar.energyCost); pod(scalar.remainEnergyCost);
		pod(scalar.selectedEnergyCardCount); pod(scalar.removedDamageCounter); pod(scalar.selectCounts);
		pod(scalar.effectState.ability.skillId); pod(scalar.effectState.ability.usePlayerIndex);
		pod(scalar.effectState.ability.isEffectStack); pod(scalar.effectState.ability.effectStackIndex);
		pod(scalar.effectState.ability.isSpecialCondition); pod(scalar.effectState.effectIndex);
		pod(scalar.effectState.onEffect); pod(scalar.effectState.selectedListIndex); pod(scalar.effectState.eachListIndex);
		pod(scalar.effectState.effectRate); pod(scalar.effectState.damageChange);
		pod(scalar.triggerInfo.type); pod(scalar.triggerInfo.depth); pod(scalar.triggerInfo.value);
		pod(scalar.effectJump); pod(scalar.attachActive); pod(scalar.currentAttackId); pod(scalar.srcAttackId);
		pod(scalar.attackDamageChange); pod(scalar.lastAttackDamage); pod(scalar.postAttackEffect);
		pod(scalar.postEffectActivate); pod(scalar.failAttack); pod(scalar.secondAttack); pod(scalar.currentSkillOrder);
		const ExactHiddenState& exact = scalar.exact;
		pod(exact.enabled); pod(exact.actor); pod(exact.deckUnknown); pod(exact.deckExchangeable);
		pod(exact.prizeExchangeable); pod(exact.pending); pod(exact.pendingPlayer); pod(exact.pendingCount);
		pod(exact.pendingDetail); pod(exact.pendingIntent); pod(exact.blockReason); pod(exact.pendingEffectCardId);
		pod(exact.pendingEffectPlayer); pod(exact.pendingSkillId); pod(exact.pendingEffectIndex);
		pod(exact.pendingNullCount); pod(exact.typeCount);
		pod(exact.cardId); pod(exact.cardCount); pod(exact.profileKnown); pod(exact.provisionalOpponentPolicy);
		for (const TurnHistory& history : scalar.turnHistories) {
			pod(history.ko); pod(history.koTeamRocket); pod(history.koAttackDamage);
			pod(history.koAttackDamageEthan); pod(history.koAttackDamageHop);
			pod(history.takePrizeCountTurnPlayer); pod(history.turnAttackId);
		}
		for (const PlayerState& player : scalar.players) {
			pod(player.playerIndex); pod(player.koPrizeOnceChanged); pod(player.thisTurn.value); pod(player.nextTurn.value);
			pod(player.activeState); pod(player.continualState); pod(player.turnState);
		}
		// Deferred frames are an ABI-level int tuple, but the tuple's meaning is
		// part of the state identity.  Do not put raw CardRef indices into the
		// binary scalar image: those indices are allocation artifacts and would
		// prevent equivalent states from sharing a DAG node.  The semantic blob
		// below converts CardReference arguments through the same card token used
		// by the rest of this canonicalizer.
		std::string deferredFunctions;
		for (const GameFunction& function : scalar.functionStack) {
			pod(function.functionIndex);
			pod(function.argType); pod(function.callCount); pod(function.calledCount);
			const DeferredArgSemantics semantics = DeferredSemanticsFor(function);
			const int values[3] = { function.arg0, function.arg1, function.arg2 };
			const int argumentCount = function.argType == ArgType::None ? 0
				: (function.argType == ArgType::I || function.argType == ArgType::B ? 1
					: (function.argType == ArgType::II ? 2 : 3));
			appendInt(deferredFunctions, argumentCount);
			for (int i = 0; i < argumentCount; ++i) {
				appendInt(deferredFunctions, (int)semantics[(size_t)i]);
				if (semantics[(size_t)i] == DeferredArgSemantic::CardReference
					&& values[i] > 0 && values[i] < (int)state.allCard.size()) {
					appendBlob(deferredFunctions, shallowCard(CardRef(values[i])));
				} else {
					appendInt(deferredFunctions, values[i]);
				}
			}
		}
		std::string result((const char*)writer.buf.data(), writer.buf.size());
		appendBlob(result, deferredFunctions);

		appendBlob(result, cardToken(state.contextCard));
		appendBlob(result, cardToken(state.selectingEnergyPokemonRef));
		appendBlob(result, cardToken(state.attacker));
		appendBlob(result, areaToken(state.effectState.ability.effectCard));
		appendBlob(result, areaToken(state.triggerInfo.subject));
		appendBlob(result, areaToken(state.triggerInfo.object));
		for (const TurnHistory& history : state.turnHistories) appendBlob(result, cardToken(history.turnAttackCard));

		appendCardList(result, state.stadium, true);
		appendCardList(result, state.looking, false);
		appendCardList(result, state.selectedList, false);
		appendCardList(result, state.eachList, false);
		appendCardList(result, state.playing, false);
		appendCardList(result, state.checkList, false);
		for (int player = 0; player < 2; ++player) {
			const PlayerState& ps = state.players[player];
			appendCardList(result, ps.active, false);
			appendCardList(result, ps.bench, true);
			appendCardList(result, ps.prize, state.exact.prizeExchangeable[player]);
			appendCardList(result, ps.hand, true);
			appendCardList(result, ps.deck, state.exact.deckExchangeable[player]);
			appendCardList(result, ps.trash, true);
			appendCardList(result, ps.energy, true);
			appendCardList(result, ps.tool, true);
			appendCardList(result, ps.preEvolution, true);
			appendCardList(result, ps.temporary, false);
		}

		std::vector<std::string> options;
		for (const SelectOption& option : state.options) options.push_back(optionToken(option));
		std::sort(options.begin(), options.end());
		appendInt(result, (long long)options.size());
		for (const std::string& option : options) appendBlob(result, option);
		appendAreaList(result, state.preTargetList, false);
		appendAreaList(result, state.targetList, false);
		appendAreaList(result, state.koList, false);
		appendTriggerStack(result, state.delayTriggerStack);
		appendTriggerStack(result, state.temporaryTriggerStack);
		appendTriggerStack(result, state.triggerStack);

		auto usedSkills = state.turnUsedSkill;
		std::sort(usedSkills.begin(), usedSkills.end());
		appendBlob(result, rawBytes(usedSkills.data(), usedSkills.size() * sizeof(int)));
		appendCardList(result, state.turnPlay, true);
		appendCardList(result, state.turnHeal, true);
		std::vector<std::string> evolves;
		for (const EvolveInfo& evolve : state.turnEvolve) {
			std::string token;
			appendBlob(token, cardToken(evolve.preRef)); appendBlob(token, cardToken(evolve.ref));
			evolves.push_back(std::move(token));
		}
		std::sort(evolves.begin(), evolves.end());
		for (const std::string& evolve : evolves) appendBlob(result, evolve);
		return result;
	}
};
