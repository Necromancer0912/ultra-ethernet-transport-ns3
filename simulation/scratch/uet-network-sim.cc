/**
 * @file             uet-network-sim.cc
 * @brief            End-to-end UET benchmark over an ns-3 point-to-point link
 *
 * @details
 * Runs the UetSesPdsEngine over a real ns-3 UDP socket path (queues, serialization
 * delay, propagation delay) between two nodes. Supports deterministic seeding and
 * optional receive-side loss injection (RateErrorModel) to exercise the PDS
 * loss-recovery machinery (RTO retransmission, SACK, NACK).
 *
 * Reported metrics: goodput, message completion rate, delivery latency
 * percentiles, retransmission counts, and PDC state summaries.
 */

#include "ns3/core-module.h"
#include "ns3/network-module.h"
#include "ns3/internet-module.h"
#include "ns3/point-to-point-module.h"
#include "ns3/applications-module.h"
#include "ns3/error-model.h"
#include "ns3/log.h"
#include "ns3/simulator.h"
#include "ns3/udp-socket-factory.h"

#include "../src/point-to-point/model/uet-pdc.h"
#include "../src/point-to-point/model/uet-pds-header.h"
#include "../src/point-to-point/model/uet-ses-header.h"
#include "../src/point-to-point/model/uet-ses-pds-engine.h"

#include <iostream>
#include <sstream>
#include <iomanip>
#include <algorithm>
#include <vector>
#include <fstream>
#include <cmath>
#include <numeric>

using namespace ns3;

NS_LOG_COMPONENT_DEFINE("UetNetworkSim");

// ─────────────────────────────────────────────────────────────────────────────
// STATISTICS
// ─────────────────────────────────────────────────────────────────────────────

struct AggregatedStats
{
    uint64_t totalMsgSent = 0;
    uint64_t totalMsgReceived = 0;
    uint64_t totalMsgCompleted = 0;   // ACKed end-to-end (initiator view)
    uint64_t totalMsgFailed = 0;      // retry budget exhausted
    uint64_t totalBytesSent = 0;
    uint64_t totalBytesReceived = 0;

    Time firstSendTime;
    Time lastSendTime;
    Time firstReceiveTime;
    Time lastReceiveTime;

    std::vector<double> latenciesNs;

    double GetAverageLatencyNs() const {
        if (latenciesNs.empty()) return 0.0;
        return std::accumulate(latenciesNs.begin(), latenciesNs.end(), 0.0) / latenciesNs.size();
    }
    double GetPercentileNs(double q) const {
        if (latenciesNs.empty()) return 0.0;
        std::vector<double> sorted = latenciesNs;
        std::sort(sorted.begin(), sorted.end());
        size_t idx = static_cast<size_t>(sorted.size() * q);
        return sorted[std::min(idx, sorted.size() - 1)];
    }
    double GetMinLatencyNs() const {
        if (latenciesNs.empty()) return 0.0;
        return *std::min_element(latenciesNs.begin(), latenciesNs.end());
    }
    double GetMaxLatencyNs() const {
        if (latenciesNs.empty()) return 0.0;
        return *std::max_element(latenciesNs.begin(), latenciesNs.end());
    }
    double GetJitterNs() const {
        if (latenciesNs.size() < 2) return 0.0;
        double mean = GetAverageLatencyNs();
        double sq_sum = 0.0;
        for (double lat : latenciesNs) sq_sum += (lat - mean) * (lat - mean);
        return std::sqrt(sq_sum / latenciesNs.size());
    }
    double GetThroughputGbps() const {
        if (firstReceiveTime == lastReceiveTime) return 0.0;
        double durationNs = (lastReceiveTime - firstReceiveTime).GetNanoSeconds();
        if (durationNs <= 0) return 0.0;
        return (totalBytesReceived * 8.0) / durationNs;
    }
    double GetMessageRate() const {
        if (firstReceiveTime == lastReceiveTime) return 0.0;
        double durationSec = (lastReceiveTime - firstReceiveTime).GetSeconds();
        if (durationSec <= 0) return 0.0;
        return totalMsgReceived / durationSec;
    }
    double GetDeliveryRate() const {
        if (totalMsgSent == 0) return 0.0;
        return 100.0 * totalMsgReceived / totalMsgSent;
    }
    double GetCompletionRate() const {
        if (totalMsgSent == 0) return 0.0;
        return 100.0 * totalMsgCompleted / totalMsgSent;
    }
};

