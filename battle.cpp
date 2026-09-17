#include "battle.h"
#include "dex.h"
#include "types.h"
#include "digimon.h"

static bool creatureHasStab(int16_t dex,uint8_t t){return creatureType1(dex)==t||creatureType2(dex)==t;}
static uint16_t creatureTypeEff(uint8_t atk,int16_t dex){return typeEffPct(atk,creatureType1(dex),creatureType2(dex));}

// ---------- building a combatant ----------

static void fill(Combatant &c, int16_t dex, uint8_t lvl, uint16_t hp,
                 uint16_t a, uint16_t d, uint16_t sa, uint16_t sd, uint16_t sp) {
  (void)sa; (void)sd; // v3.61.6 casual rules: no physical/special stat split
  c = Combatant();
  c.dex = dex;
  c.level = lvl;
  c.maxHp = hp ? hp : 1;
  c.hp = c.maxHp;
  c.base[SI_ATK] = a; c.base[SI_DEF] = d;
  c.base[SI_SPA] = a; c.base[SI_SPD] = d; c.base[SI_SPE] = sp;
}

void combatantFromPet(Combatant &c, const Pet &p) {
  fill(c, p.speciesId, p.level(), p.vitStat(), p.atkStat(), p.defStat(),
       p.spaStat(), p.spdStat(), p.speStat());
  for (int i = 0; i < MOVE_SLOTS; i++) c.moves[i] = p.moves[i];
  c.shiny = p.shiny;
  const char *nm = p.nick[0] ? p.nick : creatureName(p.speciesId);
  snprintf(c.name, sizeof(c.name), "%s", nm);
}

void combatantFromParty(Combatant &c, const PartyMon &m) {
  fill(c, m.dex, (uint8_t)m.level, party.vitOf(m), party.atkOf(m), party.defOf(m),
       party.spaOf(m), party.spdOf(m), party.speOf(m));
  for (int i = 0; i < MOVE_SLOTS; i++) c.moves[i] = m.moves[i];
  c.shiny = m.shiny != 0;
  const char *nm = m.nick[0] ? m.nick : creatureName(m.dex);
  snprintf(c.name, sizeof(c.name), "%s", nm);
}

// ---------- stat stages ----------

// The series' own table, as a fraction so it stays integer: +1 is 3/2, -1 is
// 2/3, and so on out to +6 = 4x and -6 = 1/4.
uint16_t stagedStat(uint16_t base, int8_t stage) {
  if (stage > 6) stage = 6;
  if (stage < -6) stage = -6;
  uint16_t num = 2 + (stage > 0 ? stage : 0);
  uint16_t den = 2 + (stage < 0 ? -stage : 0);
  uint32_t v = (uint32_t)base * num / den;
  return v < 1 ? 1 : (v > 65535 ? 65535 : (uint16_t)v);
}

static uint16_t effStat(const Combatant &c, uint8_t idx) {
  uint16_t v = stagedStat(c.base[idx], c.stage[idx]);
  // burn halves physical attack, paralysis halves speed -- the two ailments
  // that do something beyond chip damage
  if (idx == SI_ATK && c.ailment == AIL_BURN) v = v / 2 ? v / 2 : 1;
  if (idx == SI_SPE && c.ailment == AIL_PARA) v = v / 2 ? v / 2 : 1;
  return v;
}

// ---------- damage ----------

// roll is 217..255, the series' damage spread, passed in so tests can pin it.
uint16_t battleDamage(const Combatant &atk, const Combatant &def, uint8_t mv,
                      bool crit, uint8_t roll) {
  if (!mv || mv >= MOVE_COUNT) return 0;
  const MoveEntry &m = MOVE_TBL[mv];
  if (m.cat == MC_STATUS) return 0;

  if (m.effect == EF_FIXED_LVL) return atk.level ? atk.level : 1;
  if (m.effect == EF_FIXED) return m.param > 0 ? (uint16_t)m.param : 1;

  uint16_t A = effStat(atk, SI_ATK);
  uint16_t D = effStat(def, SI_DEF);
  // A critical hit ignores the defender's positive stages and the attacker's
  // negative ones, so a Barrier cannot make you immune to a lucky roll.
  if (crit) {
    A = atk.base[SI_ATK];
    D = def.base[SI_DEF];
  }
  if (!D) D = 1;

  uint32_t dmg = (2UL * atk.level / 5 + 2) * m.power * A / D / 50 + 2;
  if (crit) dmg *= 2;
  if (creatureHasStab(atk.dex, m.type)) dmg = dmg * 3 / 2;
  uint16_t eff = creatureTypeEff(m.type, def.dex);
  dmg = dmg * eff / 100;
  if (eff == 0) return 0;               // immune: no chip, no minimum
  dmg = dmg * roll / 255;
  return dmg < 1 ? 1 : (dmg > 65535 ? 65535 : (uint16_t)dmg);
}

