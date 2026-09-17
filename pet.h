#pragma once
#include <Arduino.h>
#include <Preferences.h>
#include "dex.h"
#include "trainers.h"   // GYM_REGIONS sizes the badge masks
#include "party.h"
#include "digimon.h"

// 1 tick = 1 minuto de juego. Baja este valor para probar mas rapido
// (p. ej. 5000UL = las estadisticas caen 12x mas rapido).
#define PET_TICK_MS 60000UL
// v3.57.5 growth tempo: awake pets gain 1 level every 2 real minutes, while
// sleeping pets keep growing at the gentler old-life pace of 1 level / 10 min.
// levelMinutes stays in "awake-minute equivalents" so existing v3.57.4 saves
// preserve their visible level. Five sleeping minutes earn one levelMinute.
#define MINUTES_PER_LEVEL 2
#define SLEEP_MINUTES_PER_LEVEL 10
#define SLEEP_PROGRESS_QUANTUM (SLEEP_MINUTES_PER_LEVEL / MINUTES_PER_LEVEL)
#if (SLEEP_MINUTES_PER_LEVEL % MINUTES_PER_LEVEL) != 0
#error "SLEEP_MINUTES_PER_LEVEL must be divisible by MINUTES_PER_LEVEL"
#endif
#define LEGACY_MINUTES_PER_LEVEL 10
#define LEVEL_MINUTES_UNSET 0xFFFFFFFFUL
#define MAX_LEVEL 100              // 99 subidas x 2 min = ~3 h 18 min despierto
#define EAT_ANIM_MS 2500UL
#define HEART_MS 1500UL
#define EVOLVE_ANIM_MS 5200UL              // animacion de evolucion (mas larga = mas epica)
#define CEREMONY_MS 10000UL                // duracion de la despedida en pantalla
#define FAREWELL_AGE_MIN 0UL               // immediate good farewell after hatching
// Early retirement still delays the next creature, but the old v3.57 value
// was 144 LEVELS (24 h / the former 10-min clock). With MAX_LEVEL=100 that
// could make ordinary Lv.16-64 evolutions display as Lv.160+ and become
// impossible. v3.61.9 normalises the debt to a modest 12 levels and the
// effective gate is always clamped to MAX_LEVEL. Old saves carrying 144 are
// migrated on load.
#define EVO_PENALTY_LEVELS ((uint8_t)0)
#define RUNAWAY_TICKS 60                   // se escapa tras 1 h con TODO a cero
// Night, by the RTC: midnight to 06:00. Auto-sleep needs BOTH: the screen off
// AND this window.
// The screen alone would pause the game every time you put the device in a
// pocket -- the creature is supposed to get hungry during the day. The hour
// alone would send it to bed while you were playing. Set the clock in SETTINGS
// or over the console with RTCSET; an unset board reads months out and simply
// never auto-sleeps, which fails in the safe direction (it still drains, and
// the light button still works by hand).
#define NIGHT_START 0
#define NIGHT_END 6
enum : uint8_t { SLEEP_NONE = 0, SLEEP_AUTO, SLEEP_PLAYER };
#define DEF_TRAIN_TICKS 60                 // minutos de bienestar por +1 de DEF
// v3.89 casual energy balance. These constants are shared by live ticking,
// power-off catch-up and inactive care slots so switching tabs cannot produce
// a different recovery/drain rate.
#define ENERGY_SLEEP_RECOVERY 10
#define ENERGY_AWAKE_DECAY_MINUTES 6
#define ENERGY_DEF_BASE_COST 3
#define ENERGY_DEF_SCORE_DIVISOR 12
#define ENERGY_DEF_MAX_COST 9
#define ENERGY_ATK_COST 4
#define ENERGY_SPE_COST 3
#define ENERGY_HP_COST 3
#define ENERGY_PLAY_COST 2

// ceremonias de fin de ciclo
enum : uint8_t { CER_NONE = 0, CER_FAREWELL, CER_RUNAWAY, CER_RELEASE };

enum PetMood : uint8_t { MOOD_HAPPY, MOOD_SAD, MOOD_EATING, MOOD_SLEEPING };

// medallas del individuo (bitmask)
enum : uint16_t {
  MED_LV10 = 1 << 0, MED_LV25 = 1 << 1, MED_LV50 = 1 << 2,
  MED_BERRY = 1 << 3, MED_STREAK7 = 1 << 4, MED_BOND = 1 << 5,
  MED_FINAL = 1 << 6, MED_FIT = 1 << 7,
};
#define MED_COUNT 8

