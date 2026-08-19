#!/usr/bin/env bash
# Foundation build: recompiles the GMFE69 native game module for Android arm64.
# A production Android app still needs ANativeWindow/Vulkan/audio/lifecycle glue.
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
: "${ANDROID_NDK_HOME:?Set ANDROID_NDK_HOME to an Android NDK directory}"
JOBS="${JOBS:-$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 4)}"
DOLRECOMP="${DOLRECOMP:-$ROOT/build/dolrecomp-bootstrap/dolrecomp}"
[[ -x "$DOLRECOMP" ]] || { echo "Build host dolrecomp first: ./build.sh" >&2; exit 1; }
[[ -f "$ROOT/extracted/sys/main.dol" ]] || { echo "Missing extracted GMFE69 game" >&2; exit 1; }
TOOLCHAIN="$ANDROID_NDK_HOME/build/cmake/android.toolchain.cmake"
OUT="$ROOT/module/android-arm64/gGMFE69_recomp.so"
mkdir -p "$(dirname "$OUT")"
ANDROID_PLATFORM="${ANDROID_PLATFORM:-android-26}"
python3 "$ROOT/tools/build_all_exec_module.py" \
  --extracted "$ROOT/extracted" --dolrecomp "$DOLRECOMP" --project-root "$ROOT" \
  --work "$ROOT/port-build/GMFE69/android-arm64" --output "$OUT" \
  --game-id GMFE69 --jobs "$JOBS" --backend c --opt-level 3 \
  --cmake-toolchain "$TOOLCHAIN" --module-type SHARED \
  --cmake-define ANDROID_ABI=arm64-v8a --cmake-define "ANDROID_PLATFORM=$ANDROID_PLATFORM"
echo "Android ARM64 recomp module: $OUT"
echo "NOTE: v9 provides the core/module/input bridge foundation, not a finished APK."
