#include "pet.h"
#include "care_slots.h"
#include "avatars.h"
#include "dex.h"
#include "moves.h"
#include "noart.h"   // speciesHasArt(): the egg pool skips what cannot be drawn
#include "audio.h"
#include "game_extras.h"
#include "personality.h"
#include "digimon.h"
#include <stddef.h>

// Reads a blob that may be LONGER than the array we are reading it into.
//
// Preferences::getBytes reads the stored length FIRST and, if it exceeds the
// caller's buffer, logs and returns 0 WITHOUT COPYING ANYTHING:
//
//     if (len > maxLen) { log_e("not enough space in buffer"); return 0; }
//
// Growing the dex was always safe -- a short blob lands in the front of a bigger
// array and the rest keeps its zero initialiser, which is what every migration
// here relies on. SHRINKING was not: flashing a build with a smaller DEX_COUNT,
// GYM_REGIONS or REGION_COUNT over a newer save left dexReg, dexShinyReg, the
// badge arrays and eggByRegion entirely ZERO. The creature survived, being all
// scalars, while the Pokedex and every badge past Kanto quietly vanished --
// which from the player's side is their game rolling back.
//
// The PREFIX is the right thing to keep: dex bit n means the same species
// whatever the table grew to afterwards, and region n is the same region.
// Only the tail we have no room for is dropped, which is data about content
// this build does not have anyway.
//
// The emulator's Preferences stub used to truncate rather than refuse, so no
// test could see any of this -- see tools/emu/Preferences.h.
static void loadBlob(Preferences &p, const char *key, void *dst, size_t n) {
  size_t have = p.getBytesLength(key);
  if (!have) return;                       // absent: keep the initialiser
  if (have <= n) { p.getBytes(key, dst, n); return; }
  uint8_t *tmp = (uint8_t *)malloc(have);
  if (!tmp) return;                        // rather no read than a half one
  if (p.getBytes(key, tmp, have) == have) memcpy(dst, tmp, n);
  free(tmp);
}


// v3.46 critical-progress guard -------------------------------------------
//
// The ordinary save is deliberately split into many human-readable NVS keys,
// which made migrations easy but also left two weak points:
//   1) boot trusted the single, ancient "init" key as the whole-save sentinel;
//   2) badges/training had no second copy if a key write/read was interrupted.
//
// Keep a compact CRC-checked shadow in a DIFFERENT namespace. Badges are
// monotonic for the player, so recovery safely ORs them. Training is monotonic
// only within one living creature, so each live-care slot stores an IV+shiny
// signature and is restored only when that exact slot/signature still matches.
#define PROGRESS_GUARD_MAGIC 0x47503436UL  // "GP46"
#define PROGRESS_GUARD_VERSION 2
#define PROGRESS_GUARD_SLOTS 3

struct GuardTrain46 {
  uint8_t valid;
  uint8_t ivAtk, ivDef, ivSpe, ivHp;
  uint8_t shiny;
  uint8_t trAtk, trDef, trSpe, trHp;
};

struct ProgressGuard46 {
  uint32_t magic;
  uint8_t version;
  uint8_t reserved[3];
  uint32_t sequence;
  uint16_t badges;
  uint16_t badgesHard;
  uint16_t badgesX[GYM_REGIONS - 1];
  uint16_t badgesHardX[GYM_REGIONS - 1];
  GuardTrain46 train[PROGRESS_GUARD_SLOTS];
  uint32_t crc;
};

static uint32_t guardCrc46(const ProgressGuard46 &g) {
  const uint8_t *p = (const uint8_t *)&g;
  const size_t n = offsetof(ProgressGuard46, crc);
  uint32_t h = 2166136261UL;
  for (size_t i = 0; i < n; ++i) { h ^= p[i]; h *= 16777619UL; }
  return h;
}

static bool readProgressGuard46(ProgressGuard46 &g) {
  memset(&g, 0, sizeof(g));
  Preferences p;
  if (!p.begin("tamapoke_guard", true)) return false;
  size_t n = p.getBytesLength("progress");
  bool ok = n == sizeof(g) && p.getBytes("progress", &g, sizeof(g)) == sizeof(g);
  p.end();
  if (!ok || g.magic != PROGRESS_GUARD_MAGIC || g.version != PROGRESS_GUARD_VERSION) return false;
  return g.crc == guardCrc46(g);
}

static bool writeProgressGuard46(const Pet &pet, uint8_t activeSlot) {
  if (activeSlot >= PROGRESS_GUARD_SLOTS) activeSlot = 0;
  ProgressGuard46 g;
  bool had = readProgressGuard46(g);
  if (!had) memset(&g, 0, sizeof(g));
  g.magic = PROGRESS_GUARD_MAGIC;
  g.version = PROGRESS_GUARD_VERSION;
  g.sequence = had ? g.sequence + 1 : 1;

  // Never let a transient zero overwrite earned badges in the shadow copy.
  g.badges |= pet.badges;
  g.badgesHard |= pet.badgesHard;
  for (uint8_t r = 0; r + 1 < GYM_REGIONS; ++r) {
    g.badgesX[r] |= pet.badgesX[r];
    g.badgesHardX[r] |= pet.badgesHardX[r];
  }

  if (!pet.isEgg()) {
    GuardTrain46 &t = g.train[activeSlot];
    const bool same = t.valid && t.ivAtk == pet.ivAtk && t.ivDef == pet.ivDef &&
                      t.ivSpe == pet.ivSpe && t.ivHp == pet.ivHp && t.shiny == (uint8_t)pet.shiny;
    if (!same) memset(&t, 0, sizeof(t));
    t.valid = 1;
    t.ivAtk = pet.ivAtk; t.ivDef = pet.ivDef; t.ivSpe = pet.ivSpe; t.ivHp = pet.ivHp;
    t.shiny = pet.shiny ? 1 : 0;
    // Training cannot legitimately decrease for the same living creature.
    if (pet.trAtk > t.trAtk) t.trAtk = pet.trAtk;
    if (pet.trDef > t.trDef) t.trDef = pet.trDef;
    if (pet.trSpe > t.trSpe) t.trSpe = pet.trSpe;
    if (pet.trHp > t.trHp) t.trHp = pet.trHp;
  }

  g.crc = guardCrc46(g);
  Preferences p;
  if (!p.begin("tamapoke_guard", false)) return false;
  bool ok = p.putBytes("progress", &g, sizeof(g)) == sizeof(g);
  p.end();
  return ok;
}

static bool mergeProgressGuard46(Pet &pet, uint8_t activeSlot) {
  ProgressGuard46 g;
  if (!readProgressGuard46(g)) return false;
  bool changed = false;

  uint16_t v = pet.badges | g.badges;
  if (v != pet.badges) { pet.badges = v; changed = true; }
  v = pet.badgesHard | g.badgesHard;
  if (v != pet.badgesHard) { pet.badgesHard = v; changed = true; }
  for (uint8_t r = 0; r + 1 < GYM_REGIONS; ++r) {
    v = pet.badgesX[r] | g.badgesX[r];
    if (v != pet.badgesX[r]) { pet.badgesX[r] = v; changed = true; }
    v = pet.badgesHardX[r] | g.badgesHardX[r];
    if (v != pet.badgesHardX[r]) { pet.badgesHardX[r] = v; changed = true; }
  }

  if (!pet.isEgg()) {
    if (activeSlot >= PROGRESS_GUARD_SLOTS) activeSlot = 0;
    const GuardTrain46 &t = g.train[activeSlot];
    const bool same = t.valid && t.ivAtk == pet.ivAtk && t.ivDef == pet.ivDef &&
                      t.ivSpe == pet.ivSpe && t.ivHp == pet.ivHp && t.shiny == (uint8_t)pet.shiny;
    if (same) {
      uint8_t a = t.trAtk > pet.trAtk ? t.trAtk : pet.trAtk;
      uint8_t d = t.trDef > pet.trDef ? t.trDef : pet.trDef;
      uint8_t e = t.trSpe > pet.trSpe ? t.trSpe : pet.trSpe;
      uint8_t h = t.trHp > pet.trHp ? t.trHp : pet.trHp;
      if (a > pet.trMaxAtk()) a = pet.trMaxAtk();
      if (d > pet.trMaxDef()) d = pet.trMaxDef();
      if (e > pet.trMaxSpe()) e = pet.trMaxSpe();
      if (h > pet.trMaxHp()) h = pet.trMaxHp();
      if (a != pet.trAtk) { pet.trAtk = a; changed = true; }
      if (d != pet.trDef) { pet.trDef = d; changed = true; }
      if (e != pet.trSpe) { pet.trSpe = e; changed = true; }
      if (h != pet.trHp) { pet.trHp = h; changed = true; }
    }
  }
  return changed;
}

void Pet::begin() {
  persistenceReady = prefs.begin("tamapoke", false);
  if (!persistenceReady) {
    Serial.println("NVS tamapoke open FAILED");
    lastTick = millis();
    return;
  }
  // Zeroed BEFORE the branch below, not inside load(): getBytes() leaves its
  // destination untouched when the key is missing.
  memset(badgesX, 0, sizeof(badgesX));
  memset(badgesHardX, 0, sizeof(badgesHardX));
  memset(dexReg, 0, sizeof(dexReg));
  memset(dexShinyReg, 0, sizeof(dexShinyReg));
  for (int i = 0; i < REGION_COUNT; i++) eggByRegion[i] = 0;

  // v3.45 and older trusted only `init`. If that one sentinel was missing or
  // unreadable, perfectly good progress keys were skipped and newEgg() wrote
  // defaults back over them. Detect a save by its durable payload as well.
  const bool hasPayload = prefs.isKey("dexn") || prefs.isKey("age") ||
                          prefs.isKey("tatk") || prefs.isKey("tdef") || prefs.isKey("tspe") ||
                          prefs.isKey("badg") || prefs.isKey("badgX") || prefs.isKey("tnam") ||
                          prefs.isKey("dexreg");
  const bool hadInit = prefs.getBool("init", false);
  if (hadInit || hasPayload) {
    if (!hadInit) Serial.println("SAVE RECOVERY: init missing, loading payload");
    load();
    prefs.putBool("init", true);  // refresh the sentinel instead of leaving it ancient forever
  } else {
    prefs.putBool("init", true);
    newEgg();
  }
  // Independent player-wide search state. Never part of creature snapshots.
  loadHuntState();

  uint8_t activeSlot = prefs.getUChar("carea", 0);
  // If a care-slot transaction was interrupted, scalar pet keys and carea may
  // temporarily refer to different creatures. CareSlots::begin() will recover
  // from the journalled snapshots; do not merge training into the wrong slot
  // before that happens.
  const bool careSwitchPending = prefs.getBool("caretx", false);
  if (!careSwitchPending) {
    if (mergeProgressGuard46(*this, activeSlot)) {
      Serial.println("SAVE RECOVERY: critical progress restored from guard");
      save();
    } else {
      // Seed/refresh the guard even on the first v3.46 boot.
      writeProgressGuard46(*this, activeSlot);
    }
  } else {
    Serial.println("SAVE RECOVERY: pending care-slot transaction detected");
  }
  lastTick = millis();
}

void Pet::newEgg() {
  ceremony = CER_NONE;
  neglectTicks = 0;
  weight = 0;
  speciesId = -1;
  prevSpeciesId = -1;
  for (int i = 0; i < REGION_COUNT; i++) eggByRegion[i] = 0;
  // Search bookkeeping is committed only after the whole egg itself is saved.
  // If power dies between the two writes, the player may keep an extra pity
  // point, but can never LOSE pity for an egg that failed to persist.
  huntRollPending = false;
  huntRollHit = false;
  if (eggSource >= REGION_COUNT && eggSource < REGION_COUNT + 11) {
    uint8_t slot=eggSource-REGION_COUNT;
    uint8_t version=slot<5?(slot+1):(slot==5?10:slot+5);
    uint16_t baby=0; for(uint16_t i=0;i<DIGI_SPECIES_COUNT;i++)if(DIGI_SPECIES[i].version==version&&DIGI_SPECIES[i].stage==DIGI_BABY1){baby=i;break;}
    eggTarget = makeDigimonId(baby);
  } else {
    eggSource = region;
    eggTarget = pickEggSpecies(true);  // especie oculta segun rareza y pokedex
    eggByRegion[region % REGION_COUNT] = eggTarget;
  }
  starterPick = (registeredCount() == 0 && digiRegisteredCount() == 0);
  // Shiny: 1/32 base, 1/16 after a good farewell. An earned Shiny Charm
  // arms one EXTRA 2x boost for the next egg, then is consumed. Care streak and
  // bond still improve the denominator, with a floor so it never becomes free.
  int shinyBase = (lastEnd == CER_FAREWELL ? 16 : 32) - careBonus();
  if (extras.consumeShinyBoostForEgg()) shinyBase = (shinyBase + 1) / 2;
  if (shinyBase < 3) shinyBase = 3;
  eggShiny = eggIsDigimon() ? false : (random(shinyBase) == 0);
  // The debt lands on the creature about to hatch, and is spent doing so --
  // it is a one-day penalty, not a running total that compounds each retire.
  evoPen = retirePending ? EVO_PENALTY_LEVELS : 0;
  retirePending = false;
  eggTaps = 0;
  fullness = 80;
  joy = 80;
  energy = 80;
  hygiene = 100;
  poops = 0;
  ageMinutes = 0;
  levelMinutes = 0;
  sleepLevelRemainder = 0;
  careMistakes = 0;
  mistakeCooldown = 0;
  evoDeclinedLv = 0;
  sleeping = false;
  frozen = false;
  save();
  if (huntRollPending) {
    noteHuntRoll(huntRollHit);
    huntRollPending = false;
  }
}

