#!/usr/bin/env python3
"""Build chunked GitHub Pages/WebSerial TPAK files from current PMDCollab catalog.

Each output file is kept below ~18 MiB so GitHub Pages deployments do not depend
on a single huge asset. sprite-manifest.json tells the installer which chunks
to send for each region and for the one-click "all added" operation.
"""
from pathlib import Path
import json,struct
HERE=Path(__file__).resolve().parent;MONS=HERE/'sdcard'/'mons';OUT=HERE/'web_extra';CAT=HERE/'pmd_catalog.json'
REGIONS=['kanto','johto','hoenn','sinnoh','unova','kalos','alola','galar','hisui','paldea']
MAX_DATA=18*1024*1024

def write_pak(path,files):
    ents=[]
    for f in files:
        n=f'mons/{f.name}'.encode();d=f.read_bytes();ents.append((n,d))
    with path.open('wb') as out:
        out.write(b'TPAK');out.write(struct.pack('<H',len(ents)))
        for n,d in ents:out.write(struct.pack('<B',len(n)));out.write(n);out.write(struct.pack('<I',len(d)))
        for _,d in ents:out.write(d)
    return len(ents)

def files_for(entries):
    fs=[]
    for e in entries:
        i=int(e['id'])
        if i<810 or not e.get('enabled'):continue
        for pref in ('p','ps'):
            f=MONS/f'{pref}{i:03d}.bin'
            if f.is_file() and f.stat().st_size>=64:fs.append(f)
    return fs

def chunks(fs):
    out=[];cur=[];size=0
    for f in fs:
        n=f.stat().st_size
        if cur and size+n>MAX_DATA:out.append(cur);cur=[];size=0
        cur.append(f);size+=n
    if cur:out.append(cur)
    return out

def main():
    cat=json.loads(CAT.read_text(encoding='utf-8'));entries=cat['entries'];OUT.mkdir(parents=True,exist_ok=True)
    for p in OUT.glob('sprites-extra-*.pak'):p.unlink()
    audit=json.loads((HERE/'sprite_audit.json').read_text(encoding='utf-8')) if (HERE/'sprite_audit.json').is_file() else {}
    retired_ids=sorted({int(x) for x in cat.get('retired_sprite_ids',[]) if int(x)>=810})
    retired_files=[]
    for iid in retired_ids:
        retired_files += [f'mons/p{iid:03d}.bin', f'mons/ps{iid:03d}.bin']
    manifest={'schema':4,'catalog_fingerprint':audit.get('catalog_fingerprint'),'regions':{},'groups':{},'all':[],'audit':'sprite-audit.json',
              'retired_ids':retired_ids,'retired_files':retired_files}
    total=0
    for r,name in enumerate(REGIONS):
        fs=files_for([e for e in entries if int(e.get('region',10))==r]);parts=[]
        for idx,ch in enumerate(chunks(fs),1):
            fn=f'sprites-extra-{name}-{idx}.pak';write_pak(OUT/fn,ch);parts.append(fn);manifest['all'].append(fn)
        manifest['regions'][name]=parts;total+=len(fs)
        print(f'{name}: {len(fs)} files, {len(parts)} chunk(s)')
    # v3.62.3 retained upgrade path: users who already have the existing 810+/1200+
    # catalog on microSD should not have to resend every older form just to add
    # the new Mega36 set. Build a dedicated delta group from catalog entries
    # explicitly marked mega=True. These files are also still present in their
    # normal region chunks/all list for clean first-time installations.
    mega_fs=files_for([e for e in entries if e.get('mega')])
    mega_parts=[]
    for idx,ch in enumerate(chunks(mega_fs),1):
        fn=f'sprites-extra-mega36-{idx}.pak'
        write_pak(OUT/fn,ch); mega_parts.append(fn)
    manifest['groups']['mega36']=mega_parts
    manifest['groups']['added-all']=list(manifest['all'])
    manifest['counts']={'all_files':total,'mega36_files':len(mega_fs),'retired_files':len(retired_files)}
    if len(mega_fs) != 72:
        raise SystemExit(f'Mega36 delta expected 72 normal/shiny files, got {len(mega_fs)}')

    (OUT/'sprite-manifest.json').write_text(json.dumps(manifest,ensure_ascii=False,indent=2),encoding='utf-8')
    if total==0:raise SystemExit('No added PMDCollab sprite files to publish')
    print(f'added sprite files: {total}; pak chunks: {len(manifest["all"])}; Mega36 delta: {len(mega_fs)} files/{len(mega_parts)} chunks; retired/disabled files: {len(retired_files)}')

if __name__=='__main__':main()
