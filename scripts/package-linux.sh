#!/usr/bin/env bash
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
OUT="${1:-$ROOT/dist/MOHFrontline-PC-linux-x86_64}"
rm -rf "$OUT"
mkdir -p "$OUT/runtime" "$OUT/module" "$OUT/extracted" "$OUT/user"
install -m755 "$ROOT/runtime/moderngekko-run" "$OUT/runtime/moderngekko-run"
cp -a "$ROOT/runtime/Sys" "$OUT/runtime/" 2>/dev/null || true
install -m755 "$ROOT/module/gGMFE69_recomp.so" "$OUT/module/gGMFE69_recomp.so"
cp "$ROOT/run.sh" "$ROOT/LICENSE" "$OUT/"
cat > "$OUT/extracted/README.md" <<'TXT'
Extract your legally owned USA GameCube release of Medal of Honor: Frontline (GMFE69) here.
Retail game data is intentionally not included in release packages.
TXT
mkdir -p "$(dirname "$OUT")"
tar -C "$(dirname "$OUT")" -cJf "$OUT.tar.xz" "$(basename "$OUT")"
echo "Created $OUT.tar.xz"