uint8_t moveUnlockLevel(int16_t dex, uint8_t idx);

// ---------------------------------------------------------------------------
// Which regions have their sprite pack on the microSD.
//
// One bit per region and THE DEFAULT IS ALL SET, which is doing two jobs: a
// board with no SD at all keeps today's behaviour rather than presenting an
// empty game, and pet.cpp stays free of any SD dependency. That second part is
// not tidiness -- pet.cpp is compiled into all 33 test binaries and none of
// them link sdmon.cpp, so a direct call would mean stubbing it 33 times. The
// firmware narrows the mask ONCE, after the card mounts. Same shape as
// link.cpp taking a transport pointer instead of calling ESP-NOW across a
// layer, and for the same reason: the tests can drive it.
//
// uint16_t, not uint8_t. REGION_COUNT is 5 today and this project has been
// bitten five separate times by a width chosen for the current size of the dex.
extern uint16_t gRegionArt;
static_assert(REGION_COUNT <= 16, "gRegionArt needs a bit per region");

// Is this region playable? REGION_ALL is available while ANY real region is --
// it is the mixed pool and must not vanish because one pack is missing.
bool regionAvailable(uint8_t r);

// Which region a dex number belongs to, or REGION_ALL if somehow none. The
// regions tile the dex exactly once (dexdata_test pins that), so this is a
// lookup rather than a judgement.
uint8_t regionOfDex(int16_t d);
// Explicit region membership is non-contiguous once regional/form sprites are
// appended. These helpers enumerate only real, enabled PMDCollab entries.
uint16_t regionDexCount(uint8_t r);
int16_t regionDexAt(uint8_t r, uint16_t ordinal);
bool dexHasEvolution(int16_t d);
uint8_t dexEvolutionLevel(int16_t d);
// One canonical, overflow-safe answer for UI and evolution checks. The base
// catalog level plus care/retirement delay can never exceed MAX_LEVEL.
uint8_t effectiveEvolutionLevel(int16_t d, uint8_t careMistakes, uint8_t evoPenalty);
#define MAX_EVO_OPTIONS 128

// The next region the player can actually choose, skipping any whose sprite
// pack is missing. The egg pill cycles with this rather than (region + 1) %
// REGION_COUNT, which would land on a locked region and silently do nothing.
uint8_t nextAvailableRegion(uint8_t from);

// v3.20 live-raising slots -------------------------------------------------
// The old PartyMon is deliberately NOT reused here: it represents a retired,
// frozen battler and therefore has no hunger/energy/age/bond state. A care slot
// needs the complete per-creature raising state so several pets can keep aging
// while the player switches between them. Player-wide state (Pokedex, badges,
// trainer name, streak, records, inventory) stays on Pet/GameExtras and is not
// duplicated per slot.
#define CARE_SNAPSHOT_MAGIC 0x43533230UL  // "CS20"
#define CARE_LEGACY_REGIONS 8
struct CareSnapshot {
  uint32_t magic = CARE_SNAPSHOT_MAGIC;
  uint32_t parkedEpoch = 0;
  uint8_t fullness = 80, joy = 80, energy = 80, hygiene = 100;
  uint8_t poops = 0, weight = 0;
  uint8_t ivAtk = 16, ivDef = 16, ivSpe = 16, ivHp = 16;
  uint8_t trAtk = 0, trDef = 0, trSpe = 0;
  uint8_t berryKnown = 0, shiny = 0, sleeping = 0, frozen = 0;
  uint32_t ageMinutes = 0;
  int16_t speciesId = -1;
  uint8_t careMistakes = 0, bond = 0;
  char nick[12] = "";
  uint16_t medals = 0;
  uint8_t moves[MOVE_SLOTS] = {0, 0, 0, 0};
  uint8_t lastLearnLevel = 0;
  int16_t eggTarget = 1;
  uint8_t eggShiny = 0, eggTaps = 0;
  uint8_t mistakeCooldown = 0, evoDeclinedLv = 0;
  uint32_t farDeclinedAge = 0;
  uint8_t starterPick = 0, evoPen = 0, retirePending = 0;
  uint8_t neglectTicks = 0;
  uint16_t goodTicks = 0;
  uint8_t bondToday = 0, sleepAuto = 0;
  // Keep the v3.58 array width in-place so old raw snapshots retain the exact
  // offsets of levelMinutes/sleepLevelRemainder. New region cache slots are
  // append-only at the end of this struct.
  int16_t eggByRegion[CARE_LEGACY_REGIONS] = {0};
  // v3.57.4+: level progress is separate from total age. v3.57.5 also keeps
  // the 0..4 sleeping-minute remainder so 10-min sleep growth remains exact
  // across power cycles and care-slot switches.
  uint32_t levelMinutes = LEVEL_MINUTES_UNSET;
  uint8_t sleepLevelRemainder = 0;
  int16_t eggByRegionExtra[REGION_COUNT > CARE_LEGACY_REGIONS ? REGION_COUNT - CARE_LEGACY_REGIONS : 1] = {0};
  // 0 = legacy v3.58 layout (eggByRegion[7] was ALL), 1 = v3.61 layout.
  // Appended at the END so older raw snapshots keep every preceding offset.
  uint8_t regionSchema = 0;
  uint8_t trHp = 0;  // appended: older snapshots default to zero
};

