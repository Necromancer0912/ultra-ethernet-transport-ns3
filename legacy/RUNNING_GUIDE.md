# UET Simulation Running Guide

This guide describes how to configure, compile, and execute the Ultra Ethernet (UET) protocol simulator built into the `ns-3-alibabacloud` module.

## Prerequisites

- **OS:** Linux or macOS
- **Compiler:** GCC or Clang (C++17 or later support required)
- **CMake:** Version 3.10 or later

## Build Workflow Choice (Important)

This project supports two valid build workflows. Pick one and stay consistent:

- **Workflow A (ns-3 wrapper):** `./ns3 configure` + `./ns3 build` + `./ns3 run` (default build dir: `build/`)
- **Workflow B (direct CMake):** `cmake -S . -B cmake-build` + `cmake --build cmake-build` + run binaries

If you configure with `./ns3 configure` only, `cmake-build/` may not exist yet.

### Quick fix for this exact error

If you see:
`Error: .../simulation/cmake-build is not a directory`

Run:

```bash
cd ns-3-alibabacloud/simulation
cmake -S . -B cmake-build
cmake --build cmake-build -j$(sysctl -n hw.ncpu)
```

## 1. Directory Structure

The UET protocol implementation resides within the `point-to-point` module of `ns-3-alibabacloud`.

- **Headers/Model Directory:** `ns-3-alibabacloud/simulation/src/point-to-point/model/`
  - `uet-pds-header.h/cc`
  - `uet-ses-header.h/cc`
  - `uet-pdc.h/cc`
  - `uet-ses-pds-engine.h/cc`
- **Test/Demo Application:** `ns-3-alibabacloud/simulation/scratch/uet-complete-demo.cc`

## 2. Compiling the Simulator

The repository uses standard CMake to build the `ns3` framework. From the project root, navigate to the `simulation` folder and compile using `_build_test`:

```bash
cd ns-3-alibabacloud/simulation

# Configure the build (only required the first time or if CMakeLists change)
cmake -B _build_test

# Compile the specific UET integration demo
cmake --build _build_test --target scratch_uet-complete-demo
```

### Preferred modern compile path (`cmake-build`)

```bash
cd ns-3-alibabacloud/simulation
cmake -S . -B cmake-build
cmake --build cmake-build -j$(sysctl -n hw.ncpu)
```

### Wrapper compile path (`build/`)

```bash
cd ns-3-alibabacloud/simulation
./ns3 configure --build-profile=optimized
./ns3 build -j $(sysctl -n hw.ncpu)
```

> **Note:** The `-Werror` flags have been strictly maintained across the legacy LTE modules. The compilation process handles all warnings safely without skipping verification flags.

## 3. Running the Simulation

You can execute the demo simulation using the `ns3` test runner payload:

```bash
./ns3 run scratch/uet-complete-demo
```

_Alternatively, you can run the binary directly from the build folder:_

```bash
./_build_test/scratch/uet-complete-demo
```

_If your build outputs versioned binaries, use the explicit binary name from `build/scratch` or `_build_test/scratch`:_

```bash
./build/scratch/ns3.36.1-uet-complete-demo-default
# or
./_build_test/scratch/ns3.36.1-uet-complete-demo-default
```

### Enabling Debug Logs

If you want to view verbose internal packet serialization, PSN assignments, or PDC State Machine transitions, you can optionally enable the `UetPdcManager` log component:

```bash
export NS_LOG="UetPdcManager=logic"
./ns3 run scratch/uet-complete-demo
```

## 4. Understanding the Execution Phases

The `uet-complete-demo.cc` executes **9 automated testing phases** representing different components of the UE Specification exactly as laid out in the whitepapers:

1. **Phase 1: SES Header Formats**
   - Validates serialization sizes against Spec Tables 3-8 through 3-14.
2. **Phase 2: PDS Formats & NACK Codes**
   - Encodes and decrypts RUD, ROD, UUD wires mapping exactly to Table 3-58 definitions.
3. **Phase 3: PDC Establishment (SYN Handshake) + RUD Transfer**
   - Automatically simulates N0 to N1 opening a transaction mapping to `IPDCID`/`TPDCID`.
4. **Phase 4: ROD READ (Reliable Ordered Delivery)**
   - Tests strict out-of-order drop sequences across `Start_PSN` alignment.
5. **Phase 5: RUDI Datagram**
   - Asserts reliable unordered packet routing without holding persistent PDC memory.
