#include "uet-pds-target-state.h"

#include <algorithm>
#include <sstream>

namespace ns3
{

    UetPdsTargetState::UetPdsTargetState() = default;

    UetPdsTargetState::~UetPdsTargetState() = default;

    std::shared_ptr<UetPdsTargetState::PdcTargetState>
    UetPdsTargetState::GetOrCreatePdc(uint32_t pdcId, UetDeliveryMode mode)
    {
        auto it = m_pdcStates.find(pdcId);
        if (it != m_pdcStates.end())
        {
            return it->second;
        }

        auto pdc = std::shared_ptr<PdcTargetState>(new PdcTargetState());
        pdc->pdcId = pdcId;
        pdc->mode = mode;
        pdc->rodMode = (mode == UET_MODE_ROD);
        pdc->state = TARGET_IDLE;
        m_pdcStates[pdcId] = pdc;

        std::ostringstream os;
        os << "PDC created: id=" << pdcId << " mode=" << static_cast<uint32_t>(mode);
        pdc->stateTransitions.push_back(os.str());

        return pdc;
    }

    void
    UetPdsTargetState::TrackReceivedPsn(uint32_t pdcId, uint32_t psn)
    {
        auto pdc = GetOrCreatePdc(pdcId, UET_MODE_RUD);
        pdc->receivedPsns.insert(psn);

        if (pdc->rodMode && psn > pdc->lastReceivedPsn)
        {
            pdc->lastReceivedPsn = psn;
        }
        else if (!pdc->rodMode)
        {
            pdc->lastReceivedPsn = psn;
        }
    }

    void
    UetPdsTargetState::StoreDefaultResponse(uint32_t pdcId,
                                            uint32_t resourceIndex,
                                            UetSesReturnCode responseCode,
                                            uint32_t modifiedLength)
    {
        auto pdc = GetOrCreatePdc(pdcId, UET_MODE_RUD);

        auto it = pdc->resources.find(resourceIndex);
        if (it == pdc->resources.end())
        {
            it = pdc->resources
                     .insert({resourceIndex, ResourceEntry()})
                     .first;
        }

        it->second.resourceIndex = resourceIndex;
        it->second.responseStored = true;
        it->second.storedResponseCode = responseCode;
        it->second.storedModifiedLength = modifiedLength;
        pdc->outstandingGuaranteedDeliveryResponses++;
    }

    bool
    UetPdsTargetState::HasStoredResponse(uint32_t pdcId, uint32_t resourceIndex) const
    {
        auto pdc = GetPdc(pdcId);
        if (!pdc)
        {
            return false;
        }

        auto it = pdc->resources.find(resourceIndex);
        if (it == pdc->resources.end())
        {
            return false;
        }

        return it->second.responseStored;
    }

    UetSesReturnCode
    UetPdsTargetState::GetStoredResponseCode(uint32_t pdcId, uint32_t resourceIndex) const
    {
        auto pdc = GetPdc(pdcId);
        if (!pdc)
        {
            return RC_INTERNAL_ERROR;
        }

        auto it = pdc->resources.find(resourceIndex);
        if (it == pdc->resources.end())
        {
            return RC_INTERNAL_ERROR;
        }

        return it->second.storedResponseCode;
    }

    uint32_t
    UetPdsTargetState::AllocateResourceIndex(uint32_t pdcId,
                                             uint32_t jobId,
                                             uint32_t initiator,
                                             uint32_t maxResponseSize)
    {
        auto pdc = GetOrCreatePdc(pdcId, UET_MODE_RUD);
        const uint32_t ri = m_nextResourceIndex++;

        ResourceEntry entry;
        entry.resourceIndex = ri;
        entry.riGeneration = 0;
        entry.jobId = jobId;
        entry.initiator = initiator;
        entry.maxResponseSize = maxResponseSize;

        pdc->resources[ri] = entry;
        return ri;
    }

    uint32_t
    UetPdsTargetState::GetClearPsn(uint32_t pdcId) const
    {
        auto pdc = GetPdc(pdcId);
        if (!pdc)
        {
            return 0;
        }

        return pdc->clearPsn;
    }

    void
    UetPdsTargetState::AdvanceClearPsn(uint32_t pdcId, uint32_t newClearPsn)
    {
        auto pdc = GetOrCreatePdc(pdcId, UET_MODE_RUD);
        if (newClearPsn >= pdc->clearPsn)
        {
            pdc->clearPsn = newClearPsn;

            uint32_t clearedResponses = 0;
            for (auto it = pdc->resources.begin(); it != pdc->resources.end();)
            {
                if (it->first <= newClearPsn)
                {
                    if (it->second.responseStored)
                    {
                        ++clearedResponses;
                    }
                    it = pdc->resources.erase(it);
                    continue;
                }
                ++it;
            }

            if (clearedResponses > 0)
            {
                pdc->outstandingGuaranteedDeliveryResponses =
                    (clearedResponses >= pdc->outstandingGuaranteedDeliveryResponses)
                        ? 0
                        : (pdc->outstandingGuaranteedDeliveryResponses - clearedResponses);
            }

            std::ostringstream os;
            os << "Clear advanced: clearPsn=" << newClearPsn
               << " responses_cleared=" << clearedResponses;
            pdc->stateTransitions.push_back(os.str());
        }
    }

    void
    UetPdsTargetState::UpdatePdcState(uint32_t pdcId, TargetStateEnum newState, const std::string &reason)
    {
        auto pdc = GetOrCreatePdc(pdcId, UET_MODE_RUD);
        pdc->state = newState;

        std::ostringstream os;
        os << "State: " << static_cast<uint32_t>(newState) << " (" << reason << ")";
        pdc->stateTransitions.push_back(os.str());
    }

    bool
    UetPdsTargetState::IsOutstandingResponseLimited(uint32_t pdcId) const
    {
        auto pdc = GetPdc(pdcId);
        if (!pdc)
        {
            return false;
        }

        return pdc->outstandingGuaranteedDeliveryResponses >= pdc->maxOutstandingResponses;
    }

    std::shared_ptr<UetPdsTargetState::PdcTargetState>
    UetPdsTargetState::GetPdc(uint32_t pdcId) const
    {
        auto it = m_pdcStates.find(pdcId);
        if (it != m_pdcStates.end())
        {
            return it->second;
        }

        return nullptr;
    }

    std::string
    UetPdsTargetState::DescribePdcState(uint32_t pdcId) const
    {
        auto pdc = GetPdc(pdcId);
        if (!pdc)
        {
            return "[PDC] unknown pdcId=" + std::to_string(pdcId);
        }

        std::ostringstream os;
        os << "[PDC] id=" << pdc->pdcId << " mode=" << static_cast<uint32_t>(pdc->mode)
           << " state=" << static_cast<uint32_t>(pdc->state) << " clearPsn=" << pdc->clearPsn
           << " lastReceivedPsn=" << pdc->lastReceivedPsn
           << " outstandingResponses=" << pdc->outstandingGuaranteedDeliveryResponses
           << " resourceCount=" << pdc->resources.size();

        return os.str();
    }

} // namespace ns3
