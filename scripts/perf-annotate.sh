#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# When downloaded into the repo root, ROOT is already correct. When installed
# under scripts/, use its parent as the project root.
if [[ ! -x "$ROOT/run.sh" && -x "$ROOT/../run.sh" ]]; then
  ROOT="$(cd "$ROOT/.." && pwd)"
fi

RUNNER="$ROOT/run.sh"
RUNTIME="$ROOT/runtime/moderngekko-run"
MODULE="$ROOT/module/gGMFE69_recomp.so"

PERF_FREQ="${PERF_FREQ:-997}"
PERF_EVENT="${PERF_EVENT:-cycles:u}"
PERF_DWARF_STACK="${PERF_DWARF_STACK:-16384}"
STAMP="$(date +%Y%m%d-%H%M%S)"
OUT="${PERF_OUT:-$ROOT/perf-moh-$STAMP}"
DATA="$OUT/perf.data"

fail() {
  echo "error: $*" >&2
  exit 1
}

command -v perf >/dev/null 2>&1 || fail "perf is not installed (Arch: sudo pacman -S perf)"
[[ "$(uname -s)" == "Linux" ]] || fail "this profiler is Linux-only"
[[ -x "$RUNNER" ]] || fail "missing executable: $RUNNER"
[[ -x "$RUNTIME" ]] || fail "missing runtime: $RUNTIME (run ./build.sh first)"
[[ -f "$MODULE" ]] || fail "missing module: $MODULE (run ./build.sh first)"

mkdir -p "$OUT"

{
  echo "date: $(date --iso-8601=seconds 2>/dev/null || date)"
  echo "kernel: $(uname -a)"
  echo "perf: $(perf version 2>/dev/null || true)"
  echo "event: $PERF_EVENT"
  echo "frequency: $PERF_FREQ"
  echo "dwarf_stack: $PERF_DWARF_STACK"
  echo "runtime: $RUNTIME"
  echo "module: $MODULE"
  if [[ -r /proc/sys/kernel/perf_event_paranoid ]]; then
    echo "perf_event_paranoid: $(cat /proc/sys/kernel/perf_event_paranoid)"
  fi
  echo
  command -v lscpu >/dev/null 2>&1 && lscpu
} > "$OUT/system.txt"

# Check whether the preferred hardware event is available before launching the
# game. Fall back to the software CPU clock event when permissions/PMU access
# reject cycles:u.
PROBE="$OUT/.perf-probe.data"
if ! perf record -q -o "$PROBE" -e "$PERF_EVENT" -c 100000 -- true 2>/dev/null; then
  echo "warning: '$PERF_EVENT' is not available; falling back to cpu-clock:u" >&2
  PERF_EVENT="cpu-clock:u"
fi
rm -f "$PROBE"

cat <<MSG
==> MOH Frontline perf capture
    event:      $PERF_EVENT
    frequency:  $PERF_FREQ Hz
    call graph: DWARF (${PERF_DWARF_STACK} bytes/sample)
    output:     $OUT

Play a representative section (preferably the same level/scene each run).
Quit the game normally when you have enough samples; perf will then generate
report + annotate files automatically.
MSG

echo "==> Recording"
set +e
perf record \
  -o "$DATA" \
  -e "$PERF_EVENT" \
  -F "$PERF_FREQ" \
  --call-graph "dwarf,$PERF_DWARF_STACK" \
  -- "$RUNNER" "$@"
record_rc=$?
set -e

if [[ ! -s "$DATA" ]]; then
  fail "perf produced no data (record exit code $record_rc)"
fi

# Keep report generation tolerant: one unsupported presentation option should
# not destroy a useful capture.
echo "==> Generating flat hotspot report"
perf report \
  -i "$DATA" \
  --stdio \
  --no-children \
  --show-nr-samples \
  --sort dso,symbol \
  > "$OUT/perf-report-flat.txt" 2> "$OUT/perf-report-flat.err" || true

echo "==> Generating call-graph report"
perf report \
  -i "$DATA" \
  --stdio \
  --show-nr-samples \
  --sort dso,symbol \
  > "$OUT/perf-report-callgraph.txt" 2> "$OUT/perf-report-callgraph.err" || true

echo "==> Generating annotate output (assembly + source when debug info exists)"
perf annotate \
  -i "$DATA" \
  --stdio \
  > "$OUT/perf-annotate.txt" 2> "$OUT/perf-annotate.err" || true

# A compact view focused on the two pieces we control: the ModernGekko runtime
# and the generated GMFE69 static-recomp module.
{
  echo "# Runtime/module hotspots extracted from perf-report-flat.txt"
  echo "# Look for high Self% rows in moderngekko-run and gGMFE69_recomp.so."
  echo
  grep -E 'moderngekko-run|gGMFE69_recomp\.so' "$OUT/perf-report-flat.txt" || true
} > "$OUT/perf-hotspots-moh.txt"

perf buildid-list -i "$DATA" > "$OUT/perf-buildids.txt" 2>/dev/null || true

# Preserve project metadata that helps map ppc/generated symbols back to the
# static-recomp inventory without copying the large binaries.
if [[ -f "$ROOT/module/multi-image-report.json" ]]; then
  cp "$ROOT/module/multi-image-report.json" "$OUT/"
fi
if [[ -f "$ROOT/module/build-info.txt" ]]; then
  cp "$ROOT/module/build-info.txt" "$OUT/"
fi
if [[ -f "$ROOT/module/symbols/function-symbols.json" ]]; then
  cp "$ROOT/module/symbols/function-symbols.json" "$OUT/"
fi

if command -v git >/dev/null 2>&1 && git -C "$ROOT" rev-parse --is-inside-work-tree >/dev/null 2>&1; then
  {
    git -C "$ROOT" status --short
    echo
    git -C "$ROOT" rev-parse HEAD 2>/dev/null || true
    git -C "$ROOT" diff --stat 2>/dev/null || true
  } > "$OUT/git-state.txt"
fi

# Small bundle suitable for sending for analysis. perf.data is intentionally
# kept outside the archive by default because it can be very large; set
# PERF_BUNDLE_DATA=1 to include it.
BUNDLE="$OUT/perf-moh-analysis.tar.gz"
files=(
  system.txt
  perf-report-flat.txt
  perf-report-callgraph.txt
  perf-annotate.txt
  perf-hotspots-moh.txt
  perf-buildids.txt
)
for f in multi-image-report.json build-info.txt function-symbols.json git-state.txt; do
  [[ -f "$OUT/$f" ]] && files+=("$f")
done
if [[ "${PERF_BUNDLE_DATA:-0}" == "1" ]]; then
  files+=(perf.data)
fi
(
  cd "$OUT"
  tar -czf "$BUNDLE" "${files[@]}"
)

echo
echo "==> Done"
echo "Flat report: $OUT/perf-report-flat.txt"
echo "MOH hotspots: $OUT/perf-hotspots-moh.txt"
echo "Annotate:    $OUT/perf-annotate.txt"
echo "Bundle:      $BUNDLE"

if [[ "$record_rc" -ne 0 ]]; then
  echo "note: game/perf exited with status $record_rc, but the capture was usable" >&2
fi
