# Ultra Ethernet Transport (UET) in ns-3: SES + PDS Implementation

A working implementation of the **Ultra Ethernet Consortium (UEC) transport layer** inside the
ns-3.36.1 network simulator, covering the **Semantic Sublayer (SES, spec section 3.4)** and the
**Packet Delivery Sublayer (PDS, spec section 3.5)** of UE-Specification 1.0, built on top of the
ns-3-alibabacloud (SimAI) tree.

The stack runs end-to-end over real ns-3 UDP sockets and point-to-point links, delivers messages
reliably under injected packet loss, and is verified by a deterministic 86-check test suite.

## Background

RoCEv2, today's dominant RDMA transport for AI and HPC fabrics, assumes a lossless network built
on Priority Flow Control and recovers from any loss with go-back-N retransmission. At scale this
produces head-of-line blocking, congestion trees, PFC deadlock risk, and heavy bandwidth waste on
rare loss events. InfiniBand solves reliability with a similar lossless, credit-based assumption
but stays a closed, single-vendor fabric. Ultra Ethernet removes the losslessness requirement from
the transport's correctness model and instead lets packets arrive out of order (a precondition for
multipath packet spraying), acknowledges selectively with a SACK bitmap, keeps connection state
ephemeral and established in-band at zero extra round-trip cost, and lets each application choose
the ordering and reliability trade-off it actually needs, message by message, on standard Ethernet
PHY/MAC rather than a proprietary fabric.

| Mode | Reliability | Ordering | Use case |
|------|-------------|----------|----------|
| RUD  | Reliable    | Unordered | RDMA writes/reads, gradient allreduce |
| ROD  | Reliable    | Ordered   | MPI collectives, atomics |
| RUDI | Reliable    | Unordered, idempotent | Small datagrams, PDC-less |
| UUD  | Unreliable  | Unordered | Best-effort telemetry |

## What is implemented

**SES (section 3.4)** - all seven wire header formats with exact bit layouts: Standard SOM=1
(44 B) and SOM=0 continuation (32 B), Optimized Non-Matching (20 B), Small-Message (28 B),
Response (12 B), Response-with-Data (20 B), Optimized Response-with-Data (12 B), plus the atomic
extension header. Message fragmentation to MTU, SOM/EOM flags, message offsets, opcodes, return
codes.

**PDS (section 3.5)** - all twelve packet formats (requests, ACK, ACK_CC with 64-bit SACK
bitmap, ACK_CCX, NACK with all twenty NACK codes of Table 3-58, control packets, RUDI
request/response, UUD); PDC lifecycle with in-band SYN establishment, IPDCID/TPDCID exchange,
randomized Start_PSN, per-direction PSN spaces (CACK_PSN, CLEAR_PSN, MP_RANGE windowing);
duplicate detection and re-ACK; guaranteed-delivery response storage with clear-window
management.

**Loss recovery** - stored-packet retransmission driven by an exponential-backoff RTO timer,
SACK-based selective release, NACK-code-specific reactions (retransmit / new-PDC retry /
PDC-fatal teardown), bounded retry budget with explicit per-message failure reporting, and a
target-side reorder buffer that gives ROD in-order delivery without go-back-N retransmit storms.

**Integration and tooling** - an RDMA-hardware bridge layer hooked into the SimAI RdmaHw
RX/completion path, six demo programs (protocol walkthrough, AI/HPC profile workloads, socket
benchmark, drop-injection demo, control-packet scenarios), and a deterministic test suite.

## Architecture

