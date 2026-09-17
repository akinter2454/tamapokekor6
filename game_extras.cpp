#include "game_extras.h"
#include "pet.h"
#include "personality.h"
#include "battle.h"
#include "types.h"
#include <string.h>

GameExtras extras;

static uint8_t satAdd8(uint8_t a, uint8_t b, uint8_t cap = 99) {
  uint16_t v = (uint16_t)a + b;
  return v > cap ? cap : (uint8_t)v;
}

void GameExtras::begin() {
  prefs.begin("tamapoke", false);
  // v3.20 appended one inventory slot. Load older shorter arrays into the
  // front so existing candies and Shiny Charms keep their numeric IDs.
  size_t itemStored = prefs.getBytesLength("xitem");
  if (itemStored) {
    memset(_items, 0, sizeof(_items));
    size_t n = itemStored < sizeof(_items) ? itemStored : sizeof(_items);
    prefs.getBytes("xitem", _items, n);
  }
  if (prefs.getBytesLength("xtm") == sizeof(_tms)) prefs.getBytes("xtm", _tms, sizeof(_tms));
  _shinyBoost = prefs.getBool("xshb", false);
  _exploreActive = prefs.getBool("xepa", false);
  _exploreReady = prefs.getBool("xepr", false);
  _exploreMatch = prefs.getBool("xepm", false);
  _exploreType = prefs.getUChar("xept", T_NORMAL);
  if (_exploreType >= TYPE_COUNT) _exploreType = T_NORMAL;
  _exploreEndEpoch = prefs.getUInt("xepe", 0);
  _exploreEndMinute = prefs.getUInt("xepn", 0);
  _exploreRewardKind = prefs.getUChar("xepk", REWARD_ITEM);
  _exploreRewardId = prefs.getUChar("xepi", 0);
  _exploreRewardCount = prefs.getUChar("xepc", 0);
  _exploreWeather = prefs.getUChar("xepw", WEATHER_CLEAR);
  if (_exploreWeather >= WEATHER_ACTIVE_COUNT) _exploreWeather = WEATHER_CLEAR;
  _mapPieces = prefs.getUChar("xmap", 0);
  if (_mapPieces > 9) _mapPieces = 9;
  _treasureState = prefs.getUChar("xtrs", 0);
  if (_treasureState > 2) _treasureState = 0;
  _treasureRewardKind = prefs.getUChar("xtrk", REWARD_ITEM);
  _treasureRewardId = prefs.getUChar("xtri", 0);
  _treasureRewardCount = prefs.getUChar("xtrc", 0);
  _missionDay = prefs.getUInt("xmd", 0);
  if (prefs.getBytesLength("xmk") == sizeof(_missionKind)) prefs.getBytes("xmk", _missionKind, sizeof(_missionKind));
  if (prefs.getBytesLength("xmp") == sizeof(_missionProg)) prefs.getBytes("xmp", _missionProg, sizeof(_missionProg));
  _missionClaimMask = prefs.getUChar("xmc", 0);
  if (prefs.getBytesLength("xres") == sizeof(_research)) prefs.getBytes("xres", _research, sizeof(_research));
  _observedDex = prefs.getShort("xobs", 0);
  _towerStreak = prefs.getUShort("xtws", 0);
  _towerBest = prefs.getUShort("xtwb", 0);
  if (prefs.getBytesLength("xtbf") == sizeof(_towerBuffs)) prefs.getBytes("xtbf", _towerBuffs, sizeof(_towerBuffs));
  _towerBuffPending = prefs.getBool("xtbp", false);
  if (prefs.getBytesLength("xtbc") == sizeof(_towerBuffChoices)) prefs.getBytes("xtbc", _towerBuffChoices, sizeof(_towerBuffChoices));
  for (uint8_t i = 0; i < 3; i++) if (_towerBuffChoices[i] >= TBUFF_COUNT) _towerBuffChoices[i] = i;
  _bossWinMask = prefs.getUInt("xbwm", 0);
  _bossRewardKind = prefs.getUChar("xbwk", REWARD_ITEM);
  _bossRewardId = prefs.getUChar("xbwi", 0);
  _bossRewardCount = prefs.getUChar("xbwc", 0);
  _rivalWins = prefs.getUShort("xrvw", 0);
  _rivalLosses = prefs.getUShort("xrvl", 0);
  _rivalNextEpoch = prefs.getUInt("xrve", 0);
  _rivalNextMinute = prefs.getUInt("xrvm", 0);
  _rivalRewardKind = prefs.getUChar("xrvk", REWARD_ITEM);
  _rivalRewardId = prefs.getUChar("xrvi", 0);
  _rivalRewardCount = prefs.getUChar("xrvc", 0);
  _rivalSpecial = prefs.getUChar("xrvs", RIVSPEC_NONE);
  if (_rivalSpecial > RIVSPEC_TREASURE_RACE) _rivalSpecial = RIVSPEC_NONE;
  _rivalTradeWant = prefs.getUChar("xrvq", T_NORMAL);
  _rivalTradeGive = prefs.getUChar("xrvg", T_FIRE);
  if (_rivalTradeWant >= TYPE_COUNT) _rivalTradeWant = T_NORMAL;
  if (_rivalTradeGive >= TYPE_COUNT) _rivalTradeGive = T_FIRE;
  _rivalSpecialSerial = prefs.getUShort("xrvsr", 0);
  _eventId = prefs.getUChar("xevt", 0);
  _eventRewardKind = prefs.getUChar("xerk", 0);
  _eventRewardId = prefs.getUChar("xerid", 0);
  _eventRewardCount = prefs.getUChar("xerc", 0);
  _lastEventMinute = prefs.getUInt("xelast", 0);
}

void GameExtras::save() {
  if (_saveBatchDepth) {
    _saveDirty = true;
    return;
  }
  saveNow();
}