// ---------- turn order ----------

bool battleMovesFirst(const Combatant &a, uint8_t ma,
                      const Combatant &b, uint8_t mb) {
  int pa = (ma && ma < MOVE_COUNT && MOVE_TBL[ma].effect == EF_PRIORITY)
               ? MOVE_TBL[ma].param : 0;
  int pb = (mb && mb < MOVE_COUNT && MOVE_TBL[mb].effect == EF_PRIORITY)
               ? MOVE_TBL[mb].param : 0;
  if (pa != pb) return pa > pb;
  uint16_t sa = effStat(a, SI_SPE), sb = effStat(b, SI_SPE);
  if (sa != sb) return sa > sb;
  return random(2) == 0;               // a genuine speed tie is a coin flip
}

// ---------- one action ----------

static void applyStages(Combatant &c, uint8_t mask, int8_t delta) {
  // Physical/special move categories remain metadata for move identity, but
  // stat stages are deliberately collapsed for the casual TamaPoke rules.
  bool touched[SI_COUNT] = { false, false, false, false, false };
  if (mask & (ST_ATK | ST_SPA)) touched[SI_ATK] = true;
  if (mask & (ST_DEF | ST_SPD)) touched[SI_DEF] = true;
  if (mask & ST_SPE) touched[SI_SPE] = true;
  for (int i = 0; i < SI_COUNT; i++) {
    if (!touched[i]) continue;
    int v = c.stage[i] + delta;
    c.stage[i] = v > 6 ? 6 : (v < -6 ? -6 : (int8_t)v);
  }
  // Mirror legacy slots so old UI/AI code can inspect them safely.
  c.stage[SI_SPA] = c.stage[SI_ATK];
  c.stage[SI_SPD] = c.stage[SI_DEF];
}

static void hurt(Combatant &c, uint16_t amount) {
  c.hp = (amount >= c.hp) ? 0 : c.hp - amount;
}

static void heal(Combatant &c, uint16_t amount) {
  uint32_t v = (uint32_t)c.hp + amount;
  c.hp = v > c.maxHp ? c.maxHp : (uint16_t)v;
}

