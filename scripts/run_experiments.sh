#!/usr/bin/env bash
# Reproduce the evaluation sweep reported in docs/REPORT.md.
# Runs the point-to-point benchmark across delivery modes and loss rates
# with a fixed seed, writing one output file per configuration to results/.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BIN="$ROOT/simulation/build/scratch/ns3.36.1-uet-network-sim-debug"
OUT="$ROOT/results"

if [[ ! -x "$BIN" ]]; then
    "$ROOT/scripts/build.sh"
fi

mkdir -p "$OUT"
SEED="${SEED:-1}"

for mode in RUD ROD; do
    for loss in 0 0.001 0.01 0.05; do
        name="${mode}_loss${loss}_seed${SEED}"
        echo "[sweep] $name"
        "$BIN" --numMsgs=2000 --msgSize=8192 --seed="$SEED" \
               --mode="$mode" --lossRate="$loss" > "$OUT/$name.txt" 2>&1
    done
done

echo "[sweep] RUDI_loss0.01_seed${SEED}"
"$BIN" --numMsgs=2000 --msgSize=4096 --seed="$SEED" \
       --mode=RUDI --lossRate=0.01 > "$OUT/RUDI_loss0.01_seed${SEED}.txt" 2>&1

echo ""
echo "Summary (completion / goodput per configuration):"
for f in "$OUT"/*_seed${SEED}.txt; do
    printf "%-28s " "$(basename "$f" .txt)"
    grep -E "Messages Completed:" "$f" | sed 's/^ *//' | tr -d '\n'
    printf "  |  "
    grep -E "Achieved Goodput:" "$f" | sed 's/^ *//'
done