void GameExtras::saveNow() {
  _saveDirty = false;
  prefs.putBytes("xitem", _items, sizeof(_items));
  prefs.putBytes("xtm", _tms, sizeof(_tms));
  prefs.putBool("xshb", _shinyBoost);
  prefs.putBool("xepa", _exploreActive);
  prefs.putBool("xepr", _exploreReady);
  prefs.putBool("xepm", _exploreMatch);
  prefs.putUChar("xept", _exploreType);
  prefs.putUInt("xepe", _exploreEndEpoch);
  prefs.putUInt("xepn", _exploreEndMinute);
  prefs.putUChar("xepk", _exploreRewardKind);
  prefs.putUChar("xepi", _exploreRewardId);
  prefs.putUChar("xepc", _exploreRewardCount);
  prefs.putUChar("xepw", _exploreWeather);
  prefs.putUChar("xmap", _mapPieces);
  prefs.putUChar("xtrs", _treasureState);
  prefs.putUChar("xtrk", _treasureRewardKind);
  prefs.putUChar("xtri", _treasureRewardId);
  prefs.putUChar("xtrc", _treasureRewardCount);
  prefs.putUInt("xmd", _missionDay);
  prefs.putBytes("xmk", _missionKind, sizeof(_missionKind));
  prefs.putBytes("xmp", _missionProg, sizeof(_missionProg));
  prefs.putUChar("xmc", _missionClaimMask);
  prefs.putBytes("xres", _research, sizeof(_research));
  prefs.putShort("xobs", _observedDex);
  prefs.putUShort("xtws", _towerStreak);
  prefs.putUShort("xtwb", _towerBest);
  prefs.putBytes("xtbf", _towerBuffs, sizeof(_towerBuffs));
  prefs.putBool("xtbp", _towerBuffPending);
  prefs.putBytes("xtbc", _towerBuffChoices, sizeof(_towerBuffChoices));
  prefs.putUInt("xbwm", _bossWinMask);
  prefs.putUChar("xbwk", _bossRewardKind);
  prefs.putUChar("xbwi", _bossRewardId);
  prefs.putUChar("xbwc", _bossRewardCount);
  prefs.putUShort("xrvw", _rivalWins);
  prefs.putUShort("xrvl", _rivalLosses);
  prefs.putUInt("xrve", _rivalNextEpoch);
  prefs.putUInt("xrvm", _rivalNextMinute);
  prefs.putUChar("xrvk", _rivalRewardKind);
  prefs.putUChar("xrvi", _rivalRewardId);
  prefs.putUChar("xrvc", _rivalRewardCount);
  prefs.putUChar("xrvs", _rivalSpecial);
  prefs.putUChar("xrvq", _rivalTradeWant);
  prefs.putUChar("xrvg", _rivalTradeGive);
  prefs.putUShort("xrvsr", _rivalSpecialSerial);
  prefs.putUChar("xevt", _eventId);
  prefs.putUChar("xerk", _eventRewardKind);
  prefs.putUChar("xerid", _eventRewardId);
  prefs.putUChar("xerc", _eventRewardCount);
  prefs.putUInt("xelast", _lastEventMinute);
}

void GameExtras::beginBatch() {
  if (_saveBatchDepth < 255) _saveBatchDepth++;
}

void GameExtras::endBatch(bool flushNow) {
  if (_saveBatchDepth) _saveBatchDepth--;
  if (_saveBatchDepth == 0 && _saveDirty && flushNow) saveNow();
}

void GameExtras::flushPendingSave() {
  if (_saveBatchDepth == 0 && _saveDirty) saveNow();
}

uint32_t GameExtras::dayFor(const Pet &pet) const {
  if (pet.lastSeenEpoch) return pet.lastSeenEpoch / 86400UL;
  return 1UL + pet.ageMinutes / 1440UL;
}

void GameExtras::rollMissions(uint32_t day) {
  uint32_t x = day ^ 0x7A6D31C5UL;
  for (uint8_t i = 0; i < 3; i++) {
    // Tiny deterministic xorshift: missions do not consume the game's RNG, so
    // merely opening the mission screen cannot change an egg/shiny roll.
    x ^= x << 13; x ^= x >> 17; x ^= x << 5;
    uint8_t k = (uint8_t)(x % MIS_KIND_COUNT);
    bool dup = true;
    while (dup) {
      dup = false;
      for (uint8_t j = 0; j < i; j++) if (_missionKind[j] == k) { dup = true; k = (k + 1) % MIS_KIND_COUNT; break; }
    }
    _missionKind[i] = k;
    _missionProg[i] = 0;
  }
  _missionClaimMask = 0;
}

void GameExtras::ensureDaily(const Pet &pet) {
  uint32_t d = dayFor(pet);
  if (!d) d = 1;
  if (_missionDay == d) return;
  _missionDay = d;
  rollMissions(d);
  save();
}

uint8_t GameExtras::missionGoal(uint8_t i) const {
  if (i >= 3) return 1;
  static const uint8_t G[MIS_KIND_COUNT] = {3, 2, 2, 1, 2, 5};
  uint8_t k = _missionKind[i] < MIS_KIND_COUNT ? _missionKind[i] : 0;
  return G[k];
}

const char *GameExtras::missionNameKo(uint8_t kind) const {
  static const char *const N[MIS_KIND_COUNT] = {
    "먹이 3번 주기", "미니게임 2번", "훈련 2번", "목욕 1번", "배틀 2승", "쓰다듬기 5번"
  };
  return kind < MIS_KIND_COUNT ? N[kind] : "오늘의 미션";
}

void GameExtras::missionAction(uint8_t kind, uint8_t amount, Pet &pet) {
  ensureDaily(pet);
  bool dirty = false;
  for (uint8_t i = 0; i < 3; i++) {
    if (_missionKind[i] != kind || missionClaimed(i)) continue;
    uint8_t goal = missionGoal(i);
    uint8_t before = _missionProg[i];
    _missionProg[i] = satAdd8(_missionProg[i], amount, goal);
    dirty |= before != _missionProg[i];
  }
  if (dirty) save();
}

