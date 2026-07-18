# UET Simulator - Quick Reference Guide

## Fastest Way to Get Started

### 1. Clean Build Cache (if needed)

```bash
cd /Users/sayan/Sayan/Study/Rinku_maam/NS3_UET/ns-3-alibabacloud/simulation
# Note: rm -rf commands are restricted in this environment
# Manually delete: cmake-build/, build/, .lock-ns3_darwin_build
```

### 2. Build Everything

```bash
cd simulation
cmake --build cmake-build -j4
```

### 3. Run Phase 3 Demo (Control Packets)

```bash
./build/scratch/ns3.36.1-uet-advanced-demo-default --scenario=all
```

### 4. Run Phase 4 Demo (RDMA Integration)

```bash
./build/scratch/ns3.36.1-uet-phase4-rdma-integration-default --scenario=all
```

### 5. Run Phase 2 Demo (Interactive)

```bash
./build/scratch/ns3.36.1-uet-ses-pds-demo-default
# Then enter commands like:
# >>> topology: nodes=8 flows=16 hops=3 load=1.2
# >>> mode: ROD
# >>> message_bytes: 1000000
# >>> go
```

---

## Build Targets

```bash
# Individual builds
cmake --build cmake-build -j4 --target scratch_uet-advanced-demo
cmake --build cmake-build -j4 --target scratch_uet-phase4-rdma-integration
cmake --build cmake-build -j4 --target scratch_uet-ses-pds-demo

# Rebuild from scratch
cmake --build cmake-build -j4 --clean-first
```

---

## Running Scenarios

### Phase 3 Scenarios

```bash
# All scenarios
./build/scratch/ns3.36.1-uet-advanced-demo-default --scenario=all

# Individual scenarios
./build/scratch/ns3.36.1-uet-advanced-demo-default --scenario=control       # CP types
./build/scratch/ns3.36.1-uet-advanced-demo-default --scenario=target        # Target state
./build/scratch/ns3.36.1-uet-advanced-demo-default --scenario=integration   # Full flow
```

### Phase 4 Scenarios

```bash
# All scenarios
./build/scratch/ns3.36.1-uet-phase4-rdma-integration-default --scenario=all

# Individual scenarios
./build/scratch/ns3.36.1-uet-phase4-rdma-integration-default --scenario=send      # TX path
./build/scratch/ns3.36.1-uet-phase4-rdma-integration-default --scenario=receive   # RX path
./build/scratch/ns3.36.1-uet-phase4-rdma-integration-default --scenario=control   # CP types
```

---

## Key Implementations

### Phase 1-2 Files (Foundation)

```
✅ uet-ses-header.h/cc           - Serialized Endpoint Service header
✅ uet-pds-header.h/cc           - Packet Delivery Service header
✅ uet-ses-pds-engine.h/cc        - Core transaction engine
```

### Phase 3 Files (Control & State)

```
✅ uet-pds-control-packet.h/cc   - 8 control packet types
✅ uet-pds-target-state.h/cc     - Target-side PDC state machine
✅ scratch/uet-advanced-demo.cc  - Phase 3 demonstration
```

### Phase 4 Files (RDMA)

```
✅ rdma-hw-uet-integration.h/cc   - RDMA hardware bridge
✅ scratch/uet-phase4-rdma-integration.cc - Phase 4 demonstration
```

---

## Key Classes & Methods

### UetSesPdsEngine

```cpp
// Create engine
Ptr<UetSesPdsEngine> engine = CreateObject<UetSesPdsEngine>();

// Run transaction
auto report = engine->RunTransaction(request, topology);

// Get description
std::string desc = engine->DescribeReport(report);
```

### UetPdsControlPacket

```cpp
// Create control packet
controlPacket.SetControlType(UetPdsControlPacket::CLEAR_COMMAND);
controlPacket.SetPsn(100);
controlPacket.SetPayload(clearPsn);
controlPacket.SetRequiresAck(true);

// Describe
std::string desc = controlPacket.Describe();
```

