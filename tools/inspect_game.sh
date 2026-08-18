#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
GAME="$ROOT/extracted"
REPORT="$ROOT/module/multi-image-report.json"

[[ -f "$GAME/sys/boot.bin" ]] || { echo "error: extracted/sys/boot.bin is missing" >&2; exit 1; }
[[ -f "$GAME/sys/main.dol" ]] || { echo "error: extracted/sys/main.dol is missing" >&2; exit 1; }

DISC_ID="$(LC_ALL=C dd if="$GAME/sys/boot.bin" bs=1 count=6 status=none 2>/dev/null || true)"
printf 'Disc ID: %s\n\n' "$DISC_ID"

echo 'Executable images discovered:'
mapfile -d '' EXECS < <(find "$GAME" -type f \( -iname '*.dol' -o -iname '*.elf' \) -print0 | sort -z)
if (( ${#EXECS[@]} == 0 )); then
  echo '  <none>'
  exit 1
fi

for path in "${EXECS[@]}"; do
  rel="${path#"$GAME/"}"
  size="$(stat -c '%s' "$path")"
  hash="$(sha256sum "$path" | awk '{print $1}')"
  printf '  %-30s %10s bytes  %s\n' "$rel" "$size" "$hash"
  if [[ "${path,,}" == *.elf ]]; then
    sym_count="$(python3 "$ROOT/tools/elf_symbols.py" "$path" --count 2>/dev/null || echo '?')"
    printf '    ELF named function symbols: %s\n' "$sym_count"
    if [[ -x "$(command -v readelf || true)" ]]; then
      entry="$(readelf -h "$path" 2>/dev/null | awk -F: '/Entry point address/ {gsub(/^[ \t]+/,"",$2); print $2; exit}')"
      [[ -n "$entry" ]] && printf '    ELF entry: %s\n' "$entry"
    fi
  fi
done

MAIN_HASH="$(sha256sum "$GAME/sys/main.dol" | awk '{print $1}')"
for path in "${EXECS[@]}"; do
  [[ "$path" == "$GAME/sys/main.dol" ]] && continue
  hash="$(sha256sum "$path" | awk '{print $1}')"
  if [[ "$hash" == "$MAIN_HASH" ]]; then
    echo
    echo "Exact duplicate of sys/main.dol: ${path#"$GAME/"}"
  fi
done

if [[ -f "$REPORT" ]]; then
  echo
  echo 'Published multi-image module report:'
  python3 - "$REPORT" <<'PY'
import json, sys
r=json.load(open(sys.argv[1]))
print(f"  unique executable images: {len(r['images'])}")
print(f"  canonical native chunks:  {r['canonical_chunks']}")
print(f"  overlap chunks:           {r['overlap_chunks']}")
print(f"  recovered function names: {r.get('function_symbols_total', 0)}")
for img in r['images']:
    alias = f" (aliases: {', '.join(img['aliases'])})" if img.get('aliases') else ''
    syms = img.get('function_symbols', 0)
    print(f"  image {img['index']}: {img['source']} -> {img['generated_chunks']} generated chunks, {syms} named functions{alias}")
PY
fi
