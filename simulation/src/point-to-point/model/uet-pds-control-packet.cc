#include "uet-pds-control-packet.h"

#include <sstream>

namespace ns3
{

    UetPdsControlPacket::UetPdsControlPacket() = default;

    UetPdsControlPacket::~UetPdsControlPacket() = default;

    void
    UetPdsControlPacket::SetControlType(ControlType type)
    {
        m_header.controlType = type;
    }

    void
    UetPdsControlPacket::SetPsn(uint32_t psn)
    {
        m_header.psn = psn;
    }

    void
    UetPdsControlPacket::SetPayload(uint32_t payload)
    {
        m_header.payload = payload;
    }

    void
    UetPdsControlPacket::SetSpdcId(uint32_t id)
    {
        m_header.spdcId = id;
    }

    void
    UetPdsControlPacket::SetDpdcId(uint32_t id)
    {
        m_header.dpdcId = id;
    }

    void
    UetPdsControlPacket::SetRequiresAck(bool flag)
    {
        m_header.requiresAck = flag;
    }

    void
    UetPdsControlPacket::SetClearPsn(uint32_t clearPsn, ClearReason reason)
    {
        m_header.payload = clearPsn;
    }

    UetPdsControlPacket::ControlType
    UetPdsControlPacket::GetControlType() const
    {
        return m_header.controlType;
    }

    uint32_t
    UetPdsControlPacket::GetPsn() const
    {
        return m_header.psn;
    }

    uint32_t
    UetPdsControlPacket::GetPayload() const
    {
        return m_header.payload;
    }

    uint32_t
    UetPdsControlPacket::GetSpdcId() const
    {
        return m_header.spdcId;
    }

    uint32_t
    UetPdsControlPacket::GetDpdcId() const
    {
        return m_header.dpdcId;
    }

    bool
    UetPdsControlPacket::RequiresAck() const
    {
        return m_header.requiresAck;
    }

    const UetPdsControlPacket::ControlHeader &
    UetPdsControlPacket::GetHeader() const
    {
        return m_header;
    }

    std::string
    UetPdsControlPacket::Describe() const
    {
        std::ostringstream os;
        os << "[CP] type=" << static_cast<uint32_t>(m_header.controlType)
           << " psn=" << m_header.psn << " payload=" << m_header.payload
           << " requiresAck=" << (m_header.requiresAck ? "yes" : "no");
        return os.str();
    }

} // namespace ns3
