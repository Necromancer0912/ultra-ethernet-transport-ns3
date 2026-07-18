#!/usr/bin/env bash
# Run the deterministic UET verification suite (11 tests, 86 checks).
# Exit code 0 means every check passed.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BIN="$ROOT/simulation/build/scratch/ns3.36.1-uet-tests-debug"

if [[ ! -x "$BIN" ]]; then
    echo "[tests] binary not found, building first..."
    "$ROOT/scripts/build.sh"
fi

"$BIN"
