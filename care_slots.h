#pragma once
#include <Arduino.h>
#include <Preferences.h>
#include "pet.h"

#define CARE_SLOT_COUNT 3
// v3.57.5: version 5 appends the sleeping-growth remainder. Older snapshots
// default that tail byte to zero, so their visible level remains unchanged.
#define CARE_SLOT_VERSION 6

// Three simultaneously-aging raising slots. Only one is rendered/interactive
// at a time; the other two are parked snapshots whose elapsed time is applied
// when the player taps their tab again.
class CareSlots {
public:
  void begin(Pet &pet, uint32_t nowEpoch);
  bool switchTo(uint8_t slot, Pet &pet, uint32_t nowEpoch);
  uint8_t active() const { return _active; }
  bool has(uint8_t slot) const { return slot < CARE_SLOT_COUNT && _valid[slot]; }
  const CareSnapshot *snapshot(uint8_t slot) const {
    return has(slot) ? &_slots[slot] : nullptr;
  }

  // Called after an ordinary Pet save. Once the slot manager is ready, every
  // successful creature save also gets a CRC-checked A/B snapshot. This turns
  // the slot snapshot into a recovery point instead of something refreshed
  // only when the user changes tabs.
  void checkpointActive(const Pet &pet, uint32_t nowEpoch);
  bool ready() const { return _ready; }

private:
  Preferences prefs;
  CareSnapshot _slots[CARE_SLOT_COUNT];
  bool _valid[CARE_SLOT_COUNT] = {false, false, false};
  bool _safeLoaded[CARE_SLOT_COUNT] = {false, false, false};
  uint8_t _active = 0;
  bool _ready = false;

  const char *keyFor(uint8_t slot) const;
  const char *safeKeyFor(uint8_t slot, bool b) const;
  bool writeSlot(uint8_t slot);
  bool readSlot(uint8_t slot);
  bool readSafeSlot(uint8_t slot, CareSnapshot &out, uint32_t &seq);
  bool validateSnapshot(CareSnapshot &s) const;
  void clearSwitchTxn();
};

extern CareSlots careSlots;
