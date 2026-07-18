/**
 * uet-ses-pds-engine.cc  —  UET SES/PDS Transaction Engine implementation
 *
 * References: UE-Specification-1.0.2 §3.4, §3.5
 */

#include "uet-ses-pds-engine.h"

#include "ns3/log.h"
#include "ns3/simulator.h"

#include <iomanip>
#include <sstream>

namespace ns3
{

NS_LOG_COMPONENT_DEFINE("UetSesPdsEngine");
NS_OBJECT_ENSURE_REGISTERED(UetSesPdsEngine);

TypeId
UetSesPdsEngine::GetTypeId()
{
    static TypeId tid = TypeId("ns3::UetSesPdsEngine")
                            .SetParent<Object>()
                            .AddConstructor<UetSesPdsEngine>();
    return tid;
}

UetSesPdsEngine::UetSesPdsEngine()
    : m_srcFa(0), m_mtu(4096), m_pdc(256),
      m_nextMsgId(1), m_rtoInitUs(30.0), m_maxRetries(7),
      m_nextRudiPktId(1),
      m_txPktCount(0), m_rxPktCount(0),
      m_nackSentCnt(0), m_nackRcvdCnt(0), m_retxCnt(0),
      m_msgsCompleted(0), m_msgsFailed(0)
{
}

UetSesPdsEngine::~UetSesPdsEngine()
{
    if (m_rtoEvent.IsRunning())
        Simulator::Cancel(m_rtoEvent);
}

// ─────────────────────────────────────────────────────────────────────────────
//  AllocMsgId
// ─────────────────────────────────────────────────────────────────────────────
uint16_t
UetSesPdsEngine::AllocMsgId()
{
    uint16_t id = m_nextMsgId;
    while (id == 0 || m_txMsgs.count(id))
    {
        ++id;
        if (id == 0) id = 1;
    }
    m_nextMsgId = id + 1;
    if (m_nextMsgId == 0) m_nextMsgId = 1;
    return id;
}

// ═════════════════════════════════════════════════════════════════════════════
//  Send  —  enqueue a new RDMA message
// ═════════════════════════════════════════════════════════════════════════════
uint16_t
UetSesPdsEngine::Send(uint32_t dstFa,  uint8_t tc,  UetDeliveryMode mode,
                       UetSesOpcode opcode, uint64_t totalBytes,
                       uint64_t bufOffset, uint32_t jobId,
                       uint16_t srcPid, uint16_t dstRi,
                       bool needsGuaranteedDel)
{
    NS_LOG_FUNCTION(this << dstFa << (int)mode << totalBytes);

    // ── 1. Select or create a PDC (RUD/ROD only; RUDI/UUD are PDC-less) ──────
    bool isRod = (mode == UET_MODE_ROD);
    PdcTuple tuple;
    tuple.srcFa  = m_srcFa;
    tuple.dstFa  = dstFa;
    tuple.tc     = tc;
    tuple.mode   = isRod ? PDS_TYPE_ROD_REQ : PDS_TYPE_RUD_REQ;
    tuple.jobId  = jobId;
    tuple.srcPid = srcPid;
    tuple.dstPid = dstRi;

    uint16_t ipdcid = 0;
    if (mode == UET_MODE_RUD || mode == UET_MODE_ROD)
    {
        ipdcid = m_pdc.FindExistingPdc(tuple);
        if (ipdcid == 0)
        {
            ipdcid = m_pdc.AllocInitiatorPdc(tuple, m_srcFa, dstFa, isRod, tc);
            if (ipdcid == 0)
            {
                NS_LOG_WARN("UetSesPdsEngine: cannot allocate PDC for dstFa=0x"
                            << std::hex << dstFa << std::dec);
                return 0;
            }
            NS_LOG_INFO("UetSesPdsEngine: new PDC IPDCID=" << ipdcid << " for dstFa=0x"
                        << std::hex << dstFa << std::dec);
        }
    }

    // ── 2. Build message state ─────────────────────────────────────────────────
    uint16_t msgId = AllocMsgId();
    if (ipdcid != 0)
        m_pdc.MapMsg(msgId, ipdcid);

    UetSendMessage& msg = m_txMsgs[msgId];
    msg.message_id        = msgId;
    msg.opcode            = opcode;
    msg.tc                = tc;
    msg.mode              = mode;
    msg.dstFa             = dstFa;
    msg.totalBytes        = totalBytes;
    msg.sentBytes         = 0;
    msg.ipdcid            = ipdcid;
    msg.complete          = false;
    msg.notified          = false;
    msg.pendingPkts       = 0;
    msg.needsGuaranteedDel = needsGuaranteedDel;

    bool multiPkt = (totalBytes > m_mtu);
    bool needsMatchBits = (opcode == UET_TAGGED_SEND || opcode == UET_RENDEZVOUS_TSEND);

    // Build the SES header template  (actual packet headers filled in per-chunk)
    UetSesNextHdr fmt = UetSesHeader::ChooseRequestFormat(opcode, multiPkt, needsMatchBits);
    msg.sesHdr.SetFormat(fmt);
    msg.sesHdr.opcode          = (uint8_t)opcode;
    msg.sesHdr.version         = 0;
    msg.sesHdr.dc              = false;
    msg.sesHdr.ie              = false;
    msg.sesHdr.rel             = true;
    msg.sesHdr.hd              = false;
    msg.sesHdr.message_id      = msgId;
    msg.sesHdr.ri_generation   = 0;
    msg.sesHdr.jobId           = jobId;
    msg.sesHdr.pidOnFep        = srcPid;
    msg.sesHdr.resource_index  = dstRi;
    msg.sesHdr.buffer_offset   = bufOffset;
    msg.sesHdr.request_length  = (uint32_t)totalBytes;
    msg.sesHdr.header_data     = 0;
    msg.sesHdr.match_bits      = 0;
    msg.sesHdr.hasAtomic       = false;

    NS_LOG_INFO("UetSesPdsEngine: enqueued msgId=" << msgId
                << " opcode=0x" << std::hex << (int)opcode << std::dec
                << " totalBytes=" << totalBytes
                << " ipdcid=" << ipdcid
                << " mode=" << (int)mode);

    Pump();
    return msgId;
}

// ═════════════════════════════════════════════════════════════════════════════
//  Pump  —  transmit all sendable chunks for all pending messages
// ═════════════════════════════════════════════════════════════════════════════
void
UetSesPdsEngine::Pump()
{
    if (m_inPump)
    {
        // Nested call from an inline ACK/NACK: defer to the outer Pump
        m_pumpAgain = true;
        return;
    }
    m_inPump = true;

    do
    {
        m_pumpAgain = false;

        // Snapshot the message IDs: sending a packet can synchronously
        // complete (and erase) messages, so never hold live iterators
        // across a wire send.
        std::vector<uint16_t> ids;
        ids.reserve(m_txMsgs.size());
        for (auto& kv : m_txMsgs) ids.push_back(kv.first);

        for (uint16_t id : ids)
        {
            while (true)
            {
                auto it = m_txMsgs.find(id);
                if (it == m_txMsgs.end()) break;   // completed inline
                UetSendMessage& msg = it->second;
                if (msg.complete) break;

                uint64_t remaining = msg.totalBytes - msg.sentBytes;
                if (remaining == 0) { msg.complete = true; break; }

                // Check PDC window (stop this message if full)
                if (msg.mode == UET_MODE_RUD || msg.mode == UET_MODE_ROD)
                {
                    PdcContext* ctx = m_pdc.GetByIpdcid(msg.ipdcid);
                    if (!ctx || ctx->fwd.IsWindowFull())
                        break;
                }

                // Slice into at most MTU-sized PDS payload
                uint32_t chunk = (uint32_t)std::min<uint64_t>(remaining, m_mtu);
                bool isSom = (msg.sentBytes == 0);
                bool isEom = (msg.sentBytes + chunk >= msg.totalBytes);

                // Update SES flags
                msg.sesHdr.som         = isSom;
                msg.sesHdr.eom         = isEom;
                msg.sesHdr.message_offset = (uint32_t)msg.sentBytes;
                msg.sesHdr.payload_length = (uint16_t)chunk;

                Ptr<Packet> out = BuildUdpPayloadPacket(msg, chunk, isSom, isEom);
                if (!out) break;

                msg.sentBytes += chunk;
                if (isEom) msg.complete = true;
                uint32_t dstFa = msg.dstFa;
                UetDeliveryMode mode = msg.mode;
                bool nowComplete = msg.complete;

                ++m_txPktCount;
                // msg may be erased inside this call — do not touch it after
                if (m_wireSendCb) m_wireSendCb(out, dstFa);

                // UUD: no ACK will ever come; report completion at send time
                if (mode == UET_MODE_UUD && nowComplete)
                {
                    CompleteMessage(id, true, RC_OK);
                    break;
                }
            }
        }
    } while (m_pumpAgain);

    m_inPump = false;
    ScheduleRtoCheck();
}

// ─────────────────────────────────────────────────────────────────────────────
//  BuildUdpPayloadPacket  —  construct complete PDS+SES packet
// ─────────────────────────────────────────────────────────────────────────────
Ptr<Packet>
UetSesPdsEngine::BuildUdpPayloadPacket(UetSendMessage& msg, uint32_t chunkBytes,
                                        bool isSom, bool isEom)
{
    UetSesNextHdr sesNextHdr = msg.sesHdr.GetFormat();

    if (msg.mode == UET_MODE_UUD)
    {
        // UUD: no PDC, minimal PDS header, no reliability state
        UetPdsHeader pds;
        pds.prologue.pdsType = PDS_TYPE_UUD_REQ;
        pds.prologue.nextHdr = sesNextHdr;
        pds.prologue.flags   = UetPdsFlags{};
        pds.spdcid           = 0;
        pds.dpdcid           = 0;

        Ptr<Packet> p = Create<Packet>(chunkBytes);
        p->AddHeader(msg.sesHdr);
        p->AddHeader(pds);
        return p;
    }

    if (msg.mode == UET_MODE_RUDI)
    {
        // RUDI: reliable but PDC-less; reliability keyed on pkt_id (§3.5.3.5)
        UetPdsHeader pds;
        pds.prologue.pdsType = PDS_TYPE_RUDI_REQ;
        pds.prologue.nextHdr = sesNextHdr;
        pds.prologue.flags   = UetPdsFlags{};
        uint32_t pktId       = m_nextRudiPktId++;
        pds.pkt_id           = pktId;
        pds.spdcid           = 0;
        pds.dpdcid           = 0;

        Ptr<Packet> p = Create<Packet>(chunkBytes);
        p->AddHeader(msg.sesHdr);
        p->AddHeader(pds);

        RudiTxPkt tracked;
        tracked.wirePkt         = p->Copy();
        tracked.dstFa           = msg.dstFa;
        tracked.msgId           = msg.message_id;
        tracked.retry_cnt       = 0;
        tracked.rto_deadline_ns = Simulator::Now().GetNanoSeconds() + m_rtoInitUs * 1000.0;
        m_rudiOutstanding[pktId] = tracked;
        ++msg.pendingPkts;
        return p;
    }

    // RUD / ROD: build PDS request header with PSN
    UetPdsHeader pds;
    uint32_t txPsn = 0;
    bool ok = m_pdc.BuildTxReqHeader(msg.ipdcid,
                                      (uint8_t)sesNextHdr,
                                      chunkBytes,
                                      isSom, isEom,
                                      msg.needsGuaranteedDel,
                                      pds, &txPsn);
    if (!ok) return nullptr;

    Ptr<Packet> p = Create<Packet>(chunkBytes);
    p->AddHeader(msg.sesHdr);
    p->AddHeader(pds);

    // Store a copy of the wire packet against the PSN so RTO / NACK
    // recovery can retransmit it byte-for-byte.
    PdcContext* ctx = m_pdc.GetByIpdcid(msg.ipdcid);
    if (ctx)
    {
        auto it = ctx->fwd.outstanding.find(txPsn);
        if (it != ctx->fwd.outstanding.end())
        {
            it->second.msg_id  = msg.message_id;
            it->second.wirePkt = p->Copy();
            it->second.rto_deadline =
                Simulator::Now().GetNanoSeconds() + m_rtoInitUs * 1000.0;
        }
    }
    ++msg.pendingPkts;
    return p;
}

// ═════════════════════════════════════════════════════════════════════════════
//  ProcessRxPacket  —  RX dispatch
// ═════════════════════════════════════════════════════════════════════════════
bool
UetSesPdsEngine::ProcessRxPacket(Ptr<Packet> pkt, uint32_t srcFaIP)
{
    NS_LOG_FUNCTION(this << srcFaIP);

    // ── 1. Parse PDS header ───────────────────────────────────────────────────
    UetPdsHeader pds;
    if (pkt->GetSize() < 2)
    {
        NS_LOG_WARN("RxPacket: too short for PDS header");
        return false;
    }
    pkt->RemoveHeader(pds);

    ++m_rxPktCount;

    // ── 2. Dispatch by PDS type ───────────────────────────────────────────────
    if (pds.IsAck())
    {
        HandleRxAck(pds);
        return true;
    }

    if (pds.IsNack())
    {
        ++m_nackRcvdCnt;
        HandleRxNack(pds.dpdcid, pds); // our IPDCID echoed as dpdcid
        return true;
    }

    if (pds.IsCP())
    {
        m_pdc.ProcessRxCP(pds.dpdcid, pds);
        return true;
    }

    if (pds.IsRequest())
    {
        HandleRxRequest(pkt, pds, srcFaIP);
        return true;
    }

    if (pds.prologue.pdsType == PDS_TYPE_RUDI_REQ)
    {
        HandleRxRudi(pkt, pds, srcFaIP);
        return true;
    }

    if (pds.prologue.pdsType == PDS_TYPE_RUDI_RESP)
    {
        // Initiator side: RUDI delivery confirmed for pkt_id
        auto it = m_rudiOutstanding.find(pds.pkt_id);
        if (it != m_rudiOutstanding.end())
        {
            uint16_t msgId = it->second.msgId;
            m_rudiOutstanding.erase(it);
            auto mit = m_txMsgs.find(msgId);
            if (mit != m_txMsgs.end())
            {
                UetSendMessage& msg = mit->second;
                if (msg.pendingPkts > 0) --msg.pendingPkts;
                if (msg.complete && msg.pendingPkts == 0)
                    CompleteMessage(msgId, true, RC_OK);
            }
        }
        return true;
    }

    if (pds.IsUud())
    {
        // UUD: unreliable — pass directly to SES
        if (pkt->GetSize() > 0)
        {
            UetSesHeader ses;
            bool hasSes = (pds.prologue.nextHdr != UET_HDR_NONE);
            if (hasSes)
            {
                ses.SetFormat((UetSesNextHdr)pds.prologue.nextHdr);
                pkt->RemoveHeader(ses);
            }
            if (m_rxCb) m_rxCb(pkt, ses);
        }
        return true;
    }

    NS_LOG_WARN("RxPacket: unrecognized PDS type=" << (int)pds.prologue.pdsType);
    return false;
}

// ─────────────────────────────────────────────────────────────────────────────
//  HandleRxAck  (initiator RX path, Fig 3-48)
// ─────────────────────────────────────────────────────────────────────────────
void
UetSesPdsEngine::HandleRxAck(const UetPdsHeader& pds)
{
    // dpdcid in the ACK is our IPDCID echoed back by the target
    PdcContext* ctx = m_pdc.GetByIpdcid(pds.dpdcid);
    if (!ctx)
    {
        NS_LOG_WARN("RxAck: unknown IPDCID=" << pds.dpdcid);
        return;
    }

    // Learn TPDCID / clear SYN and collect the PSNs this ACK releases
    std::vector<PdcPsnSpace::TxPkt> released;
    {
        // Inline what ProcessRxAck did, but with release tracking
        if (ctx->syn && pds.spdcid != 0)
        {
            ctx->tpdcid = pds.spdcid;
            ctx->syn    = false;
            ctx->Transition(PDC_STATE_ESTABLISHED,
                            "received_first_ack_tpdcid=" + std::to_string(pds.spdcid));
            NS_LOG_INFO("Initiator: PDC " << ctx->ipdcid
                        << " ESTABLISHED, TPDCID=" << ctx->tpdcid);
        }

        uint32_t ackPsn  = pds.ComputeAckPsn();
        uint32_t cackPsn = pds.cack_psn;

        uint64_t sackBitmap = 0;
        uint32_t sackBase   = 0;
        if (pds.prologue.pdsType == PDS_TYPE_ACK_CC ||
            pds.prologue.pdsType == PDS_TYPE_ACK_CCX)
        {
            sackBitmap = pds.sack_bitmap;
            sackBase   = pds.ComputeSackPsn();
        }

        ctx->fwd.OnAckReceived(ackPsn, cackPsn, sackBitmap, sackBase, &released);

        NS_LOG_LOGIC("Initiator: RX ACK psn=" << ackPsn << " cack=" << cackPsn
                    << " PDC=" << ctx->ipdcid
                    << " outstanding=" << ctx->fwd.outstanding.size());
    }

    OnPsnsReleased(released);

    // ACK freed window space: try to push more chunks out
    Pump();
}

// ─────────────────────────────────────────────────────────────────────────────
//  HandleRxRequest  (target RX path, Fig 3-49)
// ─────────────────────────────────────────────────────────────────────────────
void
UetSesPdsEngine::HandleRxRequest(Ptr<Packet> pkt, UetPdsHeader& pds, uint32_t srcFaIP)
{
    uint32_t clearPsn;
    bool     isNew, isDup;
    bool isTrimmed = false; // future: check for trimmed indication in IP flags
    uint8_t nackCode = m_pdc.ProcessRxRequest(pds, srcFaIP, isTrimmed,
                                               clearPsn, isNew, isDup);

    if (nackCode != 0)
    {
        SendNack(srcFaIP, pds.dpdcid, pds.spdcid, pds.psn, nackCode);
        ++m_nackSentCnt;
        return;
    }

    if (isDup)
    {
        NS_LOG_LOGIC("RxRequest: duplicate psn=" << pds.psn << " from srcFa=0x"
                     << std::hex << srcFaIP << std::dec << " — payload dropped, re-ACKed");
    }

    // Parse SES header (if present)
    bool hasSes = (pds.prologue.nextHdr != UET_HDR_NONE);
    UetSesHeader ses;
    if (hasSes && pkt->GetSize() >= UetSesHeader::FormatSize(
                      (UetSesNextHdr)pds.prologue.nextHdr, true))
    {
        ses.SetFormat((UetSesNextHdr)pds.prologue.nextHdr);
        ses.som = true; // set to allow right parse; actual bit read in Deserialize
        pkt->RemoveHeader(ses);
    }

    // Notify application layer for genuinely new payload only. ROD delivers
    // strictly in PSN order: out-of-order chunks wait in the reorder buffer
    // until the missing PSN arrives (retransmitted by the initiator's RTO).
    if (hasSes && isNew && m_rxCb)
    {
        PdcContext* tctx = m_pdc.GetByTargetKey(srcFaIP, pds.spdcid);
        if (tctx && tctx->isRod)
        {
            auto& buf = m_rodRxBuffer[tctx->ipdcid];
            buf[pds.psn] = {pkt->Copy(), ses};
            for (auto bit = buf.begin();
                 bit != buf.end() &&
                 (int32_t)(bit->first - tctx->fwd.expected_rx_psn) < 0;)
            {
                m_rxCb(bit->second.first, bit->second.second);
                bit = buf.erase(bit);
            }
        }
        else
        {
            m_rxCb(pkt, ses);
        }
    }

    // Build and send ACK back to the initiator (also for duplicates,
    // because their original ACK may have been the loss)
    bool needsSack = true; // SACK bitmap always helps the initiator
    UetPdsHeader ack;
    if (m_pdc.BuildAckHeader(srcFaIP, pds.spdcid, pds.psn, needsSack, ack))
    {
        Ptr<Packet> ackPkt = Create<Packet>(0);
        ackPkt->AddHeader(ack);
        if (m_wireSendCb) m_wireSendCb(ackPkt, srcFaIP);
    }
}

// ─────────────────────────────────────────────────────────────────────────────
//  HandleRxRudi  (target side: dedup on pkt_id, respond, deliver)
// ─────────────────────────────────────────────────────────────────────────────
void
UetSesPdsEngine::HandleRxRudi(Ptr<Packet> pkt, UetPdsHeader& pds, uint32_t srcFaIP)
{
    auto key   = std::make_pair(srcFaIP, pds.pkt_id);
    bool isNew = m_rudiSeen.insert(key).second;

    if (isNew && pkt->GetSize() > 0)
    {
        UetSesHeader ses;
        bool hasSes = (pds.prologue.nextHdr != UET_HDR_NONE);
        if (hasSes)
        {
            ses.SetFormat((UetSesNextHdr)pds.prologue.nextHdr);
            pkt->RemoveHeader(ses);
        }
        if (m_rxCb) m_rxCb(pkt, ses);
    }

    // Always respond (idempotent): retransmits get a fresh RUDI_RESP too
    UetPdsHeader resp;
    resp.prologue.pdsType = PDS_TYPE_RUDI_RESP;
    resp.prologue.nextHdr = UET_HDR_NONE;
    resp.prologue.flags   = UetPdsFlags{};
    resp.pkt_id           = pds.pkt_id;
    Ptr<Packet> respPkt = Create<Packet>(0);
    respPkt->AddHeader(resp);
    if (m_wireSendCb) m_wireSendCb(respPkt, srcFaIP);
}

// ─────────────────────────────────────────────────────────────────────────────
//  CompleteMessage / OnPsnsReleased
// ─────────────────────────────────────────────────────────────────────────────
void
UetSesPdsEngine::CompleteMessage(uint16_t msgId, bool ok, uint8_t rc)
{
    auto it = m_txMsgs.find(msgId);
    if (it == m_txMsgs.end() || it->second.notified)
        return;
    it->second.notified = true;
    if (ok) ++m_msgsCompleted; else ++m_msgsFailed;

    // On failure, purge any leftover state owned by this message so orphaned
    // PSNs / RUDI packets do not keep retransmitting forever.
    if (!ok)
    {
        if (it->second.ipdcid != 0)
        {
            PdcContext* ctx = m_pdc.GetByIpdcid(it->second.ipdcid);
            if (ctx)
            {
                for (auto oit = ctx->fwd.outstanding.begin();
                     oit != ctx->fwd.outstanding.end();)
                {
                    if (oit->second.msg_id == msgId)
                        oit = ctx->fwd.outstanding.erase(oit);
                    else
                        ++oit;
                }
            }
        }
        for (auto rit = m_rudiOutstanding.begin(); rit != m_rudiOutstanding.end();)
        {
            if (rit->second.msgId == msgId)
                rit = m_rudiOutstanding.erase(rit);
            else
                ++rit;
        }
    }

    m_pdc.UnmapMsg(msgId);
    if (m_txCb) m_txCb(msgId, ok, rc);
    m_txMsgs.erase(msgId);
}

void
UetSesPdsEngine::OnPsnsReleased(const std::vector<PdcPsnSpace::TxPkt>& released)
{
    std::vector<uint16_t> done;
    for (const auto& txp : released)
    {
        if (txp.msg_id == 0) continue;
        auto it = m_txMsgs.find(txp.msg_id);
        if (it == m_txMsgs.end()) continue;
        UetSendMessage& msg = it->second;
        if (msg.pendingPkts > 0) --msg.pendingPkts;
        if (msg.complete && msg.pendingPkts == 0 && !msg.notified)
            done.push_back(txp.msg_id);
    }
    for (uint16_t id : done)
        CompleteMessage(id, true, RC_OK);
}

// ─────────────────────────────────────────────────────────────────────────────
//  SendNack
// ─────────────────────────────────────────────────────────────────────────────
void
UetSesPdsEngine::SendNack(uint32_t dstFa, uint16_t spdcid, uint16_t dpdcid,
                           uint32_t nackPsn, uint8_t nackCode)
{
    UetPdsHeader nack;
    m_pdc.BuildNackHeader(dstFa, spdcid, dpdcid, nackPsn, nackCode, nack);
    Ptr<Packet> pkt = Create<Packet>(0);
    pkt->AddHeader(nack);
    if (m_wireSendCb) m_wireSendCb(pkt, dstFa);
    NS_LOG_INFO("UetEngine: Sent NACK code=" << UetPdsHeader::NackCodeName(nackCode)
                << " psn=" << nackPsn);
}

// ─────────────────────────────────────────────────────────────────────────────
//  RetransmitPsn — resend a stored wire packet
// ─────────────────────────────────────────────────────────────────────────────
void
UetSesPdsEngine::RetransmitPsn(PdcContext* ctx, uint32_t psn)
{
    auto it = ctx->fwd.outstanding.find(psn);
    if (it == ctx->fwd.outstanding.end() || !it->second.wirePkt)
        return;

    // Safety valve: a synchronous zero-latency fabric can ping-pong
    // NACK/retransmit; beyond this bound leave recovery to the RTO path.
    if (it->second.nack_retry_cnt > 32)
        return;

    ++m_retxCnt;
    ++m_txPktCount;
    it->second.rto_deadline = Simulator::Now().GetNanoSeconds() +
        m_rtoInitUs * 1000.0 * (double)(1u << std::min(it->second.retry_cnt, 10u));

    NS_LOG_INFO("UetEngine: RETX psn=" << psn << " PDC=" << ctx->ipdcid
                << " retry=" << it->second.retry_cnt);
    if (m_wireSendCb) m_wireSendCb(it->second.wirePkt->Copy(), ctx->dstFa);
}

// ─────────────────────────────────────────────────────────────────────────────
//  HandleRxNack  (initiator processing per §3.5.12.7 error model)
// ─────────────────────────────────────────────────────────────────────────────
void
UetSesPdsEngine::HandleRxNack(uint16_t ipdcid, const UetPdsHeader& nack)
{
    NS_LOG_INFO("UetEngine: RX NACK ipdcid=" << ipdcid
                << " code=" << UetPdsHeader::NackCodeName(nack.nack_code)
                << "(0x" << std::hex << (int)nack.nack_code << std::dec << ")"
                << " nack_psn=" << nack.nack_psn);

    PdcContext* ctx = m_pdc.GetByIpdcid(ipdcid);

    switch (nack.nack_code)
    {
    case NACK_TRIMMED:
    case NACK_TRIMMED_LASTHOP:
    case NACK_NO_PKT_BUFFER:
    case NACK_NO_GTD_DEL_AVAIL:
    case NACK_NO_SES_MSG_AVAIL:
    case NACK_NO_RESOURCE:
    case NACK_PSN_OOR_WINDOW:
        // NORMAL/RETX — retransmit same PSN on same PDC
        if (ctx)
        {
            auto it = ctx->fwd.outstanding.find(nack.nack_psn);
            if (it != ctx->fwd.outstanding.end())
            {
                it->second.nack_retry_cnt++;
                RetransmitPsn(ctx, nack.nack_psn);
            }
        }
        break;

    case NACK_ROD_OOO:
        // ROD gap: the target refuses everything past the missing PSN.
        // Retransmit every outstanding PSN up to and including nack_psn,
        // lowest first, so the ordered stream can resume (go-back-N style).
        if (ctx)
        {
            std::vector<uint32_t> toResend;
            for (auto& kv : ctx->fwd.outstanding)
            {
                if ((int32_t)(kv.first - nack.nack_psn) <= 0)
                    toResend.push_back(kv.first);
            }
            for (uint32_t p : toResend)
            {
                auto it = ctx->fwd.outstanding.find(p);
                if (it != ctx->fwd.outstanding.end())
                {
                    it->second.nack_retry_cnt++;
                    RetransmitPsn(ctx, p);
                }
            }
        }
        break;

    case NACK_NO_PDC_AVAIL:
    case NACK_NO_CCC_AVAIL:
    case NACK_NO_BITMAP:
        // NORMAL/RETRY — spec: create a new PDC and retry. Modeled as a
        // plain retransmit after backoff (single-PDC-per-tuple simulation).
        if (ctx)
        {
            auto it = ctx->fwd.outstanding.find(nack.nack_psn);
            if (it != ctx->fwd.outstanding.end())
                RetransmitPsn(ctx, nack.nack_psn);
        }
        break;

    case NACK_INV_DPDCID:
    case NACK_PDC_HDR_MISMATCH:
    case NACK_CLOSING:
    case NACK_PDC_MODE_MISMATCH:
        // PDC_FATAL — fail inflight messages and close the PDC
        NS_LOG_WARN("  → PDC_FATAL: closing PDC " << ipdcid);
        if (ctx)
        {
            std::vector<uint16_t> failed;
            for (auto& kv : m_txMsgs)
                if (kv.second.ipdcid == ipdcid && !kv.second.notified)
                    failed.push_back(kv.first);
            for (uint16_t id : failed)
                CompleteMessage(id, false, RC_INTERNAL_ERROR);
            m_pdc.InitiateClose(ipdcid);
        }
        break;

    case NACK_INVALID_SYN:
        // PDC_ERR — PDC remains active
        NS_LOG_INFO("  → PDC_ERR: invalid SYN, continuing");
        break;

    case NACK_NEW_START_PSN:
        // Encrypted: target gives us new Start_PSN in nack.nack_payload
        NS_LOG_INFO("  → NEW_START_PSN=0x" << std::hex << nack.nack_payload << std::dec);
        break;

    default:
        NS_LOG_WARN("  → Unknown NACK code=0x" << std::hex << (int)nack.nack_code);
        break;
    }
}

// ─────────────────────────────────────────────────────────────────────────────
//  RTO machinery
// ─────────────────────────────────────────────────────────────────────────────
bool
UetSesPdsEngine::HasOutstandingTx() const
{
    if (!m_rudiOutstanding.empty()) return true;
    for (auto& kv : m_txMsgs)
        if (kv.second.pendingPkts > 0)
            return true;
    return false;
}

void
UetSesPdsEngine::ScheduleRtoCheck()
{
    if (m_rtoEvent.IsRunning())
        return;
    if (!HasOutstandingTx())
        return;
    m_rtoEvent = Simulator::Schedule(MicroSeconds(m_rtoInitUs),
                                     &UetSesPdsEngine::RtoCheck, this);
}

void
UetSesPdsEngine::RtoCheck()
{
    double nowNs = Simulator::Now().GetNanoSeconds();

    // ── RUD/ROD outstanding packets across all inflight messages ─────────────
    // Collect owning PDCs from inflight messages (each message knows its PDC)
    std::set<uint16_t> pdcs;
    for (auto& kv : m_txMsgs)
        if (kv.second.ipdcid != 0)
            pdcs.insert(kv.second.ipdcid);

    std::vector<uint16_t> failedMsgs;
    for (uint16_t ipdcid : pdcs)
    {
        PdcContext* ctx = m_pdc.GetByIpdcid(ipdcid);
        if (!ctx) continue;

        std::vector<uint32_t> expired;
        for (auto& kv : ctx->fwd.outstanding)
            if (kv.second.wirePkt && kv.second.rto_deadline <= nowNs)
                expired.push_back(kv.first);

        for (uint32_t psn : expired)
        {
            auto it = ctx->fwd.outstanding.find(psn);
            if (it == ctx->fwd.outstanding.end()) continue;
            if (it->second.retry_cnt >= m_maxRetries)
            {
                // Retry budget exhausted: fail the owning message
                if (it->second.msg_id != 0)
                    failedMsgs.push_back(it->second.msg_id);
                ctx->fwd.outstanding.erase(it);
                continue;
            }
            it->second.retry_cnt++;
            RetransmitPsn(ctx, psn);
        }
    }

    // ── RUDI outstanding packets ─────────────────────────────────────────────
    std::vector<uint32_t> rudiExpired;
    for (auto& kv : m_rudiOutstanding)
        if (kv.second.rto_deadline_ns <= nowNs)
            rudiExpired.push_back(kv.first);

    for (uint32_t pktId : rudiExpired)
    {
        auto it = m_rudiOutstanding.find(pktId);
        if (it == m_rudiOutstanding.end()) continue;
        if (it->second.retry_cnt >= m_maxRetries)
        {
            failedMsgs.push_back(it->second.msgId);
            m_rudiOutstanding.erase(it);
            continue;
        }
        it->second.retry_cnt++;
        it->second.rto_deadline_ns = nowNs +
            m_rtoInitUs * 1000.0 * (double)(1u << std::min(it->second.retry_cnt, 10u));
        ++m_retxCnt;
        ++m_txPktCount;
        if (m_wireSendCb) m_wireSendCb(it->second.wirePkt->Copy(), it->second.dstFa);
    }

    for (uint16_t id : failedMsgs)
        CompleteMessage(id, false, RC_RETRY_REQUIRED);

    // Keep scanning while anything is unacknowledged
    if (HasOutstandingTx())
        m_rtoEvent = Simulator::Schedule(MicroSeconds(m_rtoInitUs),
                                         &UetSesPdsEngine::RtoCheck, this);
}

} // namespace ns3
