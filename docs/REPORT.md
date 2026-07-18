# Project Report: UET SES/PDS Implementation in ns-3

Audit, repair, and evaluation of an Ultra Ethernet Transport (SES + PDS) implementation
built inside ns-3.36.1 (ns-3-alibabacloud tree), against UE-Specification 1.0 sections
3.4 (Semantic Sublayer) and 3.5 (Packet Delivery Sublayer).

## 1. Starting point

The codebase arrived with a substantial and well-structured foundation:

- Complete SES and PDS wire-format classes with spec-exact bit layouts and table
  references (about 1,700 lines).
- A PDC manager implementing in-band SYN establishment, IPDCID/TPDCID exchange,
  randomized Start_PSN, per-direction PSN spaces, SACK bitmap construction, and the
  twenty NACK codes of Table 3-58 (about 1,160 lines).
- An event-driven transaction engine and six demo programs.
- Documentation that was internally inconsistent: one document claimed a "complete,
  spec-compliant implementation" while another correctly listed scenario tests and
  production wiring as unfinished.

The audit ran everything before changing anything. The single most revealing run was the
socket-level benchmark on a lossless 200 Gbps link: 2000 messages sent, 511 delivered,
zero acknowledged end-to-end, 256 PDCs allocated for one flow, and a final banner reading
"EXCELLENT - UET reliably saturated line rate".

## 2. Defects found and fixed

### 2.1 Reliability was not implemented (design gap)

Outstanding packets stored no payload, no retransmission timer existed, and the NACK
handler only incremented counters. Any dropped packet was permanently lost, making
"Reliable Unordered Delivery" unreliable.

Fix: every transmitted RUD/ROD packet stores a wire copy against its PSN
(`PdcPsnSpace::TxPkt::wirePkt`); an RTO timer (100 us initial, exponential backoff,
capped budget) rescans outstanding packets and retransmits from the stored copy; NACK
codes now trigger their spec-mandated reactions (retransmit same PSN, retry, or
PDC-fatal teardown that fails inflight messages explicitly). RUDI gained pkt_id-keyed
retransmission and target-side dedup with RUDI_RESP confirmation.
Files: `uet-pdc.h/.cc`, `uet-ses-pds-engine.h/.cc`.

### 2.2 The wire callback could not route (design gap)

`SetWireSendCb` delivered a bare packet with no destination. Demos worked around it by
overwriting the single callback per destination pair, so a node talking to two peers sent
every ACK to whichever peer was wired last; the UDP benchmark server called `Send()` on
an unconnected socket, so ACKs never left the target at all. This is why 0 of 2000
messages completed.

Fix: the callback signature is now `(Ptr<Packet>, uint32_t dstFa)`. Demo fabrics route by
fabric address through one registry; the benchmark maps dstFa to `SendTo(ip, port)`.
ACKs and NACKs are addressed to the source fabric address of the packet that triggered
them. Files: `uet-ses-pds-engine.h`, all six scratch programs.

### 2.3 Pump sent one chunk per message and was never re-driven

A 12 KB message at MTU 4096 sent exactly one packet, forever. `Pump()` looped once per
message per call and nothing called it again when ACKs freed window space.

Fix: `Pump()` sends until the window closes and is re-driven on every ACK arrival and
after retransmissions. It is also reentrancy-safe: with a synchronous in-process fabric,
sending can deliver an ACK inline which completes and erases messages mid-iteration, so
the loop iterates over an ID snapshot, re-finds state after every wire send, and defers
nested calls.

### 2.4 PDC pool exhaustion

`FindExistingPdc` only matched ESTABLISHED contexts. Every `Send()` issued while the SYN
handshake was still in flight allocated a fresh PDC until the pool of 256 was gone, after
which sends failed silently. Fix: CREATING contexts are reusable, giving one PDC per
tuple as the spec intends. Result: 256 active PDCs became 1.

### 2.5 Wire-format serialization bugs (found by the new test suite)

- Control Packet header: `Serialize` wrote 16 bytes while `GetSerializedSize` declared
  14 - a two-byte buffer overrun that crashed inside ns-3's `Buffer` the first time a CP
  was round-tripped through a real packet.
- NACK header: `Deserialize` consumed 14 bytes but reported 12, silently corrupting
  anything behind a NACK header.
- NACK_CCX: declared 28 bytes, wrote 16.
- SES Standard header: `Deserialize` trusted a caller-preset SOM flag instead of reading
  it from the wire, so 32-byte SOM=0 continuation headers were parsed with the 44-byte
  SOM=1 layout, swallowing 12 payload bytes of every continuation packet (measured:
  16,360,000 of 16,384,000 bytes delivered; after the fix, byte-exact).

### 2.6 Receive-window and duplicate handling