class Pet {
public:
  // Estadisticas 0..100
  uint8_t fullness = 80;  // comida
  uint8_t joy = 80;       // felicidad
  uint8_t energy = 80;    // energia
  uint8_t hygiene = 100;  // limpieza
  uint8_t poops = 0;      // cacas en pantalla (max 3)
  uint8_t weight = 0;     // 0-100: las chuches engordan, el minijuego quema
  // IV (valores individuales 0-31, como en los juegos de 3a gen en adelante):
  // se tiran al eclosionar. Desde v3.61.6 pueden subir gradualmente con
  // capsulas/Gold Crown. Aportan IV x nivel / 100 al stat y ademas fijan el
  // tope de entrenamiento (trMaxFor): un individuo
  // mediocre no solo empieza peor, es que no puede llegar tan lejos.
  uint8_t ivAtk = 16, ivDef = 16, ivSpe = 16, ivHp = 16;
  uint8_t trAtk = 0, trDef = 0, trSpe = 0, trHp = 0;
  bool berryKnown = false;  // ya descubrio su baya favorita
  bool shiny = false;       // variante de color rara (se sortea en el huevo)
  uint32_t ageMinutes = 0;
  // Progress clock in awake-minute equivalents. Awake adds 1/min; sleeping
  // accumulates 1 equivalent minute per SLEEP_PROGRESS_QUANTUM real minutes.
  uint32_t levelMinutes = LEVEL_MINUTES_UNSET;
  uint8_t sleepLevelRemainder = 0;
  int16_t speciesId = -1;      // numero de Pokedex (1..DEX_COUNT), -1 = huevo
  int16_t prevSpeciesId = -1;  // para la animacion de evolucion
  uint8_t careMistakes = 0;   // descuidos: cada uno retrasa la evolucion 1 nivel
  bool sleeping = false;
  uint32_t lastSeenEpoch = 0;   // ultima hora RTC vista (para progresion offline)
  uint8_t ceremony = CER_NONE;  // despedida/escapada/liberacion en curso
  uint8_t lastEnd = CER_NONE;   // como acabo la anterior (afecta al huevo)
  // A finished ceremony hands the creature over here before newEgg() wipes the
  // live state. The UI drains it (endedKind back to CER_NONE) once the pet has
  // either taken a party slot or been let go. Only farewell and release fill
  // it; a runaway leaves endedKind at CER_NONE and the pet is simply gone.
  PartyMon endedMon;
  uint8_t endedKind = CER_NONE;
  // Pokedex bitmaps, one bit per species. Widening these is SAFE on an existing
  // save: getBytes() copies only what was stored, and the array is zeroed by its
  // initialiser, so a 19-byte blob from the Kanto-only build lands in the front
  // and bits 1-151 keep exactly their old meaning.
  uint8_t dexReg[(DEX_COUNT + 7) / 8] = { 0 };       // criados
  uint8_t dexShinyReg[(DEX_COUNT + 7) / 8] = { 0 };  // criados en version shiny
  uint8_t digiReg[(DIGI_SPECIES_CAP + 7) / 8] = {0};
  uint8_t digiBest[DIGI_SPECIES_CAP] = {0};
  bool isDigiRegistered(uint16_t id) const { return id < DIGI_SPECIES_COUNT && (digiReg[id >> 3] & (1 << (id & 7))); }
  uint16_t digiRegisteredCount() const { uint16_t n=0; for(uint16_t i=0;i<DIGI_SPECIES_COUNT;i++) if(isDigiRegistered(i)) n++; return n; }
  // racha de cuidado diario (del jugador: persiste entre crianzas)
  uint16_t streak = 0, bestStreak = 0;
  uint32_t lastCareDay = 0;
  // vinculo (del bicho: sube lento con cuidado, se resetea al nacer otro)
  uint8_t bond = 0;
  char nick[12] = "";    // apodo (vacio = nombre de especie)
  // medallas: del individuo + contador acumulado entre todas las crianzas
  uint16_t medals = 0, totalMedals = 0;
  uint16_t newMedal = 0;   // recien conseguida(s), para celebrar
  uint16_t lastMilestone = 0;  // hito de racha ya celebrado
  uint16_t gameHi = 0;     // record del minijuego (del jugador)
  uint16_t strHi = 0;      // record de golpes al saco

