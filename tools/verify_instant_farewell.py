#!/usr/bin/env python3
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
pet_h = (ROOT / "pet.h").read_text(encoding="utf-8")
pet_cpp = (ROOT / "pet.cpp").read_text(encoding="utf-8")
ino = (ROOT / "TamaPoke.ino").read_text(encoding="utf-8")

def need(ok, message):
    if not ok:
        raise SystemExit("FAIL: " + message)

need("#define FAREWELL_AGE_MIN 0UL" in pet_h, "farewell wait remains")
need("#define EVO_PENALTY_LEVELS ((uint8_t)0)" in pet_h, "evolution penalty remains")
need("bool Pet::canFarewellNow() const {\n  return canRetireNow();\n}" in pet_cpp,
     "good farewell is not immediately tied to the safe retire availability check")
need("retirePending = false;" in pet_cpp, "retirement debt can still be armed")
need('case 6: snprintf(out, n, "좋은 이별"); break;' in ino,
     "menu still presents the action as penalized retirement")
need("sub1 = T(S_RETIRE_COST)" not in ino, "penalty warning is still shown")

print("PASS: good farewell is immediate and carries no evolution penalty")
