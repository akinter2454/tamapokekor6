#!/usr/bin/env python3
from pathlib import Path
import re, sys

root = Path(__file__).resolve().parents[1]
moves = (root / 'moves.h').read_text(encoding='utf-8')
pet_h = (root / 'pet.h').read_text(encoding='utf-8')
pet_cpp = (root / 'pet.cpp').read_text(encoding='utf-8')
ino = (root / 'TamaPoke.ino').read_text(encoding='utf-8')
errs=[]

def need(cond,msg):
    if not cond: errs.append(msg)

need('v3.63.0 learnset expansion' in moves, 'moves: v3.63.0 supplemental layer marker missing')
need('legacyNaturalLearnCount' in moves, 'moves: legacy natural-count helper missing')
need('legacySupplementTarget' in moves, 'moves: sparse legacy supplement policy missing')
need('post809SupplementEntry' in moves, 'moves: post-809 level progression missing')
need('if (total <= 2 || natural >= 7) return 0;' in moves,
     'moves: intentional tiny learnset preservation guard missing')
need('if (dex < 1 || dex > DEX_COUNT) return MV_STRUGGLE;' in moves,
     'moves: learnMove bounds guard missing')
need('if (dex < 1 || dex > DEX_COUNT) return 0;' in moves,
     'moves: learnLevel/count bounds guard missing')
need('learnQueue[12]' in pet_h, 'pet: learn queue was not expanded to 12')
need('if (at == 0 || at > lvl) continue;' in pet_cpp,
     'pet: pending learnables still assumes sorted future gates / includes TM entries')
need('if (mv >= MOVE_COUNT) mv = 0;' in ino,
     'ui: move-row bounds guard missing')
need('lg.move && lg.move < MOVE_COUNT' in ino,
     'battle ui: battle SFX move bounds guard missing')
need('bool validMove = mv && mv < MOVE_COUNT;' in ino,
     'battle ui: battle move-grid bounds guard missing')
need('uint8_t known = pet.moves[i] < MOVE_COUNT ? pet.moves[i] : 0;' in ino,
     'tm ui: known-move bounds guard missing')

# Parse the frozen legacy 1..809 table and verify that the supplemental policy
# materially reduces ordinary 3-4 move learnsets while preserving tiny gimmick pools.
tm = re.search(r'static const LearnEntry LEARN_TBL\[\d+\] = \{(.*?)\n\};', moves, re.S)
om = re.search(r'static const uint16_t LEARN_OFS\[DEX_COUNT \+ 2\] = \{(.*?)\n\};', moves, re.S)
need(bool(tm and om), 'moves: could not parse LEARN_TBL/LEARN_OFS')
if tm and om:
    entries=[(int(a),int(b)) for a,b in re.findall(r'\{\s*(\d+)\s*,\s*(\d+)\s*\}',tm.group(1))]
    ofs=[int(x) for x in re.findall(r'\d+',om.group(1))]
    need(len(ofs) >= 811, f'moves: legacy offset table too short ({len(ofs)})')
    if len(ofs) >= 811:
        sparse_before=0; sparse_after=0; seven_after=0; preserved_tiny=0
        for dex in range(1,810):
            arr=entries[ofs[dex]:ofs[dex+1]]
            natural=[mv for mv,lv in arr if lv>0]
            natset=set(natural)
            unique_tm=[]
            for mv,lv in arr:
                if lv==0 and mv not in natset and mv not in unique_tm:
                    unique_tm.append(mv)
            n=len(natural)
            if n <= 4: sparse_before += 1
            if len(arr) <= 2 or n >= 7:
                added=0
                if len(arr) <= 2 and n <= 4: preserved_tiny += 1
            else:
                added=min(7-n, len(unique_tm))
            a=n+added
            if a <= 4: sparse_after += 1
            if a >= 7: seven_after += 1
        need(sparse_before >= 250, f'moves: baseline sparse count unexpectedly low ({sparse_before})')
        need(sparse_after <= 40, f'moves: sparse learnsets remain too common after policy ({sparse_after})')
        need(seven_after >= 730, f'moves: too few legacy species reach 7 natural moves ({seven_after})')
        need(preserved_tiny >= 8, f'moves: tiny intentional pools do not appear preserved ({preserved_tiny})')
        print(f'legacy learnset audit: <=4 {sparse_before} -> {sparse_after}; >=7 after={seven_after}; tiny preserved={preserved_tiny}')

if errs:
    print('learnset/stability audit FAILED')
    for e in errs: print(' -', e)
    sys.exit(1)
print('learnset/stability audit OK')
print(' - legacy sparse natural learnsets are supplemented from existing legal pools')
print(' - intentional tiny/gimmick learnsets remain sparse')
print(' - National Dex 810+ has staged natural-learning support')
print(' - move queue and UI/battle bounds guards are present')
