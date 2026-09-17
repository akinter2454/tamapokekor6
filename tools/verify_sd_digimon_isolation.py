#!/usr/bin/env python3
from pathlib import Path
r=Path(__file__).parents[1]
ino=(r/'TamaPoke.ino').read_text(encoding='utf-8')
sd=(r/'sdmon.cpp').read_text(encoding='utf-8')
pet=(r/'pet.cpp').read_text(encoding='utf-8')
digi=(r/'digimon.cpp').read_text(encoding='utf-8')
html=(r/'tools/Digimon-SD-Pack-Maker.html').read_text(encoding='utf-8')
assert 'prefs.begin("tamapoke", false)' in pet
assert 'prefs.begin("digipet",false)' in digi
assert '"/mons/' in sd and '"/digimon/d%03u.dgi"' in ino
assert 'SD_MMC.begin("/sdcard", true /* 1-bit mode */, false' in sd
assert "name:`digimon/d${String(id).padStart(3,'0')}.dgi`" in html
assert '3*48*48*2' in html and '3,48,48' in html
assert 'w===48&&h===48' in html and 'w===48&&h===64' in html
assert 'mons/' not in html, 'converter must never write into the Pokemon folder'
print('SD isolation OK: no format, separate folders, separate NVS namespaces')
