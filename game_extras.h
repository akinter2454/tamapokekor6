#pragma once
#include <Arduino.h>
#include <Preferences.h>
#include "dex.h"
#include "moves.h"

class Pet;
struct Combatant;

// Event/mission inventory items. Basic berries and the original sweet candy
// stay unlimited and are handled by Pet; these are earned consumables.
enum ExtraItemId : uint8_t {
  XITEM_ATK = 0,
  XITEM_DEF,
  XITEM_SPE,
  XITEM_VITAL,
  XITEM_SHINY,
  XITEM_ENERGY,
  // v3.61.6+: IV growth items are appended so legacy xitem[] save prefixes
  // keep exactly the same meaning. The four IV berries raise one IV by +1;
  // Gold Crown raises every non-perfect IV by +1. v3.63.1 appends a separate
  // Shiny Berry that transforms the CURRENT Pokemon; XITEM_SHINY above stays
  // the original next-egg Shiny boost and its ID/meaning never changes.
  XITEM_IV_ATK,
  XITEM_IV_DEF,
  XITEM_IV_SPE,
  XITEM_IV_HP,
  XITEM_GOLD_CROWN,
  XITEM_SHINY_BERRY,
  XITEM_COUNT
};

enum MissionKind : uint8_t {
  MIS_FEED = 0,
  MIS_PLAY,
  MIS_TRAIN,
  MIS_CLEAN,
  MIS_BATTLE,
  MIS_CARE,
  MIS_KIND_COUNT
};

enum RewardKind : uint8_t { REWARD_ITEM = 0, REWARD_TM = 1, REWARD_MAP = 2 };

// v3.25 weather changes every few hours and influences exploration/battles.
enum WeatherId : uint8_t {
  WEATHER_CLEAR = 0, WEATHER_RAIN, WEATHER_SNOW, WEATHER_SAND, WEATHER_STORM, WEATHER_FOG,
  WEATHER_COUNT
};
// WEATHER_FOG is retained only as a legacy save value. Active weather choices
// are IDs 0..4; keeping the old enum value avoids breaking existing NVS data.
#define WEATHER_ACTIVE_COUNT WEATHER_FOG

// Special rival encounters that can appear when the rematch cooldown ends.
enum RivalSpecialId : uint8_t {
  RIVSPEC_NONE = 0, RIVSPEC_GIFT, RIVSPEC_TRADE, RIVSPEC_TREASURE_RACE
};

enum TowerBuffId : uint8_t {
  TBUFF_POWER = 0, TBUFF_GUARD, TBUFF_SPEED, TBUFF_HP, TBUFF_BALANCE,
  TBUFF_COUNT
};

class GameExtras {
public:
  void begin();
  void update(Pet &pet, bool allowEvent);
  void observePet(const Pet &pet);
  void syncObservedPet(const Pet &pet);  // tab switch/boot without awarding research

  // Daily missions ---------------------------------------------------------
  void ensureDaily(const Pet &pet);
  void missionAction(uint8_t kind, uint8_t amount, Pet &pet);
  uint8_t missionKind(uint8_t i) const { return i < 3 ? _missionKind[i] : 0; }
  uint8_t missionProgress(uint8_t i) const { return i < 3 ? _missionProg[i] : 0; }
  uint8_t missionGoal(uint8_t i) const;
  bool missionClaimed(uint8_t i) const { return i < 3 && (_missionClaimMask & (1u << i)); }
  bool missionComplete(uint8_t i) const { return i < 3 && _missionProg[i] >= missionGoal(i); }
  uint8_t missionRewardKind(uint8_t i) const;
  uint8_t missionRewardId(uint8_t i, const Pet &pet) const;
  bool claimMission(uint8_t i, Pet &pet);
  const char *missionNameKo(uint8_t kind) const;

  // Items / TM -------------------------------------------------------------
  uint8_t itemCount(uint8_t id) const { return id < XITEM_COUNT ? _items[id] : 0; }
  uint8_t tmCount(uint8_t type) const { return type < TYPE_COUNT ? _tms[type] : 0; }
  const char *itemNameKo(uint8_t id) const;
  const char *itemEffectKo(uint8_t id) const;
  bool useItem(uint8_t id, Pet &pet);
  void giveItem(uint8_t id, uint8_t count = 1);
  void giveTm(uint8_t type, uint8_t count = 1);
  bool consumeTm(uint8_t type);

