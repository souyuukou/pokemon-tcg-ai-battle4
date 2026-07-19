// SPDX-FileCopyrightText: © Pokémon/Nintendo/Creatures/GAME FREAK TM, ®, and character names are trademarks of Nintendo.
// SPDX-License-Identifier: LicenseRef-PTCG-ABC-Competition-Use-Only
// Part of the Pokémon TCG AI Battle Challenge. Provided for Competition use only;
// the full license is in the LICENSES/ folder and incorporates the Competition Rules.
// Competition Rules: https://www.kaggle.com/competitions/pokemon-tcg-ai-battle/rules

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX

#include "All.h"
#include <future>
#include <atomic>
#include <mutex>
#include <cmath>

#ifdef _MSC_VER
#	define GAME_API __declspec(dllexport)
#else
#	define GAME_API __attribute__ ((visibility("default")))
#endif


static JsonBuilder AllCardJson;
static JsonBuilder AllAttackJson;
static void AppendUnsignedLongLong(JsonBuilder& j, unsigned long long value);

static const char8_t* ExactErrorJson(ApiData* data, int code, const std::string& message = {}) {
  JsonBuilder& j = data->jsonBuilder;
  j.clear(); j.append('{'); j.appendKeyValue("error", code);
  if (!message.empty()) {
    j.appendCommaKey("message");
    j.appendDoubleQuote(std::u8string((const char8_t*)message.c_str(), message.size()));
  }
  j.append('}');
  return j.buf.c_str();
}

extern "C" GAME_API const char8_t* ExactLoadEvaluatorModel(ApiData* data, const char* path) {
	if (data == nullptr) {
		// No ApiData exists to own a JsonBuilder in this error path.  Return a
		// stable thread-local response rather than dereferencing a null pointer.
		static thread_local const char8_t kInvalidApiData[] = u8"{\"loaded\":false,\"error\":\"invalid ApiData\"}";
		return kInvalidApiData;
	}
	if (path == nullptr || path[0] == '\0') return ExactErrorJson(data, 400, "evaluator path is null or empty");
  JsonBuilder& j = data->jsonBuilder;
  j.clear(); j.append('{');
  std::string error;
  bool loaded = false;
  auto evaluator = std::make_shared<ExactCpuEvaluator>();
  loaded = evaluator->load(path, error);
  int schema = loaded ? evaluator->schemaVersion() : 0;
  int evaluatorVersion = loaded ? (int)evaluator->evaluatorVersion() : 0;
  bool informationSetSafe = loaded && evaluator->informationSetSafe();
  bool boundaryOnly = loaded && evaluator->boundaryOnly();
  unsigned long long modelHash = loaded ? evaluator->modelHash() : 0;
  unsigned long long residentBytes = loaded ? evaluator->residentBytes() : 0;
  if (loaded) data->exactEvaluator = std::move(evaluator);
  j.appendKeyValue("loaded", loaded);
  j.appendCommaKeyValue("schemaVersion", schema);
  j.appendCommaKeyValue("evaluatorVersion", evaluatorVersion);
  j.appendCommaKeyValue("informationSetSafe", informationSetSafe);
  j.appendCommaKey("featureSchemaHash"); AppendUnsignedLongLong(j, ExactFeatureSchemaHash);
  j.appendCommaKey("beliefSchemaHash"); AppendUnsignedLongLong(j, ExactBeliefSchemaHash);
  j.appendCommaKeyValue("boundaryOnly", boundaryOnly);
  j.appendCommaKey("modelHash"); AppendUnsignedLongLong(j, modelHash);
  j.appendCommaKey("residentBytes"); AppendUnsignedLongLong(j, residentBytes);
  j.appendCommaKey("error");
  j.appendDoubleQuote(std::u8string((const char8_t*)error.c_str(), error.size()));
  j.append('}');
  return j.buf.c_str();
}

extern "C" GAME_API void ExactUnloadEvaluatorModel(ApiData* data) {
  if (data != nullptr) data->exactEvaluator.reset();
}

extern "C" GAME_API const char8_t* ExactArithmeticDiagnostics() {
  static thread_local JsonBuilder j; j.clear();
  ExactWeight a(50'063'860ULL), b(321'387'366'339'585ULL);
  ExactWeight product = ExactWeight::multiply(a, b);
  auto division = ExactWeight::divideRemainder(product, a);
  ExactWeight common = ExactWeight::gcd(product, a);
  bool hashPairMatchesScalar = true;
  for (size_t length = 0; length <= 257; ++length) {
    std::string input(length, '\0');
    for (size_t i = 0; i < length; ++i) input[i] = (char)((i * 131U + length * 17U) & 255U);
    unsigned long long lo = ExactSipHash24(input, 0x7766554433221100ULL, 0xffeeddccbbaa9988ULL);
    unsigned long long hi = ExactSipHash24(input, 0x8899aabbccddeeffULL, 0x0011223344556677ULL);
    size_t scalar = (size_t)(lo ^ ExactRotl64(hi, 1));
    if (ExactStringHasher{}(input) != scalar) { hashPairMatchesScalar = false; break; }
  }
  j.append('{'); j.appendKey("product"); j.appendDoubleQuote(product.text().c_str());
  j.appendCommaKey("quotient"); j.appendDoubleQuote(division.first.text().c_str());
  j.appendCommaKey("remainder"); j.appendDoubleQuote(division.second.text().c_str());
  j.appendCommaKey("gcd"); j.appendDoubleQuote(common.text().c_str());
  j.appendCommaKeyValue("bits", (int)product.bitLength());
  j.appendCommaKeyValue("hashPairMatchesScalar", hashPairMatchesScalar);
  j.appendCommaKeyValue("promoted", product.isLarge()); j.append('}'); return j.buf.c_str();
}

extern "C" GAME_API int ExactCardLivenessV4SchemaVersion() {
  return ExactCardLivenessV4::LivenessSchemaVersion;
}

extern "C" GAME_API const char8_t* ExactCardLivenessV4Diagnostics() {
  static thread_local JsonBuilder j;
  // Observation coverage: unclassified EffectType values fail closed as Unknown.
  int classified = 0, unknown = 0;
  for (int raw = 0; raw < 256; ++raw) {
    auto kind = ExactCardLivenessV4::ObservationKindForEffect((EffectType)raw);
    if (kind == ExactCardLivenessV4::CardObservationKind::Unknown) ++unknown;
    else ++classified;
  }
  // Synthetic locked-energy Passive proof (no Game): energyPlayed + energy card.
  int samplePassive = 0, sampleActive = 0, sampleUnknown = 0;
  Game game; GameConfig config{}; game.init(config); State state{}; state.game = &game;
  state.turn = 2; state.firstPlayer = 0; state.energyPlayed = true;
  state.players[0].playerIndex = 0; state.players[1].playerIndex = 1;
  int energyId = 0, itemId = 0, basicId = 0, supporterId = 0;
  const int ultraBallId = 1121;
  for (const auto& item : CardTable) {
    if (energyId == 0 && IsEnergy(item.second.cardType)) energyId = item.first;
    if (itemId == 0 && item.second.cardType == CardType::Item) itemId = item.first;
    if (basicId == 0 && item.second.cardType == CardType::Pokemon
      && item.second.evolutionType == EvolutionType::Basic) basicId = item.first;
    if (supporterId == 0 && item.second.cardType == CardType::Supporter) supporterId = item.first;
  }
  auto classify = [&](int id, const ExactCardLivenessV4::OperatorClosure& closure) {
    auto result = ExactCardLivenessV4::ClassifyCardId(state, 0, id, closure);
    if (result.liveness == ExactCardLivenessV4::CardLiveness::Passive) ++samplePassive;
    else if (result.liveness == ExactCardLivenessV4::CardLiveness::Active) ++sampleActive;
    else ++sampleUnknown;
    return result;
  };
  ExactCardLivenessV4::OperatorClosure empty =
    ExactCardLivenessV4::BuildOperatorClosure({}, 0);
  if (energyId) classify(energyId, empty);
  if (itemId) classify(itemId, empty);
  if (basicId) classify(basicId, empty);

  // P0-1 counterexample: supporter already used, but Ultra Ball can discard it.
  state.supporterPlayed = true;
  ExactCardLivenessV4::OperatorClosure withUltra =
    ExactCardLivenessV4::BuildOperatorClosure({ ultraBallId }, 0);
  ExactCardLivenessV4::ApplyStateCoverageScanners(withUltra, state);
  bool ultraDiscardCostObserved = false;
  for (const auto& fp : withUltra.footprints) {
    if (fp.operatorCardId == ultraBallId && fp.mayDiscardHand) {
      ultraDiscardCostObserved = true;
      break;
    }
  }
  ExactCardLivenessV4::CardLivenessResult supporterVsUltra{};
  bool ultraBlocksSupporter = false;
  if (supporterId) {
    supporterVsUltra = ExactCardLivenessV4::ClassifyCardId(state, 0, supporterId, withUltra);
    ultraBlocksSupporter = supporterVsUltra.liveness != ExactCardLivenessV4::CardLiveness::Passive;
  }
  // Empty closure: used supporter with no hand-targeting operators can be Passive.
  ExactCardLivenessV4::CardLivenessResult supporterAlone =
    supporterId ? ExactCardLivenessV4::ClassifyCardId(state, 0, supporterId, empty)
      : ExactCardLivenessV4::CardLivenessResult{};

  // Damage-only operator must not block Passive for a locked energy.
  bool damageDoesNotBlockEnergy = false;
  if (energyId && basicId) {
    ExactCardLivenessV4::OperatorClosure damageOnly =
      ExactCardLivenessV4::BuildOperatorClosure({ basicId }, 0);
    ExactCardLivenessV4::ApplyStateCoverageScanners(damageOnly, state);
    // Strip non-None footprints to simulate AttackDamage-only reachability.
    std::vector<ExactCardLivenessV4::OperatorFootprint> kept;
    for (const auto& fp : damageOnly.footprints) {
      if (fp.observation == ExactCardLivenessV4::CardObservationKind::None
        || fp.observation == ExactCardLivenessV4::CardObservationKind::CountOnly)
        kept.push_back(fp);
    }
    damageOnly.footprints.swap(kept);
    damageOnly.hasUnknown = false;
    auto energyVsDamage = ExactCardLivenessV4::ClassifyCardId(state, 0, energyId, damageOnly);
    damageDoesNotBlockEnergy =
      energyVsDamage.liveness == ExactCardLivenessV4::CardLiveness::Passive;
  }

  // Energy-only hand discard must Active energy but leave used Supporter Passive.
  bool energyDiscardKeepsSupporterPassive = false;
  bool energyDiscardActivesEnergy = false;
  if (energyId && supporterId) {
    ExactCardLivenessV4::OperatorClosure energyCost = empty;
    ExactCardLivenessV4::OperatorFootprint fp;
    fp.operatorCardId = 999001;
    fp.effectType = EffectType::ToTrash;
    fp.observation = ExactCardLivenessV4::CardObservationKind::CardIdentity;
    fp.hasTarget = true;
    fp.target.areas.push_back(AreaType::Hand);
    TargetCondition cond{};
    cond.targetType = TargetType::EnergyCard;
    cond.comparatorType = ComparatorType::Equal;
    fp.target.conditions.push_back(cond);
    fp.mayTargetHand = true;
    fp.mayDiscardHand = true;
    fp.mayMoveCardZones = true;
    energyCost.footprints.push_back(fp);
    auto sup = ExactCardLivenessV4::ClassifyCardId(state, 0, supporterId, energyCost);
    auto en = ExactCardLivenessV4::ClassifyCardId(state, 0, energyId, energyCost);
    energyDiscardKeepsSupporterPassive =
      sup.liveness == ExactCardLivenessV4::CardLiveness::Passive;
    energyDiscardActivesEnergy =
      en.liveness != ExactCardLivenessV4::CardLiveness::Passive;
  }

  // Attack/delay footprints are present for pokemon with attacks.
  bool attackFootprintsPresent = false;
  if (basicId) {
    ExactCardLivenessV4::OperatorClosure withBasic =
      ExactCardLivenessV4::BuildOperatorClosure({ basicId }, 0);
    const CardMaster* bm = FindCardMaster(basicId);
    if (bm != nullptr && !bm->attacks.empty()) {
      for (const auto& fp : withBasic.footprints) {
        if (fp.operatorCardId == basicId) { attackFootprintsPresent = true; break; }
      }
    }
  }

  // Reachable Draw operator ⇒ AnyReachableFurtherChance.
  bool drawImpliesFutureChance = false;
  {
    ExactCardLivenessV4::OperatorClosure drawOp = empty;
    ExactCardLivenessV4::OperatorFootprint fp;
    fp.effectType = EffectType::Draw;
    fp.observation = ExactCardLivenessV4::CardObservationKind::CountOnly;
    drawOp.footprints.push_back(fp);
    drawImpliesFutureChance = ExactCardLivenessV4::AnyReachableFurtherChance(drawOp);
  }

  // Same-card later Draw must NOT be skipped when excluding only the current Effect key.
  bool sameCardLaterDrawDetected = false;
  {
    ExactCardLivenessV4::OperatorClosure twoDraws = empty;
    ExactCardLivenessV4::OperatorSourceKey first;
    first.cardId = 13; first.skillId = 6; first.effectIndex = 0;
    first.kind = ExactCardLivenessV4::OperatorSourceKind::Play;
    ExactCardLivenessV4::OperatorSourceKey second = first;
    second.effectIndex = 1;
    ExactCardLivenessV4::OperatorFootprint a;
    a.source = first; a.operatorCardId = 13; a.effectType = EffectType::Draw;
    a.observation = ExactCardLivenessV4::CardObservationKind::CountOnly;
    ExactCardLivenessV4::OperatorFootprint b = a;
    b.source = second;
    twoDraws.footprints.push_back(a);
    twoDraws.footprints.push_back(b);
    sameCardLaterDrawDetected = ExactCardLivenessV4::AnyReachableFurtherChance(twoDraws, first);
  }

  j.clear(); j.append('{');
  j.appendKeyValue("livenessSchemaVersion", ExactCardLivenessV4::LivenessSchemaVersion);
  j.appendCommaKeyValue("effectObservationClassified", classified);
  j.appendCommaKeyValue("effectObservationUnknown", unknown);
  j.appendCommaKeyValue("samplePassive", samplePassive);
  j.appendCommaKeyValue("sampleActive", sampleActive);
  j.appendCommaKeyValue("sampleUnknown", sampleUnknown);
  j.appendCommaKeyValue("energyOncePassive", energyId != 0 && samplePassive >= 1);
  j.appendCommaKeyValue("ultraBallDiscardCostObserved", ultraDiscardCostObserved);
  j.appendCommaKeyValue("ultraBallBlocksUsedSupporter", ultraBlocksSupporter);
  j.appendCommaKeyValue("usedSupporterPassiveWithoutUltra",
    supporterId != 0 && supporterAlone.liveness == ExactCardLivenessV4::CardLiveness::Passive);
  j.appendCommaKeyValue("damageOnlyDoesNotBlockPassiveEnergy", damageDoesNotBlockEnergy);
  j.appendCommaKeyValue("energyDiscardKeepsSupporterPassive", energyDiscardKeepsSupporterPassive);
  j.appendCommaKeyValue("energyDiscardActivesEnergy", energyDiscardActivesEnergy);
  j.appendCommaKeyValue("attackFootprintsPresent", attackFootprintsPresent);
  j.appendCommaKeyValue("drawImpliesFutureChance", drawImpliesFutureChance);
  j.appendCommaKeyValue("sameCardLaterDrawDetected", sameCardLaterDrawDetected);
  // Which reachable operators still produce Unknown footprints?
  int unknownFootprintTypes = 0;
  {
    ExactCardLivenessV4::OperatorClosure probe =
      ExactCardLivenessV4::BuildOperatorClosure({ 5, 13, 741, 305, 1123, 1266 }, 0);
    for (const auto& fp : probe.footprints) {
      if (fp.observation == ExactCardLivenessV4::CardObservationKind::Unknown)
        ++unknownFootprintTypes;
    }
    j.appendCommaKeyValue("safeDeckUnknownFootprints", unknownFootprintTypes);
    j.appendCommaKeyValue("safeDeckHasUnknown", probe.hasUnknown);
  }
  j.appendCommaKeyValue("supporterLiveness", (int)supporterVsUltra.liveness);
  j.append('}');
  return j.buf.c_str();
}