```mermaid
flowchart TB
    subgraph App["Application / Demo layer"]
        demos["uet-complete-demo | uet-hpc-ai-profiles | uet-network-sim | uet-tests"]
    end

    subgraph Engine["UetSesPdsEngine (per endpoint)"]
        tx["TX: Send() -> fragment to MTU -> SES header -> PDS header"]
        rx["RX: ProcessRxPacket() -> dispatch by PDS type"]
        rto["RTO timer: exponential backoff, stored-packet retransmit"]
        rob["ROD reorder buffer: in-order SES delivery"]
    end

    subgraph PdcMgr["UetPdcManager"]
        pdc["PDC contexts: IPDCID/TPDCID, state machine\nCLOSED -> CREATING -> ESTABLISHED -> QUIESCE -> CLOSE"]
        psn["PSN spaces: Start_PSN, CACK_PSN, CLEAR_PSN,\nMP_RANGE window, SACK bitmap, outstanding map"]
    end

    subgraph Headers["Wire formats"]
        ses["UetSesHeader: 7 formats, section 3.4.2"]
        pds["UetPdsHeader: 12 formats, section 3.5.10"]
    end

    subgraph Net["ns-3 substrate"]
        sock["UDP sockets / direct fabric routing (dstFa)"]
        p2p["PointToPoint devices, queues, RateErrorModel loss"]
    end

    demos --> tx
    demos --> rx
    tx --> pdc
    rx --> pdc
    pdc --> psn
    tx --> ses
    tx --> pds
    rx --> pds
    rto --> psn
    rx --> rob
    tx --> sock
    rx --> sock
    sock --> p2p
```

The engine is transport-pure: its only output is a callback `void(Ptr<Packet>, uint32_t dstFa)`.
The benchmark maps `dstFa` to a UDP `SendTo`, the demos route through an in-process fabric table,
and the test harness interposes drop filters on the same wire path - the same engine code runs
unmodified in all three environments.

Packet walk (RUD write, first message on a fresh PDC):

1. `Send()` picks or creates a PDC for the tuple (srcFA, dstFA, TC, mode); the initiator
   assigns an IPDCID and a randomized Start_PSN.
2. Each MTU-sized chunk gets an SES header (SOM/EOM, message id, offsets) and a PDS request
   header (PSN, SYN flag while the TPDCID is unknown, spdcid = IPDCID, dpdcid carrying
   pdc_info + psn_offset during SYN).
3. The target sees SYN, allocates a TPDCID, records the PSN, and returns an ACK_CC whose
   spdcid teaches the initiator the TPDCID; the SACK bitmap advertises out-of-order arrivals.
4. ACKs release outstanding packets (cumulative + selective); the completion callback fires
   exactly once per message when its last PSN is acknowledged.
5. A lost packet is retransmitted from the stored wire copy when its RTO expires (100 us
   initial, doubling per retry); after the retry budget (default 7) the message fails loudly.

The most consequential engineering decision in this codebase is how ROD recovers from loss.
The spec permits NACK-and-drop (go-back-N, the strategy RoCEv2 effectively uses): any
out-of-order arrival is NACKed, forcing retransmission of everything from the gap forward. An
earlier version of this implementation used that strategy and it collapsed under load - 54%
completion at 0.1% loss, 8% at 1% loss - because every packet behind a gap triggered its own
NACK and the retransmit storm exhausted retry budgets. Replacing it with the also-spec-valid
receiver-side strategy (accept out-of-order packets within the window, buffer them, let the
initiator's RTO retransmit only the genuinely lost PSN, deliver to the application strictly in
order) took ROD to 100% completion at every tested loss rate with goodput identical to RUD. This
reproduces, with a live measurement rather than a citation, the central result of IRN (SIGCOMM
2018): selective retransmission dominates go-back-N for RDMA.

## Results

Benchmark: 2 nodes, 200 Gbps link, 100 ns propagation delay, 2000 messages of 8192 B,
MTU 4096, seed 1, `RateErrorModel` loss injected on both devices
(`scripts/run_experiments.sh` reproduces every row).

### Reliability and goodput vs injected loss

