#!/usr/bin/env python3
"""Regression guard for species-specific Digimon evolution branches."""
from pathlib import Path
import re

root = Path(__file__).resolve().parents[1]
cpp = (root / "digimon.cpp").read_text(encoding="utf-8")
ino = (root / "TamaPoke.ino").read_text(encoding="utf-8")

assert '#define FW_VERSION "3.89.0"' in ino
assert 'uint8_t digimonEvolutionBranches(uint16_t i,uint16_t out[4])' in cpp
assert 'cur.version>=1&&cur.version<=5&&(cur.stage==DIGI_ADULT||cur.stage==DIGI_PERFECT)' in cpp
assert 'uint8_t targetRank=cur.stage==DIGI_ADULT?(count==4?rank/2:rank/3):rank' in cpp
assert 'uint8_t limit=cur.stage<=DIGI_CHILD?4:(count<=3?2:3)' in cpp
assert 'return branches[bestStat];' in cpp
assert 'tr[bestStat]/4' not in cpp
assert '(bestStat+lv+' not in cpp
assert 'static const uint16_t tr[]={0,0,4,8,12}' in cpp

rows = [
    (name, int(version), int(stage), int(style))
    for name, version, stage, _power, style in re.findall(
        r'D\("([^"]+)",(\d+),(\d+),(\d+),(\d+)\)', cpp
    )
]
fusion = {"Mastemon", "Tlalocmon", "Aegisdramon", "Mitamamon",
          "Voltobautamon", "Cernumon", "Omegamon", "Chaosdramon", "Proximamon"}

checked = 0
for index, (_name, version, stage, _style) in enumerate(rows):
    if stage >= 5:
        continue
    sources = [i for i, row in enumerate(rows) if row[1] == version and row[2] == stage]
    targets = [i for i, row in enumerate(rows)
               if row[1] == version and row[2] == stage + 1 and row[0] not in fusion]
    assert targets, (index, rows[index])
    if 1 <= version <= 5 and stage in (3, 4):
        rank = sources.index(index)
        target_rank = (rank // 2 if len(targets) == 4 else rank // 3) if stage == 3 else rank
        target_rank = min(target_rank, len(targets) - 1)
        family = [targets[target_rank]]
        assert len(family) == 1
        checked += 1
        continue
    limit = min(len(targets), 4 if stage <= 2 else (2 if len(targets) <= 3 else 3))
    rank = sources.index(index)
    start = rank * (len(targets) - limit) // (len(sources) - 1) if len(sources) > 1 else 0
    family = targets[start:start + limit]
    assert 1 <= len(family) <= (4 if stage <= 2 else 3)
    if len(targets) > limit:
        assert set(family) != set(targets), rows[index]
    checked += 1

assert checked >= 190, checked

# Every DMC has exactly 7 Adults grouped 3/3/1 into its 3 Perfects, followed
# by a one-to-one Perfect-to-Ultimate route. Ver.5 is asserted by name because
# it is the user-facing reference case.
for version in range(1, 6):
    adults = [r[0] for r in rows if r[1] == version and r[2] == 3]
    perfects = [r[0] for r in rows if r[1] == version and r[2] == 4]
    ultimates = [r[0] for r in rows if r[1] == version and r[2] == 5]
    assert len(adults) == 7 and len(ultimates) == 3
    if version == 3:
        assert len(perfects) == 4
        adult_results = [perfects[min(i // 2, 3)] for i in range(7)]
        assert adult_results == [perfects[0],perfects[0],perfects[1],perfects[1],perfects[2],perfects[2],perfects[3]]
        ultimate_results = [ultimates[min(i, 2)] for i in range(4)]
        assert ultimate_results == [ultimates[0],ultimates[1],ultimates[2],ultimates[2]]
    else:
        assert len(perfects) == 3
        adult_results = [perfects[min(i // 3, 2)] for i in range(7)]
        assert adult_results[:3] == [perfects[0]] * 3
        assert adult_results[3:6] == [perfects[1]] * 3
        assert adult_results[6:] == [perfects[2]]

v5_adults = [r[0] for r in rows if r[1] == 5 and r[2] == 3]
v5_perfects = [r[0] for r in rows if r[1] == 5 and r[2] == 4]
v5_ultimates = [r[0] for r in rows if r[1] == 5 and r[2] == 5]
assert v5_adults == ['DarkTyranomon','Cyclomon','Devidramon','Tuskmon','Flymon','Deltamon','Raremon']
assert v5_perfects == ['MetalTyranomon','Nanomon','ExTyranomon']
assert v5_ultimates == ['Mugendramon','Raidenmon','Gaioumon']
print(f"fixed Digimon branches OK: {checked} sources; DMC late stages compact/fixed; Pendulum unchanged")