// progresion offline: el tiempo paso aunque estuviera apagado, pero con
// piedad — las barras bajan con suelo en 15 (vuelve hambriento, no muerto),
// sin descuidos ni escapadas en ausencia
static uint8_t dropTo(uint8_t v, uint8_t d, uint8_t fl) {
  if (v <= fl) return v;
  return (v - fl > d) ? v - d : fl;
}

void Pet::factoryReset() {
  if (persistenceReady) prefs.clear();
  Preferences g;
  if (g.begin("tamapoke_guard", false)) { g.clear(); g.end(); }
}

void Pet::setClock(uint32_t nowEpoch) {
  lastSeenEpoch = nowEpoch;
  if (nowEpoch) save();  // persiste ya: un corte de luz no pierde la referencia
}

void Pet::syncClock(uint32_t nowEpoch) {
  uint32_t seen = prefs.getUInt("seen", 0);
  lastSeenEpoch = nowEpoch;
  if (nowEpoch == 0) return;
  uint32_t mins = (seen && nowEpoch > seen) ? (nowEpoch - seen) / 60 : 0;
  if (mins < 2 || ceremony != CER_NONE || starterPick) {
    save();  // primera vez, sin tiempo que aplicar o aun eligiendo inicial
    return;
  }
  if (mins > 14UL * 24 * 60) mins = 14UL * 24 * 60;  // tope: 2 semanas
  if (levelMinutes == LEVEL_MINUTES_UNSET)
    levelMinutes = migrateLegacyLevelMinutes(ageMinutes);

  for (uint32_t i = 0; i < mins; i++) {
    ageMinutes++;
    if (isEgg()) {
      if (ageMinutes >= 3) hatch();  // eclosiona en tu ausencia
      continue;
    }
    if (sleeping) {  // v3.22: sleeping needs decay very slowly
      energy = clamp100(energy + ENERGY_SLEEP_RECOVERY);
      if (ageMinutes % 6 == 0) {
        fullness = dropTo(fullness, 1, 30);
        joy = dropTo(joy, 1, 35);
      }
      if (ageMinutes % 8 == 0) hygiene = dropTo(hygiene, 1, 45);
      // v3.57.5: sleep grows at 1 level / 10 real minutes. Keep the partial
      // 0..4 minute bucket so shutdowns do not lose sleeping progress.
      if (!frozen && ++sleepLevelRemainder >= SLEEP_PROGRESS_QUANTUM) {
        sleepLevelRemainder = 0;
        levelMinutes++;
      }
      continue;
    }
    if (!frozen) levelMinutes++;
    // v3.22 gentler care tempo: hunger -1/2min, joy/cleanliness -1/3min.
    if (ageMinutes % 2 == 0) fullness = dropTo(fullness, 1, 15);
    if (ageMinutes % ENERGY_AWAKE_DECAY_MINUTES == 0) {
      energy = dropTo(energy, 1, 20);
    }
    if (ageMinutes % 3 == 0) {
      hygiene = dropTo(hygiene, 1, 15);
      joy = dropTo(joy, 1, 15);
    }
  }
  if (!isEgg()) {
    if (!sleeping) {  // durmiendo no ensucia
      uint8_t p = poops + mins / 240;
      poops = p > 3 ? 3 : p;
    }
    // la evolucion NO se aplica offline: queda lista y la dispara el usuario
    // tocando al bicho cuando vuelve (para que vea la transformacion)
  }
  Serial.printf("offline: %u min aplicados (nv.%u)\n", mins, level());
  save();
}

void Pet::update(uint32_t nowMs) {
  // fin de ceremonia: la criatura se va y queda un huevo nuevo
  if (ceremony != CER_NONE && millis() > ceremonyUntil) {
    snapshotForParty();  // hand it over BEFORE newEgg() erases everything
    newEgg();
    return;
  }
  while (nowMs - lastTick >= PET_TICK_MS) {
    lastTick += PET_TICK_MS;
    tick();
  }
}

void Pet::tick() {
  if (ceremony != CER_NONE) return;  // el tiempo se detiene en la despedida
  if (starterPick) return;  // la partida no empieza hasta elegir inicial: si el
                            // tiempo corriera aqui, el huevo eclosionaria solo a
                            // los 3 min con la especie sorteada y se perderia la
                            // eleccion del jugador
  if (levelMinutes == LEVEL_MINUTES_UNSET)
    levelMinutes = migrateLegacyLevelMinutes(ageMinutes);
  if (!frozen) ageMinutes++;   // a revived companion does not age

  if (isEgg()) {
    if (ageMinutes >= 3) hatch();  // si no lo tocas, eclosiona solo a los 3 min
    return;
  }

  applyAutoSleep();   // put down at 21:00 still goes to bed at 22:00


  // v3.22: care stats last longer. Sleeping is especially forgiving so a pet
  // does not wake up needing immediate maintenance after a normal night.
  if (sleeping) {
    energy = clamp100(energy + ENERGY_SLEEP_RECOVERY);
    if (weight > 0 && ageMinutes % 3 == 0) weight--;
    if (ageMinutes % 6 == 0) {
      fullness = dropTo(fullness, 1, 30);
      joy = dropTo(joy, 1, 35);
    }
    if (ageMinutes % 8 == 0) hygiene = dropTo(hygiene, 1, 45);
    // Sleeping growth is intentionally silent: one level every 10 real minutes
    // without waking the user with a level-up sound.
    if (!frozen && ++sleepLevelRemainder >= SLEEP_PROGRESS_QUANTUM) {
      sleepLevelRemainder = 0;
      levelMinutes++;
    }
    defTick(true);  // descansar tambien es bienestar: cuenta para la DEF
    checkMedals();
    if (++ticksSinceSave >= 5) pendingSave = true;
    return;
  }

  // v3.57.5: awake, non-frozen minutes count at the fast 2-min/level rate.
  if (!frozen) {
    uint8_t beforeLv = level();
    levelMinutes++;
    if (level() > beforeLv) sfxPlay(SFX_LEVEL);
  }

  // v3.22: slower maintenance drain. Hunger lasts roughly four times longer
  // than the old -2/min rule, while joy and hygiene tick only every 3 minutes.
  if (ageMinutes % 2 == 0) fullness = clamp100(fullness - 1);
  if (ageMinutes % ENERGY_AWAKE_DECAY_MINUTES == 0) energy = clamp100(energy - 1);
  if (fullness > 40 && poops < 3 && random(100) < 15) poops++;

  if (ageMinutes % 3 == 0) {
    // Poops still matter, but the old -4 per poop every minute emptied the
    // cleanliness bar extremely quickly. Apply only -1 per poop on this pulse.
    hygiene = clamp100(hygiene - 1 - poops);
  }
  if (weight > 50 && ageMinutes % ENERGY_AWAKE_DECAY_MINUTES == 0) energy = clamp100(energy - 1);
  if (weight > 0 && ageMinutes % 3 == 0) weight--;

  defTick(false);  // la calma forja la defensa

  if (ageMinutes % 3 == 0) {
    int dJoy = -1;
    if (fullness < 30) dJoy -= 1;
    if (hygiene < 30) dJoy -= 1;
    joy = clamp100(joy + dJoy);
  }

  // Descuido: dejar una estadistica por los suelos cuenta como error de
  // cuidado (con enfriamiento para no contar el mismo descuido cada minuto)
  if (mistakeCooldown > 0) mistakeCooldown--;
  if (lowestStat() <= 10 && mistakeCooldown == 0) {
    // Saturate instead of wrapping uint8_t 255 -> 0 after very long neglect.
    // The effective evolution gate itself is capped at MAX_LEVEL.
    if (careMistakes < MAX_LEVEL) careMistakes++;
    mistakeCooldown = 60;
    if (bond > 1) bond--;  // el descuido enfria el vinculo, pero sin arrasarlo:
                           // a -3 cada 30 min se perdia mucho mas de lo que se
                           // podia ganar en todo un dia y el vinculo se atascaba
  }

  checkMedals();  // la evolucion la dispara el usuario (canEvolveNow + tap), no el tick
  checkLearnGates();

  // abandono total: con TODO a cero durante una hora queda lista para escaparse;
  // NO se va sola, la dispara el usuario con el boton (final triste, lo presencia)
  if (inTotalNeglect()) {
    if (neglectTicks < RUNAWAY_TICKS) neglectTicks++;
  } else {
    neglectTicks = 0;  // un solo cuidado la salva
  }

  // ciclo completo (forma final + 6 h): la despedida NO salta sola; queda
  // lista (canFarewellNow) y la dispara el usuario con el boton, para que la vea

  // autoguardado periodico: NO escribir a flash aqui (corre dentro del loop,
  // mientras se anima); solo marcar y dejar que el loop lo vuelque al atenuar
  if (++ticksSinceSave >= 5) pendingSave = true;
}

// Copies the creature into endedMon so it can be offered a party slot, since
// newEgg() is about to wipe every field. Only the two endings the player CHOSE
// qualify: a runaway ran off after an hour of total neglect, and letting it
// come back on the team would remove the cost from the one ending that has any.
// Brings a banked creature back as the live pet, frozen.
void Pet::reviveFrom(const PartyMon &m) {
  if (m.empty()) return;
  ceremony = CER_NONE;
  neglectTicks = 0;
  speciesId = m.dex;
  prevSpeciesId = -1;
  eggTaps = 0;
  starterPick = false;
  shiny = m.shiny != 0;
  ivAtk = m.ivAtk; ivDef = m.ivDef; ivSpe = m.ivSpe; ivHp = m.ivHp;
  trAtk = m.trAtk; trDef = m.trDef; trSpe = m.trSpe; trHp = m.trHp;
  for (int i = 0; i < MOVE_SLOTS; i++) moves[i] = m.moves[i];
  // Restore the banked level directly into the awake progression clock.
  ageMinutes = (uint32_t)(m.level ? m.level - 1 : 0) * MINUTES_PER_LEVEL;
  levelMinutes = ageMinutes;
  sleepLevelRemainder = 0;
  lastLearnLevel = level();     // do not replay every gate it already passed
  learnQCount = 0;
  medals = m.medals;
  careMistakes = 0;
  mistakeCooldown = 0;
  sleeping = false;
  bond = 0;
  bondToday = 0;
  berryKnown = false;
  weight = 0;
  fullness = joy = energy = 80;
  hygiene = 100;
  poops = 0;
  frozen = true;
  strncpy(nick, m.nick, sizeof(nick) - 1);
  nick[sizeof(nick) - 1] = 0;
  registerSpecies(speciesId);
  save();
}

void Pet::snapshotForParty() {
  endedKind = CER_NONE;
  if (isEgg()) return;
  if (ceremony != CER_FAREWELL && ceremony != CER_RELEASE) return;
  // An EARLY retire gives the creature up for good -- it is not banked at all.
  // Retiring one that has EARNED its farewell still banks it, because that is
  // simply the farewell reached by another button. retirePending is still set
  // here: update() snapshots before newEgg() spends it, and that ordering is
  // what this depends on, so retire_test drives the real update() rather than
  // calling the two halves by hand.
  if (retireIsEarly()) return;
  if (currentIsDigimon()) {
    uint16_t i=digimonIndex(speciesId); if(level()>digiBest[i]) digiBest[i]=level();
  }
  endedMon = PartyMon();
  endedMon.dex = speciesId;
  endedMon.level = level();
  endedMon.medals = medals;
  endedMon.ivAtk = ivAtk;
  endedMon.ivDef = ivDef;
  endedMon.ivSpe = ivSpe;
  endedMon.ivHp = ivHp;
  endedMon.trAtk = trAtk;
  endedMon.trDef = trDef;
  endedMon.trSpe = trSpe;
  endedMon.trHp = trHp;
  endedMon.shiny = shiny ? 1 : 0;
  for (int i = 0; i < MOVE_SLOTS; i++) endedMon.moves[i] = moves[i];  // frozen too
  strncpy(endedMon.nick, nick, sizeof(endedMon.nick) - 1);
  endedMon.nick[sizeof(endedMon.nick) - 1] = 0;
  endedKind = ceremony;
}

// vuelca el guardado periodico pendiente (lo llama el loop en un momento sin
// animacion para que el paron de la escritura a flash no se vea)
void Pet::saveNow() { save(); }

bool Pet::makeCurrentShiny() {
  if (isEgg() || ceremony != CER_NONE || shiny || speciesId < 1 || speciesId > DEX_COUNT) return false;
  shiny = true;
  registerSpecies(speciesId);
  return true;
}

void Pet::flushSave() {
  if (pendingSave) save();
}

void Pet::verifyCriticalProgress() {
  if (!persistenceReady) return;
  uint8_t activeSlot = prefs.getUChar("carea", 0);
  if (mergeProgressGuard46(*this, activeSlot)) {
    Serial.println("SAVE RECOVERY: runtime rollback healed");
    save();
  }
}

// La calma forja la defensa: cada hora de bienestar (descansando, o despierto
// con todo >= 40) da +1 de DEF, hasta el tope que permita el IV.
//
// Antes pedia 12 h SEGUIDAS y CUALQUIER desliz ponia el contador a cero, ademas
// de no contar el sueno. Simulando una vida entera (3 dias) eso daba 1 punto al
// jugador teoricamente perfecto (uno que actue cada minuto durante 72 h) y 0 a
// todos los demas, incluido uno que atienda cada 15 min: la comida cae 2/min,
// asi que quien no pase por el bicho cada media hora esta SIEMPRE por debajo de
// 40 y el contador no arrancaba nunca. La DEF era, en la practica, inentrenable.
// Ahora acumula en vez de resetear: un descuido cuesta los minutos malos, no
// todo el progreso.
void Pet::defTick(bool resting) {
  if (!resting && lowestStat() < 40) return;
  if (++goodTicks < DEF_TRAIN_TICKS) return;
  goodTicks = 0;
  if (trDef < trMaxDef()) trDef++;
}

