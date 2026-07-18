/**
 * uet-pdc.cc  —  PDC state machine and PDS Manager implementation
 *
 * References: UE-Specification-1.0.2 §3.5.8, §3.5.9, §3.5.11, §3.5.12
 */

#include "uet-pdc.h"

#include "ns3/log.h"

#include <algorithm>
#include <cassert>
#include <cstdlib>
#include <iomanip>
#include <sstream>

namespace ns3
{

NS_LOG_COMPONENT_DEFINE("UetPdcManager");

// ═════════════════════════════════════════════════════════════════════════════
//  PdcPsnSpace methods
// ═════════════════════════════════════════════════════════════════════════════

uint32_t
PdcPsnSpace::AssignTxPsn()
{
    uint32_t p = next_tx_psn;
    next_tx_psn++;
    TxPkt pkt;
    pkt.psn             = p;
    pkt.retry_cnt       = 0;
    pkt.nack_retry_cnt  = 0;
    pkt.rto_deadline    = 0.0;
    pkt.needs_guaranteed_delivery = false;
    pkt.msg_id          = 0;
    pkt.wirePkt         = nullptr;
    outstanding[p]      = pkt;
    return p;
}

int16_t
PdcPsnSpace::GetClearPsnOffset() const
{
    // clear_psn_offset = CLEAR_PSN − current_psn (signed 16-bit)
    // Per spec §3.5.11.4.4:  CLEAR_PSN is the highest cleared PSN.
    // The pds.clear_psn_offset is added to pds.psn to compute CLEAR_PSN.
    int32_t diff = (int32_t)clear_psn - (int32_t)(next_tx_psn - 1);
    if (diff < INT16_MIN) diff = INT16_MIN;
    if (diff > INT16_MAX) diff = INT16_MAX;
    return (int16_t)diff;
}

bool
PdcPsnSpace::InExpectedWindow(uint32_t rxPsn) const
{
    // Check: rxPsn in [rx_cack_psn + 1, rx_cack_psn + MP_RANGE].
    // The window MUST be anchored at CACK_PSN, not at the highest PSN seen:
    // a retransmit of a dropped packet arrives with a PSN below
    // expected_rx_psn and must still be accepted (§3.5.12.2).
    // Uses unsigned arithmetic to handle wraparound correctly.
    uint32_t offset = rxPsn - (rx_cack_psn + 1);
    return offset < MP_RANGE;
}

void
PdcPsnSpace::OnAckReceived(uint32_t ackPsn, uint32_t newCackPsn,
                            uint64_t sackBitmap, uint32_t sackBase,
                            std::vector<TxPkt>* removed)
{
    // 1. Remove explicitly ACK'd PSN from outstanding
    auto it = outstanding.find(ackPsn);
    if (it != outstanding.end())
    {
        if (removed) removed->push_back(it->second);
        outstanding.erase(it);
    }

    // 2. Remove any PSN ≤ newCackPsn (cumulative ACK)
    for (auto oit = outstanding.begin(); oit != outstanding.end();)
    {
        if ((int32_t)(oit->first - newCackPsn) <= 0)
        {
            if (removed) removed->push_back(oit->second);
            oit = outstanding.erase(oit);
        }
        else
            ++oit;
    }

    // 3. Advance cack_psn if newCackPsn is higher
    if ((int32_t)(newCackPsn - cack_psn) > 0)
        cack_psn = newCackPsn;

    // 4. Process SACK bitmap: remove selectively ACK'd packets
    if (sackBitmap != 0)
    {
        for (int bit = 0; bit < 64; ++bit)
        {
            if (sackBitmap & (1ULL << bit))
            {
                uint32_t sackPsn = sackBase + bit;
                auto sit = outstanding.find(sackPsn);
                if (sit != outstanding.end())
                {
                    if (removed) removed->push_back(sit->second);
                    outstanding.erase(sit);
                }
            }
        }
    }
}

uint32_t
PdcPsnSpace::OnClearReceived(uint32_t newClearPsn)
{
    if ((int32_t)(newClearPsn - clear_psn) <= 0)
        return 0;
    clear_psn = newClearPsn;

    // Free any stored responses with PSN ≤ newClearPsn
    uint32_t freed = 0;
    for (auto it = stored_responses.begin(); it != stored_responses.end();)
    {
        if ((int32_t)(it->first - newClearPsn) <= 0)
        {
            if (!it->second.isDefault)
                --pending_guaranteed_cnt;
            it = stored_responses.erase(it);
            ++freed;
        }
        else
        {
            ++it;
        }
    }
    return freed;
}

bool
PdcPsnSpace::OnPktReceived(uint32_t rxPsn, bool isRod)
{
    bool alreadyReceived = received_psns.count(rxPsn) > 0;
    if (alreadyReceived)
        return false; // duplicate - drop

    received_psns.insert(rxPsn);

    if (isRod)
    {
        // ROD: only advance if this is exactly the expected PSN
        if (rxPsn == expected_rx_psn)
        {
            ++expected_rx_psn;
            // Advance past any already-received consecutive PSNs
            while (received_psns.count(expected_rx_psn))
                ++expected_rx_psn;
        }
    }
    else
    {
        // RUD: advance expected_rx_psn to max received + 1
        if ((int32_t)(rxPsn - expected_rx_psn) >= 0)
            expected_rx_psn = rxPsn + 1;
    }

    // Advance rx_cack_psn: highest consecutive PSN received starting from start_psn
    while (received_psns.count(rx_cack_psn + 1))
        ++rx_cack_psn;

    return true; // new packet
}

uint64_t
PdcPsnSpace::BuildSackBitmap(uint32_t& sackBase) const
{
    sackBase = rx_cack_psn + 1; // SACK_PSN (§3.5.11.4.7)
    uint64_t bitmap = 0;
    for (int bit = 0; bit < 64; ++bit)
    {
        uint32_t p = sackBase + (uint32_t)bit;
        if (received_psns.count(p))
            bitmap |= (1ULL << bit);
    }
    return bitmap;
}

void
PdcPsnSpace::StoreResponse(uint32_t psn, uint8_t rc, uint32_t modLen, uint16_t msgId, bool isDef)
{
    StoredResponse sr;
    sr.return_code     = rc;
    sr.modified_length = modLen;
    sr.message_id      = msgId;
    sr.isDefault       = isDef;
    stored_responses[psn] = sr;
    if (!isDef)
        ++pending_guaranteed_cnt;
}

// ═════════════════════════════════════════════════════════════════════════════
//  UetPdcManager
// ═════════════════════════════════════════════════════════════════════════════
UetPdcManager::UetPdcManager(uint32_t maxPdcs)
    : m_maxPdcs(maxPdcs),
      m_nextIpdcid(1),  // PDCID=0 is reserved (§3.5.8.2)
      m_nextTpdcid(1),
      m_lastUsedPsn(0)
{
}

uint32_t
UetPdcManager::GenStartPsn()
{
    // "Start_PSN MUST be at least 2^16 distance from last used PSN" (§3.5.8.2)
    // Uses an internal xorshift generator instead of ::rand() so that runs are
    // reproducible and independent of global libc RNG state.
    m_psnRngState ^= m_psnRngState << 13;
    m_psnRngState ^= m_psnRngState >> 17;
    m_psnRngState ^= m_psnRngState << 5;
    uint32_t candidate = m_lastUsedPsn + 65536 + (m_psnRngState & 0xFFFF);
    m_lastUsedPsn = candidate;
    return candidate;
}

uint16_t
UetPdcManager::AllocIpdcid()
{
    uint16_t id = m_nextIpdcid;
    // Skip 0 and any already in use
    while (id == 0 || m_byIpdcid.count(id))
    {
        ++id;
        if (id == 0) id = 1;
    }
    m_nextIpdcid = id + 1;
    if (m_nextIpdcid == 0) m_nextIpdcid = 1;
    return id;
}

uint16_t
UetPdcManager::AllocTpdcid()
{
    uint16_t id = m_nextTpdcid;
    while (id == 0)
    {
        ++id;
    }
    m_nextTpdcid = id + 1;
    if (m_nextTpdcid == 0) m_nextTpdcid = 1;
    return id;
}

// ─────────────────────────────────────────────────────────────────────────────
//  AllocInitiatorPdc  (alloc_pdc §3.5.9.2)
// ─────────────────────────────────────────────────────────────────────────────
uint16_t
UetPdcManager::AllocInitiatorPdc(const PdcTuple& tuple,
                                  uint32_t srcFa, uint32_t dstFa,
                                  bool isRod, uint8_t tc)
{
    if (m_byIpdcid.size() >= m_maxPdcs)
    {
        NS_LOG_WARN("PdcManager: out of PDC resources");
        return 0;
    }

    uint16_t ipdcid = AllocIpdcid();
    PdcContext& ctx  = m_byIpdcid[ipdcid];
    ctx.ipdcid  = ipdcid;
    ctx.tpdcid  = 0;           // unknown until target responds
    ctx.state   = PDC_STATE_CREATING;
    ctx.isRod   = isRod;
    ctx.pdsTypeMode = isRod ? PDS_TYPE_ROD_REQ : PDS_TYPE_RUD_REQ;
    ctx.tuple   = tuple;
    ctx.srcFa   = srcFa;
    ctx.dstFa   = dstFa;
    ctx.syn     = true;        // SYN flag stays set until TPDCID received
    uint32_t startPsn = GenStartPsn();
    ctx.fwd.Init(startPsn);
    ctx.rev.Init(startPsn);    // §3.5.8.2: return dir starts at same Start_PSN

    ctx.Transition(PDC_STATE_CREATING, "alloc_initiator_pdc");

    NS_LOG_INFO("PdcManager: allocated IPDCID=" << ipdcid
                << " mode=" << (isRod ? "ROD" : "RUD")
                << " startPsn=0x" << std::hex << startPsn << std::dec);
    return ipdcid;
}

PdcContext*
UetPdcManager::GetByIpdcid(uint16_t ipdcid)
{
    auto it = m_byIpdcid.find(ipdcid);
    return (it != m_byIpdcid.end()) ? &it->second : nullptr;
}

PdcContext*
UetPdcManager::GetByTargetKey(uint32_t srcFa, uint16_t spdcid)
{
    auto key = std::make_pair(srcFa, spdcid);
    auto it  = m_targetIndex.find(key);
    if (it == m_targetIndex.end()) return nullptr;
    return GetByIpdcid(it->second);
}

// ─────────────────────────────────────────────────────────────────────────────
//  EstablishTargetPdc  (OPEN handler in Fig 3-49)
//  Called when the target receives the first SYN packet.
// ─────────────────────────────────────────────────────────────────────────────
uint16_t
UetPdcManager::EstablishTargetPdc(uint32_t srcFa, uint16_t ipdcidAsSpdcid,
                                   bool isRod, uint8_t pdcInfo,
                                   uint32_t startPsn,
                                   uint8_t& nackCode)
{
    if (m_byIpdcid.size() >= m_maxPdcs)
    {
        nackCode = NACK_NO_PDC_AVAIL;
        return 0;
    }

    // Check RUD vs ROD mixing: "RUD and ROD traffic MUST NOT use the same PDC" (§3.5.8.1)
    uint16_t tpdcid = AllocTpdcid();
    uint16_t ipdcid = AllocIpdcid();

    PdcContext& ctx = m_byIpdcid[ipdcid];
    ctx.ipdcid     = ipdcid;
    ctx.tpdcid     = tpdcid;
    ctx.state      = PDC_STATE_ESTABLISHED;
    ctx.isRod      = isRod;
    ctx.pdsTypeMode = isRod ? PDS_TYPE_ROD_REQ : PDS_TYPE_RUD_REQ;
    ctx.srcFa      = srcFa;
    ctx.syn        = false;

    ctx.fwd.Init(startPsn);
    ctx.rev.Init(startPsn); // return direction starts at same PSN (§3.5.8.2)

    // Register in target index: key = {srcFa, ipdcidAsSpdcid}
    m_targetIndex[{srcFa, ipdcidAsSpdcid}] = ipdcid;

    ctx.Transition(PDC_STATE_ESTABLISHED, "establish_target_pdc");

    NS_LOG_INFO("PdcManager: EstablishTargetPdc: TPDCID=" << tpdcid
                << " from srcFa=0x" << std::hex << srcFa
                << " spdcid=" << ipdcidAsSpdcid << std::dec
                << " startPsn=0x" << std::hex << startPsn << std::dec);

    nackCode = 0; // success
    return tpdcid;
}

void
UetPdcManager::FreePdc(uint16_t ipdcid)
{
    auto it = m_byIpdcid.find(ipdcid);
    if (it == m_byIpdcid.end()) return;

    PdcContext& ctx = it->second;
    // Save last PSN for next Start_PSN computation
    m_lastUsedPsn = ctx.fwd.next_tx_psn;

    // Remove from target index
    auto key = std::make_pair(ctx.srcFa, (uint16_t)ctx.ipdcid);
    m_targetIndex.erase(key);

    m_byIpdcid.erase(it);
    NS_LOG_INFO("PdcManager: freed IPDCID=" << ipdcid);
}

void   UetPdcManager::MapMsg(uint16_t msgId, uint16_t ipdcid) { m_msgMap[msgId] = ipdcid; }
uint16_t UetPdcManager::GetMsgPdc(uint16_t msgId) const
{
    auto it = m_msgMap.find(msgId);
    return (it != m_msgMap.end()) ? it->second : 0;
}
void   UetPdcManager::UnmapMsg(uint16_t msgId) { m_msgMap.erase(msgId); }

uint16_t
UetPdcManager::SelectPdcToClose() const
{
    // §3.5.9.2: prefer idle PDCs, otherwise select any
    for (auto& kv : m_byIpdcid)
    {
        if (kv.second.state == PDC_STATE_ESTABLISHED &&
            kv.second.fwd.outstanding.empty())
            return kv.first;
    }
    // Fallback: return first ESTABLISHED
    for (auto& kv : m_byIpdcid)
    {
        if (kv.second.state == PDC_STATE_ESTABLISHED)
            return kv.first;
    }
    return 0;
}

uint16_t
UetPdcManager::FindExistingPdc(const PdcTuple& t) const
{
    // A PDC still in CREATING (SYN in flight, TPDCID unknown) is also
    // reusable: messages queued while the handshake completes MUST share it.
    // Allocating a fresh PDC per Send() would exhaust the PDC pool under
    // any pipelined workload.
    for (auto& kv : m_byIpdcid)
    {
        const PdcContext& ctx = kv.second;
        if (ctx.state != PDC_STATE_ESTABLISHED &&
            ctx.state != PDC_STATE_CREATING) continue;
        const PdcTuple& ct = ctx.tuple;
        if (ct.srcFa == t.srcFa && ct.dstFa == t.dstFa &&
            ct.tc   == t.tc    && ct.mode  == t.mode)
            return kv.first;
    }
    return 0;
}

// ─────────────────────────────────────────────────────────────────────────────
//  BuildTxReqHeader  (initiator TX REQ path, Fig 3-48)
// ─────────────────────────────────────────────────────────────────────────────
bool
UetPdcManager::BuildTxReqHeader(uint16_t ipdcid, uint8_t sesNextHdr,
                                  uint32_t payloadBytes,
                                  bool som, bool eom,
                                  bool needsGuaranteedDelivery,
                                  UetPdsHeader& pds,
                                  uint32_t* outPsn)
{
    PdcContext* ctx = GetByIpdcid(ipdcid);
    if (!ctx) return false;
    if (ctx->fwd.IsWindowFull())
    {
        NS_LOG_WARN("PDC " << ipdcid << ": TX window full, cannot send");
        return false;
    }

    uint32_t txPsn = ctx->fwd.AssignTxPsn();
    if (outPsn) *outPsn = txPsn;

    pds.prologue.pdsType = (UetPdsType)ctx->pdsTypeMode;
    pds.prologue.nextHdr = sesNextHdr;
    pds.prologue.flags   = UetPdsFlags{};
    pds.prologue.flags.syn  = ctx->syn;         // §3.5.11.8.2: set until TPDCID known
    pds.prologue.flags.retx = false;            // not a retransmit
    pds.prologue.flags.ar   = false;            // ACK-request (set by CC layer)

    pds.psn    = txPsn;
    pds.spdcid = ctx->ipdcid;

    if (ctx->syn)
    {
        // Overload dpdcid = {pdc_info(4), psn_offset(12)} (§3.5.8.2)
        uint16_t psnOff = (uint16_t)(txPsn - ctx->fwd.start_psn);
        pds.SetSynDpdcid(0 /*pdc_info: use global pool*/, psnOff);
    }
    else
    {
        pds.dpdcid = ctx->tpdcid;
    }

    // CLEAR_PSN offset
    pds.clear_psn_offset = ctx->fwd.GetClearPsnOffset();

    // Mark if this packet needs guaranteed delivery
    if (needsGuaranteedDelivery)
    {
        auto& pkt = ctx->fwd.outstanding[txPsn];
        pkt.needs_guaranteed_delivery = true;
    }

    NS_LOG_LOGIC("PDC " << ipdcid << " TX psn=" << txPsn
                << " spdcid=" << pds.spdcid << " dpdcid=" << pds.dpdcid
                << " syn=" << ctx->syn);
    return true;
}

// Forward-declared helper: compute PSN as offset (ignores wrap intentionally)
static inline uint32_t pdsOffset(uint32_t a, uint32_t b) { return a - b; }

// ─────────────────────────────────────────────────────────────────────────────
//  ProcessRxRequest  (target RX path, Fig 3-49 OPEN / RX REQ / chk_clear)
// ─────────────────────────────────────────────────────────────────────────────
uint8_t
UetPdcManager::ProcessRxRequest(UetPdsHeader& pds,
                                 uint32_t srcFaIP,
                                 bool isTrimmed,
                                 uint32_t& outClearPsn,
                                 bool& outIsNewPsn,
                                 bool& outIsDuplicate)
{
    outIsNewPsn = false;
    outIsDuplicate = false;
    outClearPsn = 0;

    // §3.5.8.2 step 2: trimmed packets must be NACK'd
    if (isTrimmed)
    {
        return NACK_TRIMMED; // Caller generates NACK
    }

    bool isSyn = pds.prologue.flags.syn;

    // §3.5.8.2 step 3: SYN packets
    if (isSyn)
    {
        // Check if PDC already exists for {srcFa, spdcid}
        PdcContext* existing = GetByTargetKey(srcFaIP, pds.spdcid);
        if (existing)
        {
            // PDC already established — treat as SYN=0 below
        }
        else
        {
            // New PDC establishment
            uint8_t nackCode = 0;
            uint8_t pdcInfo; uint16_t psnOffset;
            pds.GetSynDpdcid(pdcInfo, psnOffset);
            uint32_t startPsn = pds.psn - psnOffset;

            bool isRod = (pds.prologue.pdsType == PDS_TYPE_ROD_REQ ||
                          pds.prologue.pdsType == PDS_TYPE_ROD_CC_REQ);

            // ROD check: first packet must have PSN == Start_PSN (§3.5.8.2.e)
            if (isRod && pdsOffset(pds.psn, startPsn) != 0)
            {
                return NACK_ROD_OOO;
            }

            uint16_t tpdcid = EstablishTargetPdc(srcFaIP, pds.spdcid, isRod,
                                                   pdcInfo, startPsn, nackCode);
            if (nackCode != 0)
                return nackCode;

            // Return TPDCID to initiator: in ACK, pds.spdcid = TPDCID
            pds.dpdcid = tpdcid; // caller uses this when building ACK
            NS_LOG_INFO("Target: new PDC TPDCID=" << tpdcid << " for srcFa=0x"
                        << std::hex << srcFaIP << std::dec
                        << " spdcid=" << pds.spdcid);
        }
    }

    // Lookup PDC for this packet (either by SYN lookup or existing)
    PdcContext* ctx = GetByTargetKey(srcFaIP, pds.spdcid);
    if (!ctx)
    {
        // pds.flags.syn=0 but PDC not found → UET_INV_DPDCID (§3.5.8.2 step 4a)
        return NACK_INV_DPDCID;
    }

    // §3.5.8.2 step 4b: verify source IP
    if (ctx->srcFa != srcFaIP)
        return NACK_PDC_HDR_MISMATCH;

    bool isRod = ctx->isRod;

    // Duplicate check MUST come before the window / ordering checks:
    // a retransmit of an already-received PSN (its ACK was lost) sits at or
    // below CACK_PSN and must be re-ACKed, never NACKed (§3.5.12.3).
    if ((int32_t)(pds.psn - ctx->fwd.rx_cack_psn) <= 0 ||
        ctx->fwd.received_psns.count(pds.psn))
    {
        outIsDuplicate = true;
        uint32_t dupClearPsn = pds.ComputeClearPsn();
        ctx->fwd.OnClearReceived(dupClearPsn);
        outClearPsn = dupClearPsn;
        return 0;
    }

    // §3.5.12.2: check PSN within expected window
    if (!ctx->fwd.InExpectedWindow(pds.psn))
        return NACK_PSN_OOR_WINDOW;

    // ROD: out-of-order packets inside the window are ACCEPTED and tracked
    // (SACK advertises them); the SES delivery layer buffers them until the
    // gap fills, so only the genuinely lost PSN needs retransmission.
    // NACK_ROD_OOO + drop (go-back-N) is spec-permitted but collapses under
    // loss at line rate: every packet behind a gap triggers a retransmit
    // storm. Reserved here for SYN-time violations only (§3.5.8.2.e).

    // §3.5.8.2 step 4e: duplicate packet
    bool isNew = ctx->fwd.OnPktReceived(pds.psn, isRod);
    if (!isNew)
    {
        // Duplicate: drop unless pds.flags.retx set
        if (!pds.prologue.flags.retx)
        {
            outIsDuplicate = true;
            return 0; // drop silently (no NACK)
        }
    }

    outIsNewPsn = isNew;

    // Process CLEAR_PSN from incoming request (target receives clear info)
    uint32_t incomingClearPsn = pds.ComputeClearPsn();
    uint32_t freed = ctx->fwd.OnClearReceived(incomingClearPsn);
    outClearPsn = incomingClearPsn;

    NS_LOG_LOGIC("Target: RX psn=" << pds.psn << " PDC=" << ctx->ipdcid
                << " isNew=" << isNew << " clearPsn=" << incomingClearPsn
                << " freed=" << freed);

    return 0; // success
}

// (pdsOffset is defined above ProcessRxRequest)

// ─────────────────────────────────────────────────────────────────────────────
//  ProcessRxAck  (initiator RX ACK path, Fig 3-48)
// ─────────────────────────────────────────────────────────────────────────────
bool
UetPdcManager::ProcessRxAck(uint16_t ipdcid, const UetPdsHeader& ack)
{
    PdcContext* ctx = GetByIpdcid(ipdcid);
    if (!ctx) return false;

    // On first ACK: learn TPDCID and clear SYN flag (§3.5.8.2)
    if (ctx->syn && ack.spdcid != 0)
    {
        ctx->tpdcid = ack.spdcid;
        ctx->syn    = false;
        ctx->Transition(PDC_STATE_ESTABLISHED,
                        "received_first_ack_tpdcid=" + std::to_string(ack.spdcid));
        NS_LOG_INFO("Initiator: PDC " << ipdcid << " ESTABLISHED, TPDCID=" << ctx->tpdcid);
    }

    uint32_t ackPsn  = ack.ComputeAckPsn();
    uint32_t cackPsn = ack.cack_psn;

    uint64_t sackBitmap = 0;
    uint32_t sackBase   = 0;
    if (ack.prologue.pdsType == PDS_TYPE_ACK_CC ||
        ack.prologue.pdsType == PDS_TYPE_ACK_CCX)
    {
        sackBitmap = ack.sack_bitmap;
        sackBase   = ack.ComputeSackPsn();
    }

    ctx->fwd.OnAckReceived(ackPsn, cackPsn, sackBitmap, sackBase);

    // §3.5.11.4.4: if ACK has req flag, send CLEAR_COMMAND
    if (ack.prologue.flags.req)
    {
        NS_LOG_LOGIC("PDC " << ipdcid << ": peer requests CLEAR");
        // Caller should call BuildCP(ipdcid, CP_CLEAR_COMMAND, ...)
    }

    NS_LOG_LOGIC("Initiator: RX ACK psn=" << ackPsn << " cack=" << cackPsn
                << " PDC=" << ipdcid
                << " outstanding=" << ctx->fwd.outstanding.size());
    return true;
}

// ─────────────────────────────────────────────────────────────────────────────
//  BuildAckHeader  (gen_ack, §3.5.9)
// ─────────────────────────────────────────────────────────────────────────────
bool
UetPdcManager::BuildAckHeader(uint32_t srcFa, uint16_t spdcid,
                                uint32_t ackPsn, bool needsSack,
                                UetPdsHeader& ack)
{
    PdcContext* ctx = GetByTargetKey(srcFa, spdcid);
    if (!ctx) return false;

    bool needsGtd = (ctx->fwd.pending_guaranteed_cnt > 0);

    ack.prologue.pdsType = needsSack ? PDS_TYPE_ACK_CC : PDS_TYPE_ACK;
    ack.prologue.nextHdr = UET_HDR_NONE;
    ack.prologue.flags   = UetPdsFlags{};
    ack.prologue.flags.req = needsGtd; // request clear when guaranteed delivery pending

    ack.cack_psn = ctx->fwd.rx_cack_psn;
    // ack_psn_offset = ackPsn – cack_psn (signed)
    ack.ack_psn_offset = (int16_t)(ackPsn - ack.cack_psn);
    ack.probe_opaque   = 0;
    ack.spdcid = ctx->tpdcid;
    ack.dpdcid = spdcid; // echo back IPDCID as DPDCID

    if (needsSack)
    {
        uint32_t sackBase;
        ack.sack_bitmap     = ctx->fwd.BuildSackBitmap(sackBase);
        ack.sack_psn_offset = (int16_t)(sackBase - ack.cack_psn);
        ack.mpr             = (uint8_t)std::min<uint32_t>(ctx->fwd.MP_RANGE, 255);
    }
    return true;
}

// ─────────────────────────────────────────────────────────────────────────────
//  BuildNackHeader
// ─────────────────────────────────────────────────────────────────────────────
bool
UetPdcManager::BuildNackHeader(uint32_t /*srcFa*/, uint16_t spdcid, uint16_t dpdcid,
                                 uint32_t nackPsn, uint8_t nackCode,
                                 UetPdsHeader& nack)
{
    nack.prologue.pdsType = PDS_TYPE_NACK;
    nack.prologue.nextHdr = UET_HDR_NONE;
    nack.prologue.flags   = UetPdsFlags{};
    nack.nack_code    = nackCode;
    nack.vendor_code  = 0;
    nack.nack_psn     = nackPsn;
    nack.spdcid       = spdcid;
    nack.dpdcid       = dpdcid;
    nack.nack_payload = 0;

    // If PDC not established, spdcid=0 per spec (§3.5.12.7)
    bool pdcNotCreated = (nackCode == NACK_NO_PDC_AVAIL ||
                          nackCode == NACK_NO_CCC_AVAIL ||
                          nackCode == NACK_NO_BITMAP    ||
                          nackCode == NACK_INV_DPDCID   ||
                          nackCode == NACK_PDC_HDR_MISMATCH);
    if (pdcNotCreated)
        nack.spdcid = 0;
    return true;
}

// ─────────────────────────────────────────────────────────────────────────────
//  BuildCP  (§3.5.16: generate control packet)
// ─────────────────────────────────────────────────────────────────────────────
bool
UetPdcManager::BuildCP(uint16_t ipdcid, UetPdsCtlType ctlType,
                        uint32_t payload, UetPdsHeader& cp)
{
    PdcContext* ctx = GetByIpdcid(ipdcid);
    if (!ctx) return false;

    cp.prologue.pdsType = PDS_TYPE_CP;
    cp.prologue.nextHdr = (uint8_t)ctlType; // ctl_type stored in nextHdr field for CP
    cp.prologue.flags   = UetPdsFlags{};
    cp.prologue.flags.isrod = ctx->isRod;
    cp.prologue.flags.syn   = ctx->syn;

    // Some CPs consume a new PSN (e.g., NOOP, PROBE); others use PSN=0 (CLEAR_CMD)
    bool usesPsn = (ctlType == CP_NOOP || ctlType == CP_PROBE ||
                    ctlType == CP_ACK_REQUEST || ctlType == CP_NEGOTIATION);
    if (usesPsn)
        cp.psn = ctx->fwd.AssignTxPsn();
    else
        cp.psn = 0;

    cp.spdcid      = ctx->ipdcid;
    cp.dpdcid      = ctx->tpdcid;
    cp.cp_payload  = payload;
    cp.probe_opaque = 0;
    return true;
}

// ─────────────────────────────────────────────────────────────────────────────
//  ProcessRxCP  (RX CONTROL handler)
// ─────────────────────────────────────────────────────────────────────────────
void
UetPdcManager::ProcessRxCP(uint16_t ipdcid, const UetPdsHeader& cp)
{
    PdcContext* ctx = GetByIpdcid(ipdcid);
    if (!ctx) return;

    UetPdsCtlType ctlType = (UetPdsCtlType)(cp.prologue.nextHdr & 0x0F);
    NS_LOG_INFO("PDC " << ipdcid << ": RX CP ctl_type=" << (int)ctlType);

    switch (ctlType)
    {
    case CP_CLEAR_COMMAND:
        ctx->fwd.OnClearReceived(cp.cp_payload);
        break;
    case CP_CLEAR_REQUEST:
        NS_LOG_INFO("  PDC " << ipdcid << ": target requests CLEAR");
        break;
    case CP_CLOSE_REQUEST:
        ctx->Transition(PDC_STATE_QUIESCE, "rx_close_request");
        break;
    case CP_CLOSE_COMMAND:
        ctx->Transition(PDC_STATE_CLOSE, "rx_close_command");
        FreePdc(ipdcid);
        break;
    case CP_ACK_REQUEST:
        NS_LOG_LOGIC("  PDC " << ipdcid << ": ACK_REQUEST for psn=" << cp.psn);
        break;
    case CP_NOOP:
        NS_LOG_LOGIC("  PDC " << ipdcid << ": NOOP CP received");
        break;
    default:
        break;
    }
}

// ─────────────────────────────────────────────────────────────────────────────
//  PDC close protocol (§3.5.8.3)
// ─────────────────────────────────────────────────────────────────────────────
void
UetPdcManager::InitiateClose(uint16_t ipdcid)
{
    PdcContext* ctx = GetByIpdcid(ipdcid);
    if (!ctx) return;
    ctx->closing = true;
    ctx->Transition(PDC_STATE_QUIESCE, "initiate_close_begin_drain");
    NS_LOG_INFO("PDC " << ipdcid << ": close initiated");
}

void
UetPdcManager::HandleCloseCommand(uint16_t ipdcid)
{
    PdcContext* ctx = GetByIpdcid(ipdcid);
    if (!ctx) return;
    ctx->Transition(PDC_STATE_CLOSE, "handle_close_command");
    FreePdc(ipdcid);
}

// ─────────────────────────────────────────────────────────────────────────────
//  DescribePdc / DescribeAll (for demo output)
// ─────────────────────────────────────────────────────────────────────────────
std::string
UetPdcManager::DescribePdc(uint16_t ipdcid) const
{
    auto it = m_byIpdcid.find(ipdcid);
    if (it == m_byIpdcid.end())
        return "  [PDC " + std::to_string(ipdcid) + "] NOT FOUND";

    const PdcContext& ctx = it->second;
    std::ostringstream os;
    os << "  PDC IPDCID=" << ctx.ipdcid
       << " TPDCID=" << ctx.tpdcid
       << " state=" << PdcStateName(ctx.state)
       << " mode=" << (ctx.isRod ? "ROD" : "RUD")
       << " syn=" << ctx.syn
       << "\n    FWD: nextPsn=" << ctx.fwd.next_tx_psn
       << " cackPsn=" << ctx.fwd.cack_psn
       << " clearPsn=" << ctx.fwd.clear_psn
       << " outstanding=" << ctx.fwd.outstanding.size()
       << " rxCackPsn=" << ctx.fwd.rx_cack_psn
       << " received=" << ctx.fwd.received_psns.size()
       << "\n    REV: nextPsn=" << ctx.rev.next_tx_psn
       << " cackPsn=" << ctx.rev.cack_psn
       << "\n    Transitions:";
    for (auto& t : ctx.transitions)
        os << "\n      " << t;
    return os.str();
}

std::string
UetPdcManager::DescribeAll() const
{
    std::ostringstream os;
    os << "=== PDC Manager State === (" << m_byIpdcid.size() << " active PDCs)\n";
    for (auto& kv : m_byIpdcid)
        os << DescribePdc(kv.first) << "\n";
    return os.str();
}

} // namespace ns3
