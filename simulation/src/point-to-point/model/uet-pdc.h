/**
 * uet-pdc.h  —  PDC (Packet Delivery Context) state machine
 *
 * Implements the full PDC lifecycle from UE-Specification-1.0.2:
 *
 *   §3.5.8    Packet Delivery Contexts
 *   §3.5.8.1  PDC Selection and Sharing (mapping tuples)
 *   §3.5.8.2  PDC Establishment (SYN handshake, IPDCID/TPDCID)
 *   §3.5.8.3  PDC Close
 *   §3.5.9.2  PDS Manager (alloc/free, mapping/msgmap)
 *   §3.5.9.3  PDC Initiator State Machine (Fig 3-48)
 *   §3.5.9.4  PDC Target State Machine   (Fig 3-49)
 *   §3.5.11.4 PSN spaces (Start_PSN randomization, CLEAR_PSN, CACK_PSN)
 *   §3.5.12.7 NACK processing
 *
 * There are two roles per PDC:
 *   - Initiator: creates the PDC, sets SYN flag until TPDCID known
 *   - Target:    receives first SYN packet, assigns TPDCID, moves to ESTABLISHED
 *
 * One PdcContext holds both forward (initiator→target) and return (target→initiator) PSN spaces.
 */

#ifndef UET_PDC_H
#define UET_PDC_H

#include "uet-pds-header.h"
#include "uet-ses-header.h"

#include <cstdint>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

namespace ns3
{

// ─────────────────────────────────────────────────────────────────────────────
//  PDC Establishment States (spec §3.5.8.2 Fig 3-43, §3.5.9.3 Fig 3-48)
// ─────────────────────────────────────────────────────────────────────────────
enum PdcState : uint8_t
{
    PDC_STATE_CLOSED      = 0, // Not allocated
    PDC_STATE_CREATING    = 1, // Initiator: SYN packets sent, TPDCID unknown
    PDC_STATE_PENDING     = 2, // Target: SYN received, Start_PSN being validated (encrypted)
    PDC_STATE_ESTABLISHED = 3, // Both sides have PDCIDs; normal operation
    PDC_STATE_QUIESCE     = 4, // Close requested; draining in-flight packets
    PDC_STATE_ACK_WAIT    = 5, // Waiting for ACK of close or final packets
    PDC_STATE_CLOSE       = 6, // Fully closed; PSNs saved
};

static inline const char* PdcStateName(PdcState s)
{
    switch (s)
    {
    case PDC_STATE_CLOSED:      return "CLOSED";
    case PDC_STATE_CREATING:    return "CREATING";
    case PDC_STATE_PENDING:     return "PENDING";
    case PDC_STATE_ESTABLISHED: return "ESTABLISHED";
    case PDC_STATE_QUIESCE:     return "QUIESCE";
    case PDC_STATE_ACK_WAIT:    return "ACK_WAIT";
    case PDC_STATE_CLOSE:       return "CLOSE";
    default:                    return "UNKNOWN";
    }
}

// ─────────────────────────────────────────────────────────────────────────────
//  PSN State (per direction — forward or return)
//  Tracks the full PSN life of one direction of a PDC.
// ─────────────────────────────────────────────────────────────────────────────
struct PdcPsnSpace
{
    // ── TX side (sender) ──────────────────────────────────────────────────────
    uint32_t  start_psn;           // Randomized starting PSN (§3.5.8.2)
    uint32_t  next_tx_psn;         // Next PSN to assign to new packet
    uint32_t  cack_psn;            // Cumulative ACK PSN; all below here are delivered+cleared
    uint32_t  clear_psn;           // Highest cleared PSN (CLEAR_PSN, §3.5.11.4.4)

    // Retry tracking per-outstanding-PSN
    struct TxPkt
    {
        uint32_t psn;
        uint32_t retry_cnt;       // Number of RTO retransmits
        uint32_t nack_retry_cnt;  // Number of NACK retransmits (optional counter)
        double   rto_deadline;    // Simulated time deadline (ns)
        bool     needs_guaranteed_delivery; // SES requires guaranteed ACK storage
        uint16_t msg_id;          // SES message this PSN carries (0 = none/CP)
        Ptr<Packet> wirePkt;      // Copy of the full wire packet for retransmission
    };
    std::map<uint32_t /*psn*/, TxPkt> outstanding; // inflight, unacknowledged