void battleAct(Combatant &atk, Combatant &def, uint8_t mv, TurnLog &log) {
  log = TurnLog();
  log.move = mv;
  if (atk.fainted() || def.fainted()) { log.skipped = true; return; }

  // --- things that can cost the turn before a move is even chosen
  if (atk.recharge) { atk.recharge = false; log.skipped = true; return; }
  if (atk.ailment == AIL_FREEZE) {
    if (random(100) < 20) atk.ailment = AIL_NONE;   // thaws
    else { log.skipped = true; return; }
  }
  if (atk.ailment == AIL_SLEEP) {
    if (atk.ailTurns) atk.ailTurns--;
    if (atk.ailTurns == 0) atk.ailment = AIL_NONE;
    else { log.skipped = true; return; }
  }
  if (atk.ailment == AIL_PARA && random(100) < 25) { log.skipped = true; return; }
  if (atk.confuseTurns) {
    atk.confuseTurns--;
    if (random(100) < 33) {               // hits itself instead
      uint16_t self = (2UL * atk.level / 5 + 2) * 40 *
                          atk.base[SI_ATK] / (atk.base[SI_DEF] ? atk.base[SI_DEF] : 1) / 50 + 2;
      hurt(atk, self);
      log.hurtSelf = true;
      log.damage = self;
      return;
    }
  }

  // a wound-up EF_CHARGE move fires this turn instead of whatever was picked
  if (atk.charging) { mv = atk.charging; atk.charging = 0; }
  else if (mv && mv < MOVE_COUNT && MOVE_TBL[mv].effect == EF_CHARGE) {
    atk.charging = mv;
    log.charged = true;
    return;
  }

  if (!mv || mv >= MOVE_COUNT) { log.skipped = true; return; }
  const MoveEntry &m = MOVE_TBL[mv];
  log.move = mv;

  // --- accuracy. acc 0 means it cannot miss (SWIFT, and every status move)
  if (m.acc && m.effect != EF_NEVER_MISS && random(100) >= m.acc) {
    log.missed = true;
    return;
  }

  if (m.cat == MC_STATUS) {
    if (m.effect == EF_HEAL) {
      heal(atk, (uint32_t)atk.maxHp * (m.param > 0 ? m.param : 50) / 100);
      log.healed = true;
    } else if (m.effect == EF_STAGE) {
      Combatant &t = (m.target == TG_SELF) ? atk : def;
      applyStages(t, m.statMask, m.stages);
      log.stageMask = m.statMask;
      log.stageDelta = m.stages;
    }
    return;
  }

  // --- damage, including multi-hit
  if (m.effect == EF_STEAL_STAGE) {
    bool stole = false;
    const uint8_t coreStats[] = { SI_ATK, SI_DEF, SI_SPE };
    for (uint8_t n = 0; n < sizeof(coreStats); ++n) {
      uint8_t i = coreStats[n];
      if (def.stage[i] > 0) {
        int8_t raised = (int8_t)(atk.stage[i] + def.stage[i]);
        atk.stage[i] = raised > 6 ? 6 : raised;
        def.stage[i] = 0;
        stole = true;
      }
    }
    atk.stage[SI_SPA] = atk.stage[SI_ATK];
    atk.stage[SI_SPD] = atk.stage[SI_DEF];
    def.stage[SI_SPA] = def.stage[SI_ATK];
    def.stage[SI_SPD] = def.stage[SI_DEF];
    log.stoleStages = stole;
  }
  uint8_t hits = (m.effect == EF_MULTI) ? (uint8_t)(2 + random(4)) : 1;
  uint16_t total = 0;
  log.effPct = creatureTypeEff(m.type, def.dex);
  if (log.effPct == 0) { log.immune = true; return; }
  for (uint8_t h = 0; h < hits; h++) {
    bool crit = m.effect == EF_ALWAYS_CRIT ||
                (m.effect == EF_HIGH_CRIT ? random(8) == 0 : random(16) == 0);
    uint16_t d = battleDamage(atk, def, mv, crit, (uint8_t)(217 + random(39)));
    total += d;
    if (crit) log.crit = true;
    hurt(def, d);
    if (def.fainted()) { hits = h + 1; break; }
  }
  log.hits = hits;
  log.damage = total;

  if (m.effect == EF_RECOIL && m.param > 0) hurt(atk, total / m.param ? total / m.param : 1);
  if (m.effect == EF_DRAIN && m.param > 0) {
    uint16_t oldHp = atk.hp;
    heal(atk, total * m.param / 100);
    log.healed = atk.hp > oldHp;
  }
  if (m.effect == EF_RECHARGE) atk.recharge = true;
  if (m.effect == EF_STAGE_HIT && (m.target == TG_SELF || !def.fainted())) {
    Combatant &t = (m.target == TG_SELF) ? atk : def;
    applyStages(t, m.statMask, m.stages);
    log.stageMask = m.statMask;
    log.stageDelta = m.stages;
  }

  // --- secondary ailment. Never overwrites an existing one, and confusion is
  // tracked separately so it can stack with a real status, as in the games.
  if (m.ailment != AIL_NONE && m.ailChance && !def.fainted() &&
      random(100) < m.ailChance) {
    if (m.ailment == AIL_CONFUSE) {
      if (!def.confuseTurns) {
        def.confuseTurns = 2 + random(3);
        log.inflicted = AIL_CONFUSE;
      }
    } else if (def.ailment == AIL_NONE) {
      // a type cannot be given the status it is made of
      bool immune = (m.ailment == AIL_BURN && creatureHasStab(def.dex, T_FIRE)) ||
                    (m.ailment == AIL_FREEZE && creatureHasStab(def.dex, T_ICE)) ||
                    (m.ailment == AIL_POISON && creatureHasStab(def.dex, T_POISON)) ||
                    (m.ailment == AIL_PARA && creatureHasStab(def.dex, T_ELECTRIC));
      if (!immune) {
        def.ailment = m.ailment;
        if (m.ailment == AIL_SLEEP) def.ailTurns = 2 + random(3);
        log.inflicted = m.ailment;
      }
    }
  }
  log.targetFainted = def.fainted();
}

