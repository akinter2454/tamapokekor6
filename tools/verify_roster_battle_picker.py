#!/usr/bin/env python3
"""Regression checks for gym and type-boss roster selection."""

from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
SRC = (ROOT / "TamaPoke.ino").read_text(encoding="utf-8")


def body(signature: str) -> str:
    start = SRC.find(signature)
    if start < 0:
        raise SystemExit(f"missing function: {signature}")
    brace = SRC.find("{", start)
    depth = 0
    for pos in range(brace, len(SRC)):
        if SRC[pos] == "{":
            depth += 1
        elif SRC[pos] == "}":
            depth -= 1
            if depth == 0:
                return SRC[brace + 1:pos]
    raise SystemExit(f"unterminated function: {signature}")


build = body("static void buildSquad(uint8_t maxLvl")
exists = body("bool pickExists(uint8_t n) {")
draw = body("static void drawPickCell(uint8_t n")
render = body("void renderPick() {")
tap = body("void pickTap(int16_t x, int16_t y) {")
boss_start = body("void startBossBattle() {")
boss_tap = body("void bossTap(int16_t x,int16_t y){")
screen = body("uint8_t uiCurrentScreen() {")
main_render = body("void render() {")
on_tap = body("void onTap(int16_t x, int16_t y) {")

checks = {
    "32-bit selection mask": "uint32_t squadMask" in SRC and "1UL <<" in SRC,
    "box candidates": "party.box[b]" in exists,
    "box combatants": "party.box[i]" in build and "combatantFromParty" in build,
    "source labels": all(x in draw for x in ('"현재"', '"파티"', '"박스"')),
    "boss picker mode": "#define PICK_BOSS" in SRC and "pickTrainer == PICK_BOSS" in render,
    "boss exact-three rule": "pickChosen() == 3" in render and "pickChosen() != 3" in tap,
    "boss opens picker": all(x in boss_tap for x in ("pickTrainer=PICK_BOSS", "pickDefault(3)", "pickOpen=true")),
    "boss uses selected squad": "buildSquad(0, 3, squadMask)" in boss_start and "btlSquadN != 3" in boss_start,
    "LAN remains party-only": "pickTrainer == PICK_LAN ? PARTY_SLOTS" in SRC,
    "picker owns screen priority": screen.find("if (pickOpen)") < screen.find("if (bagOpen"),
    "picker owns render priority": main_render.find("if (pickOpen)") < main_render.find("if (bagOpen"),
    "picker owns touch priority": on_tap.find("if (pickOpen)") < on_tap.find("if (bagOpen"),
    "visible roster tabs": all(x in SRC for x in ("pickSourceTab", '"현재·파티"', '"박스"', "PICK_TAB_Y")),
    "tab filter preserves global selection": "pickVisible(n)" in render and "squadMask" in tap,
    "new firmware is identifiable": '#define FW_VERSION "3.89.0"' in SRC,
}

failed = [name for name, ok in checks.items() if not ok]
if failed:
    raise SystemExit("roster battle picker regression failed: " + ", ".join(failed))

print("gym/box and exact-three boss roster picker regression OK")
