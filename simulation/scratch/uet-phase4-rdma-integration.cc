#include "ns3/core-module.h"
#include "ns3/point-to-point-module.h"

#include <iomanip>
#include <iostream>
#include <string>

using namespace ns3;

NS_LOG_COMPONENT_DEFINE("UetPhase4Demo");

int main(int argc, char *argv[])
{
    bool verbose = true;
    std::string scenario = "all";

    CommandLine cmd(__FILE__);
    cmd.AddValue("verbose", "Print detailed logs", verbose);
    cmd.AddValue("scenario", "Scenario: all|send|receive|control", scenario);
    cmd.Parse(argc, argv);

    if (verbose)
    {
        LogComponentEnable("UetPhase4Demo", LOG_LEVEL_INFO);
        LogComponentEnable("RdmaHwUetIntegration", LOG_LEVEL_LOGIC);
    }

    NS_LOG_UNCOND("\n=== UET Phase 4: RDMA HW Integration ===\n");
    NS_LOG_UNCOND("[PHASE 4] SES/PDS Engine + RDMA Hardware");
    NS_LOG_UNCOND("[SPEC REF] Section 3.5.14 (Packet Delivery Service Behavior)\n");

    // Initialize integration layer
    Ptr<RdmaHwUetIntegration> integration = CreateObject<RdmaHwUetIntegration>();
    integration->Initialize(8, 100000000000ULL, 2048); // 8 nodes, 100Gbps, 2KB MTU

    if (scenario == "all" || scenario == "send")
    {
        NS_LOG_UNCOND(
            "--- Scenario 1: TX Path - Packet Processing and Sequencing (Section 3.5.14) ---");

        struct TxTestCase
        {
            uint32_t srcId;
            uint32_t dstId;
            uint16_t sport;
            uint32_t jobId;
            uint8_t mode;
            double dropRate;
            const char *modeStr;
        };

        TxTestCase txTests[] = {
            {0, 7, 1001, 100, 2, 0.05, "RUD"},
            {0, 7, 1002, 101, 3, 0.00, "ROD"},
            {1, 6, 2001, 200, 4, 0.03, "RUDI"},
        };

        for (const auto &test : txTests)
        {
            NS_LOG_UNCOND("\n[TX] Testing mode=" << test.modeStr << " src=" << test.srcId
                                                 << " dst=" << test.dstId);

            Ptr<Packet> basePacket = Create<Packet>(1472); // Typical payload

            // Process multiple packets in sequence
            for (uint32_t pktSeq = 0; pktSeq < 5; ++pktSeq)
            {
                Ptr<Packet> txPacket = basePacket->Copy();
                auto result = integration->ProcessTxPacket(
                    txPacket,
                    test.srcId,
                    test.dstId,
                    test.sport,
                    test.jobId,
                    test.mode,
                    test.dropRate);

                if (result == nullptr)
                {
                    NS_LOG_UNCOND("  [TX-DROP] Packet " << pktSeq
                                                        << " dropped by simulation");
                }
                else
                {
                    NS_LOG_UNCOND("  [TX-OK] Packet " << pktSeq << " size=" << result->GetSize()
                                                      << " bytes");
                }
            }
        }
        NS_LOG_UNCOND("");
    }

    if (scenario == "all" || scenario == "receive")
    {
        NS_LOG_UNCOND("--- Scenario 2: RX Path - State Tracking and Default Response (Section "
                      "3.4.4.1.1) ---");

        struct RxTestCase
        {
            uint32_t srcId;
            uint32_t dstId;
            uint16_t sport;
            uint32_t jobId;
            uint8_t mode;
            double dropRate;
            const char *modeStr;
        };

        RxTestCase rxTests[] = {
            {7, 0, 1001, 100, 2, 0.05, "RUD"},
            {6, 1, 2001, 200, 4, 0.00, "RUDI"},
        };

        for (const auto &test : rxTests)
        {
            NS_LOG_UNCOND("\n[RX] Testing mode=" << test.modeStr << " src=" << test.srcId
                                                 << " dst=" << test.dstId);

            Ptr<Packet> basePacket = Create<Packet>(1472);

            for (uint32_t pktSeq = 0; pktSeq < 3; ++pktSeq)
            {
                Ptr<Packet> rxPacket = basePacket->Copy();
                bool shouldRespond = integration->ProcessRxPacket(
                    rxPacket,
                    test.srcId,
                    test.dstId,
                    test.sport,
                    test.jobId,
                    test.mode,
                    test.dropRate);

                if (shouldRespond)
                {
                    uint8_t respCode = integration->GenerateDefaultResponse(
                        test.srcId,
                        test.dstId,
                        test.sport,
                        test.jobId,
                        test.mode);
                    NS_LOG_UNCOND("  [RX-ACK] Packet " << pktSeq << " -> response code=0x"
                                                       << std::hex << (uint32_t)respCode
                                                       << std::dec);
                }
                else
                {
                    NS_LOG_UNCOND("  [RX-DROP] ACK dropped for packet " << pktSeq);
                }
            }
        }
        NS_LOG_UNCOND("");
    }

    if (scenario == "all" || scenario == "control")
    {
        NS_LOG_UNCOND("--- Scenario 3: Control Packet Generation (Section 3.5.16) ---");

        NS_LOG_UNCOND("\n[CP] Control packet pending checks:");
        NS_LOG_UNCOND("  Testing CLEAR_COMMAND generation under resource exhaustion...");

        uint64_t pdcId1 = 0x6400600064ULL; // Synthetic PDC ID
        auto cp1 = integration->GetPendingControlPacket(pdcId1);

        if (cp1 != nullptr)
        {
            NS_LOG_UNCOND("  [CP-PENDING] CLEAR_COMMAND for PDC=" << std::hex << pdcId1
                                                                  << std::dec);
        }
        else
        {
            NS_LOG_UNCOND("  [CP-NONE] No control packets pending (expected at this stage)");
        }

        NS_LOG_UNCOND("\n[CP] Expected control packet types (from Section 3.5.16):");
        NS_LOG_UNCOND("  - NOOP (0): PDC initialization");
        NS_LOG_UNCOND("  - ACK_REQUEST (1): Lost ACK recovery");
        NS_LOG_UNCOND("  - CLEAR_COMMAND (2): State cleanup");
        NS_LOG_UNCOND("  - CLEAR_REQUEST (3): Initiator requests clear");
        NS_LOG_UNCOND("  - PDC_CLOSE_COMMAND (4): Close initiated by target");
        NS_LOG_UNCOND("  - PDC_CLOSE_REQUEST (5): Close requested by initiator");
        NS_LOG_UNCOND("  - PROBE (6): Connectivity check");
        NS_LOG_UNCOND("  - CREDIT (7): Flow control credit update");

        NS_LOG_UNCOND("");
    }

    if (scenario == "all")
    {
        NS_LOG_UNCOND("--- Scenario 4: Integration State Summary ---");
        NS_LOG_UNCOND(integration->DescribeState());
        NS_LOG_UNCOND("");
    }

    NS_LOG_UNCOND("=== Phase 4 Demo Complete ===");
    NS_LOG_UNCOND("[Next] Integrate into actual RDMA send/receive handlers");
    NS_LOG_UNCOND("[Future] Phase 5: Scenario tests + sequence validation\n");

    return 0;
}