// v3.57.3 ------------------------------------------------------------------
// Pokedex Search / egg hunt state.
//
// One 32-bit NVS key holds BOTH target and pity so a power cut can never leave
// a new target paired with an old counter (or vice versa). Bits 0..9 are dex,
// 10..12 are eligible misses, high 16 bits are a format marker.
static const uint32_t HUNT_MAGIC = 0xA5730000UL;
static const uint32_t HUNT_MAGIC_MASK = 0xFFFF0000UL;
static const uint16_t HUNT_DEX_MASK = 0x03FFU;   // enough for dex 1..809
static const uint8_t HUNT_PITY_CAP = 5;          // 5 misses -> 6th eligible guaranteed

void Pet::saveHuntState() {
  if (!persistenceReady) return;
  uint16_t d = (huntTarget >= 1 && huntTarget <= DEX_COUNT) ? (uint16_t)huntTarget : 0;
  uint8_t p = huntPity > HUNT_PITY_CAP ? HUNT_PITY_CAP : huntPity;
  uint32_t raw = HUNT_MAGIC | ((uint32_t)p << 10) | (uint32_t)(d & HUNT_DEX_MASK);
  if (prefs.putUInt("hunt", raw) != 4) Serial.println("SAVE ERROR: hunt state write failed");
}

void Pet::loadHuntState() {
  huntTarget = 0;
  huntPity = 0;
  if (!persistenceReady || !prefs.isKey("hunt")) return;
  uint32_t raw = prefs.getUInt("hunt", 0);
  if ((raw & HUNT_MAGIC_MASK) != HUNT_MAGIC) {
    Serial.println("SAVE RECOVERY: invalid hunt state ignored");
    return;
  }
  int16_t d = (int16_t)(raw & HUNT_DEX_MASK);
  uint8_t p = (uint8_t)((raw >> 10) & 0x07U);
  if (d < 1 || d > DEX_COUNT || p > HUNT_PITY_CAP) {
    Serial.println("SAVE RECOVERY: out-of-range hunt state ignored");
    return;
  }
  // Do not retain a target that cannot ever be displayed/raised in this build.
  int16_t base = huntBaseFor(d);
  if (base < 1 || !speciesHasArt(base) || !speciesHasArt(d)) {
    Serial.println("SAVE RECOVERY: unavailable hunt target cleared");
    return;
  }
  huntTarget = d;
  huntPity = p;
}

void Pet::noteHuntRoll(bool hit) {
  if (huntTarget < 1) return;
  if (hit) huntPity = 0;
  else if (huntPity < HUNT_PITY_CAP) huntPity++;
  saveHuntState();
}

// Return the earliest species in the target's normal hatchable line.
// This is intentionally derived from dex.h rather than a second hand-maintained
// table, so adding ordinary evolutions cannot silently break Search. Eevee is
// the one branch represented outside DexEntry and is handled explicitly.
int16_t Pet::huntBaseFor(int16_t target) const {
  if (target < 1 || target > DEX_COUNT || !speciesHasArt(target)) return 0;
  // Search the generated evolution graph instead of following DexEntry's one
  // legacy arrow. This is what makes second/third-stage Galar/Hisui/Paldea
  // species and every split evolution reachable by Pokedex Search.
  for (int16_t base = 1; base <= DEX_COUNT; base++) {
    if (DEX_TBL[base].rarity == R_EVO || !speciesHasArt(base)) continue;
    int16_t stack[256]; uint16_t sp = 0;
    int16_t seen[256]; uint16_t sn = 0;
    stack[sp++] = base;
    while (sp) {
      int16_t cur = stack[--sp];
      bool dup = false; for (uint16_t k=0;k<sn;k++) if (seen[k]==cur) { dup=true; break; }
      if (dup) continue;
      if (sn < 256) seen[sn++] = cur;
      if (cur == target) return base;
      int16_t opts[MAX_EVO_OPTIONS]; uint8_t n = evolutionOptions(cur, opts, MAX_EVO_OPTIONS);
      for (uint8_t i=0;i<n && sp<256;i++) stack[sp++] = opts[i];
    }
  }
  return 0;
}

bool Pet::huntCanTarget(int16_t target) const {
  int16_t base = huntBaseFor(target);
  if (base < 1 || !speciesHasArt(base) || !speciesHasArt(target)) return false;
  uint8_t r = regionOfDex(base);
  return r == REGION_ALL || regionAvailable(r);
}

void Pet::toggleHuntTarget(int16_t target) {
  if (huntTarget == target) {
    huntTarget = 0;
    huntPity = 0;
    saveHuntState();
    return;
  }
  if (!huntCanTarget(target)) return;

  huntTarget = target;
  huntPity = 0;
  saveHuntState();

  // Casual rule: selecting a target also points the egg-region chooser at the
  // hatchable ancestor's region. If an egg is already waiting, setRegion keeps
  // its rarity and the existing anti-reroll cache; it does NOT advance pity.
  int16_t base = huntBaseFor(target);
  uint8_t r = regionOfDex(base);
  if (r != REGION_ALL && r < REGION_COUNT && regionAvailable(r)) setRegion(r);
}

// quedan miembros sin registrar en la linea evolutiva de esta base?
bool Pet::lineHasUnregistered(int16_t base) const {
  if (base < 1 || base > DEX_COUNT) return false;
  int16_t stack[256]; uint16_t sp=0;
  int16_t seen[256]; uint16_t sn=0;
  stack[sp++]=base;
  while (sp) {
    int16_t cur=stack[--sp];
    bool dup=false; for(uint16_t k=0;k<sn;k++) if(seen[k]==cur){dup=true;break;}
    if(dup) continue;
    if(sn<256) seen[sn++]=cur;
    if(!isRegistered(cur)) return true;
    int16_t opts[MAX_EVO_OPTIONS]; uint8_t n=evolutionOptions(cur,opts,MAX_EVO_OPTIONS);
    for(uint8_t i=0;i<n && sp<256;i++) stack[sp++]=opts[i];
  }
  return false;
}

uint8_t Pet::eeveeOptions(int16_t *out) const {
  uint8_t n = 0;
  for (uint8_t i = 0; i < EEVEE_EVO_COUNT; i++) {
    int16_t b = EEVEE_EVOS[i];
    if (b < 1 || b > DEX_COUNT) continue;
    if (!speciesHasArt(b)) continue;                     // no art anywhere
    if (!regionAvailable(regionOfDex(b))) continue;      // pack not on the card
    out[n++] = b;
  }
  return n;
}

uint8_t Pet::evolutionOptions(int16_t base, int16_t *out, uint8_t cap) const {
  if (!out || cap == 0 || base < 1 || base > DEX_COUNT) return 0;
  if (base == DEX_EEVEE) {
    int16_t tmp[EEVEE_EVO_COUNT];
    uint8_t n = eeveeOptions(tmp);
    if (n > cap) n = cap;
    for (uint8_t i = 0; i < n; i++) out[i] = tmp[i];
    return n;
  }
  uint8_t n = 0;
  auto addUnique = [&](int16_t d) {
    if (n >= cap || d < 1 || d > DEX_COUNT) return;
    if (!speciesHasArt(d) || !regionAvailable(regionOfDex(d))) return;
    for (uint8_t j = 0; j < n; j++) if (out[j] == d) return;
    out[n++] = d;
  };
  addUnique(DEX_TBL[base].evolvesTo);
  for (uint8_t i = 0; i < ALOLA_BRANCH_COUNT; i++)
    if (ALOLA_BRANCH_BASES[i] == base) addUnique(ALOLA_BRANCH_EVOS[i]);
  for (uint16_t i = 0; i < EXTRA_BRANCH_COUNT; i++)
    if (EXTRA_BRANCH_BASES[i] == base) addUnique(EXTRA_BRANCH_EVOS[i]);
  return n;
}

uint8_t Pet::eggRarity() const {
  return (eggTarget >= 1 && eggTarget <= DEX_COUNT) ? DEX_TBL[eggTarget].rarity : R_COMUN;
}

// elige la especie del huevo: tirada de rareza (mejorada por una despedida
// completa, castigada por una escapada) y sesgo hacia lineas incompletas
// Room for the candidate list. A whole rarity tier of a 386-species dex is far
// more than the 80 the Kanto-only build needed.
#define CAND_MAX 512

uint16_t gRegionArt = 0xFFFF;   // everything, until the SD narrows it

bool regionAvailable(uint8_t r) {
  if (r >= REGION_COUNT) return false;
  if (r == REGION_ALL) {                       // the mixed pool: any pack will do
    for (uint8_t i = 0; i < REGION_COUNT; i++)
      if (i != REGION_ALL && (gRegionArt & (uint16_t)(1u << i))) return true;
    return false;
  }
  return (gRegionArt & (uint16_t)(1u << r)) != 0;
}

uint8_t regionOfDex(int16_t d) {
  if (d < 1 || d > DEX_COUNT) return REGION_ALL;
  uint8_t r = DEX_REGION[d];
  return (r < REGION_ALL) ? r : REGION_ALL;
}

uint16_t regionDexCount(uint8_t r) {
  uint16_t n = 0;
  for (int16_t d = 1; d <= DEX_COUNT; d++) {
    if (!speciesHasArt(d)) continue;
    if (r == REGION_ALL || regionOfDex(d) == r) n++;
  }
  return n;
}

int16_t regionDexAt(uint8_t r, uint16_t ordinal) {
  for (int16_t d = 1; d <= DEX_COUNT; d++) {
    if (!speciesHasArt(d)) continue;
    if (r != REGION_ALL && regionOfDex(d) != r) continue;
    if (ordinal == 0) return d;
    ordinal--;
  }
  return 0;
}

bool dexHasEvolution(int16_t d) {
  if (d < 1 || d > DEX_COUNT) return false;
  int16_t n = DEX_TBL[d].evolvesTo;
  if (n >= 1 && n <= DEX_COUNT && speciesHasArt(n)) return true;
  if (d == DEX_EEVEE) {
    for (uint8_t i=0;i<EEVEE_EVO_COUNT;i++) if (speciesHasArt(EEVEE_EVOS[i])) return true;
  }
  for (uint8_t i=0;i<ALOLA_BRANCH_COUNT;i++) if (ALOLA_BRANCH_BASES[i]==d && speciesHasArt(ALOLA_BRANCH_EVOS[i])) return true;
  for (uint16_t i=0;i<EXTRA_BRANCH_COUNT;i++) if (EXTRA_BRANCH_BASES[i]==d && speciesHasArt(EXTRA_BRANCH_EVOS[i])) return true;
  return false;
}

uint8_t dexEvolutionLevel(int16_t d) {
  if (d < 1 || d > DEX_COUNT) return 0;
  // v3.62.0: derive the displayed/overall gate from REAL outgoing edges.
  // Previously an extra-only evolution (for example Charizard -> Mega X) was
  // given the generic special-evolution fallback Lv.30 before its explicit
  // Lv.70 branch was inspected.  The target filter still blocked the actual
  // evolution, but UI/canEvolveNow could incorrectly say it was ready at 30.
  uint8_t need = 0;
  auto take = [&](uint8_t v) {
    if (!v) v = 30;
    if (!need || v < need) need = v;
  };
  int16_t direct = DEX_TBL[d].evolvesTo;
  if (direct >= 1 && direct <= DEX_COUNT && speciesHasArt(direct))
    take(DEX_TBL[d].evolveLevel);
  if (d == DEX_EEVEE) {
    for (uint8_t i=0;i<EEVEE_EVO_COUNT;i++)
      if (speciesHasArt(EEVEE_EVOS[i])) { take(DEX_TBL[d].evolveLevel); break; }
  }
  for (uint8_t i=0;i<ALOLA_BRANCH_COUNT;i++)
    if (ALOLA_BRANCH_BASES[i] == d && speciesHasArt(ALOLA_BRANCH_EVOS[i]))
      take(DEX_TBL[d].evolveLevel);
  for (uint16_t i=0;i<EXTRA_BRANCH_COUNT;i++) {
    if (EXTRA_BRANCH_BASES[i] != d) continue;
    int16_t target = EXTRA_BRANCH_EVOS[i];
    if (target < 1 || target > DEX_COUNT || !speciesHasArt(target)) continue;
    take(EXTRA_BRANCH_LEVELS[i]);
  }
  // Generated catalogs are also validated in Actions, but never let a bad or
  // future upstream value create an unreachable level on-device.
  if (need > MAX_LEVEL) need = MAX_LEVEL;
  return need;
}

uint8_t effectiveEvolutionLevel(int16_t d, uint8_t careMistakes, uint8_t evoPenalty) {
  uint16_t base = dexEvolutionLevel(d);
  if (!base) return 0;
  uint16_t need = base + (uint16_t)careMistakes + (uint16_t)evoPenalty;
  if (need > MAX_LEVEL) need = MAX_LEVEL;
  return (uint8_t)need;
}

// v3.62.0: split evolutions can have different level gates.  The old branch
// selector considered every target as soon as the LOWEST branch level was met,
// which would let a Lv.70 Mega branch appear at an ordinary Lv.30 form change.
// Return the threshold for one concrete base->target edge instead.
static uint8_t evolutionTargetLevel(int16_t base, int16_t target) {
  if (base < 1 || base > DEX_COUNT || target < 1 || target > DEX_COUNT) return 0;
  uint8_t need = 0;
  auto take = [&](uint8_t v) {
    if (!v) v = 30;
    if (!need || v < need) need = v;
  };
  if (DEX_TBL[base].evolvesTo == target) take(DEX_TBL[base].evolveLevel);
  if (base == DEX_EEVEE) {
    for (uint8_t i=0;i<EEVEE_EVO_COUNT;i++) if (EEVEE_EVOS[i] == target) take(DEX_TBL[base].evolveLevel);
  }
  for (uint8_t i=0;i<ALOLA_BRANCH_COUNT;i++)
    if (ALOLA_BRANCH_BASES[i] == base && ALOLA_BRANCH_EVOS[i] == target) take(DEX_TBL[base].evolveLevel);
  for (uint16_t i=0;i<EXTRA_BRANCH_COUNT;i++)
    if (EXTRA_BRANCH_BASES[i] == base && EXTRA_BRANCH_EVOS[i] == target) take(EXTRA_BRANCH_LEVELS[i]);
  if (!need) need = dexEvolutionLevel(base);
  if (need > MAX_LEVEL) need = MAX_LEVEL;
  return need;
}

