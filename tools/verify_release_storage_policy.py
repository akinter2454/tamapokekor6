#!/usr/bin/env python3
from pathlib import Path
import re

ROOT = Path(__file__).resolve().parents[1]
WF = ROOT / '.github' / 'workflows' / 'main.yml'
INSTALLER = ROOT / 'TamaPoke-KO-OneClick-Installer.html'

wf = WF.read_text(encoding='utf-8')
ins = INSTALLER.read_text(encoding='utf-8')
errors=[]

def need(cond,msg):
    if not cond: errors.append(msg)

need('TAG="sprites-current"' in wf, 'rolling Release tag sprites-current missing')
need('STABLE_FILE="TamaPoke-SD-Sprites-Current-Full.zip"' in wf, 'stable full sprite asset filename missing')
need('gh release upload "$TAG" "$STABLE_ASSET" --clobber' in wf, 'rolling asset clobber upload missing')
need('--json isImmutable' in wf, 'immutable Release guard missing')
need("m['release_tag']='sprites-current'" in wf, 'sprite-bundle release_tag rewrite missing')
need('/releases/download/sprites-current/' in wf, 'stable sprites-current download URL missing')
need("grep -E '^sprites-v[0-9]+\\.[0-9]+\\.[0-9]+$'" in wf, 'legacy sprite Release cleanup guard missing')
need('gh release delete "$OLD_TAG" -y --cleanup-tag' in wf, 'legacy sprite Release deletion missing')
need('Upload full sprite ZIP as workflow artifact backup' not in wf, 'large full sprite artifact backup step still present')
need('actions: write' in wf, 'Actions write permission missing for legacy artifact cleanup')
need('Remove legacy full-sprite Actions artifact backups' in wf, 'legacy full-sprite Actions artifact cleanup step missing')
need('/actions/artifacts/${ARTIFACT_ID}' in wf, 'legacy Actions artifact DELETE endpoint missing')
need('full-sd-sprites$' in wf, 'legacy Actions artifact name guard missing')
# It is fine for Pages itself to use a Pages artifact; only the full sprite ZIP must never go to actions/upload-artifact.
for m in re.finditer(r'actions/upload-artifact@[^\n]+', wf):
    around=wf[max(0,m.start()-400):m.end()+700]
    if re.search(r'TamaPoke-SD-Sprites|Full\.zip|full-sd-sprites',around,re.I):
        errors.append('full sprite ZIP is still routed through actions/upload-artifact')
        break
need('sprites-v${VER}' not in wf, 'versioned sprite Release tag assignment still present')
need('sprites-current' in ins, 'installer does not explain rolling sprite Release')
need('Actions Artifact에는 전체 ZIP을 올리지 않습니다.' in ins, 'installer storage-policy notice missing')

if errors:
    for e in errors: print('FAIL:',e)
    raise SystemExit(1)
print('release storage policy OK: single sprites-current Release, stable asset, no full-ZIP Actions artifact, legacy sprite Releases/artifacts cleaned')