  // v3.62.9: training can award mission progress + IV items in one result.
  // Batch those writes so rapid minigame completion never performs several
  // full Preferences commits back-to-back on the render loop. endBatch(false)
  // leaves one consolidated save pending for the main loop to flush safely.
  void beginBatch();
  void endBatch(bool flushNow = true);
  bool savePending() const { return _saveDirty && _saveBatchDepth == 0; }
  void flushPendingSave();
  uint8_t bestTmMove(const Pet &pet, uint8_t type) const;
  bool shinyBoostArmed() const { return _shinyBoost; }
  bool consumeShinyBoostForEgg();

  // Weather ---------------------------------------------------------------
  uint8_t weatherId(const Pet &pet) const;
  const char *weatherNameKo(uint8_t id) const;
  const char *weatherEffectKo(uint8_t id) const;
  bool weatherBoostsType(uint8_t weather, uint8_t type) const;
  void applyWeatherToCombatant(Combatant &c, uint8_t weather) const;

  // Type exploration -------------------------------------------------------
  bool explorationActive() const { return _exploreActive; }
  bool explorationReady() const { return _exploreReady; }
  uint8_t explorationType() const { return _exploreType; }
  uint8_t explorationWeather() const { return _exploreWeather; }
  uint16_t explorationMinutesLeft(const Pet &pet) const;
  bool startExploration(uint8_t type, const Pet &pet);
  void updateExploration(Pet &pet);
  uint8_t explorationRewardKind() const { return _exploreRewardKind; }
  uint8_t explorationRewardId() const { return _exploreRewardId; }
  uint8_t explorationRewardCount() const { return _exploreRewardCount; }
  bool claimExploration(Pet &pet);

  // Treasure map -----------------------------------------------------------
  uint8_t mapPieces() const { return _mapPieces; }
  bool mapComplete() const { return _mapPieces >= 3; }
  bool treasureChoosing() const { return _treasureState == 1; }
  bool treasureRewardReady() const { return _treasureState == 2; }
  bool startTreasureHunt();
  bool chooseTreasureChest(uint8_t chest, const Pet &pet);
  bool claimTreasure();
  uint8_t treasureRewardKind() const { return _treasureRewardKind; }
  uint8_t treasureRewardId() const { return _treasureRewardId; }
  uint8_t treasureRewardCount() const { return _treasureRewardCount; }
  void giveMapPiece(uint8_t count = 1);

  // Random events ----------------------------------------------------------
  bool hasEvent() const { return _eventId != 0; }
  uint8_t eventId() const { return _eventId; }
  const char *eventTitleKo() const;
  const char *eventTextKo() const;
  uint8_t eventRewardKind() const { return _eventRewardKind; }
  uint8_t eventRewardId() const { return _eventRewardId; }
  uint8_t eventRewardCount() const { return _eventRewardCount; }
  void claimEvent();

  // Battle tower -----------------------------------------------------------
  uint16_t towerStreak() const { return _towerStreak; }
  uint16_t towerBest() const { return _towerBest; }
  bool towerBuffPending() const { return _towerBuffPending; }
  uint8_t towerBuffChoice(uint8_t i) const { return i < 3 ? _towerBuffChoices[i] : 0; }
  uint8_t towerBuffLevel(uint8_t id) const { return id < TBUFF_COUNT ? _towerBuffs[id] : 0; }
  const char *towerBuffNameKo(uint8_t id) const;
  const char *towerBuffEffectKo(uint8_t id) const;
  bool chooseTowerBuff(uint8_t choiceIndex);
  void applyTowerBuffs(Combatant &c) const;
  void towerWin(Pet &pet);
  void towerLose();

  // Three-care-slot daily type boss ---------------------------------------
  uint8_t bossType(const Pet &pet) const;
  bool bossDefeated(uint8_t type) const { return type < TYPE_COUNT && (_bossWinMask & (1UL << type)); }
  void bossWin(uint8_t type);
  uint8_t bossRewardKind() const { return _bossRewardKind; }
  uint8_t bossRewardId() const { return _bossRewardId; }
  uint8_t bossRewardCount() const { return _bossRewardCount; }

