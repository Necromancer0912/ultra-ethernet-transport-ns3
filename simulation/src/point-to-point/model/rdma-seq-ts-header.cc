#include "rdma-seq-ts-header.h"

#include "ns3/assert.h"
#include "ns3/log.h"
#include "ns3/simulator.h"

NS_LOG_COMPONENT_DEFINE("RdmaSeqTsHeader");

namespace ns3
{

    NS_OBJECT_ENSURE_REGISTERED(RdmaSeqTsHeader);

    RdmaSeqTsHeader::RdmaSeqTsHeader()
        : m_seq(0),
          m_pg(0)
    {
        if (IntHeader::mode == 1)
        {
            ih.ts = Simulator::Now().GetTimeStep();
        }
    }

    void
    RdmaSeqTsHeader::SetSeq(uint64_t seq)
    {
        m_seq = seq;
    }

    uint64_t
    RdmaSeqTsHeader::GetSeq() const
    {
        return m_seq;
    }

    void
    RdmaSeqTsHeader::SetPG(uint16_t pg)
    {
        m_pg = pg;
    }

    uint16_t
    RdmaSeqTsHeader::GetPG() const
    {
        return m_pg;
    }

    Time
    RdmaSeqTsHeader::GetTs() const
    {
        NS_ASSERT_MSG(IntHeader::mode == 1, "RdmaSeqTsHeader cannot GetTs when IntHeader::mode != 1");
        return TimeStep(ih.ts);
    }

    TypeId
    RdmaSeqTsHeader::GetTypeId()
    {
        static TypeId tid = TypeId("ns3::RdmaSeqTsHeader").SetParent<Header>().AddConstructor<RdmaSeqTsHeader>();
        return tid;
    }

    TypeId
    RdmaSeqTsHeader::GetInstanceTypeId() const
    {
        return GetTypeId();
    }

    void
    RdmaSeqTsHeader::Print(std::ostream &os) const
    {
        os << m_seq << " " << m_pg;
    }

    uint32_t
    RdmaSeqTsHeader::GetSerializedSize() const
    {
        return GetHeaderSize();
    }

    uint32_t
    RdmaSeqTsHeader::GetHeaderSize()
    {
        RdmaSeqTsHeader tmp;
        return sizeof(tmp.m_seq) + sizeof(tmp.m_pg) + IntHeader::GetStaticSize();
    }

    void
    RdmaSeqTsHeader::Serialize(Buffer::Iterator start) const
    {
        Buffer::Iterator i = start;
        i.WriteHtonU64(m_seq);
        i.WriteHtonU16(m_pg);
        ih.Serialize(i);
    }

    uint32_t
    RdmaSeqTsHeader::Deserialize(Buffer::Iterator start)
    {
        Buffer::Iterator i = start;
        m_seq = i.ReadNtohU64();
        m_pg = i.ReadNtohU16();
        ih.Deserialize(i);
        return GetSerializedSize();
    }

} // namespace ns3
