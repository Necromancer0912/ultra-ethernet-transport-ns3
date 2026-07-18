#!/usr/bin/env bash
# Configure and build the ns-3 tree including all UET targets.
# Usage: scripts/build.sh [--clean]
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
SIM="$ROOT/simulation"

if [[ "${1:-}" == "--clean" ]]; then
    echo "[build] removing previous build artifacts"
    rm -rf "$SIM/build" "$SIM/cmake-cache" "$SIM/.lock-ns3_darwin_build" "$SIM/.lock-ns3_linux_build"
fi

cd "$SIM"

if [[ ! -f build/.ns3_configured ]]; then
    ./ns3 configure --build-profile=debug
    mkdir -p build
    touch build/.ns3_configured
fi

./ns3 build
echo "[build] done. Binaries are in simulation/build/scratch/"
