#include "care_slots.h"
#include "moves.h"
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

CareSlots careSlots;

namespace {
constexpr uint32_t CARE_SAFE_MAGIC = 0x31475343UL; // "CSG1"
constexpr uint16_t CARE_SAFE_FMT = 3;

struct __attribute__((packed)) CareSafeRecord {
  uint32_t magic;
  uint16_t version;
  uint16_t payloadLen;
  uint32_t sequence;
  uint8_t payload[sizeof(CareSnapshot)];
  uint32_t crc;
};

struct __attribute__((packed)) CareSafeHeader {
  uint32_t magic;
  uint16_t version;
  uint16_t payloadLen;
  uint32_t sequence;
};

static uint32_t crc32ish(const uint8_t *p, size_t n) {
  uint32_t h = 2166136261UL;
  for (size_t i = 0; i < n; ++i) { h ^= p[i]; h *= 16777619UL; }
  return h;
}

static bool newerSeq(uint32_t a, uint32_t b) {
  return (int32_t)(a - b) > 0;
}

// Read old and current safe records. The payload is append-only, so copying a
// shorter ancestor into a default-constructed CareSnapshot leaves new tail
// fields (levelMinutes / sleepLevelRemainder) at safe migration defaults.
static bool readSafeBlob(Preferences &prefs, const char *key, CareSnapshot &out, uint32_t &seq) {
  size_t n = prefs.getBytesLength(key);
  if (n < sizeof(CareSafeHeader) + sizeof(uint32_t) || n > sizeof(CareSafeRecord)) return false;
  uint8_t buf[sizeof(CareSafeRecord)] = {0};
  if (prefs.getBytes(key, buf, n) != n) return false;
  CareSafeHeader h{};
  memcpy(&h, buf, sizeof(h));
  if (h.magic != CARE_SAFE_MAGIC || h.version < 1 || h.version > CARE_SAFE_FMT) return false;
  size_t expected = sizeof(CareSafeHeader) + (size_t)h.payloadLen + sizeof(uint32_t);
  if (expected != n || h.payloadLen > sizeof(CareSnapshot) || h.payloadLen < 24) return false;
  uint32_t stored = 0;
  memcpy(&stored, buf + n - sizeof(uint32_t), sizeof(stored));
  if (stored != crc32ish(buf, n - sizeof(uint32_t))) return false;
  CareSnapshot tmp;
  memcpy(&tmp, buf + sizeof(CareSafeHeader), h.payloadLen);
  out = tmp;
  seq = h.sequence;
  return true;
}
}

const char *CareSlots::keyFor(uint8_t slot) const {
  static const char *const K[CARE_SLOT_COUNT] = { "care0", "care1", "care2" };
  return slot < CARE_SLOT_COUNT ? K[slot] : "care0";
}

const char *CareSlots::safeKeyFor(uint8_t slot, bool b) const {
  static const char *const A[CARE_SLOT_COUNT] = { "c0a", "c1a", "c2a" };
  static const char *const B[CARE_SLOT_COUNT] = { "c0b", "c1b", "c2b" };
  return slot < CARE_SLOT_COUNT ? (b ? B[slot] : A[slot]) : (b ? "c0b" : "c0a");
}

bool CareSlots::validateSnapshot(CareSnapshot &s) const {
  if (s.magic != CARE_SNAPSHOT_MAGIC) return false;
  if (s.regionSchema > 1) return false;
  if (!(s.speciesId == -1 || isCreatureId(s.speciesId))) return false;
  if (s.speciesId >= 1 && !isDigimonId(s.speciesId)) s.speciesId = canonicalizeRetiredVariant(s.speciesId);

  if (s.fullness > 100) s.fullness = 100;
  if (s.joy > 100) s.joy = 100;
  if (s.energy > 100) s.energy = 100;
  if (s.hygiene > 100) s.hygiene = 100;
  if (s.poops > 3) s.poops = 3;
  if (s.weight > 100) s.weight = 100;
  if (s.ivAtk > 31) s.ivAtk = 31;
  if (s.ivDef > 31) s.ivDef = 31;
  if (s.ivSpe > 31) s.ivSpe = 31;
  if (s.ivHp > 31) s.ivHp = 31;
  uint8_t hpCap = (uint8_t)(70 + (30 * (uint16_t)s.ivHp) / 31);
  if (s.trHp > hpCap) s.trHp = hpCap;
  s.berryKnown = !!s.berryKnown;
  s.shiny = !!s.shiny;
  s.sleeping = !!s.sleeping;
  s.frozen = !!s.frozen;
  s.eggShiny = !!s.eggShiny;
  s.starterPick = !!s.starterPick;
  s.retirePending = !!s.retirePending;
  s.nick[sizeof(s.nick) - 1] = 0;
  for (uint8_t i = 0; i < MOVE_SLOTS; ++i) if (s.moves[i] >= MOVE_COUNT) s.moves[i] = 0;
  if (!isCreatureId(s.eggTarget)) s.eggTarget = 1;
  else if (!isDigimonId(s.eggTarget)) s.eggTarget = canonicalizeRetiredVariant(s.eggTarget);
  for (uint8_t r = 0; r < CARE_LEGACY_REGIONS; ++r)
    if (s.eggByRegion[r] < 0 || s.eggByRegion[r] > DEX_COUNT) s.eggByRegion[r] = 0;
    else if (s.eggByRegion[r] > 0) s.eggByRegion[r] = canonicalizeRetiredVariant(s.eggByRegion[r]);
  for (uint8_t r = CARE_LEGACY_REGIONS; r < REGION_COUNT; ++r) {
    int16_t &v = s.eggByRegionExtra[r - CARE_LEGACY_REGIONS];
    if (v < 0 || v > DEX_COUNT) v = 0;
    else if (v > 0) v = canonicalizeRetiredVariant(v);
  }
  if (s.sleepLevelRemainder >= SLEEP_PROGRESS_QUANTUM)
    s.sleepLevelRemainder %= SLEEP_PROGRESS_QUANTUM;
  return true;
}