static uint8_t effectiveEvolutionTargetLevel(int16_t base, int16_t target, uint8_t careMistakes, uint8_t evoPenalty) {
  uint16_t need = evolutionTargetLevel(base,target);
  if (!need) return 0;
  need += (uint16_t)careMistakes + (uint16_t)evoPenalty;
  if (need > MAX_LEVEL) need = MAX_LEVEL;
  return (uint8_t)need;
}

uint8_t nextAvailableRegion(uint8_t from) {
  for (uint8_t i = 1; i <= REGION_COUNT; i++) {
    uint8_t r = (uint8_t)((from + i) % REGION_COUNT);
    if (regionAvailable(r)) return r;
  }
  return from;                      // nothing available anywhere: stay put
}

// The region to actually hatch from. Normally the player's own, but a card can
// be swapped under a save: rather than rewrite their choice (which would lose
// it silently the moment they put the right card back), the CHOICE is kept and
// only the roll falls through to somewhere playable.
static uint8_t eggRegionFallback(uint8_t want) {
  if (regionAvailable(want)) return want;
  for (uint8_t i = 0; i < REGION_COUNT; i++)
    if (i != REGION_ALL && regionAvailable(i)) return i;
  return want;                      // no art anywhere: behave as we always did
}

int16_t Pet::pickEggSpecies(bool countHunt) {
  const uint8_t use = eggRegionFallback(region % REGION_COUNT);
  const RegionInfo &rg = REGIONS[use];
  // primera partida: inicial clasico -- del region elegida, so a Johto run
  // starts with a Johto starter rather than a Kanto one. Search never alters
  // the very first starter choice.
  if (registeredCount() == 0) {
    return rg.starters[random(rg.starterCount)];
  }

  uint8_t tier = R_COMUN;
  if (lastEnd != CER_RUNAWAY) {
    bool blessed = (lastEnd == CER_FAREWELL);
    int rare = (blessed ? 45 : 27) + careBonus();
    int leg = (registeredCount() >= 25) ? (blessed ? 10 : 3) + careBonus() / 3 : 0;
    int r = random(100);
    if (r < leg) tier = R_LEGENDARIO;
    else if (r < leg + rare) tier = R_RARO;
  }

  int16_t huntBase = huntBaseFor(huntTarget);

  // candidatos del tier con linea incompleta; si no hay, baja de tier;
  // si la pokedex del tier esta completa, vale cualquiera del tier.
  // Search preserves this rarity selection completely: it only weights a target
  // when that target's hatchable ancestor is already a valid candidate in the
  // tier that this egg actually received.
  for (int pass = 0; pass < 2; pass++) {
    for (int t = tier; t >= R_COMUN; t--) {
      int16_t cand[CAND_MAX];
      int n = 0;
      int huntIdx = -1;
      // On the target's own rarity tier Search deliberately uses the FULL tier
      // pool, not the normal "unfinished dex lines" subset. Otherwise a fully
      // completed dex would leave only the hunted line in pass 0 and turn the
      // advertised 3x weight into an accidental 100% chance.
      const bool huntTier = huntBase >= 1 && (use == REGION_ALL || regionOfDex(huntBase) == use) &&
                            DEX_TBL[huntBase].rarity == t &&
                            regionAvailable(regionOfDex(huntBase)) && speciesHasArt(huntBase);
      for (int16_t d = 1; d <= DEX_COUNT && n < CAND_MAX; d++) {
        if (use != REGION_ALL && regionOfDex(d) != use) continue;
        if (DEX_TBL[d].rarity != t) continue;
        if (pass == 0 && !huntTier && !lineHasUnregistered(d)) continue;
        if (!regionAvailable(regionOfDex(d))) continue;
        if (!speciesHasArt(d)) continue;
        if (d == huntBase) huntIdx = n;
        cand[n++] = d;
      }
      if (n > 0) {
        const bool eligible = huntIdx >= 0 && huntBase >= 1 && DEX_TBL[huntBase].rarity == t;
        int16_t chosen;
        if (eligible && huntPity >= HUNT_PITY_CAP) {
          chosen = huntBase;                 // 6th eligible roll: guaranteed
        } else if (eligible) {
          // 3x total weight for the desired hatchable line: its normal slot +
          // two extra virtual slots. No dynamic allocations, no duplicated list.
          int pick = (int)random(n + 2);
          chosen = (pick >= n) ? huntBase : cand[pick];
        } else {
          chosen = cand[random(n)];
        }
        if (eligible && countHunt) {
          huntRollPending = true;
          huntRollHit = (chosen == huntBase);
        }
        return chosen;
      }
    }
  }
  return rg.starters[random(rg.starterCount)];  // inalcanzable, por si acaso
}

// Rolls a species of a GIVEN tier inside a region. Used when the player changes
// region while an egg is waiting: the rarity they were granted is kept and only
// the region changes, so switching cannot be farmed for a legendary.
int16_t Pet::rollInRegion(uint8_t r, uint8_t tier) {
  const uint8_t use = eggRegionFallback(r % REGION_COUNT);
  const RegionInfo &rg = REGIONS[use];
  int16_t huntBase = huntBaseFor(huntTarget);
  for (int t = tier; t >= R_COMUN; t--) {
    int16_t cand[CAND_MAX];
    int n = 0, huntIdx = -1;
    for (int16_t d = 1; d <= DEX_COUNT && n < CAND_MAX; d++) {
      if (use != REGION_ALL && regionOfDex(d) != use) continue;
      if (DEX_TBL[d].rarity != t || !regionAvailable(regionOfDex(d)) ||
          !speciesHasArt(d)) continue;
      if (d == huntBase) huntIdx = n;
      cand[n++] = d;
    }
    if (n) {
      // Region switching is intentionally NOT a pity roll: the existing egg
      // can be aimed/bias-weighted, but flipping the pill cannot farm the 6-cap.
      if (huntIdx >= 0 && huntBase >= 1 && DEX_TBL[huntBase].rarity == t) {
        int pick = (int)random(n + 2);
        return (pick >= n) ? huntBase : cand[pick];
      }
      return cand[random(n)];
    }
  }
  return rg.starters[0];      // a region with nothing in it cannot happen
}

// Changing region swaps the WAITING egg to that region's creature.
//
// Without this the setting would look broken: you would pick Johto and still
// hatch a Rattata, because the species is rolled when the egg appears and not
// when it cracks. Two rules stop it becoming a re-roll button:
//
//   1. The rarity tier is kept. Only which species of that tier changes, so
//      toggling can never be farmed for a legendary.
//   2. Each region's answer is REMEMBERED for this egg. Switching back shows
//      the same creature again, so there is nothing to gain by flipping.
//
// A hatched creature is untouched -- this only ever moves an egg.
void Pet::setRegion(uint8_t r) {
  r %= REGION_COUNT;
  // The sprite pack is a real gate, not a hint: without it the region is not
  // selectable at all. The chooser still SHOWS it, greyed and with a reason --
  // hiding it outright is how Johto and Hoenn once came to look absent when
  // they were built and reachable all along.
  if (!regionAvailable(r)) return;
  if (r == region) return;
  uint8_t old = region;
  region = r;
  eggSource = r;
  if (isEgg() && eggTarget >= 1) {
    if (old < REGION_COUNT) eggByRegion[old] = eggTarget;
    int16_t known = eggByRegion[r];
    eggTarget = known >= 1 ? known : rollInRegion(r, eggRarity());
    eggByRegion[r] = eggTarget;
  }
  save();
}

void Pet::setDigimonVersion(uint8_t version) {
  bool dmc=version>=1&&version<=5, pen=version>=10&&version<=15;
  if (!dmc && !pen) return;
  uint8_t slot=dmc?(version-1):(version==10?5:version-5);
  uint16_t baby=0; for(uint16_t i=0;i<DIGI_SPECIES_COUNT;i++)if(DIGI_SPECIES[i].version==version&&DIGI_SPECIES[i].stage==DIGI_BABY1){baby=i;break;}
  eggSource = (uint8_t)(REGION_COUNT + slot);
  if (isEgg()) {
    eggTarget = makeDigimonId(baby);
    eggShiny = false;
    starterPick = false;
  }
  save();
}

void Pet::registerSpecies(int16_t dex) {
  if (isDigimonId(dex)) {
    uint16_t i=digimonIndex(dex); digiReg[i>>3]|=(1<<(i&7));
    if(level()>digiBest[i]) digiBest[i]=level();
    return;
  }
  if (dex < 1 || dex > DEX_COUNT) return;
  dexReg[(dex - 1) >> 3] |= (1 << ((dex - 1) & 7));
  if (shiny) dexShinyReg[(dex - 1) >> 3] |= (1 << ((dex - 1) & 7));
}

// la racha y el vinculo mejoran el sorteo del huevo (0..~14)
int Pet::careBonus() const {
  int s = streak > 30 ? 30 : streak;
  return s / 3 + bond / 25;
}

// primer cuidado del dia: avanza la racha y afianza el vinculo
void Pet::registerCare() {
  if (isEgg() || ceremony != CER_NONE) return;
  uint32_t d = today();
  if (d == 0 || d == lastCareDay) return;  // sin reloj, o ya conto hoy
  if (lastCareDay == 0 || d == lastCareDay + 1) {
    streak++;
  } else {
    streak = 1;        // hubo un hueco de dias
    lastMilestone = 0;
  }
  lastCareDay = d;
  bondToday = 0;
  if (streak > bestStreak) bestStreak = streak;
  bond = clamp100(bond + 4);
  uint16_t ms = (streak >= 100) ? 100 : (streak >= 30) ? 30
              : (streak >= 7)   ? 7   : (streak >= 3)  ? 3 : 0;
  if (ms > lastMilestone) {
    lastMilestone = ms;
    milestoneUntil = millis() + 4500;
  }
  checkMedals();
  save();
}

void Pet::addBond(uint8_t amt) {
  if (bondToday >= 20) return;  // tope diario: el vinculo no se farmea
  bond = clamp100(bond + amt);
  bondToday += amt;
}

void Pet::checkMedals() {
  if (isEgg()) return;
  uint16_t before = medals;
  if (level() >= 10) medals |= MED_LV10;
  if (level() >= 25) medals |= MED_LV25;
  if (level() >= 50) medals |= MED_LV50;
  if (berryKnown) medals |= MED_BERRY;
  if (streak >= 7) medals |= MED_STREAK7;
  if (bond >= 100) medals |= MED_BOND;
  if (!dexHasEvolution(speciesId)) medals |= MED_FINAL;
  if (weight == 0 && level() >= 5 && careMistakes == 0) medals |= MED_FIT;
  uint16_t gained = medals & ~before;
  if (gained) {
    for (uint16_t m = gained; m; m &= (m - 1)) totalMedals++;
    newMedal = gained;
    medalUntil = millis() + 4000;
    if (!sleeping) sfxPlay(SFX_MEDAL);
    save();
  }
}

void Pet::rename(const char *name) {
  strncpy(nick, name, sizeof(nick) - 1);
  nick[sizeof(nick) - 1] = 0;
  save();
}

// La aportacion del IV (IV x nivel / 100) es exactamente la de los juegos de
// 3a generacion en adelante: un IV perfecto vale +31 a nivel 100. El resto de
// la formula es la de TamaPoke (base plana + nivel) y no la de los juegos: con
// el x nivel/100 canonico sobre la base, un bicho recien nacido mostraria
// stats de un solo digito, que en una pantalla de mascota parece un error.
static uint16_t calcStat(uint8_t base, uint8_t iv, uint8_t lvl, uint8_t tr) {
  return (uint16_t)base + lvl + (uint16_t)iv * lvl / 100 + tr;
}

uint16_t Pet::atkStat() const {
  if (isEgg()) return 0;
  uint8_t p = personalityIdFor(speciesId, ivAtk, ivDef, ivSpe, ivHp);
  return personalityApply(calcStat(creatureBaseAtk(speciesId), ivAtk, level(), trAtk), p, PST_ATK);
}
uint16_t Pet::defStat() const {
  if (isEgg()) return 0;
  uint8_t p = personalityIdFor(speciesId, ivAtk, ivDef, ivSpe, ivHp);
  return personalityApply(calcStat(creatureBaseDef(speciesId), ivDef, level(), trDef), p, PST_DEF);
}
uint16_t Pet::speStat() const {
  if (isEgg()) return 0;
  uint8_t p = personalityIdFor(speciesId, ivAtk, ivDef, ivSpe, ivHp);
  return personalityApply(calcStat(creatureBaseSpe(speciesId), ivSpe, level(), trSpe), p, PST_SPE);
}
// HP keeps the existing +10 TamaPoke rule, then the Tough personality can add 5%.
uint16_t Pet::vitStat() const {
  if (isEgg()) return 0;
  uint8_t p = personalityIdFor(speciesId, ivAtk, ivDef, ivSpe, ivHp);
  return personalityApply(calcStat(creatureBaseHp(speciesId), ivHp, level(), (uint8_t)(10 + trHp)), p, PST_HP);
}
uint16_t Pet::spaStat() const {
  if (isEgg()) return 0;
  uint8_t p = personalityIdFor(speciesId, ivAtk, ivDef, ivSpe, ivHp);
  return personalityApply(calcStat(creatureBaseSpA(speciesId), ivAtk, level(), trAtk), p, PST_SPA);
}
uint16_t Pet::spdStat() const {
  if (isEgg()) return 0;
  uint8_t p = personalityIdFor(speciesId, ivAtk, ivDef, ivSpe, ivHp);
  return personalityApply(calcStat(creatureBaseSpD(speciesId), ivDef, level(), trDef), p, PST_SPD);
}

