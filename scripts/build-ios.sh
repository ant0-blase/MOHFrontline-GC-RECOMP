#!/usr/bin/env bash
# Foundation build: static GMFE69 module for a future signed iOS shell.
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
[[ "$(uname -s)" == Darwin ]] || { echo "iOS cross-build requires macOS + Xcode" >&2; exit 1; }
command -v xcrun >/dev/null || { echo "xcrun/Xcode not found" >&2; exit 1; }
JOBS="${JOBS:-$(sysctl -n hw.ncpu)}"
DOLRECOMP="${DOLRECOMP:-$ROOT/build/dolrecomp-bootstrap/dolrecomp}"
[[ -x "$DOLRECOMP" ]] || { echo "Build host dolrecomp first" >&2; exit 1; }
[[ -f "$ROOT/extracted/sys/main.dol" ]] || { echo "Missing extracted GMFE69 game" >&2; exit 1; }
OUT="$ROOT/module/ios-arm64/gGMFE69_recomp.a"
mkdir -p "$(dirname "$OUT")"
python3 "$ROOT/tools/build_all_exec_module.py" \
  --extracted "$ROOT/extracted" --dolrecomp "$DOLRECOMP" --project-root "$ROOT" \
  --work "$ROOT/port-build/GMFE69/ios-arm64" --output "$OUT" \
  --game-id GMFE69 --jobs "$JOBS" --backend c --opt-level 3 \
  --cmake-toolchain "$ROOT/scripts/toolchains/ios-arm64.cmake" --module-type STATIC
echo "iOS ARM64 static recomp module: $OUT"
echo "NOTE: a signed UIKit/Metal(or MoltenVK) shell is still required."
