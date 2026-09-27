#!/usr/bin/env bash
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cmake -S "$ROOT/launcher" -B "$ROOT/build/launcher" -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build "$ROOT/build/launcher" --target MOHFrontline-Launcher -j "${JOBS:-$(nproc)}"
echo "Launcher: $ROOT/launcher/bin/MOHFrontline-Launcher"