uint8_t GameExtras::missionRewardKind(uint8_t i) const {
  // One of three daily rewards is commonly a TM; the rest are growth items.
  return (((_missionDay + i * 3UL) % 4UL) == 0UL) ? REWARD_TM : REWARD_ITEM;
}

uint8_t GameExtras::missionRewardId(uint8_t i, const Pet &pet) const {
  if (missionRewardKind(i) == REWARD_TM) {
    if (!pet.isEgg() && pet.speciesId >= 1 && pet.speciesId <= DEX_COUNT)
      return DEX_TBL[pet.speciesId].type1 < TYPE_COUNT ? DEX_TBL[pet.speciesId].type1 : T_NORMAL;
    return (uint8_t)((_missionDay + i) % TYPE_COUNT);
  }
  // Shiny charms are deliberately rarer than ordinary raising items. Energy
  // candy joins the normal pool without moving the old item IDs.
  uint8_t r = (uint8_t)((_missionDay * 7UL + i * 11UL) % 15UL);
  if (r == 0) return XITEM_SHINY;
  static const uint8_t COMMON[5] = { XITEM_ATK, XITEM_DEF, XITEM_SPE, XITEM_VITAL, XITEM_ENERGY };
  return COMMON[r % 5];
}

bool GameExtras::claimMission(uint8_t i, Pet &pet) {
  ensureDaily(pet);
  if (i >= 3 || missionClaimed(i) || !missionComplete(i)) return false;
  uint8_t kind = missionRewardKind(i), id = missionRewardId(i, pet);
  if (kind == REWARD_TM) giveTm(id, 1); else giveItem(id, 1);
  _missionClaimMask |= (uint8_t)(1u << i);
  save();
  return true;
}

const char *GameExtras::itemNameKo(uint8_t id) const {
  static const char *const N[XITEM_COUNT] = {
    "공격사탕", "방어사탕", "스피드사탕", "활력사탕", "반짝부적", "기력사탕",
    "공격개체열매", "방어개체열매", "스피드개체열매", "체력개체열매", "금빛왕관", "샤이니열매"
  };
  return id < XITEM_COUNT ? N[id] : "아이템";
}

const char *GameExtras::itemEffectKo(uint8_t id) const {
  static const char *const N[XITEM_COUNT] = {
    "공격 훈련 +8", "방어 훈련 +8", "스피드 훈련 +8", "기력·행복 회복", "다음 알 Shiny 2배", "기력 +70",
    "공격 개체값 +1", "방어 개체값 +1", "스피드 개체값 +1", "체력 개체값 +1", "모든 개체값 +1", "현재 포켓몬을 Shiny로 변화"
  };
  return id < XITEM_COUNT ? N[id] : "";
}

void GameExtras::giveItem(uint8_t id, uint8_t count) {
  if (id >= XITEM_COUNT || !count) return;
  _items[id] = satAdd8(_items[id], count, 99);
  save();
}

void GameExtras::giveTm(uint8_t type, uint8_t count) {
  if (type >= TYPE_COUNT || !count) return;
  _tms[type] = satAdd8(_tms[type], count, 99);
  save();
}

bool GameExtras::consumeTm(uint8_t type) {
  if (type >= TYPE_COUNT || !_tms[type]) return false;
  _tms[type]--;
  save();
  return true;
}

bool GameExtras::useItem(uint8_t id, Pet &pet) {
  if (id >= XITEM_COUNT || !_items[id] || pet.isEgg() || pet.ceremony != CER_NONE) return false;
  bool used = false;
  switch (id) {
    case XITEM_ATK: {
      uint8_t before = pet.trAtk;
      pet.trAtk = (uint8_t)min<uint16_t>((uint16_t)pet.trAtk + 8, pet.trMaxAtk());
      used = pet.trAtk != before;
      break;
    }
    case XITEM_DEF: {
      uint8_t before = pet.trDef;
      pet.trDef = (uint8_t)min<uint16_t>((uint16_t)pet.trDef + 8, pet.trMaxDef());
      used = pet.trDef != before;
      break;
    }
    case XITEM_SPE: {
      uint8_t before = pet.trSpe;
      pet.trSpe = (uint8_t)min<uint16_t>((uint16_t)pet.trSpe + 8, pet.trMaxSpe());
      used = pet.trSpe != before;
      break;
    }
    case XITEM_VITAL: {
      uint8_t e0 = pet.energy, j0 = pet.joy, f0 = pet.fullness;
      pet.energy = (uint8_t)min<int>(100, pet.energy + 40);
      pet.joy = (uint8_t)min<int>(100, pet.joy + 12);
      pet.fullness = (uint8_t)min<int>(100, pet.fullness + 8);
      used = (e0 != pet.energy || j0 != pet.joy || f0 != pet.fullness);
      break;
    }
    case XITEM_SHINY:
      if (!_shinyBoost) { _shinyBoost = true; used = true; }
      break;
    case XITEM_ENERGY: {
      uint8_t e0 = pet.energy;
      pet.energy = (uint8_t)min<int>(100, pet.energy + 70);
      used = pet.energy != e0;
      break;
    }
    case XITEM_IV_ATK:
      if (pet.ivAtk < 31) { pet.ivAtk++; used = true; }
      break;
    case XITEM_IV_DEF:
      if (pet.ivDef < 31) { pet.ivDef++; used = true; }
      break;
    case XITEM_IV_SPE:
      if (pet.ivSpe < 31) { pet.ivSpe++; used = true; }
      break;
    case XITEM_IV_HP:
      if (pet.ivHp < 31) { pet.ivHp++; used = true; }
      break;
    case XITEM_GOLD_CROWN: {
      bool any = false;
      if (pet.ivAtk < 31) { pet.ivAtk++; any = true; }
      if (pet.ivDef < 31) { pet.ivDef++; any = true; }
      if (pet.ivSpe < 31) { pet.ivSpe++; any = true; }
      if (pet.ivHp  < 31) { pet.ivHp++;  any = true; }
      used = any;
      break;
    }
    case XITEM_SHINY_BERRY:
      // Unlike XITEM_SHINY (the next-egg boost), this berry changes the living
      // companion immediately. Pet owns Pokedex registration internally;
      // GameExtras must never call Pet::registerSpecies(), which is private.
      used = pet.makeCurrentShiny();
      break;
  }
  if (!used) return false;
  _items[id]--;
  pet.saveNow();
  save();
  return true;
}