// ---------- moves ----------

uint8_t Pet::moveCount() const {
  uint8_t n = 0;
  for (int i = 0; i < MOVE_SLOTS; i++)
    if (moves[i]) n++;
  return n;
}

bool Pet::knowsMove(uint8_t mv) const {
  if (!mv) return false;
  for (int i = 0; i < MOVE_SLOTS; i++)
    if (moves[i] == mv) return true;
  return false;
}

// Picks a sensible default set, best first.
//
// NOT "the newest four": 1907 of the 2281 learnset entries sit at level 0 and
// another 237 at level 1, so for most species level orders nothing and taking
// the last four in table order is arbitrary -- it handed a level 100 Charizard
// GROWL and LEER. So score instead: attacks over status, stronger over weaker,
// STAB ahead of equal power, and a bonus for the handful of moves that really
// are gated behind a level, since those are meant to be upgrades.
// TMs unlock at one level, for everything.
//
// This replaced a power/2 curve, which was impossible for a player to predict
// (SURF at 45, ROCK SLIDE at 37) and dribbled unlocks out one at a time so none
// of them felt like anything. A single number is explainable in one sentence and
// lands on a seam the game already has: the first five leaders sit at 14-43, so
// you fight the early ladder on what your species actually learns, and TMs
// arrive as you enter the back half. A creature retires at 73 and caps at 100.
//
// It only works because dex_moves.py now carries the cheap early attacks --
// SCRATCH, PECK, POISON STING, BUBBLE and the rest. Without those, gating TMs
// this hard would leave young creatures with nothing at all, which is exactly
// what the power/2 version was papering over.
#define TM_LEVEL 40

static uint8_t tmLevelFor(const MoveEntry &m) {
  (void)m;
  return TM_LEVEL;
}

// THE single answer, used by relearnFromLevel(), by the STAB fallback and by
// the move picker in the sketch. Three call sites once had three opinions.
uint8_t moveUnlockLevel(int16_t dex, uint8_t idx) {
  uint8_t at = learnLevel(dex, idx);
  if (at > 0) return at;                 // a real level-up move
  uint8_t mv = learnMove(dex, idx);
  if (!mv || mv >= MOVE_COUNT) return 255;
  return tmLevelFor(MOVE_TBL[mv]);       // a TM: no natural level, so the gate
}

void Pet::relearnFromLevel() {
  for (int i = 0; i < MOVE_SLOTS; i++) moves[i] = 0;
  if (isEgg()) return;
  if (currentIsDigimon()) {
    digimonDefaultMoves(digimonIndex(speciesId),level(),ivAtk,ivDef,ivSpe,ivHp,moves);
    return;
  }
  const DexEntry &d = DEX_TBL[speciesId];
  uint8_t lvl = level(), n = learnCount(speciesId);
  if (n == 0 && DEX_NATDEX[speciesId] > 809) {
    fallbackMovesForDex(speciesId, lvl, moves, MOVE_SLOTS);
    return;
  }
  int16_t score[MOVE_SLOTS] = { 0, 0, 0, 0 };
  // Two passes. Level-up moves (level >= 1) are what a creature grows into, so
  // they fill the set first; TMs (level 0, no gate) only top up the slots left
  // over. Without this a just-hatched pet opens with FIRE BLAST and SOLAR BEAM,
  // because every TM is legal at level 1.
  for (int pass = 0; pass < 2; pass++) {
  bool tmPass = (pass == 1);
  for (uint8_t i = 0; i < n; i++) {
    uint8_t at = learnLevel(speciesId, i);
    if (at > lvl) continue;
    if (tmPass != (at == 0)) continue;
    uint8_t mv = learnMove(speciesId, i);
    if (!mv || mv >= MOVE_COUNT || knowsMove(mv)) continue;
    const MoveEntry &m = MOVE_TBL[mv];
    // A TM carries no level requirement in the data, which is true of the games
    // but wrong here: with only one or two level-up moves early on, the spare
    // slots were filled with the strongest TMs in the table and a NEWBORN opened
    // with SURF, BLIZZARD and OUTRAGE. That, not the damage formula, is why a
    // level 1 Squirtle could beat Brock.
    //
    // So a TM is gated by its own power: roughly power/2, which puts the 40s
    // around level 20 and the 110s out past 50 where a creature is genuinely
    // built. Level-up moves are untouched -- they already have real gates.
    if (tmPass && lvl < tmLevelFor(m)) continue;
    int16_t sc = (m.cat == MC_STATUS) ? 10 : (int16_t)m.power + 20;
    // STAB outweighs raw power, or every species defaults to the same two
    // generic sledgehammers and the roster loses its identity.
    if (m.cat != MC_STATUS && (m.type == d.type1 || m.type == d.type2)) sc += 40;
    if (m.effect == EF_RECHARGE) sc -= 35;   // a free turn for the opponent
    if (m.effect == EF_RECOIL) sc -= 20;
    sc += at;
    if (sc < 1) sc = 1;
    int slot = -1;
    for (int s = 0; s < MOVE_SLOTS; s++)
      if (sc > score[s]) { slot = s; break; }
    if (slot < 0) continue;
    for (int s = MOVE_SLOTS - 1; s > slot; s--) {
      score[s] = score[s - 1];
      moves[s] = moves[s - 1];
    }
    score[slot] = sc;
    moves[slot] = mv;
  }
  if (moveCount() >= MOVE_SLOTS) break;   // level-up moves already filled it
  }
  // Guarantee one same-type move. Machamp's only Fighting options are weak or
  // recoil-laden, so pure scoring left it with four generic attacks and nothing
  // that reads as a Machamp. If the set came out with no STAB, the weakest slot
  // gives way to the best same-type attack the species can actually learn.
  for (int i = 0; i < MOVE_SLOTS; i++) {
    if (!moves[i] || moves[i] >= MOVE_COUNT) continue;
    const MoveEntry &m = MOVE_TBL[moves[i]];
    if (m.cat != MC_STATUS && (m.type == d.type1 || m.type == d.type2)) return;
  }
  uint8_t best = 0;
  int16_t bestSc = 0;
  for (uint8_t i = 0; i < n; i++) {
    uint8_t at = learnLevel(speciesId, i);
    if (at > lvl) continue;
    uint8_t mv = learnMove(speciesId, i);
    if (!mv || mv >= MOVE_COUNT) continue;
    const MoveEntry &m = MOVE_TBL[mv];
    if (m.cat == MC_STATUS || (m.type != d.type1 && m.type != d.type2)) continue;
    // The same TM gate as above. This fallback used to ignore it, which is how
    // a level 1 Squirtle ended up holding SURF: it had no Water move, so the
    // guarantee reached past every check and handed it the best one in the
    // table. A creature with no STAB it can legally use simply has none yet.
    if (at == 0 && lvl < tmLevelFor(m)) continue;
    int16_t sc = (int16_t)m.power;
    if (m.effect == EF_RECHARGE) sc -= 35;
    if (m.effect == EF_RECOIL) sc -= 20;
    if (sc > bestSc) { bestSc = sc; best = mv; }
  }
  if (best) moves[MOVE_SLOTS - 1] = best;
}

// Queues every level-up move unlocked since the last check. A free slot is
// filled silently -- the games do not ask when there is room either -- and only
// a full moveset produces an offer the player has to answer.
void Pet::checkLearnGates() {
  if (isEgg() || ceremony != CER_NONE) return;
  uint8_t lvl = level();
  if (lvl <= lastLearnLevel) return;
  if (currentIsDigimon()) {
    if (lastLearnLevel == 0 || moveCount() == 0) relearnFromLevel();
    lastLearnLevel=lvl; pendingSave=true; return;
  }
  uint8_t n = learnCount(speciesId);
  for (uint8_t i = 0; i < n; i++) {
    uint8_t at = learnLevel(speciesId, i);
    if (at == 0 || at <= lastLearnLevel || at > lvl) continue;  // 0 = TM, no gate
    uint8_t mv = learnMove(speciesId, i);
    if (!mv || mv >= MOVE_COUNT || knowsMove(mv)) continue;
    int freeSlot = -1;
    for (int s = 0; s < MOVE_SLOTS; s++)
      if (!moves[s]) { freeSlot = s; break; }
    if (freeSlot >= 0) { moves[freeSlot] = mv; continue; }
    if (learnQCount >= sizeof(learnQueue)) continue;
    bool dup = false;
    for (uint8_t q = 0; q < learnQCount; q++)
      if (learnQueue[q] == mv) dup = true;
    if (!dup) learnQueue[learnQCount++] = mv;
  }
  lastLearnLevel = lvl;
  pendingSave = true;
}

static void popLearn(uint8_t *q, uint8_t &n) {
  if (!n) return;
  for (uint8_t i = 0; i + 1 < n; i++) q[i] = q[i + 1];
  q[--n] = 0;
}

void Pet::acceptLearn(uint8_t slot) {
  if (!learnQCount || slot >= MOVE_SLOTS) return;
  moves[slot] = learnQueue[0];
  popLearn(learnQueue, learnQCount);
  save();
}

void Pet::declineLearn() {
  popLearn(learnQueue, learnQCount);
  save();
}

uint8_t Pet::pendingLearnables(uint8_t *out, uint8_t max) const {
  if (isEgg() || !out || !max) return 0;
  if (currentIsDigimon()) return 0;
  uint8_t lvl = level(), n = learnCount(speciesId), w = 0;
  for (uint8_t i = 0; i < n && w < max; i++) {
    uint8_t at = learnLevel(speciesId, i);
    if (at == 0 || at > lvl) continue;   // TM entries and future gates are not pending level-up moves
    uint8_t mv = learnMove(speciesId, i);
    if (knowsMove(mv)) continue;
    bool dup = false;                     // do not offer the same move twice
    for (uint8_t j = 0; j < w; j++)
      if (out[j] == mv) { dup = true; break; }
    if (!dup) out[w++] = mv;
  }
  return w;
}

// Tirada de un IV: 8-31. El suelo en 8 es deliberado — en los juegos un 0 es
// posible porque puedes criar cientos de huevos; aqui cada crianza exige tiempo real
// y un individuo de desecho seria un castigo desproporcionado. La racha y el
// vinculo del bicho ANTERIOR empujan la tirada: cuidar bien mejora la camada.
uint8_t Pet::rollIV(int bonus) const {
  int v = 8 + (int)random(24) + bonus / 2;  // bonus 0..14 -> +0..7
  return (uint8_t)(v > 31 ? 31 : v);
}

// Guardados con el sistema viejo de genes (90-110%): se convierten al rango de
// IV que se sortea hoy (8-31) para que nadie salga perdiendo con la
// actualizacion. gene 0 = mascota anterior incluso a los genes.
uint8_t Pet::ivFromGene(uint8_t gene) const {
  if (gene == 0) return rollIV(0);
  if (gene < 90) gene = 90;
  if (gene > 110) gene = 110;
  return 8 + (uint8_t)(((uint16_t)(gene - 90) * 23) / 20);
}

void Pet::rollIVs() {
  int bonus = careBonus();
  ivAtk = rollIV(bonus);
  ivDef = rollIV(bonus);
  ivSpe = rollIV(bonus);
  ivHp = rollIV(bonus);
  // los legendarios nacen con 3 de 4 IV perfectos, como en los juegos
  if (speciesId >= 1 && speciesId <= DEX_COUNT && DEX_TBL[speciesId].rarity == R_LEGENDARIO) {
    uint8_t *p[4] = { &ivAtk, &ivDef, &ivSpe, &ivHp };
    for (int k = 3; k > 0; k--) {  // baraja para elegir cuales 3
      int j = random(k + 1);
      uint8_t *t = p[k]; p[k] = p[j]; p[j] = t;
    }
    for (int k = 0; k < 3; k++) *p[k] = 31;
  }
  // en la 2a generacion el shiny ERA un patron de DV concreto: un shiny nunca
  // era mediocre. Aqui se traduce como un suelo de 20 en todos los IV.
  if (shiny) {
    if (ivAtk < 20) ivAtk = 20;
    if (ivDef < 20) ivDef = 20;
    if (ivSpe < 20) ivSpe = 20;
    if (ivHp < 20) ivHp = 20;
  }
}

uint16_t Pet::registeredCount() const {
  uint16_t n = 0;
  for (int i = 1; i <= DEX_COUNT; i++)
    if (speciesHasArt(i) && isRegistered(i)) n++;
  return n;
}

// forma final que ya cumplio su ciclo (6 h): lista para despedirse. La
// despedida la dispara el usuario con el boton (no salta sola, para que la vea)
bool Pet::canFarewellNow() const {
  return canRetireNow();
}

// abandono total durante 1h: lista para escaparse. La dispara el usuario con el
// boton (final triste); cuidarla un solo tick la salva (neglectTicks se resetea)
bool Pet::canRunawayNow() const {
  if (frozen) return false;
  // inTotalNeglect() as well as the counter, and NOT just the counter. The
  // sleeping branch of tick() returns before the neglect block, so neglectTicks
  // is frozen rather than cleared for the whole night: a creature that went to
  // bed at zero woke with energy back at 100 and was still one tap from
  // leaving, until the next tick 60 s later cleared it. That tap is a caress --
  // the button is drawn over the creature -- so the window really was reachable
  // and it cost somebody a DRAGONAIR.
  return !isEgg() && !sleeping && ceremony == CER_NONE &&
         neglectTicks >= RUNAWAY_TICKS && inTotalNeglect();
}

bool Pet::canRetireNow() const {
  if (frozen) return false;     // a companion is never given up
  return !isEgg() && !sleeping && ceremony == CER_NONE && !starterPick;
}

