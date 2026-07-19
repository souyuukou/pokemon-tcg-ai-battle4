// SPDX-FileCopyrightText: © Pokémon/Nintendo/Creatures/GAME FREAK TM, ®, and character names are trademarks of Nintendo.
// SPDX-License-Identifier: LicenseRef-PTCG-ABC-Competition-Use-Only
// Part of the Pokémon TCG AI Battle Challenge. Provided for Competition use only;
// the full license is in the LICENSES/ folder and incorporates the Competition Rules.
// Competition Rules: https://www.kaggle.com/competitions/pokemon-tcg-ai-battle/rules

#pragma once

#include "Card.h"
#include "Skill.h"

#include <algorithm>
#include <map>
#include <set>
#include <string>
#include <type_traits>
#include <vector>

// A reversible partition of a card-count population.  Search keys and chance
// arithmetic may use class totals, while atoms are retained so a later
// operator can split a class without inventing a prior or losing probability
// mass.
struct ExactCardAtom {
	int cardId = 0;
	int count = 0;
};

struct ExactCardClass {
	std::vector<ExactCardAtom> atoms;
	int count = 0;
	bool operatorVisible = false;
};

class ExactCardPartition {
public:
	ExactCardPartition() = default;

	explicit ExactCardPartition(std::vector<ExactCardAtom> population) {
		std::sort(population.begin(), population.end(), [](const auto& left, const auto& right) {
			return left.cardId < right.cardId;
		});
		ExactCardClass initial;
		for (const ExactCardAtom& atom : population) if (atom.count > 0) {
			initial.atoms.push_back(atom); initial.count += atom.count;
		}
		if (!initial.atoms.empty()) classes_.push_back(std::move(initial));
	}

	void exposeAllIdentities() {
		std::vector<ExactCardClass> split;
		for (const ExactCardClass& group : classes_) for (const ExactCardAtom& atom : group.atoms)
			split.push_back({ { atom }, atom.count, true });
		classes_ = std::move(split);
	}

	// Matching identities become singleton classes because choosing one search
	// result exposes its card ID.  Non-matching atoms remain reversibly grouped.
	template<class Predicate>
	void refineVisible(Predicate predicate) {
		std::vector<ExactCardClass> refined;
		for (const ExactCardClass& group : classes_) {
			// Refinement is monotone.  Once an operator has distinguished an
			// identity, a later predicate that does not match it must not hide or
			// merge it back into another equivalence class.
			if (group.operatorVisible) {
				refined.push_back(group);
				continue;
			}
			ExactCardClass hidden;
			for (const ExactCardAtom& atom : group.atoms) {
				if (predicate(atom.cardId)) refined.push_back({ { atom }, atom.count, true });
				else { hidden.atoms.push_back(atom); hidden.count += atom.count; }
			}
			if (!hidden.atoms.empty()) refined.push_back(std::move(hidden));
		}
		std::sort(refined.begin(), refined.end(), [](const auto& left, const auto& right) {
			int li = left.atoms.empty() ? 0 : left.atoms.front().cardId;
			int ri = right.atoms.empty() ? 0 : right.atoms.front().cardId;
			return li < ri;
		});
		classes_ = std::move(refined);
	}

	// Split a hidden class only when a continuation can distinguish its atoms.
	// Unlike refineVisible(), equal keys stay exchangeable and are not exposed as
	// physical identities. Key equality is exact; callers must not use a digest.
	template<class KeyFunction>
	void refineEquivalent(KeyFunction keyFor) {
		std::vector<ExactCardClass> refined;
		for (const ExactCardClass& group : classes_) {
			if (group.operatorVisible || group.atoms.size() <= 1) {
				refined.push_back(group); continue;
			}
			using Key = std::decay_t<decltype(keyFor(group.atoms.front().cardId))>;
			std::map<Key, ExactCardClass> byKey;
			for (const ExactCardAtom& atom : group.atoms) {
				ExactCardClass& target = byKey[keyFor(atom.cardId)];
				target.atoms.push_back(atom); target.count += atom.count;
			}
			for (auto& item : byKey) refined.push_back(std::move(item.second));
		}
		std::sort(refined.begin(), refined.end(), [](const auto& left, const auto& right) {
			int li = left.atoms.empty() ? 0 : left.atoms.front().cardId;
			int ri = right.atoms.empty() ? 0 : right.atoms.front().cardId;
			return li < ri;
		});
		classes_ = std::move(refined);
	}

	const std::vector<ExactCardClass>& classes() const { return classes_; }

	std::vector<int> visibleCardIds() const {
		std::vector<int> ids;
		for (const ExactCardClass& group : classes_)
			if (group.operatorVisible && group.atoms.size() == 1) ids.push_back(group.atoms.front().cardId);
		std::sort(ids.begin(), ids.end());
		return ids;
	}

	bool hasCompressedClass() const {
		for (const ExactCardClass& group : classes_) if (group.atoms.size() > 1) return true;
		return false;
	}

	int totalCount() const {
		int total = 0; for (const auto& group : classes_) total += group.count; return total;
	}

	std::string schemaKey() const {
		std::string key = "CARD-PARTITION-V1|";
		for (const ExactCardClass& group : classes_) {
			key += group.operatorVisible ? 'V' : 'H'; key += std::to_string(group.count); key += ':';
			for (const ExactCardAtom& atom : group.atoms)
				key += std::to_string(atom.cardId) + "x" + std::to_string(atom.count) + ',';
			key += ';';
		}
		return key;
	}

private:
	std::vector<ExactCardClass> classes_;
};

