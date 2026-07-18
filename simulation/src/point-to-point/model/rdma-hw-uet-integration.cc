#include "rdma-hw-uet-integration.h"
#include <ns3/random-variable-stream.h>
#include <ns3/simulator.h>
#include <sstream>
#include <iomanip>
#include <algorithm>

namespace ns3
{

    namespace
    {
        inline uint32_t
        NarrowPdcId(uint64_t pdcId)
        {
            return static_cast<uint32_t>(pdcId & 0xffffffffu);
        }
    } // namespace

    NS_LOG_COMPONENT_DEFINE("RdmaHwUetIntegration");
    NS_OBJECT_ENSURE_REGISTERED(RdmaHwUetIntegration);

    TypeId
    RdmaHwUetIntegration::GetTypeId(void)
    {
        static TypeId tid =
            TypeId("ns3::RdmaHwUetIntegration")
                .SetParent<Object>()
                .SetGroupName("PointToPoint")
                .AddConstructor<RdmaHwUetIntegration>();
        return tid;
    }

    RdmaHwUetIntegration::RdmaHwUetIntegration()
        : m_nodeCount(0),
          m_linkBandwidth(0),
          m_configuredMtuBytes(0),
          m_initialized(false)
    {
        NS_LOG_LOGIC("[RDMA-UET] Integration layer constructed");
    }

    RdmaHwUetIntegration::~RdmaHwUetIntegration()
    {
    }

    void
    RdmaHwUetIntegration::Initialize(uint32_t numNodes, uint64_t linkBandwidth, uint32_t mtuBytes)
    {
        m_nodeCount = numNodes;
        m_linkBandwidth = linkBandwidth;
        m_configuredMtuBytes = mtuBytes;
        m_initialized = true;

        NS_LOG_LOGIC("[RDMA-UET] Initialized with nodes=" << numNodes << " bandwidth="
                                                          << linkBandwidth << " mtu=" << mtuBytes);
    }

    uint64_t
    RdmaHwUetIntegration::ComputePdcId(uint32_t srcId,
                                       uint32_t dstId,
                                       uint16_t sport,
                                       uint32_t jobId)
    {
        // PDC ID = jobId * 1000000 + srcId * 10000 + dstId * 100 + (sport % 100)
        // Ensures unique PDCs per (job, src, dst, sport) 4-tuple
        return (uint64_t)jobId * 1000000 + (uint64_t)srcId * 10000 + (uint64_t)dstId * 100 +
               (sport % 100);
    }

    uint32_t
    RdmaHwUetIntegration::AllocateResourceIndex(uint64_t pdcId)
    {
        auto it = m_resourceCounter.find(pdcId);
        if (it == m_resourceCounter.end())
        {
            m_resourceCounter[pdcId] = 1;
            return 1;
        }
        else
        {
            it->second++;
            return it->second;
        }
    }

    Ptr<Packet>
    RdmaHwUetIntegration::ProcessTxPacket(
        Ptr<Packet> p,
        uint32_t srcId,
        uint32_t dstId,
        uint16_t sport,
        uint32_t jobId,
        uint8_t mode,
        double dropRate)
    {
        if (!m_initialized)
        {
            NS_LOG_WARN("[RDMA-UET] ProcessTxPacket called before Initialize()");
            return p; // Pass through if not initialized
        }

        // Compute PDC ID
        uint64_t pdcId = ComputePdcId(srcId, dstId, sport, jobId);

        // Check for simulated drop
        Ptr<UniformRandomVariable> rng = CreateObject<UniformRandomVariable>();
        if (rng->GetValue() < dropRate)
        {
            NS_LOG_LOGIC("[RDMA-UET] TX packet dropped (PDC=" << std::hex << pdcId << std::dec
                                                              << ", drop_rate=" << dropRate << ")");
            return nullptr; // Indicate drop
        }

        // Track PSN for this PDC (ROD mode needs ordering)
        auto seqIt = m_pdcSequence.find(pdcId);
        uint32_t psn;
        if (seqIt == m_pdcSequence.end())
        {
            m_pdcSequence[pdcId] = 1;
            psn = 1;
        }
        else
        {
            psn = ++seqIt->second;
        }

        NS_LOG_LOGIC("[RDMA-UET] TX psn=" << psn << " mode=" << (uint32_t)mode << " pdcId="
                                          << std::hex << pdcId << std::dec);

        UetSesHeader ses;
        ses.opcode = UET_SEND;
        ses.SetFormat(UET_HDR_REQUEST_STD);
        ses.jobId = jobId;
        ses.initiator = srcId;
        // ses routing doesn't natively include "target", we rely on dpdcid in PDS
        ses.message_id = (static_cast<uint16_t>(pdcId & 0xFFFF));
        ses.resource_index = 0;
        ses.request_length = p->GetSize();
        ses.payload_length = p->GetSize();
        ses.message_offset = 0;

        UetPdsHeader pds;
        pds.prologue.pdsType = (mode == UET_MODE_ROD) ? PDS_TYPE_ROD_REQ : PDS_TYPE_RUD_REQ;
        pds.prologue.nextHdr = ses.GetFormat();
        pds.spdcid = NarrowPdcId(pdcId);
        pds.dpdcid = NarrowPdcId(pdcId);
        pds.psn = psn;
        pds.cack_psn = psn;

        p->AddHeader(ses);
        p->AddHeader(pds);

        return p;
    }

