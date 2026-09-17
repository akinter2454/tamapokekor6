#include "party.h"
#include <stdlib.h>
#include <string.h>
#include <stddef.h>
#include "dex.h"
#include "moves.h"
#include "personality.h"
#include "digimon.h"

Party party;

namespace {
constexpr uint32_t BANK_MAGIC = 0x314B4254UL; // "TBK1"
constexpr uint16_t BANK_VERSION = 1;

struct __attribute__((packed)) SafeBankRecord {
  uint32_t magic;
  uint16_t version;
  uint16_t payloadLen;
  uint32_t sequence;
  uint8_t payload[sizeof(PartyMon) * (PARTY_SLOTS + BOX_SLOTS)];
  uint32_t crc;
};

static uint32_t bankCrc(const uint8_t *p, size_t n) {
  uint32_t h = 2166136261UL;
  for (size_t i = 0; i < n; ++i) { h ^= p[i]; h *= 16777619UL; }
  return h;
}
static bool seqNewer(uint32_t a, uint32_t b) { return (int32_t)(a - b) > 0; }

static void sanitizeMon(PartyMon &m) {
  if (!isCreatureId(m.dex)) { m = PartyMon(); return; }
  if (!isDigimonId(m.dex)) m.dex = canonicalizeRetiredVariant(m.dex);
  if (m.level < 1) m.level = 1;
  if (m.level > 100) m.level = 100;
  if (m.ivAtk > 31) m.ivAtk = 31;
  if (m.ivDef > 31) m.ivDef = 31;
  if (m.ivSpe > 31) m.ivSpe = 31;
  if (m.ivHp > 31) m.ivHp = 31;
  uint8_t hpCap = (uint8_t)(70 + (30 * (uint16_t)m.ivHp) / 31);
  if (m.trHp > hpCap) m.trHp = hpCap;
  m.shiny = !!m.shiny;
  m.nick[sizeof(m.nick)-1] = 0;
  for (uint8_t i=0;i<MOVE_SLOTS;i++) if (m.moves[i] >= MOVE_COUNT) m.moves[i]=0;
  if (isDigimonId(m.dex) && digimonMovesNeedRefresh(m.moves))
    digimonDefaultMoves(digimonIndex(m.dex),(uint8_t)m.level,m.ivAtk,m.ivDef,m.ivSpe,m.ivHp,m.moves);
}

static bool decodeBank(Preferences &prefs, const char *key,
                       PartyMon *partyOut, PartyMon *boxOut, uint32_t &seqOut) {
  if (prefs.getBytesLength(key) != sizeof(SafeBankRecord)) return false;
  SafeBankRecord rec{};
  if (prefs.getBytes(key, &rec, sizeof(rec)) != sizeof(rec)) return false;
  if (rec.magic != BANK_MAGIC || rec.version != BANK_VERSION ||
      rec.payloadLen != sizeof(rec.payload) ||
      rec.crc != bankCrc((const uint8_t *)&rec, offsetof(SafeBankRecord, crc))) return false;
  size_t po = sizeof(PartyMon) * PARTY_SLOTS;
  memcpy(partyOut, rec.payload, po);
  memcpy(boxOut, rec.payload + po, sizeof(PartyMon) * BOX_SLOTS);
  for (uint8_t i=0;i<PARTY_SLOTS;i++) sanitizeMon(partyOut[i]);
  for (uint8_t i=0;i<BOX_SLOTS;i++) sanitizeMon(boxOut[i]);
  seqOut = rec.sequence;
  return true;
}

static bool loadLegacyArray(Preferences &prefs, const char *key,
                            PartyMon *dst, size_t slots, bool rollbackFormAware) {
  for (size_t i=0;i<slots;i++) dst[i] = PartyMon();
  size_t stored = prefs.getBytesLength(key);
  if (!stored) return true;
  const size_t curStride = sizeof(PartyMon);
  const size_t curBytes = curStride * slots;

  if (stored == curBytes) {
    if (prefs.getBytes(key, dst, curBytes) != curBytes) return false;
  } else if (stored > curBytes && stored % curStride == 0 && stored % slots != 0) {
    // Same record format with more slots. Keep the first slots records. The
    // exact-slot-count case below has priority when both interpretations are
    // mathematically possible, because that is how v3.58/59 changed stride.
    uint8_t *tmp=(uint8_t*)malloc(stored);
    if(!tmp) return false;
    size_t got=prefs.getBytes(key,tmp,stored);
    if(got==stored) memcpy(dst,tmp,curBytes);
    free(tmp);
    if(got!=stored) return false;
  } else if (stored % slots == 0) {
    size_t stride = stored / slots;
    // Older append-only records, or v3.58/59's append-only form byte. v3.57's
    // PartyMon is 30 bytes on ESP32 and v3.58/59 is 32 due to alignment. Copy
    // each record prefix independently; NEVER memcpy a whole larger blob, which
    // would shift slot 1 onward and invent/duplicate Pokemon.
    bool knownFuture = rollbackFormAware && stride > curStride && stride <= curStride + 4;
    bool knownPast = stride < curStride && stride >= 20;
    if (!knownFuture && !knownPast) return false;
    uint8_t *tmp = (uint8_t *)malloc(stored);
    if (!tmp) return false;
    size_t got = prefs.getBytes(key, tmp, stored);
    if (got == stored) {
      size_t take = stride < curStride ? stride : curStride;
      for (size_t i=0;i<slots;i++) memcpy(&dst[i], tmp + i*stride, take);
    }
    free(tmp);
    if (got != stored) return false;
  } else if (stored > curBytes && stored % curStride == 0) {
    // Same stride, more slots, and an ambiguous divisibility case. This branch
    // is only safe when no known future-stride marker is present.
    if (rollbackFormAware) return false;
    uint8_t *tmp=(uint8_t*)malloc(stored);
    if(!tmp) return false;
    size_t got=prefs.getBytes(key,tmp,stored);
    if(got==stored) memcpy(dst,tmp,curBytes);
    free(tmp);
    if(got!=stored) return false;
  } else {
    return false;
  }
  for (size_t i=0;i<slots;i++) sanitizeMon(dst[i]);
  return true;
}
}

