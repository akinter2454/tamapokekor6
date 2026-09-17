#!/usr/bin/env python3
"""Embed compiled TamaPoke firmware into the GitHub Pages installer HTML.

Sprite packs stay as separate site/*.pak files: the PMDCollab catalog can grow
large, and keeping those files external avoids a giant single HTML document.
"""
from pathlib import Path
import base64,json,re,sys
if len(sys.argv)!=4:
    raise SystemExit('usage: embed_tamapoke_firmware.py TEMPLATE firmware_dir OUTPUT')
template=Path(sys.argv[1]);fwdir=Path(sys.argv[2]);out=Path(sys.argv[3])
required={'bootloader':fwdir/'bootloader.bin','partitions':fwdir/'partitions.bin','boot_app0':fwdir/'boot_app0.bin','app':fwdir/'app.bin'}
missing=[str(p) for p in required.values() if not p.is_file()]
if missing:raise SystemExit('missing firmware: '+', '.join(missing))
fw={k:base64.b64encode(p.read_bytes()).decode('ascii') for k,p in required.items()}
s=template.read_text(encoding='utf-8')
s,n=re.subn(r'const EMBEDDED_FIRMWARE = \{.*?\n\};','const EMBEDDED_FIRMWARE = '+json.dumps(fw,separators=(',',':'))+';',s,count=1,flags=re.S)
if n!=1:raise SystemExit('EMBEDDED_FIRMWARE block not found')
s,n=re.subn(r'const EMBEDDED_SPRITE_PAKS = \{.*?\};','const EMBEDDED_SPRITE_PAKS = {};',s,count=1,flags=re.S)
if n!=1:raise SystemExit('EMBEDDED_SPRITE_PAKS block not found')
out.write_text(s,encoding='utf-8')
print('wrote',out,'bytes=',out.stat().st_size)
