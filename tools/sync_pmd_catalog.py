#!/usr/bin/env python3
"""Synchronize TamaPoke with the CURRENT official PMDCollab SpriteCollab catalog.

Rules for this build:
- Start from TamaPoke KO v3.58 (1..809 + fixed Alola forms 810..827).
- Add every CURRENT PMDCollab base Pokemon/form that has a real behaviour sprite.
- Treat only canonical official/region/form-change forms as independent TamaPoke species.
- Never turn PMDCollab presentation-only slots (AltColor/Alternate/Cutscene/Beta) into species.
- Include ONLY the 36 user-approved Mega forms that currently have complete
  PMDCollab behaviour sprites; every other temporary battle gimmick remains excluded.
- Approved Mega forms are permanent Lv.70 TamaPoke evolutions and independent Dex entries.
- Preserve IDs append-only with tools/catalog_lock.json.
- New National-Dex base species use stable id = natdex + 18.
- Other forms use locked IDs from 1200 upward.
- Evolution triggers are simplified to level only; extra branches are emitted
  for the generic Eevee-style branch selector in pet.cpp.

The script writes dex.h, ko_species.h, noart.h and tools/pmd_catalog.json BEFORE
Arduino compilation.  It intentionally downloads metadata at GitHub Actions
runtime so a future PMDCollab upload can be picked up without guessing folders.
"""
from __future__ import annotations
import concurrent.futures as cf
import copy
import csv
import io
import json
import hashlib
import os
import re
import time
import urllib.error
import urllib.request
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parent
BASE_DEX = HERE/'dex_base_v358.h'
BASE_KO = HERE/'ko_species_base_v358.h'
LOCK_PATH = HERE/'catalog_lock.json'
CATALOG_PATH = HERE/'pmd_catalog.json'
REPORT_PATH = HERE/'pmd_catalog_report.txt'
AUDIT_PATH = HERE/'sprite_audit.json'
MAPPING_REPORT_PATH = HERE/'sprite_mapping_report.txt'
TRACKER_URL = 'https://raw.githubusercontent.com/PMDCollab/SpriteCollab/master/tracker.json'
POKE = 'https://pokeapi.co/api/v2'
SPECIES_CSV_URL = 'https://raw.githubusercontent.com/PokeAPI/pokeapi/master/data/v2/csv/pokemon_species.csv'
EVOLUTION_CSV_URL = 'https://raw.githubusercontent.com/PokeAPI/pokeapi/master/data/v2/csv/pokemon_evolution.csv'
USER_AGENT = 'TamaPoke-PMDCatalog/1.0 (+noncommercial classroom project)'
FORM_ID_START = 1200
EVOLUTION_LEVEL_CAP = 100
MEGA_EVOLVE_LEVEL = 70
# Leave room for future base species while keeping form IDs stable.
MAX_BASE_INTERNAL_BEFORE_FORMS = FORM_ID_START - 1

FIXED_ALOLA = {
    (19,'0001'):810,(20,'0001'):811,(26,'0001'):812,(27,'0001'):813,
    (28,'0001'):814,(37,'0001'):815,(38,'0001'):816,(50,'0001'):817,
    (51,'0001'):818,(52,'0001'):819,(53,'0001'):820,(74,'0001'):821,
    (75,'0001'):822,(76,'0001'):823,(88,'0001'):824,(89,'0001'):825,
    (103,'0001'):826,(105,'0001'):827,
}
NO_ART_BASE = {514,516,520,522,523,538,558,564,565,591,592,616,626,668,732,735,741,756,765}

# User-requested temporary removals. Keep their National Dex/internal slots reserved
# so they can be re-enabled later without renumbering saves, moves, or catalog IDs.
# #1020 Gouging Fire / 꿰뚫는화염 -> internal base ID 1038
# #1021 Raging Bolt / 날뛰는우레 -> internal base ID 1039
USER_DISABLED_NATDEX = {1020, 1021}

# Temporary/battle-powerup mechanics remain excluded by default.  Mega is
# deliberately left in this list so ONLY the explicit whitelist below can pass.
GIMMICK_WORDS = (
    'mega', 'gigantamax', 'g-max', 'gmax', 'dynamax', 'primal', 'eternamax',
    'ultra necrozma', 'ultra burst', 'ash-greninja', 'ash greninja', 'battle bond',
    'crowned sword', 'crowned shield', 'crowned', 'totem', 'terastal', 'stellar form',
    'stellar forme', 'tera form', 'tera forme', 'tera type',
)
# non-Pokemon utility/special repository slots that must not become hatchable entries.
SKIP_WORDS = ('missingno', 'substitute doll', 'manaphy egg')

# PMDCollab deliberately carries several presentation-only slots alongside
# canonical Pokemon/forms. AltColor is a recolor of the same Pokemon,
# Alternate is an alternative spritesheet, Cutscene is a scene-specific art
# slot, and Beta is a prototype resource. None of these are separate species
# in TamaPoke. Earlier catalogs accidentally assigned them form IDs, which
# could make normal evolutions branch into e.g. Charizard Altcolor and then
# lose the real Mega evolution.
PRESENTATION_VARIANT_WORDS = ('alternate', 'cutscene', 'beta')

def presentation_variant_reason(name:str):
    n=low(name).replace('_',' ').replace('-',' ')
    flat=re.sub(r'[^a-z]+','',n)
    if 'altcolor' in flat or 'altcolour' in flat:
        return 'AltColor recolor'
    toks=set(re.findall(r'[a-z0-9]+',n))
    for w in PRESENTATION_VARIANT_WORDS:
        if w in toks:
            return f'{w} presentation slot'
    return None

# v3.62.5: retain the Mega36 list while adding regional evolution routing guards
# verified on 2026-09-06.  This is intentionally a NATDEX whitelist rather than
# "allow every Mega": future SpriteCollab additions must not silently change a
# player's evolution graph.  Charizard and Mewtwo are restricted to the one
# animated branch requested by the user (X and Y respectively).
APPROVED_MEGA_NATDEX = {
    6, 65, 80, 94, 115, 142, 150, 208, 227, 229, 248, 282, 302, 303,
    308, 310, 323, 334, 354, 362, 380, 381, 384, 428, 448, 475, 491,
    530, 604, 670, 691, 718, 719, 780, 807, 978,
}
APPROVED_MEGA_COUNT = 36

def approved_mega_variant(nat:int, name:str):
    """Return '', 'X' or 'Y' for an approved Mega label, else None.

    The tracker has changed naming/layout conventions over time, so matching is
    based on the National Dex owner plus normalized label tokens.  X/Y/Z-style
    alternates are rejected unless explicitly whitelisted.
    """
    if nat not in APPROVED_MEGA_NATDEX:
        return None
    toks=re.findall(r'[a-z0-9]+',low(name).replace('_',' ').replace('-',' '))
    if 'mega' not in toks:
        return None
    letters={t for t in toks if t in ('x','y','z')}
    if nat == 6:
        return 'X' if 'x' in letters and 'y' not in letters and 'z' not in letters else None
    if nat == 150:
        return 'Y' if 'y' in letters and 'x' not in letters and 'z' not in letters else None
    # Normal Mega only: e.g. Mega Lucario is approved, Mega Lucario Z is not.
    if letters:
        return None
    return ''

def is_approved_mega(nat:int, name:str)->bool:
    return approved_mega_variant(nat,name) is not None

REGIONAL_TOKENS = {
    'alola':6, 'alolan':6,
    'galar':7, 'galarian':7,
    'hisui':8, 'hisuian':8,
    'paldea':9, 'paldean':9,
}

