// SPDX-FileCopyrightText: © Pokémon/Nintendo/Creatures/GAME FREAK TM, ®, and character names are trademarks of Nintendo.
// SPDX-License-Identifier: LicenseRef-PTCG-ABC-Competition-Use-Only
#pragma once

#include "../../boundary_ai/include/boundary_ai/boundary_ai.hpp"
#include "State.h"

#include <sstream>

namespace ptcg_boundary {

struct Action {
	std::vector<int> selected;
	SelectOptionType primaryType = SelectOptionType::Number;
};

struct Position {
	Game game;
	State state;
	int rootPlayer = 0;
	int rootTurn = 0;

	explicit Position(const State& source)
		: game(*source.game), state(source),
		  rootPlayer(source.activePlayerIndex()), rootTurn(source.turn) {
		prepare();
	}

	Position(const Position& source)
		: game(source.game), state(source.state),
		  rootPlayer(source.rootPlayer), rootTurn(source.rootTurn) {
		prepare();
	}

	Position(Position&& source) noexcept
		: game(std::move(source.game)), state(std::move(source.state)),
		  rootPlayer(source.rootPlayer), rootTurn(source.rootTurn) {
		prepare();
	}

	Position& operator=(const Position& source) {
		if (this == &source) return *this;
		game = source.game;
		state = source.state;
		rootPlayer = source.rootPlayer;
		rootTurn = source.rootTurn;
		prepare();
		return *this;
	}

