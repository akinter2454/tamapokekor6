#!/usr/bin/env python3
"""Regression checks for the card-only evolution prompt."""

from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
SOURCE = (ROOT / "TamaPoke.ino").read_text(encoding="utf-8")


def function_body(signature: str) -> str:
    start = SOURCE.find(signature)
    if start < 0:
        raise SystemExit(f"missing function: {signature}")
    brace = SOURCE.find("{", start)
    depth = 0
    for pos in range(brace, len(SOURCE)):
        if SOURCE[pos] == "{":
            depth += 1
        elif SOURCE[pos] == "}":
            depth -= 1
            if depth == 0:
                return SOURCE[brace + 1 : pos]
    raise SystemExit(f"unterminated function: {signature}")


touch = function_body("void onTap(int16_t x, int16_t y) {")
stats = function_body("void renderCardStats() {")
card = function_body("void renderCard() {")
render = function_body("void render() {")

required_stats = (
    "pet.wantEvolveButton()",
    "CARD_EVO_X",
    "CARD_EVO_Y",
    '"진화하기"',
)
for token in required_stats:
    if token not in stats:
        raise SystemExit(f"card stats evolution control missing: {token}")

for token in ("cardPage == 1", "CARD_EVO_X", "choiceKind = 1"):
    if token not in touch:
        raise SystemExit(f"card evolution touch path missing: {token}")

for token in ("pet.evolve()", "pet.declineEvolve()"):
    if token not in touch:
        raise SystemExit(f"card evolution decision missing: {token}")

evolve_block = touch[touch.find("if (evolveYes)") : touch.find("else if (evolveLater)")]
if "cardOpen = false" not in evolve_block:
    raise SystemExit("confirmed evolution must close the card for the home animation")

if "choiceKind == 1" not in card or "drawChoiceDialog()" not in card:
    raise SystemExit("evolution confirmation must be rendered inside the card")

if "drawEvolveButton()" in render:
    raise SystemExit("home render must not draw the evolution button")
if "EVO_BTN_X" in touch or "EVO_BTN_Y" in touch:
    raise SystemExit("home touch handler still exposes the old evolution button")

print("card-only evolution UI regression OK")
