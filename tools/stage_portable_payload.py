#!/usr/bin/env python3
"""Stage the source subset required by the portable end-user AOT bundle."""
from __future__ import annotations

import argparse
from pathlib import Path
import shutil

FILES = (
    "LICENSE",
    "README.md",
    "CHANGELOG.md",
    "run.sh",
    "scripts/run-windows.ps1",
    "scripts/build-portable.sh",
    "scripts/build-portable.ps1",
    "tools/build_all_exec_module.py",
    "tools/elf2dol.py",
    "tools/elf_symbols.py",
    "tools/gmfe69_postgen.py",
    "tools/gmfe69_hotstack_postgen.py",
    "tools/gmfe69_particle_wpar_postgen.py",
    "tools/gmfe69_pc_native_postgen.py",
    "tools/gmfe69_prevchunk_postgen.py",
)

TREES = (
    "multi-module-template",
    "ModernGekko/vendor/dolphin/GXRuntime/include",
    "ModernGekko/vendor/dolphin/GXRuntime/src/core",
    "ModernGekko/vendor/dolphin/Source/Core/Core/PowerPC/StaticRecomp",
)

EMPTY_DIRS = (
    "module",
    "runtime",
    "user",
    "disc-cache",
    "HD/PS3_FILES",
)

def copy_file(source: Path, destination: Path) -> None:
    if not source.is_file():
        raise FileNotFoundError(source)
    destination.parent.mkdir(parents=True, exist_ok=True)
    shutil.copy2(source, destination)

def copy_tree(source: Path, destination: Path) -> None:
    if not source.is_dir():
        raise FileNotFoundError(source)
    shutil.copytree(source, destination, dirs_exist_ok=True)

def stage(source_root: Path, destination_root: Path) -> None:
    source_root = source_root.resolve()
    destination_root = destination_root.resolve()
    destination_root.mkdir(parents=True, exist_ok=True)

    for relative in FILES:
        copy_file(source_root / relative, destination_root / relative)

    for relative in TREES:
        copy_tree(source_root / relative, destination_root / relative)

    for relative in EMPTY_DIRS:
        (destination_root / relative).mkdir(parents=True, exist_ok=True)

    manifest = destination_root / "PORTABLE-PAYLOAD.txt"
    manifest.write_text(
        "MOHFrontline portable AOT payload v1\n"
        "This intentionally excludes the full ModernGekko/Dolphin source tree.\n"
        "The prebuilt runtime is shipped separately inside this same archive.\n"
        "Only the C module template, GXRuntime headers, StaticRecomp ABI headers,\n"
        "and project-local Python post-generation tools are retained for local AOT.\n",
        encoding="utf-8",
    )

def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--source", type=Path, default=Path("."))
    parser.add_argument("--destination", type=Path, required=True)
    args = parser.parse_args()
    stage(args.source, args.destination)
    return 0

if __name__ == "__main__":
    raise SystemExit(main())
