#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
GAME_ID="GMFE69"
RUNTIME="$ROOT/runtime/moderngekko-run"
MODULE="$ROOT/module/g${GAME_ID}_recomp.so"
GAME="$ROOT/extracted"
USER_DIR="$ROOT/user"

FOV="default"
WEAPON_FOV="follow"
ASPECT="default"
FPS="default"
HUD_MODE="safe"
MOUSE_SENS="default"
PC_INPUT=1
PASS_ARGS=()

usage() {
  cat <<'USAGE'
Usage: ./run.sh [MOH options] [ModernGekko options]

MOH Frontline PC enhancements:
  --fov <degrees|default>     Final horizontal world FOV. Example: --fov 90
  --weapon-fov <degrees|follow>
                              Viewmodel FOV. Omitted/follow = follows --fov
  --aspect <ratio|auto|default>
                              default/original, auto, 4:3, 16:10, 16:9,
                              21:9, 32:9, WIDTH:HEIGHT or WIDTHxHEIGHT
  --fps <target|default>      default, 30, 60, 90, 120, 144, 165, 240,
                              any 1-1000 value, or unlimited
  --hud <safe|stretch>        Aspect-correct 4:3 HUD/menu safe area (default: safe)
  --mouse-sensitivity <value> Native mouse sensitivity, 0.05-10.0
  --no-pc-input               Disable the keyboard/mouse FPS layer
  --moh-help                  Show this help and exit

PC controls (default): WASD move, mouse look, LMB fire, RMB aim, E use,
R reload, F melee, Space jump, C/Ctrl crouch, wheel/1/2 weapons, Esc pause.
Ctrl+F10 or ` opens the in-game PC settings menu.

Examples:
  ./run.sh
  ./run.sh --aspect 16:9
  ./run.sh --aspect 21:9 --fov 100 --fps 144
  ./run.sh --aspect 3440x1440 --fps unlimited
  MOH_OUTPUT_SIZE=5120x1440 ./run.sh --aspect auto --fps 240

With no MOH options, the original game FOV/aspect/FPS behavior is preserved.
USAGE
}

need_value() {
  local opt="$1"
  if (($# < 2)) || [[ -z "${2:-}" ]]; then
    echo "error: $opt requires a value" >&2
    exit 2
  fi
}

while (($#)); do
  case "$1" in
    --fov)
      need_value "$1" "${2:-}"
      FOV="$2"
      shift 2
      ;;
    --weapon-fov)
      need_value "$1" "${2:-}"
      WEAPON_FOV="$2"
      shift 2
      ;;
    --aspect)
      need_value "$1" "${2:-}"
      ASPECT="$2"
      shift 2
      ;;
    --fps)
      need_value "$1" "${2:-}"
      FPS="$2"
      shift 2
      ;;
    --hud)
      need_value "$1" "${2:-}"
      HUD_MODE="${2,,}"
      shift 2
      ;;
    --mouse-sensitivity)
      need_value "$1" "${2:-}"
      MOUSE_SENS="$2"
      shift 2
      ;;
    --no-pc-input)
      PC_INPUT=0
      shift
      ;;
    --moh-help)
      usage
      exit 0
      ;;
    *)
      PASS_ARGS+=("$1")
      shift
      ;;
  esac
done

# New-game bring-up starts with no game-specific forced interpreter range.
if [[ -n "${STATICRECOMP_FALLBACK_RANGES:-}" ]]; then
  export STATICRECOMP_FALLBACK_RANGES
else
  unset STATICRECOMP_FALLBACK_RANGES 2>/dev/null || true
fi
export STATICRECOMP_NATIVE_BURST="${STATICRECOMP_NATIVE_BURST:-1}"

if [[ ! -x "$RUNTIME" ]]; then
  echo "error: local runtime is missing: $RUNTIME" >&2
  echo "run $ROOT/build.sh first" >&2
  exit 1
fi
if [[ ! -f "$MODULE" ]]; then
  echo "error: local $GAME_ID module is missing: $MODULE" >&2
  echo "run $ROOT/build.sh first" >&2
  exit 1
fi
if [[ ! -f "$GAME/sys/main.dol" ]]; then
  echo "error: extracted Medal of Honor: Frontline game is missing" >&2
  echo "run $ROOT/build.sh after placing your own GMFE69 ISO in iso/" >&2
  exit 1
fi
mkdir -p "$USER_DIR"

export MOH_PC_SETTINGS_PATH="$USER_DIR/moh_pc_settings.ini"
export MOH_PC_INPUT="$PC_INPUT"

unset MOH_CAMERA_PATCH MOH_TIMING_PATCH MOH_FOV_DEGREES MOH_WEAPON_FOV_DEGREES \
      MOH_ASPECT_VALUE MOH_ASPECT_NUM MOH_ASPECT_DEN MOH_ASPECT_AUTO MOH_FPS_TARGET \
      MOH_UI_SAFE MOH_MOUSE_SENSITIVITY 2>/dev/null || true

# FOV: an explicit value is the final horizontal FOV on the selected aspect.
case "${FOV,,}" in
  default|original)
    ;;
  *)
    if ! awk -v v="$FOV" 'BEGIN { exit !(v+0==v && v>=20 && v<179) }'; then
      echo "error: --fov must be default or a number from 20 to <179" >&2
      exit 2
    fi
    export MOH_CAMERA_PATCH=1
    export MOH_FOV_DEGREES="$FOV"
    ;;
esac

case "${WEAPON_FOV,,}" in
  follow)
    ;;
  *)
    if ! awk -v v="$WEAPON_FOV" 'BEGIN { exit !(v+0==v && v>=20 && v<179) }'; then
      echo "error: --weapon-fov must be follow or a number from 20 to <179" >&2
      exit 2
    fi
    export MOH_CAMERA_PATCH=1
    export MOH_WEAPON_FOV_DEGREES="$WEAPON_FOV"
    ;;
esac

# Return WIDTH HEIGHT for an aspect token.
resolve_aspect() {
  local raw="${1,,}"
  case "$raw" in
    default|original|4:3) echo "4 3" ; return ;;
    16:9)  echo "16 9" ; return ;;
    16:10) echo "16 10"; return ;;
    21:9)  echo "21 9" ; return ;;
    32:9)  echo "32 9" ; return ;;
    auto)
      local size="${MOH_OUTPUT_SIZE:-}"
      if [[ -z "$size" ]] && command -v xrandr >/dev/null 2>&1 && [[ -n "${DISPLAY:-}" ]]; then
        size="$(xrandr --current 2>/dev/null | sed -nE 's/.*current ([0-9]+) x ([0-9]+).*/\1x\2/p' | head -n1)"
      fi
      if [[ -z "$size" ]] && command -v xdpyinfo >/dev/null 2>&1 && [[ -n "${DISPLAY:-}" ]]; then
        size="$(xdpyinfo 2>/dev/null | sed -nE 's/^[[:space:]]*dimensions:[[:space:]]*([0-9]+)x([0-9]+).*/\1x\2/p' | head -n1)"
      fi
      if [[ -z "$size" ]]; then
        echo "warning: --aspect auto could not detect the output size; using 16:9" >&2
        echo "warning: set MOH_OUTPUT_SIZE=3440x1440 (for example) for exact auto detection" >&2
        echo "16 9"
        return
      fi
      raw="${size,,}"
      ;;
  esac

  if [[ "$raw" =~ ^([0-9]+)[x:]([0-9]+)$ ]]; then
    local w="${BASH_REMATCH[1]}" h="${BASH_REMATCH[2]}"
    if (( w > 0 && h > 0 )); then
      echo "$w $h"
      return
    fi
  fi
  echo "error: invalid aspect '$1'" >&2
  exit 2
}

if [[ "${ASPECT,,}" != "default" && "${ASPECT,,}" != "original" ]]; then
  if [[ "${ASPECT,,}" == "auto" ]]; then export MOH_ASPECT_AUTO=1; fi
  read -r ASPECT_W ASPECT_H < <(resolve_aspect "$ASPECT")
  export MOH_CAMERA_PATCH=1
  export MOH_ASPECT_NUM="$ASPECT_W"
  export MOH_ASPECT_DEN="$ASPECT_H"
  export MOH_ASPECT_VALUE="$(awk -v w="$ASPECT_W" -v h="$ASPECT_H" 'BEGIN { printf "%.12f", w/h }')"
elif [[ -n "${MOH_FOV_DEGREES:-}" ]]; then
  # FOV-only mode keeps the original 4:3 presentation.
  export MOH_ASPECT_VALUE="1.333333333333"
fi

case "$HUD_MODE" in
  safe) export MOH_UI_SAFE=1 ;;
  stretch) export MOH_UI_SAFE=0 ;;
  *) echo "error: --hud must be safe or stretch" >&2; exit 2 ;;
esac

if [[ "$MOUSE_SENS" != "default" ]]; then
  if ! awk -v v="$MOUSE_SENS" 'BEGIN { exit !(v+0==v && v>=0.05 && v<=10.0) }'; then
    echo "error: --mouse-sensitivity must be from 0.05 to 10.0" >&2
    exit 2
  fi
  export MOH_MOUSE_SENSITIVITY="$MOUSE_SENS"
fi

case "${FPS,,}" in
  default|original)
    ;;
  unlimited)
    export MOH_TIMING_PATCH=1
    export MOH_FPS_TARGET=unlimited
    ;;
  *)
    if ! [[ "$FPS" =~ ^[0-9]+([.][0-9]+)?$ ]] || \
       ! awk -v v="$FPS" 'BEGIN { exit !(v>=1 && v<=1000) }'; then
      echo "error: --fps must be default, unlimited, or a value from 1 to 1000" >&2
      exit 2
    fi
    export MOH_TIMING_PATCH=1
    export MOH_FPS_TARGET="$FPS"
    ;;
esac

if [[ -n "${MOH_CAMERA_PATCH:-}" || -n "${MOH_TIMING_PATCH:-}" ]]; then
  echo "MOH native enhancements: aspect=${ASPECT} fov=${FOV} weapon-fov=${WEAPON_FOV} fps=${FPS}"
fi

exec "$RUNTIME" \
  --game "$GAME" \
  --module "$MODULE" \
  --user-dir "$USER_DIR" \
  --graphics Vulkan \
  --wayland \
  "${PASS_ARGS[@]}"
