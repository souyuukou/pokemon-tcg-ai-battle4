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
// very different things.  Keep that meaning beside the deferred frame so the
// exact canonicalizer never has to guess from a raw value.  Scalar is the
// conservative default used by legacy pushFunction callers.
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
	DeferredArgSemantic arg0Semantic = DeferredArgSemantic::None;
	DeferredArgSemantic arg1Semantic = DeferredArgSemantic::None;
	DeferredArgSemantic arg2Semantic = DeferredArgSemantic::None;
	unsigned char callCount = 1;
	unsigned char calledCount = 0;

	GameFunction() = default;
	GameFunction(void* functionPointer, ArgType argType) : argType(argType) {
		functionIndex = FunctionIndexTable.at((long long)functionPointer);
		const DeferredArgSemantic scalar = argType == ArgType::None
			? DeferredArgSemantic::None : DeferredArgSemantic::Scalar;
		arg0Semantic = scalar; arg1Semantic = scalar; arg2Semantic = scalar;
	}

	void setArgumentSemantics(DeferredArgSemantic first,
		DeferredArgSemantic second = DeferredArgSemantic::None,
		DeferredArgSemantic third = DeferredArgSemantic::None) {
		arg0Semantic = first; arg1Semantic = second; arg2Semantic = third;
	}

	void setCallCount(int callCount) {
		this->callCount = (unsigned char)callCount;
	}
};

inline DeferredArgSemantics DeferredSemanticsFor(const GameFunction& function) {
	const auto found = DeferredFunctionSemanticTable().find(function.functionIndex);
	if (found != DeferredFunctionSemanticTable().end()) return found->second;
	return { function.arg0Semantic, function.arg1Semantic, function.arg2Semantic };
}
