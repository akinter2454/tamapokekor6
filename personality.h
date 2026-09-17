#pragma once
#include <stdint.h>

// Korean-only personality layer. It is derived from the creature's IV fingerprint
// rather than stored, so the same individual keeps its personality through
// evolution and party/box migration without changing the PartyMon save stride.
enum PersonalityId : uint8_t {
  PERS_BRAVE = 0, PERS_CALM, PERS_QUICK, PERS_TOUGH, PERS_CHEERFUL, PERS_LUCKY,
  PERS_COUNT
};
enum PersonalityStat : uint8_t { PST_ATK = 0, PST_DEF, PST_SPE, PST_HP, PST_SPA, PST_SPD };

static inline uint8_t personalityIdFor(int16_t dex, uint8_t a, uint8_t d, uint8_t s, uint8_t h) {
  (void)dex;  // deliberately excluded: evolution must not change personality
  uint32_t v = 0x9E3779B9UL;
  v ^= (uint32_t)a * 0x45D9F3BUL;
  v ^= (uint32_t)d * 0x119DE1F3UL;
  v ^= (uint32_t)s * 0x344B1A4DUL;
  v ^= (uint32_t)h * 0x27D4EB2DUL;
  v ^= v >> 16;
  v *= 0x7FEB352DUL;
  v ^= v >> 15;
  return (uint8_t)(v % PERS_COUNT);
}

static inline const char *personalityNameKo(uint8_t p) {
  static const char *const N[PERS_COUNT] = {
    "용감함", "침착함", "재빠름", "튼튼함", "명랑함", "행운"
  };
  return p < PERS_COUNT ? N[p] : "보통";
}

static inline const char *personalityEffectKo(uint8_t p) {
  static const char *const N[PERS_COUNT] = {
    "공격 +5%", "방어 +5%", "스피드 +5%", "체력 +5%", "공격·방어 +3%", "이벤트 확률 +15%p"
  };
  return p < PERS_COUNT ? N[p] : "";
}

static inline uint16_t personalityApply(uint16_t value, uint8_t p, uint8_t stat) {
  // v3.61.6 casual rules expose no special-attack/special-defence split.
  // Cheerful becomes a small balanced Attack/Defence bonus instead.
  if (p == PERS_CHEERFUL && (stat == PST_ATK || stat == PST_DEF))
    return (uint16_t)(((uint32_t)value * 103UL + 50UL) / 100UL);
  bool boost = (p == PERS_BRAVE && stat == PST_ATK) ||
               (p == PERS_CALM && stat == PST_DEF) ||
               (p == PERS_QUICK && stat == PST_SPE) ||
               (p == PERS_TOUGH && stat == PST_HP);
  if (!boost) return value;
  return (uint16_t)(((uint32_t)value * 105UL + 50UL) / 100UL);
}