	Position& operator=(Position&& source) noexcept {
		if (this == &source) return *this;
		game = std::move(source.game);
		state = std::move(source.state);
		rootPlayer = source.rootPlayer;
		rootTurn = source.rootTurn;
		prepare();
		return *this;
	}

private:
	void prepare() {
		state.game = &game;
		game.config.manualCoin = true;
		game.config.deviceRand = false;
		game.config.recordLog = true; // shuffle detection uses the log
	}
};

// Phase-1 adapter: exact for completely known, ordered decks and manual coin
// branches. A card effect that shuffles is rejected instead of silently
// turning RNG output into an exact branch.
class FullyObservedDomain final
	: public boundary_ai::SearchDomain<Position, Action> {
public:
	[[nodiscard]] boundary_ai::InformationState canonicalKey(
		const Position& position) const override {
		boundary_ai::InformationState key;
		key.board = intern(position);
		const PlayerState& player = position.state.players[position.rootPlayer];
		for (CardRef ref : player.hand) {
			key.ownHand.add(static_cast<boundary_ai::CardTypeId>(
				position.state.getCardId(ref)), 1);
		}
		key.resources.supporterAvailable = !position.state.supporterPlayed;
		key.resources.energyAttachmentAvailable = !position.state.energyPlayed;
		key.resources.retreatAvailable = !position.state.retreated;
		key.resources.stadiumAvailable = !position.state.stadiumPlayed;
		key.resources.openBenchSlots = static_cast<std::uint8_t>(
			std::max(0, position.state.remainingBench(position.rootPlayer)));
		key.observation.observingPlayer =
			static_cast<std::uint8_t>(position.state.selectPlayer);
		key.pendingEffect = pendingEffectFingerprint(position.state);
		return key;
	}

	[[nodiscard]] boundary_ai::Expansion<Position, Action> expand(
		const Position& position) const override {
		if (isBoundary(position)) return boundary_ai::BoundaryExpansion{};
		const State& state = position.state;
		if (state.isFinish()) return boundary_ai::BoundaryExpansion{};
		if (state.selectType == SelectType::None) {
			throw std::logic_error("PTCG state is not stopped at a selection");
		}

		if (state.selectContext == SelectContext::CoinHead) {
			if (state.options.size() != 2 || state.selectMin != 1 || state.selectMax != 1) {
				throw std::logic_error("unexpected coin selection shape");
			}
			boundary_ai::ChanceExpansion<Position> chance;
			for (int option = 0; option < 2; ++option) {
				chance.branches.push_back({
					static_cast<std::uint64_t>(state.options[option].type),
					boundary_ai::Rational(1, 2),
					apply(position, {option}),
					0.0
				});
			}
			return chance;
		}

		boundary_ai::DecisionExpansion<Position, Action> decision;
		for (auto selection : selections(state)) {
			const SelectOptionType type = selection.empty()
				? SelectOptionType::Number
				: state.options.at(selection.front()).type;
			decision.branches.push_back({
				Action{selection, type},
				apply(position, selection),
				policyPriority(type)
			});
		}
		return decision;
	}

private:
	mutable std::vector<std::vector<std::uint8_t>> boardStates_;
	mutable std::unordered_multimap<std::size_t, std::uint64_t> boardHashes_;

	static bool isBoundary(const Position& position) {
		const State& state = position.state;
		if (state.isFinish()) return true;
		return state.turn > position.rootTurn &&
			state.phase == GamePhase::Main &&
			state.activePlayerIndex() != position.rootPlayer &&
			state.selectType != SelectType::None;
	}

	static std::uint32_t pendingEffectFingerprint(const State& state) {
		std::uint32_t value = static_cast<std::uint32_t>(state.selectType);
		value = value * 131u + static_cast<std::uint32_t>(state.selectContext);
		value = value * 131u + static_cast<std::uint32_t>(state.effectState.ability.skillId);
		value = value * 131u + static_cast<std::uint32_t>(state.currentAttackId);
		return value;
	}

	static double policyPriority(SelectOptionType type) {
		switch (type) {
		case SelectOptionType::Attack: return 4.0;
		case SelectOptionType::Play: return 3.0;
		case SelectOptionType::Ability: return 2.5;
		case SelectOptionType::Attach:
		case SelectOptionType::Evolve: return 2.0;
		case SelectOptionType::End: return -1.0;
		default: return 0.0;
		}
	}

	static std::vector<std::vector<int>> selections(const State& state) {
		std::vector<std::vector<int>> output;
		std::vector<int> current;
		const int optionCount = static_cast<int>(state.options.size());
		std::function<void(int, int)> choose = [&](int start, int target) {
			if (static_cast<int>(current.size()) == target) {
				output.push_back(current);
				return;
			}
			const int needed = target - static_cast<int>(current.size());
			for (int i = start; i <= optionCount - needed; ++i) {
				current.push_back(i);
				choose(i + 1, target);
				current.pop_back();
			}
		};
		for (int count = state.selectMin; count <= state.selectMax; ++count) {
			choose(0, count);
		}
		if (output.empty()) throw std::logic_error("selection has no legal combination");
		return output;
	}

	static Position apply(const Position& source, const std::vector<int>& selected) {
		Position next(source);
		const auto logStart = next.state.logs.size();
		next.state.selected = selected;
		const int error = next.state.checkPlayerSelect();
		if (error != 0) throw std::logic_error("generated an illegal selection");
		next.state.step();
		while (!next.state.isFinish() && next.state.selectMax == 0) {
			next.state.selected.clear();
			next.state.step();
		}
		for (std::size_t i = logStart; i < next.state.logs.size(); ++i) {
			if (next.state.logs[i].logType == LogType::Shuffle) {
				throw std::logic_error(
					"symbolic shuffle required: use the information-set adapter");
			}
		}
		return next;
	}

	std::uint64_t intern(const Position& position) const {
		State canonical = position.state;
		canonical.logs.clear();
		canonical.logIndex = {};
		BinaryWriter writer;
		canonical.serialize(writer);
		std::ostringstream rng;
		rng << position.game.rng;
		const std::string rngState = rng.str();
		writer.set(rngState.data(), rngState.size());

		std::size_t hash = 1469598103934665603ull;
		for (const auto byte : writer.buf) {
			hash ^= byte;
			hash *= 1099511628211ull;
		}
		const auto range = boardHashes_.equal_range(hash);
		for (auto it = range.first; it != range.second; ++it) {
			if (boardStates_[it->second] == writer.buf) return it->second;
		}
		const auto id = static_cast<std::uint64_t>(boardStates_.size());
		boardStates_.push_back(std::move(writer.buf));
		boardHashes_.emplace(hash, id);
		return id;
	}
};

class VisibleBoundaryValue final
	: public boundary_ai::BoundaryEvaluator<Position> {
public:
	[[nodiscard]] boundary_ai::Rational evaluateBoundary(
		const Position& position) const override {
		const State& state = position.state;
		if (state.isFinish()) {
			if (state.winPlayer() == position.rootPlayer) return boundary_ai::Rational(1);
			if (state.winPlayer() == 1 - position.rootPlayer) return boundary_ai::Rational(-1);
			return boundary_ai::Rational(0);
		}
		const auto& me = state.players[position.rootPlayer];
		const auto& enemy = state.players[1 - position.rootPlayer];
		const int prizeAdvantage =
			static_cast<int>(enemy.prize.size()) - static_cast<int>(me.prize.size());
		const int handAdvantage =
			static_cast<int>(me.hand.size()) - static_cast<int>(enemy.hand.size());
		int score = prizeAdvantage * 12 + handAdvantage;
		score = std::clamp(score, -100, 100);
		return boundary_ai::Rational(score, 100);
	}
};

} // namespace ptcg_boundary
