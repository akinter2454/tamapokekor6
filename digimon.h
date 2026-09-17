#pragma once
#include <Arduino.h>
#include <Preferences.h>
#include "moves.h"
#include "dex.h"
#include "types.h"

enum DigiStage : uint8_t { DIGI_BABY1, DIGI_BABY2, DIGI_CHILD, DIGI_ADULT, DIGI_PERFECT, DIGI_ULTIMATE };
enum DigiTrain : uint8_t { DIGI_ATK, DIGI_DEF, DIGI_SPE, DIGI_HP };

struct DigiSpecies {
  const char *name;
  uint8_t version, stage, power, style;
};

// These declarations must appear before DigiPet because two inline member
// functions below reference the catalog count during header compilation.
extern const DigiSpecies DIGI_SPECIES[];
extern const uint16_t DIGI_SPECIES_COUNT;
static const uint16_t DIGI_SPECIES_CAP = 320;
static const uint16_t DIGI_NO_FUSION = 0xFFFF;

// Digimon share Pet/PartyMon/Combatant with Pokemon. Pokemon keep their exact
// National Dex values; Digimon live in a disjoint positive range so old saves
// and future Pokemon additions cannot collide.
static const int16_t DIGI_CREATURE_BASE = 2000;
static inline bool isDigimonId(int16_t id) { return id >= DIGI_CREATURE_BASE && id < DIGI_CREATURE_BASE + DIGI_SPECIES_COUNT; }
static inline int16_t makeDigimonId(uint16_t id) { return (int16_t)(DIGI_CREATURE_BASE + id); }
static inline uint16_t digimonIndex(int16_t id) { return isDigimonId(id) ? (uint16_t)(id - DIGI_CREATURE_BASE) : 0; }
static inline bool isCreatureId(int16_t id) { return (id >= 1 && id <= DEX_COUNT) || isDigimonId(id); }

const char *creatureName(int16_t id);
const char *digimonNameKo(uint16_t index);
uint8_t creatureType1(int16_t id);
uint8_t creatureType2(int16_t id);
uint8_t creatureBaseAtk(int16_t id);
uint8_t creatureBaseDef(int16_t id);
uint8_t creatureBaseSpe(int16_t id);
uint8_t creatureBaseHp(int16_t id);
uint8_t creatureBaseSpA(int16_t id);
uint8_t creatureBaseSpD(int16_t id);
uint16_t creatureAccent(int16_t id);
uint8_t digimonSignatureMove(uint16_t index);
uint8_t digimonLearnableMoves(uint16_t index, uint8_t level, uint8_t *out, uint8_t max);
void digimonDefaultMoves(uint16_t index, uint8_t level, uint8_t ivAtk,
                         uint8_t ivDef, uint8_t ivSpe, uint8_t ivHp,
                         uint8_t out[4]);
bool digimonMovesNeedRefresh(const uint8_t moves[4]);
uint16_t digimonEvolutionTarget(uint16_t index, uint8_t level, uint8_t atkRuns,
                               uint8_t defRuns, uint8_t speRuns, uint8_t hpRuns,
                               const uint8_t *bestLevels);
// Returns the fixed ATK/DEF/SPE/HP branches for this individual species.
// Several training styles may intentionally share a target when a canon-like
// family has fewer than four sensible next-stage branches.
uint8_t digimonEvolutionBranches(uint16_t index, uint16_t out[4]);
uint8_t digimonEvolutionLevel(uint16_t index);
bool digimonHasEvolutionPotential(uint16_t index);
bool isPendulumFusionSpecies(uint16_t index);
const char *digimonDeviceLabel(uint8_t version);

class DigiPet {
public:
  void begin();
  void update(uint32_t nowMs);
  void start(uint8_t version);
  void startEgg(uint8_t version);
  bool tapEgg();
  void train(DigiTrain kind, uint8_t amount = 1);
  bool canEvolve() const;
  bool evolve();
  uint16_t fusionTarget() const;
  uint8_t bestLevelFor(uint16_t id) const { return id<DIGI_SPECIES_COUNT ? bestLevel[id] : 0; }
  uint16_t stat(DigiTrain kind) const;
  uint8_t type1() const;
  uint8_t type2() const;
  bool hasStab(uint8_t moveType) const { return moveType==type1() || moveType==type2(); }
  void relearnMoves();
  uint8_t signatureMove() const;
  uint8_t level() const;
  const DigiSpecies &species() const;
  void save();

  bool enabled = false;
  bool egg = false;
  uint8_t eggTaps = 0;
  uint16_t speciesId = 0;
  uint8_t version = 1;
  uint8_t iv[4] = {16,16,16,16};
  uint16_t training[4] = {0,0,0,0};
  uint8_t moves[4] = {MV_TACKLE,MV_NONE,MV_NONE,MV_NONE};
  uint32_t levelMinutes = 0;
  uint32_t lastTick = 0;
  uint8_t registered[(DIGI_SPECIES_CAP+7)/8] = {0};
  // Highest level ever raised for each species. Fusion materials remain
  // qualified after starting another Digital Monster egg.
  uint8_t bestLevel[DIGI_SPECIES_CAP] = {0};
  bool isRegistered(uint16_t id) const { return id<DIGI_SPECIES_COUNT && (registered[id>>3]&(1<<(id&7))); }
  uint16_t registeredCount() const;
  uint16_t displayIndex() const;
private:
  Preferences prefs;
  uint16_t chooseEvolution() const;
  void recordCurrentLevel();
};

extern DigiPet digiPet;

static const uint8_t DIGI_OMNIMON_ALTER_S = 86;
static const uint8_t DIGI_CHAOSMON = 87;
static const uint8_t DIGI_MILLENNIUMMON = 88;
static const uint8_t DIGI_CHAOSDRAMON = 89;
static inline bool isDigiFusionSpecies(uint16_t id) { return (id>=DIGI_OMNIMON_ALTER_S && id<=DIGI_MILLENNIUMMON) || isPendulumFusionSpecies(id); }
static inline bool isDigiExtraSpecies(uint16_t id) { return id>=DIGI_OMNIMON_ALTER_S && id<=DIGI_CHAOSDRAMON; }