bool GameExtras::consumeShinyBoostForEgg() {
  if (!_shinyBoost) return false;
  _shinyBoost = false;
  save();
  return true;
}


// ---------- weather ----------

uint8_t GameExtras::weatherId(const Pet &pet) const {
  // Three-hour blocks keep weather noticeable without changing every time the
  // user opens a screen. Hash the block so consecutive blocks do not just walk
  // through the enum in a predictable order.
  uint32_t block = pet.lastSeenEpoch ? pet.lastSeenEpoch / 10800UL : pet.ageMinutes / 180UL;
  uint32_t x = block ^ 0x6D2B79F5UL;
  x ^= x >> 15; x *= 0x2C1B3C6DUL; x ^= x >> 12; x *= 0x297A2D39UL; x ^= x >> 15;
  return (uint8_t)(x % WEATHER_ACTIVE_COUNT);
}

const char *GameExtras::weatherNameKo(uint8_t id) const {
  static const char *const N[WEATHER_ACTIVE_COUNT] = { "맑음", "비", "눈", "모래바람", "뇌우" };
  return id < WEATHER_ACTIVE_COUNT ? N[id] : "맑음";
}

const char *GameExtras::weatherEffectKo(uint8_t id) const {
  static const char *const E[WEATHER_ACTIVE_COUNT] = {
    "불꽃·풀·페어리 탐험 보너스", "물 탐험·물타입 전투 강화", "얼음 탐험·방어 강화",
    "땅·바위·강철 탐험 보너스", "전기·비행 탐험·속도 강화"
  };
  return id < WEATHER_ACTIVE_COUNT ? E[id] : E[0];
}

bool GameExtras::weatherBoostsType(uint8_t weather, uint8_t type) const {
  if (type >= TYPE_COUNT) return false;
  switch (weather) {
    case WEATHER_CLEAR: return type == T_FIRE || type == T_GRASS || type == T_FAIRY;
    case WEATHER_RAIN:  return type == T_WATER;
    case WEATHER_SNOW:  return type == T_ICE;
    case WEATHER_SAND:  return type == T_GROUND || type == T_ROCK || type == T_STEEL;
    case WEATHER_STORM: return type == T_ELECTRIC || type == T_FLYING;
    default: return false;
  }
}

void GameExtras::applyWeatherToCombatant(Combatant &c, uint8_t weather) const {
  if (c.dex < 1 || c.dex > DEX_COUNT || weather >= WEATHER_ACTIVE_COUNT) return;
  uint8_t t1 = DEX_TBL[c.dex].type1, t2 = DEX_TBL[c.dex].type2;
  bool fav = weatherBoostsType(weather, t1) || weatherBoostsType(weather, t2);
  if (!fav) {
    if (weather == WEATHER_RAIN && (t1 == T_FIRE || t2 == T_FIRE))
      c.base[SI_ATK] = (uint16_t)((uint32_t)c.base[SI_ATK] * 95UL / 100UL);
  } else {
    switch (weather) {
      case WEATHER_CLEAR: c.base[SI_ATK] = (uint16_t)((uint32_t)c.base[SI_ATK] * 108UL / 100UL); break;
      case WEATHER_RAIN:  c.base[SI_ATK] = (uint16_t)((uint32_t)c.base[SI_ATK] * 112UL / 100UL); break;
      case WEATHER_SNOW:  c.base[SI_DEF] = (uint16_t)((uint32_t)c.base[SI_DEF] * 112UL / 100UL); break;
      case WEATHER_SAND:  c.base[SI_DEF] = (uint16_t)((uint32_t)c.base[SI_DEF] * 110UL / 100UL); break;
      case WEATHER_STORM:
        c.base[SI_SPE] = (uint16_t)((uint32_t)c.base[SI_SPE] * 112UL / 100UL);
        c.base[SI_ATK] = (uint16_t)((uint32_t)c.base[SI_ATK] * 106UL / 100UL); break;
    }
  }
  c.base[SI_SPA] = c.base[SI_ATK];
  c.base[SI_SPD] = c.base[SI_DEF];
}

// ---------- type exploration ----------

bool GameExtras::startExploration(uint8_t type, const Pet &pet) {
  if (type >= TYPE_COUNT || _exploreActive || _exploreReady || pet.isEgg() || pet.ceremony != CER_NONE) return false;
  _exploreType = type;
  _exploreMatch = pet.speciesId >= 1 && DEX_TBL[pet.speciesId].type1 == type;
  _exploreWeather = weatherId(pet);
  uint16_t mins = _exploreMatch ? 15 : 20;
  if (weatherBoostsType(_exploreWeather, type) && mins > 4) mins -= 3;
  _exploreActive = true;
  _exploreReady = false;
  _exploreRewardCount = 0;
  _exploreEndEpoch = pet.lastSeenEpoch ? pet.lastSeenEpoch + (uint32_t)mins * 60UL : 0;
  _exploreEndMinute = pet.ageMinutes + mins;
  save();
  return true;
}

uint16_t GameExtras::explorationMinutesLeft(const Pet &pet) const {
  if (!_exploreActive || _exploreReady) return 0;
  if (_exploreEndEpoch && pet.lastSeenEpoch) {
    if (pet.lastSeenEpoch >= _exploreEndEpoch) return 0;
    uint32_t sec = _exploreEndEpoch - pet.lastSeenEpoch;
    return (uint16_t)((sec + 59UL) / 60UL);
  }
  if (pet.ageMinutes >= _exploreEndMinute) return 0;
  uint32_t m = _exploreEndMinute - pet.ageMinutes;
  return m > 65535UL ? 65535 : (uint16_t)m;
}