bool CareSlots::readSafeSlot(uint8_t slot, CareSnapshot &out, uint32_t &seq) {
  bool found = false;
  uint32_t bestSeq = 0;
  for (uint8_t which = 0; which < 2; ++which) {
    CareSnapshot tmp;
    uint32_t curSeq = 0;
    if (!readSafeBlob(prefs, safeKeyFor(slot, which != 0), tmp, curSeq)) continue;
    if (!validateSnapshot(tmp)) continue;
    if (!found || newerSeq(curSeq, bestSeq)) {
      out = tmp;
      bestSeq = curSeq;
      found = true;
    }
  }
  if (found) seq = bestSeq;
  return found;
}

bool CareSlots::writeSlot(uint8_t slot) {
  if (slot >= CARE_SLOT_COUNT || !_valid[slot]) return false;

  // A/B safe copy first. If power disappears during the legacy mirror below,
  // the previous or newly-written safe copy still describes a whole snapshot.
  CareSnapshot aSnap, bSnap;
  uint32_t aSeq = 0, bSeq = 0;
  bool haveA = readSafeBlob(prefs, safeKeyFor(slot, false), aSnap, aSeq) && validateSnapshot(aSnap);
  bool haveB = readSafeBlob(prefs, safeKeyFor(slot, true), bSnap, bSeq) && validateSnapshot(bSnap);
  uint32_t seq = 1;
  if (haveA || haveB) {
    uint32_t latest = !haveA ? bSeq : !haveB ? aSeq : (newerSeq(aSeq, bSeq) ? aSeq : bSeq);
    seq = latest + 1;
    if (!seq) seq = 1;
  }
  bool toB = (seq & 1u) != 0;
  CareSafeRecord rec{};
  rec.magic = CARE_SAFE_MAGIC;
  rec.version = CARE_SAFE_FMT;
  rec.payloadLen = sizeof(CareSnapshot);
  rec.sequence = seq;
  memcpy(rec.payload, &_slots[slot], sizeof(CareSnapshot));
  rec.crc = crc32ish((const uint8_t *)&rec, offsetof(CareSafeRecord, crc));
  bool safeOk = prefs.putBytes(safeKeyFor(slot, toB), &rec, sizeof(rec)) == sizeof(rec);

  // Keep the old raw key as a downgrade/export mirror, but it is no longer the
  // authoritative copy once the safe records exist.
  bool legacyOk = prefs.putBytes(keyFor(slot), &_slots[slot], sizeof(CareSnapshot)) == sizeof(CareSnapshot);
  if (!safeOk || !legacyOk)
    Serial.printf("SAVE ERROR: care slot %u write failed (safe=%d legacy=%d)\n", slot, safeOk, legacyOk);
  return safeOk;
}

bool CareSlots::readSlot(uint8_t slot) {
  if (slot >= CARE_SLOT_COUNT) return false;
  _valid[slot] = false;
  _safeLoaded[slot] = false;

  CareSnapshot safe;
  uint32_t seq = 0;
  if (readSafeSlot(slot, safe, seq)) {
    _slots[slot] = safe;
    _valid[slot] = true;
    _safeLoaded[slot] = true;
    return true;
  }

  // Legacy raw snapshot. v3.58/59 appended form fields after the v3.57 layout,
  // so a rollback sees a LONGER blob. Read the whole value then keep only the
  // v3.57 prefix; never let getBytes() reject it just because our buffer is
  // smaller. Shorter append-only ancestors are default-filled at the tail.
  size_t n = prefs.getBytesLength(keyFor(slot));
  if (n < 24) return false;
  CareSnapshot tmp;
  size_t take = n < sizeof(CareSnapshot) ? n : sizeof(CareSnapshot);
  if (n <= sizeof(CareSnapshot)) {
    size_t got = prefs.getBytes(keyFor(slot), &tmp, sizeof(tmp));
    if (got != n) return false;
  } else {
    uint8_t *buf = (uint8_t *)malloc(n);
    if (!buf) return false;
    size_t got = prefs.getBytes(keyFor(slot), buf, n);
    if (got == n) memcpy(&tmp, buf, take);
    free(buf);
    if (got != n) return false;
  }
  if (!validateSnapshot(tmp)) return false;
  _slots[slot] = tmp;
  _valid[slot] = true;
  writeSlot(slot); // one-time migration into the CRC-checked store
  return true;
}

