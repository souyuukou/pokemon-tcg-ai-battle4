// Exact-search boundaries and audited symmetry predicates.
#pragma once

#include "GameProc.h"

// Leaf is after end-of-turn effects and Pokémon Checkup, but before the next
// player's TurnStart/draw.  This predicate does not mutate State.
inline bool IsExactTurnLeaf(const State& state) {
	// PokemonCheckupEnd is assigned only after all checkup/turn-end effects
	// finish and is replaced at the start of TurnStart.  Depending on whether
	// a caller pauses inside State::step, the just-completed stack frame may
	// still be present, so the function-stack shape is not a stable boundary.
	return state.phase == GamePhase::PokemonCheckupEnd;
}

// Returns whether physical prize indices are semantically exchangeable at the
// current selection. The exact solver still has to branch on hidden identity
// and aggregate its integer weight; this only removes meaningless decisions.
inline bool CanQuotientFacedownPrizeSelection(const State& state) {
	if (state.selectType != SelectType::Card
		|| state.selectContext != SelectContext::ToHand
		|| state.selectMin != state.selectMax) {
		return false;
	}
	const PlayerState& ps = state.players.at(state.selectPlayer);
	if (state.options.size() != ps.prize.size()) {
		return false;
	}
	for (CardRef ref : ps.prize) {
		const Card& card = state.getCard(ref);
		if (!card.reverse) {
			return false;
		}
		const Skill* ability = state.getAbility(card, card.getMaster());
		if (ability != nullptr && ability->luckyBonus) {
			return false;
		}
	}
	return true;
}

inline std::vector<int> ExactPrizeRepresentative(const State& state) {
	std::vector<int> selected;
	if (!CanQuotientFacedownPrizeSelection(state)) {
		return selected;
	}
	for (int i = 0; i < state.selectMax; ++i) {
		selected.push_back(i);
	}
	return selected;
}