extern "C" GAME_API const char8_t* ExactPassiveExpectationV4Oracle(
    int poolSize, int take, int copiesI, int copiesJ, int mode) {
  // mode: 0=E[X_i], 1=E[X_i X_j], 2=E[C(X_i,2)]
  static thread_local JsonBuilder j;
  ExactBigRational value(0, 1);
  try {
    if (mode == 0) value = ExactPassiveExpectationV4::ExpectedCount(poolSize, take, copiesI);
    else if (mode == 1) value = ExactPassiveExpectationV4::ExpectedProductDistinct(poolSize, take, copiesI, copiesJ);
    else value = ExactPassiveExpectationV4::ExpectedChoose2(poolSize, take, copiesI);
  } catch (...) {
    j.clear(); j.appendStr("{\"error\":1}"); return j.buf.c_str();
  }
  j.clear(); j.append('{');
  j.appendKey("numerator"); j.appendDoubleQuote(value.numerator.text().c_str());
  ExactBigUnsigned den = ExactBigRational::factorProduct(value.denominator);
  j.appendCommaKey("denominator"); j.appendDoubleQuote(den.text().c_str());
  j.append('}');
  return j.buf.c_str();
}

extern "C" GAME_API const char8_t* ExactEvaluatorTokensV3() {
  static thread_local JsonBuilder j; std::vector<int> tokens{ 0 };
  for (const auto& item : CardTable) { tokens.push_back(item.first);
    tokens.push_back(ExactSparseEvaluatorV3::ComboTokenBase + item.first);
    if (item.second.evolutionType == EvolutionType::Stage2)
      tokens.push_back(ExactSparseEvaluatorV3::ComboTokenBase + 250'000 + item.first); }
  for (const auto& item : AttackTable) { tokens.push_back(ExactSparseEvaluatorV3::AttackTokenBase + item.first);
    tokens.push_back(ExactSparseEvaluatorV3::ComboTokenBase + 500'000 + item.first); }
  for (int id = 1; id <= 1'000; ++id) tokens.push_back(ExactSparseEvaluatorV3::EffectTokenBase + id);
  for (const auto& item : SkillTable) tokens.push_back(ExactSparseEvaluatorV3::EffectTokenBase + 400 + item.first);
  std::sort(tokens.begin(), tokens.end()); tokens.erase(std::unique(tokens.begin(), tokens.end()), tokens.end());
  j.clear(); j.append('['); for (int i = 0; i < (int)tokens.size(); ++i) { j.comma(i); j.append(tokens[i]); }
  j.append(']'); return j.buf.c_str();
}

extern "C" GAME_API const char8_t* ExactEvaluatorV3Diagnostics() {
  static thread_local JsonBuilder j;
  int pokemon0 = 0, pokemon1 = 0, energy = 0, hidden0 = 0, hidden1 = 0;
  for (const auto& item : CardTable) {
    if (item.second.isPokemon() && pokemon0 == 0) pokemon0 = item.first;
    else if (item.second.isPokemon() && item.first != pokemon0 && pokemon1 == 0) pokemon1 = item.first;
    if (IsEnergy(item.second.cardType) && energy == 0) energy = item.first;
    if (hidden0 == 0) hidden0 = item.first; else if (item.first != hidden0 && hidden1 == 0) hidden1 = item.first;
  }
  bool initialized = pokemon0 && pokemon1 && energy && hidden0 && hidden1;
  auto bytes = [](const ExactSparseEvaluatorV3::FeatureRecord& f) {
    std::string out((const char*)f.globalDense.data(), sizeof(f.globalDense));
    out.append((const char*)&f.globalSparse.count, sizeof(f.globalSparse.count));
    out.append((const char*)f.globalSparse.values.data(), f.globalSparse.count * sizeof(f.globalSparse.values[0]));
    out.push_back((char)f.entityCount);
    for (int i = 0; i < f.entityCount; ++i) { const auto& e = f.entities[i];
      out.append((const char*)&e.pool, sizeof(e.pool)); out.append((const char*)e.dense.data(), sizeof(e.dense));
      out.append((const char*)&e.sparse.count, sizeof(e.sparse.count));
      out.append((const char*)e.sparse.values.data(), e.sparse.count * sizeof(e.sparse.values[0])); }
    return out;
  };
  bool benchOrderInvariant = false, attachmentSensitive = false;
  bool opponentHiddenInvariant = false, typedEffectSensitive = false;
  if (initialized) {
    Game game; GameConfig config{}; game.init(config); State state{}; state.game = &game;
    state.turn = 2; state.firstPlayer = 0; state.players[0].playerIndex = 0; state.players[1].playerIndex = 1;
    auto card = [&](int index, int id, int owner, AreaType area) -> CardRef {
      state.allCard[index].init(id, 100 + index, owner); state.allCard[index].area = area; return CardRef(index);
    };
    CardRef active0 = card(1, pokemon0, 0, AreaType::Active);
    CardRef bench0 = card(2, pokemon0, 0, AreaType::Bench);
    CardRef bench1 = card(3, pokemon1, 0, AreaType::Bench); state.getCard(bench1).damage = 10;
    CardRef active1 = card(4, pokemon1, 1, AreaType::Active);
    CardRef attached = card(5, energy, 0, AreaType::Energy);
    CardRef hidden = card(6, hidden0, 1, AreaType::Hand);
    state.players[0].active.push_back(active0); state.players[0].bench.push_back(bench0); state.players[0].bench.push_back(bench1);
    state.players[0].energy.push_back(attached); state.getCard(attached).attachMoveCounter = state.getCard(bench0).moveCounter;
    state.players[1].active.push_back(active1); state.players[1].hand.push_back(hidden);
    auto original = bytes(ExactSparseEvaluatorV3::extractFeatures(state, 0));
    std::swap(state.players[0].bench[0], state.players[0].bench[1]);
    benchOrderInvariant = original == bytes(ExactSparseEvaluatorV3::extractFeatures(state, 0));
    state.getCard(attached).attachMoveCounter = state.getCard(bench1).moveCounter;
    attachmentSensitive = original != bytes(ExactSparseEvaluatorV3::extractFeatures(state, 0));
    state.getCard(attached).attachMoveCounter = state.getCard(bench0).moveCounter;
    state.getCard(hidden).cardId = hidden1;
    opponentHiddenInvariant = original == bytes(ExactSparseEvaluatorV3::extractFeatures(state, 0));
    state.getCard(hidden).cardId = hidden0; state.getCard(bench0).cannotAttack = true;
    typedEffectSensitive = original != bytes(ExactSparseEvaluatorV3::extractFeatures(state, 0));
  }
  j.clear(); j.append('{'); j.appendKeyValue("initialized", initialized);
  j.appendCommaKeyValue("benchOrderInvariant", benchOrderInvariant);
  j.appendCommaKeyValue("attachmentSensitive", attachmentSensitive);
  j.appendCommaKeyValue("opponentHiddenInvariant", opponentHiddenInvariant);
  j.appendCommaKeyValue("typedEffectSensitive", typedEffectSensitive); j.append('}'); return j.buf.c_str();
}

extern "C" GAME_API long long ExactEvaluateFeaturesV3(ApiData* data,
  const int* globalDense, int globalDenseCount,
  const int* globalSparseTriplets, int globalSparseCount,
  const int* entityDense, const int* entityPools, int entityCount,
  const int* entitySparseQuads, int entitySparseCount, int* error) {
  if (error != nullptr) *error = 0;
  if (data == nullptr || !data->exactEvaluator || globalDense == nullptr
    || globalDenseCount != ExactSparseEvaluatorV3::GlobalDenseCount
    || globalSparseCount < 0 || entityCount < 0 || entityCount > ExactSparseEvaluatorV3::MaxEntities
    || entitySparseCount < 0 || (globalSparseCount != 0 && globalSparseTriplets == nullptr)
    || (entityCount != 0 && (entityDense == nullptr || entityPools == nullptr))
    || (entitySparseCount != 0 && entitySparseQuads == nullptr)) {
    if (error != nullptr) *error = 1; return 0;
  }
  ExactSparseEvaluatorV3::FeatureRecord features;
  for (int i = 0; i < globalDenseCount; ++i) features.globalDense[i] = globalDense[i];
  for (int i = 0; i < globalSparseCount; ++i) {
    int relation = globalSparseTriplets[i * 3], token = globalSparseTriplets[i * 3 + 1], value = globalSparseTriplets[i * 3 + 2];
    if (relation < 0 || relation >= ExactSparseEvaluatorV3::GlobalRelationCount
      || !features.globalSparse.push(token, (short)relation, value)) {
      if (error != nullptr) *error = 2; return 0;
    }
  }
  features.entityCount = (unsigned char)entityCount;
  for (int entity = 0; entity < entityCount; ++entity) {
    if (entityPools[entity] < 0 || entityPools[entity] >= ExactSparseEvaluatorV3::PoolCount) {
      if (error != nullptr) *error = 2; return 0;
    }
    features.entities[entity].pool = entityPools[entity];
    for (int d = 0; d < ExactSparseEvaluatorV3::EntityDenseCount; ++d)
      features.entities[entity].dense[d] = entityDense[entity * ExactSparseEvaluatorV3::EntityDenseCount + d];
  }
  for (int i = 0; i < entitySparseCount; ++i) {
    int entity = entitySparseQuads[i * 4], relation = entitySparseQuads[i * 4 + 1];
    int token = entitySparseQuads[i * 4 + 2], value = entitySparseQuads[i * 4 + 3];
    if (entity < 0 || entity >= entityCount || relation < 0 || relation >= ExactSparseEvaluatorV3::EntityRelationCount
      || !features.entities[entity].sparse.push(token, (short)relation, value)) {
      if (error != nullptr) *error = 2; return 0;
    }
  }
  long long value = 0;
  if (!data->exactEvaluator->evaluateV3Features(features, value)) {
    if (error != nullptr) *error = 3; return 0;
  }
  return value;
}

extern "C" GAME_API int ExactReplayTraceBegin(ApiData* data) {
  if (data == nullptr || data->apiDataType != 1) return 30;
  data->exactReplayTurnLeaves.clear(); data->exactReplayLastTurn = -1;
  data->exactReplayTraceEnabled = true; data->game.config.pauseAtExactTurnLeaf = true; return 0;
}

extern "C" GAME_API int ExactReplaySetDeckOrder(ApiData* data, int playerIndex,
  const int* cardIds, int count) {
  if (data == nullptr || data->apiDataType != 1) return 30;
  if (playerIndex < 0 || playerIndex >= 2 || cardIds == nullptr || count < 0) return 1;
  PlayerState& player = data->state.players[playerIndex];
  if (player.deck.size() != count) return 2;
  CardList remaining = player.deck;
  CardList ordered;
  for (int outputIndex = 0; outputIndex < count; ++outputIndex) {
    int found = -1;
    for (int inputIndex = 0; inputIndex < remaining.size(); ++inputIndex) {
      if ((int)data->state.getCard(remaining[inputIndex]).cardId == cardIds[outputIndex]) {
        found = inputIndex; break;
      }
    }
    if (found < 0) return 3;
    ordered.push_back(remaining.take(found));
  }
  if (!remaining.empty()) return 4;
  player.deck = ordered;
  return 0;
}

extern "C" GAME_API int ExactReplaySetHiddenZones(ApiData* data, int playerIndex,
  const int* handIds, int handCount, const int* deckIds, int deckCount) {
  if (data == nullptr || data->apiDataType != 1) return 30;
  if (playerIndex < 0 || playerIndex >= 2 || handCount < 0 || deckCount < 0
    || (handCount > 0 && handIds == nullptr) || (deckCount > 0 && deckIds == nullptr)) return 1;
  PlayerState& player = data->state.players[playerIndex];
  if (player.hand.size() + player.deck.size() != handCount + deckCount) return 2;
  CardList remaining = player.hand;
  for (CardRef ref : player.deck) remaining.push_back(ref);
  std::unordered_map<int, int> actualCounts;
  std::unordered_map<int, int> requestedCounts;
  for (CardRef ref : remaining) actualCounts[(int)data->state.getCard(ref).cardId]++;
  for (int i = 0; i < handCount; ++i) requestedCounts[handIds[i]]++;
  for (int i = 0; i < deckCount; ++i) requestedCounts[deckIds[i]]++;
  if (actualCounts != requestedCounts) return 3;
  CardList hand;
  CardList deck;
  auto appendById = [&](CardList& destination, int cardId, AreaType area) {
    int found = -1;
    for (int inputIndex = 0; inputIndex < remaining.size(); ++inputIndex) {
      if ((int)data->state.getCard(remaining[inputIndex]).cardId == cardId) {
        found = inputIndex; break;
      }
    }
    if (found < 0) return false;
    CardRef ref = remaining.take(found);
    data->state.cardMoved(ref, area);
    destination.push_back(ref);
    return true;
  };
  for (int i = 0; i < handCount; ++i)
    if (!appendById(hand, handIds[i], AreaType::Hand)) return 4;
  for (int i = 0; i < deckCount; ++i)
    if (!appendById(deck, deckIds[i], AreaType::Deck)) return 5;
  if (!remaining.empty()) return 6;
  player.hand = hand;
  player.deck = deck;
  return 0;
}

extern "C" GAME_API void ExactReplayTraceEnd(ApiData* data) {
  if (data == nullptr) return;
  data->exactReplayTraceEnabled = false; data->game.config.pauseAtExactTurnLeaf = false;
  data->exactReplayTurnLeaves.clear(); data->exactReplayLastTurn = -1;
}

extern "C" GAME_API const char8_t* ExactReplayTraceDrain(ApiData* data) {
  if (data == nullptr || data->apiDataType != 1) return nullptr;
  JsonBuilder& j = data->jsonBuilder; j.clear(); j.append('[');
  for (int sampleIndex = 0; sampleIndex < (int)data->exactReplayTurnLeaves.size(); ++sampleIndex) {
    j.comma(sampleIndex);
    State& state = data->exactReplayTurnLeaves[sampleIndex].first; state.game = &data->game;
    int actor = data->exactReplayTurnLeaves[sampleIndex].second;
    std::unordered_map<int, int> profile;
    for (int id : data->game.config.decks[actor].cards) profile[id]++;
    auto features = ExactSparseEvaluatorV3::extractFeatures(state, actor, &profile);
    std::string featureBytes((const char*)features.globalDense.data(), sizeof(features.globalDense));
    featureBytes.append((const char*)features.globalSparse.values.data(), features.globalSparse.count * sizeof(features.globalSparse.values[0]));
    for (int entity = 0; entity < features.entityCount; ++entity) {
      featureBytes.append((const char*)&features.entities[entity].pool, sizeof(features.entities[entity].pool));
      featureBytes.append((const char*)features.entities[entity].dense.data(), sizeof(features.entities[entity].dense));
      featureBytes.append((const char*)features.entities[entity].sparse.values.data(),
        features.entities[entity].sparse.count * sizeof(features.entities[entity].sparse.values[0]));
    }
    unsigned long long lo = ExactSipHash24(featureBytes, 0x4b4e4f574c454447ULL, 0x4553544154454b45ULL);
    unsigned long long hi = ExactSipHash24(featureBytes, 0x494e464f524d4154ULL, 0x494f4e5345545632ULL);
    std::ostringstream key; key << std::hex << std::setfill('0') << std::setw(16) << hi << std::setw(16) << lo;
    j.append('{'); j.appendKeyValue("turn", state.turn); j.appendCommaKeyValue("actor", actor);
    std::string keyText = key.str();
    j.appendCommaKey("informationStateKey"); j.appendDoubleQuote(keyText.c_str());
    j.appendCommaKeyValue("featureSchemaVersion", 3);
    j.appendCommaKey("globalDense"); j.append('[');
    for (int i = 0; i < (int)features.globalDense.size(); ++i) { j.comma(i); j.append(features.globalDense[i]); }
    j.append(']'); j.appendCommaKey("globalSparse"); j.append('[');
    for (int i = 0; i < features.globalSparse.count; ++i) {
      const auto& item = features.globalSparse.values[i];
      j.comma(i); j.append('['); j.append((int)item.relation); j.append(',');
      j.append(item.token); j.append(','); j.append(item.value); j.append(']');
    }
    j.append(']'); j.appendCommaKey("entities"); j.append('[');
    for (int entity = 0; entity < features.entityCount; ++entity) {
      const auto& item = features.entities[entity]; j.comma(entity); j.append('{');
      j.appendKeyValue("pool", item.pool); j.appendCommaKey("dense"); j.append('[');
      for (int d = 0; d < (int)item.dense.size(); ++d) { j.comma(d); j.append(item.dense[d]); }
      j.append(']'); j.appendCommaKey("sparse"); j.append('[');
      for (int s = 0; s < item.sparse.count; ++s) { const auto& token = item.sparse.values[s];
        j.comma(s); j.append('['); j.append((int)token.relation); j.append(','); j.append(token.token); j.append(','); j.append(token.value); j.append(']'); }
      j.append(']'); j.append('}');
    }
    j.append(']'); j.appendCommaKeyValue("opponentInferenceVersion", 0);
    j.appendCommaKeyValue("overflow", features.overflow); j.append('}');
  }
  j.append(']'); data->exactReplayTurnLeaves.clear(); return j.buf.c_str();
}

static const char8_t* JsonResult(ApiData* data, const SearchInfo& si) {
  SearchReturnJson(data->jsonBuilder, si);
  return data->jsonBuilder.buf.c_str();
}

static void CopyIdPtr(int* src, std::vector<int>& dest, int count, int& error) {
  if (error) {
    return;
  }
  dest.resize(count);
  for (int i : range(count)) {
    dest[i] = src[i];
    if (!CardTable.contains(dest[i])) {
      error = 1;
      break;
    }
  }
}

static void AppendLongLong(JsonBuilder& j, long long value) {
  std::string text = std::to_string(value);
  for (char c : text) j.append(c);
}

static void AppendUnsignedLongLong(JsonBuilder& j, unsigned long long value) {
  std::string text = std::to_string(value);
  for (char c : text) j.append(c);
}

static void AppendExactNumerator(JsonBuilder& j, const ExactFraction& value) {
  for (char c : value.numeratorText()) j.append(c);
}
static void AppendExactDenominator(JsonBuilder& j, const ExactFraction& value) {
  for (char c : value.denominatorText()) j.append(c);
}

static const char8_t* ExactDecisionJson(ApiData* data, const ExactDecision& decision, long long sessionId = -1) {
  JsonBuilder& j = data->jsonBuilder;
  j.clear(); j.append('{');
  j.appendKey("selected"); j.append('[');
  for (int i : range(decision.score.action.size())) { j.comma(i); j.append(decision.score.action[i]); }
  j.append(']');
  j.appendCommaKey("lowerNumerator"); AppendExactNumerator(j, decision.score.lower);
  j.appendCommaKey("lowerDenominator"); AppendExactDenominator(j, decision.score.lower);
  j.appendCommaKey("upperNumerator"); AppendExactNumerator(j, decision.score.upper);
  j.appendCommaKey("upperDenominator"); AppendExactDenominator(j, decision.score.upper);
  j.appendCommaKeyValue("certified", decision.score.certified);
  j.appendCommaKeyValue("actionValueCertified", decision.actionValueCertified);
  j.appendCommaKeyValue("bestActionCertified", decision.bestActionCertified);
  j.appendCommaKeyValue("exactValueCertified", decision.exactValueCertified);
  j.appendCommaKey("certificationScope");
  j.appendDoubleQuote(ExactSkeleton::CertScopeName(decision.metrics.certificationScope));
  j.appendCommaKeyValue("probabilityExact", decision.metrics.probabilityExact);
  j.appendCommaKeyValue("informationSetSafe", decision.metrics.informationSetSafe);
  j.appendCommaKeyValue("evaluatorApproximate", true);
  j.appendCommaKeyValue("transitionSufficientKey", true);
  j.appendCommaKeyValue("evaluatorProjectionLossy", true);
	 j.appendCommaKeyValue("runtimeVersion", decision.metrics.runtimeVersion);
	 j.appendCommaKeyValue("canonicalSchemaVersion", decision.metrics.canonicalSchemaVersion);
  j.appendCommaKeyValue("beliefScale", ExactSparseEvaluatorV3::BeliefScale);
  j.appendCommaKeyValue("evaluatorSchemaVersion", data->exactEvaluator ? data->exactEvaluator->schemaVersion() : 0);
  j.appendCommaKey("evaluatorModelHash"); AppendUnsignedLongLong(j, data->exactEvaluator ? data->exactEvaluator->modelHash() : 0);
  j.appendCommaKey("expandedNodes"); AppendUnsignedLongLong(j, decision.metrics.expanded);
	 j.appendCommaKey("stateCopies"); AppendUnsignedLongLong(j, decision.metrics.stateCopies);
	 j.appendCommaKey("stateCopyBytes"); AppendUnsignedLongLong(j, decision.metrics.stateCopyBytes);
	 j.appendCommaKey("stateCopySampleNs"); AppendUnsignedLongLong(j, decision.metrics.stateCopySampleNs);
	 j.appendCommaKey("canonicalBuilds"); AppendUnsignedLongLong(j, decision.metrics.canonicalBuilds);
	 j.appendCommaKey("canonicalBytes"); AppendUnsignedLongLong(j, decision.metrics.canonicalBytes);
	 j.appendCommaKey("canonicalSampleNs"); AppendUnsignedLongLong(j, decision.metrics.canonicalSampleNs);
	 j.appendCommaKey("ttReadHits"); AppendUnsignedLongLong(j, decision.metrics.ttReadHits);
	 j.appendCommaKey("ttReadMisses"); AppendUnsignedLongLong(j, decision.metrics.ttReadMisses);
	 j.appendCommaKey("ttReadSampleNs"); AppendUnsignedLongLong(j, decision.metrics.ttReadSampleNs);
	 j.appendCommaKey("ttInsertions"); AppendUnsignedLongLong(j, decision.metrics.ttInsertions);
	 j.appendCommaKey("transitionCacheHits"); AppendUnsignedLongLong(j, decision.metrics.transitionCacheHits);
	 j.appendCommaKey("arenaBytes"); AppendUnsignedLongLong(j, decision.metrics.arenaBytes);
	 j.appendCommaKey("heapAllocations"); AppendUnsignedLongLong(j, decision.metrics.heapAllocations);
	 j.appendCommaKey("statePoolReuses"); AppendUnsignedLongLong(j, decision.metrics.statePoolReuses);
	 j.appendCommaKey("engineStepCalls"); AppendUnsignedLongLong(j, decision.metrics.engineStepCalls);
	 j.appendCommaKey("engineStepSampleNs"); AppendUnsignedLongLong(j, decision.metrics.engineStepSampleNs);
	 j.appendCommaKey("actionApplyCalls"); AppendUnsignedLongLong(j, decision.metrics.actionApplyCalls);
	 j.appendCommaKey("actionApplySampleNs"); AppendUnsignedLongLong(j, decision.metrics.actionApplySampleNs);
	 j.appendCommaKey("actionKeyCalls"); AppendUnsignedLongLong(j, decision.metrics.actionKeyCalls);
	 j.appendCommaKey("actionKeySampleNs"); AppendUnsignedLongLong(j, decision.metrics.actionKeySampleNs);
	 j.appendCommaKey("partitionKeyCalls"); AppendUnsignedLongLong(j, decision.metrics.partitionKeyCalls);
	 j.appendCommaKey("partitionKeySampleNs"); AppendUnsignedLongLong(j, decision.metrics.partitionKeySampleNs);
	 j.appendCommaKey("observationKeyCalls"); AppendUnsignedLongLong(j, decision.metrics.observationKeyCalls);
	 j.appendCommaKey("observationKeySampleNs"); AppendUnsignedLongLong(j, decision.metrics.observationKeySampleNs);
	 j.appendCommaKey("evaluatorCacheHits"); AppendUnsignedLongLong(j, decision.metrics.evaluatorCacheHits);
	 j.appendCommaKey("evaluatorCalls"); AppendUnsignedLongLong(j, decision.metrics.evaluatorCalls);
	 j.appendCommaKey("evaluatorSampleNs"); AppendUnsignedLongLong(j, decision.metrics.evaluatorSampleNs);
	 j.appendCommaKey("evaluatorExtractSampleNs"); AppendUnsignedLongLong(j, decision.metrics.evaluatorExtractSampleNs);
	 j.appendCommaKey("evaluatorInferenceSampleNs"); AppendUnsignedLongLong(j, decision.metrics.evaluatorInferenceSampleNs);
	 j.appendCommaKey("evaluatorPublicSampleNs"); AppendUnsignedLongLong(j, decision.metrics.evaluatorPublicSampleNs);
	 j.appendCommaKey("evaluatorHiddenSampleNs"); AppendUnsignedLongLong(j, decision.metrics.evaluatorHiddenSampleNs);
	 j.appendCommaKey("evaluatorEntitySampleNs"); AppendUnsignedLongLong(j, decision.metrics.evaluatorEntitySampleNs);
	 j.appendCommaKey("workerBusyNs"); AppendUnsignedLongLong(j, decision.metrics.workerBusyNs);
	 j.appendCommaKey("workerWaitNs"); AppendUnsignedLongLong(j, decision.metrics.workerWaitNs);
	 j.appendCommaKey("legacyShadowMismatches"); AppendUnsignedLongLong(j, decision.metrics.legacyShadowMismatches);
	 j.appendCommaKey("packedObservationBuilds"); AppendUnsignedLongLong(j, decision.metrics.packedObservationBuilds);
	 j.appendCommaKey("packedObservationBytes"); AppendUnsignedLongLong(j, decision.metrics.packedObservationBytes);
	 j.appendCommaKey("keyArenaBytes"); AppendUnsignedLongLong(j, decision.metrics.keyArenaBytes);
	 j.appendCommaKey("cowFullCopies"); AppendUnsignedLongLong(j, decision.metrics.cowFullCopies);
	 j.appendCommaKey("cowPageCopies"); AppendUnsignedLongLong(j, decision.metrics.cowPageCopies);
	 j.appendCommaKey("cowCopyBytes"); AppendUnsignedLongLong(j, decision.metrics.cowCopyBytes);
	 j.appendCommaKey("mutationMisses"); AppendUnsignedLongLong(j, decision.metrics.mutationMisses);
	 j.appendCommaKey("materializedSnapshots"); AppendUnsignedLongLong(j, decision.metrics.materializedSnapshots);
	 j.appendCommaKey("inFlightWaits"); AppendUnsignedLongLong(j, decision.metrics.inFlightWaits);
	 j.appendCommaKey("workerDuplicateClaims"); AppendUnsignedLongLong(j, decision.metrics.workerDuplicateClaims);
	 j.appendCommaKey("exactWeightInlineOps"); AppendUnsignedLongLong(j, decision.metrics.exactWeightInlineOps);
	 j.appendCommaKey("exactWeightSpills"); AppendUnsignedLongLong(j, decision.metrics.exactWeightSpills);
	 j.appendCommaKey("evaluatorAccumulatorHits"); AppendUnsignedLongLong(j, decision.metrics.evaluatorAccumulatorHits);
  j.appendCommaKey("mergedNodes"); AppendUnsignedLongLong(j, decision.metrics.merged);
  j.appendCommaKeyValue("timedOut", decision.metrics.timedOut);
  j.appendCommaKeyValue("arithmeticOverflow", decision.metrics.arithmeticOverflow);
  j.appendCommaKey("leafNodes"); AppendUnsignedLongLong(j, decision.metrics.leaves);
  j.appendCommaKey("opaqueNodes"); AppendUnsignedLongLong(j, decision.metrics.opaque);
  j.appendCommaKey("exceptionNodes"); AppendUnsignedLongLong(j, decision.metrics.exceptions);
  j.appendCommaKey("lastException");
  j.appendDoubleQuote(std::u8string((const char8_t*)decision.metrics.lastException.c_str(), decision.metrics.lastException.size()));
  j.appendCommaKeyValue("lastPendingDetail", decision.metrics.lastPendingDetail);
  j.appendCommaKeyValue("lastPendingPlayer", decision.metrics.lastPendingPlayer);
  j.appendCommaKeyValue("lastPendingEffectCardId", decision.metrics.lastPendingEffectCardId);
  j.appendCommaKeyValue("lastPendingEffectPlayer", decision.metrics.lastPendingEffectPlayer);
  j.appendCommaKeyValue("lastPendingNullCount", decision.metrics.lastPendingNullCount);
  j.appendCommaKeyValue("lastPendingDeckUnknown", decision.metrics.lastPendingDeckUnknown);
  j.appendCommaKey("unknownOpponentListNodes"); AppendUnsignedLongLong(j, decision.metrics.unknownOpponentList);
  j.appendCommaKey("unsupportedConcreteReferenceNodes"); AppendUnsignedLongLong(j, decision.metrics.unsupportedConcreteReference);
  j.appendCommaKey("interruptedTransitionNodes"); AppendUnsignedLongLong(j, decision.metrics.interruptedTransition);
  j.appendCommaKey("rawOutcomes"); AppendUnsignedLongLong(j, decision.metrics.rawOutcomes);
  j.appendCommaKey("groupedOutcomes"); AppendUnsignedLongLong(j, decision.metrics.groupedOutcomes);
  j.appendCommaKey("depthLimitNodes"); AppendUnsignedLongLong(j, decision.metrics.depthLimitNodes);
  j.appendCommaKeyValue("maxDepth", decision.metrics.maxDepth);
  j.appendCommaKeyValue("lastDepthSelectType", decision.metrics.lastDepthSelectType);
  j.appendCommaKeyValue("lastDepthTurnActionCount", decision.metrics.lastDepthTurnActionCount);
  j.appendCommaKeyValue("rootWorkers", decision.metrics.rootWorkers);
  j.appendCommaKeyValue("maxConcurrentSearchThreads", ExactGlobalThreadBudget().peak());
  j.appendCommaKey("rootQueueLeases"); AppendUnsignedLongLong(j, decision.metrics.rootQueueLeases);
  j.appendCommaKey("rootQueueReassignments"); AppendUnsignedLongLong(j, decision.metrics.rootQueueReassignments);
  j.appendCommaKey("rootQueueSteals"); AppendUnsignedLongLong(j, decision.metrics.rootQueueSteals);
  j.appendCommaKey("rootQueueEliminations"); AppendUnsignedLongLong(j, decision.metrics.rootQueueEliminations);
  j.appendCommaKey("policyNodes"); AppendUnsignedLongLong(j, decision.metrics.policyNodes);
  j.appendCommaKey("policyHits"); AppendUnsignedLongLong(j, decision.metrics.policyHits);
  j.appendCommaKey("policyMisses"); AppendUnsignedLongLong(j, decision.metrics.policyMisses);
  j.appendCommaKey("rerootCount"); AppendUnsignedLongLong(j, decision.metrics.rerootCount);
  j.appendCommaKey("conditionedWorldsRemoved"); AppendUnsignedLongLong(j, 0);
  j.appendCommaKey("conditionedMass"); AppendUnsignedLongLong(j, 0);
  j.appendCommaKey("resumedNodes"); AppendUnsignedLongLong(j, decision.metrics.resumedNodes);
  j.appendCommaKey("avoidedExpandedNodes"); AppendUnsignedLongLong(j, decision.metrics.avoidedExpandedNodes);
  j.appendCommaKey("partialDecisionNodes"); AppendUnsignedLongLong(j, decision.metrics.partialDecisionNodes);
  j.appendCommaKey("partialChanceNodes"); AppendUnsignedLongLong(j, decision.metrics.partialChanceNodes);
  j.appendCommaKey("semanticActionRemaps"); AppendUnsignedLongLong(j, decision.metrics.semanticActionRemaps);
  j.appendCommaKey("sessionInvalidations"); AppendUnsignedLongLong(j, decision.metrics.sessionInvalidations);
  j.appendCommaKey("sessionBytes"); AppendUnsignedLongLong(j, decision.metrics.sessionBytes);
  j.appendCommaKey("deadlineOverrunMs"); AppendLongLong(j, decision.metrics.deadlineOverrunMs);
	 j.appendCommaKey("canonicalStateMerges"); AppendUnsignedLongLong(j, decision.metrics.canonicalStateMerges);
	 j.appendCommaKey("successorMerges"); AppendUnsignedLongLong(j, decision.metrics.successorMerges);
	 j.appendCommaKey("distributionMerges"); AppendUnsignedLongLong(j, decision.metrics.distributionMerges);
	 j.appendCommaKey("rootSharedTTHits"); AppendUnsignedLongLong(j, decision.metrics.rootSharedTTHits);
	 j.appendCommaKey("beliefWorldsBefore"); AppendUnsignedLongLong(j, decision.metrics.beliefWorldsBefore);
	 j.appendCommaKey("beliefWorldsAfter"); AppendUnsignedLongLong(j, decision.metrics.beliefWorldsAfter);
	 j.appendCommaKey("largestEquivalenceClass"); AppendUnsignedLongLong(j, decision.metrics.largestEquivalenceClass);
	 j.appendCommaKey("resumedActionCount"); AppendUnsignedLongLong(j, decision.metrics.resumedActionCount);
	 j.appendCommaKey("resumedChanceMass"); AppendUnsignedLongLong(j, decision.metrics.resumedChanceMass);
	 j.appendCommaKey("partialRevealHits"); AppendUnsignedLongLong(j, decision.metrics.partialRevealHits);
	 j.appendCommaKey("enumeratedHiddenWorlds"); AppendUnsignedLongLong(j, decision.metrics.enumeratedHiddenWorlds);
	 j.appendCommaKeyValue("currentRootAction", decision.metrics.currentRootAction);
	 j.appendCommaKey("currentRssBytes"); AppendUnsignedLongLong(j, decision.metrics.currentRssBytes);
	 j.appendCommaKey("peakRssBytes"); AppendUnsignedLongLong(j, decision.metrics.peakRssBytes);
  j.appendCommaKeyValue("memoryLimitReached", decision.metrics.memoryLimitReached);
  j.appendCommaKeyValue("structurallyBlocked", decision.metrics.structurallyBlocked);
  j.appendCommaKey("searchStatus");
  j.appendDoubleQuote(decision.score.certified ? u8"certified"
    : (decision.metrics.structurallyBlocked ? u8"blocked" : u8"resumable"));
	 j.appendCommaKey("partialDecisionHits"); AppendUnsignedLongLong(j, decision.metrics.partialDecisionHits);
	 j.appendCommaKey("partialChanceHits"); AppendUnsignedLongLong(j, decision.metrics.partialChanceHits);
	 j.appendCommaKey("partialTableBytes"); AppendUnsignedLongLong(j, decision.metrics.partialTableBytes);
	 j.appendCommaKey("rootRetryKeyMatches"); AppendUnsignedLongLong(j, decision.metrics.rootRetryKeyMatches);
	 j.appendCommaKey("rootRetryKeyMismatches"); AppendUnsignedLongLong(j, decision.metrics.rootRetryKeyMismatches);
	 j.appendCommaKey("beliefNodes"); AppendUnsignedLongLong(j, decision.metrics.beliefNodes);
	 j.appendCommaKey("informationSets"); AppendUnsignedLongLong(j, decision.metrics.informationSets);
	 j.appendCommaKey("strategyFusionPrevented"); AppendUnsignedLongLong(j, decision.metrics.strategyFusionPrevented);
	 j.appendCommaKey("smallWeightOps"); AppendUnsignedLongLong(j, decision.metrics.smallWeightOps);
	 j.appendCommaKey("bigWeightPromotions"); AppendUnsignedLongLong(j, decision.metrics.bigWeightPromotions);
	 j.appendCommaKeyValue("maxWeightBits", (int)decision.metrics.maxWeightBits);
	 j.appendCommaKey("chanceMassMismatches"); AppendUnsignedLongLong(j, decision.metrics.chanceMassMismatches);
	 j.appendCommaKey("illegalInformationSetSplits"); AppendUnsignedLongLong(j, decision.metrics.illegalInformationSetSplits);
	 j.appendCommaKey("attackPreviewExactCount"); AppendUnsignedLongLong(j, decision.metrics.attackPreviewExactCount);
	 j.appendCommaKey("attackPreviewUnavailableCount"); AppendUnsignedLongLong(j, decision.metrics.attackPreviewUnavailableCount);
	 j.appendCommaKey("entityFeatureCount"); AppendUnsignedLongLong(j, decision.metrics.entityFeatureCount);
	 j.appendCommaKey("comboFeatureCount"); AppendUnsignedLongLong(j, decision.metrics.comboFeatureCount);
	 j.appendCommaKey("provisionalOpponentPolicyNodes"); AppendUnsignedLongLong(j, decision.metrics.provisionalOpponentPolicyNodes);
	 j.appendCommaKey("dynamicPartitionBuilds"); AppendUnsignedLongLong(j, decision.metrics.dynamicPartitionBuilds);
	 j.appendCommaKey("dynamicPartitionFallbacks"); AppendUnsignedLongLong(j, decision.metrics.dynamicPartitionFallbacks);
	 j.appendCommaKey("dynamicPartitionCacheHits"); AppendUnsignedLongLong(j, decision.metrics.dynamicPartitionCacheHits);
	 j.appendCommaKey("dynamicPartitionMaxClasses"); AppendUnsignedLongLong(j, decision.metrics.dynamicPartitionMaxClasses);
	 j.appendCommaKey("dynamicPartitionMaxVisibleIdentities"); AppendUnsignedLongLong(j, decision.metrics.dynamicPartitionMaxVisibleIdentities);
	 j.appendCommaKey("continuationDraws"); AppendUnsignedLongLong(j, decision.metrics.continuationDraws);
	 j.appendCommaKey("continuationDrawClasses"); AppendUnsignedLongLong(j, decision.metrics.continuationDrawClasses);
	 j.appendCommaKey("continuationClassOutcomes"); AppendUnsignedLongLong(j, decision.metrics.continuationClassOutcomes);
	 j.appendCommaKey("continuationConditionalSplits"); AppendUnsignedLongLong(j, decision.metrics.continuationConditionalSplits);
	 j.appendCommaKey("continuationPreparedOutcomes"); AppendUnsignedLongLong(j, decision.metrics.continuationPreparedOutcomes);
	 j.appendCommaKey("continuationDrawOutcomes"); AppendUnsignedLongLong(j, decision.metrics.continuationDrawOutcomes);
	 j.appendCommaKey("continuationCompletedOutcomeNodes"); AppendUnsignedLongLong(j, decision.metrics.continuationCompletedOutcomeNodes);
	 j.appendCommaKey("continuationMaxOutcomeNodes"); AppendUnsignedLongLong(j, decision.metrics.continuationMaxOutcomeNodes);
	 j.appendCommaKey("streamingCursorHits"); AppendUnsignedLongLong(j, decision.metrics.streamingCursorHits);
	 j.appendCommaKey("streamingCursorResumes"); AppendUnsignedLongLong(j, decision.metrics.streamingCursorResumes);
	 j.appendCommaKey("streamingCursorGenerated"); AppendUnsignedLongLong(j, decision.metrics.streamingCursorGenerated);
	 j.appendCommaKey("streamingCursorPeakBytes"); AppendUnsignedLongLong(j, decision.metrics.streamingCursorPeakBytes);
	j.appendCommaKey("continuationAtomsMerged"); AppendUnsignedLongLong(j, decision.metrics.continuationAtomsMerged);
	j.appendCommaKey("v4SemanticEvaluations"); AppendUnsignedLongLong(j, decision.metrics.v4SemanticEvaluations);
	j.appendCommaKey("v4PassiveEvaluations"); AppendUnsignedLongLong(j, decision.metrics.v4PassiveEvaluations);
	j.appendCommaKey("passiveCardsIntegrated"); AppendUnsignedLongLong(j, decision.metrics.passiveCardsIntegrated);
	j.appendCommaKey("passivePairTermsEvaluated"); AppendUnsignedLongLong(j, decision.metrics.passivePairTermsEvaluated);
	j.appendCommaKey("passiveExpectationCalls"); AppendUnsignedLongLong(j, decision.metrics.passiveExpectationCalls);
	j.appendCommaKey("activeCardCount"); AppendUnsignedLongLong(j, decision.metrics.activeCardCount);
	j.appendCommaKey("passiveCardCount"); AppendUnsignedLongLong(j, decision.metrics.passiveCardCount);
	j.appendCommaKey("unknownLivenessCount"); AppendUnsignedLongLong(j, decision.metrics.unknownLivenessCount);
	j.appendCommaKey("livenessFallbackCount"); AppendUnsignedLongLong(j, decision.metrics.livenessFallbackCount);
	j.appendCommaKey("richActiveOutcomeCount"); AppendUnsignedLongLong(j, decision.metrics.richActiveOutcomeCount);
	j.appendCommaKey("passiveResidualCalls"); AppendUnsignedLongLong(j, decision.metrics.passiveResidualCalls);
	j.appendCommaKey("passiveResidualElapsedNs"); AppendUnsignedLongLong(j, decision.metrics.passiveResidualElapsedNs);
	j.appendCommaKey("richPassiveIntegratedWeight"); j.appendDoubleQuote(decision.metrics.richPassiveIntegratedWeight.text().c_str());
	j.appendCommaKey("richTotalChanceWeight"); j.appendDoubleQuote(decision.metrics.richTotalChanceWeight.text().c_str());
	j.appendCommaKeyValue("v4PassiveDrawExperimental", decision.metrics.v4PassiveDrawExperimental);
	j.appendCommaKey("nestedChancePassiveFallbacks"); AppendUnsignedLongLong(j, decision.metrics.nestedChancePassiveFallbacks);
	j.appendCommaKey("representativeInvariantFallbacks"); AppendUnsignedLongLong(j, decision.metrics.representativeInvariantFallbacks);
	j.appendCommaKey("fallbackIncompletePending"); AppendUnsignedLongLong(j, decision.metrics.fallbackIncompletePending);
	j.appendCommaKey("fallbackIncompleteGlobal"); AppendUnsignedLongLong(j, decision.metrics.fallbackIncompleteGlobal);
	j.appendCommaKey("fallbackIncompleteCosts"); AppendUnsignedLongLong(j, decision.metrics.fallbackIncompleteCosts);
	j.appendCommaKey("fallbackIncompleteSelection"); AppendUnsignedLongLong(j, decision.metrics.fallbackIncompleteSelection);
	j.appendCommaKey("fallbackIncompleteConditions"); AppendUnsignedLongLong(j, decision.metrics.fallbackIncompleteConditions);
	j.appendCommaKey("fallbackSemanticInvariant"); AppendUnsignedLongLong(j, decision.metrics.fallbackSemanticInvariant);
	j.appendCommaKey("fallbackFurtherChance"); AppendUnsignedLongLong(j, decision.metrics.fallbackFurtherChance);
	j.appendCommaKey("fallbackAnalyticBound"); AppendUnsignedLongLong(j, decision.metrics.fallbackAnalyticBound);
	j.appendCommaKey("fallbackUnknownToken"); AppendUnsignedLongLong(j, decision.metrics.fallbackUnknownToken);
	j.appendCommaKey("livenessAnalysisNs"); AppendUnsignedLongLong(j, decision.metrics.livenessAnalysisNs);
	j.appendCommaKey("semanticForwardNs"); AppendUnsignedLongLong(j, decision.metrics.semanticForwardNs);
	j.appendCommaKey("passiveExpectationNs"); AppendUnsignedLongLong(j, decision.metrics.passiveExpectationNs);
	j.appendCommaKey("activeDrawEnumerationNs"); AppendUnsignedLongLong(j, decision.metrics.activeDrawEnumerationNs);
	j.appendCommaKey("activeOutcomeSolveNs"); AppendUnsignedLongLong(j, decision.metrics.activeOutcomeSolveNs);
	j.appendCommaKey("activeOutcomeSolveCount"); AppendUnsignedLongLong(j, decision.metrics.activeOutcomeSolveCount);
	{
		const unsigned long long avgSolveOwnedNsPerActiveOutcome = decision.metrics.activeOutcomeSolveCount > 0
			? decision.metrics.activeOutcomeSolveNs / decision.metrics.activeOutcomeSolveCount : 0;
		j.appendCommaKey("avgSolveOwnedNsPerActiveOutcome");
		AppendUnsignedLongLong(j, avgSolveOwnedNsPerActiveOutcome);
	}
	j.appendCommaKey("skeletonClasses"); AppendUnsignedLongLong(j, decision.metrics.skeletonClasses);
	j.appendCommaKey("skeletonClassCount"); AppendUnsignedLongLong(j, decision.metrics.skeletonClassCount);
	 j.appendCommaKey("skeletonClassMembers"); AppendUnsignedLongLong(j, decision.metrics.skeletonClassMembers);
	 j.appendCommaKey("skeletonExpansions"); AppendUnsignedLongLong(j, decision.metrics.skeletonExpansions);
	 j.appendCommaKey("skeletonSweeps"); AppendUnsignedLongLong(j, decision.metrics.skeletonSweeps);
	 j.appendCommaKey("skeletonGuardFallbacks"); AppendUnsignedLongLong(j, decision.metrics.skeletonGuardFallbacks);
	 j.appendCommaKey("skeletonGuardNodeCap"); AppendUnsignedLongLong(j, decision.metrics.skeletonGuardNodeCap);
	 j.appendCommaKey("skeletonGuardInertLegal"); AppendUnsignedLongLong(j, decision.metrics.skeletonGuardInertLegal);
	 j.appendCommaKey("skeletonGuardInteriorChance"); AppendUnsignedLongLong(j, decision.metrics.skeletonGuardInteriorChance);
	 j.appendCommaKey("skeletonGuardIncomplete"); AppendUnsignedLongLong(j, decision.metrics.skeletonGuardIncomplete);
	 j.appendCommaKey("skeletonGuardOther"); AppendUnsignedLongLong(j, decision.metrics.skeletonGuardOther);
	 j.appendCommaKey("skeletonNodes"); AppendUnsignedLongLong(j, decision.metrics.skeletonNodes);
	 j.appendCommaKey("skeletonInteriorChances"); AppendUnsignedLongLong(j, decision.metrics.skeletonInteriorChances);
	 j.appendCommaKey("turnInertIdentities"); AppendUnsignedLongLong(j, decision.metrics.turnInertIdentities);
	 j.appendCommaKey("macroCollapsedTransitions"); AppendUnsignedLongLong(j, decision.metrics.macroCollapsedTransitions);
	 j.appendCommaKey("sleepSetPrunes"); AppendUnsignedLongLong(j, decision.metrics.sleepSetPrunes);
	 j.appendCommaKey("argmaxDominatedCuts"); AppendUnsignedLongLong(j, decision.metrics.argmaxDominatedCuts);
	 j.appendCommaKeyValue("dynamicPartitionFallbackCardId", decision.metrics.dynamicPartitionFallbackCardId);
	 j.appendCommaKeyValue("dynamicPartitionFallbackEffectType", decision.metrics.dynamicPartitionFallbackEffectType);
	 j.appendCommaKeyValue("dynamicPartitionFallbackTargetType", decision.metrics.dynamicPartitionFallbackTargetType);
	 j.appendCommaKeyValue("provisionalOpponentPolicy", decision.metrics.provisionalOpponentPolicyNodes > 0);
	 j.appendCommaKeyValue("opponentPolicyOptimal", decision.metrics.provisionalOpponentPolicyNodes == 0);
	 j.appendCommaKeyValue("hiddenInformationLeakDetected", decision.metrics.hiddenInformationLeakDetected);
	 j.appendCommaKey("rootActions"); j.append('[');
	 for (int ri : range(decision.rootActions)) {
	   j.comma(ri); j.append('{');
	   j.appendKey("selected"); j.append('[');
	   for (int ai : range(decision.rootActions[ri].action)) { j.comma(ai); j.append(decision.rootActions[ri].action[ai]); }
	   j.append(']');
	   j.appendCommaKey("lowerNumerator"); AppendExactNumerator(j, decision.rootActions[ri].lower);
	   j.appendCommaKey("lowerDenominator"); AppendExactDenominator(j, decision.rootActions[ri].lower);
	   j.appendCommaKey("upperNumerator"); AppendExactNumerator(j, decision.rootActions[ri].upper);
	   j.appendCommaKey("upperDenominator"); AppendExactDenominator(j, decision.rootActions[ri].upper);
	   j.appendCommaKeyValue("certified", decision.rootActions[ri].certified); j.append('}');
	 }
	 j.append(']');
  if (sessionId >= 0) { j.appendCommaKey("sessionId"); AppendLongLong(j, sessionId); }
  j.append('}');
  return j.buf.c_str();
}

static void MergeExactMetrics(ExactMetrics& into, const ExactMetrics& from) {
	into.stateCopies += from.stateCopies;
	into.stateCopyBytes += from.stateCopyBytes;
	into.stateCopySampleNs += from.stateCopySampleNs;
	into.canonicalBuilds += from.canonicalBuilds;
	into.canonicalBytes += from.canonicalBytes;
	into.canonicalSampleNs += from.canonicalSampleNs;
	into.ttReadHits += from.ttReadHits;
	into.ttReadMisses += from.ttReadMisses;
	into.ttReadSampleNs += from.ttReadSampleNs;
	into.ttInsertions += from.ttInsertions;
	into.transitionCacheHits += from.transitionCacheHits;
	into.arenaBytes = std::max(into.arenaBytes, from.arenaBytes);
	into.heapAllocations += from.heapAllocations;
	into.statePoolReuses += from.statePoolReuses;
	into.engineStepCalls += from.engineStepCalls;
	into.engineStepSampleNs += from.engineStepSampleNs;
	into.actionApplyCalls += from.actionApplyCalls;
	into.actionApplySampleNs += from.actionApplySampleNs;
	into.actionKeyCalls += from.actionKeyCalls;
	into.actionKeySampleNs += from.actionKeySampleNs;
	into.partitionKeyCalls += from.partitionKeyCalls;
	into.partitionKeySampleNs += from.partitionKeySampleNs;
	into.observationKeyCalls += from.observationKeyCalls;
	into.observationKeySampleNs += from.observationKeySampleNs;
	into.evaluatorCacheHits += from.evaluatorCacheHits;
	into.evaluatorCalls += from.evaluatorCalls;
	into.evaluatorSampleNs += from.evaluatorSampleNs;
	into.evaluatorExtractSampleNs += from.evaluatorExtractSampleNs;
	into.evaluatorInferenceSampleNs += from.evaluatorInferenceSampleNs;
	into.evaluatorPublicSampleNs += from.evaluatorPublicSampleNs;
	into.evaluatorHiddenSampleNs += from.evaluatorHiddenSampleNs;
	into.evaluatorEntitySampleNs += from.evaluatorEntitySampleNs;
	into.workerBusyNs += from.workerBusyNs;
	into.workerWaitNs += from.workerWaitNs;
	into.legacyShadowMismatches += from.legacyShadowMismatches;
	into.packedObservationBuilds += from.packedObservationBuilds;
	into.packedObservationBytes += from.packedObservationBytes;
	into.keyArenaBytes += from.keyArenaBytes;
	into.cowFullCopies += from.cowFullCopies;
	into.cowPageCopies += from.cowPageCopies;
	into.cowCopyBytes += from.cowCopyBytes;
	into.mutationMisses += from.mutationMisses;
	into.materializedSnapshots += from.materializedSnapshots;
	into.inFlightWaits += from.inFlightWaits;
	into.workerDuplicateClaims += from.workerDuplicateClaims;
	into.exactWeightInlineOps += from.exactWeightInlineOps;
	into.exactWeightSpills += from.exactWeightSpills;
	into.evaluatorAccumulatorHits += from.evaluatorAccumulatorHits;
  into.runtimeVersion = std::max(into.runtimeVersion, from.runtimeVersion);
  into.canonicalSchemaVersion = std::max(into.canonicalSchemaVersion, from.canonicalSchemaVersion);
  into.rootQueueLeases += from.rootQueueLeases;
  into.rootQueueReassignments += from.rootQueueReassignments;
  into.rootQueueSteals += from.rootQueueSteals;
  into.rootQueueEliminations += from.rootQueueEliminations;
  into.expanded += from.expanded; into.merged += from.merged; into.leaves += from.leaves;
  into.opaque += from.opaque; into.exceptions += from.exceptions;
  into.unknownOpponentList += from.unknownOpponentList;
  into.unsupportedConcreteReference += from.unsupportedConcreteReference;
  into.interruptedTransition += from.interruptedTransition;
  into.rawOutcomes += from.rawOutcomes; into.groupedOutcomes += from.groupedOutcomes;
  into.depthLimitNodes += from.depthLimitNodes; into.maxDepth = std::max(into.maxDepth, from.maxDepth);
  into.timedOut = into.timedOut || from.timedOut;
  into.arithmeticOverflow = into.arithmeticOverflow || from.arithmeticOverflow;
  into.policyNodes += from.policyNodes; into.policyHits += from.policyHits;
  into.policyMisses += from.policyMisses; into.rerootCount += from.rerootCount;
  into.resumedNodes += from.resumedNodes; into.avoidedExpandedNodes += from.avoidedExpandedNodes;
  into.partialDecisionNodes += from.partialDecisionNodes; into.partialChanceNodes += from.partialChanceNodes;
  into.semanticActionRemaps += from.semanticActionRemaps;
  into.sessionInvalidations += from.sessionInvalidations;
  into.sessionBytes += from.sessionBytes;
  into.deadlineOverrunMs = std::max(into.deadlineOverrunMs, from.deadlineOverrunMs);
	into.canonicalStateMerges += from.canonicalStateMerges;
	into.successorMerges += from.successorMerges;
	into.distributionMerges += from.distributionMerges;
	into.rootSharedTTHits += from.rootSharedTTHits;
	into.beliefWorldsBefore += from.beliefWorldsBefore;
	into.beliefWorldsAfter += from.beliefWorldsAfter;
	into.largestEquivalenceClass = std::max(into.largestEquivalenceClass, from.largestEquivalenceClass);
	into.resumedActionCount += from.resumedActionCount;
	into.resumedChanceMass += from.resumedChanceMass;
	into.partialRevealHits += from.partialRevealHits;
	into.enumeratedHiddenWorlds += from.enumeratedHiddenWorlds;
	if (from.currentRootAction >= 0) into.currentRootAction = from.currentRootAction;
	into.currentRssBytes = std::max(into.currentRssBytes, from.currentRssBytes);
	into.peakRssBytes = std::max(into.peakRssBytes, from.peakRssBytes);
  into.memoryLimitReached = into.memoryLimitReached || from.memoryLimitReached;
  into.structurallyBlocked = into.structurallyBlocked || from.structurallyBlocked;
	into.partialDecisionHits += from.partialDecisionHits;
	into.partialChanceHits += from.partialChanceHits;
	into.partialTableBytes += from.partialTableBytes;
	into.rootRetryKeyMatches += from.rootRetryKeyMatches;
	into.rootRetryKeyMismatches += from.rootRetryKeyMismatches;
	into.smallWeightOps += from.smallWeightOps; into.bigWeightPromotions += from.bigWeightPromotions;
	into.maxWeightBits = std::max(into.maxWeightBits, from.maxWeightBits);
	into.chanceMassMismatches += from.chanceMassMismatches;
	into.beliefNodes += from.beliefNodes; into.informationSets += from.informationSets;
	into.strategyFusionPrevented += from.strategyFusionPrevented;
	into.illegalInformationSetSplits += from.illegalInformationSetSplits;
	into.attackPreviewExactCount += from.attackPreviewExactCount;
	into.attackPreviewUnavailableCount += from.attackPreviewUnavailableCount;
	into.entityFeatureCount += from.entityFeatureCount; into.comboFeatureCount += from.comboFeatureCount;
	into.provisionalOpponentPolicyNodes += from.provisionalOpponentPolicyNodes;
	into.dynamicPartitionBuilds += from.dynamicPartitionBuilds;
	into.dynamicPartitionFallbacks += from.dynamicPartitionFallbacks;
	into.dynamicPartitionCacheHits += from.dynamicPartitionCacheHits;
	into.dynamicPartitionMaxClasses = std::max(into.dynamicPartitionMaxClasses, from.dynamicPartitionMaxClasses);
	into.dynamicPartitionMaxVisibleIdentities = std::max(into.dynamicPartitionMaxVisibleIdentities,
		from.dynamicPartitionMaxVisibleIdentities);
	into.continuationDraws += from.continuationDraws;
	into.continuationDrawClasses += from.continuationDrawClasses;
	into.continuationClassOutcomes += from.continuationClassOutcomes;
	into.continuationConditionalSplits += from.continuationConditionalSplits;
	into.continuationPreparedOutcomes += from.continuationPreparedOutcomes;
	into.continuationDrawOutcomes += from.continuationDrawOutcomes;
	into.continuationCompletedOutcomeNodes += from.continuationCompletedOutcomeNodes;
	into.continuationMaxOutcomeNodes = std::max(into.continuationMaxOutcomeNodes, from.continuationMaxOutcomeNodes);
	into.streamingCursorHits += from.streamingCursorHits;
	into.streamingCursorResumes += from.streamingCursorResumes;
	into.streamingCursorGenerated += from.streamingCursorGenerated;
	into.streamingCursorPeakBytes = std::max(into.streamingCursorPeakBytes, from.streamingCursorPeakBytes);
	into.continuationAtomsMerged += from.continuationAtomsMerged;
	into.v4SemanticEvaluations += from.v4SemanticEvaluations;
	into.v4PassiveEvaluations += from.v4PassiveEvaluations;
	into.passiveCardsIntegrated += from.passiveCardsIntegrated;
	into.passivePairTermsEvaluated += from.passivePairTermsEvaluated;
	into.passiveExpectationCalls += from.passiveExpectationCalls;
	into.activeCardCount += from.activeCardCount;
	into.passiveCardCount += from.passiveCardCount;
	into.unknownLivenessCount += from.unknownLivenessCount;
	into.livenessFallbackCount += from.livenessFallbackCount;
	into.richActiveOutcomeCount += from.richActiveOutcomeCount;
	into.passiveResidualCalls += from.passiveResidualCalls;
	into.passiveResidualElapsedNs += from.passiveResidualElapsedNs;
	into.richPassiveIntegratedWeight += from.richPassiveIntegratedWeight;
	into.richTotalChanceWeight += from.richTotalChanceWeight;
	into.v4PassiveDrawExperimental = into.v4PassiveDrawExperimental || from.v4PassiveDrawExperimental;
	into.nestedChancePassiveFallbacks += from.nestedChancePassiveFallbacks;
	into.representativeInvariantFallbacks += from.representativeInvariantFallbacks;
	into.fallbackIncompletePending += from.fallbackIncompletePending;
	into.fallbackIncompleteGlobal += from.fallbackIncompleteGlobal;
	into.fallbackIncompleteCosts += from.fallbackIncompleteCosts;
	into.fallbackIncompleteSelection += from.fallbackIncompleteSelection;
	into.fallbackIncompleteConditions += from.fallbackIncompleteConditions;
	into.fallbackSemanticInvariant += from.fallbackSemanticInvariant;
	into.fallbackFurtherChance += from.fallbackFurtherChance;
	into.fallbackAnalyticBound += from.fallbackAnalyticBound;
	into.fallbackUnknownToken += from.fallbackUnknownToken;
	into.livenessAnalysisNs += from.livenessAnalysisNs;
	into.semanticForwardNs += from.semanticForwardNs;
	into.passiveExpectationNs += from.passiveExpectationNs;
	into.activeDrawEnumerationNs += from.activeDrawEnumerationNs;
	into.activeOutcomeSolveNs += from.activeOutcomeSolveNs;
	into.activeOutcomeSolveCount += from.activeOutcomeSolveCount;
	into.skeletonClasses += from.skeletonClasses;
	into.skeletonClassCount += from.skeletonClassCount;
	into.skeletonClassMembers += from.skeletonClassMembers;
	into.skeletonExpansions += from.skeletonExpansions;
	into.skeletonSweeps += from.skeletonSweeps;
	into.skeletonGuardFallbacks += from.skeletonGuardFallbacks;
	into.skeletonGuardNodeCap += from.skeletonGuardNodeCap;
	into.skeletonGuardInertLegal += from.skeletonGuardInertLegal;
	into.skeletonGuardInteriorChance += from.skeletonGuardInteriorChance;
	into.skeletonGuardIncomplete += from.skeletonGuardIncomplete;
	into.skeletonGuardOther += from.skeletonGuardOther;
	into.avgSolveOwnedNsPerActiveOutcome = into.activeOutcomeSolveCount > 0
		? into.activeOutcomeSolveNs / into.activeOutcomeSolveCount : 0;
	into.skeletonNodes += from.skeletonNodes;
	into.skeletonInteriorChances += from.skeletonInteriorChances;
	into.turnInertIdentities = std::max(into.turnInertIdentities, from.turnInertIdentities);
	into.macroCollapsedTransitions += from.macroCollapsedTransitions;
	into.sleepSetPrunes += from.sleepSetPrunes;
	into.argmaxDominatedCuts += from.argmaxDominatedCuts;
	if (from.certificationScope == ExactSkeleton::CertScope::Argmax)
		into.certificationScope = ExactSkeleton::CertScope::Argmax;
	if (from.dynamicPartitionFallbackCardId != 0) {
		into.dynamicPartitionFallbackCardId = from.dynamicPartitionFallbackCardId;
		into.dynamicPartitionFallbackEffectType = from.dynamicPartitionFallbackEffectType;
		into.dynamicPartitionFallbackTargetType = from.dynamicPartitionFallbackTargetType;
	}
	into.hiddenInformationLeakDetected = into.hiddenInformationLeakDetected || from.hiddenInformationLeakDetected;
	into.probabilityExact = into.probabilityExact && from.probabilityExact;
	into.informationSetSafe = into.informationSetSafe && from.informationSetSafe;
  if (!from.lastException.empty()) into.lastException = from.lastException;
  if (from.lastPendingDetail != 0) into.lastPendingDetail = from.lastPendingDetail;
  if (from.lastPendingPlayer >= 0) into.lastPendingPlayer = from.lastPendingPlayer;
  if (from.lastPendingEffectCardId != 0) into.lastPendingEffectCardId = from.lastPendingEffectCardId;
  if (from.lastPendingEffectPlayer >= 0) into.lastPendingEffectPlayer = from.lastPendingEffectPlayer;
  if (from.lastPendingNullCount != 0) into.lastPendingNullCount = from.lastPendingNullCount;
  into.lastPendingDeckUnknown = into.lastPendingDeckUnknown || from.lastPendingDeckUnknown;
}

struct ExactTurnSession {
  struct Worker {
    Game game;
    std::unique_ptr<ExactPlanner> planner;
    std::vector<ExactScore> actions;
    bool argmaxCut = false;
  };

  struct RootTask {
    int option = -1;
    unsigned long long estimate = 1;
    unsigned long long consumedNodes = 0;
    ExactScore score;
    long double intervalWidth = 200'000'000.0L;
    std::unique_ptr<Game> game;
    std::unique_ptr<ExactPlanner> planner;
    bool claimed = false;
    bool blocked = false;
    bool eliminated = false;
    bool hasBeenVisited = false;
    int lastWorker = -1;
  };

  // Tasks are leased only for one bounded slice.  The planner and its partial
  // chance cursors stay inside the task, so a later lease may be taken by the
  // other worker without regenerating completed draw results.
  struct RootTaskQueue {
    std::vector<std::unique_ptr<RootTask>> tasks;
    mutable std::mutex mutex;
    unsigned long long leases = 0;
    unsigned long long reassignments = 0;
    unsigned long long steals = 0;
    unsigned long long eliminations = 0;

    static long double fractionApprox(const ExactFraction& value) {
      if (!value.valid) return 0.0L;
      if (value.big) return 100'000'000.0L;
      return (long double)value.numerator / (long double)value.denominator;
    }

    static long double remainingWidth(const ExactScore& score) {
      const long double width = fractionApprox(score.upper) - fractionApprox(score.lower);
      if (width < 0.0L || width > 200'000'000.0L) return 200'000'000.0L;
      return width;
    }

    int acquire(int workerId) {
      std::lock_guard<std::mutex> lock(mutex);
      int selected = -1;
      long double selectedPriority = -1.0L;
      for (int i = 0; i < (int)tasks.size(); ++i) {
        const RootTask& task = *tasks[i];
        if (task.claimed || task.blocked || task.eliminated || task.score.certified) continue;
        const bool intervalOpen = ExactCompare(task.score.upper, task.score.lower) > 0;
        const long double widthBonus = 1.0L
          + std::min(task.intervalWidth, 200'000'000.0L) / 200'000'000.0L;
        const long double priority = (long double)(task.estimate + 1ULL)
          / (long double)(task.consumedNodes + 1ULL) * (intervalOpen ? 2.0L : 1.0L)
          * widthBonus;
        if (selected < 0 || priority > selectedPriority
            || (priority == selectedPriority && task.option < tasks[selected]->option)) {
          selected = i; selectedPriority = priority;
        }
      }
      if (selected >= 0) {
        RootTask& task = *tasks[selected];
        if (task.lastWorker >= 0) {
          ++reassignments;
          if (task.lastWorker != workerId) ++steals;
        }
        task.lastWorker = workerId; task.claimed = true; ++leases;
      }
      return selected;
    }

    void finishSlice(int index, const ExactScore& fresh,
        unsigned long long consumedNodes, bool blocked) {
      std::lock_guard<std::mutex> lock(mutex);
      if (index < 0 || index >= (int)tasks.size()) return;
      RootTask& task = *tasks[index];
      if (!task.hasBeenVisited) task.score = fresh;
      else {
        if (ExactCompare(fresh.lower, task.score.lower) > 0) task.score.lower = fresh.lower;
        if (ExactCompare(fresh.upper, task.score.upper) < 0) task.score.upper = fresh.upper;
        if (ExactCompare(task.score.lower, task.score.upper) > 0) task.score = ExactScore{};
        else task.score.certified = ExactCompare(task.score.lower, task.score.upper) == 0;
        task.score.action = { task.option };
      }
      task.intervalWidth = remainingWidth(task.score);
      task.consumedNodes += consumedNodes;
      task.score.action = { task.option };
      task.hasBeenVisited = true;
      task.claimed = false;
      task.blocked = task.blocked || blocked;
      pruneDominatedLocked();
    }

    void pruneDominatedLocked() {
      int best = -1;
      for (int i = 0; i < (int)tasks.size(); ++i) {
        const auto& item = tasks[i];
        if (item->blocked || item->eliminated) continue;
        if (best < 0 || ExactCompare(item->score.lower, tasks[best]->score.lower) > 0)
          best = i;
      }
      if (best < 0) return;
      const ExactFraction bestLower = tasks[best]->score.lower;
      for (auto& item : tasks) {
        if (item->claimed || item->blocked || item->eliminated) continue;
        if (ExactCompare(item->score.upper, bestLower) < 0) {
          item->eliminated = true;
          ++eliminations;
        }
      }
    }

    bool pending() const {
      std::lock_guard<std::mutex> lock(mutex);
      for (const auto& item : tasks) {
        if (item->claimed) return true;
        if (!item->blocked && !item->eliminated && !item->score.certified) return true;
      }
      return false;
    }
  };

  std::unique_ptr<Game> game;
  std::unique_ptr<ExactPlanner> planner;
	std::unique_ptr<Worker> alternateWorker;
  ExactMetrics discardedMetrics;
  int turn = -1;
  int actor = -1;
	ExactDecision lastDecision;
	std::chrono::steady_clock::time_point started = std::chrono::steady_clock::now();

  ExactDecision beginFixed(const State& source, const int* deck, const int* handValues, int deckCount,
      const int* opponentDeck, int opponentDeckCount, int budgetMilliseconds,
      std::shared_ptr<const ExactCpuEvaluator> evaluator = nullptr) {
    // Root decisions are exclusively handled by begin()'s shared priority
    // queue. Keep this helper for fixed/non-choice states only; the guard also
    // prevents any legacy fixed-partition code below from being reached by a
    // future caller that bypasses begin().
    if (source.selectMin == 1 && source.selectMax == 1 && source.options.size() > 1)
      return begin(source, deck, handValues, deckCount, opponentDeck, opponentDeckCount,
        budgetMilliseconds, std::move(evaluator));
    turn = source.turn; actor = source.selectPlayer;
		started = std::chrono::steady_clock::now();
    ExactDecision decision;
    if (source.selectMin == 1 && source.selectMax == 1 && source.options.size() > 1) {
		auto sharedTable = std::make_shared<ExactSharedTransposition>();
		auto absoluteDeadline = std::chrono::steady_clock::now()
			+ std::chrono::milliseconds(std::max(1, budgetMilliseconds));
		std::vector<int> representative(source.options.size());
		std::vector<unsigned long long> rootWorkEstimate(source.options.size(), 1);
		std::unordered_map<std::string, int, ExactStringHasher> successorRepresentative;
		for (int option = 0; option < (int)source.options.size(); ++option) {
			Game probeGame = *source.game;
			State probeState = source; probeState.game = &probeGame;
			ExactPlanner probe(deck, handValues, deckCount, 1,
				opponentDeckCount == 0 ? nullptr : opponentDeck, opponentDeckCount, nullptr, evaluator);
			unsigned long long estimate = 1;
			std::string key = probe.canonicalRootSuccessor(probeState, option, &estimate);
			auto [found, inserted] = successorRepresentative.emplace(std::move(key), option);
			representative[option] = inserted ? option : found->second;
			rootWorkEstimate[representative[option]] = std::max(rootWorkEstimate[representative[option]], estimate);
		}
		std::vector<int> orderedOptions;
		for (int option = 0; option < (int)source.options.size(); ++option)
			if (representative[option] == option && source.options[option].type == SelectOptionType::End) orderedOptions.push_back(option);
		for (int option = 0; option < (int)source.options.size(); ++option)
			if (representative[option] == option && source.options[option].type != SelectOptionType::End) orderedOptions.push_back(option);
		// Assign expensive symbolic successors first.  A multi-draw root must not
		// lose half of its wall-clock budget merely because its option index has the
		// same parity as another expensive action.  Evaluation order inside each
		// worker remains the stable End-first order above.
		std::vector<int> byDescendingWork = orderedOptions;
		std::stable_sort(byDescendingWork.begin(), byDescendingWork.end(), [&](int left, int right) {
			return rootWorkEstimate[left] > rootWorkEstimate[right];
		});
		std::array<std::vector<int>, 2> workerAssignments;
		std::array<unsigned long long, 2> assignedWork{};
		for (int option : byDescendingWork) {
			const int worker = assignedWork[0] < assignedWork[1] ? 0 : 1;
			workerAssignments[worker].push_back(option);
			const unsigned long long cost = rootWorkEstimate[option];
			assignedWork[worker] = assignedWork[worker] > std::numeric_limits<unsigned long long>::max() - cost
				? std::numeric_limits<unsigned long long>::max() : assignedWork[worker] + cost;
		}
		for (auto& assigned : workerAssignments) std::stable_sort(assigned.begin(), assigned.end(), [&](int left, int right) {
			return std::find(orderedOptions.begin(), orderedOptions.end(), left)
				< std::find(orderedOptions.begin(), orderedOptions.end(), right);
		});
      std::array<std::unique_ptr<Worker>, 2> workers;
      auto run = [&](int parity) {
        auto output = std::make_unique<Worker>();
        output->game = *source.game;
        output->planner = std::make_unique<ExactPlanner>(deck, handValues, deckCount, budgetMilliseconds,
          opponentDeckCount == 0 ? nullptr : opponentDeck, opponentDeckCount, sharedTable, evaluator);
		output->planner->setConcreteWorldCaching(true);
		output->planner->setChanceParallelEnabled(false);
		output->planner->setThreadPermitHeld(true);
		output->actions.resize(source.options.size());
		std::vector<bool> structurallyBlocked(source.options.size(), false);
		std::vector<int> assigned = workerAssignments[parity];
		output->planner->setReverseActionOrder(parity == 0 && assigned.size() > 1);
		for (int option : assigned) output->actions[option].action = { option };
		if (source.options.size() <= 2) {
			for (int option : assigned) {
				State local = source; local.game = &output->game;
				output->actions[option] = output->planner->evaluateRootAction(local, option).score;
			}
		} else {
			int fairShare = std::max(50, budgetMilliseconds / std::max(1, (int)assigned.size()));
			int firstRoundSlice = std::min(1'000, fairShare);
			const int sliceMilliseconds = std::min(60'000, fairShare);
			bool firstRound = true;
			while (std::chrono::steady_clock::now() < absoluteDeadline) {
				bool pending = false, attempted = false, resourceStopped = false;
				for (int option : assigned) {
					if (output->actions[option].certified || structurallyBlocked[option]) continue;
					pending = true;
					auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
						absoluteDeadline - std::chrono::steady_clock::now()).count();
					if (remaining <= 0) break;
					int slice = (int)std::min<long long>(remaining, firstRound ? firstRoundSlice : sliceMilliseconds);
					output->planner->setBudgetMilliseconds(std::max(1, slice));
					State local = source; local.game = &output->game;
					unsigned long long unknownBefore = output->planner->currentMetrics().unknownOpponentList;
					ExactScore fresh = output->planner->evaluateRootAction(local, option).score;
					if (output->planner->currentMetrics().unknownOpponentList > unknownBefore)
						structurallyBlocked[option] = true;
					ExactScore& saved = output->actions[option];
					if (saved.action.empty()) saved = fresh;
					else {
						if (ExactCompare(fresh.lower, saved.lower) > 0) saved.lower = fresh.lower;
						if (ExactCompare(fresh.upper, saved.upper) < 0) saved.upper = fresh.upper;
						saved.certified = ExactCompare(saved.lower, saved.upper) == 0;
					}
					attempted = true;
					if (output->planner->resourceStopped()) { resourceStopped = true; break; }
				}
				// Argmax certification (opt-in): stop once one root action's lower
				// bound dominates every other action's upper bound.
				if (output->planner->currentMetrics().certificationScope
					== ExactSkeleton::CertScope::Argmax) {
					int best = -1;
					for (int option : assigned) {
						if (output->actions[option].action.empty()) continue;
						if (best < 0 || ExactCompare(output->actions[option].lower,
							output->actions[best].lower) > 0) best = option;
					}
					if (best >= 0 && output->actions[best].certified) {
						bool dominates = true;
						for (int option : assigned) {
							if (option == best || output->actions[option].action.empty()) continue;
							if (ExactCompare(output->actions[best].lower, output->actions[option].upper) < 0) {
								dominates = false; break;
							}
						}
						if (dominates) {
							// Record the cut only. Do not mutate sibling upper bounds —
							// tightening them to best.lower would falsify reported intervals.
							output->argmaxCut = true;
							break;
						}
					}
				}
				firstRound = false;
				if (!pending || !attempted || resourceStopped) break;
			}
		}
        return output;
      };
	  auto rootLease0 = ExactGlobalThreadBudget().acquire();
	  ExactThreadBudget::Lease rootLease1;
	  const size_t assignedCount = workerAssignments[0].size() + workerAssignments[1].size();
	  if (assignedCount > 1) rootLease1 = ExactGlobalThreadBudget().tryLease();
      if (rootLease1) {
	        auto future0 = std::async(std::launch::async, run, 0);
	        auto future1 = std::async(std::launch::async, run, 1);
	        workers[0] = future0.get(); workers[1] = future1.get();
      } else {
	        workers[0] = run(0);
	        workers[1] = run(1);
      }
	  for (auto& worker : workers) if (worker && worker->planner)
		worker->planner->setThreadPermitHeld(false);
	  std::unordered_map<int, ExactScore> representativeScores;
	  std::unordered_map<int, int> representativeWorker;
      for (int wi = 0; wi < 2; ++wi) {
        for (const ExactScore& item : workers[wi]->actions) {
		  if (item.action.empty()) continue;
		  int option = item.action.front();
		  auto found = representativeScores.find(option);
		  if (found == representativeScores.end()) {
			representativeScores.emplace(option, item);
			representativeWorker[option] = wi;
		  } else {
			bool existingCertified = found->second.certified;
			// Both intervals enclose the same exact value. Their intersection
			// combines progress made from opposite traversal directions without
			// sharing mutable partial cursors between workers.
			if (ExactCompare(item.lower, found->second.lower) > 0) {
			  found->second.lower = item.lower;
			  representativeWorker[option] = wi;
			}
			if (ExactCompare(item.upper, found->second.upper) < 0)
			  found->second.upper = item.upper;
			if (item.certified && !existingCertified) representativeWorker[option] = wi;
			if (ExactCompare(found->second.lower, found->second.upper) > 0) {
			  found->second = ExactScore{};
			} else {
			  found->second.certified = ExactCompare(found->second.lower, found->second.upper) == 0;
			}
		  }
        }
        MergeExactMetrics(decision.metrics, workers[wi]->planner->currentMetrics());
        if (workers[wi]->argmaxCut) decision.metrics.argmaxDominatedCuts++;
      }
	  bool first = true;
	  ExactFraction maxUpper = ExactFraction::integer(-100'000'000);
	  ExactFraction maxOtherUpper;
	  bool hasOtherUpper = false;
	  int selectedWorker = 0;
	  for (int option : orderedOptions) {
		auto found = representativeScores.find(option);
		if (found == representativeScores.end()) continue;
		ExactScore item = found->second; item.action = { option };
		decision.rootActions.push_back({ item.action, item.lower, item.upper, item.certified });
		if (first || ExactCompare(item.lower, decision.score.lower) > 0
			|| (ExactCompare(item.lower, decision.score.lower) == 0 && item.action < decision.score.action)) {
		  decision.score = item; selectedWorker = representativeWorker[option]; first = false;
		}
		if (ExactCompare(item.upper, maxUpper) > 0) maxUpper = item.upper;
	  }
	  for (int option = 0; option < (int)representative.size(); ++option) {
		if (representative[option] == option) continue;
		auto found = representativeScores.find(representative[option]);
		if (found == representativeScores.end()) continue;
		ExactScore alias = found->second; alias.action = { option };
		decision.rootActions.push_back({ alias.action, alias.lower, alias.upper, alias.certified });
		decision.metrics.successorMerges++;
		decision.metrics.largestEquivalenceClass = std::max<unsigned long long>(decision.metrics.largestEquivalenceClass, 2);
	  }
      decision.metrics.rootWorkers = 2;
	  if (!first) {
		for (const auto& item : representativeScores) {
			if (item.second.action.empty() || item.first == decision.score.action.front()) continue;
			if (!hasOtherUpper || ExactCompare(item.second.upper, maxOtherUpper) > 0) {
				maxOtherUpper = item.second.upper;
				hasOtherUpper = true;
			}
		}
		decision.bestActionCertified = decision.metrics.informationSetSafe
			&& (!hasOtherUpper || ExactCompare(decision.score.lower, maxOtherUpper) >= 0);
		decision.actionValueCertified = decision.metrics.informationSetSafe
			&& ExactCompare(decision.score.lower, decision.score.upper) == 0;
		decision.exactValueCertified = decision.bestActionCertified
			&& ExactCompare(decision.score.lower, decision.score.upper) == 0;
		decision.score.upper = maxUpper;
		decision.score.certified = decision.exactValueCertified;
	  }
      int other = 1 - selectedWorker;
      discardedMetrics = workers[other]->planner->currentMetrics();
	  alternateWorker = std::move(workers[other]);
      game = std::make_unique<Game>(std::move(workers[selectedWorker]->game));
      planner = std::move(workers[selectedWorker]->planner);
    } else {
      game = std::make_unique<Game>(*source.game);
      planner = std::make_unique<ExactPlanner>(deck, handValues, deckCount, budgetMilliseconds,
        opponentDeckCount == 0 ? nullptr : opponentDeck, opponentDeckCount, nullptr, evaluator);
	  planner->setConcreteWorldCaching(true);
      State local = source; local.game = game.get();
      decision = planner->decide(local);
    }
		lastDecision = decision;
    return decision;
  }

  ExactDecision begin(const State& source, const int* deck, const int* handValues, int deckCount,
      const int* opponentDeck, int opponentDeckCount, int budgetMilliseconds,
      std::shared_ptr<const ExactCpuEvaluator> evaluator = nullptr) {
    if (!(source.selectMin == 1 && source.selectMax == 1 && source.options.size() > 1))
      return beginFixed(source, deck, handValues, deckCount, opponentDeck, opponentDeckCount,
        budgetMilliseconds, std::move(evaluator));

    turn = source.turn; actor = source.selectPlayer;
    started = std::chrono::steady_clock::now();
    ExactDecision decision;
    auto sharedTable = std::make_shared<ExactSharedTransposition>();
    const auto absoluteDeadline = std::chrono::steady_clock::now()
      + std::chrono::milliseconds(std::max(1, budgetMilliseconds));
    std::vector<int> representative(source.options.size());
    std::vector<unsigned long long> estimates(source.options.size(), 1);
    std::unordered_map<std::string, int, ExactStringHasher> successorRepresentative;
    for (int option = 0; option < (int)source.options.size(); ++option) {
      Game probeGame = *source.game;
      State probeState = source; probeState.game = &probeGame;
      ExactPlanner probe(deck, handValues, deckCount, 1,
        opponentDeckCount == 0 ? nullptr : opponentDeck, opponentDeckCount, nullptr, evaluator);
      unsigned long long estimate = 1;
      std::string key = probe.canonicalRootSuccessor(probeState, option, &estimate);
      auto [found, inserted] = successorRepresentative.emplace(std::move(key), option);
      representative[option] = inserted ? option : found->second;
      estimates[representative[option]] = std::max(estimates[representative[option]], estimate);
    }
    std::vector<int> orderedOptions;
    for (int option = 0; option < (int)source.options.size(); ++option)
      if (representative[option] == option && source.options[option].type == SelectOptionType::End)
        orderedOptions.push_back(option);
    for (int option = 0; option < (int)source.options.size(); ++option)
      if (representative[option] == option && source.options[option].type != SelectOptionType::End)
        orderedOptions.push_back(option);

    RootTaskQueue queue;
    for (int option : orderedOptions) {
      auto task = std::make_unique<RootTask>();
      task->option = option; task->estimate = estimates[option];
      task->game = std::make_unique<Game>(*source.game);
      task->planner = std::make_unique<ExactPlanner>(deck, handValues, deckCount,
        std::max(1, std::min(500, budgetMilliseconds)),
        opponentDeckCount == 0 ? nullptr : opponentDeck, opponentDeckCount, sharedTable, evaluator);
      task->planner->setConcreteWorldCaching(true);
      queue.tasks.push_back(std::move(task));
    }
	// Reserve one root permit, then opportunistically claim the second.  Holding
	// the first permit while blocking for a second would deadlock two concurrent
	// sessions, each of which already owns one permit.
	auto rootLease0 = ExactGlobalThreadBudget().acquire();
	ExactThreadBudget::Lease rootLease1;
	if (queue.tasks.size() > 1) rootLease1 = ExactGlobalThreadBudget().tryLease();
	const bool rootParallel = static_cast<bool>(rootLease1);
	for (const auto& task : queue.tasks)
		task->planner->setThreadPermitHeld(true);
	for (const auto& task : queue.tasks)
		task->planner->setChanceParallelEnabled(!rootParallel);

    auto runWorker = [&](int workerId) {
      while (std::chrono::steady_clock::now() < absoluteDeadline) {
        const int index = queue.acquire(workerId);
        if (index < 0) {
          if (!queue.pending()) break;
          std::this_thread::yield();
          continue;
        }
        RootTask& task = *queue.tasks[index];
        const unsigned long long expandedBefore = task.planner->currentMetrics().expanded;
        const unsigned long long unknownBefore = task.planner->currentMetrics().unknownOpponentList;
        const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
          absoluteDeadline - std::chrono::steady_clock::now()).count();
        const int slice = (int)std::max<long long>(1, std::min<long long>(remaining, 500));
        task.planner->setBudgetMilliseconds(slice);
        State local = source; local.game = task.game.get();
        ExactScore fresh = task.planner->evaluateRootAction(local, task.option).score;
        fresh.action = { task.option };
        const unsigned long long expandedAfter = task.planner->currentMetrics().expanded;
        const unsigned long long consumed = expandedAfter >= expandedBefore ? expandedAfter - expandedBefore : 0;
        const bool blocked = task.planner->currentMetrics().unknownOpponentList > unknownBefore;
        queue.finishSlice(index, fresh, consumed, blocked || task.planner->resourceStopped());
      }
    };
    auto future0 = std::async(std::launch::async, runWorker, 0);
    std::future<void> future1;
    if (rootParallel) future1 = std::async(std::launch::async, runWorker, 1);
    future0.get();
    if (future1.valid()) future1.get();
	for (const auto& task : queue.tasks)
		if (task->planner) task->planner->setThreadPermitHeld(false);
	for (const auto& task : queue.tasks)
		MergeExactMetrics(decision.metrics, task->planner->currentMetrics());

    int selectedIndex = -1, alternateIndex = -1;
    bool first = true;
    ExactFraction maxUpper = ExactFraction::integer(-100'000'000);
    for (int option : orderedOptions) {
      int index = -1;
      for (int i = 0; i < (int)queue.tasks.size(); ++i)
        if (queue.tasks[i]->option == option) { index = i; break; }
      if (index < 0) continue;
      ExactScore item = queue.tasks[index]->score; item.action = { option };
      decision.rootActions.push_back({ item.action, item.lower, item.upper, item.certified });
      if (queue.tasks[index]->eliminated) continue;
      if (first || ExactCompare(item.lower, decision.score.lower) > 0
          || (ExactCompare(item.lower, decision.score.lower) == 0 && item.action < decision.score.action)) {
        if (selectedIndex >= 0) alternateIndex = selectedIndex;
        selectedIndex = index; decision.score = item; first = false;
      } else if (alternateIndex < 0) alternateIndex = index;
      maxUpper = ExactCompare(item.upper, maxUpper) > 0 ? item.upper : maxUpper;
    }
    for (int option = 0; option < (int)representative.size(); ++option) {
      if (representative[option] == option) continue;
      int repIndex = -1;
      for (int i = 0; i < (int)queue.tasks.size(); ++i)
        if (queue.tasks[i]->option == representative[option]) { repIndex = i; break; }
      if (repIndex < 0) continue;
      ExactScore alias = queue.tasks[repIndex]->score; alias.action = { option };
      decision.rootActions.push_back({ alias.action, alias.lower, alias.upper, alias.certified });
      decision.metrics.successorMerges++;
      decision.metrics.largestEquivalenceClass = std::max<unsigned long long>(decision.metrics.largestEquivalenceClass, 2);
    }
    if (!first) {
      ExactFraction maxOtherUpper;
      bool hasOtherUpper = false;
      for (const auto& task : queue.tasks) {
        if (task->eliminated || task->option == queue.tasks[selectedIndex]->option)
          continue;
        if (!hasOtherUpper || ExactCompare(task->score.upper, maxOtherUpper) > 0) {
          maxOtherUpper = task->score.upper;
          hasOtherUpper = true;
        }
      }
      decision.bestActionCertified = decision.metrics.informationSetSafe
        && queue.tasks[selectedIndex]->hasBeenVisited
        && (!hasOtherUpper
          || ExactCompare(decision.score.lower, maxOtherUpper) >= 0);
      decision.actionValueCertified = decision.metrics.informationSetSafe
        && ExactCompare(decision.score.lower, decision.score.upper) == 0;
      decision.exactValueCertified = decision.bestActionCertified
        && ExactCompare(decision.score.lower, decision.score.upper) == 0;
      if (!decision.bestActionCertified) decision.score.upper = maxUpper;
      decision.score.certified = decision.exactValueCertified;
    }
    decision.metrics.rootWorkers = rootParallel ? 2 : 1;
    decision.metrics.rootQueueLeases += queue.leases;
    decision.metrics.rootQueueReassignments += queue.reassignments;
    decision.metrics.rootQueueSteals += queue.steals;
    decision.metrics.rootQueueEliminations += queue.eliminations;
    if (selectedIndex >= 0) {
      game = std::move(queue.tasks[selectedIndex]->game);
      planner = std::move(queue.tasks[selectedIndex]->planner);
      if (alternateIndex >= 0 && alternateIndex != selectedIndex) {
        alternateWorker = std::make_unique<Worker>();
        alternateWorker->game = std::move(*queue.tasks[alternateIndex]->game);
        alternateWorker->planner = std::move(queue.tasks[alternateIndex]->planner);
      }
    }
    lastDecision = decision;
    return decision;
  }

  ExactDecision advance(const State& source, int budgetMilliseconds) {
    ExactDecision decision;
    if (source.turn != turn || source.selectPlayer != actor || planner == nullptr) {
      decision.metrics = discardedMetrics;
      decision.metrics.sessionInvalidations++;
      return decision;
    }
    State local = source; local.game = game.get();
	bool alternatePolicyHit = false;
    if (!planner->lookupPolicy(local, decision)) {
	  if (alternateWorker != nullptr && alternateWorker->planner != nullptr) {
		State alternate = source; alternate.game = &alternateWorker->game;
		alternatePolicyHit = alternateWorker->planner->lookupPolicy(alternate, decision);
	  }
	  if (!alternatePolicyHit) decision = planner->resume(local, budgetMilliseconds);
    }
	ExactMetrics combined = alternatePolicyHit ? planner->currentMetrics() : discardedMetrics;
    MergeExactMetrics(combined, decision.metrics);
    combined.rootWorkers = 2;
    decision.metrics = combined;
		lastDecision = decision;
    return decision;
  }

	long long elapsedMilliseconds() const {
		return std::chrono::duration_cast<std::chrono::milliseconds>(
			std::chrono::steady_clock::now() - started).count();
	}
};

static const char8_t* ExactProgressJson(ApiData* data, long long sessionId, const ExactTurnSession& session) {
  const ExactMetrics& metrics = session.lastDecision.metrics;
  JsonBuilder& j = data->jsonBuilder;
  j.clear(); j.append('{');
  j.appendKey("sessionId"); AppendLongLong(j, sessionId);
  j.appendCommaKeyValue("turn", session.turn);
  j.appendCommaKeyValue("actor", session.actor);
  j.appendCommaKeyValue("currentRootAction", metrics.currentRootAction);
  j.appendCommaKeyValue("maxDepth", metrics.maxDepth);
  j.appendCommaKey("expandedNodes"); AppendUnsignedLongLong(j, metrics.expanded);
  j.appendCommaKey("resumedActionCount"); AppendUnsignedLongLong(j, metrics.resumedActionCount);
  j.appendCommaKey("resumedChanceMass"); AppendUnsignedLongLong(j, metrics.resumedChanceMass);
  j.appendCommaKey("largestEquivalenceClass"); AppendUnsignedLongLong(j, metrics.largestEquivalenceClass);
  j.appendCommaKey("canonicalStateMerges"); AppendUnsignedLongLong(j, metrics.canonicalStateMerges);
  j.appendCommaKey("successorMerges"); AppendUnsignedLongLong(j, metrics.successorMerges);
  j.appendCommaKey("distributionMerges"); AppendUnsignedLongLong(j, metrics.distributionMerges);
  j.appendCommaKey("sessionBytes"); AppendUnsignedLongLong(j, metrics.sessionBytes);
	 j.appendCommaKey("peakRssBytes"); AppendUnsignedLongLong(j, metrics.peakRssBytes);
	 j.appendCommaKeyValue("memoryLimitReached", metrics.memoryLimitReached);
	 j.appendCommaKeyValue("structurallyBlocked", metrics.structurallyBlocked);
	 j.appendCommaKey("searchStatus");
	 j.appendDoubleQuote(session.lastDecision.score.certified ? u8"certified"
		 : (metrics.structurallyBlocked ? u8"blocked" : u8"resumable"));
  j.appendCommaKey("elapsedMilliseconds"); AppendLongLong(j, session.elapsedMilliseconds());
  j.appendCommaKeyValue("certified", session.lastDecision.score.certified);
  j.appendCommaKeyValue("probabilityExact", metrics.probabilityExact);
  j.appendCommaKeyValue("informationSetSafe", metrics.informationSetSafe);
  j.appendCommaKey("beliefNodes"); AppendUnsignedLongLong(j, metrics.beliefNodes);
  j.appendCommaKey("informationSets"); AppendUnsignedLongLong(j, metrics.informationSets);
	j.appendCommaKey("provisionalOpponentPolicyNodes"); AppendUnsignedLongLong(j, metrics.provisionalOpponentPolicyNodes);
  j.appendCommaKey("bigWeightPromotions"); AppendUnsignedLongLong(j, metrics.bigWeightPromotions);
  j.appendCommaKeyValue("maxWeightBits", (int)metrics.maxWeightBits);
  j.append('}');
  return j.buf.c_str();
}

static std::mutex ExactSessionMutex;
static std::unordered_map<ApiData*, std::unordered_map<long long, std::unique_ptr<ExactTurnSession>>> ExactSessions;
static std::atomic<long long> NextExactSessionId{ 1 };

extern "C" {

  GAME_API void GameInitialize() {
    InitializeAll();
  }

  GAME_API StartData BattleStart(int* cards) {
    return ApiBattleStart(cards);
  }

  GAME_API StartData BattleStartSeeded(int* cards, unsigned int seed) {
    return ApiBattleStartSeeded(cards, seed, true);
  }

  GAME_API StartData BattleStartOrdered(int* cards, unsigned int seed) {
    return ApiBattleStartOrdered(cards, seed);
  }

  GAME_API ApiData* AgentStart() {
    return ApiAgentStart();
  }

  GAME_API void BattleFinish(ApiData* data) {
    {
      std::lock_guard<std::mutex> lock(ExactSessionMutex);
      ExactSessions.erase(data);
    }
    return ApiBattleFinish(data);
  }

  GAME_API SerialData GetBattleData(ApiData* data) {
    if (data->apiDataType != 1) {
      return {};
    }

    if (data->preGetSelectCount != data->selectCount) {
      const State& state = data->state;
      int index = std::max(state.logIndex[0], state.logIndex[1]);
      const std::vector<int>* selected = nullptr;
      if (data->visData.size() > 0) {
        selected = &data->selected;
      }
      ToJsonVis(data->state, data->jsonBuilder, index, selected);
      data->visData.push_back(data->jsonBuilder.buf);
      data->preGetSelectCount = data->selectCount;
    }

    return ApiGetBattleData(data);
  }

  GAME_API int Select(ApiData* data, int* select, int selectCount) {
    if (data->apiDataType != 1) {
      return 30;
    }
    return ApiSelect(data, select, selectCount);
  }

  GAME_API const char8_t* VisualizeData(ApiData* data) {
    if (data->apiDataType != 1) {
      return nullptr;
    }

    ApiVisualizeData(data->jsonBuilder, data->visData);
    
    return data->jsonBuilder.buf.c_str();
  }

  GAME_API const char8_t* SearchBegin(ApiData* data, const char* serialized, int count, int* myDeck, int* myPrize, int* enemyDeck, int* enemyPrize, int* enemyHand, int* enemyActive, int manualCoin) {
    SearchInfo si;
    if (data->apiDataType != 2) {
      si = SearchInfo::error(30);
    } else {
      SetBattleData(data, serialized, count);

      try {
        const State& state = data->state;
        int myIndex = state.selectPlayer;

        int error = 0;
        SearchStartConfig config;
        if (!state.selectDeck) {
          CopyIdPtr(myDeck, config.myDeck, state.players[myIndex].deck.size(), error);
        }
        CopyIdPtr(myPrize, config.myPrize, state.players[myIndex].prize.size(), error);
        CopyIdPtr(enemyDeck, config.enemyDeck, state.players[1 - myIndex].deck.size(), error);
        CopyIdPtr(enemyPrize, config.enemyPrize, state.players[1 - myIndex].prize.size(), error);
        CopyIdPtr(enemyHand, config.enemyHand, state.players[1 - myIndex].hand.size(), error);
        if (IsActiveNull(state, 1 - myIndex)) {
          CopyIdPtr(enemyActive, config.enemyActive, state.players[1 - myIndex].active.size(), error);
        }
        config.manualCoin = (bool)manualCoin;

        if (error) {
          si = SearchInfo::error(error);
        } else {
          si = ApiSearchBegin(data, config);
        }

      } catch (...) {
        si = SearchInfo::error(99);
      }
    }
    return JsonResult(data, si);
  }

  GAME_API const char8_t* SearchStep(ApiData* data, long long searchId, int* select, int selectCount) {
    SearchInfo si;
    if (data->apiDataType != 2) {
      si = SearchInfo::error(30);
    } else {
      try {
        si = ApiSearchStep(data, searchId, select, selectCount);
      } catch (...) {
        si = SearchInfo::error(99);
      }
    }
    return JsonResult(data, si);
  }

  GAME_API const char8_t* ExactDecide(ApiData* data, const char* serialized, int count,
      int* deck, int* handValues, int deckCount, int budgetMilliseconds) {
    if (data->apiDataType != 2 || deckCount <= 0 || deckCount > DECK_SIZE) {
      data->jsonBuilder.clear();
      data->jsonBuilder.appendStr("{\"error\":30}");
      return data->jsonBuilder.buf.c_str();
    }
    try {
      SetBattleData(data, serialized, count);
      auto session = std::make_unique<ExactTurnSession>();
      ExactDecision decision = session->begin(data->state, deck, handValues, deckCount,
        nullptr, 0, budgetMilliseconds, data->exactEvaluator);
      return ExactDecisionJson(data, decision);
    } catch (...) {
      data->jsonBuilder.clear();
      data->jsonBuilder.appendStr("{\"error\":99}");
      return data->jsonBuilder.buf.c_str();
    }
  }

  GAME_API const char8_t* ExactEvaluateAction(ApiData* data, const char* serialized, int count,
      int* deck, int* handValues, int deckCount, int budgetMilliseconds, int optionIndex) {
    if (data->apiDataType != 2 || deckCount <= 0 || deckCount > DECK_SIZE) {
      data->jsonBuilder.clear(); data->jsonBuilder.appendStr("{\"error\":30}");
      return data->jsonBuilder.buf.c_str();
    }
    try {
      SetBattleData(data, serialized, count);
      ExactPlanner planner(deck, handValues, deckCount, budgetMilliseconds,
        nullptr, 0, nullptr, data->exactEvaluator);
      ExactDecision decision = planner.evaluateRootAction(data->state, optionIndex);
      return ExactDecisionJson(data, decision);
    } catch (...) {
      data->jsonBuilder.clear(); data->jsonBuilder.appendStr("{\"error\":99}");
      return data->jsonBuilder.buf.c_str();
    }
  }

  GAME_API const char8_t* ExactDecideV2(ApiData* data, const char* serialized, int count,
      int* deck, int* handValues, int deckCount, int* opponentDeck, int opponentDeckCount,
      int budgetMilliseconds) {
    if (data->apiDataType != 2 || deckCount <= 0 || deckCount > DECK_SIZE
        || opponentDeckCount < 0 || opponentDeckCount > DECK_SIZE) {
      data->jsonBuilder.clear(); data->jsonBuilder.appendStr("{\"error\":30}");
      return data->jsonBuilder.buf.c_str();
    }
    try {
      SetBattleData(data, serialized, count);
      // Route the public one-shot API through the same lease-based root queue
      // as turn sessions; no parity-based root splitter remains on this path.
      auto session = std::make_unique<ExactTurnSession>();
      ExactDecision queuedDecision = session->begin(data->state, deck, handValues, deckCount,
        opponentDeckCount == 0 ? nullptr : opponentDeck, opponentDeckCount,
        budgetMilliseconds, data->exactEvaluator);
      return ExactDecisionJson(data, queuedDecision);
    } catch (...) {
      data->jsonBuilder.clear(); data->jsonBuilder.appendStr("{\"error\":99}");
      return data->jsonBuilder.buf.c_str();
    }
  }

  GAME_API const char8_t* ExactEvaluateActionV2(ApiData* data, const char* serialized, int count,
      int* deck, int* handValues, int deckCount, int* opponentDeck, int opponentDeckCount,
      int budgetMilliseconds, int optionIndex) {
    if (data->apiDataType != 2 || deckCount <= 0 || deckCount > DECK_SIZE
        || opponentDeckCount < 0 || opponentDeckCount > DECK_SIZE) {
      data->jsonBuilder.clear(); data->jsonBuilder.appendStr("{\"error\":30}");
      return data->jsonBuilder.buf.c_str();
    }
    try {
      SetBattleData(data, serialized, count);
      ExactPlanner planner(deck, handValues, deckCount, budgetMilliseconds,
          opponentDeckCount == 0 ? nullptr : opponentDeck, opponentDeckCount,
          nullptr, data->exactEvaluator);
      const auto absoluteDeadline = std::chrono::steady_clock::now()
        + std::chrono::milliseconds(std::max(1, budgetMilliseconds));
      ExactDecision accumulated;
      bool first = true;
      while (std::chrono::steady_clock::now() < absoluteDeadline) {
        const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
          absoluteDeadline - std::chrono::steady_clock::now()).count();
        if (remaining <= 0) break;
        planner.setBudgetMilliseconds((int)std::min<long long>(remaining, 60'000));
        ExactDecision fresh = planner.evaluateRootAction(data->state, optionIndex);
        if (first) { accumulated = fresh; first = false; }
        else {
          if (ExactCompare(fresh.score.lower, accumulated.score.lower) > 0)
            accumulated.score.lower = fresh.score.lower;
          if (ExactCompare(fresh.score.upper, accumulated.score.upper) < 0)
            accumulated.score.upper = fresh.score.upper;
          accumulated.score.action = { optionIndex };
          accumulated.score.certified = ExactCompare(accumulated.score.lower, accumulated.score.upper) == 0;
          accumulated.rootActions = fresh.rootActions;
          accumulated.metrics = fresh.metrics;
        }
        if (accumulated.score.certified || planner.resourceStopped()) break;
      }
      if (first) accumulated = planner.evaluateRootAction(data->state, optionIndex);
      accumulated.bestActionCertified = false;
      accumulated.actionValueCertified = accumulated.score.certified;
      accumulated.exactValueCertified = accumulated.score.certified;
      return ExactDecisionJson(data, accumulated);
    } catch (...) {
      data->jsonBuilder.clear(); data->jsonBuilder.appendStr("{\"error\":99}");
      return data->jsonBuilder.buf.c_str();
    }
  }

  GAME_API const char8_t* ExactTurnBegin(ApiData* data, const char* serialized, int count,
      int* deck, int* handValues, int deckCount, int* opponentDeck, int opponentDeckCount,
      int budgetMilliseconds) {
    if (data->apiDataType != 2 || deckCount <= 0 || deckCount > DECK_SIZE
        || opponentDeckCount < 0 || opponentDeckCount > DECK_SIZE) {
      data->jsonBuilder.clear(); data->jsonBuilder.appendStr("{\"error\":30}");
      return data->jsonBuilder.buf.c_str();
    }
    try {
      SetBattleData(data, serialized, count);
      auto session = std::make_unique<ExactTurnSession>();
      ExactDecision decision = session->begin(data->state, deck, handValues, deckCount,
        opponentDeck, opponentDeckCount, budgetMilliseconds, data->exactEvaluator);
      long long id = NextExactSessionId.fetch_add(1);
      {
        std::lock_guard<std::mutex> lock(ExactSessionMutex);
        ExactSessions[data].clear();
        ExactSessions[data][id] = std::move(session);
      }
      return ExactDecisionJson(data, decision, id);
    } catch (const std::exception& error) {
      return ExactErrorJson(data, 99, error.what());
    } catch (...) {
      return ExactErrorJson(data, 99, "unknown native exception");
    }
  }

  GAME_API const char8_t* ExactTurnAdvance(ApiData* data, long long sessionId,
      const char* serialized, int count, int budgetMilliseconds) {
    if (data->apiDataType != 2) {
      data->jsonBuilder.clear(); data->jsonBuilder.appendStr("{\"error\":30}");
      return data->jsonBuilder.buf.c_str();
    }
    try {
      SetBattleData(data, serialized, count);
      ExactTurnSession* session = nullptr;
      {
        std::lock_guard<std::mutex> lock(ExactSessionMutex);
        auto owner = ExactSessions.find(data);
        if (owner != ExactSessions.end()) {
          auto found = owner->second.find(sessionId);
          if (found != owner->second.end()) session = found->second.get();
        }
      }
      if (session == nullptr) {
        data->jsonBuilder.clear(); data->jsonBuilder.appendStr("{\"error\":31}");
        return data->jsonBuilder.buf.c_str();
      }
      return ExactDecisionJson(data, session->advance(data->state, budgetMilliseconds), sessionId);
    } catch (...) {
      data->jsonBuilder.clear(); data->jsonBuilder.appendStr("{\"error\":99}");
      return data->jsonBuilder.buf.c_str();
    }
  }

  GAME_API const char8_t* ExactTurnProgress(ApiData* data, long long sessionId) {
    if (data->apiDataType != 2) {
      data->jsonBuilder.clear(); data->jsonBuilder.appendStr("{\"error\":30}");
      return data->jsonBuilder.buf.c_str();
    }
    std::lock_guard<std::mutex> lock(ExactSessionMutex);
    auto owner = ExactSessions.find(data);
    if (owner == ExactSessions.end() || !owner->second.contains(sessionId)) {
      data->jsonBuilder.clear(); data->jsonBuilder.appendStr("{\"error\":31}");
      return data->jsonBuilder.buf.c_str();
    }
    return ExactProgressJson(data, sessionId, *owner->second.at(sessionId));
  }

  GAME_API void ExactTurnRelease(ApiData* data, long long sessionId) {
    std::lock_guard<std::mutex> lock(ExactSessionMutex);
    auto owner = ExactSessions.find(data);
    if (owner == ExactSessions.end()) return;
    owner->second.erase(sessionId);
    if (owner->second.empty()) ExactSessions.erase(owner);
  }

  GAME_API void SearchEnd(ApiData* data) {
    if (data->apiDataType != 2) {
      return;
    }
    ApiSearchEnd(data);
  }

  GAME_API void SearchRelease(ApiData* data, long long searchId) {
    if (data->apiDataType != 2) {
      return;
    }
    ApiSearchRelease(data, searchId);
  }

  GAME_API const char8_t* AllCard() {
    if (AllCardJson.buf.empty()) {
      ApiAllCard(AllCardJson);
    }
    return AllCardJson.buf.c_str();
  }

  GAME_API const char8_t* AllAttack() {
    if (AllAttackJson.buf.empty()) {
      ApiAllAttack(AllAttackJson);
    }
    return AllAttackJson.buf.c_str();
  }
}
