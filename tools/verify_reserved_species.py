#!/usr/bin/env python3
"""Verify temporarily disabled species keep stable IDs but cannot enter gameplay/resources."""
from __future__ import annotations
import json
import re
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
SYNC = ROOT / 'tools' / 'sync_pmd_catalog.py'
CATALOG = ROOT / 'tools' / 'pmd_catalog.json'
DEX = ROOT / 'dex.h'
MONS = ROOT / 'tools' / 'sdcard' / 'mons'

# National Dex -> deterministic TamaPoke internal base ID (nat + 18 for >809).
EXPECTED = {1020: 1038, 1021: 1039}


def parse_int_array(text: str, name: str) -> list[int]:
    m = re.search(rf'static const [^\n]+\b{name}\[DEX_COUNT \+ 1\]\s*=\s*\{{(.*?)\n\}};', text, re.S)
    if not m:
        raise SystemExit(f'{name} array not found in dex.h')
    return [int(x) for x in re.findall(r'-?\d+', m.group(1))]


def main() -> None:
    src = SYNC.read_text(encoding='utf-8')
    if not re.search(r'USER_DISABLED_NATDEX\s*=\s*\{\s*1020\s*,\s*1021\s*\}', src):
        raise SystemExit('reserved species policy missing: expected USER_DISABLED_NATDEX={1020,1021}')
    if 'enabled=bool(base_path) and not user_disabled' not in src:
        raise SystemExit('base-species user-disable gate missing')
    if 'enabled=bool(f) and nat not in USER_DISABLED_NATDEX' not in src:
        raise SystemExit('form user-disable gate missing')

    # Generated outputs only exist after the catalog sync. When present, prove
    # that both rows retain their exact IDs while DEX_ENABLED is zero.
    if CATALOG.is_file():
        cat = json.loads(CATALOG.read_text(encoding='utf-8'))
        got_policy = {int(x) for x in cat.get('user_disabled_natdex', [])}
        if got_policy != set(EXPECTED):
            raise SystemExit(f'catalog user-disabled policy mismatch: {sorted(got_policy)}')
        entries = cat.get('entries', [])
        retired_ids = {int(x) for x in cat.get('retired_sprite_ids', [])}
        missing_retire = set(EXPECTED.values()) - retired_ids
        if missing_retire:
            raise SystemExit(f'reserved sprite IDs missing from deletion/tombstone manifest: {sorted(missing_retire)}')
        for nat, iid in EXPECTED.items():
            rows = [e for e in entries if int(e.get('natdex', 0)) == nat and e.get('kind') == 'base']
            if len(rows) != 1:
                raise SystemExit(f'nat#{nat}: expected one base catalog row, got {len(rows)}')
            e = rows[0]
            if int(e.get('id', 0)) != iid:
                raise SystemExit(f'nat#{nat}: internal ID changed {e.get("id")} != {iid}')
            if e.get('enabled'):
                raise SystemExit(f'nat#{nat}/id{iid}: must remain disabled')
            if e.get('disabled_reason') != 'user-disabled reserved slot':
                raise SystemExit(f'nat#{nat}/id{iid}: reserved disabled_reason missing')

    if DEX.is_file() and '#define REGION_COUNT 11' in DEX.read_text(encoding='utf-8'):
        text = DEX.read_text(encoding='utf-8')
        enabled = parse_int_array(text, 'DEX_ENABLED')
        natmap = parse_int_array(text, 'DEX_NATDEX')
        for nat, iid in EXPECTED.items():
            if iid >= len(enabled) or iid >= len(natmap):
                raise SystemExit(f'nat#{nat}: reserved internal ID {iid} missing from generated dex arrays')
            if natmap[iid] != nat:
                raise SystemExit(f'nat#{nat}: DEX_NATDEX[{iid}]={natmap[iid]}')
            if enabled[iid] != 0:
                raise SystemExit(f'nat#{nat}: DEX_ENABLED[{iid}] must be 0')

    # Once sprites have been packed, these reserved IDs must not have normal or
    # shiny TPK3 files. Their IDs remain reserved in the catalog only.
    if MONS.is_dir():
        leaked = []
        for iid in EXPECTED.values():
            for pref in ('p', 'ps'):
                f = MONS / f'{pref}{iid:03d}.bin'
                if f.exists():
                    leaked.append(str(f.relative_to(ROOT)))
        if leaked:
            raise SystemExit('reserved species sprite files leaked into pack: ' + ', '.join(leaked))

    print('reserved species OK: #1020->1038 and #1021->1039 stay disabled without renumbering')


if __name__ == '__main__':
    main()
