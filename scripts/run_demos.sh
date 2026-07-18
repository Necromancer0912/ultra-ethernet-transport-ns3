#!/usr/bin/env bash
# Run every UET demonstration program in sequence.
# Usage: scripts/run_demos.sh [complete|hpc|network|drop|advanced|phase4|all]
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BINDIR="$ROOT/simulation/build/scratch"
WHICH="${1:-all}"

need_build() {
    if [[ ! -x "$BINDIR/ns3.36.1-uet-tests-debug" ]]; then
        "$ROOT/scripts/build.sh"
    fi
}
need_build

run() {
    echo ""
    echo "================================================================"
    echo "  $1"
    echo "================================================================"
    shift
    "$@"
}

if [[ "$WHICH" == "complete" || "$WHICH" == "all" ]]; then
    run "Complete SES/PDS/PDC walkthrough (4-node fabric)" \
        "$BINDIR/ns3.36.1-uet-complete-demo-debug"
fi

if [[ "$WHICH" == "hpc" || "$WHICH" == "all" ]]; then
    run "AI Base / AI Full / HPC profile workloads (8 nodes)" \
        "$BINDIR/ns3.36.1-uet-hpc-ai-profiles-debug"
fi

if [[ "$WHICH" == "network" || "$WHICH" == "all" ]]; then
    run "Point-to-point benchmark, lossless (200 Gbps)" \
        "$BINDIR/ns3.36.1-uet-network-sim-debug" --numMsgs=2000 --msgSize=8192 --seed=1
    run "Point-to-point benchmark, 1 percent injected loss" \
        "$BINDIR/ns3.36.1-uet-network-sim-debug" --numMsgs=2000 --msgSize=8192 --seed=1 --lossRate=0.01
fi

if [[ "$WHICH" == "drop" || "$WHICH" == "all" ]]; then
    run "Deterministic drop injection and recovery" \
        "$BINDIR/ns3.36.1-uet-ses-pds-demo-debug" \
        --messageBytes=8000 --mtuBytes=1000 --dropReqPsns=1,3 --dropAckPsns=0 --mode=rud
fi

if [[ "$WHICH" == "advanced" || "$WHICH" == "all" ]]; then
    run "Control packets and target state scenarios" \
        "$BINDIR/ns3.36.1-uet-advanced-demo-debug" --scenario=all
fi

if [[ "$WHICH" == "phase4" || "$WHICH" == "all" ]]; then
    run "RDMA hardware integration layer scenarios" \
        "$BINDIR/ns3.36.1-uet-phase4-rdma-integration-debug" --scenario=all
fi