| Mode | Loss | Completion | Goodput | Retransmits | Avg delivery latency | P99 |
|------|------|-----------|---------|-------------|----------------------|-----|
| RUD | 0%    | 100.00% | 196.27 Gbps | 0   | 7.4 us   | 14.3 us |
| RUD | 0.1%  | 100.00% | 192.96 Gbps | 3   | 10.5 us  | 25.7 us |
| RUD | 1%    | 100.00% | 163.47 Gbps | 63  | 30.5 us  | 151.2 us |
| RUD | 5%    | 100.00% | 100.75 Gbps | 429 | 180.3 us | 519.2 us |
| ROD | 0%    | 100.00% | 196.27 Gbps | 0   | 7.4 us   | 14.3 us |
| ROD | 0.1%  | 100.00% | 192.96 Gbps | 3   | 54.7 us  | 187.0 us |
| ROD | 1%    | 100.00% | 163.47 Gbps | 63  | 155.5 us | 226.9 us |
| ROD | 5%    | 100.00% | 100.75 Gbps | 429 | 438.4 us | 796.4 us |
| RUDI | 1% (4096 B msgs) | 100.00% | 131.03 Gbps | 35 | n/a | n/a |

Every sent message is acknowledged end-to-end at every loss rate; the cost of loss appears
where it should: goodput and tail latency. ROD pays an additional head-of-line-blocking
latency penalty over RUD for the same loss rate, which is exactly the RUD-vs-ROD trade the
UEC spec is built around. Identical seeds produce byte-identical outputs.

### Before and after this audit-and-fix pass

The codebase arrived with complete SES/PDS wire-format classes and a substantial PDC manager,
but the reliability path did not actually work. The audit ran everything before changing
anything; the single most revealing run was the socket-level benchmark on a lossless 200 Gbps
link, which reported 511 of 2000 messages delivered, zero acknowledged end-to-end, 256 PDCs
allocated for one flow, and a closing banner that read "EXCELLENT - UET reliably saturated line
rate."

| Metric (lossless 200 Gbps run) | Before | After |
|--------------------------------|--------|-------|
| Messages delivered | 511 / 2000 (25.6%) | 2000 / 2000 (100%) |
| Messages ACKed end-to-end | 0 (ACK path broken) | 2000 (100%) |
| Active PDCs for one flow | 256 (pool exhausted) | 1 |
| Payload bytes delivered | 2.09 MB of 16.38 MB | 16,384,000 of 16,384,000 (byte-exact) |
| Retransmission machinery | none (drops were permanent) | RTO + SACK + NACK recovery |
| ROD under 1% loss | 8% completion | 100% completion |
| Verification | none | 86 deterministic checks, all passing |

The headline defects behind the before column: the wire callback carried no destination
address, so ACKs could not be routed back on any topology with more than one peer; `Pump()`
sent one chunk per message and was never re-driven by ACKs; every `Send()` during the SYN
handshake allocated a fresh PDC until the pool of 256 was exhausted; three PDS wire formats
had serialize/deserialize size mismatches (the CP header overran its buffer by two bytes);
and SOM=0 continuation headers were parsed as SOM=1, silently swallowing 12 payload bytes
per message. Details with file/line references are in [docs/REPORT.md](docs/REPORT.md).

## Comparison with existing transports

| Dimension | TCP | RoCEv2 | InfiniBand | This implementation |
|---|---|---|---|---|
| Where transport runs | Kernel software stack | NIC hardware, requires a lossless fabric | Proprietary NIC hardware and fabric | NIC-ASIC model, tolerant of a lossy fabric |
| Loss assumption | Tolerated, recovers slowly | Assumed away via PFC; recovery is expensive | Assumed away, credit-based | Loss is an expected, cheap event |
| Ordering under loss | Cumulative ACK, in-order only | Go-back-N on drop | Vendor-specific, lossless assumption | Selective retransmit plus receiver-side reorder buffer |
| Connection setup | Three-way handshake, extra RTT | Out-of-band queue-pair setup | Out-of-band queue-pair setup | In-band PDC establishment, zero extra RTT |
| Fabric | Open, any IP network | Open standard, tied to Ethernet + PFC | Closed, single-vendor fabric | Open UEC standard on standard Ethernet PHY/MAC |
| Congestion control | Software (Reno/CUBIC/BBR family) | Hardware-assisted (DCQCN and similar) | Credit-based | Not yet implemented; CC fields are carried on the wire, unused |

