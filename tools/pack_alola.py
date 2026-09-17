#!/usr/bin/env python3
"""Download the real PMDCollab Alolan-form behaviour sprites and pack them for TamaPoke.

This v3.58 rollback intentionally supports ONLY the 18 Alolan forms added as IDs 810..827.
No Mega / Hisui fallback art is used.  Every source must be the PMDCollab SpriteCollab
behaviour-sprite set (form 0001), with a real AnimData.xml plus Idle and Walk animation.

PMDCollab layout used here:
  sprite/NNNN/0001/              Alolan form
  sprite/NNNN/0001/0001/         Alolan shiny

Output:
  tools/sdcard/mons/p810.bin .. p827.bin
  tools/sdcard/mons/ps810.bin .. ps827.bin
  tools/alola_sprite_report.txt
"""
from __future__ import annotations
import os
import struct
import urllib.request
import urllib.error
import xml.etree.ElementTree as ET
from pathlib import Path
from PIL import Image

HERE = Path(__file__).resolve().parent
OUT = HERE / 'sdcard' / 'mons'
CACHE = HERE / 'pmd_cache_alola'
REPORT = HERE / 'alola_sprite_report.txt'
RAW_BASES = (
    'https://raw.githubusercontent.com/PMDCollab/SpriteCollab/master/sprite',
    'https://github.com/PMDCollab/SpriteCollab/raw/refs/heads/master/sprite',
)
SLOW, MIN_MS, ALPHA_T = 1.4, 70, 128
ACTIONS = [
    (0, 'Idle', 0), (1, 'Walk', 6), (2, 'Walk', 2), (3, 'Sleep', 0),
    (4, 'Eat', 0), (5, 'Hurt', 0), (6, 'Attack', 0), (7, 'Pose', 0),
    (8, 'Hop', 0), (9, 'Nod', 0), (10, 'DeepBreath', 0), (11, 'Sit', 0),
]
# TamaPoke appended ID -> (National Dex source, English label)
ALOLA = {
    810:(19,'Rattata'), 811:(20,'Raticate'), 812:(26,'Raichu'),
    813:(27,'Sandshrew'), 814:(28,'Sandslash'), 815:(37,'Vulpix'),
    816:(38,'Ninetales'), 817:(50,'Diglett'), 818:(51,'Dugtrio'),
    819:(52,'Meowth'), 820:(53,'Persian'), 821:(74,'Geodude'),
    822:(75,'Graveler'), 823:(76,'Golem'), 824:(88,'Grimer'),
    825:(89,'Muk'), 826:(103,'Exeggutor'), 827:(105,'Marowak'),
}

def source_rel(source_dex: int, shiny: bool) -> str:
    return f'{source_dex:04d}/0001' + ('/0001' if shiny else '')

def fetch_rel(rel: str, dest: Path) -> str:
    """Fetch one exact PMDCollab repo file. Returns the source URL used."""
    if dest.is_file() and dest.stat().st_size:
        return '(cache)'
    dest.parent.mkdir(parents=True, exist_ok=True)
    last = None
    for base in RAW_BASES:
        url = f'{base}/{rel}'
        try:
            req = urllib.request.Request(url, headers={'User-Agent':'TamaPoke-v3.58-AlolaPMD/1.0'})
            data = urllib.request.urlopen(req, timeout=60).read()
            if not data:
                raise RuntimeError('empty response')
            dest.write_bytes(data)
            return url
        except Exception as exc:
            last = exc
    raise RuntimeError(f'PMDCollab file missing: {rel} ({last})')

def rgb565(r,g,b):
    return (r>>3)<<11 | (g>>2)<<5 | (b>>3)

def load_animdata(folder: Path):
    anims = {}
    tree = ET.parse(folder/'AnimData.xml')
    root = tree.getroot()
    anim_root = root.find('Anims')
    if anim_root is None:
        raise RuntimeError('AnimData.xml has no <Anims>')
    for a in anim_root:
        name_el = a.find('Name')
        if name_el is None or not name_el.text:
            continue
        name = name_el.text
        fw_el = a.find('FrameWidth')
        if fw_el is None:
            cp = a.find('CopyOf')
            if cp is not None and cp.text:
                anims[name] = ('copy', cp.text)
            continue
        fh_el = a.find('FrameHeight')
        ds_el = a.find('Durations')
        if fh_el is None or ds_el is None:
            continue
        durs = [int(d.text) for d in ds_el if d.text]
        anims[name] = (int(fw_el.text), int(fh_el.text), durs, name)
    # Resolve copies after reading all entries.
    for k,v in list(anims.items()):
        if isinstance(v, tuple) and v and v[0] == 'copy':
            anims[k] = anims.get(v[1])
    return anims