void GameExtras::rollExplorationReward(Pet &pet) {
  _exploreRewardCount = 1;
  // Treasure-map pieces are deliberately uncommon but visible often enough to
  // make every expedition exciting. Matching-type runs still keep their strong
  // TM identity after the map roll.
  bool weatherFav = weatherBoostsType(_exploreWeather, _exploreType);
  int mapChance = (_exploreMatch ? 14 : 10) + (weatherFav ? 4 : 0);
  int roll = (int)random(100);
  if (roll < mapChance) {
    _exploreRewardKind = REWARD_MAP;
    _exploreRewardId = 0;
    return;
  }
  int tmChance = (_exploreMatch ? 52 : 30) + (weatherFav ? 10 : 0);
  if (roll < mapChance + tmChance) {
    _exploreRewardKind = REWARD_TM;
    _exploreRewardId = _exploreType;
    return;
  }
  _exploreRewardKind = REWARD_ITEM;
  int r = random(100);
  if (r < 8) _exploreRewardId = XITEM_SHINY;
  else {
    static const uint8_t COMMON[5] = { XITEM_ATK, XITEM_DEF, XITEM_SPE, XITEM_VITAL, XITEM_ENERGY };
    _exploreRewardId = COMMON[random(5)];
    if (random(100) < ((_exploreMatch ? 28 : 18) + (weatherFav ? 20 : 0))) _exploreRewardCount = 2;
  }
}

void GameExtras::updateExploration(Pet &pet) {
  if (!_exploreActive || _exploreReady) return;
  if (explorationMinutesLeft(pet)) return;
  _exploreActive = false;
  _exploreReady = true;
  rollExplorationReward(pet);
  save();
}

bool GameExtras::claimExploration(Pet &pet) {
  updateExploration(pet);
  if (!_exploreReady) return false;
  if (_exploreRewardKind == REWARD_TM) giveTm(_exploreRewardId, _exploreRewardCount);
  else if (_exploreRewardKind == REWARD_MAP) giveMapPiece(_exploreRewardCount);
  else giveItem(_exploreRewardId, _exploreRewardCount);
  _exploreReady = false;
  _exploreRewardCount = 0;
  _exploreEndEpoch = 0;
  _exploreEndMinute = 0;
  save();
  return true;
}

// ---------- treasure map ----------

void GameExtras::giveMapPiece(uint8_t count) {
  if (!count) return;
  _mapPieces = satAdd8(_mapPieces, count, 9);
  save();
}

bool GameExtras::startTreasureHunt() {
  if (_treasureState || _mapPieces < 3) return false;
  _mapPieces -= 3;
  _treasureState = 1;
  _treasureRewardCount = 0;
  save();
  return true;
}

bool GameExtras::chooseTreasureChest(uint8_t chest, const Pet &pet) {
  if (_treasureState != 1 || chest >= 3) return false;
  _treasureState = 2;
  _treasureRewardCount = 1;
  if (chest == 0) {
    // Growth chest: guaranteed bulk training supplies.
    static const uint8_t GROWTH[5] = { XITEM_ATK, XITEM_DEF, XITEM_SPE, XITEM_VITAL, XITEM_ENERGY };
    _treasureRewardKind = REWARD_ITEM;
    _treasureRewardId = GROWTH[random(5)];
    _treasureRewardCount = (uint8_t)(2 + random(2));
  } else if (chest == 1) {
    // Sparkle chest: the best Shiny-charmer, but can still contain useful candy.
    if (random(100) < 48) {
      _treasureRewardKind = REWARD_ITEM;
      _treasureRewardId = XITEM_SHINY;
    } else {
      _treasureRewardKind = REWARD_ITEM;
      _treasureRewardId = random(2) ? XITEM_ENERGY : XITEM_VITAL;
      _treasureRewardCount = 2;
    }
  } else {
    // Technique chest: two TMs, usually matching the active creature's type.
    _treasureRewardKind = REWARD_TM;
    if (!pet.isEgg() && pet.speciesId >= 1 && random(100) < 70)
      _treasureRewardId = DEX_TBL[pet.speciesId].type1;
    else
      _treasureRewardId = (uint8_t)random(TYPE_COUNT);
    _treasureRewardCount = 2;
  }
  save();
  return true;
}

bool GameExtras::claimTreasure() {
  if (_treasureState != 2 || !_treasureRewardCount) return false;
  if (_treasureRewardKind == REWARD_TM) giveTm(_treasureRewardId, _treasureRewardCount);
  else if (_treasureRewardKind == REWARD_MAP) giveMapPiece(_treasureRewardCount);
  else giveItem(_treasureRewardId, _treasureRewardCount);
  _treasureState = 0;
  _treasureRewardCount = 0;
  save();
  return true;
}

uint8_t GameExtras::bestTmMove(const Pet &pet, uint8_t type) const {
  if (type >= TYPE_COUNT || pet.isEgg()) return 0;
  uint8_t best = 0;
  int bestScore = -9999;
  // Early creatures receive a useful but not endgame-sized move. The cap rises
  // with level; at high levels the classic 90-ish power TMs become eligible.
  int cap = 45 + pet.level() / 2;
  if (cap > 105) cap = 105;
  for (uint8_t mv = 1; mv < MOVE_COUNT; mv++) {
    const MoveEntry &m = MOVE_TBL[mv];
    if (m.type != type || pet.knowsMove(mv)) continue;
    int score;
    if (m.cat == MC_STATUS) score = 12;
    else {
      if (m.power > cap) continue;
      score = 30 + m.power;
      if (m.acc && m.acc < 80) score -= 15;
      if (m.effect == EF_RECHARGE) score -= 30;
      if (m.effect == EF_RECOIL) score -= 12;
    }
    if (score > bestScore) { bestScore = score; best = mv; }
  }
  // If a very low level has no attack under the cap, allow the weakest attack.
  if (!best) {
    int weak = 9999;
    for (uint8_t mv = 1; mv < MOVE_COUNT; mv++) {
      const MoveEntry &m = MOVE_TBL[mv];
      if (m.type != type || m.cat == MC_STATUS || !m.power || pet.knowsMove(mv)) continue;
      if (m.power < weak) { weak = m.power; best = mv; }
    }
  }
  return best;
}

