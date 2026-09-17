#!/usr/bin/env python3
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
ino = (ROOT / "TamaPoke.ino").read_text(encoding="utf-8")

iv_order = "XITEM_IV_ATK, XITEM_IV_DEF, XITEM_IV_SPE, XITEM_IV_HP"
growth_order = "XITEM_ATK, XITEM_DEF, XITEM_SPE, XITEM_VITAL"

def need(ok, message):
    if not ok:
        raise SystemExit("FAIL: " + message)

need(ino.count(iv_order) >= 2, "page 1 IV order is missing from render or touch mapping")
need(ino.count(growth_order) >= 2, "ordinary growth items were not moved consistently to page 3")
need("bool ivCandy = id >= XITEM_IV_ATK && id <= XITEM_IV_HP;" in ino,
     "IV candy range is not detected after use")
need("if (!ivCandy) bagOpen = false;" in ino,
     "IV candy still closes the bag")
need('if (y > 390) { bagOpen = false; sfxPlay(SFX_TAP); }' in ino,
     "explicit Close control no longer exits the bag")

print("PASS: IV candies are on page 1 and remain open for repeated use")