  void begin();                 // carga estado de NVS (o crea el primer huevo)
  void update(uint32_t nowMs);  // llamar en cada loop()

  // Acciones (botones tactiles)
  void feed();              // baya roja (compatibilidad)
  void feedBerry(uint8_t color);  // 0 roja, 1 azul, 2 verde
  void feedCandy();
  void itemEatReaction();       // earned edible item: animation only, no extra stats
  bool lovesBerry(uint8_t color) const {
    return !isEgg() && (speciesId % 3) == color;  // gusto oculto por especie
  }
  // The ball game: happiness AND defence training. Returns the DEF gained.
  uint8_t playResult(uint16_t score);
  uint8_t trainStrength(uint16_t hits);  // saco de entrenamiento (entrena FUE)
  // Reaction test: its own trainer, so the ball game can go back to being purely
  // about joy instead of doubling as a stat grind.
  uint8_t trainSpeed(uint16_t hits);
  uint8_t trainVitality(uint16_t score); // heart-ring endurance game
  // What beating a gym leader is worth. Random WHICH stat, but only among the
  // ones with room left -- a random grant that landed on an already-capped stat
  // would silently evaporate, which reads as a bug rather than as luck. Writes
  // the stat into `which` (0 ATK, 1 DEF, 2 SPE) and returns what was actually
  // gained; 0 means every stat is at its ceiling. Costs nothing: the fight
  // already spent the energy, and that is what rate-limits rematching.
  uint8_t rewardTraining(uint8_t amount, uint8_t &which);
  uint16_t spdHi = 0;    // best reaction-test score

  // stats de combate: base real de gen 1 + nivel + IV + entrenamiento
  uint16_t atkStat() const;
  uint16_t defStat() const;
  uint16_t speStat() const;
  uint16_t vitStat() const;  // HP base + IV + level + vitality training
  // Legacy SpA/SpD accessors remain for save/data compatibility. TamaPoke's
  // v3.61.6 casual battle rules use one Attack and one Defence regardless of
  // move category, so no separate special IVs or training are exposed.
  uint16_t spaStat() const;
  uint16_t spdStat() const;

  // The four known moves (indices into MOVE_TBL; 0 = empty slot). Player-chosen
  // once the learn/forget prompt exists -- until then, and for saves made before
  // moves were stored at all, relearnFromLevel() fills them in.
  uint8_t moves[MOVE_SLOTS] = { 0, 0, 0, 0 };
  uint8_t moveCount() const;
  bool knowsMove(uint8_t mv) const;
  // The newest MOVE_SLOTS moves this species has learned by its current level,
  // newest last. Used on hatch, on a fresh save, and to backfill empty slots.
  void relearnFromLevel();
  // The level at which `dex` may legally use learnset entry `i`. A level-up
  // move carries its own; a level-0 entry is a TM/tutor/egg move, which the
  // data gives no level at all, and those unlock together at TM_LEVEL.
  //
  // It is a free function, not a Pet method, because the move PICKER needs the
  // identical answer for a banked party member. Having its own gate is what let
  // a level 22 Charmeleon be offered FIRE BLAST.

  // Moves reachable at this level that are not already known, for the learn
  // prompt. Returns how many were written into out (at most max).
  uint8_t pendingLearnables(uint8_t *out, uint8_t max) const;

  // Player-wide, like the streak and the Pokedex: badges outlive the creature
  // that earned them, so newEgg() must never clear this.
  // A creature brought back out of the party or box. It is FROZEN: it does not
  // age, cannot evolve, and cannot be lost -- a companion rather than a
  // contender. The cost is that its level never rises again, so it stops
  // improving; ageMinutes is simply set to match its banked level, which keeps
  // level() working untouched rather than needing a second source of truth.
  bool frozen = false;
  void reviveFrom(const PartyMon &m);

  // The player's own name, alongside the badges and the streak: it belongs to
  // whoever is playing, not to the creature, so newEgg() must never clear it.
  char trainerName[12] = "";
  void renameTrainer(const char *n) {
    strncpy(trainerName, n, sizeof(trainerName) - 1);
    trainerName[sizeof(trainerName) - 1] = 0;
    save();
  }