6. **Phase 6: UUD Best-Effort Delivery**
7. **Phase 7: PSN Mechanics & Offsets**
   - Simulates sliding `CACK_PSN`, generating exact hexadecimal representations of matching SACK_BITMAPs.
8. **Phase 8: Control Packets**
9. **Phase 9: End-To-End Verification Log**
   - Summarizes all payloads effectively received matching transmitted payloads completely.

At the output termination, you should see:

```
================================================================================
             Simulation Complete — UE SES/PDS/PDC Stack Validated
================================================================================
```

If you encounter `assert` errors or exit codes, reference `UET_SES_PDS_SPEC_COVERAGE.md` for format comparisons against UE specification version 1.0.2.

---

## 5. End-to-End Network Simulation (`uet-network-sim.cc`)

The benchmark simulator models real network parameters and accurately stresses the UET engine over Standard ns-3 PointToPoint topology. It calculates P50/P99 latencies, tracks drop-tail queues, and dumps comprehensive PDC state telemetry.

**Run a 200Gbps 2-Node RUD throughput test:**

```bash
./ns3 run "scratch/uet-network-sim --nodes=2 --numMsgs=20000 --msgSize=65535 --dataRate=200Gbps --mode=RUD"
```

**Parameters available:**

- `--nodes`: Number of nodes in the string topology (default 2).
- `--msgSize`: Application-level message size. The UET engine cleanly segments these out into MTU-sized packets (default 65535).
- `--numMsgs`: Number of complete messages to transmit (default 10000).
- `--dataRate`: Point-to-Point link bandwidth (default 200Gbps).
- `--delay`: Point-to-Point link latency (default 100ns).
- `--mode`: The UET delivery mode to stress via enum: `RUD`, `ROD`, `RUDI`, `UUD` (default `RUD`).

### Output Metrics Guide:

The output replicates `Soft-UE.cc` telemetry exactly but is layered securely on the UET spec formulation.

- **Achieved Throughput/Efficiency:** Assesses network saturation levels for the passed delivery mode.
- **Message Loss (%):** Demonstrates natural UDP tail-drop queueing effects when link rates are pushed directly to limits.
- **Latency Percentiles:** Accurately reveals standard distribution delays for payload assembly (P50 vs P99).
- **PDC State Machine Details:** The report dynamically retrieves the live tracking state, active/assigned PSNs, outstanding windows, and transition maps for both the **Initiator** and the **Target** endpoints.

---

## 6. Complete Clean + Configure + Build Workflow (Recommended)

This section gives end-to-end commands for common development cycles. Keep using one build directory (`cmake-build` or `_build_test`) consistently.

### 6.1 Clean Options

**A) Safe clean using ns-3 wrapper (recommended first):**

```bash
cd ns-3-alibabacloud/simulation
./ns3 clean
```

**B) Remove stale CMake cache only (fast recovery):**

```bash
cd ns-3-alibabacloud/simulation
rm -f cmake-build/CMakeCache.txt
rm -rf cmake-build/CMakeFiles
```

**C) Full clean build reset:**

```bash
cd ns-3-alibabacloud/simulation
rm -rf cmake-build _build_test build .lock-ns3_darwin_build
```

> If your environment blocks `rm -rf`, manually delete these directories from the file explorer.

### 6.2 Configure

**Option 1: ns-3 wrapper configure (optimized profile):**

```bash
cd ns-3-alibabacloud/simulation
./ns3 configure --build-profile=optimized
```

> This configures the `build/` directory by default. If you plan to run `cmake --build cmake-build`, run Option 2 as well.

**Option 2: direct CMake configure (`cmake-build`):**

```bash
cd ns-3-alibabacloud/simulation
cmake -S . -B cmake-build
```

**Option 3: direct CMake configure (`_build_test`):**

```bash
cd ns-3-alibabacloud/simulation
cmake -S . -B _build_test
```

### 6.3 Build Commands

**Build with ns-3 wrapper (paired with Option 1):**

```bash
cd ns-3-alibabacloud/simulation
./ns3 build -j $(sysctl -n hw.ncpu)
```

**Build all targets (recommended):**

```bash
cd ns-3-alibabacloud/simulation
cmake --build cmake-build -j$(sysctl -n hw.ncpu)
```

**Build all UET demos only:**