// Same NVS namespace as the pet. v3.57.1 treats party+box as ONE logical bank:
// a party<->box swap must never be half old and half new after a power loss.
void Party::begin() {
  for (auto &s : slots) s = PartyMon();
  for (auto &s : box) s = PartyMon();
  prefs.begin("tamapoke", false);

  PartyMon pa[PARTY_SLOTS], pb[PARTY_SLOTS];
  PartyMon ba[BOX_SLOTS], bb[BOX_SLOTS];
  uint32_t sa=0, sb=0;
  bool va=decodeBank(prefs,"bankA",pa,ba,sa);
  bool vb=decodeBank(prefs,"bankB",pb,bb,sb);
  if (va || vb) {
    bool useB = vb && (!va || seqNewer(sb,sa));
    memcpy(slots, useB ? pb : pa, sizeof(slots));
    memcpy(box,   useB ? bb : ba, sizeof(box));
    if(prefs.getUChar("digbmv",0)<2){
      for(auto &m:slots)if(isDigimonId(m.dex))digimonDefaultMoves(digimonIndex(m.dex),(uint8_t)m.level,m.ivAtk,m.ivDef,m.ivSpe,m.ivHp,m.moves);
      for(auto &m:box)if(isDigimonId(m.dex))digimonDefaultMoves(digimonIndex(m.dex),(uint8_t)m.level,m.ivAtk,m.ivDef,m.ivSpe,m.ivHp,m.moves);
      save();prefs.putUChar("digbmv",2);
    }
    return;
  }

  // No safe bank yet: migrate the old raw keys. `form` is a reliable marker
  // that v3.58/59 may have written the larger append-only PartyMon stride.
  bool rollbackFormAware = prefs.isKey("form") || prefs.getUChar("carev",0)==2;
  bool partyOk=loadLegacyArray(prefs,"party",slots,PARTY_SLOTS,rollbackFormAware);
  bool boxOk=loadLegacyArray(prefs,"box",box,BOX_SLOTS,rollbackFormAware);
  if (!partyOk) {
    Serial.println("SAVE RECOVERY: unreadable legacy party; kept empty rather than misalign records");
    for(auto &s:slots) s=PartyMon();
  }
  if (!boxOk) {
    Serial.println("SAVE RECOVERY: unreadable legacy box; kept empty rather than misalign records");
    for(auto &s:box) s=PartyMon();
  }
  if(prefs.getUChar("digbmv",0)<2){
    for(auto &m:slots)if(isDigimonId(m.dex))digimonDefaultMoves(digimonIndex(m.dex),(uint8_t)m.level,m.ivAtk,m.ivDef,m.ivSpe,m.ivHp,m.moves);
    for(auto &m:box)if(isDigimonId(m.dex))digimonDefaultMoves(digimonIndex(m.dex),(uint8_t)m.level,m.ivAtk,m.ivDef,m.ivSpe,m.ivHp,m.moves);
  }
  save(); // seed CRC A/B bank and rewrite legacy v3.57 layout
  prefs.putUChar("digbmv",2);
}