The acceptance window was anchored at the highest PSN seen, so a retransmit of a dropped
packet (necessarily below that) was NACKed `PSN_OOR_WINDOW` instead of accepted - loss
recovery could never converge. The window is now CACK-anchored per section 3.5.12.2, and
the duplicate check runs before window/ordering checks so a duplicate whose ACK was lost
is re-ACKed rather than NACKed.

### 2.7 ROD collapsed under loss (found by the evaluation sweep)

First working version used spec-permitted NACK-and-drop for out-of-order ROD arrivals
(go-back-N). Under 0.1% loss it completed 54%; under 1% loss, 8%: every packet behind a
gap triggered NACK_ROD_OOO, each NACK retransmitted the prefix, and the storm exhausted
retry budgets. Replaced with the also-spec-valid receiver behavior: accept out-of-order
packets within the window (SACK advertises them), buffer SES delivery until the gap
fills, and let the initiator's RTO retransmit only the genuinely lost PSN. ROD completion
went to 100% at every tested loss rate, with goodput identical to RUD - reproducing the
core insight of IRN (SIGCOMM 2018) that selective retransmission dominates go-back-N.

### 2.8 Correctness of accounting

Completion callbacks could fire many times per message (or never); they now fire exactly
once, message failure is reported explicitly when the retry budget is exhausted, and the
benchmark counts a message delivered only when its EOM chunk arrives. The dishonest
"EXCELLENT" assessment was replaced with a completion-based verdict that prints FAIL
below 100%.

## 3. Verification

`simulation/scratch/uet-tests.cc`: 11 scenario tests, 86 assertions, fixed seed,
exit-code driven. Coverage: header round-trips for every wire format, PDC establishment
and reuse, exactly-once completion, request-drop recovery, ACK-drop recovery with dedup,
ROD ordering under loss, RUDI retransmit and dedup, UUD statelessness, SACK/window
mechanics, and retry-budget exhaustion. All 86 pass; the suite is wired into
`scripts/run_tests.sh`.

The serialization bugs of section 2.5 were all caught by T01/T02 on their first run,
which is the argument for keeping the suite in the repository.

## 4. Evaluation

Setup: two nodes, 200 Gbps point-to-point, 100 ns delay, MTU 4096, 2000 messages of
8192 B, seed 1, loss injected with `RateErrorModel` on both devices, RTO 100 us, retry
budget 7. Reproduce with `scripts/run_experiments.sh`.

| Mode | Loss | Completion | Goodput | Retx | Avg latency | P99 latency |
|------|------|-----------|---------|------|-------------|-------------|
| RUD | 0%   | 100% | 196.27 Gbps | 0   | 7.4 us   | 14.3 us  |
| RUD | 0.1% | 100% | 192.96 Gbps | 3   | 10.5 us  | 25.7 us  |
| RUD | 1%   | 100% | 163.47 Gbps | 63  | 30.5 us  | 151.2 us |
| RUD | 5%   | 100% | 100.75 Gbps | 429 | 180.3 us | 519.2 us |
| ROD | 0%   | 100% | 196.27 Gbps | 0   | 7.4 us   | 14.3 us  |
| ROD | 0.1% | 100% | 192.96 Gbps | 3   | 54.7 us  | 187.0 us |
| ROD | 1%   | 100% | 163.47 Gbps | 63  | 155.5 us | 226.9 us |
| ROD | 5%   | 100% | 100.75 Gbps | 429 | 438.4 us | 796.4 us |
| RUDI | 1% (4 KB) | 100% | 131.03 Gbps | 35 | not measured | not measured |

Observations:

1. Reliability holds at every loss rate: all 2000 messages acknowledge end-to-end;
   failures are zero.
2. Loss costs goodput and tail, not correctness: 1% loss costs about 17% goodput and
   about 10x P99; 5% costs about half the goodput.
3. RUD and ROD move the same bytes at the same goodput; ROD additionally pays
   head-of-line blocking latency (P99 at 1% loss: 227 us vs 151 us), which is precisely
   the trade the UEC spec exposes by offering both modes.
4. Determinism: repeated runs with seed 1 are byte-identical; seed 2 differs in noise,
   not conclusions.

Latency is measured at SES delivery (EOM chunk arrival at the target) against send time.
RUDI latency is unmeasured because the small-message SES format carries no message id to
key the timestamp map - an accepted gap, listed in limitations.

## 5. Limitations

No congestion control algorithm (CC header state is carried, unused); single path, so no
entropy spraying or reorder-from-multipath; no TSS/encryption; no switch-side trimming;
retransmits do not set the retx wire flag; RUDI dedup state unbounded; RdmaHw bridge
tracks UET state alongside (not instead of) the SimAI RDMA recovery path; simulation
carries no NIC/host-memory model.

## 6. What I would do next

Credit-based congestion control on the existing ACK_CC fields; entropy-value multipath
with a spraying policy and reorder-tolerance measurements; replacing RdmaHw's go-back-N
with the UET engine end to end inside SimAI workloads; a conformance harness that checks
wire bits against the spec tables systematically rather than via hand-written vectors.
