#!/usr/bin/env python3
"""Build /digimon/*.dgi locally from a user-supplied PNG sprite ZIP.

The source images are never embedded in the TamaPoke distribution. Each DGI is
RGB565, 3 frames of 48x48, with transparent pixels stored as 0x0000.

Accepted PNG layouts: one 48x48 frame, 144x48 horizontal frames, 48x144
vertical frames, or the legacy 48x64 DMC sheet (three 16x16 cells on row 1).
"""
import argparse, io, re, struct, zipfile
from pathlib import Path
from PIL import Image

STAGE_DIR={0:"Baby I",1:"Baby II",2:"Child",3:"Adult",4:"Perfect",5:"Ultimate-Super Ultimate"}

def key(s): return re.sub(r"[^a-z0-9]","",s.lower().replace("centalmon","centaurmon"))
def catalog(source):
    txt=Path(source).read_text(encoding="utf-8")
    return [(n,int(v),int(s)) for n,v,s in re.findall(r'D\("([^"]+)",(\d),(\d),\d+,\d\)',txt)]
def rgb565(px):
    r,g,b,a=px
    if a<32:return 0
    return ((r>>3)<<11)|((g>>2)<<5)|(b>>3)
def frames48(im):
    nearest=getattr(Image,"Resampling",Image).NEAREST
    if im.size==(48,48): return [im.copy(),im.copy(),im.copy()]
    if im.size==(144,48): return [im.crop((i*48,0,(i+1)*48,48)) for i in range(3)]
    if im.size==(48,144): return [im.crop((0,i*48,48,(i+1)*48)) for i in range(3)]
    if im.size==(48,64):
        return [im.crop((i*16,0,(i+1)*16,16)).resize((48,48),nearest) for i in range(3)]
    raise ValueError(f"unsupported PNG size {im.width}x{im.height}")
def main():
    ap=argparse.ArgumentParser();ap.add_argument("sprites_zip");ap.add_argument("--catalog",default="digimon.cpp");ap.add_argument("--out",default="sdcard/digimon");a=ap.parse_args()
    out=Path(a.out);out.mkdir(parents=True,exist_ok=True)
    with zipfile.ZipFile(a.sprites_zip) as z:
        png={key(Path(n).stem):(n,z.read(n)) for n in z.namelist() if n.lower().endswith('.png') and not n.startswith('Idle Frame Only/')}
        made=[];missing=[]
        for idx,(name,ver,stage) in enumerate(catalog(a.catalog)):
            hit=png.get(key(name))
            if not hit: missing.append(name);continue
            im=Image.open(io.BytesIO(hit[1])).convert('RGBA')
            try: frames=frames48(im)
            except ValueError: missing.append(name);continue
            payload=bytearray(b'DGI1'+bytes((idx,ver,stage,3,48,48)))
            for frame in frames:
                for p in frame.getdata(): payload += struct.pack('<H',rgb565(p))
            (out/f'd{idx:03}.dgi').write_bytes(payload);made.append(name)
    (out/'manifest.txt').write_text('\n'.join(made),encoding='utf-8')
    print(f'created={len(made)} missing={len(missing)}')
    if missing: print('missing: '+', '.join(missing))
if __name__=='__main__':main()
