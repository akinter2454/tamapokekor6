#!/usr/bin/env python3
"""Verify Alola/Galar/Hisui/Paldea form-aware evolution routing.

Run after sync_pmd_catalog.py.  The core regression is that new National-Dex
species whose official parent is a regional form must be sourced from that
independent TamaPoke form ID, never from the normal parent sharing the same
National-Dex number.
"""
from __future__ import annotations
import json,re
from pathlib import Path

ROOT=Path(__file__).resolve().parent.parent
CAT=json.loads((ROOT/'tools/pmd_catalog.json').read_text(encoding='utf-8'))
DEX=(ROOT/'dex.h').read_text(encoding='utf-8')
OUT=ROOT/'tools/regional_evolution_report.txt'

REGION={'alola':6,'galar':7,'hisui':8,'paldea':9}

def base_id(n:int)->int:
    return n if n<=809 else n+18

entries=[e for e in CAT.get('entries',[]) if e.get('enabled')]

def toks(e):
    s=' '.join(str(e.get(k) or '') for k in ('name','form_name','lock_key','pmd_path')).casefold().replace('-',' ')
    return set(re.findall(r'[a-z0-9]+',s))

def form_id(nat:int, region, required=(), forbidden=()):
    cand=[]
    req=set(required); bad=set(forbidden)
    for e in entries:
        if e.get('kind')!='form' or int(e.get('natdex',0))!=nat:
            continue
        if region is not None and int(e.get('region',-1))!=region:
            continue
        t=toks(e)
        if req and not req.issubset(t): continue
        if bad and (bad & t): continue
        cand.append(e)
    if not cand:
        return None
    if len(cand)>1:
        # Prefer the least-specialized form label (e.g. Galar Darmanitan over
        # Galar Zen) after explicit forbidden tokens were applied.
        cand=sorted(cand,key=lambda e:(len(toks(e)),len(str(e.get('form_name') or '')),int(e['id'])))
        if len(cand)>1 and len(toks(cand[0]))==len(toks(cand[1])):
            raise SystemExit(f'ambiguous form selector nat#{nat} region={region} required={required}: '+repr([(x['id'],x.get('name'),x.get('form_name')) for x in cand[:5]]))
    return int(cand[0]['id'])

# Parse direct DexEntry evolvesTo relations.
direct=set()
start=DEX.index('static const DexEntry DEX_TBL')
block=DEX[start:DEX.index('\n};',start)]
rx=re.compile(r'^\s*\{\s*"(?:[^"\\]|\\.)*"\s*,\s*(-?\d+)\s*,.*?//\s*(\d+)\b.*$',re.M)
for m in rx.finditer(block):
    target=int(m.group(1)); src=int(m.group(2))
    if target>0: direct.add((src,target))
extra={(int(x['base']),int(x['target'])) for x in CAT.get('extra_edges',[])}
# Three original v3.58 base -> Alola final branches live in dedicated arrays.
special={(25,812),(102,826),(104,827)}
edges=direct|extra|special

checks=[]
def B(n): return ('base',n,None,(),())
def F(n,r=None,req=(),bad=()): return ('form',n,(REGION[r] if r is not None else None),tuple(req),tuple(bad))

def resolve(spec):
    kind,n,r,req,bad=spec
    if kind=='base':
        # New base species are catalog-managed and may lack a current sprite.
        iid=base_id(n)
        if n>809:
            hit=next((e for e in entries if int(e.get('id',0))==iid and int(e.get('natdex',0))==n and e.get('kind')=='base'),None)
            if not hit: return None
        return iid
    return form_id(n,r,req,bad)

def add(label,src,dst,required=True): checks.append((label,src,dst,required))

# Alola (fixed v3.58 IDs + three normal-parent branches)
add('Alola Rattata -> Raticate',F(19,'alola'),F(20,'alola'))
add('Pikachu -> Alola Raichu',B(25),F(26,'alola'))
add('Alola Sandshrew -> Sandslash',F(27,'alola'),F(28,'alola'))
add('Alola Vulpix -> Ninetales',F(37,'alola'),F(38,'alola'))
add('Alola Diglett -> Dugtrio',F(50,'alola'),F(51,'alola'))
add('Alola Meowth -> Persian',F(52,'alola'),F(53,'alola'))
add('Alola Geodude -> Graveler',F(74,'alola'),F(75,'alola'))
add('Alola Graveler -> Golem',F(75,'alola'),F(76,'alola'))
add('Alola Grimer -> Muk',F(88,'alola'),F(89,'alola'))
add('Exeggcute -> Alola Exeggutor',B(102),F(103,'alola'))
add('Cubone -> Alola Marowak',B(104),F(105,'alola'))

