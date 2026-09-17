#!/usr/bin/env python3
from pathlib import Path
import re

ROOT = Path(__file__).resolve().parents[1]
moves = (ROOT / "moves.h").read_text(encoding="utf-8")
battle = (ROOT / "battle.cpp").read_text(encoding="utf-8")
i18n = (ROOT / "i18n.cpp").read_text(encoding="utf-8")
ino = (ROOT / "TamaPoke.ino").read_text(encoding="utf-8")

def need(ok, message):
    if not ok:
        raise SystemExit("FAIL: " + message)

count = int(re.search(r"#define MOVE_COUNT (\d+)", moves).group(1))
table = moves.split("static const MoveEntry MOVE_TBL[MOVE_COUNT] = {", 1)[1].split("\n};", 1)[0]
rows = [line for line in table.splitlines() if line.lstrip().startswith("{")]
ko = i18n.split("static const char *const KO[MOVE_COUNT] = {", 1)[1].split("\n  };", 1)[0]
ko_names = re.findall(r'"(?:[^"\\]|\\.)*"', ko)

need(count == 142, "MOVE_COUNT must include 18 digital signature moves")
need(len(rows) == count, "move table length differs from MOVE_COUNT")
need(len(ko_names) == count, "Korean move-name table length differs from MOVE_COUNT")

for token in (
    "MV_ROAR_OF_TIME", "MV_SPECTRAL_THIEF", "MV_FLOWER_TRICK",
    "MV_TORCH_SONG", "MV_AQUA_STEP", "MV_LUMINA_CRASH",
    "EF_STAGE_HIT", "EF_ALWAYS_CRIT", "EF_HIGH_CRIT", "EF_STEAL_STAGE",
    "MV_DIGI_PULSE", "MV_DIGI_LIGHT",
):
    need(token in moves, f"missing {token}")

for natdex in (25, 150, 249, 250, 382, 383, 483, 484, 487, 643, 644,
               647, 716, 717, 791, 792, 802, 812, 815, 818, 894, 895, 903,
               905, 908, 911, 914, 925, 934, 936, 937, 956, 983, 1000):
    need(f"case {natdex}:" in moves, f"missing species learnset {natdex}")

need("m.effect == EF_ALWAYS_CRIT" in battle, "always-critical effect is not resolved")
need("m.effect == EF_HIGH_CRIT" in battle, "high-critical effect is not resolved")
need("m.effect == EF_STAGE_HIT" in battle, "damaging stage effect is not resolved")
need("m.effect == EF_STEAL_STAGE" in battle, "stage-steal effect is not resolved")
need("log.healed = atk.hp > oldHp" in battle, "drain healing is not logged")
need("lg.stoleStages" in ino and "lg.stageDelta" in ino and "lg.healed" in ino,
     "new secondary effects are not narrated")

print(f"PASS: {count} moves, including 18 digital signature moves, Korean names and battle effects verified")
