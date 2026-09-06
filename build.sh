#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SOURCE="$ROOT/ModernGekko"
BUILD_ROOT="$ROOT/build"
RUNTIME_BUILD="$BUILD_ROOT/runtime"
BOOTSTRAP_RECOMP_BUILD="$BUILD_ROOT/dolrecomp-bootstrap"
BOOTSTRAP_RECOMP_SOURCE="$SOURCE/vendor/dolphin/DolRecomp"
PORT_BUILD="$ROOT/port-build"
EXTRACTED="$ROOT/extracted"
ISO_DIR="$ROOT/iso"
DEFAULT_ISO="$ISO_DIR/MOH-FRONTLINE-USA.iso"

GAME_ID="GMFE69"
GAME_TITLE="Medal of Honor: Frontline"
ISO="${ISO:-$DEFAULT_ISO}"
EXPECTED_DOL_SHA256="${DOL_SHA256:-}"
BACKEND="${BACKEND:-c}"
JOBS="${JOBS:-$(nproc)}"
MODULE_OPT_LEVEL="${MODULE_OPT_LEVEL:-3}"
TOOLCHAIN="${TOOLCHAIN:-auto}"
RUNTIME_X11="${RUNTIME_X11:-OFF}"

# HPCOS-derived CPU profiles. `modern` stays portable across recent x86-64
# hosts; `native` is the maximum-performance local build.
MOH_PROFILE="${MOH_PROFILE:-modern}"
case "$MOH_PROFILE" in
  native)   MOH_MARCH_DEFAULT=native    ; MOH_RUNTIME_LTO_DEFAULT=ON  ; MOH_MEM_JOURNAL_DEFAULT=OFF ; MOH_SSP_DEFAULT=OFF ;;
  modern)   MOH_MARCH_DEFAULT=x86-64-v3 ; MOH_RUNTIME_LTO_DEFAULT=OFF ; MOH_MEM_JOURNAL_DEFAULT=OFF ; MOH_SSP_DEFAULT=OFF ;;
  compat)   MOH_MARCH_DEFAULT=x86-64-v2 ; MOH_RUNTIME_LTO_DEFAULT=OFF ; MOH_MEM_JOURNAL_DEFAULT=OFF ; MOH_SSP_DEFAULT=OFF ;;
  baseline) MOH_MARCH_DEFAULT=none      ; MOH_RUNTIME_LTO_DEFAULT=OFF ; MOH_MEM_JOURNAL_DEFAULT=OFF ; MOH_SSP_DEFAULT=OFF ;;
  lockstep) MOH_MARCH_DEFAULT=x86-64-v3 ; MOH_RUNTIME_LTO_DEFAULT=OFF ; MOH_MEM_JOURNAL_DEFAULT=ON  ; MOH_SSP_DEFAULT=ON  ;;
  *) echo "error: MOH_PROFILE must be native, modern, compat, baseline, or lockstep" >&2; exit 2 ;;
esac
MOH_MARCH="${MOH_MARCH:-$MOH_MARCH_DEFAULT}"
MOH_RUNTIME_LTO="${MOH_RUNTIME_LTO:-$MOH_RUNTIME_LTO_DEFAULT}"
MOH_MODULE_MEM_JOURNAL="${MOH_MODULE_MEM_JOURNAL:-$MOH_MEM_JOURNAL_DEFAULT}"
MOH_MODULE_STACK_PROTECTOR="${MOH_MODULE_STACK_PROTECTOR:-$MOH_SSP_DEFAULT}"
MOH_MODULE_IPO="${MOH_MODULE_IPO:-OFF}"

fail() {
  echo "error: $*" >&2
  exit 1
}

read_disc_id() {
  local path="$1"
  LC_ALL=C dd if="$path" bs=1 count=6 status=none 2>/dev/null || true
}

