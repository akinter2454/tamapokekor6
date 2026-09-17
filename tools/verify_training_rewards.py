#!/usr/bin/env python3
from pathlib import Path
import re
root=Path(__file__).resolve().parents[1]
ino=(root/'TamaPoke.ino').read_text(encoding='utf-8')
h=(root/'game_extras.h').read_text(encoding='utf-8')
cpp=(root/'game_extras.cpp').read_text(encoding='utf-8')
pet_h=(root/'pet.h').read_text(encoding='utf-8')
pet_cpp=(root/'pet.cpp').read_text(encoding='utf-8')
save=(root/'save.cpp').read_text(encoding='utf-8')
sd=(root/'sdmon.cpp').read_text(encoding='utf-8')
wf=(root/'.github/workflows/main.yml').read_text(encoding='utf-8')
wfcopy=(root/'GITHUB_WORKFLOW_COPY.txt').read_text(encoding='utf-8')

def need(cond,msg):
    if not cond: raise SystemExit('FAIL: '+msg)

vm=re.search(r'^#define\s+FW_VERSION\s+"([0-9]+\.[0-9]+\.[0-9]+)"', ino, re.M)
need(bool(vm), 'firmware semver marker missing')
# Save compatibility: new item must be appended AFTER Gold Crown, while old XITEM_SHINY stays in place.
need(re.search(r'XITEM_GOLD_CROWN,\s*XITEM_SHINY_BERRY,\s*XITEM_COUNT',h,re.S), 'Shiny Berry appended after legacy items')
need('case XITEM_SHINY:' in cpp and '_shinyBoost = true' in cpp, 'original next-egg Shiny boost retained')
need('consumeShinyBoostForEgg' in cpp, 'next-egg Shiny consumption retained')
need('case XITEM_SHINY_BERRY:' in cpp and 'pet.makeCurrentShiny()' in cpp, 'current Pokemon Shiny transformation uses public Pet API')
need('pet.registerSpecies(pet.speciesId);' not in cpp, 'GameExtras illegally calls private Pet::registerSpecies')
need('bool makeCurrentShiny();' in pet_h, 'public Shiny conversion API missing')
need('bool Pet::makeCurrentShiny()' in pet_cpp and 'registerSpecies(speciesId);' in pet_cpp, 'Pet Shiny API must register Shiny Pokedex entry internally')
need('count = random(100) < 30 ? 9 : 5;' in ino, '30 percent nine-IV-berry bonus roll with five guaranteed')
need('extras.giveItem(id, count);' in ino, 'guaranteed IV berry grant')
need('uint8_t id = primary;' in ino, 'training reward must stay on its dedicated IV candy')
need('random(100) < 10 ? XITEM_IV_HP' not in ino, 'non-HP training still redirects into HP candy')
need('random(100) >= 30' in ino and 'XITEM_SHINY_BERRY' in ino, '30 percent Shiny Berry drop')
need('defRound >= DEF_ROUNDS' in ino, 'defence requires proper completion')
need('sackHits > 0' in ino and 'spdHits > 0' in ino and 'hpRound >= HP_ROUNDS' in ino,
     'training completion gates are incomplete')
need('훈련 보상:' in ino and '희귀 보상:' in ino, 'result screen reward labels')
need('const uint8_t utility[4] = { XITEM_SHINY, XITEM_ENERGY, XITEM_GOLD_CROWN, XITEM_SHINY_BERRY }' in ino, 'bag page exposes Shiny Berry')
need('size_t n = itemStored < sizeof(_items) ? itemStored : sizeof(_items);' in cpp and
     'prefs.getBytes("xitem", _items, n);' in cpp, 'older shorter xitem arrays migrate safely')
need('{ "xitem", SK_BYTES }' in save, 'xitem inventory remains in serial backup whitelist')
need('if (!f && shiny)' in sd, 'missing Shiny sprite must fall back to normal sprite')
need("score >= great ? 38 : (score >= good ? 22 : 0)" not in wf, 'workflow still checks obsolete random IV-drop formula')
need(wf.count('verify_training_rewards.py') >= 2, 'workflow does not run training reward regression in both verification stages')

# The workflow must not duplicate reward probability literals. Those checks belong
# here so reward tuning cannot leave an obsolete grep that aborts Actions before compile.
need('count = random(100) < 30 ? 2 : 1;' not in wf, 'workflow contains obsolete 1/2 IV-berry grep')
need("grep -n 'random(100) >= 3'" not in wf and "grep -q 'random(100) >= 3'" not in wf,
     'workflow contains obsolete 3-percent Shiny Berry grep')
need(wf == wfcopy, 'GITHUB_WORKFLOW_COPY.txt drifted from .github/workflows/main.yml')
need('다음 알: 반짝부적 적용 중", 1), 82' in ino, 'Shiny Charm armed label overlaps the fourth item row')
print('Training reward regression OK: dedicated IV berries, 5 guaranteed, 30% total x9, 30% current-Shiny berry')
