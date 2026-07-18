#include "ns3/core-module.h"
#include "ns3/network-module.h"
#include "ns3/point-to-point-module.h"

#include <algorithm>
#include <cctype>
#include <memory>
#include <random>
#include <set>
#include <sstream>
#include <string>
#include <vector>

using namespace ns3;

NS_LOG_COMPONENT_DEFINE("UetSesPdsDemo");

namespace
{

    std::string
    Lowercase(std::string value)
    {
        std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c)
                       { return static_cast<char>(std::tolower(c)); });
        return value;
    }

    UetDeliveryMode
    ParseMode(const std::string &mode)
    {
        const std::string v = Lowercase(mode);
        if (v == "uud")
        {
            return UET_MODE_UUD;
        }
        if (v == "rudi")
        {
            return UET_MODE_RUDI;
        }
        if (v == "rod")
        {
            return UET_MODE_ROD;
        }
        return UET_MODE_RUD;
    }

    UetSesOpcode
    ParseOpCode(const std::string &op)
    {
        const std::string v = Lowercase(op);
        if (v == "tagged_send")
        {
            return UET_TAGGED_SEND;
        }
        if (v == "read")
        {
            return UET_READ;
        }
        if (v == "write")
        {
            return UET_WRITE;
        }
        if (v == "atomic")
        {
            return UET_ATOMIC;
        }
        return UET_SEND;
    }

    std::set<uint32_t>
    ParsePsnSet(const std::string &csv)
    {
        std::set<uint32_t> out;
        if (csv.empty())
        {
            return out;
        }

        std::stringstream ss(csv);
        std::string token;
        while (std::getline(ss, token, ','))
        {
            if (!token.empty())
            {
                out.insert(static_cast<uint32_t>(std::stoul(token)));
            }
        }
        return out;
    }

} // namespace

