# UET Simulator: Build and Run Guide

This guide covers everything needed to build the codebase, run each program, and interpret
the output. All commands are relative to the repository root.

## 1. Prerequisites

| Requirement | Version | Notes |
|-------------|---------|-------|
| C++ compiler | clang 12+ or gcc 9+ (C++17) | Apple clang works |
| CMake | 3.10+ | |
| Python 3 | any recent | used by the `ns3` wrapper script |
| OS | macOS or Linux | tested on macOS (Apple Silicon) |

No other dependencies. The ns-3 tree is vendored in `simulation/`.

## 2. Building

```bash
scripts/build.sh            # incremental build
scripts/build.sh --clean    # wipe build artifacts and rebuild from scratch
```

The script runs `./ns3 configure --build-profile=debug` once, then `./ns3 build`.
A cold build compiles the entire ns-3 tree and takes several minutes; incremental builds
that only touch UET files take seconds.

Binaries land in `simulation/build/scratch/` with the pattern
`ns3.36.1-<program>-debug`.

Manual equivalent:

```bash
cd simulation
./ns3 configure --build-profile=debug
./ns3 build                          # everything
./ns3 build scratch/uet-tests        # a single target
```

## 3. Verifying the stack

```bash
scripts/run_tests.sh
```

Expected final line:

```
UET test suite: 86/86 checks passed  — ALL TESTS PASSED
```

The suite is deterministic (fixed seed) and covers:

| Test | What it proves |
|------|----------------|
| T01 | All 7 SES header formats survive serialize/deserialize round-trips |
| T02 | All PDS formats round-trip (request, ACK, ACK_CC + SACK, NACK, CP, RUDI, UUD) |
| T03 | PDC SYN establishment, TPDCID learning, and single-PDC reuse across messages |
| T04 | Multi-packet message delivers fully; completion callback fires exactly once |
| T05 | A dropped request packet is healed by RTO retransmission |
| T06 | A dropped ACK leads to duplicate re-ACK, not duplicate delivery |
| T07 | ROD delivers in order under a mid-message drop |
| T08 | RUDI retransmits by pkt_id and the target deduplicates |
| T09 | UUD completes immediately and leaves no reliability state |
| T10 | SACK bitmap construction and CACK-anchored window acceptance |
| T11 | Exhausted retry budget reports failure exactly once (no silent loss) |

## 4. The programs

### 4.1 uet-tests: verification suite

```bash
./simulation/build/scratch/ns3.36.1-uet-tests-debug
```

Exit code 0 means all checks passed. Use this after any code change.

### 4.2 uet-complete-demo: protocol walkthrough

```bash
scripts/run_demos.sh complete
```

Four engines (N0..N3) joined by an in-process fabric router. Phases: all SES header
formats printed with sizes and spec table references; all PDS formats; PDC establishment
for a 3 x 4 KB RUD write (N0 to N1); ROD read; RUDI datagram; UUD send; PSN space
mechanics including a worked SACK bitmap example and the spec Table 3-43 ACK_PSN offset
vectors; final per-node statistics. Every node should report a TX completion
(`ack msgId=... OK rc=0x0`).

### 4.3 uet-hpc-ai-profiles: workload profiles

```bash
scripts/run_demos.sh hpc
```

Eight nodes run AI Base (ring allreduce with RUD writes), AI Full (tagged rendezvous
sends, deferrable sends, ROD atomics), and HPC (MPI-style scatter/gather with ROD, SHMEM
put/get with RUD) workloads, then print the profile feature matrix from spec section
2.2.2.

### 4.4 uet-network-sim: socket-level benchmark

The main quantitative tool. Runs the engine over real ns-3 UDP sockets and a
point-to-point link.

```bash
cd simulation
./build/scratch/ns3.36.1-uet-network-sim-debug [flags]
```