resolve_iso() {
  if [[ -f "$ISO" ]]; then
    return 0
  fi

  # If the default name is absent, accept exactly one user-supplied .iso in
  # iso/. This keeps setup convenient without silently picking between dumps.
  if [[ "$ISO" == "$DEFAULT_ISO" && -d "$ISO_DIR" ]]; then
    local -a candidates=()
    shopt -s nullglob nocaseglob
    candidates=("$ISO_DIR"/*.iso)
    shopt -u nullglob nocaseglob
    if (( ${#candidates[@]} == 1 )); then
      ISO="${candidates[0]}"
      return 0
    fi
    if (( ${#candidates[@]} > 1 )); then
      echo "error: multiple ISO files found in $ISO_DIR" >&2
      printf '  %s\n' "${candidates[@]}" >&2
      echo "set ISO=/path/to/your/GMFE69.iso ./build.sh to choose one" >&2
      exit 1
    fi
  fi

  return 1
}

if [[ "$BACKEND" != "c" ]]; then
  echo "error: the GMFE69 all-executable module currently requires BACKEND=c" >&2
  echo "       (ELF/DOL overlap needs generated-symbol namespacing before link)" >&2
  exit 2
fi
if [[ ! "$JOBS" =~ ^[1-9][0-9]*$ ]]; then
  echo "error: JOBS must be a positive integer" >&2
  exit 2
fi
if [[ ! "$MODULE_OPT_LEVEL" =~ ^[0-3]$ ]]; then
  echo "error: MODULE_OPT_LEVEL must be 0, 1, 2, or 3" >&2
  exit 2
fi
if [[ "$RUNTIME_X11" != "ON" && "$RUNTIME_X11" != "OFF" ]]; then
  echo "error: RUNTIME_X11 must be ON or OFF" >&2
  exit 2
fi
if [[ -n "$EXPECTED_DOL_SHA256" && ! "$EXPECTED_DOL_SHA256" =~ ^[0-9a-fA-F]{64}$ ]]; then
  echo "error: DOL_SHA256 must be a 64-character SHA-256 value" >&2
  exit 2
fi
[[ -f "$SOURCE/CMakeLists.txt" ]] || fail "local ModernGekko source is missing: $SOURCE"

if [[ "$TOOLCHAIN" == "auto" ]]; then
  if command -v clang >/dev/null 2>&1; then
    TOOLCHAIN="clang"
  else
    TOOLCHAIN="gcc"
  fi
fi
if [[ "$TOOLCHAIN" != "clang" && "$TOOLCHAIN" != "gcc" ]]; then
  echo "error: TOOLCHAIN must be auto, clang, or gcc" >&2
  exit 2
fi
if [[ "$BACKEND" == "llvm" && "$TOOLCHAIN" != "clang" ]]; then
  echo "error: BACKEND=llvm requires TOOLCHAIN=clang" >&2
  exit 2
fi

exec 9>"$ROOT/.build.lock"
if ! flock -n 9; then
  fail "another Medal of Honor: Frontline build is already running"
fi

mkdir -p "$ISO_DIR" "$ROOT/.cache/dolrecomp/llvm" "$ROOT/.cache/ccache" "$ROOT/.cache/sccache"

export XDG_CACHE_HOME="$ROOT/.cache"
export CMAKE_BUILD_PARALLEL_LEVEL="$JOBS"
export MODERNGEKKO_BUILD_JOBS="$JOBS"
export MODERNGEKKO_MODULE_OPT_LEVEL="$MODULE_OPT_LEVEL"
export DOLRECOMP_LLVM_CACHE="$ROOT/.cache/dolrecomp/llvm"
export CCACHE_DIR="$ROOT/.cache/ccache"
export SCCACHE_DIR="$ROOT/.cache/sccache"

LLVM_ENABLED=OFF

# Build the ISO extractor from DolRecomp directly. This intentionally avoids
# configuring the full Dolphin/ModernGekko frontend before the user's game has
# even been identified, and keeps ISO extraction independent of host graphics
# and window-system development packages.
echo "==> Building DolRecomp bootstrap extractor"
cmake -S "$BOOTSTRAP_RECOMP_SOURCE" -B "$BOOTSTRAP_RECOMP_BUILD" -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DDOLRECOMP_ENABLE_LLVM=OFF
cmake --build "$BOOTSTRAP_RECOMP_BUILD" --target dolrecomp -j "$JOBS"
BOOTSTRAP_DOLRECOMP="$BOOTSTRAP_RECOMP_BUILD/dolrecomp"
[[ -x "$BOOTSTRAP_DOLRECOMP" ]] || fail "bootstrap build did not produce dolrecomp"

# A clean checkout only needs the user's own USA GameCube ISO.
if [[ ! -f "$EXTRACTED/sys/main.dol" ]]; then
  if ! resolve_iso; then
    echo "error: no extracted game found and no ISO is available" >&2
    echo "place your own GMFE69 USA GameCube ISO at:" >&2
    echo "  $DEFAULT_ISO" >&2
    echo "or run:" >&2
    echo "  ISO=/path/to/your.iso ./build.sh" >&2
    exit 1
  fi

  ISO_DISC_ID="$(read_disc_id "$ISO")"
  if [[ "$ISO_DISC_ID" != "$GAME_ID" ]]; then
    echo "error: the selected disc image is not the supported USA release" >&2
    echo "expected disc ID: $GAME_ID" >&2
    echo "found disc ID:    ${ISO_DISC_ID:-<unreadable>}" >&2
    echo "ISO:              $ISO" >&2
    exit 1
  fi

  echo "==> Extracting user-supplied $GAME_TITLE ISO"
  echo "    ISO: $ISO"
  rm -rf "$EXTRACTED"
  "$BOOTSTRAP_DOLRECOMP" extract "$ISO" "$EXTRACTED"
fi

[[ -f "$EXTRACTED/sys/boot.bin" ]] || fail "extracted game is missing: $EXTRACTED/sys/boot.bin"
[[ -f "$EXTRACTED/sys/main.dol" ]] || fail "extracted game is missing: $EXTRACTED/sys/main.dol"
[[ -d "$EXTRACTED/files" ]] || fail "extracted game is missing: $EXTRACTED/files"

EXTRACTED_DISC_ID="$(read_disc_id "$EXTRACTED/sys/boot.bin")"
if [[ "$EXTRACTED_DISC_ID" != "$GAME_ID" ]]; then
  echo "error: extracted/ contains the wrong GameCube game" >&2
  echo "expected disc ID: $GAME_ID" >&2
  echo "found disc ID:    ${EXTRACTED_DISC_ID:-<unreadable>}" >&2
  echo "remove extracted/ and rerun ./build.sh with the GMFE69 ISO" >&2
  exit 1
fi

read -r ACTUAL_DOL_SHA256 _ < <(sha256sum "$EXTRACTED/sys/main.dol")
ACTUAL_DOL_SHA256="${ACTUAL_DOL_SHA256,,}"
if [[ -n "$EXPECTED_DOL_SHA256" && "${EXPECTED_DOL_SHA256,,}" != "$ACTUAL_DOL_SHA256" ]]; then
  echo "error: main.dol does not match the explicitly pinned DOL_SHA256" >&2
  echo "expected: ${EXPECTED_DOL_SHA256,,}" >&2
  echo "actual:   $ACTUAL_DOL_SHA256" >&2
  exit 1
fi

echo "==> Detected supported game"
echo "    disc ID:      $EXTRACTED_DISC_ID"
echo "    main.dol SHA: $ACTUAL_DOL_SHA256"
echo "    backend:      $BACKEND"
echo "    toolchain:    $TOOLCHAIN"
echo "    module opt:   O$MODULE_OPT_LEVEL"
echo "    jobs:         $JOBS"
echo "    X11 runtime:  $RUNTIME_X11"
echo "    CPU profile:  $MOH_PROFILE (march=$MOH_MARCH runtime-lto=$MOH_RUNTIME_LTO module-ipo=$MOH_MODULE_IPO)"

if [[ "$MOH_MARCH" == "none" ]]; then
  RUNTIME_ARCH_FLAGS="-ffp-contract=off"
else
  RUNTIME_ARCH_FLAGS="-march=$MOH_MARCH -ffp-contract=off"
fi

# Reconfigure with the exact extracted DOL hash. The resulting runtime will
# reject a different revision instead of accidentally loading the wrong native
# module against it.
cmake -S "$SOURCE" -B "$RUNTIME_BUILD" -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_C_FLAGS="$RUNTIME_ARCH_FLAGS" \
  -DCMAKE_CXX_FLAGS="$RUNTIME_ARCH_FLAGS" \
  -DENABLE_LTO="$MOH_RUNTIME_LTO" \
  -DBUILD_TESTING=OFF \
  -DMODERNGEKKO_REQUIRED_DISC_ID="$GAME_ID" \
  -DMODERNGEKKO_REQUIRED_DOL_SHA256="$ACTUAL_DOL_SHA256" \
  -DMODERNGEKKO_DEFAULT_WINDOW_TITLE="$GAME_TITLE" \
  -DENABLE_X11="$RUNTIME_X11" \
  -DDOLRECOMP_ENABLE_LLVM="$LLVM_ENABLED"

cmake --build "$RUNTIME_BUILD" \
  --target moderngekko-run \
  -j "$JOBS"

echo "==> Discovering and recompiling every GMFE69 DOL/ELF image"
MULTI_WORK="$PORT_BUILD/$GAME_ID/all-execs"
BUILT_MODULE="$MULTI_WORK/g${GAME_ID}_recomp.so"
rm -rf "$MULTI_WORK"
mkdir -p "$MULTI_WORK"
"$ROOT/tools/build_all_exec_module.py" \
  --extracted "$EXTRACTED" \
  --dolrecomp "$BOOTSTRAP_DOLRECOMP" \
  --project-root "$ROOT" \
  --work "$MULTI_WORK/work" \
  --output "$BUILT_MODULE" \
  --game-id "$GAME_ID" \
  --jobs "$JOBS" \
  --backend "$BACKEND" \
  --compiler "$TOOLCHAIN" \
  --opt-level "$MODULE_OPT_LEVEL" \
  --cmake-define "RECOMPCORE_MODULE_MARCH=$MOH_MARCH" \
  --cmake-define "RECOMPCORE_MODULE_ENABLE_IPO=$MOH_MODULE_IPO" \
  --cmake-define "RECOMPCORE_MODULE_ENABLE_MEM_JOURNAL=$MOH_MODULE_MEM_JOURNAL" \
  --cmake-define "RECOMPCORE_MODULE_STACK_PROTECTOR=$MOH_MODULE_STACK_PROTECTOR"
[[ -f "$BUILT_MODULE" ]] || fail "multi-image module build did not produce $BUILT_MODULE"
MULTI_REPORT="$MULTI_WORK/work/multi-image-report.json"
MULTI_SYMBOLS="$MULTI_WORK/work/symbols"
[[ -f "$MULTI_REPORT" ]] || fail "multi-image report is missing: $MULTI_REPORT"
[[ -f "$MULTI_SYMBOLS/function-symbols.json" ]] || fail "combined function-symbol inventory is missing: $MULTI_SYMBOLS/function-symbols.json"
SYMBOL_TOTAL="$(python3 - "$MULTI_REPORT" <<'PYREPORT'
import json, sys
print(json.load(open(sys.argv[1])).get("function_symbols_total", 0))
PYREPORT
)"
[[ -x "$RUNTIME_BUILD/moderngekko-run" ]] || fail "runtime build did not produce moderngekko-run"
[[ -d "$RUNTIME_BUILD/Sys" ]] || fail "runtime build did not produce its adjacent Sys directory"

STAGE="$(mktemp -d "$BUILD_ROOT/.publish.XXXXXX")"
mkdir -p "$STAGE/new-runtime/Sys" "$STAGE/new-module"
install -m 0755 "$RUNTIME_BUILD/moderngekko-run" "$STAGE/new-runtime/moderngekko-run"
cp -a "$RUNTIME_BUILD/Sys/." "$STAGE/new-runtime/Sys/"
install -m 0755 "$BUILT_MODULE" "$STAGE/new-module/g${GAME_ID}_recomp.so"
cp "$MULTI_REPORT" "$STAGE/new-module/multi-image-report.json"
cp -a "$MULTI_SYMBOLS" "$STAGE/new-module/symbols"
cat > "$STAGE/new-module/build-info.txt" <<INFO
Game: $GAME_TITLE
Disc ID: $GAME_ID
Boot DOL SHA-256: $ACTUAL_DOL_SHA256
Module mode: all DOL/ELF executables, overlap-safe ABI v5
Backend: $BACKEND
Toolchain: $TOOLCHAIN
Module optimization: O$MODULE_OPT_LEVEL
Recovered ELF function symbols: $SYMBOL_TOTAL
INFO

RUNTIME_TARGET="$ROOT/runtime"
MODULE_TARGET="$ROOT/module"
HAD_RUNTIME=0
HAD_MODULE=0
RUNTIME_PUBLISHED=0
MODULE_PUBLISHED=0
PUBLISH_COMPLETE=0

finish_publish() {
  status=$?
  trap - EXIT INT TERM HUP
  rollback_ok=1
  if [[ "$PUBLISH_COMPLETE" -ne 1 ]]; then
    if [[ "$RUNTIME_PUBLISHED" -eq 1 && -e "$RUNTIME_TARGET" ]]; then
      mv "$RUNTIME_TARGET" "$STAGE/failed-runtime" || rollback_ok=0
    fi
    if [[ "$MODULE_PUBLISHED" -eq 1 && -e "$MODULE_TARGET" ]]; then
      mv "$MODULE_TARGET" "$STAGE/failed-module" || rollback_ok=0
    fi
    if [[ "$HAD_RUNTIME" -eq 1 && -e "$STAGE/old-runtime" ]]; then
      mv "$STAGE/old-runtime" "$RUNTIME_TARGET" || rollback_ok=0
    fi
    if [[ "$HAD_MODULE" -eq 1 && -e "$STAGE/old-module" ]]; then
      mv "$STAGE/old-module" "$MODULE_TARGET" || rollback_ok=0
    fi
  fi

  if [[ "$rollback_ok" -eq 1 ]]; then
    rm -rf -- "$STAGE"
  else
    echo "error: publication rollback was incomplete; preserved $STAGE" >&2
  fi
  exit "$status"
}
trap finish_publish EXIT
trap 'exit 130' INT TERM HUP

if [[ -e "$RUNTIME_TARGET" ]]; then
  HAD_RUNTIME=1
  mv "$RUNTIME_TARGET" "$STAGE/old-runtime"
fi
if [[ -e "$MODULE_TARGET" ]]; then
  HAD_MODULE=1
  mv "$MODULE_TARGET" "$STAGE/old-module"
fi

RUNTIME_PUBLISHED=1
mv "$STAGE/new-runtime" "$RUNTIME_TARGET"
MODULE_PUBLISHED=1
mv "$STAGE/new-module" "$MODULE_TARGET"
PUBLISH_COMPLETE=1

echo "==> Build complete"
echo "published runtime: $ROOT/runtime/moderngekko-run"
echo "published module:  $ROOT/module/g${GAME_ID}_recomp.so"
echo "function symbols:  $ROOT/module/symbols/function-symbols.json ($SYMBOL_TOTAL functions)"
echo "run with:          $ROOT/run.sh"
