#ifndef UET_PDS_TARGET_STATE_H
#define UET_PDS_TARGET_STATE_H

#include "uet-pds-header.h"
#include "uet-ses-header.h"

#include <cstdint>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

namespace ns3
{

    class UetPdsTargetState
    {
    public:
        enum TargetStateEnum : uint8_t
        {
            TARGET_IDLE = 0,
            TARGET_RECEIVING = 1,
            TARGET_GENERATING_RESPONSE = 2,
            TARGET_CLEARING = 3,
            TARGET_CLOSED = 4
        };

        struct ResourceEntry
        {
            uint32_t resourceIndex{0};
            uint16_t riGeneration{0};
            uint32_t jobId{0};
            uint32_t initiator{0};
            uint32_t maxResponseSize{0};
            bool responseStored{false};
            UetSesReturnCode storedResponseCode{RC_INTERNAL_ERROR};
            uint32_t storedModifiedLength{0};
        };

        struct PdcTargetState
        {
            uint32_t pdcId{0};
            TargetStateEnum state{TARGET_IDLE};
            uint32_t clearPsn{0};
            uint32_t expectedPsn{1};
            uint32_t lastReceivedPsn{0};
            UetDeliveryMode mode{UET_MODE_RUD};
            bool rodMode{false};
            uint32_t outstandingGuaranteedDeliveryResponses{0};
            uint32_t maxOutstandingResponses{128};
            std::set<uint32_t> receivedPsns;
            std::map<uint32_t, ResourceEntry> resources;
            std::vector<std::string> stateTransitions;
        };

        UetPdsTargetState();
        ~UetPdsTargetState();

        std::shared_ptr<PdcTargetState> GetOrCreatePdc(uint32_t pdcId,
                                                       UetDeliveryMode mode);
        void TrackReceivedPsn(uint32_t pdcId, uint32_t psn);
        void StoreDefaultResponse(uint32_t pdcId,
                                  uint32_t resourceIndex,
                                  UetSesReturnCode responseCode,
                                  uint32_t modifiedLength);
        bool HasStoredResponse(uint32_t pdcId, uint32_t resourceIndex) const;
        UetSesReturnCode GetStoredResponseCode(uint32_t pdcId,
                                                         uint32_t resourceIndex) const;
        uint32_t AllocateResourceIndex(uint32_t pdcId,
                                       uint32_t jobId,
                                       uint32_t initiator,
                                       uint32_t maxResponseSize);
        uint32_t GetClearPsn(uint32_t pdcId) const;
        void AdvanceClearPsn(uint32_t pdcId, uint32_t newClearPsn);
        void UpdatePdcState(uint32_t pdcId, TargetStateEnum newState, const std::string &reason);
        bool IsOutstandingResponseLimited(uint32_t pdcId) const;
        std::shared_ptr<PdcTargetState> GetPdc(uint32_t pdcId) const;
        std::string DescribePdcState(uint32_t pdcId) const;

    private:
        std::map<uint32_t, std::shared_ptr<PdcTargetState>> m_pdcStates;
        uint32_t m_nextResourceIndex{1};
    };

} // namespace ns3

#endif // UET_PDS_TARGET_STATE_H
