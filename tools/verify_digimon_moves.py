#!/usr/bin/env python3
"""Regression checks for distinct Pokemon-derived Digimon movesets."""
from pathlib import Path

root=Path(__file__).resolve().parents[1]
cpp=(root/'digimon.cpp').read_text(encoding='utf-8')
pet=(root/'pet.cpp').read_text(encoding='utf-8')
party=(root/'party.cpp').read_text(encoding='utf-8')
ino=(root/'TamaPoke.ino').read_text(encoding='utf-8')
h=(root/'digimon.h').read_text(encoding='utf-8')

checks={
 'Pokemon pool excludes digital moves':'for(uint16_t mv=1;mv<MV_DIGI_PULSE' in cpp,
 'type-compatible pool':'m.type!=t1&&m.type!=t2&&m.type!=t3&&m.type!=T_NORMAL' in cpp,
 'role coverage pool':'digiCoverageType' in cpp and 'static const uint8_t role[4][4]' in cpp,
 'level power cap':'m.power>cap' in cpp,
 'deterministic IV seed':all(x in cpp for x in ('((uint32_t)ia<<24)','((uint32_t)id<<16)','((uint32_t)is<<8)','^ih')),
 'three Pokemon plus one signature':'w<3' in cpp and 'out[3]=digimonSignatureMove(i);' in cpp,
 'duplicate prevention':'if(out[x]==mv)dup=true' in cpp,
 'legacy duplicate detection':'digital>1' in cpp and 'if(m[i]==m[j])return true' in cpp,
 'live save migration':'currentIsDigimon() && (digiMoveSchema<2||digimonMovesNeedRefresh(moves))' in pet,
 'v2 moveset migration':'digiMoveSchema<2' in pet and 'prefs.putUChar("digmv",2)' in pet,
 'party and box migration':'digimonMovesNeedRefresh(m.moves)' in party,
 'v2 party box migration':'prefs.getUChar("digbmv",0)<2' in party and 'prefs.putUChar("digbmv",2)' in party,
 'evolution refresh':'digimonDefaultMoves(next,level(),ivAtk,ivDef,ivSpe,ivHp,moves);' in pet,
 'move picker support':'if (isDigimonId(dex)) return digimonLearnableMoves' in ino,
 'old duplicate seeding removed':'moves[0]=MV_TACKLE; moves[1]=(uint8_t)(MV_DIGI_PULSE' not in pet,
 'public API':all(x in h for x in ('digimonLearnableMoves','digimonDefaultMoves','digimonMovesNeedRefresh')),
}
bad=[name for name,ok in checks.items() if not ok]
if bad: raise SystemExit('Digimon move regression FAIL: '+', '.join(bad))
print('Digimon moves OK: distinct Pokemon pool x3 + one signature, picker/evolution/legacy migration')
