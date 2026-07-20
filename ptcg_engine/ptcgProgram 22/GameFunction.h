// SPDX-FileCopyrightText: © Pokémon/Nintendo/Creatures/GAME FREAK TM, ®, and character names are trademarks of Nintendo.
// SPDX-License-Identifier: LicenseRef-PTCG-ABC-Competition-Use-Only
// Part of the Pokémon TCG AI Battle Challenge. Provided for Competition use only;
// the full license is in the LICENSES/ folder and incorporates the Competition Rules.
// Competition Rules: https://www.kaggle.com/competitions/pokemon-tcg-ai-battle/rules

#pragma once

#include "Core.h"

struct State;

using GameFunctionNoArg = void(*)(State&);
using GameFunctionI = void(*)(State&, int);
using GameFunctionB = void(*)(State&, bool);
using GameFunctionII = void(*)(State&, int, int);
using GameFunctionIII = void(*)(State&, int, int, int);


inline std::vector<void*> FunctionTable;
inline std::unordered_map<long long, int> FunctionIndexTable;

enum class ArgType : unsigned char {
	None = 0, // 無し
	I, // int
	B, // bool
	II, // int, int
	III, // int, int, int
};

// The engine ABI stores deferred arguments as ints, but an int can represent
// very different things. Keep that meaning in a function-index side table:
// GameFunction is serialized as raw bytes by the official runtime and its
// 20-byte wire layout must never be extended.
enum class DeferredArgSemantic : unsigned char {
	None = 0,
	Scalar,
	CardId,
	CardReference,
	PlayerIndex,
	ZoneIndex,
	EffectId,
	Unknown,
};

using DeferredArgSemantics = std::array<DeferredArgSemantic, 3>;

struct GameFunction;

inline std::unordered_map<int, DeferredArgSemantics>& DeferredFunctionSemanticTable() {
	static std::unordered_map<int, DeferredArgSemantics> semantics;
	return semantics;
}

inline void RegisterDeferredFunctionSemanticTableEntry(int functionIndex, DeferredArgSemantics semantics) {
	if (functionIndex >= 0) DeferredFunctionSemanticTable()[functionIndex] = semantics;
}

inline DeferredArgSemantics DeferredSemanticsFor(const GameFunction& function);

// 呼び出し予約情報
struct GameFunction {
	int functionIndex = 0;
	int arg0 = 0;
	int arg1 = 0;
	int arg2 = 0;
	ArgType argType = ArgType::None; // 引数タイプ
	unsigned char callCount = 1;
	unsigned char calledCount = 0;

	GameFunction() = default;
	GameFunction(void* functionPointer, ArgType argType) : argType(argType) {
		functionIndex = FunctionIndexTable.at((long long)functionPointer);
	}

	void setCallCount(int callCount) {
		this->callCount = (unsigned char)callCount;
	}
};

static_assert(sizeof(GameFunction) == 20,
	"GameFunction is part of the official raw wire format");

inline DeferredArgSemantics DeferredSemanticsFor(const GameFunction& function) {
	const auto found = DeferredFunctionSemanticTable().find(function.functionIndex);
	if (found != DeferredFunctionSemanticTable().end()) return found->second;
	if (function.argType == ArgType::None)
		return { DeferredArgSemantic::None, DeferredArgSemantic::None,
			DeferredArgSemantic::None };
	// An unregistered deferred callback is opaque. Treating its integer
	// arguments as scalars is an estimate and must not enter exact state keys.
	return { DeferredArgSemantic::Unknown, DeferredArgSemantic::Unknown,
		DeferredArgSemantic::Unknown };
}
