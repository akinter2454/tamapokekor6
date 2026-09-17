#!/usr/bin/env python3
from pathlib import Path
import subprocess,sys
ROOT=Path(__file__).resolve().parents[1]
H=(ROOT/'TamaPoke-KO-OneClick-Installer.html').read_text(encoding='utf-8')
W=(ROOT/'.github/workflows/main.yml').read_text(encoding='utf-8')
B=(ROOT/'tools/build_full_sd_sprite_zip.py').read_text(encoding='utf-8')
assert 'downloadFullSpriteZip' in H
assert 'sprite-bundle.json' in H
assert 'PC용 전체 스프라이트 ZIP' in H
assert 'build_full_sd_sprite_zip.py' in W
assert 'Publish full sprite ZIP to one rolling GitHub Release' in W
assert 'TAG="sprites-current"' in W
assert 'STABLE_FILE="TamaPoke-SD-Sprites-Current-Full.zip"' in W
assert 'gh release upload' in W and '--clobber' in W
assert 'Upload full sprite ZIP as workflow artifact backup' not in W
assert 'full-sprite-bundle-report.txt' in W
assert "BASE_PACK_COMMIT='7d5a2b3f4a4bee4f107cdae77ad13949dddfc615'" in B
assert "BANNED=('altcolor','alt colour','alternate','cutscene','beta')" in B
assert 'retired/non-active managed sprite in bundle' in B
subprocess.check_call([sys.executable,str(ROOT/'tools/build_full_sd_sprite_zip.py'),'--self-test'])
print('full SD bundle integration audit OK')