void Party::save() {
  SafeBankRecord a{}, b{};
  uint32_t sa=0,sb=0;
  PartyMon dummyP[PARTY_SLOTS], dummyB[BOX_SLOTS];
  bool va=decodeBank(prefs,"bankA",dummyP,dummyB,sa);
  bool vb=decodeBank(prefs,"bankB",dummyP,dummyB,sb);
  uint32_t latest = !va ? (vb?sb:0) : !vb ? sa : (seqNewer(sa,sb)?sa:sb);
  uint32_t next=latest+1; if(!next) next=1;

  SafeBankRecord rec{};
  rec.magic=BANK_MAGIC; rec.version=BANK_VERSION; rec.payloadLen=sizeof(rec.payload); rec.sequence=next;
  size_t po=sizeof(slots);
  memcpy(rec.payload,slots,po);
  memcpy(rec.payload+po,box,sizeof(box));
  rec.crc=bankCrc((const uint8_t*)&rec,offsetof(SafeBankRecord,crc));
  const char *dst=(next&1u)?"bankB":"bankA";
  bool safeOk=prefs.putBytes(dst,&rec,sizeof(rec))==sizeof(rec);

  // Legacy mirrors are written only after the atomic logical bank. If a reset
  // interrupts either mirror, next boot still chooses the valid A/B bank.
  bool partyOk=prefs.putBytes("party",slots,sizeof(slots))==sizeof(slots);
  bool boxOk=prefs.putBytes("box",box,sizeof(box))==sizeof(box);
  if(!safeOk||!partyOk||!boxOk)
    Serial.printf("SAVE ERROR: party bank write failed (safe=%d party=%d box=%d)\n",safeOk,partyOk,boxOk);
}

void Party::boxSave() { save(); }

uint8_t Party::boxCount() const {
  uint8_t n=0; for(auto &s:box) if(!s.empty()) n++; return n;
}
int Party::boxFirstFree() const {
  for(int i=0;i<BOX_SLOTS;i++) if(box[i].empty()) return i; return -1;
}
bool Party::boxAdd(const PartyMon &m) {
  int i=boxFirstFree(); if(i<0) return false; box[i]=m; save(); return true;
}
void Party::boxReleaseAt(uint8_t i) {
  if(i>=BOX_SLOTS) return; box[i]=PartyMon(); save();
}
void Party::swapPartyBox(uint8_t partyIdx,uint8_t boxIdx) {
  if(partyIdx>=PARTY_SLOTS||boxIdx>=BOX_SLOTS) return;
  PartyMon t=slots[partyIdx]; slots[partyIdx]=box[boxIdx]; box[boxIdx]=t; save();
}
uint8_t Party::count() const {
  uint8_t n=0; for(auto &s:slots) if(!s.empty()) n++; return n;
}
int Party::firstFree() const {
  for(int i=0;i<PARTY_SLOTS;i++) if(slots[i].empty()) return i; return -1;
}
bool Party::add(const PartyMon &m) {
  int i=firstFree(); if(i<0) return false; slots[i]=m; save(); return true;
}
void Party::replaceAt(uint8_t i,const PartyMon &m) {
  if(i>=PARTY_SLOTS) return; slots[i]=m; save();
}
void Party::releaseAt(uint8_t i) {
  if(i>=PARTY_SLOTS) return; slots[i]=PartyMon(); save();
}

// Mirrors calcStat() in pet.cpp: base + level + IV contribution + training.
// Kept in step with it by hand; there is no shared home for it that both the
// live pet and a frozen party member could use without dragging Pet in here.
static uint16_t calcStat(uint8_t base, uint8_t iv, uint16_t lvl, uint8_t tr) {
  return (uint16_t)base + lvl + (uint32_t)iv * lvl / 100 + tr;
}

static uint8_t persOf(const PartyMon &m) {
  return personalityIdFor(m.dex, m.ivAtk, m.ivDef, m.ivSpe, m.ivHp);
}
uint16_t Party::atkOf(const PartyMon &m) const {
  return m.empty() ? 0 : personalityApply(calcStat(creatureBaseAtk(m.dex), m.ivAtk, m.level, m.trAtk), persOf(m), PST_ATK);
}
uint16_t Party::defOf(const PartyMon &m) const {
  return m.empty() ? 0 : personalityApply(calcStat(creatureBaseDef(m.dex), m.ivDef, m.level, m.trDef), persOf(m), PST_DEF);
}
uint16_t Party::speOf(const PartyMon &m) const {
  return m.empty() ? 0 : personalityApply(calcStat(creatureBaseSpe(m.dex), m.ivSpe, m.level, m.trSpe), persOf(m), PST_SPE);
}
uint16_t Party::vitOf(const PartyMon &m) const {
  return m.empty() ? 0 : personalityApply(calcStat(creatureBaseHp(m.dex), m.ivHp, m.level, (uint8_t)(10 + m.trHp)), persOf(m), PST_HP);
}
uint16_t Party::spaOf(const PartyMon &m) const {
  return m.empty() ? 0 : personalityApply(calcStat(creatureBaseSpA(m.dex), m.ivAtk, m.level, m.trAtk), persOf(m), PST_SPA);
}
uint16_t Party::spdOf(const PartyMon &m) const {
  return m.empty() ? 0 : personalityApply(calcStat(creatureBaseSpD(m.dex), m.ivDef, m.level, m.trDef), persOf(m), PST_SPD);
}
