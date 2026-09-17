#!/usr/bin/env python3
"""Regression guard for the browser-only Pendulum sprite importer."""
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
html = (ROOT / "tools/Digimon-SD-Pack-Maker.html").read_text(encoding="utf-8")
standalone = ROOT / "tools/Digimon-Pendulum-SD-Pack-Maker-v3.85.0.html"
ino = (ROOT / "TamaPoke.ino").read_text(encoding="utf-8")

for token in (
    "0. VB~5. ME", "upload=s.match(/^([0-5])", "pendulumOnly",
    "scope=${scope}", "whamonperfect", "bakumon",
    "gatomon:'tailmon'", "omnimon:'omegamon'",
    "DGI3", "15,48,48", "id&255", "d${String(id).padStart(3,'0')}.dgi",
):
    assert token in html, f"converter regression: missing {token!r}"

for token in (
    "frame=11+(now/850)%2", "frame=((now/260)&1)?2:0",
    "frame=13+(now/650)%2", "frame=((now/240)&1)?3:1",
    "frame=((now/300)&1)?2:1", "flip=movingRight", "frame=0",
    "y=PET_GROUND-drawH", "sx=flip?digiSpriteW-1-px:px",
):
    assert token in ino, f"firmware animation regression: missing {token!r}"

assert standalone.exists(), "standalone Pendulum converter missing"
assert standalone.read_bytes() == (ROOT / "tools/Digimon-SD-Pack-Maker.html").read_bytes(), \
    "standalone converter is not synchronized"
print("PASS: Pendulum P0-P5 importer, DGI3 output and shared Digimon animation mapping verified")
