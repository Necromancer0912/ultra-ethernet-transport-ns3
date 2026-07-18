#ifndef UET_PDS_CONTROL_PACKET_H
#define UET_PDS_CONTROL_PACKET_H

#include <cstdint>
#include <vector>

namespace ns3
{

    class UetPdsControlPacket
    {
    public:
        enum ControlType : uint8_t
        {
            NOOP = 0,
            ACK_REQUEST = 1,
            CLEAR_COMMAND = 2,
            CLEAR_REQUEST = 3,
            PDC_CLOSE_COMMAND = 4,
            PDC_CLOSE_REQUEST = 5,
            PROBE = 6,
            CREDIT = 7
        };

        enum ClearReason : uint8_t
        {
            CLEAR_NONE = 0,
            CLEAR_GUARANTEED_DELIVERY = 1,
            CLEAR_RESOURCE_LIMIT = 2,
            CLEAR_PDC_CLOSE = 3
        };

        struct ControlHeader
        {
            uint32_t spdcId{0};
            uint32_t dpdcId{0};
            uint32_t psn{0};
            ControlType controlType{NOOP};
            uint32_t payload{0};
            bool requiresAck{false};
            bool isRetransmit{false};
        };

        UetPdsControlPacket();
        ~UetPdsControlPacket();

        void SetControlType(ControlType type);
        void SetPsn(uint32_t psn);
        void SetPayload(uint32_t payload);
        void SetSpdcId(uint32_t id);
        void SetDpdcId(uint32_t id);
        void SetRequiresAck(bool flag);
        void SetClearPsn(uint32_t clearPsn, ClearReason reason);

        ControlType GetControlType() const;
        uint32_t GetPsn() const;
        uint32_t GetPayload() const;
        uint32_t GetSpdcId() const;
        uint32_t GetDpdcId() const;
        bool RequiresAck() const;
        const ControlHeader &GetHeader() const;
        std::string Describe() const;

    private:
        ControlHeader m_header;
    };

} // namespace ns3

#endif // UET_PDS_CONTROL_PACKET_H
