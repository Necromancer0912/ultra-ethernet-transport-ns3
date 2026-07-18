#include "ns3/core-module.h"
#include "ns3/point-to-point-module.h"

#include <string>

using namespace ns3;

NS_LOG_COMPONENT_DEFINE("UetAdvancedDemo");

int main(int argc, char *argv[])
{
    bool verbose = true;
    std::string scenario = "all";

    CommandLine cmd(__FILE__);
    cmd.AddValue("verbose", "Print detailed transactions", verbose);
    cmd.AddValue("scenario", "Scenario: all|control|target|integration", scenario);
    cmd.Parse(argc, argv);

    if (verbose)
    {
        LogComponentEnable("UetAdvancedDemo", LOG_LEVEL_INFO);
    }

    NS_LOG_UNCOND("\n=== UET Advanced SES/PDS Implementation Demo ===\n");
    NS_LOG_UNCOND("[PHASE 3-4] Control Packets + Target State Management");
    NS_LOG_UNCOND("[SPEC REF] Section 3.5.16 (Control Packets), 3.5 (Target State)\n");

    UetPdsControlPacket controlPacket;
    UetPdsTargetState targetState;

    if (scenario == "all" || scenario == "control")
    {
        NS_LOG_UNCOND("--- Scenario 1: Control Packet Management (Section 3.5.16) ---");

        controlPacket.SetControlType(UetPdsControlPacket::NOOP);
        controlPacket.SetPsn(1);
        controlPacket.SetSpdcId(100);
        controlPacket.SetDpdcId(200);
        controlPacket.SetRequiresAck(true);

        NS_LOG_UNCOND("[CP-NOOP] " << controlPacket.Describe()
                                   << " -- Opens PDC, triggers ACK (Section 3.5.16.1)");

        controlPacket.SetControlType(UetPdsControlPacket::CLEAR_COMMAND);
        controlPacket.SetPayload(100);
        controlPacket.SetPsn(0);
        controlPacket.SetRequiresAck(false);

        NS_LOG_UNCOND("[CP-CLEAR] " << controlPacket.Describe()
                                    << " -- Clears state >= PSN 100 (Section 3.5.16.3.1)");

        controlPacket.SetControlType(UetPdsControlPacket::ACK_REQUEST);
        controlPacket.SetPsn(50);
        controlPacket.SetPayload(0x0F00);
        controlPacket.SetRequiresAck(true);

        NS_LOG_UNCOND("[CP-ACK_REQ] " << controlPacket.Describe()
                                      << " -- Recover lost ACK for PSN 50 (Section 3.5.16.2)");

        NS_LOG_UNCOND("");
    }

    if (scenario == "all" || scenario == "target")
    {
        NS_LOG_UNCOND("--- Scenario 2: Target State + Default Response Storage (Section 3.5, "
                      "3.4.4.1.1) ---");

        const uint32_t pdcId = 300;
        targetState.GetOrCreatePdc(pdcId, UET_MODE_RUD);

        NS_LOG_UNCOND("[TARGET] " << targetState.DescribePdcState(pdcId)
                                  << " -- PDC initialized");

        for (uint32_t psn = 1; psn <= 5; ++psn)
        {
            targetState.TrackReceivedPsn(pdcId, psn);
        }

        NS_LOG_UNCOND("[TARGET] Tracked PSN 1-5");
        NS_LOG_UNCOND("[TARGET] " << targetState.DescribePdcState(pdcId)
                                  << " -- Packets received and tracked");

        const uint32_t ri1 = targetState.AllocateResourceIndex(pdcId, 101, 0, 1024);
        const uint32_t ri2 = targetState.AllocateResourceIndex(pdcId, 101, 0, 2048);

        NS_LOG_UNCOND("[TARGET] Allocated resource indices: ri1=" << ri1 << " ri2=" << ri2);

        targetState.StoreDefaultResponse(pdcId, ri1, RC_DEFAULT_RESPONSE, 1024);
        targetState.StoreDefaultResponse(pdcId, ri2, RC_OK, 2048);

        NS_LOG_UNCOND("[TARGET] " << targetState.DescribePdcState(pdcId)
                                  << " -- Stored default responses (Section 3.4.4.1.1)");

        const bool hasResp1 = targetState.HasStoredResponse(pdcId, ri1);
        const bool hasResp2 = targetState.HasStoredResponse(pdcId, ri2);

        NS_LOG_UNCOND("[TARGET] Resource " << ri1 << " has response: "
                                           << (hasResp1 ? "yes" : "no"));
        NS_LOG_UNCOND("[TARGET] Resource " << ri2 << " has response: "
                                           << (hasResp2 ? "yes" : "no"));

        targetState.AdvanceClearPsn(pdcId, 5);
        NS_LOG_UNCOND("[TARGET] " << targetState.DescribePdcState(pdcId)
                                  << " -- Advanced clear PSN to 5");

        NS_LOG_UNCOND("");
    }

    if (scenario == "all" || scenario == "integration")
    {
        NS_LOG_UNCOND("--- Scenario 3: Integration Test - Dynamic SES/PDS + Control + Target ---");

        UetSesPdsEngine initiator;
        UetSesPdsEngine target;

        const uint32_t initiatorFa = 0x0A000001;
        const uint32_t targetFa = 0x0A000008;
        const uint32_t jobId = 555;
        const uint32_t pdcId = jobId * 1000;
        const uint64_t messageBytes = 2048;

        initiator.SetSrcFa(initiatorFa);
        initiator.SetMsgMtu(4096);
        target.SetSrcFa(targetFa);
        target.SetMsgMtu(4096);

        bool txCompletionSeen = false;
        bool txSuccess = false;
        uint8_t txReturnCode = RC_INTERNAL_ERROR;
        uint32_t rxMessages = 0;

        initiator.SetWireSendCb([&target](Ptr<Packet> pkt, uint32_t /*dstFa*/)
                                { target.ProcessRxPacket(pkt, initiatorFa); });
        target.SetWireSendCb([&initiator](Ptr<Packet> pkt, uint32_t /*dstFa*/)
                             { initiator.ProcessRxPacket(pkt, targetFa); });

        initiator.SetTxCompletionCb([&](uint16_t msgId, bool ok, uint8_t rc)
                                    {
                                        txCompletionSeen = true;
                                        txSuccess = ok;
                                        txReturnCode = rc;
                                        NS_LOG_UNCOND("[TX-COMPLETE] msgId=" << msgId
                                                                              << " ok=" << (ok ? "yes" : "no")
                                                                              << " rc=0x" << std::hex
                                                                              << static_cast<uint32_t>(rc)
                                                                              << std::dec); });

        target.SetRxMessageCb([&](Ptr<Packet> reqData, const UetSesHeader &ses)
                              {
                                  ++rxMessages;
                                  targetState.TrackReceivedPsn(pdcId, rxMessages);
                                  NS_LOG_UNCOND("[TARGET-RX] msgId=" << ses.message_id
                                                                     << " payload=" << reqData->GetSize() << "B"); });

        targetState.GetOrCreatePdc(pdcId, UET_MODE_ROD);

        uint16_t msgId = initiator.Send(targetFa,
                                        0,
                                        UET_MODE_ROD,
                                        UET_WRITE,
                                        messageBytes,
                                        0x1000,
                                        jobId,
                                        1,
                                        5,
                                        true);
        initiator.Pump();

        const uint32_t ri = targetState.AllocateResourceIndex(pdcId, jobId, 0, 4096);
        targetState.StoreDefaultResponse(
            pdcId,
            ri,
            txSuccess ? RC_OK : RC_RETRY_REQUIRED,
            static_cast<uint32_t>(messageBytes));
        targetState.AdvanceClearPsn(pdcId, rxMessages);

        NS_LOG_UNCOND("[DYNAMIC TX] msgId=" << msgId
                                            << " mode=ROD"
                                            << " txPackets=" << initiator.GetTxPktCount()
                                            << " targetRxPackets=" << target.GetRxPktCount()
                                            << " completion=" << (txCompletionSeen ? "seen" : "pending")
                                            << " rc=0x" << std::hex
                                            << static_cast<uint32_t>(txReturnCode)
                                            << std::dec);

        NS_LOG_UNCOND("[TARGET] " << targetState.DescribePdcState(pdcId));

        NS_LOG_UNCOND("");
    }

    NS_LOG_UNCOND("=== Demo Complete ===");
    NS_LOG_UNCOND("[Next Phase] Full RdmaHw integration + ordering constraints");

    return 0;
}