# Some later-generation species officially evolve ONLY from a regional/form
# variant of an older National-Dex species. PokeAPI's species CSV necessarily
# stores only the National-Dex parent (e.g. #211 -> #904), so blindly mirroring
# that edge would create Normal Qwilfish -> Overqwil and leave Hisuian
# Qwilfish with no evolution. Route these descendants through the actual form
# entry instead. Basculin is the one special case whose Hisui-origin source is
# named White-Striped rather than Hisui in upstream form metadata.
REGIONAL_DESCENDANT_SOURCES = {
    # Galar
    862: {'parent_nat':264, 'tag':'galar'},   # Galarian Linoone -> Obstagoon
    863: {'parent_nat':52,  'tag':'galar'},   # Galarian Meowth -> Perrserker
    864: {'parent_nat':222, 'tag':'galar'},   # Galarian Corsola -> Cursola
    865: {'parent_nat':83,  'tag':'galar'},   # Galarian Farfetch'd -> Sirfetch'd
    866: {'parent_nat':122, 'tag':'galar'},   # Galarian Mr. Mime -> Mr. Rime
    867: {'parent_nat':562, 'tag':'galar'},   # Galarian Yamask -> Runerigus
    # Hisui
    902: {'parent_nat':550, 'tag':'hisui'},       # White-Striped Basculin -> Basculegion
    903: {'parent_nat':215, 'tag':'hisui'},   # Hisuian Sneasel -> Sneasler
    904: {'parent_nat':211, 'tag':'hisui'},   # Hisuian Qwilfish -> Overqwil
    # Paldea (same ambiguity class; keep it correct while auditing region forms)
    980: {'parent_nat':194, 'tag':'paldea'},  # Paldean Wooper -> Clodsire
}
REGION_NAMES = ['KANTO','JOHTO','HOENN','SINNOH','UNOVA','KALOS','ALOLA','GALAR','HISUI','PALDEA','ALL']
TYPE_ENUM = {
    'normal':'T_NORMAL','fire':'T_FIRE','water':'T_WATER','electric':'T_ELECTRIC',
    'grass':'T_GRASS','ice':'T_ICE','fighting':'T_FIGHTING','poison':'T_POISON',
    'ground':'T_GROUND','flying':'T_FLYING','psychic':'T_PSYCHIC','bug':'T_BUG',
    'rock':'T_ROCK','ghost':'T_GHOST','dragon':'T_DRAGON','dark':'T_DARK',
    'steel':'T_STEEL','fairy':'T_FAIRY',
}
ACCENT = {
    'normal':'0x8C4D','fire':'0xEA87','water':'0x4C98','electric':'0xBCA1','grass':'0x3C49',
    'ice':'0x4DB8','fighting':'0xA2A5','poison':'0x7CC4','ground':'0xB447','flying':'0x8C4D',
    'psychic':'0xD28F','bug':'0x7CC4','rock':'0x9407','ghost':'0x6AD3','dragon':'0x5A98',
    'dark':'0x5A47','steel':'0x6BF1','fairy':'0xC333',
}
BIOME = {
    'water':1,'dragon':1,
    'grass':2,'bug':2,'poison':2,'dark':2,
    'fire':3,
    'rock':4,'ground':4,'steel':4,'fighting':4,
    'ice':5,
}


def get_json(url:str, retries:int=4, timeout:int=90):
    last=None
    for i in range(retries):
        try:
            req=urllib.request.Request(url,headers={'User-Agent':USER_AGENT,'Accept':'application/json'})
            with urllib.request.urlopen(req,timeout=timeout) as r:
                return json.loads(r.read().decode('utf-8'))
        except Exception as e:
            last=e
            if i+1<retries: time.sleep(1.5*(i+1))
    raise RuntimeError(f'GET failed {url}: {last}')


def get_text(url:str, retries:int=4, timeout:int=90):
    last=None
    for i in range(retries):
        try:
            req=urllib.request.Request(url,headers={'User-Agent':USER_AGENT,'Accept':'text/plain,*/*'})
            with urllib.request.urlopen(req,timeout=timeout) as r:
                return r.read().decode('utf-8-sig')
        except Exception as e:
            last=e
            if i+1<retries: time.sleep(1.5*(i+1))
    raise RuntimeError(f'GET failed {url}: {last}')


def load_evolution_csv():
    """Return official species IDs and parent->child level-only evolution edges.

    PokeAPI's pokemon_species.csv exposes evolves_from_species_id for every
    official species. pokemon_evolution.csv supplies minimum_level when the
    original game has one. All other methods are deliberately normalized to
    level 30 for this TamaPoke build.
    """
    species_rows=list(csv.DictReader(io.StringIO(get_text(SPECIES_CSV_URL))))
    official={}
    for r in species_rows:
        try: n=int(r['id'])
        except Exception: continue
        official[n]=r
    evo_rows=list(csv.DictReader(io.StringIO(get_text(EVOLUTION_CSV_URL))))
    evo_level={}
    for r in evo_rows:
        try: child=int(r.get('evolved_species_id') or 0)
        except Exception: continue
        if child<1: continue
        # Prefer the default evolution row and a real minimum level when one
        # exists; otherwise every special method becomes level 30.
        try: ml=int(r.get('minimum_level') or 0)
        except Exception: ml=0
        val=ml if ml>0 else 30
        old=evo_level.get(child)
        if old is None or (ml>0 and old==30): evo_level[child]=val
    edges={}
    for child,r in official.items():
        try: parent=int(r.get('evolves_from_species_id') or 0)
        except Exception: parent=0
        if parent>0: edges[(parent,child)]=evo_level.get(child,30)
    return official,edges


def has_sprite(node:dict)->bool:
    files=node.get('sprite_files')
    if isinstance(files,dict): return bool(files)
    if isinstance(files,(list,tuple,set)): return len(files)>0
    return bool(files)


def base_sprite_path(nat:int,node:dict):
    """Return the real PMDCollab path for a species' default behavior sprite.

    Most species store AnimData.xml at sprite/NNNN itself. A few tracker nodes
    are grouping-only and keep the default variant under 0000; accepting that
    layout avoids incorrectly disabling a Pokemon that is visibly present on
    the PMDCollab site.
    """
    root=f'{nat:04d}'
    if has_sprite(node): return root
    n0=(node.get('subgroups') or {}).get('0000')
    if isinstance(n0,dict):
        if has_sprite(n0): return f'{root}/0000'
        # A grouping-only species may keep its true default under a nested
        # 0000/Normal node.  Never fall through to a sibling presentation slot
        # (AltColor/Alternate/Cutscene/Beta), battle gimmick, regional form, or
        # arbitrary named child.  Returning None is safer than silently binding
        # the base dex number to the wrong visual resource.
        for ck,ch in sorted((n0.get('subgroups') or {}).items()):
            if not isinstance(ch,dict): continue
            raw_name=clean(ch.get('name'))
            nm=low(raw_name)
            if 'shiny' in nm or 'female' in nm or 'male' in nm: continue
            if presentation_variant_reason(raw_name): continue
            if is_gimmick(raw_name): continue
            if any(w in nm for w in ('alola','galar','hisui','paldea')): continue
            is_default = ck == '0000' or not nm or any(w in nm for w in ('normal','default','base'))
            if is_default and has_sprite(ch): return f'{root}/0000/{ck}'
    return None

def find_shiny_path(node:dict, path:str, base_root:bool=False):
    """Find the tracker child that represents the shiny recolor.

    At a species root, 0001 can itself be an alternate form, so shiny lookup is
    constrained to the normal-variant 0000 subtree. At an already selected form
    root we may search its children directly.
    """
    roots=[]
    if base_root:
        n0=(node.get('subgroups') or {}).get('0000')
        if isinstance(n0,dict): roots=[('0000',n0)]
    else:
        roots=list((node.get('subgroups') or {}).items())
    def walk(k,n,p):
        nm=low(n.get('name'))
        if 'shiny' in nm and has_sprite(n): return p
        for ck,ch in sorted((n.get('subgroups') or {}).items()):
            if isinstance(ch,dict):
                got=walk(ck,ch,f'{p}/{ck}')
                if got:return got
        return None
    for k,n in roots:
        got=walk(k,n,f'{path}/{k}')
        if got:return got
    return None


def clean(s): return re.sub(r'\s+',' ',str(s or '')).strip()

def low(s): return clean(s).casefold()

def is_gimmick(name:str)->bool:
    n=low(name).replace('_',' ').replace('-','-')
    return any(w in n for w in GIMMICK_WORDS) or any(w in n for w in SKIP_WORDS)


def prune_presentation_lock(lock:dict, presentation_excluded:list[dict]):
    """Remove non-species PMDCollab slots without reusing their numeric IDs."""
    keys={f"{f['natdex']:04d}/{f['form_key']}" for f in presentation_excluded}
    retired=[]
    forms=lock.setdefault('forms',{})
    for lk,rec in list(forms.items()):
        reason=presentation_variant_reason(rec.get('label',''))
        if lk in keys and not reason:
            reason='presentation-only PMDCollab slot'
        if not reason:
            continue
        retired.append({'lock_key':lk,'id':int(rec['id']),'natdex':int(rec['natdex']),
                        'form_key':rec.get('form_key',''),'label':rec.get('label',''),
                        'reason':reason})
        del forms[lk]
    return retired


