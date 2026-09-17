#!/usr/bin/env python3
"""Build one PC-downloadable microSD sprite ZIP for the current TamaPoke catalog.

The ZIP contains a root-level ``mons/`` directory. Users can remove/rename the
old ``/mons`` directory on the microSD and copy this one in a single operation.

Gen1-7 base sprites are unpacked from the immutable audited upstream TPAKs.
Current 810+/official-form/Mega sprites come from the exact local catalog build
that also feeds the browser's chunked TPAKs.
"""
from __future__ import annotations
from pathlib import Path
from urllib.request import Request, urlopen
from urllib.error import URLError, HTTPError
import io, json, re, shutil, struct, tempfile, time, zipfile

ROOT=Path(__file__).resolve().parents[1]
TOOLS=ROOT/'tools'
MONS=TOOLS/'sdcard'/'mons'
OUT=TOOLS/'web_extra'
CAT=TOOLS/'pmd_catalog.json'
AUDIT=TOOLS/'sprite_audit.json'
FW=ROOT/'TamaPoke.ino'
CREDITS=ROOT/'PMDCOLLAB_CREDITS.txt'
BASE_PACK_COMMIT='7d5a2b3f4a4bee4f107cdae77ad13949dddfc615'
BASE_URL=f'https://raw.githubusercontent.com/DylanPDao/TamaPoke/{BASE_PACK_COMMIT}/web/'
BASE_RANGES={
    'kanto':(1,151),'johto':(152,251),'hoenn':(252,386),'sinnoh':(387,493),
    'unova':(494,649),'kalos':(650,721),'alola':(722,809),
}
UA='TamaPoke-v3.62.8-FullSDSpriteBundle/1.0 (+noncommercial classroom project)'
BANNED=('altcolor','alt colour','alternate','cutscene','beta')


def fw_version()->str:
    s=FW.read_text(encoding='utf-8')
    m=re.search(r'^#define\s+FW_VERSION\s+"([^"]+)"',s,re.M)
    if not m: raise SystemExit('FW_VERSION missing')
    return m.group(1)


def parse_tpak(data:bytes):
    if len(data)<6 or data[:4]!=b'TPAK': raise ValueError('TPAK magic missing')
    count=struct.unpack_from('<H',data,4)[0]; off=6; meta=[]
    for _ in range(count):
        if off>=len(data): raise ValueError('TPAK header truncated')
        nl=data[off]; off+=1
        if off+nl+4>len(data): raise ValueError('TPAK entry header truncated')
        name=data[off:off+nl].decode('utf-8'); off+=nl
        size=struct.unpack_from('<I',data,off)[0]; off+=4
        meta.append((name,size))
    pos=off; out=[]
    for name,size in meta:
        if pos+size>len(data): raise ValueError(f'TPAK data truncated: {name}')
        out.append((name,data[pos:pos+size])); pos+=size
    if pos!=len(data):
        # Upstream packs should not carry silent trailing payloads.
        raise ValueError(f'TPAK trailing bytes: {len(data)-pos}')
    return out


def validate_base(region:str, items):
    lo,hi=BASE_RANGES[region]; seen=set(); sprites=0
    for name,data in items:
        if name in seen: raise ValueError(f'{region}: duplicate {name}')
        seen.add(name)
        if name=='mons/thumbs.bin':
            if len(data)<6 or data[:4]!=b'TPTH': raise ValueError(f'{region}: invalid thumbs.bin')
            continue
        m=re.fullmatch(r'mons/(ps?)(\d{3,4})\.bin',name)
        if not m: raise ValueError(f'{region}: forbidden filename {name}')
        iid=int(m.group(2))
        if not lo<=iid<=hi: raise ValueError(f'{region}: id out of range {name}')
        if len(data)<64 or data[:4] not in (b'TPK2',b'TPK3'):
            raise ValueError(f'{region}: invalid sprite {name}')
        sprites+=1
    if not sprites: raise ValueError(f'{region}: no sprites')
    return sprites


def fetch(url:str, attempts=4)->bytes:
    last=None
    for n in range(1,attempts+1):
        try:
            req=Request(url,headers={'User-Agent':UA,'Accept':'application/octet-stream'})
            with urlopen(req,timeout=90) as r:
                if getattr(r,'status',200)!=200: raise HTTPError(url,r.status,'HTTP',r.headers,None)
                return r.read()
        except (URLError,HTTPError,TimeoutError,OSError) as e:
            last=e
            if n<attempts: time.sleep(n*2)
    raise RuntimeError(f'download failed after {attempts} attempts: {url}: {last}')


