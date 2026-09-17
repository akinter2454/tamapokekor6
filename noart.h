#pragma once
#include "dex.h"
static inline bool speciesHasArt(int16_t d) { return d >= 1 && d <= DEX_COUNT && DEX_ENABLED[d] != 0; }