# Galar
add('Galar Meowth -> Perrserker',F(52,'galar'),B(863))
add('Galar Ponyta -> Rapidash',F(77,'galar'),F(78,'galar'))
add('Galar Slowpoke -> Slowbro',F(79,'galar'),F(80,'galar'))
add('Galar Slowpoke -> Slowking',F(79,'galar'),F(199,'galar'))
add("Galar Farfetch'd -> Sirfetch'd",F(83,'galar'),B(865))
add('Koffing -> Galar Weezing',B(109),F(110,'galar'))
add('Mime Jr. -> Galar Mr. Mime',B(439),F(122,'galar'))
add('Galar Mr. Mime -> Mr. Rime',F(122,'galar'),B(866))
add('Galar Corsola -> Cursola',F(222,'galar'),B(864))
add('Galar Zigzagoon -> Linoone',F(263,'galar'),F(264,'galar'))
add('Galar Linoone -> Obstagoon',F(264,'galar'),B(862))
add('Galar Darumaka -> Darmanitan',F(554,'galar'),F(555,'galar',bad=('zen',)))
add('Galar Yamask -> Runerigus',F(562,'galar'),B(867))

# Hisui, including the three new evolutions of ordinary old species.
add('Stantler -> Wyrdeer',B(234),B(899))
add('Scyther -> Kleavor',B(123),B(900))
add('Ursaring -> Ursaluna',B(217),B(901))
add('White-Striped Basculin -> Basculegion',F(550,'hisui',req=('white',)),B(902))
add('Hisui Sneasel -> Sneasler',F(215,'hisui'),B(903))
add('Hisui Qwilfish -> Overqwil',F(211,'hisui'),B(904))
add('Hisui Growlithe -> Arcanine',F(58,'hisui'),F(59,'hisui'))
add('Hisui Voltorb -> Electrode',F(100,'hisui'),F(101,'hisui'))
add('Quilava -> Hisui Typhlosion',B(156),F(157,'hisui'))
add('Petilil -> Hisui Lilligant',B(548),F(549,'hisui'))
add('Dewott -> Hisui Samurott',B(502),F(503,'hisui'))
add('Rufflet -> Hisui Braviary',B(627),F(628,'hisui'))
add('Goomy -> Hisui Sliggoo',B(704),F(705,'hisui'))
add('Hisui Sliggoo -> Goodra',F(705,'hisui'),F(706,'hisui'))
add('Bergmite -> Hisui Avalugg',B(712),F(713,'hisui'))
add('Dartrix -> Hisui Decidueye',B(723),F(724,'hisui'))
add('Hisui Zorua -> Zoroark',F(570,'hisui'),F(571,'hisui'))

# Same National-Dex ambiguity exists in Paldea; keep this one guarded too.
add('Paldea Wooper -> Clodsire',F(194,'paldea'),B(980))

errors=[]; lines=['TamaPoke regional evolution audit','Regions: Alola / Galar / Hisui (+ Paldea ambiguity guard)','']
for label,ss,tt,required in checks:
    s=resolve(ss); t=resolve(tt)
    if t is None:
        lines.append(f'SKIP target sprite unavailable: {label}')
        continue
    if s is None:
        errors.append(f'MISSING SOURCE: {label}')
        lines.append(f'FAIL missing source: {label}')
        continue
    ok=(s,t) in edges
    lines.append(f'{"OK" if ok else "FAIL"} {label}: id {s} -> {t}')
    if not ok: errors.append(f'MISSING EDGE {label}: {s}->{t}')

# Explicitly ban the wrong normal-parent routes for every regional descendant.
wrong_pairs=[(264,862),(52,863),(222,864),(83,865),(122,866),(562,867),(550,902),(215,903),(211,904),(194,980)]
for parent,target in wrong_pairs:
    a=base_id(parent); b=base_id(target)
    if (a,b) in edges:
        errors.append(f'WRONG NORMAL-PARENT EDGE nat#{parent}->nat#{target}: id {a}->{b}')
        lines.append(f'FAIL wrong normal-parent edge nat#{parent}->nat#{target}: id {a}->{b}')
    else:
        lines.append(f'OK no wrong normal-parent edge nat#{parent}->nat#{target}')

OUT.write_text('\n'.join(lines)+'\n',encoding='utf-8')
print('\n'.join(lines))
if errors:
    raise SystemExit('regional evolution audit failed:\n- '+'\n- '.join(errors))
print(f'regional evolution audit OK: {sum(1 for x in lines if x.startswith("OK "))} checks')
