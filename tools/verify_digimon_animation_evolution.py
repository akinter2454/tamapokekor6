#!/usr/bin/env python3
from pathlib import Path
import re
r=Path(__file__).resolve().parents[1]
ino=(r/'TamaPoke.ino').read_text(encoding='utf-8')
pet=(r/'pet.cpp').read_text(encoding='utf-8')
checks={
 'animation globals declared before use':ino.find('static uint32_t digiActionUntil = 0;') < ino.find('static void digiReact('),
 'state priority':'PetMood mood=pet.mood();' in ino and 'mood==MOOD_SLEEPING' in ino and 'mood==MOOD_EATING' in ino and 'mood==MOOD_SAD' in ino,
 'sleep 11/12':'frame=11+(now/850)%2' in ino,
 'sad 13/14':'frame=13+(now/650)%2' in ino,
 'eat/touch 0/2':"frame=((now/260)&1)?2:0" in ino and 'DIGI_ANIM_TOUCH' in ino,
 'training 1/3':"frame=((now/240)&1)?3:1" in ino and ino.count('digiReact(DIGI_ANIM_TRAIN)')>=4,
 'joy 1/2':"frame=((now/300)&1)?2:1" in ino,
 'walk faces travel direction':'bool movingRight=p<=55;' in ino and 'flip=movingRight;' in ino and 'drift=movingRight?p:110-p;' in ino and 'frame=0;' in ino,
 'Pokemon ground':'y=PET_GROUND-drawH' in ino,
 'Pokemon evolution FX shared':'drawDigiEvolveFX' in ino and 'if (pet.evolving()) drawDigiEvolveFX(millis());' in ino,
 'old/new DGI forms alternate':'showOld?previous:current' in ino and 'digimonIndex(pet.prevSpeciesId)' in ino,
 'same halo rays sparks':'int halo=36+(int)(t*150)' in ino and 'for(int i=0;i<12;i++)' in ino and 'for(int i=0;i<10;i++)' in ino,
 'DGI ground alignment':'x=centerX-drawW/2,y=groundY-drawH;' in ino,
 'no PMD load for Digimon':'if (wasDigimon)' in ino and 'else evoPmd.load(old, pet.shiny);' in ino,
 'affection idle frame':'drawDigiFrameCentered(digimonIndex(pet.speciesId),0,CX,220,2,false,false);' in ino,
 'Digimon ceremony routing':'if(pet.currentIsDigimon()){drawDigiCeremony();return;}' in ino,
 'farewell hearts and walk':'drawDigiCeremony' in ino and 'frame=((now/300)&1)?2:1;' in ino and 'flip=true;' in ino,
 'decline gate resets':'evoDeclinedLv = 0;' in pet and 'evoDeclinedLv=0; registerSpecies(speciesId);' in pet,
 'no hidden care evolution gate':'digimonEvolutionTarget(i,level(),trAtk,trDef,trSpe,trHp,digiBest)!=i;' in pet,
 'card agrees':'pet.currentIsDigimon() ? pet.canEvolveNow() : pet.lowestStat() >= 40' in ino,
 'fusion levels':'i==14||i==32||i==51||i==66||i==48||i==83)return 55' in (r/'digimon.cpp').read_text(encoding='utf-8'),
}
bad=[k for k,v in checks.items() if not v]
if bad: raise SystemExit('Digimon animation/evolution FAIL: '+', '.join(bad))

cpp=(r/'digimon.cpp').read_text(encoding='utf-8')
rows=[(n,int(v),int(s)) for n,v,s,_,_ in re.findall(r'D\("([^"]+)",(\d+),(\d+),(\d+),(\d+)\)',cpp)]
fusion={'Mastemon','Tlalocmon','Aegisdramon','Mitamamon','Voltobautamon','Cernumon','Omegamon','Chaosdramon','Proximamon'}
assert len(rows)==283
for version in (1,2,3,4,5,10,11,12,13,14,15):
    for stage in range(5):
        sources=[n for n,v,s in rows if v==version and s==stage]
        targets=[n for n,v,s in rows if v==version and s==stage+1 and not (version>=10 and n in fusion)]
        assert sources, f'no source forms for version {version} stage {stage}'
        assert 0 < len(targets) <= 12, f'bad evolution targets for version {version} stage {stage}: {len(targets)}'
print('Digimon animation/evolution OK: 283-form DMC/P0-P5 branches, idle card, Pokemon-linked evolution/farewell FX')