| Flag | Default | Meaning |
|------|---------|---------|
| `--msgSize` | 65535 | application message size in bytes |
| `--numMsgs` | 10000 | number of messages to send |
| `--dataRate` | 200Gbps | link rate |
| `--delay` | 100ns | link propagation delay |
| `--mode` | RUD | RUD, ROD, RUDI, or UUD |
| `--seed` | 1 | RNG seed; identical seeds give identical runs |
| `--lossRate` | 0 | per-packet receive drop probability on both devices |
| `--simTime` | 10.0 | simulation stop time (seconds) |

Output sections: throughput (target vs achieved goodput), reliability (sent /
delivered / completed / failed), delivery latency percentiles, engine transport counters
(TX/RX packets, NACKs, retransmissions, PDC counts), and an honest assessment line that
says FAIL when completion is below 100%. Full PDC state dumps go to
`pdc_state_dump.log` in the working directory.

Key expectation: `Messages Completed` should be 100% for RUD/ROD/RUDI at any loss rate
the retry budget can absorb; goodput and P99 latency are where loss shows up.

### 4.5 uet-ses-pds-demo: deterministic drop injection

```bash
cd simulation
./build/scratch/ns3.36.1-uet-ses-pds-demo-debug \
    --messageBytes=8000 --mtuBytes=1000 \
    --dropReqPsns=1,3 --dropAckPsns=0 --mode=rud
```

Drops exactly the listed request/ACK ordinals on first transmission, then lets the RTO
heal the loss. Expected output shows the injected drops, the retransmissions
(`retx=2`), full delivery (`deliveredBytes=8000`), and a successful completion
(`rc=0x0`). Also accepts `--dropRequestRate` / `--dropAckRate` for seeded probabilistic
drops and `--maxRetries` to shrink the retry budget.

### 4.6 uet-advanced-demo and uet-phase4-rdma-integration

```bash
scripts/run_demos.sh advanced
scripts/run_demos.sh phase4
```

Scenario-driven walkthroughs of control-packet construction (NOOP, CLEAR, ACK_REQUEST),
target-side state (PSN tracking, resource indices, default response storage, clear
advancement), and the RDMA bridge TX/RX paths.

### 4.7 Dashboard

`dashboard/index.html` is a static visualization page (open directly in a browser).

## 5. Reproducing the reported results

```bash
scripts/run_experiments.sh          # seed 1 (the tables in README/REPORT)
SEED=2 scripts/run_experiments.sh   # robustness check with another seed
```

One output file per configuration is written to `results/` and a completion/goodput
summary is printed at the end. The sweep covers RUD and ROD at loss rates
0 / 0.001 / 0.01 / 0.05 plus RUDI at 0.01.

## 6. Troubleshooting

**`./ns3 configure` fails or CMake cannot find a compiler**
Install Xcode command line tools (`xcode-select --install`) or gcc/g++ on Linux.

**Binary not found (`ns3.36.1-uet-...-debug`)**
The build profile decides the suffix. These instructions use the debug profile; if you
configured with `--build-profile=optimized` the binaries end in `-default` or
`-optimized` instead. Reconfigure with `./ns3 configure --build-profile=debug`.

**Stale build weirdness after pulling changes**
`scripts/build.sh --clean`.

**The benchmark prints Completed < 100% with high `--lossRate`**
That is honest behavior: each RTO retry doubles the timeout and the default budget is
7 retries. Raise `--simTime` so late retransmissions can finish, or accept the reported
failures as the cost of extreme loss.

**Where did my per-run PDC details go?**
`pdc_state_dump.log` next to wherever you invoked the benchmark from.

## 7. Making changes

The protocol code lives in `simulation/src/point-to-point/model/uet-*.{h,cc}`. After any
change:

```bash
cd simulation && ./ns3 build && ./build/scratch/ns3.36.1-uet-tests-debug
```

Add a regression check to `simulation/scratch/uet-tests.cc` for any bug you fix; the
harness (`TestPair` with filterable wires) makes drop/reorder scenarios one lambda away.
