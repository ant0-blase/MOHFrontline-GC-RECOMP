#!/usr/bin/env bash
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
ISO_PATH="${1:-}"
[[ -n "$ISO_PATH" ]] || { echo "usage: launcher-build.sh /path/to/GMFE69.iso" >&2; exit 2; }
mkdir -p "$ROOT/user"
exec > >(tee "$ROOT/user/launcher-build.log") 2>&1
cd "$ROOT"
ISO="$ISO_PATH" ./build.sh
