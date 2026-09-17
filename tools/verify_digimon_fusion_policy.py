#!/usr/bin/env python3
import re
from pathlib import Path
r=Path(__file__).resolve().parents[1]
cpp=(r/'digimon.cpp').read_text(encoding='utf-8')
h=(r/'digimon.h').read_text(encoding='utf-8')
ino=(r/'TamaPoke.ino').read_text(encoding='utf-8')
html=(r/'tools/Digimon-SD-Pack-Maker.html').read_text(encoding='utf-8')
gi_path=r/'.gitignore'
gi=gi_path.read_text(encoding='utf-8') if gi_path.is_file() else ''
rows=re.findall(r'D\("([^"]+)",(\d),(\d),(\d+),(\d)\)',cpp)
assert re.search(r'^#define\s+FW_VERSION\s+"\d+\.\d+\.\d+"',ino,re.M), 'semantic firmware version missing'
assert len(rows)==90, len(rows)
assert [x[0] for x in rows[-4:]]==['Omnimon Alter-S','Chaosmon','Millenniummon','Chaosdramon']
assert all(x[1]=='0' for x in rows[-4:])
assert 'bestLevel[DIGI_SPECIES_CAP]' in h and 'prefs.putBytes("best"' in cpp
count_decl='extern const uint16_t DIGI_SPECIES_COUNT;'
class_decl='class DigiPet {'
assert h.count(count_decl)==1 and h.find(count_decl)<h.find(class_decl), 'DIGI_SPECIES_COUNT must be declared before inline DigiPet methods'
assert 'min(rl,sizeof(registered))' in cpp, 'legacy registration prefix migration missing'
for token in ['bestLevel[32]>=55','bestLevel[14]>=55','bestLevel[66]>=55','bestLevel[51]>=55','bestLevel[83]>=55','bestLevel[48]>=55','training[DIGI_ATK]>=80','training[DIGI_DEF]>=60']:
    assert token in cpp, token
for token in ['DF-001','DF-002','DF-003','DX-001','digiDexRule']:
    assert token in ino, token
for name in ['Omnimon Alter-S','Chaosmon','Millenniummon','Chaosdramon']:
    assert f'{name}|0|5' in html
if gi:
    assert '*.dgi' in gi and 'tools/sdcard/digimon/' in gi and '*_dmc.zip' in gi
for p in r.rglob('*'):
    if not p.is_file() or '.git' in p.parts: continue
    rel=p.relative_to(r).as_posix().lower()
    assert not rel.endswith('.dgi'), f'private DGI leaked: {rel}'
    assert not (rel.endswith('.png') and ('digimon' in rel or 'sprite_private' in rel)), f'private PNG leaked: {rel}'
print('Digimon fusion/Git policy OK: stable 90-species base, Pendulum extension, legacy save migration, no private sprite payload'+('' if gi else ' (.gitignore absent; content scan used)'))
