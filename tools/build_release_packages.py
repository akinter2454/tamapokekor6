#!/usr/bin/env python3
"""Build validated TamaPoke release ZIPs without omitting CI verifier files."""

from __future__ import annotations

import argparse
import re
import zipfile
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
EXCLUDED_PARTS = {".git", "__pycache__", "sdcard", "web_extra"}
EXCLUDED_SUFFIXES = {".zip", ".pyc"}
EXTERNAL_QUOTED_INCLUDES = {"Arduino_GFX_Library.h", "TouchDrvCSTXXX.hpp"}


def firmware_version() -> str:
    text = (ROOT / "TamaPoke.ino").read_text(encoding="utf-8")
    match = re.search(r'^#define\s+FW_VERSION\s+"([0-9]+\.[0-9]+\.[0-9]+)"', text, re.M)
    if not match:
        raise SystemExit("semantic FW_VERSION missing")
    return match.group(1)


def project_files() -> list[Path]:
    return sorted(
        path for path in ROOT.rglob("*")
        if path.is_file()
        and not any(part in EXCLUDED_PARTS for part in path.relative_to(ROOT).parts)
        and path.suffix.lower() not in EXCLUDED_SUFFIXES
    )


def workflow_verifiers() -> set[str]:
    workflow = (ROOT / ".github/workflows/main.yml").read_text(encoding="utf-8")
    return {f"tools/{name}" for name in re.findall(r'tools/(verify_[A-Za-z0-9_]+\.py)', workflow)}


def write_zip(path: Path, files: list[Path]) -> None:
    with zipfile.ZipFile(path, "w", zipfile.ZIP_DEFLATED, compresslevel=9) as archive:
        for source in files:
            archive.write(source, source.relative_to(ROOT).as_posix())


def validate(path: Path, version: str) -> None:
    with zipfile.ZipFile(path) as archive:
        names = set(archive.namelist())
        bad = archive.testzip()
        if bad:
            raise SystemExit(f"corrupt ZIP entry: {bad}")
        required = {"TamaPoke.ino", ".github/workflows/main.yml"} | workflow_verifiers()
        missing = sorted(required - names)
        if missing:
            raise SystemExit(f"release ZIP missing workflow files: {missing}")
        ino = archive.read("TamaPoke.ino").decode("utf-8")
        if f'#define FW_VERSION "{version}"' not in ino:
            raise SystemExit("release ZIP firmware version mismatch")
        local_missing = []
        for name in sorted(names):
            if Path(name).parent != Path(".") or Path(name).suffix.lower() not in {".ino", ".cpp", ".c", ".h", ".hpp"}:
                continue
            source = archive.read(name).decode("utf-8", errors="replace")
            for include in re.findall(r'^\s*#\s*include\s*"([^"]+)"', source, re.M):
                if include in EXTERNAL_QUOTED_INCLUDES:
                    continue
                target = (Path(name).parent / include).as_posix()
                if target not in names:
                    local_missing.append(f"{name}: {include}")
        if local_missing:
            raise SystemExit("release ZIP missing local includes: " + ", ".join(local_missing))


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--output-dir", type=Path, default=ROOT.parent)
    args = parser.parse_args()
    version = firmware_version()
    args.output_dir.mkdir(parents=True, exist_ok=True)
    files = project_files()
    full = args.output_dir / f"TamaPoke-KO-v{version}-CasualEnergyBalance-FullProject.zip"
    patch = args.output_dir / f"TamaPoke-KO-v{version}-CasualEnergyBalance-SourcePatch.zip"
    # This project is source-only. Both packages intentionally include tools and
    # workflow files so applying either one cannot leave stale CI verifiers behind.
    for target in (full, patch):
        write_zip(target, files)
        validate(target, version)
        print(f"OK {target.name}: {len(files)} files, {target.stat().st_size} bytes")


if __name__ == "__main__":
    main()