  // Which generation eggs come from. Player-wide, like the badges: it outlives
  // every creature, so newEgg() must never reset it.
  uint8_t region = REGION_ALL;
  // 0..REGION_COUNT-1 = Pokemon; next 5 = DMC 1..5; next 6 = Pendulum 0..5.
  // This is additive and does not reinterpret the old Pokemon region key.
  uint8_t eggSource = REGION_ALL;
  // The species this egg would be in each region. Filled in as the player
  // looks, cleared by newEgg(). It exists so that switching region and back
  // shows the SAME creature rather than rolling a fresh one -- without it,
  // toggling would be a re-roll button.
  int16_t eggByRegion[REGION_COUNT] = { 0 };
  void setRegion(uint8_t r);
  void setDigimonVersion(uint8_t version);
  bool eggIsDigimon() const { return isDigimonId(eggTarget); }
  bool currentIsDigimon() const { return isDigimonId(speciesId); }
  const char *regionName() const { return REGIONS[region % REGION_COUNT].name; }
  // How much of one region's dex is filled in, for the Pokedex header.
  uint16_t registeredCountIn(uint16_t lo, uint16_t hi) const {
    uint16_t n = 0;
    for (uint16_t d = lo; d <= hi && d <= DEX_COUNT; d++) if (isRegistered(d)) n++;
    return n;
  }
  uint16_t registeredCountRegion(uint8_t r) const {
    uint16_t n = 0, total = regionDexCount(r);
    for (uint16_t i = 0; i < total; i++) { int16_t d = regionDexAt(r, i); if (d >= 1 && isRegistered(d)) n++; }
    return n;
  }

  uint8_t avatar = 0;       // which player sprite, 0..3
  // Kanto's ladder, under the keys it has always used. Johto and Hoenn live in
  // a SEPARATE array under new keys rather than widening these -- purely
  // additive, so an existing save cannot be misread, exactly the reasoning that
  // put the box under its own key instead of growing the party blob.
  uint16_t badges = 0;      // bit n = trainer n beaten on easy
  uint16_t badgesHard = 0;  // ... and on hard
  uint16_t badgesX[GYM_REGIONS - 1] = { 0 };
  uint16_t badgesHardX[GYM_REGIONS - 1] = { 0 };

  uint16_t badgeMask(uint8_t rg, bool hard) const {
    if (rg == 0) return hard ? badgesHard : badges;
    if (rg >= GYM_REGIONS) return 0;
    return hard ? badgesHardX[rg - 1] : badgesX[rg - 1];
  }
  bool hasBadge(uint8_t rg, uint8_t i, bool hard) const {
    return (badgeMask(rg, hard) >> i) & 1;
  }
  void winBadge(uint8_t rg, uint8_t i, bool hard) {
    if (rg >= GYM_REGIONS) return;
    uint16_t bit = (uint16_t)1 << i;
    if (rg == 0) { if (hard) badgesHard |= bit; else badges |= bit; }
    else if (hard) badgesHardX[rg - 1] |= bit;
    else badgesX[rg - 1] |= bit;
    save();
  }
  uint8_t badgeCountIn(uint8_t rg, bool hard) const {
    uint16_t v = badgeMask(rg, hard);
    uint8_t n = 0;
    while (v) { n += v & 1; v >>= 1; }
    return n;
  }
  // Every region's badges together, for the player card's running total.
  uint8_t badgeCount(bool hard) const {
    uint8_t n = 0;
    for (uint8_t r = 0; r < GYM_REGIONS; r++) n += badgeCountIn(r, hard);
    return n;
  }

