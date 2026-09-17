#!/usr/bin/env python3
from pathlib import Path
import re, sys

ROOT = Path(__file__).resolve().parent.parent
DEX = ROOT / 'dex.h'
PET = ROOT / 'pet.h'

dex = DEX.read_text(encoding='utf-8')
pet = PET.read_text(encoding='utf-8')

m = re.search(r'#define\s+MAX_LEVEL\s+(\d+)', pet)
if not m:
    raise SystemExit('MAX_LEVEL not found')
max_level = int(m.group(1))

m = re.search(r'#define\s+DEX_COUNT\s+(\d+)', dex)
if not m:
    raise SystemExit('DEX_COUNT not found')
dex_count = int(m.group(1))

start = dex.find('static const DexEntry DEX_TBL')
if start < 0:
    raise SystemExit('DEX_TBL not found')
body_start = dex.find('{', start)
body_end = dex.find('\n};', body_start)
if body_start < 0 or body_end < 0:
    raise SystemExit('DEX_TBL body not found')
body = dex[body_start:body_end]
entries = []
for mat in re.finditer(r'\{\s*"([^"]*)"\s*,\s*(-?\d+)\s*,\s*(-?\d+)\s*,', body):
    entries.append((mat.group(1), int(mat.group(2)), int(mat.group(3))))
if len(entries) != dex_count + 1:
    raise SystemExit(f'DEX_TBL row mismatch: got {len(entries)}, expected {dex_count+1}')

bad_direct=[]
for i,(name,to,lvl) in enumerate(entries):
    if lvl < 0 or lvl > max_level:
        bad_direct.append((i,name,to,lvl))
    if to < 0 or to > dex_count:
        bad_direct.append((i,name,'target',to))
if bad_direct:
    raise SystemExit('direct evolution overflow/out-of-range: '+repr(bad_direct[:30]))

extra=[]
m = re.search(r'static const uint8_t\s+EXTRA_BRANCH_LEVELS\[[^\]]+\]\s*=\s*\{([^}]*)\};', dex, re.S)
if m:
    extra=[int(x) for x in re.findall(r'\b\d+\b',m.group(1))]
    bad=[v for v in extra if v != 0 and not (2 <= v <= max_level)]
    if bad:
        raise SystemExit('extra evolution level overflow: '+repr(bad[:30]))

# The early-retire debt itself must fit the game level range. Runtime also uses
# effectiveEvolutionLevel(), which clamps the total base + care + retire gate.
m = re.search(r'#define\s+EVO_PENALTY_LEVELS\s+\(\(uint8_t\)(\d+)\)', pet)
if not m:
    raise SystemExit('bounded EVO_PENALTY_LEVELS literal not found')
penalty=int(m.group(1))
if penalty < 0 or penalty > max_level:
    raise SystemExit(f'EVO_PENALTY_LEVELS out of range: {penalty}')

petcpp=(ROOT/'pet.cpp').read_text(encoding='utf-8')
if 'uint8_t effectiveEvolutionLevel(' not in petcpp or 'if (need > MAX_LEVEL) need = MAX_LEVEL;' not in petcpp:
    raise SystemExit('effective evolution MAX_LEVEL clamp missing')
if 'evolutionTargetLevel(' not in petcpp or 'effectiveEvolutionTargetLevel(' not in petcpp:
    raise SystemExit('per-target branch evolution level guard missing')
if 'if (need == 0 && dexHasEvolution(d)) need = 30;' in petcpp:
    raise SystemExit('legacy generic Lv.30 extra-only evolution fallback returned')
if 'int16_t target = EXTRA_BRANCH_EVOS[i];' not in petcpp:
    raise SystemExit('dexEvolutionLevel real-edge target scan missing')

nonzero=[lvl for _,_,lvl in entries if lvl]
print(f'evolution audit OK: dex_count={dex_count}, direct_max={max(nonzero) if nonzero else 0}, '
      f'extra_max={max(extra) if extra else 0}, max_level={max_level}, retire_penalty={penalty}')
