#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
DISC_IMAGE="${1:-}"
JOBS="${JOBS:-$(nproc 2>/dev/null || echo 4)}"
CACHE="$ROOT/disc-cache/GMFE69"
WORK="$ROOT/port-build/GMFE69/portable-linux"
TOOLCHAIN="$ROOT/toolchain"
CMAKE="$TOOLCHAIN/cmake/bin/cmake"
PYTHON="$TOOLCHAIN/python/bin/python3"
DOLRECOMP="$TOOLCHAIN/bin/dolrecomp"
DISC_CACHE="$ROOT/MOHFrontline-DiscCache"
[[ -x "$DISC_CACHE" ]] || DISC_CACHE="$ROOT/launcher/bin/MOHFrontline-DiscCache"

fail() { echo "error: $*" >&2; exit 1; }

[[ -n "$DISC_IMAGE" && -f "$DISC_IMAGE" ]] || fail "select a GMFE69 disc image"
[[ -x "$CMAKE" ]] || fail "portable CMake is missing"
[[ -x "$TOOLCHAIN/bin/ninja" ]] || fail "portable Ninja is missing"
[[ -x "$PYTHON" ]] || fail "portable Python is missing"
[[ -x "$TOOLCHAIN/zig/zig" ]] || fail "portable Zig is missing"
[[ -x "$DOLRECOMP" ]] || fail "prebuilt DolRecomp is missing"
[[ -x "$DISC_CACHE" ]] || fail "MOHFrontline-DiscCache is missing"
[[ -x "$ROOT/runtime/moderngekko-run" ]] || fail "prebuilt runtime is missing"

mkdir -p "$ROOT/user" "$ROOT/module" "$ROOT/port-build"
exec > >(tee "$ROOT/user/launcher-build.log") 2>&1
export PATH="$TOOLCHAIN/cmake/bin:$TOOLCHAIN/bin:$TOOLCHAIN/zig:$PATH"
export CMAKE_GENERATOR=Ninja

echo "==> Building minimal executable cache with encounter/nod"
"$DISC_CACHE" "$DISC_IMAGE" "$CACHE"
printf '%s\n' "$(realpath "$DISC_IMAGE")" > "$ROOT/user/disc-image.txt"

echo "==> Recompiling GMFE69 with bundled portable toolchain"
"$PYTHON" "$ROOT/tools/build_all_exec_module.py" \
  --extracted "$CACHE" \
  --dolrecomp "$DOLRECOMP" \
  --project-root "$ROOT" \
  --work "$WORK" \
  --output "$ROOT/module/gGMFE69_recomp.so" \
  --game-id GMFE69 \
  --jobs "$JOBS" \
  --backend c \
  --opt-level 3 \
  --module-type SHARED \
  --cmake-toolchain "$TOOLCHAIN/moh-zig.cmake"

echo "==> Ready: runtime will mount the original disc image directly"