int main(int argc, char *argv[])
{
    bool verbose = true;
    bool printTransitions = false;

    uint32_t numNodes = 4;
    uint32_t activeFlows = 4;
    uint32_t initiator = 0;
    uint32_t target = 0;
    uint32_t jobId = 101;
    uint64_t messageId = 0;
    uint32_t messageBytes = 0;
    uint32_t mtuBytes = 0;
    uint32_t maxRetries = 0;

    double offeredLoadGbpsPerFlow = 15.0;
    double linkRateGbps = 100.0;
    std::string linkDelay = "2us";
    std::string mode = "rud";
    std::string opCode = "send";

    double dropRequestRate = 0.0;
    double dropAckRate = 0.0;
    bool firstAttemptOnly = true;
    bool storeDefaultResponse = true;

    std::string dropReqPsnsCsv = "";
    std::string dropAckPsnsCsv = "";

    CommandLine cmd(__FILE__);
    cmd.AddValue("verbose", "Print detailed SES/PDS report", verbose);
    cmd.AddValue("printTransitions", "Print each PDC state transition", printTransitions);
    cmd.AddValue("numNodes", "Number of nodes in a linear topology", numNodes);
    cmd.AddValue("activeFlows", "Number of active flows sharing the path", activeFlows);
    cmd.AddValue("initiator", "Initiator node index", initiator);
    cmd.AddValue("target", "Target node index (0 selects last node)", target);
    cmd.AddValue("jobId", "SES Job ID", jobId);
    cmd.AddValue("messageId", "SES message ID (0 means auto-generated)", messageId);
    cmd.AddValue("messageBytes", "Payload bytes (0 means topology/load-driven default)", messageBytes);
    cmd.AddValue("mtuBytes", "MTU bytes (0 means derive from topology)", mtuBytes);
    cmd.AddValue("maxRetries", "Retry budget (0 means mode/load-driven default)", maxRetries);
    cmd.AddValue("offeredLoadGbpsPerFlow", "Offered load per active flow in Gbps", offeredLoadGbpsPerFlow);
    cmd.AddValue("linkRateGbps", "Bottleneck link rate in Gbps", linkRateGbps);
    cmd.AddValue("linkDelay", "Per-link propagation delay", linkDelay);
    cmd.AddValue("mode", "PDS mode: uud|rudi|rud|rod", mode);
    cmd.AddValue("opCode", "SES opcode: send|tagged_send|read|write|atomic", opCode);
    cmd.AddValue("dropRequestRate", "Probability [0..1] to drop request packets", dropRequestRate);
    cmd.AddValue("dropAckRate", "Probability [0..1] to drop ack packets", dropAckRate);
    cmd.AddValue("firstAttemptOnly", "Apply probabilistic drops only on first attempt", firstAttemptOnly);
    cmd.AddValue("storeDefaultResponse", "Store semantic default response on non-reliable failures", storeDefaultResponse);
    cmd.AddValue("dropReqPsns", "Comma-separated PSNs dropped on first attempt", dropReqPsnsCsv);
    cmd.AddValue("dropAckPsns", "Comma-separated ACK PSNs dropped on first attempt", dropAckPsnsCsv);
    cmd.Parse(argc, argv);

    if (verbose)
    {
        LogComponentEnable("UetSesPdsDemo", LOG_LEVEL_INFO);
    }

    numNodes = std::max(2u, numNodes);
    initiator = std::min(initiator, numNodes - 1);
    if (target == 0)
    {
        target = numNodes - 1;
    }
    target = std::min(target, numNodes - 1);
    if (initiator == target)
    {
        target = (target == 0) ? 1 : (target - 1);
    }

    NodeContainer nodes;
    nodes.Create(numNodes);

    PointToPointHelper p2p;
    p2p.SetDeviceAttribute("DataRate", StringValue(std::to_string(linkRateGbps) + "Gbps"));
    p2p.SetChannelAttribute("Delay", StringValue(linkDelay));
    if (mtuBytes > 0)
    {
        p2p.SetDeviceAttribute("Mtu", UintegerValue(mtuBytes));
    }

    std::vector<NetDeviceContainer> links;
    links.reserve(numNodes - 1);
    for (uint32_t i = 0; i + 1 < numNodes; ++i)
    {
        NodeContainer pair(nodes.Get(i), nodes.Get(i + 1));
        links.push_back(p2p.Install(pair));
    }

    uint32_t topologyMtu = mtuBytes;
    if (!links.empty())
    {
        Ptr<PointToPointNetDevice> dev = DynamicCast<PointToPointNetDevice>(links.front().Get(0));
        if (dev)
        {
            topologyMtu = dev->GetMtu();
        }
    }

    const double totalOfferedGbps = offeredLoadGbpsPerFlow * static_cast<double>(std::max(1u, activeFlows));
    const double pathRateGbps = std::max(1.0, linkRateGbps);
    const uint32_t hopCount = static_cast<uint32_t>(std::abs(static_cast<int64_t>(target) -
                                                             static_cast<int64_t>(initiator)));
    const double offeredLoadRatio = totalOfferedGbps / pathRateGbps;

    const UetDeliveryMode parsedMode = ParseMode(mode);
    const UetSesOpcode parsedOpcode = ParseOpCode(opCode);
    const std::set<uint32_t> dropReqPsns = ParsePsnSet(dropReqPsnsCsv);
    const std::set<uint32_t> dropAckPsns = ParsePsnSet(dropAckPsnsCsv);

    const uint32_t srcFa = 0x0A000001u + initiator;
    const uint32_t dstFa = 0x0A000001u + target;
    const uint32_t resolvedMtu = (mtuBytes > 0) ? mtuBytes : std::max(512u, topologyMtu);

    uint64_t resolvedMessageBytes = messageBytes;
    if (resolvedMessageBytes == 0)
    {
        const double loadFactor = std::max(1.0, offeredLoadRatio);
        resolvedMessageBytes = static_cast<uint64_t>(std::max(256.0, 1024.0 * loadFactor));
    }

    Ptr<UetSesPdsEngine> initiatorEngine = CreateObject<UetSesPdsEngine>();
    Ptr<UetSesPdsEngine> targetEngine = CreateObject<UetSesPdsEngine>();

    initiatorEngine->SetSrcFa(srcFa);
    targetEngine->SetSrcFa(dstFa);
    initiatorEngine->SetMsgMtu(resolvedMtu);
    targetEngine->SetMsgMtu(resolvedMtu);

    uint32_t rxMessages = 0;
    uint64_t rxBytes = 0;
    bool completionSeen = false;
    bool completionOk = false;
    uint8_t completionRc = RC_INTERNAL_ERROR;

    // Wire the two engines with deterministic drop injection. Request packets
    // (initiator → target) whose ordinal is in dropReqPsns are dropped on the
    // first attempt; likewise ACK ordinals in dropAckPsns. Probabilistic drops
    // use a fixed-seed generator so runs are reproducible.
    auto reqCounter = std::make_shared<uint32_t>(0);
    auto ackCounter = std::make_shared<uint32_t>(0);
    auto rng = std::make_shared<std::mt19937>(12345u);
    auto uni = std::make_shared<std::uniform_real_distribution<double>>(0.0, 1.0);
    uint32_t retxDelayNs = 0; // request retransmits delivered via simulator events

    initiatorEngine->SetWireSendCb(
        [targetEngine, srcFa, reqCounter, rng, uni, dropReqPsns, dropRequestRate]
        (Ptr<Packet> pkt, uint32_t /*dstFa*/)
        {
            UetPdsHeader peek;
            pkt->PeekHeader(peek);
            if (peek.IsRequest() || peek.prologue.pdsType == PDS_TYPE_RUDI_REQ)
            {
                uint32_t ord = (*reqCounter)++;
                if (dropReqPsns.count(ord))
                {
                    NS_LOG_UNCOND("[Drop] request ordinal " << ord << " dropped (injected)");
                    return;
                }
                if (dropRequestRate > 0.0 && (*uni)(*rng) < dropRequestRate)
                {
                    NS_LOG_UNCOND("[Drop] request ordinal " << ord << " dropped (random)");
                    return;
                }
            }
            targetEngine->ProcessRxPacket(pkt, srcFa);
        });
    targetEngine->SetWireSendCb(
        [initiatorEngine, dstFa, ackCounter, rng, uni, dropAckPsns, dropAckRate]
        (Ptr<Packet> pkt, uint32_t /*dstFa*/)
        {
            UetPdsHeader peek;
            pkt->PeekHeader(peek);
            if (peek.IsAck())
            {
                uint32_t ord = (*ackCounter)++;
                if (dropAckPsns.count(ord))
                {
                    NS_LOG_UNCOND("[Drop] ACK ordinal " << ord << " dropped (injected)");
                    return;
                }
                if (dropAckRate > 0.0 && (*uni)(*rng) < dropAckRate)
                {
                    NS_LOG_UNCOND("[Drop] ACK ordinal " << ord << " dropped (random)");
                    return;
                }
            }
            initiatorEngine->ProcessRxPacket(pkt, dstFa);
        });
    (void)retxDelayNs;

    targetEngine->SetRxMessageCb([&](Ptr<Packet> pkt, const UetSesHeader &ses)
                                 {
                                     ++rxMessages;
                                     rxBytes += pkt->GetSize();
                                     if (printTransitions)
                                     {
                                         NS_LOG_UNCOND("  rx msgId=" << ses.message_id
                                                                        << " opcode=0x" << std::hex
                                                                        << static_cast<uint32_t>(ses.opcode)
                                                                        << std::dec
                                                                        << " bytes=" << pkt->GetSize());
                                     } });

    initiatorEngine->SetTxCompletionCb([&](uint16_t messageIdCb, bool ok, uint8_t rc)
                                       {
                                           completionSeen = true;
                                           completionOk = ok;
                                           completionRc = rc;
                                           if (verbose)
                                           {
                                               NS_LOG_UNCOND("[TX-Completion] msgId=" << messageIdCb
                                                                                      << " ok=" << (ok ? "yes" : "no")
                                                                                      << " rc=0x" << std::hex
                                                                                      << static_cast<uint32_t>(rc)
                                                                                      << std::dec);
                                           } });

    // Loss-recovery configuration: a short RTO keeps the demo fast, and the
    // retry budget is user-controllable.
    initiatorEngine->SetRtoInitUs(50.0);
    targetEngine->SetRtoInitUs(50.0);
    if (maxRetries > 0)
    {
        initiatorEngine->SetMaxRetries(maxRetries);
    }

    const uint16_t msgId = initiatorEngine->Send(dstFa,
                                                 0,
                                                 parsedMode,
                                                 parsedOpcode,
                                                 resolvedMessageBytes,
                                                 0x1000,
                                                 jobId,
                                                 static_cast<uint16_t>(initiator & 0x0FFFu),
                                                 static_cast<uint16_t>(target & 0x0FFFu),
                                                 storeDefaultResponse);

    if (msgId == 0)
    {
        NS_LOG_UNCOND("[Dynamic SES/PDS] Failed to enqueue message");
        return 1;
    }

    const uint32_t expectedPackets =
        static_cast<uint32_t>((resolvedMessageBytes + resolvedMtu - 1) / resolvedMtu);
    for (uint32_t i = 0; i < expectedPackets + 2; ++i)
    {
        initiatorEngine->Pump();
    }

    // Let RTO-driven retransmissions run to completion (bounded by the retry
    // budget, so this always terminates).
    Simulator::Stop(Seconds(5.0));
    Simulator::Run();
    Simulator::Destroy();

    NS_LOG_UNCOND("[Dynamic SES/PDS] msgId=" << msgId
                                             << " mode=" << Lowercase(mode)
                                             << " opcode=" << Lowercase(opCode)
                                             << " resolvedBytes=" << resolvedMessageBytes
                                             << " mtu=" << resolvedMtu
                                             << " expectedPackets=" << expectedPackets);
    NS_LOG_UNCOND("[Dynamic SES/PDS] initiator txPkts=" << initiatorEngine->GetTxPktCount()
                                                        << " rxPkts=" << initiatorEngine->GetRxPktCount()
                                                        << " retx=" << initiatorEngine->GetRetxCount()
                                                        << " nackRcvd=" << initiatorEngine->GetNackRcvdCount());
    NS_LOG_UNCOND("[Dynamic SES/PDS] target txPkts=" << targetEngine->GetTxPktCount()
                                                     << " rxPkts=" << targetEngine->GetRxPktCount()
                                                     << " nackSent=" << targetEngine->GetNackSentCount());
    NS_LOG_UNCOND("[Dynamic SES/PDS] deliveredMessages=" << rxMessages
                                                         << " deliveredBytes=" << rxBytes);

    if (printTransitions)
    {
        NS_LOG_UNCOND("[Initiator PDC]");
        NS_LOG_UNCOND(initiatorEngine->GetPdcStateReport());
        NS_LOG_UNCOND("[Target PDC]");
        NS_LOG_UNCOND(targetEngine->GetPdcStateReport());
    }

    const bool reliableMode = (parsedMode == UET_MODE_RUD || parsedMode == UET_MODE_ROD);
    const bool success = reliableMode ? (completionSeen && completionOk) : (rxMessages > 0);

    if (!reliableMode)
    {
        NS_LOG_UNCOND("[Dynamic SES/PDS] completion callback not required for mode="
                      << Lowercase(mode));
    }
    else if (!completionSeen)
    {
        NS_LOG_UNCOND("[Dynamic SES/PDS] completion callback not observed");
    }

    if (completionSeen)
    {
        NS_LOG_UNCOND("[Dynamic SES/PDS] completion rc=0x" << std::hex
                                                           << static_cast<uint32_t>(completionRc)
                                                           << std::dec);
    }

    return success ? 0 : 1;
}