def approved_mega_forms(nat:int, node:dict):
    """Recursively find approved Mega nodes with real behaviour sprites.

    Some recent SpriteCollab Megas live below an ordinary-form grouping (for
    example Tatsugiri/Curly).  The legacy direct-form scanner intentionally
    stops at one level, so Megas need this narrow recursive pass.
    """
    out=[]
    root_name=clean(node.get('name'))
    def walk(cur:dict, keys:list[str], labels:list[str]):
        for key,sub in sorted((cur.get('subgroups') or {}).items()):
            if not isinstance(sub,dict):
                continue
            nm=clean(sub.get('name'))
            if not nm:
                continue
            if any(x in low(nm) for x in ('shiny','female','male')):
                continue
            if presentation_variant_reason(nm):
                # Never discover a Mega through an alternate-art/recolor tree.
                continue
            new_keys=keys+[str(key)]
            new_labels=labels+[nm]
            full=' '.join([root_name]+new_labels)
            path=f"{nat:04d}/" + '/'.join(new_keys)
            if has_sprite(sub) and is_approved_mega(nat,full):
                # Keep enough of the hierarchy to distinguish Curly Mega etc.
                label=' '.join(new_labels)
                out.append({'excluded':False,'natdex':nat,'form_key':'/'.join(new_keys),
                            'pmd_path':path,'form_name':label,'full_name':full,
                            'shiny_path':find_shiny_path(sub,path),'mega':True})
                # Once the behaviour-sprite root is found, its descendants are
                # variants such as Shiny/Normal rather than additional species.
                continue
            walk(sub,new_keys,new_labels)
    walk(node,[],[])
    return out


def direct_forms(nat:int,node:dict):
    """Return direct official form folders with actual sprite files.

    SpriteCollab's root species has normal art; subgroup 0000 carries normal
    variants (shiny/gender), while 0001+ are named forms. We intentionally do
    not turn shiny/gender children into species.
    """
    out=[]
    for key,sub in sorted((node.get('subgroups') or {}).items()):
        if str(key)=='0000' or not isinstance(sub,dict):
            continue
        name=clean(sub.get('name'))
        if not name: continue
        path=f'{nat:04d}/{key}'
        pres_reason=presentation_variant_reason(name)
        if pres_reason:
            # Keep it in the excluded report so an existing catalog_lock entry
            # can be retired, but never expose it as a species/resource.
            out.append({'excluded':True,'natdex':nat,'form_key':str(key),'pmd_path':path,
                        'form_name':name,'full_name':f"{clean(node.get('name'))} {name}",
                        'shiny_path':None,'reason':pres_reason,'presentation':True})
            continue
        # Prefer art exactly at the form root. If it is a grouping node, choose
        # the first non-shiny/non-gender child that owns real behaviour files.
        chosen=sub
        chosen_path=path
        if not has_sprite(chosen):
            for ck,child in sorted((sub.get('subgroups') or {}).items()):
                if not isinstance(child,dict): continue
                cn=low(child.get('name'))
                if any(x in cn for x in ('shiny','female','male')): continue
                child_full=f"{clean(node.get('name'))} {name} {clean(child.get('name'))}"
                # Nested gimmicks are handled by the narrow recursive Mega pass;
                # never let a grouping node masquerade as its Mega/Shiny child.
                if is_gimmick(child_full) or presentation_variant_reason(child_full): continue
                if has_sprite(child):
                    chosen=child; chosen_path=f'{path}/{ck}'; break
        if not has_sprite(chosen): continue
        full=name if low(node.get('name')) in low(name) else f"{clean(node.get('name'))} {name}"
        if is_gimmick(full) and not is_approved_mega(nat,full):
            out.append({'excluded':True,'natdex':nat,'form_key':str(key),'pmd_path':chosen_path,
                        'form_name':name,'full_name':full,'shiny_path':find_shiny_path(chosen,chosen_path),'reason':'battle gimmick'})
        else:
            mega=is_approved_mega(nat,full)
            out.append({'excluded':False,'natdex':nat,'form_key':str(key),'pmd_path':chosen_path,
                        'form_name':name,'full_name':full,'shiny_path':find_shiny_path(chosen,chosen_path),'mega':mega})
    return out


def gen_region(nat:int)->int:
    if nat<=151:return 0
    if nat<=251:return 1
    if nat<=386:return 2
    if nat<=493:return 3
    if nat<=649:return 4
    if nat<=721:return 5
    if nat<=809:return 6
    if nat<=898:return 7
    if nat<=905:return 8
    return 9


def form_region(name:str,nat:int)->int:
    # White-Striped Basculin is a Hisui-origin form, but PMDCollab labels it
    # simply 'Basculin White' rather than including the word Hisui. Keep it in
    # the Hisui regional pool so it can hatch there and evolve to Basculegion.
    if nat==550 and 'white' in low(name): return 8
    toks=re.findall(r'[a-z]+',low(name))
    for t in toks:
        if t in REGIONAL_TOKENS:return REGIONAL_TOKENS[t]
    return gen_region(nat)


def regional_tag(name:str,nat:int|None=None):
    n=low(name)
    if nat==550 and 'white' in n: return 'hisui'
    for tag in ('alola','galar','hisui','paldea'):
        if tag in n or (tag+'n') in n:
            return tag
    return None


def parse_base_entries(text:str):
    # Extract positional DEX_TBL rows 0..827. We only need values for inheritance.
    start=text.index('static const DexEntry DEX_TBL')
    block=text[start:text.index('\n};',start)]
    rows=[]
    for line in block.splitlines()[1:]:
        if '{' not in line: continue
        m=re.search(r'\{\s*"([^"]*)"\s*,\s*([0-9]+)\s*,\s*([0-9]+)\s*,\s*([^,]+),\s*([^,]+),\s*([0-9]+)\s*,\s*([0-9]+)\s*,\s*([0-9]+)\s*,\s*([0-9]+)\s*,\s*([0-9]+)\s*,\s*([0-9]+)\s*,\s*([0-9]+)\s*,\s*([^,]+)\s*,\s*([^}\s]+)',line)
        if not m: continue
        rows.append({
            'name':m.group(1),'evolvesTo':int(m.group(2)),'evolveLevel':int(m.group(3)),
            'rarity':m.group(4).strip(),'accent':m.group(5).strip(),'hp':int(m.group(6)),
            'atk':int(m.group(7)),'def':int(m.group(8)),'spe':int(m.group(9)),
            'spa':int(m.group(10)),'spd':int(m.group(11)),'biome':int(m.group(12)),
            'type1':m.group(13).strip(),'type2':m.group(14).strip()
        })
    # v3.58 row 0 is a short placeholder without type columns, so the
    # regular expression above intentionally parses only real rows 1..827.
    if len(rows)==827:
        rows.insert(0,{
            'name':'?','evolvesTo':0,'evolveLevel':0,'rarity':'0','accent':'0x2946',
            'hp':50,'atk':50,'def':50,'spe':50,'spa':50,'spd':50,'biome':0,
            'type1':'T_NONE','type2':'T_NONE'
        })
    if len(rows)<828:
        raise RuntimeError(f'Could not parse v3.58 DEX_TBL: {len(rows)} rows')
    return rows


def parse_ko_names(text:str):
    vals=[]
    for line in text.splitlines():
        m=re.match(r'\s*"(.*)"\s*,?\s*(?://.*)?$',line)
        if m: vals.append(m.group(1))
    if len(vals)<828: raise RuntimeError(f'Could not parse KO names: {len(vals)}')
    return vals


def normalize_tokens(s:str):
    s=low(s).replace('alolan','alola').replace('galarian','galar').replace('hisuian','hisui').replace('paldean','paldea')
    return [x for x in re.findall(r'[a-z0-9]+',s) if x not in ('form','forme','mode','style')]


def variety_score(base_slug:str, form_name:str, variety_slug:str):
    bt=set(normalize_tokens(base_slug)); ft=set(normalize_tokens(form_name)); vt=set(normalize_tokens(variety_slug))
    extras=vt-bt
    if not extras: return -100
    return len(ft & extras)*5 - len(extras-ft)


def ko_form_name(base_ko:str, form_name:str)->str:
    n=clean(form_name)
    l=low(n)
    if 'alola' in l: return f'알로라 {base_ko}'
    if 'galar' in l: return f'가라르 {base_ko}'
    if 'hisui' in l: return f'히스이 {base_ko}'
    if 'paldea' in l: return f'팔데아 {base_ko}'
    repl={
      'Attack':'공격','Defense':'방어','Speed':'스피드','Sky':'스카이','Origin':'오리진',
      'Therian':'영물','Incarnate':'화신','Heat':'히트','Wash':'워시','Frost':'프로스트',
      'Fan':'팬','Mow':'커트','Dusk':'황혼','Midnight':'한밤중','Midday':'한낮',
      'School':'군집','Solo':'단독','Hero':'마이티','Zero':'나이브','Hangry':'배고픔',
      'Ice Rider':'백마','Shadow Rider':'흑마','Bloodmoon':'붉은달','Three-Segment':'세마디',
      'Two-Segment':'두마디','Roaming':'떠돌이','Teal':'벽록','Wellspring':'우물',
      'Hearthflame':'화덕','Cornerstone':'주춧돌','Male':'수컷','Female':'암컷'
    }
    suffix=n
    for a,b in repl.items(): suffix=re.sub(r'\b'+re.escape(a)+r'\b',b,suffix,flags=re.I)
    suffix=re.sub(r'\b(Form|Forme|Mode|Style)\b','',suffix,flags=re.I).strip(' -_()')
    return base_ko if not suffix else f'{base_ko} {suffix}'


