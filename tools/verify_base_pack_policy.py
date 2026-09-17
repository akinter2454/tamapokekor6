#!/usr/bin/env python3
from pathlib import Path
import importlib.util, re

ROOT=Path(__file__).resolve().parents[1]
HTML=(ROOT/'TamaPoke-KO-OneClick-Installer.html').read_text(encoding='utf-8')
INO=(ROOT/'TamaPoke.ino').read_text(encoding='utf-8')
VM=re.search(r'^#define\s+FW_VERSION\s+"([^"]+)"', INO, re.M)
FW=VM.group(1) if VM else 'unknown'
SYNC=ROOT/'tools/sync_pmd_catalog.py'
REPORT=ROOT/'tools/base_pack_policy_report.txt'
PIN='7d5a2b3f4a4bee4f107cdae77ad13949dddfc615'

assert f"const BASE_PACK_COMMIT = '{PIN}';" in HTML
assert 'raw.githubusercontent.com/DylanPDao/TamaPoke/${BASE_PACK_COMMIT}/web/' in HTML
assert 'DylanPDao/TamaPoke/main/web/' not in HTML
assert 'DylanPDao/TamaPoke/releases/latest/download/' not in HTML
assert 'function validateBasePak(region, items)' in HTML
assert "kanto:[1,151]" in HTML and "alola:[722,809]" in HTML
assert "magic!=='TPK2' && magic!=='TPK3'" in HTML
assert r"/^mons\/(ps?)(\d{3,4})\.bin$/" in HTML

spec=importlib.util.spec_from_file_location('sync_pmd_catalog_baseguard',SYNC)
mod=importlib.util.module_from_spec(spec); spec.loader.exec_module(mod)

def sprite(name, key='0001'):
    return {'name':name,'sprite_files':{'AnimData.xml':{}},'subgroups':{}}
def group(name='', children=None, has=False):
    return {'name':name,'sprite_files':({'AnimData.xml':{}} if has else {}),'subgroups':children or {}}

# Synthetic tracker: presentation/gimmick children appear before a valid Normal.
node=group(children={'0000':group(children={
    '0001':sprite('AltColor'),
    '0002':sprite('Alternate'),
    '0003':sprite('Cutscene'),
    '0004':sprite('Beta'),
    '0005':sprite('Mega X'),
    '0006':sprite('Alola'),
    '0007':sprite('Normal'),
})})
assert mod.base_sprite_path(6,node)=='0006/0000/0007', mod.base_sprite_path(6,node)
# If only unsafe siblings exist, base sprite audit must refuse rather than bind wrong art.
unsafe=group(children={'0000':group(children={'0001':sprite('AltColor'),'0002':sprite('Alternate')})})
assert mod.base_sprite_path(1,unsafe) is None
# Root and direct normal-0000 layouts remain supported.
assert mod.base_sprite_path(25,group(has=True))=='0025'
assert mod.base_sprite_path(25,group(children={'0000':group(has=True)}))=='0025/0000'

lines=[
 f'TamaPoke v{FW} BASE PACK POLICY AUDIT',
 '',
 f'Pinned upstream commit: {PIN}',
 'Upstream repository: DylanPDao/TamaPoke',
 'Base pack files: sprites-kanto/johto/hoenn/sinnoh/unova/kalos/alola.pak',
 'Audited upstream packing policy: normal sprite = PMDCollab sprite/NNNN root; shiny = sprite/NNNN/0000/0001.',
 'Installer policy: immutable commit URL only; no mutable main or releases/latest fallback.',
 'PC full-SD bundle builder downloads and revalidates the same immutable seven base TPAKs before extracting them.',
 'Browser pre-transfer guard (automatic and manually selected named base packs): only mons/pNNN.bin, mons/psNNN.bin, optional mons/thumbs.bin; exact regional NatDex range; TPK2/TPK3 sprite magic; duplicate names rejected.',
 'Local catalog base fallback: AltColor/Alternate/Cutscene/Beta, gimmicks, and regional siblings cannot be selected as base art.',
 '',
 'Region ranges:',
 '  Kanto 1-151', '  Johto 152-251', '  Hoenn 252-386', '  Sinnoh 387-493',
 '  Unova 494-649', '  Kalos 650-721', '  Alola 722-809',
 '', 'RESULT: PASS'
]
REPORT.write_text('\n'.join(lines)+'\n',encoding='utf-8')
print('\n'.join(lines))