static AggregatedStats g_stats;
static std::map<uint32_t, Time> g_messageSendTimes;

// ─────────────────────────────────────────────────────────────────────────────
// UET APPLICATION
// ─────────────────────────────────────────────────────────────────────────────

class UetStressTestApp : public Application
{
public:
    UetStressTestApp() : m_socket(0), m_isServer(false), m_running(false), m_messagesSent(0) {}
    virtual ~UetStressTestApp() {}

    void Setup(uint32_t msgSize, uint32_t numMsgs, Ipv4Address dest, uint16_t port, bool isServer,
               uint64_t targetRateBps, uint8_t uetMode)
    {
        m_msgSize = msgSize;
        m_numMsgs = numMsgs;
        m_destIp = dest;
        m_port = port;
        m_isServer = isServer;
        m_targetRateBps = targetRateBps;
        m_uetMode = uetMode;
    }

    UetSesPdsEngine& GetEngine() { return m_engine; }

private:
    virtual void StartApplication() override
    {
        m_running = true;

        if (!m_socket) {
            m_socket = Socket::CreateSocket(GetNode(), UdpSocketFactory::GetTypeId());
            m_socket->Bind(InetSocketAddress(Ipv4Address::GetAny(), m_port));
        }
        m_socket->SetRecvCallback(MakeCallback(&UetStressTestApp::HandleRead, this));

        // Fabric address = this node's real IPv4 address, so the engine's
        // PDC source-address checks line up with what peers see on the wire.
        Ptr<Ipv4> ipv4 = GetNode()->GetObject<Ipv4>();
        uint32_t myFa = ipv4->GetAddress(1, 0).GetLocal().Get();
        m_engine.SetSrcFa(myFa);
        m_engine.SetMsgMtu(4096);
        m_engine.SetRtoInitUs(100.0); // ~RTT + queueing at 200G/100ns scale

        // The engine hands us (packet, dstFa); route it via UDP to that address
        uint16_t port = m_port;
        m_engine.SetWireSendCb([this, port](Ptr<Packet> pkt, uint32_t dstFa) {
            this->m_socket->SendTo(pkt, 0, InetSocketAddress(Ipv4Address(dstFa), port));
        });

        m_engine.SetRxMessageCb([](Ptr<Packet> reqData, const UetSesHeader& ses) {
            // The callback fires once per chunk; a message is only counted
            // as delivered when its EOM chunk arrives.
            Time recvTime = Simulator::Now();
            g_stats.totalBytesReceived += reqData->GetSize();
            g_stats.lastReceiveTime = recvTime;
            if (g_stats.firstReceiveTime == Time(0)) g_stats.firstReceiveTime = recvTime;

            if (ses.eom) {
                g_stats.totalMsgReceived++;
                auto it = g_messageSendTimes.find(ses.message_id);
                if (it != g_messageSendTimes.end()) {
                    g_stats.latenciesNs.push_back((recvTime - it->second).GetNanoSeconds());
                }
            }
        });

        m_engine.SetTxCompletionCb([](uint16_t msgId, bool ok, uint8_t rc) {
            if (ok) g_stats.totalMsgCompleted++;
            else    g_stats.totalMsgFailed++;
        });

        if (!m_isServer) {
            g_stats.firstSendTime = Simulator::Now();
            ScheduleNextSend();
        }
    }

    virtual void StopApplication() override
    {
        m_running = false;
        if (m_sendEvent.IsRunning()) Simulator::Cancel(m_sendEvent);
        if (m_socket) m_socket->Close();
    }

    void HandleRead(Ptr<Socket> socket)
    {
        Ptr<Packet> packet;
        Address from;
        while ((packet = socket->RecvFrom(from))) {
            uint32_t srcFa = InetSocketAddress::ConvertFrom(from).GetIpv4().Get();
            m_engine.ProcessRxPacket(packet, srcFa);
        }
    }