    // ── RX side (receiver) ────────────────────────────────────────────────────
    uint32_t  expected_rx_psn;     // ROD: next expected PSN; RUD: highest+1 received
    std::set<uint32_t> received_psns; // Full received bitmap (for SACK, dup-drop)
    uint32_t  rx_cack_psn;         // CACK_PSN for ACKs we generate (matches peer's clear)

    // Guaranteed delivery storage: PSN → SES response waiting for clear
    struct StoredResponse
    {
        uint8_t  return_code;
        uint32_t modified_length;
        uint16_t message_id;
        bool     isDefault;  // Can be coalesced (UET_DEFAULT_RESPONSE)
    };
    std::map<uint32_t, StoredResponse> stored_responses;
    uint32_t  pending_guaranteed_cnt; // Count of stored guaranteed-delivery responses

    // ── Configuration ─────────────────────────────────────────────────────────
    uint32_t  MP_RANGE   = 1024;   // Max PSN range (outstanding window, §3.5.12.2)
    uint32_t  max_gtd_responses = 128; // Max outstanding guaranteed delivery responses

    PdcPsnSpace()
        : start_psn(0), next_tx_psn(0), cack_psn(0), clear_psn(0),
          expected_rx_psn(0), rx_cack_psn(0), pending_guaranteed_cnt(0)
    {
    }

    // Initialize PSN spaces at PDC creation with a random Start_PSN
    void Init(uint32_t startPsn)
    {
        start_psn       = startPsn;
        next_tx_psn     = startPsn;
        cack_psn        = startPsn - 1;
        clear_psn       = startPsn - 1;  // CLEAR_PSN MUST init to Start_PSN - 1 (§3.5.11.4.4)
        expected_rx_psn = startPsn;
        rx_cack_psn     = startPsn - 1;
    }

    // Assign next TX PSN and get clear_psn_offset for wire header
    uint32_t  AssignTxPsn();
    int16_t   GetClearPsnOffset() const;  // signed offset from current TX PSN

    // PSN window check: is psn within [expected, expected+MP_RANGE)?
    bool InExpectedWindow(uint32_t rxPsn) const;

    // Record an ACK/NACK from peer and advance cack_psn.
    // If removed != nullptr, every TxPkt released by this ACK is appended to it
    // so the caller can drive per-message completion tracking.
    void     OnAckReceived(uint32_t ackPsn, uint32_t newCackPsn, uint64_t sackBitmap,
                           uint32_t sackBase, std::vector<TxPkt>* removed = nullptr);
    // Record CLEAR_PSN advancement and free stored responses
    uint32_t OnClearReceived(uint32_t newClearPsn); // returns # freed

    // Record receipt of a packet at RX side
    bool     OnPktReceived(uint32_t rxPsn, bool isRod); // returns false if duplicate

    // Build SACK bitmap (64 bits), base = rx_cack_psn+1, each bit = received
    uint64_t BuildSackBitmap(uint32_t& sackBase) const;

    // Store a guaranteed-delivery SES response
    void StoreResponse(uint32_t psn, uint8_t rc, uint32_t modLen, uint16_t msgId, bool isDef);

    // Is TX window full?
    bool IsWindowFull() const
    {
        if (outstanding.empty()) return false;
        uint32_t oldest = outstanding.begin()->second.psn;
        return (next_tx_psn - oldest) >= MP_RANGE;
    }
};

// ─────────────────────────────────────────────────────────────────────────────
//  PDC mapping tuple (§3.5.8.1)
//  Identifies which flows share a PDC.
// ─────────────────────────────────────────────────────────────────────────────
struct PdcTuple
{
    uint32_t srcFa;    // Source Fabric Address (IP)
    uint32_t dstFa;    // Destination Fabric Address (IP)
    uint8_t  tc;       // Traffic class
    uint8_t  mode;     // UetPdsType: PDS_TYPE_RUD_REQ or PDS_TYPE_ROD_REQ
    uint32_t jobId;    // Optional (0 = wildcard)
    uint16_t srcPid;   // Optional (0 = wildcard)
    uint16_t dstPid;   // Optional (0 = wildcard)