// The ceremony is the same one; only the debt differs. Marked BEFORE the
// ceremony starts and spent by newEgg(), so a reset mid-ceremony loses the
// penalty rather than applying it to a creature that never got retired.
void Pet::startRetire() {
  if (!canRetireNow()) return;
  retirePending = false;
  save();
  startFarewell();
  // An early retire is NOT the good ending and must not pay like one.
  // startFarewell() sets lastEnd = CER_FAREWELL, which blesses the next egg --
  // rare 27% -> 45%, legendary 3% -> 10%, shiny 1/32 -> 1/16. Combined with the
  // creature no longer being banked, that made retiring early a pure SHINY FARM:
  // retire, check the egg, retire again, with nothing accumulating to regret.
  // Neutral instead, exactly like a release. The ceremony on screen is still the
  // farewell -- this was a choice the player made, not a neglected creature
  // walking out -- but the reward is not.
}

void Pet::startFarewell() {
  if (isEgg() || ceremony != CER_NONE) return;
  // A full good farewell is the deepest observation of a creature's life. An
  // early retire still teaches a little, but does not receive the full research.
  extras.addResearch(speciesId, retirePending ? 1 : 3);
  lastEnd = CER_FAREWELL;
  ceremony = CER_FAREWELL;
  ceremonyUntil = millis() + CEREMONY_MS;
  heartUntil = ceremonyUntil;  // corazones durante toda la despedida
  sfxPlay(SFX_BYE);
  save();
}

void Pet::startRunaway() {
  if (isEgg() || ceremony != CER_NONE) return;
  lastEnd = CER_RUNAWAY;
  ceremony = CER_RUNAWAY;
  ceremonyUntil = millis() + CEREMONY_MS;
  sfxPlay(SFX_BYE);
  save();
}

void Pet::release() {
  if (isEgg() || ceremony != CER_NONE) return;
  lastEnd = CER_RELEASE;
  ceremony = CER_RELEASE;
  ceremonyUntil = millis() + CEREMONY_MS;
  heartUntil = ceremonyUntil;
  sfxPlay(SFX_BYE);
  save();
}

void Pet::hatch() {
  speciesId = eggTarget;
  shiny = eggIsDigimon() ? false : eggShiny;
  // IV del individuo (cada crianza es unica). Se tiran ANTES de resetear el
  // vinculo a proposito: el careBonus que los empuja es el del bicho anterior.
  rollIVs();
  trAtk = trDef = trSpe = trHp = 0;
  evoDeclinedLv = 0;
  goodTicks = 0;
  berryKnown = false;
  bond = 0;          // vinculo, medallas y nombre son del individuo
  bondToday = 0;
  medals = 0;
  newMedal = 0;
  nick[0] = 0;
  registerSpecies(speciesId);  // criado = registrado en la pokedex
  // Start empty: checkLearnGates() fills the level-1 moves. Seeding from TMs
  // instead would hand a newborn FIRE BLAST, which no level 1 creature knows.
  for (int i = 0; i < MOVE_SLOTS; i++) moves[i] = 0;
  learnQCount = 0;
  lastLearnLevel = 0;
  checkLearnGates();
  checkMedals();     // por si nace ya en forma final (legendario)
  sfxPlay(SFX_HATCH);
  save();
}

// ¿se dan ya las condiciones para evolucionar? Cada descuido retrasa la
// evolucion 1 nivel, y ademas tiene que estar bien cuidado en ese momento
// (ninguna estadistica por debajo de 40). NO evoluciona sola: la dispara el
// usuario tocando al bicho (evolve()), para que vea la transformacion.
bool Pet::canEvolveNow() const {
  if (frozen) return false;     // frozen at the form it was banked in
  if (isEgg() || sleeping || ceremony != CER_NONE) return false;
  if (currentIsDigimon()) {
    uint16_t i=digimonIndex(speciesId);
    return digimonEvolutionTarget(i,level(),trAtk,trDef,trSpe,trHp,digiBest)!=i;
  }
  const DexEntry &d = DEX_TBL[speciesId];
  if (!dexHasEvolution(speciesId)) return false;
  uint8_t need = effectiveEvolutionLevel(speciesId, careMistakes, evoPen);
  // Every special/trade/item evolution is normalized by the generated catalog
  // to a level threshold. Care/retirement delays remain meaningful, but the
  // final gate can never overflow past the game's Lv.100 cap.
  return need > 0 && level() >= need && lowestStat() >= 40;
}

void Pet::evolve() {
  if (!canEvolveNow()) return;
  if (currentIsDigimon()) {
    uint16_t old=digimonIndex(speciesId);
    uint16_t next=digimonEvolutionTarget(old,level(),trAtk,trDef,trSpe,trHp,digiBest);
    if(next==old)return;
    prevSpeciesId=speciesId; speciesId=makeDigimonId(next); evoDeclinedLv=0; registerSpecies(speciesId);
    digimonDefaultMoves(next,level(),ivAtk,ivDef,ivSpe,ivHp,moves);
    sfxPlay(SFX_EVOLVE); evolveUntil=millis()+EVOLVE_ANIM_MS; save(); return;
  }
  const DexEntry &d = DEX_TBL[speciesId];
  prevSpeciesId = speciesId;
  int16_t next = d.evolvesTo;
  {
    const uint8_t cap = MAX_EVO_OPTIONS;
    int16_t opts[MAX_EVO_OPTIONS];
    uint8_t n = evolutionOptions(speciesId, opts, cap);
    // Respect each branch's own level.  This is especially important for the
    // permanent Lv.70 Mega evolutions added in v3.62.0.
    uint8_t eligible = 0;
    uint8_t lv = level();
    for (uint8_t i = 0; i < n; i++) {
      uint8_t need = effectiveEvolutionTargetLevel(speciesId, opts[i], careMistakes, evoPen);
      if (need && lv >= need) opts[eligible++] = opts[i];
    }
    n = eligible;
    if (n == 0) return;
    if (n == 1) next = opts[0];
    if (n > 1) {
      // Same collection-friendly rule as Eevee: Search target first, then an
      // unregistered branch, then random after both/all forms are collected.
      bool huntedBranch = false;
      if (huntTarget >= 1) {
        for (uint8_t i = 0; i < n; i++) {
          if (opts[i] == huntTarget) { next = huntTarget; huntedBranch = true; break; }
        }
      }
      if (!huntedBranch) {
        int16_t fresh[MAX_EVO_OPTIONS];
        uint8_t m = 0;
        for (uint8_t i = 0; i < n; i++) if (!isRegistered(opts[i])) fresh[m++] = opts[i];
        next = m ? fresh[random(m)] : opts[random(n)];
      }
    }
  }
  speciesId = next;
  evoDeclinedLv = 0;
  registerSpecies(speciesId);
  checkLearnGates();   // the new form may gate a move at this very level
  sfxPlay(SFX_EVOLVE);
  evolveUntil = millis() + EVOLVE_ANIM_MS;
  save();
}

void Pet::feed() {
  feedBerry(0);
}

void Pet::feedBerry(uint8_t color) {
  if (ceremony != CER_NONE) return;
  if (isEgg() || sleeping) return;
  if (lovesBerry(color)) {
    fullness = clamp100(fullness + 35);
    joy = clamp100(joy + 10);
    heartUntil = millis() + HEART_MS;  // "le encanta!"
    berryKnown = true;                 // descubierto: se muestra en la ficha
    addBond(2);
  } else {
    fullness = clamp100(fullness + 25);
  }
  eatUntil = millis() + EAT_ANIM_MS;
  registerCare();
  extras.missionAction(MIS_FEED, 1, *this);
  save();
}

void Pet::feedCandy() {
  if (ceremony != CER_NONE) return;
  if (isEgg() || sleeping) return;
  fullness = clamp100(fullness + 10);
  joy = clamp100(joy + 12);
  weight = clamp100(weight + 12);  // las chuches pasan factura
  eatUntil = millis() + EAT_ANIM_MS;
  registerCare();
  extras.missionAction(MIS_FEED, 1, *this);
  save();
}

void Pet::itemEatReaction() {
  if (ceremony != CER_NONE || isEgg() || sleeping) return;
  eatUntil = millis() + EAT_ANIM_MS;
}

uint8_t Pet::playResult(uint16_t score) {
  if (ceremony != CER_NONE || isEgg()) return 0;
  // DEF gauge game: BLOCK/GOOD/PERFECT award 1/2/3 score. Keep the
  // roughly-two-score-per-training-point pace and the same session ceiling.
  uint8_t before = trDef;
  uint16_t rawGain = (uint16_t)score * 2 / 3;
  uint8_t gain = rawGain > 24 ? 24 : (uint8_t)rawGain;
  uint16_t v = (uint16_t)trDef + gain;
  trDef = v > trMaxDef() ? trMaxDef() : (uint8_t)v;
  gain = trDef - before;
  uint16_t joyBonus = score > 15 ? 30 : score * 2;
  joy = clamp100(joy + 5 + (uint8_t)joyBonus);
  uint8_t eCost = (uint8_t)min<uint16_t>(ENERGY_DEF_MAX_COST, ENERGY_DEF_BASE_COST + score / ENERGY_DEF_SCORE_DIVISOR);
  energy = dropTo(energy, eCost, 8);
  fullness = dropTo(fullness, 5, 5);
  int burn = (int)weight - (int)min<uint16_t>(50, score);
  weight = burn > 0 ? burn : 0;
  if (score >= 5) heartUntil = millis() + HEART_MS;
  if (score > gameHi) gameHi = score;  // nuevo record
  // Training bonds, and it scales with the session: a token effort is worth the
  // base, a full one is worth more. The daily cap in addBond() still stops it
  // being farmed -- this changes how fast a good session gets there, not the
  // ceiling.
  addBond((uint8_t)(2 + gain / 6));
  registerCare();
  if (score > 0) {
    extras.missionAction(MIS_PLAY, 1, *this);
    extras.missionAction(MIS_TRAIN, 1, *this);
  }
  // v3.62.9: minigame saves are deferred until after the result screen.
  pendingSave = true;
  return gain;
}

// saco de entrenamiento: los golpes entrenan la fuerza. Devuelve la subida.
uint8_t Pet::rewardTraining(uint8_t amount, uint8_t &which) {
  which = 0;
  if (ceremony != CER_NONE || isEgg() || !amount) return 0;
  // Only the stats with headroom are candidates.
  uint8_t room[3], n = 0;
  if (trAtk < trMaxAtk()) room[n++] = 0;
  if (trDef < trMaxDef()) room[n++] = 1;
  if (trSpe < trMaxSpe()) room[n++] = 2;
  if (!n) return 0;                     // nothing left to train
  which = room[random(n)];
  uint8_t before, capped;
  switch (which) {
    case 0: before = trAtk; capped = trMaxAtk(); trAtk = (uint8_t)min<uint16_t>(before + amount, capped); amount = trAtk - before; break;
    case 1: before = trDef; capped = trMaxDef(); trDef = (uint8_t)min<uint16_t>(before + amount, capped); amount = trDef - before; break;
    default: before = trSpe; capped = trMaxSpe(); trSpe = (uint8_t)min<uint16_t>(before + amount, capped); amount = trSpe - before; break;
  }
  // The IV-bound ceiling is never crossed: a mediocre individual not reaching
  // as far is the whole point of trMaxFor().
  save();
  return amount;
}

uint8_t Pet::trainSpeed(uint16_t hits) {
  if (ceremony != CER_NONE || isEgg()) return 0;
  uint8_t gain = (uint16_t)hits * 2 / 3;
  if (gain > 24) gain = 24;
  uint8_t before = trSpe;
  uint8_t v = trSpe + gain;
  trSpe = v > trMaxSpe() ? trMaxSpe() : v;   // el IV pone el techo
  gain = trSpe - before;
  energy = dropTo(energy, ENERGY_SPE_COST, 8);
  fullness = dropTo(fullness, 4, 5);
  int burn = (int)weight - hits / 2;
  weight = burn > 0 ? burn : 0;
  joy = clamp100(joy + 4);
  if (hits > spdHi) spdHi = hits;
  // Training bonds, and it scales with the session: a token effort is worth the
  // base, a full one is worth more. The daily cap in addBond() still stops it
  // being farmed -- this changes how fast a good session gets there, not the
  // ceiling.
  addBond((uint8_t)(2 + gain / 6));
  registerCare();
  if (hits > 0) {
    extras.missionAction(MIS_PLAY, 1, *this);
    extras.missionAction(MIS_TRAIN, 1, *this);
  }
  // v3.62.9: avoid a full NVS commit inside the active minigame frame.
  pendingSave = true;
  return gain;
}

uint8_t Pet::trainStrength(uint16_t hits) {
  if (ceremony != CER_NONE || isEgg()) return 0;
  uint8_t gain = hits / 3;
  if (gain > 24) gain = 24;
  uint8_t before = trAtk;
  uint8_t v = trAtk + gain;
  trAtk = v > trMaxAtk() ? trMaxAtk() : v;  // el IV pone el techo
  gain = trAtk - before;            // lo que de verdad subio (puede topar)
  energy = dropTo(energy, ENERGY_ATK_COST, 8);
  fullness = dropTo(fullness, 5, 5);
  int burn = (int)weight - hits / 3;  // tambien quema peso
  weight = burn > 0 ? burn : 0;
  joy = clamp100(joy + 6);
  if (hits >= 20) heartUntil = millis() + HEART_MS;
  if (hits > strHi) strHi = hits;   // record de golpes
  // Training bonds, and it scales with the session: a token effort is worth the
  // base, a full one is worth more. The daily cap in addBond() still stops it
  // being farmed -- this changes how fast a good session gets there, not the
  // ceiling.
  addBond((uint8_t)(2 + gain / 6));
  registerCare();
  if (hits > 0) {
    extras.missionAction(MIS_PLAY, 1, *this);
    extras.missionAction(MIS_TRAIN, 1, *this);
  }
  // v3.62.9: avoid a full NVS commit inside the active minigame frame.
  pendingSave = true;
  return gain;
}