def pack(out_id: int, source_dex: int, shiny: bool=False):
    relroot = source_rel(source_dex, shiny)
    folder = CACHE / (f'{source_dex:04d}-alola' + ('-shiny' if shiny else ''))
    xml_url = fetch_rel(f'{relroot}/AnimData.xml', folder/'AnimData.xml')
    anims = load_animdata(folder)
    # TamaPoke's creature must behave like the original PMD sprites, not a static image.
    missing_required = [x for x in ('Idle','Walk') if x not in anims or anims[x] is None]
    if missing_required:
        raise RuntimeError('required PMD animations missing: ' + ', '.join(missing_required))

    colmap = {}; pal = []; packed = []; used_actions = []
    for aid,name,row in ACTIONS:
        if name not in anims or anims[name] is None:
            continue
        fw,fh,durs,src = anims[name]
        if not durs:
            continue
        png = folder / f'{src}-Anim.png'
        fetch_rel(f'{relroot}/{src}-Anim.png', png)
        im = Image.open(png).convert('RGBA')
        rows = im.size[1] // fh
        r = row if rows > row else 0
        nf = min(len(durs), im.size[0] // fw, 24)
        if nf <= 0:
            continue
        data = bytearray()
        for i in range(nf):
            fr = im.crop((i*fw, r*fh, (i+1)*fw, (r+1)*fh))
            for px in fr.getdata():
                if px[3] < ALPHA_T:
                    data.append(0xFF); continue
                key = px[:3]
                if key not in colmap:
                    if len(pal) >= 255:
                        nearest = min(colmap, key=lambda c:sum((a-b)**2 for a,b in zip(c,key)))
                        colmap[key] = colmap[nearest]
                    else:
                        colmap[key] = len(pal); pal.append(key)
                data.append(colmap[key])
        ms = [max(MIN_MS, round(d*1000/60*SLOW)) for d in durs[:nf]]
        packed.append((aid,fw,fh,nf,ms,bytes(data)))
        used_actions.append((name,nf,fw,fh))

    ids = {x[0] for x in packed}
    if 0 not in ids or 1 not in ids:
        raise RuntimeError('packed Idle/Walk missing after conversion')

    OUT.mkdir(parents=True, exist_ok=True)
    path = OUT / f'p{"s" if shiny else ""}{out_id:03d}.bin'
    with path.open('wb') as f:
        f.write(b'TPK2')
        f.write(struct.pack('<BH', len(packed), len(pal)))
        for r,g,b in pal:
            f.write(struct.pack('<H', rgb565(r,g,b)))
        for aid,fw,fh,nf,ms,data in packed:
            f.write(struct.pack('<4B', aid,fw,fh,nf))
            f.write(struct.pack(f'<{nf}H', *ms))
            f.write(data)
    if path.stat().st_size < 64:
        raise RuntimeError('generated TPK2 unexpectedly small')
    return path, xml_url, used_actions

if __name__ == '__main__':
    OUT.mkdir(parents=True, exist_ok=True)
    # Delete previous Alola outputs so stale files can never hide a missing source.
    for i in range(810,828):
        for prefix in ('p','ps'):
            (OUT/f'{prefix}{i:03d}.bin').unlink(missing_ok=True)

    failures=[]; report=[]
    report.append('TamaPoke v3.58 Alola PMDCollab sprite report')
    report.append('Source: https://github.com/PMDCollab/SpriteCollab')
    report.append('Required: exact form 0001, real AnimData.xml, Idle + Walk')
    report.append('')
    for out_id,(src,name) in ALOLA.items():
        for shiny in (False, True):
            tag = 'shiny' if shiny else 'normal'
            try:
                path,url,acts = pack(out_id,src,shiny)
                act_text=', '.join(f'{n}:{nf}f@{w}x{h}' for n,nf,w,h in acts)
                line=f'OK {out_id} #{src:04d} {name} Alola {tag}: {path.name} {path.stat().st_size} bytes | {act_text}'
                print(line); report.append(line)
            except Exception as exc:
                line=f'MISSING {out_id} #{src:04d} {name} Alola {tag}: {exc}'
                print(line); report.append(line); failures.append(line)
    REPORT.write_text('\n'.join(report)+'\n', encoding='utf-8')
    expected=18*2
    made=sum(1 for i in range(810,828) for p in ('p','ps') if (OUT/f'{p}{i:03d}.bin').is_file())
    print(f'Alola PMDCollab files: {made}/{expected}')
    if failures or made != expected:
        raise SystemExit('Some Alolan PMDCollab behaviour sprites are missing. See tools/alola_sprite_report.txt')
    print('All 18 Alolan forms + shiny variants packed from PMDCollab behaviour sprites.')