// ---------- end of turn ----------

void battleEndTurn(Combatant &c, TurnLog &log) {
  log = TurnLog();
  if (c.fainted()) return;
  if (c.ailment == AIL_BURN || c.ailment == AIL_POISON) {
    uint16_t chip = c.maxHp / 16;
    if (!chip) chip = 1;
    hurt(c, chip);
    log.damage = chip;
    log.inflicted = c.ailment;
    log.targetFainted = c.fainted();
  }
}

// ---------- move choice ----------

uint8_t aiChooseMove(const Combatant &self, const Combatant &foe, bool smart) {
  uint8_t legal[MOVE_SLOTS], n = 0;
  for (int i = 0; i < MOVE_SLOTS; i++)
    if (self.moves[i] && self.moves[i] < MOVE_COUNT) legal[n++] = self.moves[i];
  if (!n) return 0;
  if (!smart) return legal[random(n)];

  int16_t bestScore = -32768;
  uint8_t best = legal[0];
  for (uint8_t i = 0; i < n; i++) {
    uint8_t mv = legal[i];
    const MoveEntry &m = MOVE_TBL[mv];
    int32_t sc;
    if (m.cat == MC_STATUS) {
      // A status move costs a whole turn, so it has to buy more than chip
      // damage would. Healing is worth it only when actually hurt; a boost is
      // worth it early and worthless once the stage is already stacked.
      if (m.effect == EF_HEAL) {
        int missing = (int)self.maxHp - self.hp;
        sc = (missing * 100 / (self.maxHp ? self.maxHp : 1)) - 30;
      } else if (m.effect == EF_STAGE) {
        int stacked = 0, hit = 0;
        const Combatant &t = (m.target == TG_SELF) ? self : foe;
        if (m.statMask & (ST_ATK | ST_SPA)) { stacked += t.stage[SI_ATK] * (m.stages > 0 ? 1 : -1); hit++; }
        if (m.statMask & (ST_DEF | ST_SPD)) { stacked += t.stage[SI_DEF] * (m.stages > 0 ? 1 : -1); hit++; }
        if (m.statMask & ST_SPE) { stacked += t.stage[SI_SPE] * (m.stages > 0 ? 1 : -1); hit++; }
        if (!hit) hit = 1;
        // diminishing: +2 ATK is strong at stage 0, pointless at +6
        sc = 26 - (stacked * 12 / hit);
        // and never set up when one more hit would finish you
        if (self.hp * 3 < self.maxHp) sc -= 40;
      } else {
        sc = 5;
      }
    } else {
      uint16_t dmg = battleDamage(self, foe, mv, false, 236);  // average roll
      sc = dmg;
      if (dmg >= foe.hp) sc += 1000;              // a kill this turn beats all
      uint8_t acc = m.acc ? m.acc : 100;
      sc = sc * acc / 100;                        // discount what tends to miss
      if (m.effect == EF_RECHARGE) sc -= dmg / 4; // a free turn for the foe
      if (m.effect == EF_RECOIL) sc -= dmg / 6;
      if (m.effect == EF_CHARGE) sc -= dmg / 3;   // a turn spent winding up
      if (m.ailment != AIL_NONE && foe.ailment == AIL_NONE)
        sc += m.ailChance / 4;
    }
    if (sc > bestScore) { bestScore = (int16_t)sc; best = mv; }
  }
  return best;
}