void GameExtras::rollEvent(Pet &pet) {
  // v3.61.8: make the IV-kit event slightly more common without flooding rewards.
  // 16% IV kit; the other seven event families share the remaining 84%.
  _eventId = random(100) < 16 ? 8 : (uint8_t)(1 + random(7));
  _eventRewardCount = 1;
  switch (_eventId) {
    case 1: // treasure box: one of the three training candies
      _eventRewardKind = REWARD_ITEM; _eventRewardId = (uint8_t)random(3); break;
    case 2: // trainer gift: TM matching primary type
      _eventRewardKind = REWARD_TM;
      _eventRewardId = (!pet.isEgg() && pet.speciesId > 0) ? DEX_TBL[pet.speciesId].type1 : (uint8_t)random(TYPE_COUNT);
      break;
    case 3: // star fragment: shiny charm
      _eventRewardKind = REWARD_ITEM; _eventRewardId = XITEM_SHINY; break;
    case 4: // berry grove: energy candy, sometimes two
      _eventRewardKind = REWARD_ITEM; _eventRewardId = XITEM_ENERGY; _eventRewardCount = random(4) == 0 ? 2 : 1; break;
    case 5: // training trace
      _eventRewardKind = REWARD_ITEM; _eventRewardId = (uint8_t)random(3); _eventRewardCount = 2; break;
    case 6: // strange machine: random TM
      _eventRewardKind = REWARD_TM; _eventRewardId = (uint8_t)random(TYPE_COUNT); break;
    case 7: // torn treasure map
      _eventRewardKind = REWARD_MAP; _eventRewardId = 0; break;
    default: // IV training kit: capsules, very rarely a Gold Crown
      _eventRewardKind = REWARD_ITEM;
      _eventRewardId = random(100) < 8 ? XITEM_GOLD_CROWN
                                      : (uint8_t)(XITEM_IV_ATK + random(4));
      break;
  }
  save();
}

void GameExtras::update(Pet &pet, bool allowEvent) {
  ensureDaily(pet);
  updateExploration(pet);
  if (pet.ageMinutes < _lastEventMinute) _lastEventMinute = pet.ageMinutes; // new creature
  if (!allowEvent || _eventId || pet.isEgg() || pet.ceremony != CER_NONE) return;
  if (pet.ageMinutes < _lastEventMinute + 20) return;
  _lastEventMinute = pet.ageMinutes;
  uint8_t pers = personalityIdFor(pet.speciesId, pet.ivAtk, pet.ivDef, pet.ivSpe, pet.ivHp);
  // Every 20 game minutes: 35%, or 50% for Lucky. Average ~57/40 min.
  int chance = pers == PERS_LUCKY ? 50 : 35;
  if ((int)random(100) < chance) rollEvent(pet); else save();
}

const char *GameExtras::eventTitleKo() const {
  static const char *const N[9] = {"", "반짝이는 상자!", "트레이너의 선물", "별빛 조각 발견!", "수상한 열매나무", "훈련 흔적 발견", "이상한 기계", "낡은 지도 조각!", "개체 훈련 키트!"};
  return _eventId < 9 ? N[_eventId] : "랜덤 이벤트";
}

const char *GameExtras::eventTextKo() const {
  static const char *const N[9] = {"", "안에서 육성 아이템을 찾았다.", "지나가던 트레이너가 선물을 건넸다.", "반짝이는 기운이 담긴 조각이다.", "튼튼해질 것 같은 열매가 열렸다.", "누군가 남긴 훈련 도구를 찾았다.", "타입의 힘이 담긴 장치를 발견했다.", "세 조각을 모으면 숨겨진 보물을 찾을 수 있다.", "포켓몬의 개체값을 단련하는 희귀 도구다."};
  return _eventId < 9 ? N[_eventId] : "무언가를 발견했다.";
}

void GameExtras::claimEvent() {
  if (!_eventId) return;
  if (_eventRewardKind == REWARD_TM) giveTm(_eventRewardId, _eventRewardCount);
  else if (_eventRewardKind == REWARD_MAP) giveMapPiece(_eventRewardCount);
  else giveItem(_eventRewardId, _eventRewardCount);
  _eventId = 0;
  _eventRewardCount = 0;
  save();
}

const char *GameExtras::towerBuffNameKo(uint8_t id) const {
  static const char *const N[TBUFF_COUNT] = { "공격 강화", "수비 강화", "속도 강화", "체력 강화", "균형 강화" };
  return id < TBUFF_COUNT ? N[id] : "강화";
}

const char *GameExtras::towerBuffEffectKo(uint8_t id) const {
  static const char *const N[TBUFF_COUNT] = {
    "공격 +8%", "방어 +8%", "스피드 +10%", "최대 HP +12%", "모든 능력 +4%"
  };
  return id < TBUFF_COUNT ? N[id] : "";
}

void GameExtras::rollTowerBuffChoices() {
  uint8_t pool[TBUFF_COUNT];
  for (uint8_t i = 0; i < TBUFF_COUNT; i++) pool[i] = i;
  for (uint8_t i = 0; i < 3; i++) {
    uint8_t j = (uint8_t)random(i, TBUFF_COUNT);
    uint8_t t = pool[i]; pool[i] = pool[j]; pool[j] = t;
    _towerBuffChoices[i] = pool[i];
  }
  _towerBuffPending = true;
}

