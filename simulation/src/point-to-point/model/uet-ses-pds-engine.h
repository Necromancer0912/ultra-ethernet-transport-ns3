/**
 * uet-ses-pds-engine.h  —  UET SES/PDS Transaction Engine
 *
 * Central per-endpoint state machine integrating:
 *   - SES (Semantic layer): opcode/message/tag management
 *   - PDS (Packet Delivery): PDC selection, PSN assignment, ACK/NACK
 *   - PDC Manager: lifecycle, mapping, establishment
 *   - Loss recovery: RTO retransmission + NACK-driven retransmission
 *
 * Supports:
 *   RUD  - Reliable Unordered Delivery   (§3.5.3.3)
 *   ROD  - Reliable Ordered Delivery     (§3.5.3.4)
 *   RUDI - Reliable Unordered Idempotent (§3.5.3.5) — pkt_id based, no PDC
 *   UUD  - Unreliable Unordered Delivery (§3.5.3.6)
 */

#ifndef UET_SES_PDS_ENGINE_H
#define UET_SES_PDS_ENGINE_H

#include "uet-pdc.h"
#include "uet-pds-header.h"
#include "uet-ses-header.h"

#include "ns3/event-id.h"
#include "ns3/nstime.h"
#include "ns3/object.h"
#include "ns3/packet.h"

#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <set>
#include <unordered_map>
#include <vector>

namespace ns3
{

// ─────────────────────────────────────────────────────────────────────────────
//  Per-message send state
// ─────────────────────────────────────────────────────────────────────────────
struct UetSendMessage
{
    uint16_t       message_id;           // SES message ID
    UetSesOpcode   opcode;               // SES operation
    uint8_t        tc;                   // Traffic class
    UetDeliveryMode mode;                // RUD/ROD/RUDI/UUD
    uint32_t       dstFa;                // Destination fabric address
    uint64_t       totalBytes;           // Total payload size
    uint64_t       sentBytes;            // Bytes enqueued so far
    uint16_t       ipdcid;              // PDC assigned to this message
    bool           complete;            // All packets sent
    bool           notified;            // Completion callback already fired
    uint32_t       pendingPkts;         // Wire packets sent but not yet ACKed
    bool           needsGuaranteedDel;  // SES response must be reliably stored

    // SES header template for this message
    UetSesHeader   sesHdr;
};

// ─────────────────────────────────────────────────────────────────────────────
//  Send-completion callback: called exactly once per message when the PDS
//  delivers ACKs for all its packets (or the message permanently fails).
//  args: (message_id, succeeded, returnCode)
// ─────────────────────────────────────────────────────────────────────────────
using UetTxCompletionCb = std::function<void(uint16_t, bool, uint8_t)>;

// ─────────────────────────────────────────────────────────────────────────────
//  Receive callback: called when a new SES packet arrives (dedup already done)
//  args: (packet with SES payload, UetSesHeader for this packet)
// ─────────────────────────────────────────────────────────────────────────────
using UetRxMessageCb = std::function<void(Ptr<Packet>, const UetSesHeader&)>;

// ─────────────────────────────────────────────────────────────────────────────
//  Wire-send callback: engine calls this to inject a packet onto the wire.
//  args: (fully built Packet with PDS+SES headers, destination fabric address)
//  The dstFa argument lets the host application route the packet (socket
//  SendTo, direct delivery in a wired demo, etc.). Without it, ACKs and
//  NACKs cannot be returned to the correct peer.
// ─────────────────────────────────────────────────────────────────────────────
using UetWireSendCb = std::function<void(Ptr<Packet>, uint32_t)>;

// ─────────────────────────────────────────────────────────────────────────────
//  Main engine
// ─────────────────────────────────────────────────────────────────────────────
class UetSesPdsEngine : public Object
{
  public:
    static TypeId GetTypeId();
    UetSesPdsEngine();
    ~UetSesPdsEngine() override;

    // ── Configuration ─────────────────────────────────────────────────────────
    void SetSrcFa(uint32_t ip)               { m_srcFa = ip; m_pdc.SetStartPsnSeed(ip * 2654435761u + 1); }
    void SetMsgMtu(uint32_t mtu)             { m_mtu = mtu; }
    void SetTxCompletionCb(UetTxCompletionCb cb) { m_txCb = cb; }
    void SetRxMessageCb(UetRxMessageCb cb)       { m_rxCb = cb; }
    void SetWireSendCb(UetWireSendCb cb)         { m_wireSendCb = cb; }
    void SetRtoInitUs(double us)             { m_rtoInitUs = us; }
    void SetMaxRetries(uint32_t n)           { m_maxRetries = n; }

    // ── TX interface ──────────────────────────────────────────────────────────
    // Enqueue a new RDMA message for transmission.
    // Returns message_id assigned, or 0 on failure.
    uint16_t Send(uint32_t dstFa, uint8_t tc, UetDeliveryMode mode,
                  UetSesOpcode opcode, uint64_t totalBytes,
                  uint64_t bufOffset, uint32_t jobId,
                  uint16_t srcPid, uint16_t  dstRi,
                  bool     needsGuaranteedDel = false);