The controlled comparison this project can actually back with numbers is go-back-N versus
selective-retransmit-with-reorder-buffer on the identical engine, fabric, and seed, varying only
the ROD recovery strategy:

| Loss rate | Go-back-N ROD | This implementation's ROD |
|---|---|---|
| 0.1% | 54% completion | 100% completion |
| 1% | 8% completion | 100% completion |
| 5% | collapses further | 100% completion, about half the goodput retained |

This project does not yet run a congestion-control algorithm, multipath spraying, or
encryption, so it is not a claim of full parity with production RoCEv2/DCQCN deployments. The
defensible claim is narrower: for the loss-recovery and connection-establishment mechanics the
UET spec defines, this implementation demonstrates with real measurements, not just a reading of
the spec, why those design choices outperform go-back-N and lossless-fabric assumptions once
loss actually happens.

## Repository layout

```
.
├── README.md                  <- you are here
├── docs/
│   ├── GUIDE.md               <- build / run / troubleshoot, all CLI flags
│   └── REPORT.md              <- audit findings, fixes, evaluation methodology
├── scripts/
│   ├── build.sh                <- configure + build everything
│   ├── run_tests.sh            <- 86-check verification suite
│   ├── run_demos.sh            <- all demos in sequence
│   └── run_experiments.sh      <- reproduce the results tables
├── analysis/                   <- trace analysis tools (Python + C++)
├── dashboard/                  <- static web visualization of the protocol
├── legacy/                     <- superseded docs kept for history
├── archive/                    <- personal notes and reference material, not part of the build
└── simulation/                 <- ns-3.36.1 tree (ns-3-alibabacloud fork)
    ├── scratch/
    │   ├── uet-tests.cc               <- deterministic test suite (T01..T11)
    │   ├── uet-complete-demo.cc       <- 4-node SES/PDS/PDC walkthrough
    │   ├── uet-hpc-ai-profiles.cc     <- AI Base / AI Full / HPC workloads
    │   ├── uet-network-sim.cc         <- socket-level benchmark with loss injection
    │   ├── uet-ses-pds-demo.cc        <- parametric demo with deterministic drops
    │   ├── uet-advanced-demo.cc       <- control packets and target state
    │   └── uet-phase4-rdma-integration.cc
    └── src/point-to-point/model/
        ├── uet-ses-header.{h,cc}      <- SES wire formats (section 3.4)
        ├── uet-pds-header.{h,cc}      <- PDS wire formats (section 3.5.10)
        ├── uet-pdc.{h,cc}             <- PDC manager, PSN spaces, state machines
        ├── uet-ses-pds-engine.{h,cc}  <- transaction engine, RTO, reorder buffer
        ├── uet-pds-control-packet.{h,cc}
        ├── uet-pds-target-state.{h,cc}
        └── rdma-hw-uet-integration.{h,cc}
```

## Quick start

Prerequisites: C++17 compiler (clang or gcc), CMake 3.10+, Python 3. Tested on macOS
(Apple Silicon) and Linux.

```bash
# 1. Build everything (first build takes a while; ns-3 is large)
scripts/build.sh

# 2. Prove the stack works: 11 tests, 86 checks, deterministic
scripts/run_tests.sh

# 3. Watch the protocol run end-to-end
scripts/run_demos.sh complete        # 4-node SES/PDS/PDC walkthrough
scripts/run_demos.sh network         # 200 Gbps benchmark, lossless + 1% loss

# 4. Reproduce the results tables
scripts/run_experiments.sh
```

Direct binary invocation (after building):