def fetch_species(nat:int):
    d=get_json(f'{POKE}/pokemon-species/{nat}')
    purl=next((v['pokemon']['url'] for v in d.get('varieties',[]) if v.get('is_default')),None)
    if not purl and d.get('varieties'):purl=d['varieties'][0]['pokemon']['url']
    p=get_json(purl) if purl else None
    return nat,d,p


def meta_from_pokemon(species:dict,pokemon:dict, fallback=None):
    if not pokemon:
        if fallback:return copy.deepcopy(fallback)
        raise RuntimeError('no pokemon metadata')
    stats={x['stat']['name']:int(x['base_stat']) for x in pokemon.get('stats',[])}
    types=[x['type']['name'] for x in sorted(pokemon.get('types',[]),key=lambda z:z['slot'])]
    t1=types[0] if types else 'normal'; t2=types[1] if len(types)>1 else None
    cap=int(species.get('capture_rate',45) or 45)
    rarity='R_LEGENDARIO' if species.get('is_legendary') or species.get('is_mythical') else ('R_RARO' if cap<=45 else 'R_COMUN')
    return {
      'hp':stats.get('hp',50),'atk':stats.get('attack',50),'def':stats.get('defense',50),
      'spe':stats.get('speed',50),'spa':stats.get('special-attack',50),'spd':stats.get('special-defense',50),
      'type1':TYPE_ENUM.get(t1,'T_NORMAL'),'type2':TYPE_ENUM.get(t2,'T_NONE') if t2 else 'T_NONE',
      'accent':ACCENT.get(t1,'0x8C4D'),'biome':BIOME.get(t1,0),'rarity':rarity,
    }


def evo_pairs(chain:dict):
    out=[]
    def walk(node):
        p=node.get('species',{}).get('name')
        purl=node.get('species',{}).get('url','')
        pm=re.search(r'/pokemon-species/(\d+)/?$',purl)
        pn=int(pm.group(1)) if pm else None
        for ch in node.get('evolves_to',[]):
            curl=ch.get('species',{}).get('url','')
            cm=re.search(r'/pokemon-species/(\d+)/?$',curl)
            cn=int(cm.group(1)) if cm else None
            lvl=0
            for det in ch.get('evolution_details',[]):
                ml=det.get('min_level')
                if isinstance(ml,int) and ml>0: lvl=ml; break
            if pn and cn: out.append((pn,cn,lvl or 30))
            walk(ch)
    walk(chain.get('chain',{}))
    return out


def internal_base_id(nat:int)->int:
    return nat if nat<=809 else nat+18


def escape_c(s:str)->str:
    return s.replace('\\','\\\\').replace('"','\\"')


