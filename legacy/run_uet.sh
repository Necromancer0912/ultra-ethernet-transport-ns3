#!/usr/bin/env bash
# ═══════════════════════════════════════════════════════════════════════════
#  run_uet.sh — Ultra Ethernet Transport Simulator — One-Click Runner
#  Usage: ./run_uet.sh [scenario] [--ui]
#  Scenarios: all | aibase | aifull | hpc | complete | network
# ═══════════════════════════════════════════════════════════════════════════

set -euo pipefail

# ── Colors ──────────────────────────────────────────────────────────────────
RST="\033[0m"
BOLD="\033[1m"
CYAN="\033[1;36m"
GREEN="\033[1;32m"
YELL="\033[1;33m"
RED="\033[1;31m"
DIM="\033[2m"

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
SIM_DIR="$SCRIPT_DIR/simulation"
DASH_DIR="$SCRIPT_DIR/dashboard"

SCENARIO="${1:-all}"
OPEN_UI="${2:-}"

banner() {
  echo ""
  echo -e "${CYAN}════════════════════════════════════════════════════════════════════════════════${RST}"
  echo -e "${CYAN}${BOLD}  Ultra Ethernet (UET) Transport Simulator${RST}"
  echo -e "${CYAN}  UE-Specification-1.0.2 · NS-3 v3.36.1 · AI Base / AI Full / HPC${RST}"
  echo -e "${CYAN}════════════════════════════════════════════════════════════════════════════════${RST}"
  echo ""
}

check_build() {
  local bin="$SIM_DIR/build/scratch/ns3.36.1-uet-hpc-ai-profiles-debug"
  local bin2="$SIM_DIR/build/scratch/ns3.36.1-uet-complete-demo-debug"

  if [[ ! -f "$bin" || ! -f "$bin2" ]]; then
    echo -e "${YELL}  Building simulation binaries...${RST}"
    pushd "$SIM_DIR" > /dev/null
    ./ns3 configure --build-profile=debug 2>&1 | grep -E "Configuring|Build profile" || true
    ./ns3 build scratch/uet-hpc-ai-profiles 2>&1 | tail -3
    ./ns3 build scratch/uet-complete-demo 2>&1 | tail -3
    popd > /dev/null
    echo -e "${GREEN}  ✓ Build complete${RST}"
  else
    echo -e "${GREEN}  ✓ Binaries found (pre-built)${RST}"
  fi
}

run_profiles() {
  echo -e "${CYAN}  Running: AI Base / AI Full / HPC Profile Demo${RST}"
  echo -e "${DIM}  Command: uet-hpc-ai-profiles --scenario=$SCENARIO${RST}"
  echo ""
  "$SIM_DIR/build/scratch/ns3.36.1-uet-hpc-ai-profiles-debug" --scenario="$SCENARIO" 2>/dev/null
}

run_complete() {
  echo -e "${CYAN}  Running: Complete SES/PDS/PDC Stack Demo${RST}"
  echo ""
  "$SIM_DIR/build/scratch/ns3.36.1-uet-complete-demo-debug" 2>/dev/null
}

open_dashboard() {
  echo ""
  echo -e "${GREEN}  Opening dashboard: $DASH_DIR/index.html${RST}"
  if command -v open &>/dev/null; then
    open "$DASH_DIR/index.html"
  elif command -v xdg-open &>/dev/null; then
    xdg-open "$DASH_DIR/index.html"
  else
    echo -e "${YELL}  → Open manually: $DASH_DIR/index.html${RST}"
  fi
}

# ── Main ─────────────────────────────────────────────────────────────────────
banner
check_build

echo ""
case "$SCENARIO" in
  all|aibase|aifull|hpc)
    run_profiles
    ;;
  complete)
    run_complete
    ;;
  network)
    echo -e "${CYAN}  Running: Network Stress Test (200Gbps fat-tree, 8 nodes)${RST}"
    echo ""
    "$SIM_DIR/build/scratch/ns3.36.1-uet-hpc-ai-profiles-debug" --scenario=all 2>/dev/null
    ;;
  dashboard|ui)
    open_dashboard
    exit 0
    ;;
  *)
    echo -e "${RED}  Unknown scenario: $SCENARIO${RST}"
    echo -e "  Usage: $0 [all|aibase|aifull|hpc|complete|network|dashboard]"
    exit 1
    ;;
esac

echo ""
echo -e "${GREEN}  ✓ Simulation complete.${RST}"

# Auto-open dashboard if requested
if [[ "$OPEN_UI" == "--ui" || "$OPEN_UI" == "--dashboard" ]]; then
  open_dashboard
fi

echo ""
echo -e "${DIM}  Output log: $SIM_DIR/pdc_state_dump.log${RST}"
echo -e "${DIM}  Dashboard:  $DASH_DIR/index.html${RST}"
echo ""
