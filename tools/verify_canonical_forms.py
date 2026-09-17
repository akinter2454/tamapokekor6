#!/usr/bin/env python3
"""Reject PMDCollab presentation-only slots as TamaPoke species.

AltColor/Alternate/Cutscene/Beta are asset variants, not canonical Pokemon
forms.  This verifier runs after sync_pmd_catalog.py and catches both fresh
tracker mistakes and stale catalog_lock entries from older TamaPoke builds.
"""
from __future__ import annotations
import json,re
from pathlib import Path

ROOT=Path(__file__).resolve().parent.parent
CAT=json.loads((ROOT/'tools/pmd_catalog.json').read_text(encoding='utf-8'))
LOCK=json.loads((ROOT/'tools/catalog_lock.json').read_text(encoding='utf-8'))
DEX=(ROOT/'dex.h').read_text(encoding='utf-8')
OUT=ROOT/'tools/canonical_forms_report.txt'

def reason(s):
    n=str(s or '').casefold().replace('_',' ').replace('-',' ')
    flat=re.sub(r'[^a-z]+','',n)
    if 'altcolor' in flat or 'altcolour' in flat:return 'AltColor'
    t=set(re.findall(r'[a-z0-9]+',n))
    for w in ('alternate','cutscene','beta'):
        if w in t:return w
    return None

errors=[]
lines=['TamaPoke canonical-form audit','Presentation-only PMDCollab slots must never be species/resources.','']

bad_lock=[]
for k,v in LOCK.get('forms',{}).items():
    r=reason(v.get('label'))
    if r: bad_lock.append((k,v,r))
if bad_lock:
    errors += [f'presentation slot remains in lock: {k} id={v.get("id")} label={v.get("label")}' for k,v,r in bad_lock]
else: lines.append('OK catalog_lock contains no AltColor/Alternate/Cutscene/Beta form records')

entries=CAT.get('entries',[])
bad_entries=[]
for e in entries:
    text=' '.join(str(e.get(k) or '') for k in ('name','form_name'))
    r=reason(text)
    if r: bad_entries.append((e,r))
if bad_entries:
    errors += [f'presentation slot remains in catalog: id={e.get("id")} nat#{e.get("natdex")} {e.get("name")}' for e,r in bad_entries]
else: lines.append('OK pmd_catalog contains no presentation-only species entries')

retired=CAT.get('retired_presentation_variants',[])
retired_ids={int(x['id']) for x in retired}
active_ids={int(e['id']) for e in entries}
overlap=retired_ids & active_ids
if overlap: errors.append('retired IDs still active in catalog: '+','.join(map(str,sorted(overlap))))
else: lines.append(f'OK retired presentation IDs are tombstones only: {len(retired_ids)} IDs')

# No evolution may ever target a retired art slot.
for e in CAT.get('extra_edges',[]):
    if int(e['base']) in retired_ids or int(e['target']) in retired_ids:
        errors.append(f'evolution references retired presentation id: {e}')
if not any('evolution references retired' in x for x in errors):
    lines.append('OK evolution graph contains no retired presentation IDs')

# The runtime must know how to repair old saves that were already on one.
if 'DEX_RETIRED_VARIANT_BASE' not in DEX or 'canonicalizeRetiredVariant' not in DEX:
    errors.append('retired-save migration map/helper missing from dex.h')
else: lines.append('OK retired-save migration map/helper generated')

# Regression for the reported Charizard issue: the only active extra Charizard
# form allowed in this build is the user-approved Mega Charizard X.
char_forms=[e for e in entries if int(e.get('natdex',0))==6 and e.get('kind')=='form' and e.get('enabled')]
nonmega=[e for e in char_forms if not e.get('mega')]
megax=[e for e in char_forms if e.get('mega')]
if nonmega:
    errors.append('non-canonical Charizard form(s) still active: '+repr([(e['id'],e.get('name'),e.get('form_name')) for e in nonmega]))
else: lines.append('OK Charizard has no AltColor/Alternate species branch')
if len(megax)!=1:
    errors.append('expected exactly one active Mega Charizard X entry, got '+repr([(e.get('id'),e.get('name')) for e in megax]))
else:
    mid=int(megax[0]['id'])
    mega_edge=next((e for e in CAT.get('extra_edges',[]) if int(e['base'])==6 and int(e['target'])==mid and int(e['level'])==70),None)
    if not mega_edge: errors.append(f'Mega Charizard X Lv.70 edge missing: 6->{mid}')
    else: lines.append(f'OK Charizard -> Mega Charizard X remains Lv.70: 6->{mid}')

OUT.write_text('\n'.join(lines + (['','ERRORS:']+['- '+x for x in errors] if errors else ['','canonical-form audit OK']))+'\n',encoding='utf-8')
print(OUT.read_text(encoding='utf-8'))
if errors: raise SystemExit('canonical-form audit failed')