def safe_write(dst:Path, data:bytes, source:str, sources:dict):
    # For p/ps files, conflicting duplicate content is always an error.
    if dst.exists():
        old=dst.read_bytes()
        if old!=data: raise ValueError(f'conflicting duplicate {dst.name}: {sources.get(dst.name)} vs {source}')
        return
    dst.write_bytes(data); sources[dst.name]=source


def build(fetcher=fetch, out_dir:Path|None=None):
    version=fw_version()
    cat=json.loads(CAT.read_text(encoding='utf-8'))
    audit=json.loads(AUDIT.read_text(encoding='utf-8'))
    enabled=[e for e in cat.get('entries',[]) if e.get('enabled')]
    bad=[(e.get('id'),e.get('name')) for e in enabled if any(x in str(e.get('name','')).lower() for x in BANNED)]
    if bad: raise SystemExit('presentation-only species leaked into active catalog: '+repr(bad[:20]))

    OUT.mkdir(parents=True,exist_ok=True)
    if out_dir is None: out_dir=OUT
    out_dir.mkdir(parents=True,exist_ok=True)
    stage=TOOLS/'full_sd_bundle'
    if stage.exists(): shutil.rmtree(stage)
    mons=stage/'mons'; mons.mkdir(parents=True)
    sources={}; region_counts={}; thumbs_candidates=[]

    # 1..809 from immutable audited base packs.
    for region in BASE_RANGES:
        url=BASE_URL+f'sprites-{region}.pak'
        raw=fetcher(url)
        items=parse_tpak(raw); region_counts[region]=validate_base(region,items)
        for name,data in items:
            fn=Path(name).name
            if fn=='thumbs.bin':
                thumbs_candidates.append((len(data),region,data)); continue
            safe_write(mons/fn,data,f'base:{region}@{BASE_PACK_COMMIT}',sources)
        print(f'base {region}: {region_counts[region]} sprite files')

    # If upstream includes thumbs in more than one regional pack, use the largest
    # valid TPTH blob.  Thumbnails are auxiliary; p/ps sprite identity never uses
    # this heuristic.
    if thumbs_candidates:
        _,region,data=max(thumbs_candidates,key=lambda x:x[0])
        (mons/'thumbs.bin').write_bytes(data); sources['thumbs.bin']=f'base-thumbs:{region}@{BASE_PACK_COMMIT}'

    # 810+ only from the exact current generated catalog.
    managed=0
    for e in enabled:
        iid=int(e['id'])
        if iid<810: continue
        for pref in ('p','ps'):
            src=MONS/f'{pref}{iid:03d}.bin'
            if not src.is_file(): raise SystemExit(f'missing current managed sprite: {src}')
            data=src.read_bytes()
            if len(data)<64 or data[:4] != b'TPK3': raise SystemExit(f'invalid managed TPK3: {src}')
            safe_write(mons/src.name,data,f'current-catalog:{iid}',sources); managed+=1

    # Hard guards: only current p/ps sprites + optional thumbs; no retired IDs.
    active_ids={int(e['id']) for e in enabled}
    retired={int(x) for x in cat.get('retired_sprite_ids',[])}
    if active_ids & retired: raise SystemExit('active/retired ID overlap')
    for p in mons.iterdir():
        if p.name=='thumbs.bin': continue
        m=re.fullmatch(r'(ps?)(\d{3,4})\.bin',p.name)
        if not m: raise SystemExit(f'unexpected bundle file {p.name}')
        iid=int(m.group(2))
        if iid>=810 and iid not in active_ids: raise SystemExit(f'retired/non-active managed sprite in bundle: {p.name}')
        magic=p.read_bytes()[:4]
        if magic not in (b'TPK2',b'TPK3'): raise SystemExit(f'bad sprite magic {p.name}: {magic!r}')

    # Include human-readable instructions and exact provenance.
    file_rows=[]
    for p in sorted(mons.iterdir(),key=lambda x:x.name):
        b=p.read_bytes(); file_rows.append((f'mons/{p.name}',len(b),sources.get(p.name,'')))
    total_bytes=sum(r[1] for r in file_rows)
    info=(
        f'TamaPoke KO v{version} - PC용 microSD 전체 스프라이트\n\n'
        '사용 방법\n'
        '1. TamaPoke 기기의 전원을 끕니다.\n'
        '2. microSD를 카드리더기로 PC에 연결합니다.\n'
        '3. 기존 SD의 /mons 폴더를 PC에 백업한 뒤 SD에서 /mons 폴더를 삭제하거나 이름을 바꿉니다.\n'
        '4. 이 ZIP을 풀어 나온 mons 폴더를 microSD 루트에 그대로 복사합니다.\n'
        '5. 안전하게 꺼낸 뒤 microSD를 기기에 다시 넣고 부팅합니다.\n\n'
        '주의: microSD 전체를 포맷할 필요가 없습니다. 게임 세이브/NVS는 이 ZIP이 건드리지 않습니다.\n'
        'AltColor / Alternate / Cutscene / Beta 표현용 리소스는 독립 종으로 포함하지 않습니다.\n'
        f'기본팩 고정 commit: {BASE_PACK_COMMIT}\n'
        f'카탈로그 fingerprint: {audit.get("catalog_fingerprint")}\n'
        f'스프라이트 파일 수: {sum(1 for r in file_rows if r[0].endswith(".bin") and "thumbs.bin" not in r[0])}\n'
        f'총 원본 바이트: {total_bytes}\n'
    )
    (stage/'README-SD-KO.txt').write_text(info,encoding='utf-8')
    if CREDITS.is_file(): shutil.copy2(CREDITS,stage/'PMDCOLLAB_CREDITS.txt')
    shutil.copy2(TOOLS/'sprite_audit.json',stage/'sprite-audit.json')
    shutil.copy2(TOOLS/'web_extra'/'sprite-manifest.json',stage/'sprite-manifest.json')
    detail={
        'schema':1,'firmware_version':version,'catalog_fingerprint':audit.get('catalog_fingerprint'),
        'base_pack_commit':BASE_PACK_COMMIT,'region_counts':region_counts,'managed_files':managed,
        'sprite_files':len(file_rows)-sum(1 for r in file_rows if r[0]=='mons/thumbs.bin'),
        'has_thumbs':any(r[0]=='mons/thumbs.bin' for r in file_rows),'uncompressed_bytes':total_bytes,
    }
    (stage/'BUNDLE-INFO.json').write_text(json.dumps(detail,ensure_ascii=False,indent=2)+'\n',encoding='utf-8')

    filename=f'TamaPoke-SD-Sprites-v{version}-Full.zip'
    out=out_dir/filename
    if out.exists(): out.unlink()
    with zipfile.ZipFile(out,'w',compression=zipfile.ZIP_DEFLATED,compresslevel=6,allowZip64=True) as z:
        for p in sorted(stage.rglob('*')):
            if p.is_file(): z.write(p,p.relative_to(stage).as_posix())
    metadata={
        'schema':1,'version':version,'filename':filename,'size_bytes':out.stat().st_size,
        'uncompressed_sprite_bytes':total_bytes,'catalog_fingerprint':audit.get('catalog_fingerprint'),
        'base_pack_commit':BASE_PACK_COMMIT,'counts':detail,'download_url':None,
        'release_tag':'sprites-current','storage_policy':'single rolling GitHub Release; no full-ZIP Actions artifact',
        'install':'Power off; back up/delete old /mons; extract this ZIP and copy its mons folder to the microSD root.',
    }
    (out_dir/'sprite-bundle.json').write_text(json.dumps(metadata,ensure_ascii=False,indent=2)+'\n',encoding='utf-8')
    report=(
        f'TamaPoke v{version} FULL SD SPRITE BUNDLE\n'
        f'file={filename}\nzip_bytes={out.stat().st_size}\n'
        f'uncompressed_sprite_bytes={total_bytes}\nbase_pack_commit={BASE_PACK_COMMIT}\n'
        f'catalog_fingerprint={audit.get("catalog_fingerprint")}\n'
        f'base_region_counts={json.dumps(region_counts,ensure_ascii=False)}\nmanaged_files={managed}\n'
        'presentation_variants=excluded (AltColor/Alternate/Cutscene/Beta)\nRESULT=PASS\n'
    )
    (out_dir/'full-sprite-bundle-report.txt').write_text(report,encoding='utf-8')
    print(report,end='')
    return out, metadata


def self_test():
    # Pure-offline parser/validator smoke test used by local checks and Actions.
    def mk(entries):
        b=io.BytesIO(); b.write(b'TPAK'); b.write(struct.pack('<H',len(entries)))
        for name,data in entries:
            nb=name.encode(); b.write(struct.pack('<B',len(nb))); b.write(nb); b.write(struct.pack('<I',len(data)))
        for _,data in entries:b.write(data)
        return b.getvalue()
    tpk=b'TPK2'+b'\0'*60
    items=parse_tpak(mk([('mons/p001.bin',tpk),('mons/ps001.bin',tpk)]))
    assert validate_base('kanto',items)==2
    try: validate_base('kanto',parse_tpak(mk([('mons/p152.bin',tpk)])))
    except ValueError: pass
    else: raise AssertionError('range guard failed')
    try: validate_base('kanto',parse_tpak(mk([('mons/AltColor.bin',tpk)])))
    except ValueError: pass
    else: raise AssertionError('filename guard failed')
    print('full SD sprite bundle self-test OK')

if __name__=='__main__':
    import sys
    if '--self-test' in sys.argv:self_test()
    else:build()