  // Level-up learning. lastLearnLevel is the highest level whose gates have
  // been handled, so a move declined once is not offered forever, and the
  // offline catch-up -- which can cross a dozen levels in one go -- queues its
  // offers instead of firing a dozen dialogs at boot.
  uint8_t lastLearnLevel = 0;
  uint8_t learnQueue[12] = { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 };
  uint8_t learnQCount = 0;
  void checkLearnGates();
  bool hasLearnOffer() const { return learnQCount > 0; }
  uint8_t learnOffer() const { return learnQCount ? learnQueue[0] : 0; }
  void acceptLearn(uint8_t slot);   // put the pending move into slot 0..3
  void declineLearn();
  // tope de entrenamiento que permite un IV: 77 (IV 8) .. 100 (IV 31)
  static uint8_t trMaxFor(uint8_t iv) { return 70 + (30 * (uint16_t)iv) / 31; }
  uint8_t trMaxAtk() const { return trMaxFor(ivAtk); }
  uint8_t trMaxDef() const { return trMaxFor(ivDef); }
  uint8_t trMaxSpe() const { return trMaxFor(ivSpe); }
  uint8_t trMaxHp() const { return trMaxFor(ivHp); }
  void play();
  void toggleLight();
  bool isNightHour() const;
  void setScreenOff(bool off);  // the sketch reports the PWR button
  bool screenIsOff = false;
  void dbgTick() { tick(); }   // tests drive minutes directly; tick() is private
  // syncClock() reads "seen" back out of NVS, so a test has to put it there
  void dbgSetSeen(uint32_t e) { lastSeenEpoch = e; prefs.putUInt("seen", e); }
  uint8_t sleepAuto = SLEEP_NONE;   // who decided the current sleep state  // dormir / despertar
  void clean();
  void caress();  // tocar al bicho
  void eggTap();  // tocar el huevo: 3 toques y eclosiona
  void newEgg();   // empezar de cero con un inicial aleatorio
  void release();  // soltar (pulsacion larga + confirmar)
  void syncClock(uint32_t nowEpoch);  // aplica el tiempo transcurrido apagado
  void setClock(uint32_t nowEpoch);   // fija la hora sin aplicar progresion
  void startFarewell();  // tambien usable desde la consola serie (BYE)
  // Retire on demand. Before the farewell is earned this is the SAME ceremony
  // -- the creature is banked exactly as it would be -- but it hands the next
  // creature EVO_PENALTY_LEVELS on every evolution threshold. Retiring one that
  // has already earned its farewell costs nothing: it is then just the button.
  void startRetire();
  bool canRetireNow() const;
  bool retireIsFree() const { return canFarewellNow(); }
  // Was the retire NOW IN PROGRESS an early one? THE single answer, asked by
  // snapshotForParty() and by the tests. Distinct from retireIsFree(), which asks
  // whether a retire started right now WOULD be free: this one remembers what the
  // retire that is already running actually was, and outlives the moment it began.
  bool retireIsEarly() const { return retirePending; }
  uint8_t evoPenalty() const { return evoPen; }
  void startRunaway();   // tambien usable desde la consola serie (RUN)