uint8_t Pet::trainVitality(uint16_t score) {
  if (ceremony != CER_NONE || isEgg()) return 0;
  uint16_t raw = (uint16_t)score * 2 / 3;
  uint8_t gain = raw > 24 ? 24 : (uint8_t)raw;
  uint8_t before = trHp;
  trHp = (uint8_t)min<uint16_t>((uint16_t)trHp + gain, trMaxHp());
  gain = trHp - before;
  energy = dropTo(energy, ENERGY_HP_COST, 8);
  fullness = dropTo(fullness, 4, 5);
  joy = clamp100(joy + 5);
  addBond((uint8_t)(2 + gain / 6));
  registerCare();
  if (score > 0) {
    extras.missionAction(MIS_PLAY, 1, *this);
    extras.missionAction(MIS_TRAIN, 1, *this);
  }
  pendingSave = true;
  return gain;
}

void Pet::play() {
  if (ceremony != CER_NONE) return;
  if (isEgg() || sleeping) return;
  joy = clamp100(joy + 25);
  energy = clamp100(energy - ENERGY_PLAY_COST);
  fullness = clamp100(fullness - 5);
  heartUntil = millis() + HEART_MS;
  addBond(2);
  registerCare();
  save();
}

// The hour off the RTC, the same source the scene uses. With no clock at all
// there is no night, so a board that has never been set never auto-sleeps.
bool Pet::isNightHour() const {
  if (!lastSeenEpoch) return false;
  int h = (int)((lastSeenEpoch / 3600) % 24);
  // The window may or may not cross midnight, and it must keep working either
  // way: with NIGHT_START 0 the old `h >= START || h < END` was true for every
  // hour of the day, which put the creature to sleep the moment the screen went
  // off at noon. sleep_test catches it, and did.
  if (NIGHT_START < NIGHT_END) return h >= NIGHT_START && h < NIGHT_END;
  return h >= NIGHT_START || h < NIGHT_END;
}

// Auto-sleep needs the screen off AND the night hours, and is re-checked every
// tick rather than only on the button, so a device put down at 21:00 nods off
// at 22:00 and gets up at 06:00 without anyone touching it.
//
// Both halves earn their place. Screen-off alone paused the game whenever you
// put the device down, and the creature is meant to get hungry during the day.
// The hour alone sent it to bed while you were still playing with it.
//
// Only what this put to sleep is woken by it: a creature the player sent to bed
// with the light button stays there until the player says otherwise.
void Pet::applyAutoSleep() {
  if (isEgg() || ceremony != CER_NONE) return;
  bool night = isNightHour();
  if (screenIsOff && night && !sleeping && sleepAuto != SLEEP_PLAYER) {
    sleeping = true;
    sleepAuto = SLEEP_AUTO;
    pendingSave = true;
  }
  // NOTHING wakes it here, and that is the whole point. Waking at 06:00 would
  // reopen the hole this exists to close: from the sleep floors, food is empty
  // by 06:15 and every stat by 07:40, so anyone who sleeps past eight would
  // find the creature ready to run away again. It sleeps until YOU are up,
  // which is the screen coming back on.
  if (!night && sleepAuto == SLEEP_PLAYER) sleepAuto = SLEEP_NONE;  // a new day
}

void Pet::setScreenOff(bool off) {
  screenIsOff = off;
  // Coming back to the device is what wakes it -- only if the device is what
  // put it to sleep. A creature sent to bed with the light stays there.
  if (!off && sleeping && sleepAuto == SLEEP_AUTO) {
    sleeping = false;
    sleepAuto = SLEEP_NONE;
  }
  applyAutoSleep();
  save();
}

void Pet::toggleLight() {
  if (ceremony != CER_NONE) return;
  if (isEgg()) return;
  sleeping = !sleeping;
  // The player's hand beats the clock until morning: waking it at 23:00 must
  // not be undone a minute later by the auto-sleep, and neither must putting
  // it to bed early.
  sleepAuto = SLEEP_PLAYER;
  save();
}

void Pet::clean() {
  if (ceremony != CER_NONE) return;
  poops = 0;
  hygiene = 100;
  addBond(1);
  registerCare();
  extras.missionAction(MIS_CLEAN, 1, *this);
  save();
}

void Pet::caress() {
  if (ceremony != CER_NONE) return;
  if (isEgg() || sleeping) return;
  joy = clamp100(joy + 5);
  heartUntil = millis() + HEART_MS;
  addBond(1);
  registerCare();
  extras.missionAction(MIS_CARE, 1, *this);
  save();
}

void Pet::eggTap() {
  if (!isEgg()) return;
  if (++eggTaps >= 3) hatch();
  else save();
}

PetMood Pet::mood() const {
  if (sleeping) return MOOD_SLEEPING;
  if (eating()) return MOOD_EATING;
  if (lowestStat() < 25) return MOOD_SAD;
  return MOOD_HAPPY;
}

void Pet::save() {
  if (!persistenceReady) return;  // local battle/test Pet objects never own NVS
  ticksSinceSave = 0;
  pendingSave = false;

  // Before committing, refuse to let an accidental in-RAM decrease erase
  // earned badges/training. During a journalled care-slot switch, however, the
  // scalar pet and carea may intentionally be between states. Never associate
  // training with a slot until CareSlots commits/recoveries that transaction.
  uint8_t activeSlot = prefs.getUChar("carea", 0);
  const bool careSwitchPending = prefs.getBool("caretx", false);
  if (!careSwitchPending && mergeProgressGuard46(*this, activeSlot))
    Serial.println("SAVE GUARD: prevented critical progress rollback");
  prefs.putBool("init", true);
  prefs.putUChar("full", fullness);
  prefs.putUChar("joy", joy);
  prefs.putUChar("ene", energy);
  prefs.putUChar("hyg", hygiene);
  prefs.putUChar("poop", poops);
  prefs.putUChar("wgt", weight);
  prefs.putUChar("ivat", ivAtk);
  prefs.putUChar("ivdf", ivDef);
  prefs.putUChar("ivsp", ivSpe);
  prefs.putUChar("ivhp", ivHp);
  bool criticalOk = true;
  criticalOk &= prefs.putUChar("tatk", trAtk) == 1;
  criticalOk &= prefs.putUChar("tdef", trDef) == 1;
  criticalOk &= prefs.putUChar("tspe", trSpe) == 1;
  criticalOk &= prefs.putUChar("thp", trHp) == 1;
  prefs.putBytes("mvs", moves, sizeof(moves));
  if(currentIsDigimon())prefs.putUChar("digmv",2);
  prefs.putUChar("mvlv", lastLearnLevel);
  prefs.putUChar("avtr", avatar);
  prefs.putUChar("reg", region);
  prefs.putUChar("egsrc", eggSource);
  prefs.putBool("r361", true);
  criticalOk &= prefs.putBytes("badgX", badgesX, sizeof(badgesX)) == sizeof(badgesX);
  criticalOk &= prefs.putBytes("badhX", badgesHardX, sizeof(badgesHardX)) == sizeof(badgesHardX);
  prefs.putBytes("eggR", eggByRegion, sizeof(eggByRegion));
  prefs.putString("tnam", trainerName);
  prefs.putBool("froz", frozen);
  criticalOk &= prefs.putUShort("badg", badges) == 2;
  criticalOk &= prefs.putUShort("badh", badgesHard) == 2;
  prefs.putBool("bk", berryKnown);
  prefs.putBool("shy", shiny);
  prefs.putBool("eshy", eggShiny);
  prefs.putBool("stpk", starterPick);
  prefs.putUChar("evop", evoPen);
  prefs.putUChar("slpa", sleepAuto);
  prefs.putBool("rtpn", retirePending);
  prefs.putBytes("dexsh", dexShinyReg, sizeof(dexShinyReg));
  prefs.putUInt("age", ageMinutes);
  uint32_t lvStore = levelMinutes == LEVEL_MINUTES_UNSET ? ageMinutes : levelMinutes;
  prefs.putUInt("lvmin", lvStore);
  prefs.putUChar("lvslp", sleepLevelRemainder);
  prefs.putShort("dexn", speciesId);
  prefs.putShort("eggT2", eggTarget);
  prefs.putUChar("crack", eggTaps);
  prefs.putUChar("mist", careMistakes);
  prefs.putBool("sleep", sleeping);
  prefs.putUChar("lend", lastEnd);
  if (lastSeenEpoch) prefs.putUInt("seen", lastSeenEpoch);
  prefs.putBytes("dexreg", dexReg, sizeof(dexReg));
  prefs.putBytes("digreg", digiReg, sizeof(digiReg));
  prefs.putBytes("digbest", digiBest, sizeof(digiBest));
  prefs.putUShort("strk", streak);
  prefs.putUShort("bstrk", bestStreak);
  prefs.putUInt("cday", lastCareDay);
  prefs.putUChar("bond", bond);
  prefs.putUShort("medal", medals);
  prefs.putUShort("tmedal", totalMedals);
  prefs.putUShort("mstone", lastMilestone);
  prefs.putUShort("ghi", gameHi);
  prefs.putUShort("shi", strHi);
  prefs.putUShort("qhi", spdHi);
  prefs.putString("nick", nick);

  if (!criticalOk) Serial.println("SAVE ERROR: critical NVS write failed");
  if (!careSwitchPending) {
    if (!writeProgressGuard46(*this, activeSlot))
      Serial.println("SAVE ERROR: progress guard write failed");
  }

  // v3.57.1: once CareSlots is initialised, every complete scalar save also
  // refreshes a CRC-checked A/B snapshot of the active creature. On the next
  // boot this gives us a whole-record recovery point instead of trusting a
  // mixture of individually-written NVS keys after an interrupted save.
  careSlots.checkpointActive(*this, lastSeenEpoch);
}

