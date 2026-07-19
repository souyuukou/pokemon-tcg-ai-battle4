// SPDX-FileCopyrightText: © Pokémon/Nintendo/Creatures/GAME FREAK TM, ®, and character names are trademarks of Nintendo.
// SPDX-License-Identifier: LicenseRef-PTCG-ABC-Competition-Use-Only
// Part of the Pokémon TCG AI Battle Challenge. Provided for Competition use only;
// the full license is in the LICENSES/ folder and incorporates the Competition Rules.
// Competition Rules: https://www.kaggle.com/competitions/pokemon-tcg-ai-battle/rules

#pragma once

#include "BattleData.h"
#include "CardImpl.h"
#include "InitializeCard.h"
#include "Api.h"
#include "ExactSearchHooks.h"
#include "ExactPlanner.h"

inline void InitializeAll() {
  assert(CardTable.size() == 0);
  assert(FunctionTable.size() == 0);
  InitializeBattleFunction();
  // Complete the deferred-effect registry before any search worker can start.
  // ExactEnsureDeferredFunctionRegistry is call_once guarded for later lazy
  // callers, but missing symbols are still a startup error here.
  ExactEnsureDeferredFunctionRegistry();

  CardImpl();
	InitializeCard();
	InitializeCardMasterIndex();
}