  bool isEgg() const { return speciesId < 0; }
  uint8_t eggCracks() const { return eggTaps; }
  bool eating() const { return millis() < eatUntil; }
  bool showHeart() const { return millis() < heartUntil; }
  bool evolving() const { return millis() < evolveUntil; }
  float evolveT() const {     // progreso de la animacion de evolucion 0..1
    uint32_t n = millis();
    uint32_t left = evolveUntil > n ? evolveUntil - n : 0;
    return 1.0f - (float)left / (float)EVOLVE_ANIM_MS;
  }
  bool canEvolveNow() const;  // condiciones de evolucion cumplidas (lista)
  void evolve();              // dispara la transformacion (la llama un toque del usuario)
  bool canFarewellNow() const;  // forma final + 6 horas: lista para despedirse (boton)
  // Total neglect RIGHT NOW. THE single answer: tick() counts against it and
  // canRunawayNow() re-checks it, because neglectTicks is frozen (neither
  // counted nor cleared) while asleep and so can outlive the state that
  // earned it -- see sleep_test.
  bool inTotalNeglect() const { return !fullness && !joy && !energy && !hygiene; }
  bool canRunawayNow() const;   // abandono total 1h: lista para escaparse (boton triste)
  // Evolution can still be offered on the main screen. Voluntary farewell is
  // deliberately MENU-ONLY in the Korean build: never auto-offer/re-offer it.
  bool wantEvolveButton() const { return canEvolveNow() && level() > evoDeclinedLv; }
  bool wantFarewellButton() const { return false; }
  void declineEvolve() { evoDeclinedLv = level(); }              // re-ofrece al subir de nivel
  void declineFarewell() {}                                      // legacy no-op; no periodic farewell prompt
  // primera partida: el jugador elige inicial (Bulbasaur/Charmander/Squirtle)
  bool awaitingStarter() const { return starterPick; }
  void chooseStarter(int16_t dex) { eggTarget = dex; starterPick = false; save(); }
  void factoryReset();  // borra NVS principal + respaldo critico (test: WIPE)
  void dbgRunawayReady() { fullness = joy = energy = hygiene = 0; neglectTicks = RUNAWAY_TICKS; }  // test
  // test: force what the egg holds and hatch it now (serial command EGG).
  // The legendary/shiny IV guarantees only fire inside hatch(), so without
  // this there is no way to exercise them from outside the class.
  void dbgHatchAs(int16_t dex, bool wantShiny) {
    if (!isCreatureId(dex)) return;
    eggTarget = dex;
    eggShiny = wantShiny;
    starterPick = false;
    speciesId = -1;
    eggTaps = 0;
    hatch();
  }
  // Convert an old 10-min/level age clock to the new 2-min awake clock while
  // preserving the visible level on the first v3.57.4 boot.
  static uint32_t migrateLegacyLevelMinutes(uint32_t oldAge) {
    uint32_t completed = oldAge / LEGACY_MINUTES_PER_LEVEL;
    if (completed >= (uint32_t)(MAX_LEVEL - 1))
      return (uint32_t)(MAX_LEVEL - 1) * MINUTES_PER_LEVEL;
    uint32_t oldInto = oldAge % LEGACY_MINUTES_PER_LEVEL;
    uint32_t newInto = (oldInto * MINUTES_PER_LEVEL) / LEGACY_MINUTES_PER_LEVEL;
    if (newInto >= MINUTES_PER_LEVEL) newInto = MINUTES_PER_LEVEL - 1;
    return completed * MINUTES_PER_LEVEL + newInto;
  }
  uint32_t levelClockMinutes() const {
    // Temporary battle Pet objects may intentionally leave levelMinutes unset
    // and express their requested level through ageMinutes. Persistent pets are
    // migrated during load()/snapshot restore before normal ticking.
    return levelMinutes == LEVEL_MINUTES_UNSET ? ageMinutes : levelMinutes;
  }
  // Capped at 100. Awake grows fast; sleep grows at the slower configured rate.
  uint8_t level() const {
    uint32_t l = 1 + levelClockMinutes() / MINUTES_PER_LEVEL;
    return l > MAX_LEVEL ? MAX_LEVEL : (uint8_t)l;
  }
  bool isRegistered(int16_t dex) const {
    return dex >= 1 && dex <= DEX_COUNT && (dexReg[(dex - 1) >> 3] & (1 << ((dex - 1) & 7)));
  }
  bool isShinyRegistered(int16_t dex) const {
    return dex >= 1 && dex <= DEX_COUNT && (dexShinyReg[(dex - 1) >> 3] & (1 << ((dex - 1) & 7)));
  }
  uint16_t registeredCount() const;
  bool lineHasUnregistered(int16_t base) const;
  uint8_t eggRarity() const;       // rareza del huevo actual (sin revelar especie)
  int16_t pickEggSpecies(bool countHunt = true); // new eggs count hunt; EGGS diagnostics do not
  // What the waiting egg would hatch into. Hidden from the PLAYER, not from the
  // code: the serial console already simulates rolls, and the region tests have
  // to see which creature a switch landed on.
  int16_t eggPeek() const { return eggTarget; }
  int16_t rollInRegion(uint8_t r, uint8_t tier);
  // Which of EEVEE's branches this player can actually be handed right now.
  // THE single answer: evolve() picks from it and lineHasUnregistered() asks it
  // whether the line is finished, and those two having their own opinions is how
  // the branch stayed at 134-136 while five more Eeveelutions sat in the dex.
  //
  // Filtered by the sprite pack AND by whether the species has art at all --
  // evolving into something that can only draw as a dex number is the same fault
  // just removed from the gym rosters, and here it would be permanent. `out`
  // must hold EEVEE_EVO_COUNT; returns how many were written.
  uint8_t eeveeOptions(int16_t *out) const;
  uint8_t evolutionOptions(int16_t base, int16_t *out, uint8_t cap) const;

  // v3.57.3 casual Pokedex search. Player-wide and deliberately NOT part of
  // PartyMon/CareSnapshot: changing a search target must never resize or alter
  // creature save records. The target is the desired dex entry; huntBaseFor()
  // resolves it to the hatchable ancestor (Dragonite -> Dratini, etc.).
  int16_t huntTargetDex() const { return huntTarget; }
  uint8_t huntMisses() const { return huntPity; }
  int16_t huntBaseFor(int16_t target) const;
  bool huntCanTarget(int16_t target) const;
  void toggleHuntTarget(int16_t target);

  uint8_t lowestStat() const { return min(min(fullness, joy), min(energy, hygiene)); }
  PetMood mood() const;
  // progreso de la ceremonia de despedida/escapada, 0..1 (para animarla)
  float ceremonyT() const {
    if (ceremony == CER_NONE) return 0.0f;
    uint32_t n = millis();
    uint32_t left = ceremonyUntil > n ? ceremonyUntil - n : 0;
    return 1.0f - (float)left / (float)CEREMONY_MS;
  }

