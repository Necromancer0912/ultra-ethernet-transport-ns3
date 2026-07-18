#ifndef RDMA_SEQ_TS_HEADER_H
#define RDMA_SEQ_TS_HEADER_H

#include "ns3/header.h"
#include "ns3/int-header.h"
#include "ns3/nstime.h"

namespace ns3
{

    class RdmaSeqTsHeader : public Header
    {
    public:
        RdmaSeqTsHeader();

        void SetSeq(uint64_t seq);
        uint64_t GetSeq() const;

        void SetPG(uint16_t pg);
        uint16_t GetPG() const;

        Time GetTs() const;

        static TypeId GetTypeId();
        TypeId GetInstanceTypeId() const override;
        void Print(std::ostream &os) const override;
        uint32_t GetSerializedSize() const override;
        static uint32_t GetHeaderSize();

    private:
        void Serialize(Buffer::Iterator start) const override;
        uint32_t Deserialize(Buffer::Iterator start) override;

        uint64_t m_seq;
        uint16_t m_pg;

    public:
        IntHeader ih;
    };

} // namespace ns3

#endif // RDMA_SEQ_TS_HEADER_H
