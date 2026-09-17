#!/usr/bin/env python3
"""Independent DGI3 pixel round-trip test (does not redistribute sprites)."""
from __future__ import annotations
import io, struct, sys, zipfile
from pathlib import Path
from PIL import Image

TRANSPARENT = 0x0001

def rgb565(px):
    r,g,b,a=px
    if a < 32: return TRANSPARENT
    v=((r>>3)<<11)|((g>>2)<<5)|(b>>3)
    return 0 if v==TRANSPARENT else v

def verify_frame(raw: bytes, label: str):
    im=Image.open(io.BytesIO(raw)).convert("RGBA")
    if im.size != (48,48): raise AssertionError(f"{label}: {im.size} != 48x48")
    expected=[rgb565(p) for p in im.getdata()]
    payload=struct.pack("<2304H",*expected)
    decoded=list(struct.unpack("<2304H",payload))
    if decoded != expected: raise AssertionError(f"{label}: RGB565 LE round-trip mismatch")
    for src,v in zip(im.getdata(),decoded):
        if src[3] < 32 and v != TRANSPARENT: raise AssertionError(f"{label}: alpha lost")
        if src[3] >= 32 and src[0] < 8 and src[1] < 4 and src[2] < 8 and v != 0:
            raise AssertionError(f"{label}: black became transparent")
    return payload

def main(path: Path):
    checked=0
    with zipfile.ZipFile(path) as outer:
        inner_names=[n for n in outer.namelist() if n.lower().endswith(".zip")]
        for inner_name in inner_names:
            with zipfile.ZipFile(io.BytesIO(outer.read(inner_name))) as inner:
                frames=[]
                for i in range(15):
                    matches=[n for n in inner.namelist() if n.replace("\\","/").endswith(f"/{i}.png") or n==f"{i}.png"]
                    if not matches: raise AssertionError(f"{inner_name}: missing {i}.png")
                    frames.append(verify_frame(inner.read(matches[0]),f"{inner_name}/{i}.png"))
                header=b"DGI3"+bytes((checked & 0xff,0,0,15,48,48))
                dgi=header+b"".join(frames)
                if len(dgi)!=10+15*48*48*2: raise AssertionError(f"{inner_name}: bad DGI size")
                if dgi[:4]!=b"DGI3" or dgi[7:10]!=bytes((15,48,48)): raise AssertionError(f"{inner_name}: bad header")
                checked+=1
    if not checked: raise AssertionError("no nested character ZIPs found")
    print(f"DGI3 round-trip OK: {checked} species, {checked*15} frames; black preserved, alpha sentinel=0x0001")

if __name__=="__main__":
    if len(sys.argv)!=2: raise SystemExit("usage: verify_dgi3_roundtrip.py <outer-sprite-pack.zip>")
    main(Path(sys.argv[1]))