    // Transmit every pending chunk that fits into the PDC windows.
    // Called automatically by Send(), on ACK arrival, and after retransmits.
    void Pump();

    // ── RX interface ──────────────────────────────────────────────────────────
    // Called by the host when a UET packet arrives.
    // Returns true if the packet was accepted, false if dropped.
    bool ProcessRxPacket(Ptr<Packet> pkt, uint32_t srcFaIP);

    // ── State inspection ──────────────────────────────────────────────────────
    uint32_t GetActivePdcCount() const       { return m_pdc.ActivePdcCount(); }
    std::string GetPdcStateReport()          { return m_pdc.DescribeAll(); }
    uint32_t GetTxPktCount() const           { return m_txPktCount; }
    uint32_t GetRxPktCount() const           { return m_rxPktCount; }
    uint32_t GetNackSentCount() const        { return m_nackSentCnt; }
    uint32_t GetNackRcvdCount() const        { return m_nackRcvdCnt; }
    uint32_t GetRetxCount() const            { return m_retxCnt; }
    uint32_t GetMsgsCompleted() const        { return m_msgsCompleted; }
    uint32_t GetMsgsFailed() const           { return m_msgsFailed; }
    uint32_t GetPendingMsgCount() const      { return (uint32_t)m_txMsgs.size(); }

  private:
    uint32_t        m_srcFa;
    uint32_t        m_mtu;          // Max PDU payload bytes
    UetPdcManager   m_pdc;

    uint16_t        m_nextMsgId;
    double          m_rtoInitUs;    // Initial retransmission timeout (µs)
    uint32_t        m_maxRetries;   // Max RTO retransmits before message failure

    // Inflight TX messages (ordered map: deterministic pump order)
    std::map<uint16_t, UetSendMessage> m_txMsgs;

    // RUDI: packet ID counter (locally unique) and reliability tracking
    uint32_t m_nextRudiPktId;
    struct RudiTxPkt
    {
        Ptr<Packet> wirePkt;
        uint32_t    dstFa;
        uint16_t    msgId;
        uint32_t    retry_cnt;
        double      rto_deadline_ns;
    };
    std::map<uint32_t /*pkt_id*/, RudiTxPkt> m_rudiOutstanding;
    // RUDI RX dedup: {srcFa, pkt_id} pairs already delivered to SES
    std::set<std::pair<uint32_t, uint32_t>> m_rudiSeen;

    // ROD reorder buffer (target side): PDC → (psn → SES chunk).
    // Out-of-order arrivals are held here and released to the application
    // strictly in PSN order once the gap fills.
    std::map<uint16_t, std::map<uint32_t, std::pair<Ptr<Packet>, UetSesHeader>>> m_rodRxBuffer;

    // Callbacks
    UetTxCompletionCb m_txCb;
    UetRxMessageCb    m_rxCb;
    UetWireSendCb     m_wireSendCb;

    // Retransmission timer
    EventId m_rtoEvent;

    // Reentrancy guard: with a synchronous wire (demo fabrics), sending a
    // packet can deliver an ACK inline, which calls Pump() again while the
    // outer Pump() is still iterating. Nested calls are deferred.
    bool m_inPump = false;
    bool m_pumpAgain = false;

    // Counters
    uint32_t m_txPktCount;
    uint32_t m_rxPktCount;
    uint32_t m_nackSentCnt;
    uint32_t m_nackRcvdCnt;
    uint32_t m_retxCnt;
    uint32_t m_msgsCompleted;
    uint32_t m_msgsFailed;

    // ── Private helpers ──────────────────────────────────────────────────────
    Ptr<Packet> BuildUdpPayloadPacket(UetSendMessage& msg, uint32_t chunkBytes,
                                       bool isSom, bool isEom);
    void SendNack(uint32_t dstFa, uint16_t spdcid, uint16_t dpdcid,
                  uint32_t nackPsn, uint8_t nackCode);
    void HandleRxNack(uint16_t ipdcid, const UetPdsHeader& nack);
    void HandleRxAck(const UetPdsHeader& pds);
    void HandleRxRequest(Ptr<Packet> pkt, UetPdsHeader& pds, uint32_t srcFaIP);
    void HandleRxRudi(Ptr<Packet> pkt, UetPdsHeader& pds, uint32_t srcFaIP);

    // Fire the completion callback for a message exactly once, then drop it
    void CompleteMessage(uint16_t msgId, bool ok, uint8_t rc);
    // Called whenever ACKed PSNs are released: updates per-message counters
    void OnPsnsReleased(const std::vector<PdcPsnSpace::TxPkt>& released);

    // Retransmit a stored wire packet on the PDC that owns it
    void RetransmitPsn(PdcContext* ctx, uint32_t psn);
    // RTO machinery: schedule/scan outstanding packets
    void ScheduleRtoCheck();
    void RtoCheck();
    bool HasOutstandingTx() const;

    uint16_t AllocMsgId();
};

} // namespace ns3

#endif // UET_SES_PDS_ENGINE_H
