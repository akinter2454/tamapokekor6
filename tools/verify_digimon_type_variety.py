#!/usr/bin/env python3
"""Static coverage guard for diverse Digimon typings and move pools."""
from pathlib import Path

root=Path(__file__).resolve().parents[1]
cpp=(root/'digimon.cpp').read_text(encoding='utf-8')
type_block=cpp.split('static uint8_t digiThemeType',1)[1].split('static void typeMoves',1)[0]
all_types=(
 'T_NORMAL','T_FIRE','T_WATER','T_ELECTRIC','T_GRASS','T_ICE','T_FIGHTING',
 'T_POISON','T_GROUND','T_FLYING','T_PSYCHIC','T_BUG','T_ROCK','T_GHOST',
 'T_DRAGON','T_DARK','T_STEEL','T_FAIRY')
missing=[t for t in all_types if t not in type_block]
assert not missing, f'Digimon type coverage missing: {missing}'
for token in ('Marin','Dokugu','Gottsu','Blastmon','Bake','Vamde','Ange','Metal','Quantum'):
    assert f'hasWord(n,"{token}")' in type_block, token
assert 'digiThemeType(species())' in type_block
assert 'digiCoverageType(i,t1,t2)' in cpp
assert 'm.type!=t1&&m.type!=t2&&m.type!=t3&&m.type!=T_NORMAL' in cpp
print('Digimon type variety OK: all 18 types, thematic dual types, four role-based coverage pools')
