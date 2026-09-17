#!/usr/bin/env python3
"""Fail CI when a quoted firmware include is absent from the repository."""
import re
from pathlib import Path

root = Path(__file__).resolve().parents[1]
external = {"Arduino_GFX_Library.h", "TouchDrvCSTXXX.hpp"}
missing = []
for source in sorted(root.iterdir()):
    if source.suffix.lower() not in {".ino", ".cpp", ".c", ".h", ".hpp"}:
        continue
    text = source.read_text(encoding="utf-8", errors="replace")
    for name in re.findall(r'^\s*#\s*include\s*"([^"]+)"', text, re.M):
        if name not in external and not (source.parent / name).is_file():
            missing.append(f"{source.name}: {name}")
if missing:
    raise SystemExit("missing local include files:\n" + "\n".join(missing))
print("PASS: every quoted firmware include exists in the project root")