def main():
    base_text=BASE_DEX.read_text(encoding='utf-8')
    ko_text=BASE_KO.read_text(encoding='utf-8')
    base_rows=parse_base_entries(base_text)
    base_ko=parse_ko_names(ko_text)
    tracker=get_json(TRACKER_URL,timeout=180)
    official_species,base_edges=load_evolution_csv()
    tracker_nums=sorted(int(k) for k in tracker.keys() if str(k).isdigit() and 1<=int(k)<=2000)
    official_ids=sorted(n for n in official_species if 1<=n<=2000)
    current_max=max(official_ids,default=809)
    # Only official National-Dex slots are considered base Pokemon. Repository
    # utility/special numeric slots are ignored even when they look like dex IDs.
    nums=sorted(set(tracker_nums) & set(official_ids))
    if current_max+18 >= FORM_ID_START:
        raise RuntimeError(f'National Dex {current_max} collides with reserved form IDs at {FORM_ID_START}; migration required')

    # Fetch PokeAPI metadata only for post-809 base species and owners of
    # alternate forms. Evolution topology itself comes from the small GitHub CSVs.
    form_owners=set()
    raw_forms=[]; excluded=[]; presentation_excluded=[]
    seen_form_paths=set()
    for nat in nums:
        node=tracker.get(f'{nat:04d}') or tracker.get(str(nat))
        if not isinstance(node,dict): continue
        found=direct_forms(nat,node) + approved_mega_forms(nat,node)
        for f in found:
            k=(f['natdex'],f['pmd_path'])
            if k in seen_form_paths:
                continue
            seen_form_paths.add(k)
            if f['excluded']:
                excluded.append(f)
                if f.get('presentation'): presentation_excluded.append(f)
            else:
                raw_forms.append(f); form_owners.add(nat)

    # There is exactly one approved Mega target per National-Dex owner.  If a
    # tracker layout exposes the same sprite through two grouping paths, keep
    # the shallowest canonical root.  Conversely, fail loudly if one of the 36
    # verified sprites disappears upstream instead of silently changing saves.
    ordinary_forms=[f for f in raw_forms if not f.get('mega')]
    mega_by_nat={}
    for f in (x for x in raw_forms if x.get('mega')):
        old=mega_by_nat.get(f['natdex'])
        if old is None or (f['pmd_path'].count('/'),len(f['pmd_path'])) < (old['pmd_path'].count('/'),len(old['pmd_path'])):
            mega_by_nat[f['natdex']]=f
    missing_mega=sorted(APPROVED_MEGA_NATDEX-set(mega_by_nat))
    if missing_mega:
        raise RuntimeError('approved Mega behaviour sprite missing from PMDCollab tracker: '+','.join(map(str,missing_mega)))
    if len(mega_by_nat) != APPROVED_MEGA_COUNT:
        raise RuntimeError(f'approved Mega count mismatch: {len(mega_by_nat)} != {APPROVED_MEGA_COUNT}')
    raw_forms=ordinary_forms+[mega_by_nat[n] for n in sorted(mega_by_nat)]
    form_owners.update(mega_by_nat)

    need_species=sorted(set([n for n in official_ids if 809<n<=current_max]) | form_owners)
    species_data={}; pokemon_data={}; failures=[]
    def one(n):
        try:return fetch_species(n)
        except Exception as e:return (n,e,None)
    with cf.ThreadPoolExecutor(max_workers=12) as ex:
        for n,d,p in ex.map(one,need_species):
            if isinstance(d,Exception): failures.append(f'PokeAPI #{n}: {d}')
            else: species_data[n]=d; pokemon_data[n]=p
    if any(n>809 and n<=current_max and n not in species_data for n in need_species):
        REPORT_PATH.write_text('\n'.join(failures)+'\n',encoding='utf-8')
        raise RuntimeError('PokeAPI metadata missing for new National-Dex species; see report')

    # lock: fixed old Alola IDs + append-only form IDs.
    if LOCK_PATH.is_file(): lock=json.loads(LOCK_PATH.read_text(encoding='utf-8'))
    else: lock={'schema':1,'next_form_id':FORM_ID_START,'forms':{}}
    lock.setdefault('schema',1); lock.setdefault('next_form_id',FORM_ID_START); lock.setdefault('forms',{})
    for (nat,key),iid in FIXED_ALOLA.items():
        lk=f'{nat:04d}/{key}'
        lock['forms'].setdefault(lk,{'id':iid,'natdex':nat,'form_key':key,'label':'Alola'})

    # v3.62.5 canonical-form cleanup. Purge presentation-only IDs that older
    # catalogs accidentally persisted. next_form_id is deliberately NOT
    # decreased, so retired IDs are never reused for a different Pokemon.
    retired_forms=prune_presentation_lock(lock,presentation_excluded)

    next_id=max(int(lock.get('next_form_id',FORM_ID_START)),FORM_ID_START)
    discovered={f"{f['natdex']:04d}/{f['form_key']}":f for f in raw_forms}
    for lk,f in sorted(discovered.items(),key=lambda kv:(kv[1]['natdex'],kv[1]['form_key'])):
        if lk not in lock['forms']:
            while next_id in range(1,current_max+19) or next_id in FIXED_ALOLA.values(): next_id+=1
            lock['forms'][lk]={'id':next_id,'natdex':f['natdex'],'form_key':f['form_key'],'label':f['form_name']}
            next_id+=1
        else:
            lock['forms'][lk]['label']=f['form_name']
            lock['forms'][lk]['natdex']=f['natdex']
            lock['forms'][lk]['form_key']=f['form_key']
    lock['next_form_id']=next_id

    # Parent/evolution topology is sourced from PokeAPI's versioned CSV files.
    # This covers split evolutions in every generation, including branches that
    # the original v3.58 single evolvesTo field could not represent.
    parent_of={b:a for (a,b),lvl in base_edges.items()}

    # Map CURRENT enabled regional forms by (National Dex, region tag) to the
    # independent TamaPoke species ID. Keep a second all/locked map only for
    # diagnostics: evolution routing must never point at a stale disabled form.
    form_entries=[]
    by_region_form={}
    by_region_form_all={}
    for lk,rec in sorted(lock['forms'].items(),key=lambda kv:int(kv[1]['id'])):
        nat=int(rec['natdex']); iid=int(rec['id']); f=discovered.get(lk)
        enabled=bool(f) and nat not in USER_DISABLED_NATDEX
        label=(f or {}).get('form_name') or rec.get('label','Form')
        full=(f or {}).get('full_name') or f"{nat} {label}"
        region=form_region(full,nat)
        tag=regional_tag(full,nat)
        if tag:
            by_region_form_all[(nat,tag)]=iid
            if enabled: by_region_form[(nat,tag)]=iid
        form_entries.append({'lock_key':lk,'id':iid,'natdex':nat,'form_key':rec.get('form_key',''),
                             'form_name':label,'full_name':full,'pmd_path':(f or {}).get('pmd_path'),
                             'shiny_path':(f or {}).get('shiny_path'), 'enabled':enabled,'region':region,'regional_tag':tag,
                             'mega':bool((f or {}).get('mega'))})

    # Build metadata for all new base slots and forms.
    new_entries={}
    # 1..809 keep the original TamaPoke art availability.  The fixed Alola
    # form IDs 810..827 are enabled ONLY when the CURRENT PMDCollab tracker
    # actually exposes their behaviour sprite; this prevents a phantom form
    # from remaining hatchable after an upstream removal.
    enabled_ids=(set(range(1,810)) - NO_ART_BASE) - USER_DISABLED_NATDEX
    # New base species 810..current_max have deterministic ids.
    for nat in range(810,current_max+1):
        iid=internal_base_id(nat)
        d=species_data.get(nat)
        if not d:
            # only possible for a number absent from PokeAPI; keep a disabled stable slot
            new_entries[iid]={'id':iid,'natdex':nat,'name':f'UNAVAILABLE-{nat}','ko_name':f'미지원 {nat}',
                              'enabled':False,'region':gen_region(nat),'rarity':'R_EVO','accent':'0x8C4D',
                              'hp':1,'atk':1,'def':1,'spe':1,'spa':1,'spd':1,'biome':0,'type1':'T_NORMAL','type2':'T_NONE',
                              'pmd_path':None,'kind':'base'}
            continue
        eng=next((x['name'] for x in d.get('names',[]) if x['language']['name']=='en'),d['name']).upper().replace('’',"'")
        ko=next((x['name'] for x in d.get('names',[]) if x['language']['name']=='ko'),d['name'])
        meta=meta_from_pokemon(d,pokemon_data.get(nat))
        node=tracker.get(f'{nat:04d}') or tracker.get(str(nat)) or {}
        base_path=base_sprite_path(nat,node)
        user_disabled = nat in USER_DISABLED_NATDEX
        enabled=bool(base_path) and not user_disabled
        parent=parent_of.get(nat)
        if parent: meta['rarity']='R_EVO'
        entry={'id':iid,'natdex':nat,'name':eng,'ko_name':ko,'enabled':enabled,'region':gen_region(nat),
               'pmd_path':base_path,'shiny_path':find_shiny_path(node,f'{nat:04d}',base_root=True) if enabled else None,
               'disabled_reason':('user-disabled reserved slot' if user_disabled else None),'kind':'base',**meta}
        new_entries[iid]=entry
        if enabled: enabled_ids.add(iid)

    # form metadata inherits base or exact PokeAPI variety when a good match exists.
    for f in form_entries:
        iid=f['id']; nat=f['natdex']
        if iid<=827: # fixed Alola rows/names already exist in v3.58; only catalog/pack metadata is needed.
            if f['enabled']: enabled_ids.add(iid)
            continue
        d=species_data.get(nat)
        if nat<=809:
            b=base_rows[nat] if nat < len(base_rows) else base_rows[1]
            meta={k:b[k] for k in ('rarity','accent','hp','atk','def','spe','spa','spd','biome','type1','type2')}
            baseko=base_ko[nat] if nat < len(base_ko) else str(nat)
            basename=b['name']
        else:
            bentry=new_entries[internal_base_id(nat)]
            meta={k:bentry[k] for k in ('rarity','accent','hp','atk','def','spe','spa','spd','biome','type1','type2')}
            baseko=bentry['ko_name']; basename=bentry['name']
        # Try specific PokeAPI variety for accurate regional/form stats/types.
        if d and f['enabled']:
            vars=d.get('varieties',[]); best=None; bestsc=-100
            for v in vars:
                if v.get('is_default'):continue
                sc=variety_score(d['name'],f['form_name'],v['pokemon']['name'])
                if sc>bestsc:bestsc=sc;best=v
            if best is not None and bestsc>=3:
                try:
                    pv=get_json(best['pokemon']['url'])
                    meta=meta_from_pokemon(d,pv,meta)
                except Exception as e: failures.append(f"variety #{nat} {f['form_name']}: {e}")
        # All alternate forms are obtained by evolution/branch, except the first stage of a regional line.
        tag=f['regional_tag']; parent_nat=parent_of.get(nat)
        hatchable_regional=bool(tag and not parent_nat)
        if not hatchable_regional: meta['rarity']='R_EVO'
        if f.get('mega'):
            variant=approved_mega_variant(nat,f['full_name']) or ''
            eng=(f'MEGA {basename}' + (f' {variant}' if variant else '')).upper()
            ko=f'm.{baseko}' + variant
        else:
            eng=(basename+' '+f['form_name']).upper()
            ko=ko_form_name(baseko,f['form_name'])
        new_entries[iid]={'id':iid,'natdex':nat,'name':eng,'ko_name':ko,'enabled':f['enabled'],'region':f['region'],
                          'pmd_path':f['pmd_path'],'shiny_path':f.get('shiny_path'),
                          'disabled_reason':('user-disabled reserved slot' if nat in USER_DISABLED_NATDEX else None),
                          'kind':'form','form_key':f['form_key'],
                          'form_name':f['form_name'],'mega':bool(f.get('mega')),**meta}
        if f['enabled']:enabled_ids.add(iid)

    def resolve_regional_descendant_source(target_nat:int):
        """Resolve the independent form ID that may evolve into target_nat.

        This is intentionally form-aware rather than National-Dex-only. If an
        expected current form disappears or its tracker label changes, fail the
        catalog build instead of silently restoring a biologically wrong edge.
        """
        spec=REGIONAL_DESCENDANT_SOURCES.get(int(target_nat))
        if not spec: return None
        parent=int(spec['parent_nat'])
        tag=spec.get('tag')
        if tag:
            iid=by_region_form.get((parent,tag))
            if iid: return iid
            stale=by_region_form_all.get((parent,tag))
            if stale:
                raise RuntimeError(f'regional evolution source disabled: nat#{parent} {tag} id {stale} -> nat#{target_nat}')
            raise RuntimeError(f'regional evolution source missing: nat#{parent} {tag} -> nat#{target_nat}')
        need=set(spec.get('tokens') or ())
        matches=[]
        for f in form_entries:
            if not f.get('enabled') or int(f.get('natdex',0))!=parent: continue
            toks=set(normalize_tokens((f.get('form_name') or '')+' '+(f.get('full_name') or '')))
            if need.issubset(toks): matches.append(int(f['id']))
        if len(matches)!=1:
            raise RuntimeError(f'form-specific evolution source ambiguous/missing: nat#{parent} tokens={sorted(need)} -> nat#{target_nat}; matches={matches}')
        return matches[0]

    # Evolution edges. Keep v3.58 normal edges untouched; add all new relations here.
    extra_edges=[]
    def add_edge(a,b,lvl=30,reason='extra'):
        if a<1 or b<1 or a==b:return
        tup=(int(a),int(b),max(2,min(EVOLUTION_LEVEL_CAP,int(lvl or 30))),reason)
        if not any(x[0]==tup[0] and x[1]==tup[1] for x in extra_edges): extra_edges.append(tup)
    # Every official base evolution is mirrored in the generic branch table.
    # Existing v3.58 direct evolvesTo targets are deduplicated at runtime; the
    # extra table is what restores Bellossom/Slowking/Gallade/etc. branches and
    # extends the same behavior through Galar/Hisui/Paldea.
    for (a,b),lvl in sorted(base_edges.items()):
        ai=internal_base_id(a); bi=internal_base_id(b)
        target_ok = (b<=809 and b not in NO_ART_BASE) or (bi in new_entries and new_entries[bi]['enabled'])
        if not target_ok:
            continue
        if b in REGIONAL_DESCENDANT_SOURCES:
            # PokeAPI says only 'old National-Dex parent -> new species'; route
            # through the real regional/form source instead of the normal form.
            src=resolve_regional_descendant_source(b)
            add_edge(src,bi,lvl,'regional-form descendant evolution')
        else:
            add_edge(ai,bi,lvl,'National-Dex evolution')
    # Form relations: regional lines follow same-region parents; other form changes branch from base.
    for f in form_entries:
        if not f['enabled']: continue
        iid=f['id']; nat=f['natdex']; tag=f['regional_tag']; parent_nat=parent_of.get(nat)
        if f.get('mega'):
            # TamaPoke treats approved Mega forms as permanent late-game level
            # evolutions rather than a temporary battle mechanic.
            add_edge(internal_base_id(nat),iid,MEGA_EVOLVE_LEVEL,'approved Mega evolution')
        elif tag:
            if parent_nat:
                parent=by_region_form.get((parent_nat,tag),internal_base_id(parent_nat))
                # Use normal parent's level when available, else simplified 30.
                lvl=base_edges.get((parent_nat,nat),30)
                add_edge(parent,iid,lvl,f'{tag} regional evolution')
            # first-stage regional forms hatch; no base->form transform.
        elif iid>827:
            # Treat an official alternate form as its own TamaPoke species.
            # If the species itself has a pre-evolution (Rockruff -> Lycanroc,
            # Burmy -> Wormadam, etc.), the alternate form is another branch
            # FROM that pre-evolution rather than a second evolution after the
            # default form.  Species with no pre-evolution (Rotom, Deoxys,
            # Oricorio...) use the default form as the level-up form-change
            # source.  Either way, split choices are handled by the same
            # collection-friendly Eevee selector in pet.cpp.
            source = internal_base_id(parent_nat) if parent_nat else internal_base_id(nat)
            lvl = base_edges.get((parent_nat,nat),30) if parent_nat else 30
            add_edge(source,iid,lvl,'ordinary form branch')
    # Preserve the three v3.58 normal->Alola final branches in generic data too is unnecessary;
    # ALOLA_BRANCH_* remains compiled and the generic code will consider both arrays.

    # Extra evolution level for any base whose ordinary table says final/level 0.
    extra_edges.sort(key=lambda x:(x[0],x[1]))
    # A malformed upstream minimum level must never escape the playable Lv.100
    # range. add_edge() clamps it; assert here so Actions fail loudly if a future
    # refactor bypasses that guard.
    bad_levels=[x for x in extra_edges if not (2 <= x[2] <= EVOLUTION_LEVEL_CAP)]
    if bad_levels:
        raise RuntimeError('evolution level overflow: '+repr(bad_levels[:20]))

    # Regression guard for the National-Dex/form ambiguity class. These target
    # species must NEVER be reachable from the normal parent. The source must
    # be the enabled regional/form entry resolved above.
    regional_route_rows=[]
    for target_nat,spec in sorted(REGIONAL_DESCENDANT_SOURCES.items()):
        ti=internal_base_id(target_nat)
        if ti not in new_entries or not new_entries[ti].get('enabled'):
            continue
        src=resolve_regional_descendant_source(target_nat)
        parent=internal_base_id(int(spec['parent_nat']))
        good=[x for x in extra_edges if x[0]==src and x[1]==ti]
        wrong=[x for x in extra_edges if x[0]==parent and x[1]==ti and parent!=src]
        if not good or wrong:
            raise RuntimeError(f'regional evolution routing failed nat#{target_nat}: source={src} parent={parent} good={good} wrong={wrong}')
        regional_route_rows.append((target_nat,src,ti,good[0][2]))

    # Fixed v3.58 Alola lines are intentionally retained in DEX_TBL. Verify
    # them here so future catalog refactors cannot silently break those forms.
    fixed_alola_direct=[(810,811),(813,814),(815,816),(817,818),(819,820),(821,822),(822,823),(824,825)]
    for a,b in fixed_alola_direct:
        if int(base_rows[a]['evolvesTo']) != b:
            raise RuntimeError(f'fixed Alola evolution missing: {a}->{b}')
    if not all(x in base_text for x in ('ALOLA_BRANCH_BASES[ALOLA_BRANCH_COUNT] = { 25, 102, 104 }',
                                        'ALOLA_BRANCH_EVOS[ALOLA_BRANCH_COUNT] = { DEX_A_RAICHU, DEX_A_EXEGGUTOR, DEX_A_MAROWAK }')):
        raise RuntimeError('fixed Alola special branch table changed/missing')

    # Decide where an old save on a retired visual slot should land. A labeled
    # regional Alternate (e.g. Galar_Alternate Ponyta) returns to the canonical
    # regional form, while plain AltColor/Alternate/Cutscene/Beta returns to the
    # ordinary National-Dex species.
    for r in retired_forms:
        nat=int(r['natdex']); tag=regional_tag(r.get('label',''),nat)
        r['canonical_id']=int(by_region_form.get((nat,tag), internal_base_id(nat))) if tag else internal_base_id(nat)
    retired_ids=[int(x['id']) for x in retired_forms]
    # Also retire sprite FILES for temporarily user-disabled species. Their catalog/Dex
    # IDs remain reserved; only p<ID>.bin/ps<ID>.bin are tombstoned for SD cleanup.
    user_disabled_sprite_ids=[]
    for nat in sorted(USER_DISABLED_NATDEX):
        if nat <= current_max:
            user_disabled_sprite_ids.append(internal_base_id(nat))
    for f in form_entries:
        if int(f['natdex']) in USER_DISABLED_NATDEX:
            user_disabled_sprite_ids.append(int(f['id']))
    retired_ids=sorted(set(retired_ids) | set(user_disabled_sprite_ids))
    max_id=max([827,current_max+18]+[int(x['id']) for x in lock['forms'].values()]+retired_ids)
    # Build every positional row; holes are disabled placeholders.
    all_rows=[None]*(max_id+1)
    for i in range(min(828,len(base_rows))): all_rows[i]=copy.deepcopy(base_rows[i])
    for iid,e in new_entries.items():
        all_rows[iid]={'name':e['name'],'evolvesTo':0,'evolveLevel':0,'rarity':e['rarity'],'accent':e['accent'],
                      'hp':e['hp'],'atk':e['atk'],'def':e['def'],'spe':e['spe'],'spa':e['spa'],'spd':e['spd'],
                      'biome':e['biome'],'type1':e['type1'],'type2':e['type2']}
    for i in range(len(all_rows)):
        if all_rows[i] is None:
            all_rows[i]={'name':'UNUSED','evolvesTo':0,'evolveLevel':0,'rarity':'R_EVO','accent':'0x8C4D',
                         'hp':1,'atk':1,'def':1,'spe':1,'spa':1,'spd':1,'biome':0,'type1':'T_NORMAL','type2':'T_NONE'}

    # KO positional list.
    ko_names=['-']*(max_id+1)
    for i in range(min(828,len(base_ko))): ko_names[i]=base_ko[i]
    for iid,e in new_entries.items(): ko_names[iid]=e['ko_name']

    # region, National-Dex owner and enabled maps.  DEX_NATDEX lets old
    # move data be reused by alternate forms of #1..809 without a giant
    # hand-maintained switch statement.
    regions=[10]*(max_id+1); enabled=[0]*(max_id+1); natdex_map=[0]*(max_id+1)
    retired_base_map=[0]*(max_id+1)
    for i in range(1,min(810,max_id+1)):
        regions[i]=gen_region(i); natdex_map[i]=i; enabled[i]=0 if i in NO_ART_BASE else 1
    for (nat,key),iid in FIXED_ALOLA.items():
        if iid <= max_id:
            regions[iid]=6; natdex_map[iid]=nat; enabled[iid]=1 if iid in enabled_ids else 0
    for iid,e in new_entries.items():
        regions[iid]=e['region']; natdex_map[iid]=e['natdex']; enabled[iid]=1 if iid in enabled_ids else 0
    for r in retired_forms:
        iid=int(r['id']); nat=int(r['natdex'])
        if 0 < iid <= max_id:
            regions[iid]=gen_region(nat)
            natdex_map[iid]=nat
            retired_base_map[iid]=int(r.get('canonical_id',internal_base_id(nat)))

    # Region starters. IDs are deterministic for base species >809.
    starters={
      0:[1,4,7,25,133],1:[152,155,158],2:[252,255,258],3:[387,390,393],
      4:[495,498,501],5:[650,653,656],6:[722,725,728],
      7:[internal_base_id(810),internal_base_id(813),internal_base_id(816)],
      8:[722,155,501],
      9:[internal_base_id(906),internal_base_id(909),internal_base_id(912)],
    }
    allstar=[]
    for r in range(10): allstar+=starters[r]
    starters[10]=allstar

    # --- generate dex.h from types/struct sections in pristine v3.58 template ---
    # Keep type chart + structs from before DEX_TBL; replace count/branch/array/regions.
    pre=base_text[:base_text.index('static const DexEntry DEX_TBL')]
    pre=re.sub(r'#define DEX_COUNT\s+827',f'#define DEX_COUNT {max_id}',pre)
    # Insert generic extra-branch table immediately after Alola branch definitions.
    insert_marker='static const int16_t ALOLA_BRANCH_EVOS[ALOLA_BRANCH_COUNT] = { DEX_A_RAICHU, DEX_A_EXEGGUTOR, DEX_A_MAROWAK };\n'
    branch_lines=['','// v3.61: all additional evolutions/forms use the same Eevee-style branch selector.',
                  f'#define EXTRA_BRANCH_COUNT {len(extra_edges)}']
    if extra_edges:
        branch_lines.append('static const int16_t EXTRA_BRANCH_BASES[EXTRA_BRANCH_COUNT] = { '+', '.join(str(x[0]) for x in extra_edges)+' };')
        branch_lines.append('static const int16_t EXTRA_BRANCH_EVOS[EXTRA_BRANCH_COUNT] = { '+', '.join(str(x[1]) for x in extra_edges)+' };')
        branch_lines.append('static const uint8_t EXTRA_BRANCH_LEVELS[EXTRA_BRANCH_COUNT] = { '+', '.join(str(x[2]) for x in extra_edges)+' };')
    else:
        branch_lines += ['static const int16_t EXTRA_BRANCH_BASES[1] = { 0 };','static const int16_t EXTRA_BRANCH_EVOS[1] = { 0 };','static const uint8_t EXTRA_BRANCH_LEVELS[1] = { 0 };']
    # maps are emitted here because pet.cpp/noart use them before DEX_TBL content details.
    branch_lines += [
      '', '// Region membership is explicit because appended forms are non-contiguous.',
      'static const uint8_t DEX_REGION[DEX_COUNT + 1] = {',
    ]
    for i in range(0,len(regions),32): branch_lines.append('  '+', '.join(str(x) for x in regions[i:i+32])+',')
    branch_lines += ['};','// National-Dex owner for base species and independent form entries.',
                     'static const uint16_t DEX_NATDEX[DEX_COUNT + 1] = {']
    for i in range(0,len(natdex_map),24): branch_lines.append('  '+', '.join(str(x) for x in natdex_map[i:i+24])+',')
    branch_lines += ['};','// 1 = a real PMDCollab behaviour sprite is available for this entry.',
                     'static const uint8_t DEX_ENABLED[DEX_COUNT + 1] = {']
    for i in range(0,len(enabled),64): branch_lines.append('  '+', '.join(str(x) for x in enabled[i:i+64])+',')
    branch_lines += ['};','// Retired presentation-only PMDCollab IDs migrate back to the canonical base species.',
                     'static const int16_t DEX_RETIRED_VARIANT_BASE[DEX_COUNT + 1] = {']
    for i in range(0,len(retired_base_map),32): branch_lines.append('  '+', '.join(str(x) for x in retired_base_map[i:i+32])+',')
    branch_lines += ['};',
                     'static inline int16_t canonicalizeRetiredVariant(int16_t d) {',
                     '  return (d >= 1 && d <= DEX_COUNT && DEX_RETIRED_VARIANT_BASE[d] > 0) ? DEX_RETIRED_VARIANT_BASE[d] : d;',
                     '}','']
    if insert_marker not in pre: raise RuntimeError('Alola branch marker not found in base dex.h')
    pre=pre.replace(insert_marker,insert_marker+'\n'.join(branch_lines)+'\n')

    dex_lines=[pre.rstrip(),'','static const DexEntry DEX_TBL[DEX_COUNT + 1] = {']
    for i,row in enumerate(all_rows):
        dex_lines.append(f'  {{ "{escape_c(row["name"])}", {row["evolvesTo"]}, {row["evolveLevel"]}, {row["rarity"]}, {row["accent"]}, {row["hp"]}, {row["atk"]}, {row["def"]}, {row["spe"]}, {row["spa"]}, {row["spd"]}, {row["biome"]}, {row["type1"]}, {row["type2"]} }},  // {i}')
    dex_lines += ['};','','// clasico Kanto','static const int16_t CLASSIC_DEX[] = { 1, 4, 7, 25, 133 };',
                  '#define CLASSIC_DEX_N (sizeof(CLASSIC_DEX)/sizeof(CLASSIC_DEX[0]))','',
                  'struct RegionInfo { const char *name; uint16_t lo, hi; const int16_t *starters; uint8_t starterCount; };']
    sn=['KANTO','JOHTO','HOENN','SINNOH','UNOVA','KALOS','ALOLA','GALAR','HISUI','PALDEA','ALL']
    for r in range(11):
        dex_lines.append(f'static const int16_t REGION_START_{sn[r]}[] = {{ '+', '.join(str(x) for x in starters[r])+' };')
    dex_lines += ['#define REGION_COUNT 11','#define REGION_ALL 10','',
                  'static const RegionInfo REGIONS[REGION_COUNT] = {',
                  '  { "KANTO", 1, 151, REGION_START_KANTO, 5 },',
                  '  { "JOHTO", 152, 251, REGION_START_JOHTO, 3 },',
                  '  { "HOENN", 252, 386, REGION_START_HOENN, 3 },',
                  '  { "SINNOH", 387, 493, REGION_START_SINNOH, 3 },',
                  '  { "UNOVA", 494, 649, REGION_START_UNOVA, 3 },',
                  '  { "KALOS", 650, 721, REGION_START_KALOS, 3 },',
                  '  { "ALOLA", 722, 827, REGION_START_ALOLA, 3 },',
                  f'  {{ "GALAR", {internal_base_id(810)}, {internal_base_id(898)}, REGION_START_GALAR, 3 }},',
                  f'  {{ "HISUI", {internal_base_id(899)}, {internal_base_id(905)}, REGION_START_HISUI, 3 }},',
                  f'  {{ "PALDEA", {internal_base_id(906)}, {internal_base_id(current_max)}, REGION_START_PALDEA, 3 }},',
                  f'  {{ "ALL", 1, {max_id}, REGION_START_ALL, {len(allstar)} }},','};','']
    (ROOT/'dex.h').write_text('\n'.join(dex_lines),encoding='utf-8')

    # ko_species.h
    kl=['#pragma once','#include "dex.h"','',f'static const char *const KO_SPECIES_NAMES[DEX_COUNT + 1] = {{']
    for i,n in enumerate(ko_names): kl.append(f'  "{escape_c(n)}",  // {i}')
    kl += ['};','static_assert(sizeof(KO_SPECIES_NAMES) / sizeof(KO_SPECIES_NAMES[0]) == DEX_COUNT + 1, "KO species table size");','']
    (ROOT/'ko_species.h').write_text('\n'.join(kl),encoding='utf-8')
    # noart becomes O(1) generated map rather than a hand list.
    (ROOT/'noart.h').write_text('#pragma once\n#include "dex.h"\nstatic inline bool speciesHasArt(int16_t d) { return d >= 1 && d <= DEX_COUNT && DEX_ENABLED[d] != 0; }\n',encoding='utf-8')

    # Catalog used by sprite packer/site builder.
    catalog_entries=[]
    # Fixed Alola entries: path from discovered tracker if available, otherwise v3.58 known 0001.
    for (nat,key),iid in FIXED_ALOLA.items():
        f=discovered.get(f'{nat:04d}/{key}')
        catalog_entries.append({'id':iid,'natdex':nat,'lock_key':f'{nat:04d}/{key}','form_key':key,
                                'form_name':(f or {}).get('form_name') or 'Alola',
                                'pmd_path':(f or {}).get('pmd_path') or f'{nat:04d}/{key}',
                                'shiny_path':(f or {}).get('shiny_path'),'name':ko_names[iid],'region':6,'kind':'form','enabled':bool(enabled[iid]),'mega':False})
    for iid,e in sorted(new_entries.items()):
        catalog_entries.append({'id':iid,'natdex':e['natdex'],'lock_key':(f"{e['natdex']:04d}/{e.get('form_key','')}" if e['kind']=='form' else None),
                                'form_key':e.get('form_key'),'form_name':e.get('form_name'),
                                'pmd_path':e.get('pmd_path'),'shiny_path':e.get('shiny_path'),'name':e['ko_name'],
                                'region':e['region'],'kind':e['kind'],'enabled':bool(e['enabled']),'mega':bool(e.get('mega')),
                                'disabled_reason':e.get('disabled_reason')})
    cat={'schema':1,'generated_utc':time.strftime('%Y-%m-%dT%H:%M:%SZ',time.gmtime()),'source':TRACKER_URL,
         'national_dex_max':current_max,'dex_count':max_id,'entries':catalog_entries,
         'user_disabled_natdex':sorted(USER_DISABLED_NATDEX),
         'extra_edges':[{'base':a,'target':b,'level':l,'reason':r} for a,b,l,r in extra_edges],
         'regional_descendant_routes':[{'natdex':n,'base':a,'target':b,'level':l} for n,a,b,l in regional_route_rows],
         'excluded_gimmicks':[x for x in excluded if not x.get('presentation')],
         'retired_presentation_variants':retired_forms,
         'retired_sprite_ids':sorted(retired_ids)}
    CATALOG_PATH.write_text(json.dumps(cat,ensure_ascii=False,indent=2),encoding='utf-8')
    LOCK_PATH.write_text(json.dumps(lock,ensure_ascii=False,indent=2,sort_keys=True),encoding='utf-8')

    # v3.62.5 sprite mapping audit (retained from v3.62.3). The device stores only p<ID>.bin, so a
    # stale microSD created with a different form-ID lock can show the right
    # Pokemon name with the wrong old sprite.  Publish the exact ID -> NatDex ->
    # PMDCollab path mapping used by this build, and validate that every source
    # path belongs to the same National-Dex owner before packing anything.
    audit_entries=[]
    for nat in range(1,min(809,current_max)+1):
        node=tracker.get(f'{nat:04d}') or tracker.get(str(nat)) or {}
        bp=base_sprite_path(nat,node) if isinstance(node,dict) else None
        nm=base_ko[nat] if nat < len(base_ko) else (base_rows[nat]['name'] if nat < len(base_rows) else str(nat))
        audit_entries.append({'id':nat,'natdex':nat,'name':nm,'region':gen_region(nat),'kind':'base','enabled':bool(bp) and nat not in NO_ART_BASE,
                              'pmd_path':bp,'local_preview':None,
                              'preview_url':(f'https://raw.githubusercontent.com/PMDCollab/SpriteCollab/master/sprite/{bp}/Idle-Anim.png' if bp else None),
                              'source_url':(f'https://github.com/PMDCollab/SpriteCollab/tree/master/sprite/{bp}' if bp else None)})
    for e in catalog_entries:
        pp=e.get('pmd_path')
        audit_entries.append({'id':int(e['id']),'natdex':int(e['natdex']),'name':e.get('name') or str(e['id']),
                              'region':int(e.get('region',10)),'kind':e.get('kind','form'),'enabled':bool(e.get('enabled')),
                              'mega':bool(e.get('mega')),'form_name':e.get('form_name'),'form_key':e.get('form_key'),'lock_key':e.get('lock_key'),
                              'pmd_path':pp,'local_preview':None,
                              'preview_url':(f'https://raw.githubusercontent.com/PMDCollab/SpriteCollab/master/sprite/{pp}/Idle-Anim.png' if pp else None),
                              'source_url':(f'https://github.com/PMDCollab/SpriteCollab/tree/master/sprite/{pp}' if pp else None)})
    ids={}
    paths={}
    mapping_errors=[]
    for e in audit_entries:
        iid=int(e['id']); nat=int(e['natdex']); pp=e.get('pmd_path')
        if iid in ids: mapping_errors.append(f'duplicate internal id {iid}: {ids[iid]} / {e.get("name")}')
        ids[iid]=e.get('name')
        if pp:
            owner=pp.split('/',1)[0]
            if not owner.isdigit() or int(owner)!=nat:
                mapping_errors.append(f'cross-species source id {iid} {e.get("name")}: nat#{nat} -> {pp}')
            if e.get('enabled'):
                if pp in paths and paths[pp]!=iid:
                    mapping_errors.append(f'duplicate active PMD path {pp}: ids {paths[pp]} and {iid}')
                paths[pp]=iid
    # Explicit regression for the photographed bug: every Paldea Tauros entry
    # must source from PMDCollab owner 0128, never a stale Lapras/other-species path.
    tauros=[e for e in audit_entries if int(e['natdex'])==128 and int(e.get('region',10))==9 and e.get('enabled')]
    for e in tauros:
        if not str(e.get('pmd_path') or '').startswith('0128/'):
            mapping_errors.append(f'Paldea Tauros wrong source: id {e["id"]} -> {e.get("pmd_path")}')
    if mapping_errors:
        raise RuntimeError('sprite mapping audit failed: '+ '; '.join(mapping_errors[:30]))
    fp_src='\n'.join(f"{int(e['id'])}:{int(e['natdex'])}:{e.get('pmd_path') or '-'}:{e.get('name') or ''}" for e in sorted(audit_entries,key=lambda x:int(x['id'])) if e.get('enabled'))
    fingerprint=hashlib.sha256(fp_src.encode('utf-8')).hexdigest()[:16]
    audit={'schema':1,'generated_utc':cat['generated_utc'],'source':TRACKER_URL,'catalog_fingerprint':fingerprint,
           'dex_count':max_id,'national_dex_max':current_max,'entries':audit_entries}
    AUDIT_PATH.write_text(json.dumps(audit,ensure_ascii=False,indent=2),encoding='utf-8')
    map_lines=['TamaPoke v3.62.5 canonical-form sprite ID/path audit',f'catalog_fingerprint={fingerprint}',
               'Rule: internal ID -> NatDex owner -> PMDCollab path first component must agree.',
               f'active_entries={sum(1 for e in audit_entries if e.get("enabled"))}',f'paldea_tauros_entries={len(tauros)}','',
               'PALDEA TAUROS:']
    map_lines += [f"- id {e['id']} / nat#{e['natdex']} / {e['name']} / {e.get('pmd_path')}" for e in tauros]
    map_lines += ['', 'MANAGED 810+ / CANONICAL FORMS / MEGA:']
    map_lines += [f"- id {e['id']} / nat#{e['natdex']} / {e['name']} / {e.get('pmd_path') or 'NO-SPRITE'}" for e in audit_entries if int(e['id'])>=810]
    map_lines += ['', 'RETIRED PRESENTATION-ONLY IDS (not species/resources):']
    map_lines += [f"- id {r['id']} / nat#{r['natdex']} / {r['label']} / {r['reason']}" for r in retired_forms]
    MAPPING_REPORT_PATH.write_text('\n'.join(map_lines)+'\n',encoding='utf-8')

    included=sum(1 for x in catalog_entries if x['enabled'])
    form_included=sum(1 for x in catalog_entries if x['enabled'] and x['kind']=='form')
    mega_included=sum(1 for x in catalog_entries if x['enabled'] and x.get('mega'))
    user_disabled_entries=[x for x in catalog_entries if int(x.get('natdex',0)) in USER_DISABLED_NATDEX]
    missing=[x for x in catalog_entries if not x['enabled'] and int(x.get('natdex',0)) not in USER_DISABLED_NATDEX]
    report=[
      'TamaPoke v3.62.5 current PMDCollab catalog sync - canonical forms + regional evolution + Mega36',
      f'Source: {TRACKER_URL}',f'National Dex detected: 1..{current_max}',f'TamaPoke DEX_COUNT: {max_id}',
      f'Added/managed entries with real current sprite: {included}',f'Form entries with real current sprite: {form_included}',
      f'Approved Mega entries with real current sprite: {mega_included}/{APPROVED_MEGA_COUNT}',
      f'Excluded non-approved battle-gimmick forms: {sum(1 for x in excluded if not x.get("presentation"))}',
      f'Retired presentation-only form IDs: {len(retired_forms)}',
      f'User-disabled reserved National Dex: {",".join(map(str,sorted(USER_DISABLED_NATDEX)))}',
      f'Current new/base locked entries without sprite: {len(missing)}','',
      'APPROVED MEGA EVOLUTIONS:'
    ]
    report += [f"- id {x['id']} nat#{x['natdex']} {x['name']} Lv.{MEGA_EVOLVE_LEVEL} ({x.get('pmd_path')})" for x in catalog_entries if x.get('enabled') and x.get('mega')]
    report += ['', 'EXCLUDED GIMMICKS:']
    report += [f"- #{x['natdex']} {x['full_name']} ({x['pmd_path']})" for x in excluded if not x.get('presentation')]
    report += ['', 'RETIRED PRESENTATION-ONLY PMDCOLLAB SLOTS:']
    report += [f"- id {r['id']} nat#{r['natdex']} {r['label']} -> canonical id {r.get('canonical_id')}" for r in retired_forms]
    report += ['', 'USER-DISABLED / RESERVED IDS:']
    report += [f"- id {x['id']} nat#{x['natdex']} {x['name']} (reserved; not hatchable/encounterable/packed)" for x in user_disabled_entries]
    report += ['', 'NO CURRENT SPRITE / DISABLED:']
    report += [f"- id {x['id']} nat#{x['natdex']} {x['name']}" for x in missing]
    if failures:
        report += ['', 'METADATA WARNINGS:']+['- '+x for x in failures]
    REPORT_PATH.write_text('\n'.join(report)+'\n',encoding='utf-8')
    print('\n'.join(report[:9]))
    print('generated dex.h, ko_species.h, noart.h, pmd_catalog.json, catalog_lock.json, sprite_audit.json, sprite_mapping_report.txt')

if __name__=='__main__': main()