  // racha / vinculo / medallas / nombre
  void rename(const char *name);
  bool hasMedal(uint16_t m) const { return medals & m; }
  bool showMedal() const { return millis() < medalUntil; }
  bool showMilestone() const { return millis() < milestoneUntil; }
  int careBonus() const;  // mejora del huevo por racha + vinculo

  // guardado periodico diferido: tick() marca pendiente y el loop lo vuelca
  // cuando la pantalla esta atenuada/apagada (la escritura a flash congela
  // ~1s ambos cores: asi no se ve ni corta el tactil)
  bool savePending() const { return pendingSave; }
  void flushSave();
  // Writes NOW, whatever pendingSave says. flushSave() is `if (pendingSave)
  // save()`, so a console command that changed something the game had not
  // already marked dirty wrote nothing at all and quietly depended on the next
  // autosave to notice. Defined out of line: save() is private and declared
  // further down.
  void saveNow();
  // Converts the current living companion to Shiny through a public, save-safe API.
  // registerSpecies() remains private so callers cannot mutate Pokedex state directly.
  bool makeCurrentShiny();
  // Lightweight runtime integrity check. Reads only the compact guard blob and
  // self-heals badges/training if a rollback is detected while the device is on.
  void verifyCriticalProgress();

  // Snapshot/restore only the creature-specific raising state. Used by the
  // three live care tabs; inactive slots catch up when selected again.
  void captureCareSnapshot(CareSnapshot &out, uint32_t nowEpoch) const;
  bool restoreCareSnapshot(const CareSnapshot &in, uint32_t nowEpoch);

private:
  Preferences prefs;
  bool persistenceReady = false;
  uint32_t lastTick = 0;
  uint32_t eatUntil = 0;
  uint32_t heartUntil = 0;
  uint32_t evolveUntil = 0;
  int16_t eggTarget = 1;       // dex oculto que saldra del huevo
  bool eggShiny = false;       // sorpresa sorteada al crear el huevo
  uint8_t eggTaps = 0;
  uint8_t mistakeCooldown = 0;
  uint8_t ticksSinceSave = 0;
  bool pendingSave = false;     // guardado periodico pendiente de volcar
  uint8_t evoDeclinedLv = 0;    // "mantener forma": no ofrecer evolucion hasta subir de nivel
  uint32_t farDeclinedAge = 0;  // "quedaros juntos": no ofrecer despedida hasta esta edad
  bool starterPick = false;     // primera partida: esperando que el jugador elija inicial
  uint8_t evoPen = 0;           // levels added to this creature's evolution gate
  bool retirePending = false;   // an early retire is under way; newEgg() spends it
  uint8_t neglectTicks = 0;
  uint16_t goodTicks = 0;  // racha bien cuidado: forja la DEF
  uint32_t ceremonyUntil = 0;
  uint8_t bondToday = 0;       // tope diario de subida de vinculo
  uint32_t medalUntil = 0;     // celebracion de medalla en pantalla
  uint32_t milestoneUntil = 0; // celebracion de hito de racha

  // Search state is a compact independent NVS record. `huntPity` means eligible
  // misses (0..5); at 5 the NEXT eligible egg is guaranteed, i.e. a 6-roll cap.
  int16_t huntTarget = 0;
  uint8_t huntPity = 0;
  bool huntRollPending = false;  // transient: committed only AFTER the egg save
  bool huntRollHit = false;
  void loadHuntState();
  void saveHuntState();
  void noteHuntRoll(bool hit);

  uint32_t today() const { return lastSeenEpoch ? lastSeenEpoch / 86400 : 0; }
  void registerCare();   // primer cuidado del dia: racha + vinculo
  void addBond(uint8_t amt);
  uint8_t rollIV(int bonus) const;  // una tirada 8-31 empujada por el cuidado
  void rollIVs();                   // los 4, con las garantias de legendario/shiny
  uint8_t ivFromGene(uint8_t gene) const;  // migracion de guardados con genes
  void defTick(bool resting);       // la calma forja la defensa (ver pet.cpp)
  void snapshotForParty();          // copy into endedMon before newEgg() wipes it
  void checkMedals();
  void tick();
  void applyAutoSleep();
  void hatch();
  void registerSpecies(int16_t dex);
  void save();
  void load();
  static uint8_t clamp100(int v) { return v < 0 ? 0 : (v > 100 ? 100 : v); }
};