void CareSlots::clearSwitchTxn() {
  prefs.remove("caretx");
  prefs.remove("carefrom");
  prefs.remove("careto");
}

void CareSlots::begin(Pet &pet, uint32_t nowEpoch) {
  prefs.begin("tamapoke", false);
  _ready = true;

  uint8_t ver = prefs.getUChar("carev", 0);
  _active = prefs.getUChar("carea", 0);
  if (_active >= CARE_SLOT_COUNT) _active = 0;
  for (uint8_t i = 0; i < CARE_SLOT_COUNT; i++) readSlot(i);

  // Crash-safe tab switch recovery. The old implementation wrote `carea` BEFORE
  // the new creature's scalar save. A reset in that window made boot copy the
  // old creature into the newly-selected slot, producing the exact "duplicate
  // in the next slot / original disappeared" symptom. While caretx exists we
  // never trust the scalar/current-slot pairing; the parked snapshots decide.
  if (prefs.getBool("caretx", false)) {
    uint8_t from = prefs.getUChar("carefrom", _active);
    uint8_t to = prefs.getUChar("careto", _active);
    if (from >= CARE_SLOT_COUNT) from = _active;
    if (to >= CARE_SLOT_COUNT) to = from;
    uint8_t chosen = _valid[to] ? to : (_valid[from] ? from : _active);
    _active = chosen;
    prefs.putUChar("carea", _active);
    if (_valid[chosen]) {
      pet.restoreCareSnapshot(_slots[chosen], nowEpoch);
    } else {
      pet.captureCareSnapshot(_slots[chosen], nowEpoch);
      _valid[chosen] = true;
      writeSlot(chosen);
      pet.saveNow();
    }
    clearSwitchTxn();
    prefs.putUChar("carev", CARE_SLOT_VERSION);
    Serial.printf("SAVE RECOVERY: care switch transaction -> slot %u\n", (unsigned)(_active + 1));
    return;
  }

  // First boot on the patched firmware: the scalar Pet keys are the freshest
  // source because old versions only refreshed careN when switching tabs. Seed
  // the new safe active checkpoint from them. On subsequent patched boots the
  // CRC snapshot is authoritative and protects against a half-written scalar
  // save; at worst the pet rolls back to the previous complete checkpoint.
  if (ver == CARE_SLOT_VERSION && _safeLoaded[_active] && _valid[_active]) {
    pet.restoreCareSnapshot(_slots[_active], nowEpoch);
  } else {
    pet.captureCareSnapshot(_slots[_active], nowEpoch);
    _valid[_active] = true;
    writeSlot(_active);
  }

  prefs.putUChar("carev", CARE_SLOT_VERSION);
  prefs.putUChar("carea", _active);
}

void CareSlots::checkpointActive(const Pet &pet, uint32_t nowEpoch) {
  if (!_ready || _active >= CARE_SLOT_COUNT) return;
  pet.captureCareSnapshot(_slots[_active], nowEpoch);
  _valid[_active] = true;
  writeSlot(_active);
}

bool CareSlots::switchTo(uint8_t slot, Pet &pet, uint32_t nowEpoch) {
  if (slot >= CARE_SLOT_COUNT || slot == _active) return true;
  if (pet.ceremony != CER_NONE || pet.endedKind != CER_NONE) return false;

  const uint8_t old = _active;

  // 1) Park the old creature completely before declaring any intent to move.
  pet.captureCareSnapshot(_slots[old], nowEpoch);
  _valid[old] = true;
  if (!writeSlot(old)) return false;

  // 2) Journal the switch. Any reset from here until clearSwitchTxn() is
  // recoverable from the two parked snapshots and can never blindly copy the
  // scalar pet into the wrong neighbouring slot.
  if (prefs.putUChar("carefrom", old) != 1 ||
      prefs.putUChar("careto", slot) != 1 ||
      prefs.putBool("caretx", true) != 1) {
    clearSwitchTxn();
    return false;
  }

  _active = slot;
  if (prefs.putUChar("carea", _active) != 1) {
    _active = old;
    clearSwitchTxn();
    return false;
  }

  if (_valid[slot]) {
    if (!pet.restoreCareSnapshot(_slots[slot], nowEpoch)) {
      _active = old;
      prefs.putUChar("carea", _active);
      clearSwitchTxn();
      return false;
    }
  } else {
    // First visit to an empty tab starts a fresh egg. Pet::newEgg() saves, and
    // because _active already points here checkpointActive() creates a safe
    // target snapshot before the transaction is committed.
    pet.newEgg();
  }

  pet.captureCareSnapshot(_slots[_active], nowEpoch);
  _valid[_active] = true;
  if (!writeSlot(_active)) {
    // Leave caretx intact. The next boot will recover to whichever complete
    // snapshot exists instead of guessing and overwriting a neighbour.
    return false;
  }
  pet.saveNow();
  clearSwitchTxn();
  prefs.putUChar("carev", CARE_SLOT_VERSION);
  return true;
}