bool GameExtras::chooseTowerBuff(uint8_t choiceIndex) {
  if (!_towerBuffPending || choiceIndex >= 3) return false;
  uint8_t id = _towerBuffChoices[choiceIndex];
  if (id >= TBUFF_COUNT) return false;
  if (_towerBuffs[id] < 5) _towerBuffs[id]++;
  _towerBuffPending = false;
  save();
  return true;
}

void GameExtras::applyTowerBuffs(Combatant &c) const {
  auto mul = [](uint16_t v, uint16_t pct) -> uint16_t {
    uint32_t n = (uint32_t)v * pct / 100UL;
    return n > 65535UL ? 65535 : (uint16_t)(n ? n : 1);
  };
  uint16_t atkPct = 100 + _towerBuffs[TBUFF_POWER] * 8 + _towerBuffs[TBUFF_BALANCE] * 4;
  uint16_t defPct = 100 + _towerBuffs[TBUFF_GUARD] * 8 + _towerBuffs[TBUFF_BALANCE] * 4;
  uint16_t spePct = 100 + _towerBuffs[TBUFF_SPEED] * 10 + _towerBuffs[TBUFF_BALANCE] * 4;
  uint16_t hpPct  = 100 + _towerBuffs[TBUFF_HP] * 12 + _towerBuffs[TBUFF_BALANCE] * 4;
  c.base[SI_ATK] = mul(c.base[SI_ATK], atkPct);
  c.base[SI_DEF] = mul(c.base[SI_DEF], defPct);
  c.base[SI_SPA] = c.base[SI_ATK];
  c.base[SI_SPD] = c.base[SI_DEF];
  c.base[SI_SPE] = mul(c.base[SI_SPE], spePct);
  c.maxHp = mul(c.maxHp, hpPct);
  c.hp = c.maxHp;
}

void GameExtras::towerWin(Pet &pet) {
  if (_towerStreak < 9999) _towerStreak++;
  if (_towerStreak > _towerBest) _towerBest = _towerStreak;
  if (_towerStreak % 4 == 0) giveMapPiece(1);
  if (_towerStreak % 5 == 0) {
    uint8_t t = (!pet.isEgg() && pet.speciesId > 0) ? DEX_TBL[pet.speciesId].type1 : (uint8_t)random(TYPE_COUNT);
    giveTm(t, 1);
  } else if (_towerStreak % 3 == 0) {
    static const uint8_t COMMON[5] = { XITEM_ATK, XITEM_DEF, XITEM_SPE, XITEM_VITAL, XITEM_ENERGY };
    giveItem(COMMON[random(5)], 1);
  }
  rollTowerBuffChoices();
  save();
}

void GameExtras::towerLose() {
  if (!_towerStreak && !_towerBuffPending) return;
  _towerStreak = 0;
  memset(_towerBuffs, 0, sizeof(_towerBuffs));
  _towerBuffPending = false;
  save();
}

uint8_t GameExtras::bossType(const Pet &pet) const {
  uint32_t d = dayFor(pet);
  return (uint8_t)((d * 7UL + 3UL) % TYPE_COUNT);
}

void GameExtras::bossWin(uint8_t type) {
  if (type >= TYPE_COUNT) type = T_NORMAL;
  bool first = !bossDefeated(type);
  _bossWinMask |= (1UL << type);
  _bossRewardCount = 1;
  int r = random(100);
  if (first) {
    _bossRewardKind = REWARD_TM;
    _bossRewardId = type;
    giveTm(type, 1);
  } else if (r < 18) {
    _bossRewardKind = REWARD_MAP;
    _bossRewardId = 0;
    giveMapPiece(1);
  } else if (r < 52) {
    _bossRewardKind = REWARD_TM;
    _bossRewardId = type;
    giveTm(type, 1);
  } else {
    _bossRewardKind = REWARD_ITEM;
    if (r < 60) _bossRewardId = XITEM_SHINY;
    else {
      static const uint8_t COMMON[5] = { XITEM_ATK, XITEM_DEF, XITEM_SPE, XITEM_VITAL, XITEM_ENERGY };
      _bossRewardId = COMMON[random(5)];
      if (random(100) < 25) _bossRewardCount = 2;
    }
    giveItem(_bossRewardId, _bossRewardCount);
  }
  save();
}

// ---------- rival trainer ----------

// ---------- rival special events ----------

void GameExtras::ensureRivalSpecial(Pet &pet) {
  if (!rivalReady(pet) || _rivalSpecial != RIVSPEC_NONE) return;
  // One roll per completed rival encounter. The serial is stored so simply
  // reopening the screen can never reroll until a battle changes the record.
  uint16_t serial = (uint16_t)(_rivalWins + _rivalLosses + 1);
  if (_rivalSpecialSerial == serial) return;
  _rivalSpecialSerial = serial;
  if (random(100) < 38) {
    _rivalSpecial = (uint8_t)(1 + random(3));
    if (_rivalSpecial == RIVSPEC_TRADE) {
      _rivalTradeWant = (uint8_t)random(TYPE_COUNT);
      _rivalTradeGive = (uint8_t)random(TYPE_COUNT);
      if (_rivalTradeGive == _rivalTradeWant) _rivalTradeGive = (uint8_t)((_rivalTradeGive + 5) % TYPE_COUNT);
    }
  }
  save();
}

const char *GameExtras::rivalSpecialTitleKo() const {
  switch (_rivalSpecial) {
    case RIVSPEC_GIFT: return "라이벌의 선물";
    case RIVSPEC_TRADE: return "기술머신 교환";
    case RIVSPEC_TREASURE_RACE: return "보물 경쟁";
    default: return "특별 이벤트";
  }
}