### UetPdsTargetState

```cpp
// Get/create PDC
auto pdc = targetState.GetOrCreatePdc(pdcId, mode);

// Track PSN
targetState.TrackReceivedPsn(pdcId, psn);

// Allocate resource
uint32_t ri = targetState.AllocateResourceIndex(pdcId, jobId, initiator, maxResponseSize);

// Store response
targetState.StoreDefaultResponse(pdcId, ri, responseCode, modifiedLength);

// Advance clear
targetState.AdvanceClearPsn(pdcId, clearedToThisPsn);
```

### RdmaHwUetIntegration

```cpp
// Initialize
Ptr<RdmaHwUetIntegration> integration = CreateObject<RdmaHwUetIntegration>();
integration->Initialize(numNodes, linkBandwidth, mtuBytes);

// TX path
auto result = integration->ProcessTxPacket(p, src, dst, sport, jobId, mode, dropRate);

// RX path
bool shouldRespond = integration->ProcessRxPacket(p, src, dst, sport, jobId, mode, dropRate);

// Generate response
uint8_t code = integration->GenerateDefaultResponse(src, dst, sport, jobId, mode);
```

---

## Environment Variables

```bash
# Enable verbose logging
export NS_LOG="UetAdvancedDemo=level_all"
export NS_LOG="RdmaHwUetIntegration=level_all"

# Run with logging
./build/scratch/ns3.36.1-uet-advanced-demo-default --scenario=all 2>&1 | grep LOGIN
```

---

## Output Examples

### Phase 3 Output

```
[CP-NOOP] [CP] type=0 psn=1 payload=0 requiresAck=yes
[TARGET] [PDC] id=300 mode=2 state=0 clearPsn=5
[DYNAMIC TX] SES/PDS report: messageId=50001 mode=3 packets=212 success=true
```

### Phase 4 Output

```
[RDMA-UET] TX psn=1 mode=2 pdcId=5f5e3bd
[RDMA-UET] RX psn=1 mode=2 pdcId=5f6f271
[RX-ACK] Packet 0 -> response code=0x0
```

---

## Troubleshooting

| Issue                   | Solution                                                |
| ----------------------- | ------------------------------------------------------- |
| Build fails after edits | Run `cmake --build cmake-build -j4 --clean-first`       |
| Demo crashes on startup | Check engine initialization before use                  |
| No output               | Use `--verbose=true` flag or check NS_LOG settings      |
| Can't find executable   | Ensure build completed: `cmake --build cmake-build -j4` |

---

## File Locations

```
Root:           /Users/sayan/Sayan/Study/Rinku_maam/NS3_UET/ns-3-alibabacloud
Simulation:     ./simulation
UET Headers:    ./simulation/src/point-to-point/model/uet-*.{h,cc}
RDMA Bridge:    ./simulation/src/point-to-point/model/rdma-hw-uet-integration.{h,cc}
Demos:          ./simulation/scratch/uet-*-demo.cc
Build Output:   ./simulation/build/scratch/ns3.36.1-*-default
Specification:  ../UE-Specification-1.0.2-1.txt
```

---

## Performance Tips

1. **Build with optimization:**

   ```bash
   ./ns3 configure --build-profile=optimized
   cmake --build cmake-build -j4
   ```

2. **Use parallel jobs:**

   ```bash
   cmake --build cmake-build -j$(sysctl -n hw.ncpu)
   ```

3. **Profile with time:**
   ```bash
   time ./build/scratch/ns3.36.1-uet-advanced-demo-default --scenario=integration
   ```

---

## Next Steps

1. **Understand Phase 3:** Run `--scenario=all` and read the output
2. **Understand Phase 4:** Run RDMA integration demo, analyze TX/RX flow
3. **Modify Scenarios:** Edit scratch/\*.cc files to create custom tests
4. **Phase 5:** Implement spec sequence diagram tests
5. **Phase 6:** Wire integration layer into rdma-hw.cc handlers

---

**For detailed documentation, see: README-UET-SIMULATOR.md**