    bool operator==(const PdcTuple& o) const
    {
        return srcFa == o.srcFa && dstFa == o.dstFa &&
               tc == o.tc && mode == o.mode &&
               jobId == o.jobId && srcPid == o.srcPid && dstPid == o.dstPid;
    }
};

// ─────────────────────────────────────────────────────────────────────────────
//  Main PDC context
//  One per active PDC; holds all state for both sides.
// ─────────────────────────────────────────────────────────────────────────────
struct PdcContext
{
    uint16_t    ipdcid;      // Initiator PDCID (assigned by initiator)
    uint16_t    tpdcid;      // Target PDCID (assigned by target; 0 until established)
    PdcState    state;       // Current establishment state
    uint8_t     pdsTypeMode; // PDS_TYPE_RUD_REQ or PDS_TYPE_ROD_REQ
    bool        isRod;       // true = ROD, false = RUD

    PdcPsnSpace fwd;         // Forward direction (initiator→target)
    PdcPsnSpace rev;         // Return direction (target→initiator)

    PdcTuple    tuple;       // Mapping tuple used to create this PDC
    uint32_t    srcFa;       // Initiator IP (for header-mismatch check)
    uint32_t    dstFa;       // Target IP

    // Initiator state machine variables (Fig 3-48)
    bool        syn;         // true while TPDCID not yet known
    bool        closing;
    bool        close_error;
    uint32_t    open_msg_cnt;  // Count of open messages

    // RTO config
    double      rto_init_us;   // Initial RTO in microseconds
    uint32_t    max_rto_retx;  // Max RTO retransmits before failure
    uint32_t    max_nack_retx; // Optional NACK retransmit limit

    // State transition log for debugging / demo output
    std::vector<std::string> transitions;

    PdcContext()
        : ipdcid(0), tpdcid(0), state(PDC_STATE_CLOSED),
          pdsTypeMode(PDS_TYPE_RUD_REQ), isRod(false),
          srcFa(0), dstFa(0),
          syn(true), closing(false), close_error(false), open_msg_cnt(0),
          rto_init_us(30.0), max_rto_retx(7), max_nack_retx(0)
    {
    }

    void Transition(PdcState newState, const std::string& reason)
    {
        std::ostringstream os;
        os << PdcStateName(state) << " → " << PdcStateName(newState) << " [" << reason << "]";
        transitions.push_back(os.str());
        state = newState;
    }
};

// ─────────────────────────────────────────────────────────────────────────────
//  PDS Manager (§3.5.9.2)
//  Manages PDC allocation, mapping, close, and error handling.
// ─────────────────────────────────────────────────────────────────────────────
class UetPdcManager
{
  public:
    explicit UetPdcManager(uint32_t maxPdcs = 256);

    // ── PDC Lifecycle ─────────────────────────────────────────────────────────

    // Create a new initiator PDC for the given tuple (alloc_pdc)
    // Returns IPDCID, or 0 on failure (generates NACK internally).
    uint16_t AllocInitiatorPdc(const PdcTuple& tuple,
                               uint32_t srcFa, uint32_t dstFa,
                               bool isRod, uint8_t tc);

    // Look up by IPDCID (initiator side)
    PdcContext* GetByIpdcid(uint16_t ipdcid);

    // Look up by {srcFa, spdcid} = target mapping tuple (spec §3.5.8.1)
    PdcContext* GetByTargetKey(uint32_t srcFa, uint16_t spdcid);

    // Target: establish PDC on first SYN packet; assigns TPDCID
    // Returns TPDCID, or 0 if resources unavailable.
    // Sets nack_code to the appropriate NACK code on failure.
    uint16_t EstablishTargetPdc(uint32_t srcFa, uint16_t ipdcid_as_spdcid,
                                bool     isRod, uint8_t  pdc_info,
                                uint32_t startPsn,
                                uint8_t& nack_code);

    // Free a PDC (saves last PSN for next Start_PSN computation)
    void FreePdc(uint16_t ipdcid);

    // Map message ID → PDC (msgmap, §3.5.9.2)
    void   MapMsg(uint16_t msgId, uint16_t ipdcid);
    uint16_t GetMsgPdc(uint16_t msgId) const;
    void   UnmapMsg(uint16_t msgId);