const char *GameExtras::rivalSpecialTextKo() const {
  switch (_rivalSpecial) {
    case RIVSPEC_GIFT: return "민호가 탐험에서 찾은 물건을 건넸다.";
    case RIVSPEC_TRADE: return "서로 필요한 기술머신을 하나씩 바꾸자고 한다.";
    case RIVSPEC_TREASURE_RACE: return "누가 먼저 보물지도를 찾는지 승부하자고 한다!";
    default: return "";
  }
}

bool GameExtras::claimRivalGift() {
  if (_rivalSpecial != RIVSPEC_GIFT) return false;
  static const uint8_t G[6] = { XITEM_ATK, XITEM_DEF, XITEM_SPE, XITEM_VITAL, XITEM_ENERGY, XITEM_SHINY };
  uint8_t id = G[random(6)];
  giveItem(id, id == XITEM_SHINY ? 1 : (uint8_t)(1 + (random(100) < 25)));
  _rivalSpecial = RIVSPEC_NONE;
  save();
  return true;
}

bool GameExtras::acceptRivalTrade() {
  if (_rivalSpecial != RIVSPEC_TRADE || _rivalTradeWant >= TYPE_COUNT || _rivalTradeGive >= TYPE_COUNT) return false;
  if (!_tms[_rivalTradeWant]) return false;
  _tms[_rivalTradeWant]--;
  _tms[_rivalTradeGive] = satAdd8(_tms[_rivalTradeGive], 1);
  _rivalSpecial = RIVSPEC_NONE;
  save();
  return true;
}

void GameExtras::dismissRivalSpecial() {
  if (_rivalSpecial == RIVSPEC_NONE) return;
  _rivalSpecial = RIVSPEC_NONE;
  save();
}

uint8_t GameExtras::rivalStage(const Pet &pet) const {
  uint16_t progress = _rivalWins + _rivalLosses / 2 + pet.badgeCount(false) / 4 + _towerBest / 5;
  return progress > 9 ? 9 : (uint8_t)progress;
}

uint16_t GameExtras::rivalMinutesLeft(const Pet &pet) const {
  if (_rivalNextEpoch && pet.lastSeenEpoch) {
    if (pet.lastSeenEpoch >= _rivalNextEpoch) return 0;
    uint32_t sec = _rivalNextEpoch - pet.lastSeenEpoch;
    return (uint16_t)((sec + 59UL) / 60UL);
  }
  if (pet.ageMinutes >= _rivalNextMinute) return 0;
  uint32_t m = _rivalNextMinute - pet.ageMinutes;
  return m > 65535UL ? 65535 : (uint16_t)m;
}

void GameExtras::rivalResult(bool playerWon, Pet &pet) {
  bool treasureRace = (_rivalSpecial == RIVSPEC_TREASURE_RACE);
  _rivalRewardCount = 0;
  if (playerWon) {
    if (_rivalWins < 9999) _rivalWins++;
    _rivalRewardCount = 1;
    int r = random(100);
    if (r < 22) {
      _rivalRewardKind = REWARD_MAP;
      _rivalRewardId = 0;
      giveMapPiece(1);
    } else if (r < 48) {
      _rivalRewardKind = REWARD_TM;
      _rivalRewardId = (!pet.isEgg() && pet.speciesId > 0) ? DEX_TBL[pet.speciesId].type1 : (uint8_t)random(TYPE_COUNT);
      giveTm(_rivalRewardId, 1);
    } else {
      _rivalRewardKind = REWARD_ITEM;
      static const uint8_t COMMON[5] = { XITEM_ATK, XITEM_DEF, XITEM_SPE, XITEM_VITAL, XITEM_ENERGY };
      _rivalRewardId = COMMON[random(5)];
      _rivalRewardCount = random(100) < 35 ? 2 : 1;
      giveItem(_rivalRewardId, _rivalRewardCount);
    }
    if (treasureRace) {
      // A treasure race is deliberately additive: the ordinary rival reward
      // remains, and winning the event guarantees one map piece.
      giveMapPiece(1);
    }
  } else {
    if (_rivalLosses < 9999) _rivalLosses++;
  }
  _rivalSpecial = RIVSPEC_NONE;
  _rivalNextEpoch = pet.lastSeenEpoch ? pet.lastSeenEpoch + 30UL * 60UL : 0;
  _rivalNextMinute = pet.ageMinutes + 30UL;
  save();
}

void GameExtras::addResearch(int16_t dex, uint8_t amount) {
  if (dex < 1 || dex > DEX_COUNT || !amount) return;
  uint16_t z = (uint16_t)(dex - 1);
  uint16_t bi = z >> 1;
  uint8_t shift = (z & 1) ? 4 : 0;
  uint8_t old = (_research[bi] >> shift) & 0x0F;
  uint8_t now = satAdd8(old, amount, 15);
  _research[bi] = (uint8_t)((_research[bi] & ~(0x0Fu << shift)) | (now << shift));
  if (now != old) save();
}

uint8_t GameExtras::researchScore(int16_t dex) const {
  if (dex < 1 || dex > DEX_COUNT) return 0;
  uint16_t z = (uint16_t)(dex - 1), bi = z >> 1;
  uint8_t shift = (z & 1) ? 4 : 0;
  return (_research[bi] >> shift) & 0x0F;
}

uint8_t GameExtras::researchLevel(int16_t dex) const {
  uint8_t s = researchScore(dex);
  if (s >= 15) return 5;
  if (s >= 10) return 4;
  if (s >= 6) return 3;
  if (s >= 3) return 2;
  if (s >= 1) return 1;
  return 0;
}

void GameExtras::observePet(const Pet &pet) {
  int16_t d = pet.isEgg() ? 0 : pet.speciesId;
  if (d == _observedDex) return;
  _observedDex = d;
  if (d >= 1 && d <= DEX_COUNT) addResearch(d, 2); // hatch/evolution/newly migrated save
  else save();
}

void GameExtras::syncObservedPet(const Pet &pet) {
  _observedDex = pet.isEgg() ? 0 : pet.speciesId;
  prefs.putShort("xobs", _observedDex);
}