```bash
cd simulation
./build/scratch/ns3.36.1-uet-network-sim-debug \
    --numMsgs=2000 --msgSize=8192 --mode=ROD --lossRate=0.01 --seed=1
```

See [docs/GUIDE.md](docs/GUIDE.md) for every program, flag, and troubleshooting entry.

## Tech stack

| Layer | Technology |
|---|---|
| Simulator | ns-3.36.1, the ns-3-alibabacloud (SimAI) fork, which already models AI-fabric RDMA (RdmaHw) |
| Protocol implementation | C++17, as ns-3 `Header` subclasses and `Object`-based engine/manager classes |
| Build system | CMake plus the `ns3` wrapper script |
| Networking primitives | ns-3 UDP sockets, PointToPoint net devices, `RateErrorModel` for loss injection |
| Reproducibility scripts | Bash (`build.sh`, `run_tests.sh`, `run_demos.sh`, `run_experiments.sh`) |
| Trace analysis | Python (bandwidth, flow-completion-time, queue-length, QP-rate analysis) and a small C++ trace reader |
| Visualization | Static HTML/CSS/JS dashboard (`dashboard/`) |
| Reference | UEC UE-Specification 1.0, sections 3.4 (SES) and 3.5 (PDS) |

## Relation to published work

- **DCQCN (SIGCOMM 2015)** and **TIMELY (SIGCOMM 2015)** motivate why lossless RoCE fabrics
  need congestion control; UET instead tolerates loss at the transport layer. This project
  implements the loss-tolerant transport, not the congestion-control algorithms (the CC
  header fields ACK_CC/ACK_CCX are carried but no CC state machine runs).
- **IRN (SIGCOMM 2018)** showed selective retransmission beats go-back-N for RDMA; the same
  result reproduces here: replacing NACK-and-drop ROD recovery with SACK plus a target-side
  reorder buffer took ROD-under-1%-loss from 8% to 100% completion.
- **Swift (SIGCOMM 2020)** style delay-based CC is a natural future addition on top of the
  carried CC state fields.
- The ns-3-alibabacloud base tree is the SimAI simulator from Alibaba; its RDMA/QBB stack
  coexists with this UET module, and the bridge layer hooks UET tracking into the RdmaHw
  receive path.

## Honest limitations

- **No congestion control.** CC header formats are serialized correctly, but no CCC/credit
  algorithm runs; the benchmark link is a single uncontended hop.
- **Single path.** Entropy-based packet spraying / multipathing (a headline UET feature) is
  not modeled; there is one path, so reordering only arises from loss.
- **No TSS/encryption**, no trimming-based fast loss signaling (NACK codes for trimming exist
  and are handled, but no switch trims packets in these topologies).
- **Retransmitted packets do not set the retx wire flag** (stored copies are resent
  verbatim); duplicate detection at the target makes this behaviorally invisible here, but a
  spec-conformance harness would flag it.
- **RUDI dedup state is unbounded** (a set of seen pkt_ids per peer); fine for simulations,
  not for a production endpoint.
- **RUDI latency is not measured** in the benchmark (small-message SES format carries no
  message id for the timestamp map).
- The RdmaHw bridge tracks UET state alongside the existing SimAI RDMA path rather than
  replacing its go-back-N recovery; full datapath substitution is future work.
- Simulation only: no NIC offload model, no host memory bandwidth effects.

## Reproduction

Every number in this README comes from a committed script:

```bash
scripts/build.sh              # build
scripts/run_tests.sh          # correctness: expect "86/86 checks passed"
scripts/run_experiments.sh    # results/: one file per mode x loss configuration
SEED=2 scripts/run_experiments.sh   # different seed, same conclusions
```

## Contributors

- Sayan Das (sayan20012002@gmail.com)
- Senjuti Ghosal (senjutighosal09@gmail.com)

## License

The ns-3 base tree keeps its GPLv2 license (see `LICENSE`). UET module code follows the same
license as the tree it lives in.
