#!/usr/bin/env python3
from __future__ import annotations

import argparse
from pathlib import Path
import re
import sys


def normalize_version(value: str) -> str:
    value = value.strip()
    if not value:
        raise ValueError("empty version")
    return value if value.startswith("v") else f"v{value}"


def extract(text: str, version: str) -> str:
    version = normalize_version(version)
    heading = re.compile(
        rf"^##\s+\[{re.escape(version)}\](?:\s+-\s+.*)?\s*$",
        re.MULTILINE,
    )
    match = heading.search(text)
    if not match:
        raise ValueError(f"CHANGELOG.md has no section for {version}")

    start = match.end()
    next_heading = re.search(r"^##\s+", text[start:], re.MULTILINE)
    end = start + next_heading.start() if next_heading else len(text)
    body = text[start:end].strip()
    if not body:
        raise ValueError(f"CHANGELOG.md section for {version} is empty")
    return body + "\n"


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Extract one version section from CHANGELOG.md for GitHub Release notes."
    )
    parser.add_argument("changelog", type=Path)
    parser.add_argument("--version", required=True)
    parser.add_argument("--output", type=Path)
    args = parser.parse_args()

    try:
        notes = extract(args.changelog.read_text(encoding="utf-8"), args.version)
    except (OSError, ValueError) as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 2

    if args.output:
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_text(notes, encoding="utf-8")
    else:
        sys.stdout.write(notes)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
