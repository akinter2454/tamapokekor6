#!/usr/bin/env python3
"""Regression guard for the global casual energy balance."""
from pathlib import Path
import re

root = Path(__file__).resolve().parents[1]
ino = (root / 'TamaPoke.ino').read_text(encoding='utf-8')
h = (root / 'pet.h').read_text(encoding='utf-8')
cpp = (root / 'pet.cpp').read_text(encoding='utf-8')

assert '#define FW_VERSION "3.89.0"' in ino
expected = {
    'ENERGY_SLEEP_RECOVERY': 10,
    'ENERGY_AWAKE_DECAY_MINUTES': 6,
    'ENERGY_DEF_BASE_COST': 3,
    'ENERGY_DEF_SCORE_DIVISOR': 12,
    'ENERGY_DEF_MAX_COST': 9,
    'ENERGY_ATK_COST': 4,
    'ENERGY_SPE_COST': 3,
    'ENERGY_HP_COST': 3,
    'ENERGY_PLAY_COST': 2,
}
for name, value in expected.items():
    assert re.search(rf'^#define {name} {value}$', h, re.M), (name, value)

# Live sleep, power-off catch-up and inactive care-slot catch-up must all use
# the same faster recovery. Awake drain likewise must be shared by all paths.
assert cpp.count('energy = clamp100(energy + ENERGY_SLEEP_RECOVERY);') == 3
assert cpp.count('ageMinutes % ENERGY_AWAKE_DECAY_MINUTES == 0') == 5
assert 'energy = clamp100(energy + 6);' not in cpp

assert 'min<uint16_t>(ENERGY_DEF_MAX_COST, ENERGY_DEF_BASE_COST + score / ENERGY_DEF_SCORE_DIVISOR)' in cpp
assert 'dropTo(energy, ENERGY_ATK_COST, 8)' in cpp
assert 'dropTo(energy, ENERGY_SPE_COST, 8)' in cpp
assert 'dropTo(energy, ENERGY_HP_COST, 8)' in cpp
assert 'clamp100(energy - ENERGY_PLAY_COST)' in cpp
assert 'pet.energy + 70' in (root / 'game_extras.cpp').read_text(encoding='utf-8')

print('casual energy OK: lower activity costs, 10/min sleep recovery, 6-min awake drain, all care slots')