    // Select a PDC to close when resources are exhausted (select_pdc_2close)
    uint16_t SelectPdcToClose() const;

    // ── PDC find helpers (assign_pdc) ─────────────────────────────────────────

    // Find an existing ESTABLISHED PDC matching the tuple, or 0 if none
    uint16_t FindExistingPdc(const PdcTuple& tuple) const;

    // ── TX / RX processing ────────────────────────────────────────────────────

    // Initiator TxReq: build and return a filled-in UetPdsHeader ready for wire.
    // If outPsn != nullptr the PSN assigned to this packet is returned so the
    // caller can attach the wire packet for retransmission tracking.
    bool BuildTxReqHeader(uint16_t ipdcid,
                          uint8_t  sesNextHdr,
                          uint32_t payloadBytes,
                          bool     som, bool eom,
                          bool     needsGuaranteedDelivery,
                          UetPdsHeader& outPds,
                          uint32_t* outPsn = nullptr);

    // Target RxReq: process incoming request header; generate ACK/NACK decision
    // Returns: 0=OK, >0=NACK code to send
    uint8_t ProcessRxRequest(UetPdsHeader& pds,
                             uint32_t      srcFaIP,
                             bool          isTrimmed,
                             uint32_t&     outClearPsn,
                             bool&         outIsNewPsn,
                             bool&         outIsDuplicate);

    // Initiator RxAck: process incoming ACK header; returns true if should retransmit
    bool ProcessRxAck(uint16_t           ipdcid,
                      const UetPdsHeader& ack);

    // Build ACK header for a received request
    bool BuildAckHeader(uint32_t srcFa, uint16_t spdcid,
                        uint32_t ackPsn, bool needsSack,
                        UetPdsHeader& outAck);

    // Build NACK header
    bool BuildNackHeader(uint32_t srcFa, uint16_t spdcid, uint16_t dpdcid,
                         uint32_t nackPsn, uint8_t nackCode,
                         UetPdsHeader& outNack);

    // Build a Control Packet
    bool BuildCP(uint16_t ipdcid, UetPdsCtlType ctlType,
                 uint32_t payload, UetPdsHeader& outCp);

    // Process an incoming CP
    void ProcessRxCP(uint16_t ipdcid, const UetPdsHeader& cp);

    // ── PDC close protocol (§3.5.8.3) ─────────────────────────────────────────
    void InitiateClose(uint16_t ipdcid);    // Sends CLOSE_REQUEST CP
    void HandleCloseCommand(uint16_t ipdcid);

    // ── State dump for demo ───────────────────────────────────────────────────
    std::string DescribeAll() const;
    std::string DescribePdc(uint16_t ipdcid) const;

    uint32_t ActivePdcCount() const { return (uint32_t)m_byIpdcid.size(); }

  private:
    uint32_t                              m_maxPdcs;
    uint16_t                              m_nextIpdcid; // simple counter, MUST be non-zero
    uint16_t                              m_nextTpdcid;
    uint32_t                              m_lastUsedPsn; // for next Start_PSN randomization

    // Primary store: IPDCID → PdcContext (initiator side)
    std::unordered_map<uint16_t, PdcContext> m_byIpdcid;

    // Target reverse index: {srcFa, spdcid} → IPDCID it maps to
    // (spdcid = IPDCID as seen by target, or TPDCID on return direction)
    std::map<std::pair<uint32_t,uint16_t>, uint16_t> m_targetIndex;

    // Message-to-PDC mapping (msgmap)
    std::unordered_map<uint16_t, uint16_t> m_msgMap;

    // Generate a random-ish Start_PSN ≥ 2^16 distance from last used
    uint32_t GenStartPsn();

    // Deterministic LCG state for Start_PSN generation (seedable for
    // reproducible simulations; see SetStartPsnSeed)
    uint32_t m_psnRngState = 0x2545F491;

  public:
    // Seed the Start_PSN generator so runs are reproducible
    void SetStartPsnSeed(uint32_t seed) { m_psnRngState = seed ? seed : 1; }

  private:

    // Allocate next unused IPDCID or TPDCID
    uint16_t AllocIpdcid();
    uint16_t AllocTpdcid();
};

} // namespace ns3

#endif // UET_PDC_H