void Pet::load() {
  fullness = prefs.getUChar("full", 80);
  joy = prefs.getUChar("joy", 80);
  energy = prefs.getUChar("ene", 80);
  hygiene = prefs.getUChar("hyg", 100);
  poops = prefs.getUChar("poop", 0);
  weight = prefs.getUChar("wgt", 0);
  if (prefs.isKey("ivat")) {
    ivAtk = prefs.getUChar("ivat", 16);
    ivDef = prefs.getUChar("ivdf", 16);
    ivSpe = prefs.getUChar("ivsp", 16);
    ivHp = prefs.getUChar("ivhp", 16);
  } else {
    // migracion desde los genes (90-110%) a IV (8-31) conservando la calidad
    // relativa: quien tenia un gen top mantiene un IV top. El IV de vitalidad
    // no existia, se tira ahora.
    ivAtk = ivFromGene(prefs.getUChar("gatk", 0));
    ivDef = ivFromGene(prefs.getUChar("gdef", 0));
    ivSpe = ivFromGene(prefs.getUChar("gspe", 0));
    ivHp = rollIV(0);
  }
  trAtk = prefs.getUChar("tatk", 0);
  trDef = prefs.getUChar("tdef", 0);
  trSpe = prefs.getUChar("tspe", 0);
  trHp = prefs.getUChar("thp", 0);
  // un guardado antiguo puede traer entrenamiento por encima del nuevo tope
  if (trAtk > trMaxAtk()) trAtk = trMaxAtk();
  if (trDef > trMaxDef()) trDef = trMaxDef();
  if (trSpe > trMaxSpe()) trSpe = trMaxSpe();
  if (trHp > trMaxHp()) trHp = trMaxHp();
  berryKnown = prefs.getBool("bk", false);
  shiny = prefs.getBool("shy", false);
  eggShiny = prefs.getBool("eshy", false);
  starterPick = prefs.getBool("stpk", false);
  evoPen = prefs.getUChar("evop", 0);
  // v3.61.8 and older could persist the legacy 144-level retirement debt.
  // That is impossible with MAX_LEVEL=100, so migrate any oversized legacy
  // value to the current bounded debt.
  if (evoPen > EVO_PENALTY_LEVELS) {
    Serial.printf("EVO PENALTY MIGRATION: %u -> %u\n", evoPen, (unsigned)EVO_PENALTY_LEVELS);
    evoPen = EVO_PENALTY_LEVELS;
  }
  sleepAuto = prefs.getUChar("slpa", SLEEP_NONE);
  retirePending = prefs.getBool("rtpn", false);
  loadBlob(prefs, "dexsh", dexShinyReg, sizeof(dexShinyReg));
  ageMinutes = prefs.getUInt("age", 0);
  if (prefs.isKey("lvmin")) {
    levelMinutes = prefs.getUInt("lvmin", 0);
    if (levelMinutes == LEVEL_MINUTES_UNSET)
      levelMinutes = migrateLegacyLevelMinutes(ageMinutes);
  } else {
    // First v3.57.4 boot: preserve the level earned by the old 10-min clock.
    levelMinutes = migrateLegacyLevelMinutes(ageMinutes);
    Serial.printf("LEVEL MIGRATION: age=%lu -> awake=%lu (Lv.%u)\n",
                  (unsigned long)ageMinutes, (unsigned long)levelMinutes, level());
  }
  sleepLevelRemainder = prefs.getUChar("lvslp", 0);
  if (sleepLevelRemainder >= SLEEP_PROGRESS_QUANTUM)
    sleepLevelRemainder %= SLEEP_PROGRESS_QUANTUM;
  if (prefs.isKey("dexn")) {
    speciesId = prefs.getShort("dexn", -1);
    eggTarget = prefs.getShort("eggT2", 4);
  } else {
    // migracion desde la version con indices de flash (0-8)
    static const uint8_t OLD2DEX[9] = { 4, 5, 6, 1, 2, 3, 7, 8, 9 };
    int8_t old = prefs.getChar("spec", -1);
    speciesId = (old >= 0 && old < 9) ? OLD2DEX[old] : -1;
    int8_t oldT = prefs.getChar("eggT", 0);
    eggTarget = (oldT >= 0 && oldT < 9) ? OLD2DEX[oldT] : 4;
  }
  // v3.62.5: catalogs before canonical-form cleanup could persist PMDCollab
  // presentation slots (AltColor/Alternate/Cutscene/Beta) as if they were
  // species. They are retired tombstones now; move any live pet/egg back to
  // the real National-Dex species without touching IVs, level, nickname, etc.
  {
    int16_t oldSpecies = speciesId, oldEgg = eggTarget;
    if (!isDigimonId(speciesId)) speciesId = canonicalizeRetiredVariant(speciesId);
    if (!isDigimonId(eggTarget)) eggTarget = canonicalizeRetiredVariant(eggTarget);
    if (speciesId != oldSpecies)
      Serial.printf("CANONICAL FORM MIGRATION: pet %d -> %d\n", oldSpecies, speciesId);
    if (eggTarget != oldEgg)
      Serial.printf("CANONICAL FORM MIGRATION: egg %d -> %d\n", oldEgg, eggTarget);
  }
  eggTaps = prefs.getUChar("crack", 0);
  careMistakes = prefs.getUChar("mist", 0);
  if (careMistakes > MAX_LEVEL) careMistakes = MAX_LEVEL;
  sleeping = prefs.getBool("sleep", false);
  lastEnd = prefs.getUChar("lend", CER_NONE);
  loadBlob(prefs, "dexreg", dexReg, sizeof(dexReg));
  loadBlob(prefs, "digreg", digiReg, sizeof(digiReg));
  loadBlob(prefs, "digbest", digiBest, sizeof(digiBest));
  streak = prefs.getUShort("strk", 0);
  bestStreak = prefs.getUShort("bstrk", 0);
  lastCareDay = prefs.getUInt("cday", 0);
  bond = prefs.getUChar("bond", 0);
  medals = prefs.getUShort("medal", 0);
  totalMedals = prefs.getUShort("tmedal", 0);
  lastMilestone = prefs.getUShort("mstone", 0);
  gameHi = prefs.getUShort("ghi", 0);
  strHi = prefs.getUShort("shi", 0);
  spdHi = prefs.getUShort("qhi", 0);
  prefs.getString("nick", nick, sizeof(nick));
  // Moves load last: relearnFromLevel() needs speciesId and ageMinutes, both of
  // which are read above. A save from before moves existed has no "mvs" key and
  // leaves the array zeroed, so an established pet is handed the moveset it
  // should already have rather than walking into a battle knowing nothing.
  loadBlob(prefs, "mvs", moves, sizeof(moves));
  for (int i = 0; i < MOVE_SLOTS; i++)
    if (moves[i] >= MOVE_COUNT) moves[i] = 0;   // never index MOVE_TBL with junk
  lastLearnLevel = prefs.getUChar("mvlv", 0);
  frozen = prefs.getBool("froz", false);
  avatar = prefs.getUChar("avtr", 0);
  // Absent on a Kanto-only save, which leaves both arrays zeroed -- exactly
  // "no Johto or Hoenn badges yet".
  loadBlob(prefs, "badgX", badgesX, sizeof(badgesX));
  loadBlob(prefs, "badhX", badgesHardX, sizeof(badgesHardX));
  bool regionV361 = prefs.getBool("r361", false);
  region = prefs.getUChar("reg", REGION_ALL);
  eggSource = prefs.getUChar("egsrc", region);
  if (eggSource >= REGION_COUNT + 11) eggSource = region;
  // In v3.58 value 7 meant ALL; v3.61 inserts Galar/Hisui/Paldea before ALL.
  // Migrate exactly once so an old "ALL" save never silently becomes Galar.
  if (!regionV361 && region == 7) region = REGION_ALL;
  if (region >= REGION_COUNT) region = REGION_ALL;
  if (!regionV361) prefs.putBool("r361", true);
  // A save from the Kanto-only build has neither key; getBytes leaves the
  // array at its zeroed initialiser, which is exactly "nothing remembered".
  loadBlob(prefs, "eggR", eggByRegion, sizeof(eggByRegion));
  if (!regionV361) {
    // v3.58 eggR had 8 slots: Kanto..Alola, ALL.  In v3.61 index 7 is
    // Galar, so preserve the old ALL roll by moving it to the new final slot.
    int16_t oldAll = eggByRegion[7];
    for (uint8_t r = 7; r < REGION_COUNT; r++) eggByRegion[r] = 0;
    eggByRegion[REGION_ALL] = oldAll;
  }
  prefs.getString("tnam", trainerName, sizeof(trainerName));
  if (avatar >= AVATAR_COUNT) avatar = 0;   // a save from when there were four
  badges = prefs.getUShort("badg", 0);
  badgesHard = prefs.getUShort("badh", 0);
  // Repair the legacy Digimon layout (TACKLE plus duplicated digital moves)
  // on the first boot that sees it. Deliberately leave valid customized sets.
  uint8_t digiMoveSchema=prefs.getUChar("digmv",0);
  if (!isEgg() && currentIsDigimon() && (digiMoveSchema<2||digimonMovesNeedRefresh(moves))) {
    relearnFromLevel();
    lastLearnLevel = level();
    prefs.putBytes("mvs", moves, sizeof(moves));
    prefs.putUChar("mvlv", lastLearnLevel);
    prefs.putUChar("digmv",2);
  }
  if (!isEgg() && moveCount() == 0 && lastLearnLevel == 0) {
    // save from before moves existed: hand it the set it should already have
    // rather than a queue of every gate it ever passed
    relearnFromLevel();
    lastLearnLevel = level();
  }
  learnQCount = 0;      // rebuilt from lastLearnLevel by the next tick
  checkLearnGates();
  // siembra: la mascota actual cuenta como criada (guardados antiguos)
  if (speciesId >= 1) registerSpecies(speciesId);
}

// v3.20 live-care slot snapshotting -----------------------------------------
// Only creature-owned state is copied. Pokedex/badges/streak/inventory and the
// trainer profile remain player-wide and therefore stay untouched on a switch.
void Pet::captureCareSnapshot(CareSnapshot &o, uint32_t nowEpoch) const {
  o = CareSnapshot();
  o.magic = CARE_SNAPSHOT_MAGIC;
  o.parkedEpoch = nowEpoch ? nowEpoch : lastSeenEpoch;
  o.fullness = fullness; o.joy = joy; o.energy = energy; o.hygiene = hygiene;
  o.poops = poops; o.weight = weight;
  o.ivAtk = ivAtk; o.ivDef = ivDef; o.ivSpe = ivSpe; o.ivHp = ivHp;
  o.trAtk = trAtk; o.trDef = trDef; o.trSpe = trSpe; o.trHp = trHp;
  o.berryKnown = berryKnown; o.shiny = shiny; o.sleeping = sleeping; o.frozen = frozen;
  o.ageMinutes = ageMinutes; o.speciesId = speciesId; o.careMistakes = careMistakes; o.bond = bond;
  memcpy(o.nick, nick, sizeof(o.nick));
  o.medals = medals;
  memcpy(o.moves, moves, sizeof(o.moves));
  o.lastLearnLevel = lastLearnLevel;
  o.eggTarget = eggTarget; o.eggShiny = eggShiny; o.eggTaps = eggTaps;
  o.mistakeCooldown = mistakeCooldown; o.evoDeclinedLv = evoDeclinedLv;
  o.farDeclinedAge = farDeclinedAge; o.starterPick = starterPick; o.evoPen = evoPen;
  o.retirePending = retirePending; o.neglectTicks = neglectTicks; o.goodTicks = goodTicks;
  o.bondToday = bondToday; o.sleepAuto = sleepAuto;
  memcpy(o.eggByRegion, eggByRegion, sizeof(o.eggByRegion));
  for (uint8_t r = CARE_LEGACY_REGIONS; r < REGION_COUNT; r++)
    o.eggByRegionExtra[r - CARE_LEGACY_REGIONS] = eggByRegion[r];
  o.regionSchema = 1;
  o.levelMinutes = levelMinutes == LEVEL_MINUTES_UNSET ? levelClockMinutes() : levelMinutes;
  o.sleepLevelRemainder = sleepLevelRemainder;
}

bool Pet::restoreCareSnapshot(const CareSnapshot &o, uint32_t nowEpoch) {
  if (o.magic != CARE_SNAPSHOT_MAGIC) return false;
  if (!(o.speciesId == -1 || isCreatureId(o.speciesId))) return false;
  fullness = o.fullness > 100 ? 100 : o.fullness;
  joy = o.joy > 100 ? 100 : o.joy;
  energy = o.energy > 100 ? 100 : o.energy;
  hygiene = o.hygiene > 100 ? 100 : o.hygiene;
  poops = o.poops; weight = o.weight;
  ivAtk = o.ivAtk; ivDef = o.ivDef; ivSpe = o.ivSpe; ivHp = o.ivHp;
  trAtk = o.trAtk; trDef = o.trDef; trSpe = o.trSpe; trHp = o.trHp;
  berryKnown = o.berryKnown; shiny = o.shiny; sleeping = o.sleeping; frozen = o.frozen;
  ageMinutes = o.ageMinutes;
  levelMinutes = (o.levelMinutes == LEVEL_MINUTES_UNSET)
                   ? migrateLegacyLevelMinutes(o.ageMinutes) : o.levelMinutes;
  sleepLevelRemainder = o.sleepLevelRemainder < SLEEP_PROGRESS_QUANTUM
                          ? o.sleepLevelRemainder : 0;
  speciesId = o.speciesId; prevSpeciesId = -1;
  careMistakes = o.careMistakes > MAX_LEVEL ? MAX_LEVEL : o.careMistakes; bond = o.bond;
  memcpy(nick, o.nick, sizeof(nick)); nick[sizeof(nick) - 1] = 0;
  medals = o.medals; newMedal = 0;
  memcpy(moves, o.moves, sizeof(moves));
  for (int i = 0; i < MOVE_SLOTS; i++) if (moves[i] >= MOVE_COUNT) moves[i] = 0;
  lastLearnLevel = o.lastLearnLevel;
  eggTarget = o.eggTarget; eggShiny = o.eggShiny; eggTaps = o.eggTaps;
  mistakeCooldown = o.mistakeCooldown; evoDeclinedLv = o.evoDeclinedLv;
  farDeclinedAge = o.farDeclinedAge; starterPick = o.starterPick;
  evoPen = o.evoPen > EVO_PENALTY_LEVELS ? EVO_PENALTY_LEVELS : o.evoPen;
  retirePending = o.retirePending; neglectTicks = o.neglectTicks; goodTicks = o.goodTicks;
  bondToday = o.bondToday; sleepAuto = o.sleepAuto;
  memset(eggByRegion, 0, sizeof(eggByRegion));
  if (o.regionSchema == 0) {
    // Snapshot written by v3.58: its eighth legacy slot was ALL, not Galar.
    for (uint8_t r = 0; r < 7; r++) eggByRegion[r] = o.eggByRegion[r];
    eggByRegion[REGION_ALL] = o.eggByRegion[7];
  } else {
    memcpy(eggByRegion, o.eggByRegion, sizeof(o.eggByRegion));
    for (uint8_t r = CARE_LEGACY_REGIONS; r < REGION_COUNT; r++)
      eggByRegion[r] = o.eggByRegionExtra[r - CARE_LEGACY_REGIONS];
  }

  // Transient UI/ending state never crosses a tab switch.
  ceremony = CER_NONE; endedKind = CER_NONE; endedMon = PartyMon();
  eatUntil = heartUntil = evolveUntil = 0;
  ceremonyUntil = medalUntil = milestoneUntil = 0;
  ticksSinceSave = 0; pendingSave = false;
  learnQCount = 0;
  lastTick = millis();

  // Inactive slots still experience real elapsed time. We deliberately use the
  // same forgiving floors as power-off progression: they can be hungry/tired
  // when revisited, but a slot left alone never runs away off-screen.
  uint32_t mins = 0;
  if (nowEpoch && o.parkedEpoch && nowEpoch > o.parkedEpoch)
    mins = (nowEpoch - o.parkedEpoch) / 60UL;
  if (mins > 14UL * 24UL * 60UL) mins = 14UL * 24UL * 60UL;
  uint32_t awakeMins = 0;
  for (uint32_t i = 0; i < mins; i++) {
    if (!frozen) ageMinutes++;
    if (isEgg()) {
      if (ageMinutes >= 3 && !starterPick) hatch();
      continue;
    }
    if (sleeping) {
      energy = clamp100(energy + ENERGY_SLEEP_RECOVERY);
      if (ageMinutes % 6 == 0) {
        fullness = dropTo(fullness, 1, 30);
        joy = dropTo(joy, 1, 35);
      }
      if (ageMinutes % 8 == 0) hygiene = dropTo(hygiene, 1, 45);
      if (!frozen && ++sleepLevelRemainder >= SLEEP_PROGRESS_QUANTUM) {
        sleepLevelRemainder = 0;
        levelMinutes++;
      }
      continue;
    }
    if (!frozen) levelMinutes++;
    awakeMins++;
    if (ageMinutes % 2 == 0) fullness = dropTo(fullness, 1, 15);
    if (ageMinutes % ENERGY_AWAKE_DECAY_MINUTES == 0) {
      energy = dropTo(energy, 1, 20);
    }
    if (ageMinutes % 3 == 0) {
      hygiene = dropTo(hygiene, 1, 15);
      joy = dropTo(joy, 1, 15);
    }
    if (weight > 50 && ageMinutes % ENERGY_AWAKE_DECAY_MINUTES == 0) energy = dropTo(energy, 1, 20);
    if (weight > 0 && ageMinutes % 3 == 0) weight--;
  }
  if (!isEgg() && awakeMins) {
    uint8_t p = poops + awakeMins / 240UL;
    poops = p > 3 ? 3 : p;
  }
  if (nowEpoch) lastSeenEpoch = nowEpoch;
  if (!isEgg()) {
    checkMedals();
    checkLearnGates();
  }
  save();
  return true;
}