  // Rival trainer ----------------------------------------------------------
  uint16_t rivalWins() const { return _rivalWins; }
  uint16_t rivalLosses() const { return _rivalLosses; }
  uint8_t rivalStage(const Pet &pet) const;
  uint16_t rivalMinutesLeft(const Pet &pet) const;
  bool rivalReady(const Pet &pet) const { return rivalMinutesLeft(pet) == 0; }
  void rivalResult(bool playerWon, Pet &pet);
  uint8_t rivalRewardKind() const { return _rivalRewardKind; }
  uint8_t rivalRewardId() const { return _rivalRewardId; }
  uint8_t rivalRewardCount() const { return _rivalRewardCount; }
  const char *rivalNameKo() const { return "라이벌 민호"; }
  uint8_t rivalSpecial() const { return _rivalSpecial; }
  bool rivalHasSpecial() const { return _rivalSpecial != RIVSPEC_NONE; }
  const char *rivalSpecialTitleKo() const;
  const char *rivalSpecialTextKo() const;
  uint8_t rivalTradeWantType() const { return _rivalTradeWant; }
  uint8_t rivalTradeGiveType() const { return _rivalTradeGive; }
  void ensureRivalSpecial(Pet &pet);
  bool claimRivalGift();
  bool acceptRivalTrade();
  void dismissRivalSpecial();

  // Pokedex research -------------------------------------------------------
  void addResearch(int16_t dex, uint8_t amount);
  uint8_t researchScore(int16_t dex) const;
  uint8_t researchLevel(int16_t dex) const;

private:
  Preferences prefs;
  uint8_t _items[XITEM_COUNT] = {0};
  uint8_t _tms[TYPE_COUNT] = {0};
  bool _shinyBoost = false;

  bool _exploreActive = false;
  bool _exploreReady = false;
  bool _exploreMatch = false;
  uint8_t _exploreType = T_NORMAL;
  uint32_t _exploreEndEpoch = 0;
  uint32_t _exploreEndMinute = 0;
  uint8_t _exploreRewardKind = REWARD_ITEM;
  uint8_t _exploreRewardId = 0;
  uint8_t _exploreRewardCount = 0;
  uint8_t _exploreWeather = WEATHER_CLEAR;

  uint8_t _mapPieces = 0;
  uint8_t _treasureState = 0;   // 0 idle, 1 choose chest, 2 reward waiting
  uint8_t _treasureRewardKind = REWARD_ITEM;
  uint8_t _treasureRewardId = 0;
  uint8_t _treasureRewardCount = 0;

  uint32_t _missionDay = 0;
  uint8_t _missionKind[3] = {0, 1, 2};
  uint8_t _missionProg[3] = {0, 0, 0};
  uint8_t _missionClaimMask = 0;

  // 4 bits/species: score 0..15, enough for five research levels.
  uint8_t _research[(DEX_COUNT + 1) / 2] = {0};
  int16_t _observedDex = 0;

  uint16_t _towerStreak = 0;
  uint16_t _towerBest = 0;
  uint8_t _towerBuffs[TBUFF_COUNT] = {0};
  bool _towerBuffPending = false;
  uint8_t _towerBuffChoices[3] = {0, 1, 2};

  uint32_t _bossWinMask = 0;
  uint8_t _bossRewardKind = REWARD_ITEM;
  uint8_t _bossRewardId = 0;
  uint8_t _bossRewardCount = 0;

  uint16_t _rivalWins = 0;
  uint16_t _rivalLosses = 0;
  uint32_t _rivalNextEpoch = 0;
  uint32_t _rivalNextMinute = 0;
  uint8_t _rivalRewardKind = REWARD_ITEM;
  uint8_t _rivalRewardId = 0;
  uint8_t _rivalRewardCount = 0;
  uint8_t _rivalSpecial = RIVSPEC_NONE;
  uint8_t _rivalTradeWant = T_NORMAL;
  uint8_t _rivalTradeGive = T_FIRE;
  uint16_t _rivalSpecialSerial = 0;

  uint8_t _eventId = 0;           // 0 none, 1..8 event
  uint8_t _eventRewardKind = 0;
  uint8_t _eventRewardId = 0;
  uint8_t _eventRewardCount = 0;
  uint32_t _lastEventMinute = 0;

  uint8_t _saveBatchDepth = 0;
  bool _saveDirty = false;

  uint32_t dayFor(const Pet &pet) const;
  void rollMissions(uint32_t day);
  void rollEvent(Pet &pet);
  void rollExplorationReward(Pet &pet);
  void rollTowerBuffChoices();
  void save();
  void saveNow();
};

extern GameExtras extras;