    bool
    RdmaHwUetIntegration::ProcessRxPacket(
        Ptr<Packet> p,
        uint32_t srcId,
        uint32_t dstId,
        uint16_t sport,
        uint32_t jobId,
        uint8_t mode,
        double dropRate)
    {
        if (!m_initialized)
        {
            NS_LOG_WARN("[RDMA-UET] ProcessRxPacket called before Initialize()");
            return true; // Generate response if not initialized
        }

        // Compute PDC ID
        uint64_t pdcId = ComputePdcId(srcId, dstId, sport, jobId);
        const uint32_t pdcKey = NarrowPdcId(pdcId);

        // Check for simulated ACK drop
        Ptr<UniformRandomVariable> rng = CreateObject<UniformRandomVariable>();
        if (rng->GetValue() < dropRate)
        {
            NS_LOG_LOGIC("[RDMA-UET] RX ACK dropped (PDC=" << std::hex << pdcId << std::dec
                                                           << ", drop_rate=" << dropRate << ")");
            return false; // Don't generate response (simulated ACK loss)
        }

        UetPdsHeader pdsHeader;
        const bool hasPds = (p != nullptr) && (p->PeekHeader(pdsHeader) > 0);
        const UetDeliveryMode deliveryMode =
            hasPds ? ((pdsHeader.prologue.pdsType == PDS_TYPE_ROD_REQ || pdsHeader.prologue.pdsType == PDS_TYPE_ROD_CC_REQ) ? UET_MODE_ROD : UET_MODE_RUD) : static_cast<UetDeliveryMode>(mode);

        // Get or create target PDC state.
        auto pdc = m_rxState.GetOrCreatePdc(pdcKey, deliveryMode);

        uint32_t psn = 1;
        if (hasPds)
        {
            psn = pdsHeader.psn;
            m_lastPsn[pdcId] = std::max(m_lastPsn[pdcId], psn);
        }
        else
        {
            auto psn_it = m_lastPsn.find(pdcId);
            psn = (psn_it == m_lastPsn.end()) ? 1 : ++psn_it->second;
            if (psn_it == m_lastPsn.end())
            {
                m_lastPsn[pdcId] = psn;
            }
        }

        if (pdc->rodMode && psn != pdc->expectedPsn)
        {
            UetPdsHeader ackReq;
            ackReq.prologue.pdsType = PDS_TYPE_CP;
            ackReq.prologue.nextHdr = CP_ACK_REQUEST;
            ackReq.spdcid = pdcKey;
            ackReq.dpdcid = pdcKey;
            ackReq.psn = pdc->expectedPsn;
            ackReq.cack_psn = psn;
            ackReq.prologue.flags.ar = true;
            m_pendingControlPackets[pdcId] = ackReq;
            return false;
        }

        m_rxState.TrackReceivedPsn(pdcKey, psn);
        if (pdc->rodMode)
        {
            ++pdc->expectedPsn;
        }

        NS_LOG_LOGIC("[RDMA-UET] RX psn=" << psn << " mode=" << (uint32_t)mode << " pdcId="
                                          << std::hex << pdcId << std::dec);

        return true; // Generate ACK
    }

    uint8_t
    RdmaHwUetIntegration::GenerateDefaultResponse(
        uint32_t srcId,
        uint32_t dstId,
        uint16_t sport,
        uint32_t jobId,
        uint8_t mode)
    {
        uint64_t pdcId = ComputePdcId(srcId, dstId, sport, jobId);
        const uint32_t pdcKey = NarrowPdcId(pdcId);

        // Allocate resource index
        uint32_t ri = AllocateResourceIndex(pdcId);

        // Store default response (status RC_OK, 0 bytes)
        m_rxState.StoreDefaultResponse(pdcKey,
                                       ri,
                                       RC_OK,
                                       0);

        auto pdc = m_rxState.GetPdc(pdcKey);
        if (pdc && m_rxState.IsOutstandingResponseLimited(pdcKey))
        {
            UetPdsHeader clear;
            clear.prologue.pdsType = PDS_TYPE_CP;
            clear.prologue.nextHdr = CP_CLEAR_COMMAND;
            clear.spdcid = pdcKey;
            clear.dpdcid = pdcKey;
            clear.psn = pdc->lastReceivedPsn;
            clear.clear_psn_offset = 0;
            clear.cack_psn = pdc->lastReceivedPsn;
            clear.prologue.flags.req = true;
            m_pendingControlPackets[pdcId] = clear;
        }

        return 0; // UET_NO_ERROR
    }

    UetPdsHeader *
    RdmaHwUetIntegration::GetPendingControlPacket(uint64_t pdcId)
    {
        auto it = m_pendingControlPackets.find(pdcId);
        if (it != m_pendingControlPackets.end())
        {
            return &it->second;
        }

        const uint32_t pdcKey = NarrowPdcId(pdcId);
        auto pdc = m_rxState.GetPdc(pdcKey);
        if (!pdc || !m_rxState.IsOutstandingResponseLimited(pdcKey))
        {
            return nullptr;
        }

        UetPdsHeader clear;
        clear.prologue.pdsType = PDS_TYPE_CP;
        clear.prologue.nextHdr = CP_CLEAR_COMMAND;
        clear.spdcid = pdcKey;
        clear.dpdcid = pdcKey;
        clear.psn = pdc->lastReceivedPsn;
        clear.clear_psn_offset = 0;
        clear.cack_psn = pdc->lastReceivedPsn;
        clear.prologue.flags.req = true;
        auto insertResult = m_pendingControlPackets.insert({pdcId, clear});
        return &insertResult.first->second;
    }

    std::string
    RdmaHwUetIntegration::DescribeState() const
    {
        std::ostringstream oss;
        oss << "[RDMA-UET-STATE] Init=" << (m_initialized ? "yes" : "no")
            << " nodes=" << m_nodeCount << " pdcs=" << m_pdcSequence.size();
        return oss.str();
    }

} // namespace ns3
