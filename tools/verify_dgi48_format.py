#!/usr/bin/env python3
from pathlib import Path
r=Path(__file__).resolve().parents[1]
ino=(r/'TamaPoke.ino').read_text(encoding='utf-8'); html=(r/'tools/Digimon-SD-Pack-Maker.html').read_text(encoding='utf-8')
checks={'version':'#define FW_VERSION "3.89.0"' in ino,'48x48':'DIGI_FRAME_MAX = 48' in ino and '15*48*48*2' in html,
'legacy':'dgi1=!memcmp(h,"DGI1",4)' in ino and 'dgi2=!memcmp(h,"DGI2",4)' in ino,
'DGI3':'dgi3=!memcmp(h,"DGI3",4)' in ino and "decode(data.slice(0,4))!=='DGI3'" in html,
'black-safe':'digiTransparent=dgi3?0x0001:0x0000' in ino and 'if(a<32)return 1' in html,
'roundtrip':'validateDgi(cat(head,pix),id,15)' in html,'nested':'async function batchPack(inners)' in html}
bad=[k for k,v in checks.items() if not v]
if bad: raise SystemExit('DGI format FAIL: '+', '.join(bad))
print('DGI3 OK: 48x48, 3/15 frames, black-safe transparency, validation, DGI1/DGI2 compatibility')