```bash
cd ns-3-alibabacloud/simulation
cmake --build cmake-build -j$(sysctl -n hw.ncpu) --target \
   scratch_uet-complete-demo \
   scratch_uet-advanced-demo \
   scratch_uet-phase4-rdma-integration \
   scratch_uet-ses-pds-demo \
   scratch_uet-network-sim
```

**Build single UET targets:**

```bash
cd ns-3-alibabacloud/simulation
cmake --build cmake-build -j4 --target scratch_uet-complete-demo
cmake --build cmake-build -j4 --target scratch_uet-advanced-demo
cmake --build cmake-build -j4 --target scratch_uet-phase4-rdma-integration
cmake --build cmake-build -j4 --target scratch_uet-ses-pds-demo
cmake --build cmake-build -j4 --target scratch_uet-network-sim
```

---

## 7. Complete Run Command Reference

### 7.1 Full protocol stack demo (`uet-complete-demo`)

```bash
cd ns-3-alibabacloud/simulation
./ns3 run scratch/uet-complete-demo
```

Direct binary:

```bash
./build/scratch/ns3.36.1-uet-complete-demo-default
```

### 7.2 Phase 3 demo (`uet-advanced-demo`)

All scenarios:

```bash
./build/scratch/ns3.36.1-uet-advanced-demo-default --scenario=all
```

Individual scenarios:

```bash
./build/scratch/ns3.36.1-uet-advanced-demo-default --scenario=control
./build/scratch/ns3.36.1-uet-advanced-demo-default --scenario=target
./build/scratch/ns3.36.1-uet-advanced-demo-default --scenario=integration
```

### 7.3 Phase 4 demo (`uet-phase4-rdma-integration`)

All scenarios:

```bash
./build/scratch/ns3.36.1-uet-phase4-rdma-integration-default --scenario=all
```

Individual scenarios:

```bash
./build/scratch/ns3.36.1-uet-phase4-rdma-integration-default --scenario=send
./build/scratch/ns3.36.1-uet-phase4-rdma-integration-default --scenario=receive
./build/scratch/ns3.36.1-uet-phase4-rdma-integration-default --scenario=control
```

### 7.4 Phase 2 parser-style demo (`uet-ses-pds-demo`)

```bash
./build/scratch/ns3.36.1-uet-ses-pds-demo-default
```

Example with explicit arguments:

```bash
./build/scratch/ns3.36.1-uet-ses-pds-demo-default \
   --mode=rud --opCode=send --messageBytes=2048 --mtuBytes=2048
```

### 7.5 End-to-end stress benchmark (`uet-network-sim`)

```bash
./build/scratch/ns3.36.1-uet-network-sim-default \
   --nodes=2 --numMsgs=20000 --msgSize=65535 --dataRate=200Gbps --delay=100ns --mode=RUD
```

Quick smoke test:

```bash
./build/scratch/ns3.36.1-uet-network-sim-default \
   --numMsgs=100 --msgSize=1024 --dataRate=10Gbps --delay=1us --mode=RUD
```

---

## 8. Build/Cache Recovery Commands

If you see source/cache mismatch errors (for example after moving directories between machines):

```bash
cd ns-3-alibabacloud/simulation
rm -f _build_test/CMakeCache.txt
rm -rf _build_test/CMakeFiles
cmake -S . -B _build_test
cmake --build _build_test -j4
```

Equivalent recovery for `cmake-build`:

```bash
cd ns-3-alibabacloud/simulation
rm -f cmake-build/CMakeCache.txt
rm -rf cmake-build/CMakeFiles
cmake -S . -B cmake-build
cmake --build cmake-build -j4
```

---

## 9. One-shot Developer Flow

Use this when you want a complete reset, build, and run in one sequence:

```bash
cd ns-3-alibabacloud/simulation
./ns3 clean
cmake -S . -B cmake-build
cmake --build cmake-build -j$(sysctl -n hw.ncpu)
./build/scratch/ns3.36.1-uet-complete-demo-default
./build/scratch/ns3.36.1-uet-advanced-demo-default --scenario=all
./build/scratch/ns3.36.1-uet-phase4-rdma-integration-default --scenario=all
./build/scratch/ns3.36.1-uet-ses-pds-demo-default --mode=rud --opCode=send --messageBytes=2048 --mtuBytes=2048
./build/scratch/ns3.36.1-uet-network-sim-default --numMsgs=100 --msgSize=1024 --dataRate=10Gbps --delay=1us --mode=RUD
```