struct ExactStaticTargetResult {
	bool supported = false;
	bool matches = false;
};

inline bool ExactBoolCompare(bool value, ComparatorType comparator) {
	return comparator == ComparatorType::Equal ? value : !value;
}

inline ExactStaticTargetResult ExactStaticTargetMatches(const CardMaster& master,
	const TargetCondition& condition) {
	bool value = false;
	switch (condition.targetType) {
	case TargetType::All: value = true; break;
	case TargetType::MaxHp: return { true, Compare(master.hp, condition.val, condition.comparatorType) };
	case TargetType::EnergyType:
		value = MatchEnergyType(master.energyType, (EnergyType)condition.val); break;
	case TargetType::EnergyType2:
		value = MatchEnergyType(master.energyType, (EnergyType)condition.val)
			|| MatchEnergyType(master.energyType, (EnergyType)condition.val2); break;
	case TargetType::Resistance:
		value = MatchEnergyType(master.resistance, (EnergyType)condition.val); break;
	case TargetType::PokemonCard: value = master.isPokemonCard(); break;
	case TargetType::BasicPokemon:
		value = master.cardType == CardType::Pokemon && master.evolutionType == EvolutionType::Basic; break;
	case TargetType::EvolvedPokemon:
		value = master.evolutionType == EvolutionType::Stage1 || master.evolutionType == EvolutionType::Stage2; break;
	case TargetType::Stage1: value = master.evolutionType == EvolutionType::Stage1; break;
	case TargetType::Stage2: value = master.evolutionType == EvolutionType::Stage2; break;
	case TargetType::BasicEnergy: value = master.cardType == CardType::BasicEnergy; break;
	case TargetType::SpecialEnergy: value = master.cardType == CardType::SpecialEnergy; break;
	case TargetType::EnergyCard: value = IsEnergy(master.cardType); break;
	case TargetType::Item: value = master.cardType == CardType::Item; break;
	case TargetType::Tool: value = master.cardType == CardType::Tool; break;
	case TargetType::Supporter: value = master.cardType == CardType::Supporter; break;
	case TargetType::Stadium: value = master.cardType == CardType::Stadium; break;
	case TargetType::Trainer: value = IsTrainer(master.cardType); break;
	case TargetType::CardId: value = master.cardId == condition.val; break;
	case TargetType::PokemonOrBasicEnergy:
		value = master.cardType == CardType::Pokemon || master.cardType == CardType::BasicEnergy; break;
	case TargetType::BasicPokemonOrBasicEnergy:
		value = (master.cardType == CardType::Pokemon && master.evolutionType == EvolutionType::Basic)
			|| master.cardType == CardType::BasicEnergy; break;
	case TargetType::NotRulePokemonCardOrBasicEnergy:
		value = master.isNotRulePokemonCard() || master.cardType == CardType::BasicEnergy; break;
	case TargetType::ItemOrTool:
		value = master.cardType == CardType::Item || master.cardType == CardType::Tool; break;
	case TargetType::EthanPokemonOrBasicFireEnergy:
		value = master.ethan || master.cardId == FIRE_ENERGY; break;
	// The engine resolves this through State::getAbility(), which may be changed
	// by rule effects.  A master-only analyzer cannot classify it safely.
	case TargetType::HasAbility: return { false, false };
	case TargetType::RulePokemon: value = master.isRulePokemon(); break;
	case TargetType::NotRulePokemon: value = master.isNotRulePokemon(); break;
	case TargetType::Ex: value = master.isEx(); break;
	case TargetType::MegaEx: value = master.pokemonType == PokemonType::MegaEx; break;
	case TargetType::Terastal: value = master.tera; break;
	case TargetType::Ancient: value = master.ancient; break;
	case TargetType::Future: value = master.future; break;
	case TargetType::Hop: value = master.hop; break;
	case TargetType::Lillie: value = master.lillie; break;
	case TargetType::Iono: value = master.iono; break;
	case TargetType::N: value = master.n; break;
	case TargetType::Ethan: value = master.ethan; break;
	case TargetType::Cynthia: value = master.cynthia; break;
	case TargetType::Misty: value = master.misty; break;
	case TargetType::Arven: value = master.arven; break;
	case TargetType::Steven: value = master.steven; break;
	case TargetType::Marnie: value = master.marnie; break;
	case TargetType::Erika: value = master.erika; break;
	case TargetType::Larry: value = master.larry; break;
	case TargetType::TeamRocket: value = master.teamRocket; break;
	case TargetType::Name: value = master.name == condition.name; break;
	case TargetType::NameContains: value = master.name.find(condition.name) != std::u8string::npos; break;
	default: return { false, false };
	}
	return { true, ExactBoolCompare(value, condition.comparatorType) };
}

inline ExactStaticTargetResult ExactStaticTargetMatches(const CardMaster& master, const Target& target) {
	for (const TargetCondition& condition : target.conditions) {
		ExactStaticTargetResult result = ExactStaticTargetMatches(master, condition);
		if (!result.supported) return result;
		if (!result.matches) return { true, false };
	}
	return { true, true };
}