    void ScheduleNextSend()
    {
        if (m_messagesSent < m_numMsgs && m_running)
        {
            double bits = m_msgSize * 8.0;
            double intervalNs = (bits / m_targetRateBps) * 1e9;
            Time tNext = NanoSeconds(std::max(1.0, intervalNs));
            m_sendEvent = Simulator::Schedule(tNext, &UetStressTestApp::SendUetMessage, this);
        }
    }

    void SendUetMessage()
    {
        Time sendTime = Simulator::Now();
        uint32_t dstFa = m_destIp.Get();

        uint16_t msgId = m_engine.Send(
            dstFa,
            0,
            (UetDeliveryMode)m_uetMode,
            (UetSesOpcode)UET_WRITE,
            m_msgSize,
            0x1000ULL + (uint64_t)m_messagesSent * m_msgSize,
            0xABC000 + m_messagesSent,
            1,
            5,
            false);

        if (msgId != 0) {
            g_messageSendTimes[msgId] = sendTime;
            g_stats.totalMsgSent++;
            g_stats.totalBytesSent += m_msgSize;
            g_stats.lastSendTime = sendTime;
            m_messagesSent++;
        }

        if (m_messagesSent % (std::max((uint32_t)1, m_numMsgs / 20)) == 0 || m_messagesSent == 1) {
            double pct = (m_messagesSent * 100.0) / m_numMsgs;
            std::cout << "\r\033[K[Live Progress] Sent " << m_messagesSent << " / " << m_numMsgs
                      << " (" << std::fixed << std::setprecision(1) << pct << "%) | Rx'd: "
                      << g_stats.totalMsgReceived << " | Completed: " << g_stats.totalMsgCompleted
                      << " | " << sendTime.GetSeconds() << "s" << std::flush;
        }

        ScheduleNextSend();
    }

    Ptr<Socket> m_socket;
    Ipv4Address m_destIp;
    uint16_t m_port;
    bool m_isServer;
    bool m_running;
    uint32_t m_msgSize;
    uint32_t m_numMsgs;
    uint32_t m_messagesSent;
    uint64_t m_targetRateBps;
    uint8_t m_uetMode;
    EventId m_sendEvent;

    UetSesPdsEngine m_engine;
};

// ─────────────────────────────────────────────────────────────────────────────
// REPORTER
// ─────────────────────────────────────────────────────────────────────────────

void PrintResults(uint64_t targetRateGbps, double lossRate, const AggregatedStats& stats,
                  UetStressTestApp* client, UetStressTestApp* target)
{
    std::cout << "\n" << std::string(80, '=') << "\n";
    std::cout << "           UET PROTOCOL NETWORK SIMULATION RESULTS\n";
    std::cout << std::string(80, '=') << "\n\n";

    double achievedGbps = stats.GetThroughputGbps();
    double efficiency = targetRateGbps > 0 ? (achievedGbps / targetRateGbps) * 100.0 : 0;

    std::cout << "=== Throughput Results ===\n"
              << "  Target Rate:         " << targetRateGbps << " Gbps\n"
              << "  Achieved Goodput:    " << std::fixed << std::setprecision(3) << achievedGbps << " Gbps\n"
              << "  Efficiency:          " << std::fixed << std::setprecision(2) << efficiency << "%\n"
              << "  Message Rate:        " << std::fixed << std::setprecision(0) << stats.GetMessageRate() << " msgs/sec\n"
              << "  Injected Loss Rate:  " << std::fixed << std::setprecision(4) << (lossRate * 100.0) << "%\n\n";

    std::cout << "=== Reliability Results ===\n"
              << "  Messages Sent:      " << stats.totalMsgSent << "\n"
              << "  Messages Delivered: " << stats.totalMsgReceived
              << " (" << std::fixed << std::setprecision(2) << stats.GetDeliveryRate() << "%)\n"
              << "  Messages Completed: " << stats.totalMsgCompleted
              << " (" << std::fixed << std::setprecision(2) << stats.GetCompletionRate() << "% ACKed end-to-end)\n"
              << "  Messages Failed:    " << stats.totalMsgFailed << " (retry budget exhausted)\n"
              << "  Bytes Sent:         " << stats.totalBytesSent << "\n"
              << "  Bytes Delivered:    " << stats.totalBytesReceived << "\n\n";

    std::cout << "=== End-to-End Delivery Latency ===\n"
              << "  Average: " << std::fixed << std::setprecision(2) << stats.GetAverageLatencyNs() << " ns\n"
              << "  P50:     " << std::fixed << std::setprecision(2) << stats.GetPercentileNs(0.50) << " ns\n"
              << "  P99:     " << std::fixed << std::setprecision(2) << stats.GetPercentileNs(0.99) << " ns\n"
              << "  Min:     " << std::fixed << std::setprecision(2) << stats.GetMinLatencyNs() << " ns\n"
              << "  Max:     " << std::fixed << std::setprecision(2) << stats.GetMaxLatencyNs() << " ns\n"
              << "  Jitter:  " << std::fixed << std::setprecision(2) << stats.GetJitterNs() << " ns\n\n";

    std::cout << "=== UET Engine Transport Statistics ===\n"
              << std::left << std::setw(25) << "  Metric" << std::setw(15) << "Initiator" << "Target\n"
              << "  " << std::string(50, '-') << "\n"
              << std::setw(25) << "  PDS Packets TX:" << std::setw(15) << client->GetEngine().GetTxPktCount() << target->GetEngine().GetTxPktCount() << "\n"
              << std::setw(25) << "  PDS Packets RX:" << std::setw(15) << client->GetEngine().GetRxPktCount() << target->GetEngine().GetRxPktCount() << "\n"
              << std::setw(25) << "  NACKs Received:" << std::setw(15) << client->GetEngine().GetNackRcvdCount() << target->GetEngine().GetNackRcvdCount() << "\n"
              << std::setw(25) << "  NACKs Sent:" << std::setw(15) << client->GetEngine().GetNackSentCount() << target->GetEngine().GetNackSentCount() << "\n"
              << std::setw(25) << "  Retransmissions:" << std::setw(15) << client->GetEngine().GetRetxCount() << target->GetEngine().GetRetxCount() << "\n"
              << std::setw(25) << "  Msgs Completed:" << std::setw(15) << client->GetEngine().GetMsgsCompleted() << target->GetEngine().GetMsgsCompleted() << "\n"
              << std::setw(25) << "  Msgs Failed:" << std::setw(15) << client->GetEngine().GetMsgsFailed() << target->GetEngine().GetMsgsFailed() << "\n"
              << std::setw(25) << "  Active PDCs:" << std::setw(15) << client->GetEngine().GetActivePdcCount() << target->GetEngine().GetActivePdcCount() << "\n\n";

    std::ofstream pdcLog("pdc_state_dump.log");
    pdcLog << "=== Target (Server) PDC State Machine Details ===\n";
    pdcLog << target->GetEngine().GetPdcStateReport() << "\n";
    pdcLog << "=== Initiator (Client) PDC State Machine Details ===\n";
    pdcLog << client->GetEngine().GetPdcStateReport() << "\n";
    pdcLog.close();

    std::cout << "  [i] Full PDC state details saved to: pdc_state_dump.log\n\n";

    std::cout << "=== Assessment ===\n";
    double completion = stats.GetCompletionRate();
    if (completion >= 99.99)
        std::cout << "  Reliability: PASS - all sent messages were ACKed end-to-end\n";
    else if (completion >= 99.0)
        std::cout << "  Reliability: MARGINAL - " << std::fixed << std::setprecision(2)
                  << completion << "% completion; check retry budget vs loss rate\n";
    else
        std::cout << "  Reliability: FAIL - only " << std::fixed << std::setprecision(2)
                  << completion << "% of messages completed\n";

    if (efficiency >= 90.0) std::cout << "  Throughput:  line-rate class (>90% of target)\n";
    else if (efficiency >= 70.0) std::cout << "  Throughput:  70-90% of target\n";
    else std::cout << "  Throughput:  below 70% of target - overhead or stalls dominate\n";

    std::cout << "\n" << std::string(80, '=') << "\n";
}

int main(int argc, char *argv[])
{
    uint32_t msgSize = 65535;
    uint32_t numMsgs = 10000;
    std::string dataRateStr = "200Gbps";
    std::string delayStr = "100ns";
    std::string modeStr = "RUD";
    uint32_t seed = 1;
    double lossRate = 0.0;
    double simTime = 10.0;

    CommandLine cmd;
    cmd.AddValue("msgSize", "Size of application message (bytes)", msgSize);
    cmd.AddValue("numMsgs", "Number of messages", numMsgs);
    cmd.AddValue("dataRate", "Link data rate (e.g. 200Gbps)", dataRateStr);
    cmd.AddValue("delay", "Link delay (e.g. 100ns)", delayStr);
    cmd.AddValue("mode", "UET Delivery Mode (RUD, ROD, RUDI, UUD)", modeStr);
    cmd.AddValue("seed", "RNG seed (reproducible runs)", seed);
    cmd.AddValue("lossRate", "Per-packet receive loss probability on both devices", lossRate);
    cmd.AddValue("simTime", "Simulation stop time in seconds", simTime);
    cmd.Parse(argc, argv);

    RngSeedManager::SetSeed(seed);
    RngSeedManager::SetRun(1);

    uint8_t devMode = UET_MODE_RUD;
    if (modeStr == "ROD") devMode = UET_MODE_ROD;
    else if (modeStr == "RUDI") devMode = UET_MODE_RUDI;
    else if (modeStr == "UUD") devMode = UET_MODE_UUD;

    std::cout << "Starting UET Point-To-Point Simulation (" << dataRateStr << ", " << delayStr
              << ", Mode=" << modeStr << ", seed=" << seed
              << ", lossRate=" << lossRate << ")\n";

    NodeContainer c;
    c.Create(2);

    PointToPointHelper p2p;
    p2p.SetDeviceAttribute("DataRate", StringValue(dataRateStr));
    p2p.SetChannelAttribute("Delay", StringValue(delayStr));
    p2p.SetDeviceAttribute("Mtu", UintegerValue(9000)); // Jumbo Ethernet

    NetDeviceContainer devices = p2p.Install(c.Get(0), c.Get(1));

    // Optional loss injection to exercise PDS recovery
    if (lossRate > 0.0)
    {
        for (uint32_t i = 0; i < devices.GetN(); ++i)
        {
            Ptr<RateErrorModel> em = CreateObject<RateErrorModel>();
            em->SetAttribute("ErrorRate", DoubleValue(lossRate));
            em->SetAttribute("ErrorUnit", EnumValue(RateErrorModel::ERROR_UNIT_PACKET));
            devices.Get(i)->SetAttribute("ReceiveErrorModel", PointerValue(em));
        }
    }

    InternetStackHelper internet;
    internet.Install(c);

    Ipv4AddressHelper ipv4;
    ipv4.SetBase("10.1.1.0", "255.255.255.0");
    Ipv4InterfaceContainer interfaces = ipv4.Assign(devices);

    uint16_t port = 9000;

    // Target Node (Sink)
    Ptr<UetStressTestApp> targetApp = CreateObject<UetStressTestApp>();
    targetApp->Setup(0, 0, Ipv4Address(), port, true, 0, devMode);
    targetApp->SetStartTime(Seconds(0.0));
    targetApp->SetStopTime(Seconds(simTime));
    c.Get(1)->AddApplication(targetApp);

    // Initiator Node (Sender)
    Ptr<UetStressTestApp> clientApp = CreateObject<UetStressTestApp>();
    uint64_t bps = DataRate(dataRateStr).GetBitRate();
    clientApp->Setup(msgSize, numMsgs, interfaces.GetAddress(1), port, false, bps, devMode);
    clientApp->SetStartTime(Seconds(0.5));
    clientApp->SetStopTime(Seconds(simTime));
    c.Get(0)->AddApplication(clientApp);

    Simulator::Stop(Seconds(simTime));
    Simulator::Run();

    uint64_t rateGbps = DataRate(dataRateStr).GetBitRate() / 1000000000ULL;
    PrintResults(rateGbps, lossRate, g_stats, clientApp.operator->(), targetApp.operator->());

    Simulator::Destroy();
    return 0;
}
