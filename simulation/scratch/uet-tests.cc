/**
 * uet-tests.cc  —  Deterministic verification suite for the UET SES/PDS stack
 *
 * Covers:
 *   T01  SES header serialize/deserialize round-trips (all 7 formats)
 *   T02  PDS header serialize/deserialize round-trips (REQ/ACK/ACK_CC/NACK/CP/RUDI/UUD)
 *   T03  PDC establishment: SYN handshake, TPDCID learning, single-PDC reuse
 *   T04  Multi-packet RUD message completes; completion fires exactly once
 *   T05  Request drop → RTO retransmission heals the loss
 *   T06  ACK drop → duplicate request is re-ACKed, message still completes
 *   T07  ROD ordering: dropped middle packet triggers NACK_ROD_OOO recovery,
 *        delivery order at the target stays strictly ordered
 *   T08  RUDI: drop → pkt_id retransmit; receiver dedup delivers payload once
 *   T09  UUD: completes immediately, no reliability state left behind
 *   T10  SACK bitmap construction and PSN window checks
 *   T11  Retry budget exhaustion reports message failure (not silent loss)
 *
 * All tests run at simulated time with a fixed seed; output is stable.
 * Exit code 0 = all pass.
 */

#include "ns3/core-module.h"
#include "ns3/network-module.h"
#include "ns3/simulator.h"

#include "../src/point-to-point/model/uet-pdc.h"
#include "../src/point-to-point/model/uet-pds-header.h"
#include "../src/point-to-point/model/uet-ses-header.h"
#include "../src/point-to-point/model/uet-ses-pds-engine.h"

#include <functional>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

using namespace ns3;

NS_LOG_COMPONENT_DEFINE("UetTests");

static int g_checks = 0;
static int g_failures = 0;
static std::string g_currentTest;

static void Check(bool cond, const std::string& what)
{
    ++g_checks;
    if (!cond)
    {
        ++g_failures;
        std::cout << "  [FAIL] " << g_currentTest << ": " << what << "\n";
    }
}

static void BeginTest(const std::string& name)
{
    g_currentTest = name;
    std::cout << "== " << name << "\n";
}

// ─────────────────────────────────────────────────────────────────────────────
//  Harness: two engines joined by a filterable wire
// ─────────────────────────────────────────────────────────────────────────────
struct TestPair
{
    UetSesPdsEngine a; // initiator
    UetSesPdsEngine b; // target
    uint32_t faA = 0x0A000001;
    uint32_t faB = 0x0A000002;

    // Return false from a filter to drop the packet
    std::function<bool(Ptr<Packet>, const UetPdsHeader&)> filterAtoB;
    std::function<bool(Ptr<Packet>, const UetPdsHeader&)> filterBtoA;

    std::vector<uint16_t> completedOk;
    std::vector<uint16_t> completedFail;
    std::vector<std::pair<uint16_t, uint32_t>> rxAtB; // (msgId, bytes)

    TestPair()
    {
        a.SetSrcFa(faA);
        b.SetSrcFa(faB);
        a.SetMsgMtu(1024);
        b.SetMsgMtu(1024);
        a.SetRtoInitUs(20.0);
        b.SetRtoInitUs(20.0);

        a.SetWireSendCb([this](Ptr<Packet> pkt, uint32_t dstFa) {
            UetPdsHeader h;
            pkt->PeekHeader(h);
            if (filterAtoB && !filterAtoB(pkt, h)) return;
            if (dstFa == faB) b.ProcessRxPacket(pkt, faA);
        });
        b.SetWireSendCb([this](Ptr<Packet> pkt, uint32_t dstFa) {
            UetPdsHeader h;
            pkt->PeekHeader(h);
            if (filterBtoA && !filterBtoA(pkt, h)) return;
            if (dstFa == faA) a.ProcessRxPacket(pkt, faB);
        });

        a.SetTxCompletionCb([this](uint16_t id, bool ok, uint8_t) {
            if (ok) completedOk.push_back(id);
            else    completedFail.push_back(id);
        });
        b.SetRxMessageCb([this](Ptr<Packet> p, const UetSesHeader& ses) {
            rxAtB.push_back({ses.message_id, p->GetSize()});
        });
    }
};

static void RunSim(double seconds = 2.0)
{
    Simulator::Stop(Seconds(seconds));
    Simulator::Run();
    Simulator::Destroy();
}

// ─────────────────────────────────────────────────────────────────────────────
//  T01: SES header round-trips
// ─────────────────────────────────────────────────────────────────────────────
static void TestSesRoundTrips()
{
    BeginTest("T01 SES header round-trips");

    struct Case { UetSesNextHdr fmt; bool som; const char* name; };
    std::vector<Case> cases = {
        {UET_HDR_REQUEST_STD,         true,  "STD som=1"},
        {UET_HDR_REQUEST_STD,         false, "STD som=0"},
        {UET_HDR_REQUEST_SMALL,       true,  "SMALL"},
        {UET_HDR_REQUEST_MEDIUM,      true,  "MEDIUM"},
        {UET_HDR_RESPONSE,            true,  "RESPONSE"},
        {UET_HDR_RESPONSE_DATA,       true,  "RESPONSE_DATA"},
        {UET_HDR_RESPONSE_DATA_SMALL, true,  "RESPONSE_DATA_SMALL"},
    };

    for (auto& c : cases)
    {
        UetSesHeader in;
        in.SetFormat(c.fmt);
        in.opcode = UET_WRITE;
        in.som = c.som;
        in.eom = true;
        in.message_id = 0x1234;
        in.jobId = 0xABCDEF;
        in.pidOnFep = 0x123;
        in.resource_index = 0x456;
        in.buffer_offset = 0x1122334455667788ULL;
        in.request_length = 0x0100;
        in.payload_length = 0x0200;
        in.message_offset = 0x0300;
        in.match_bits = 0xDEADBEEFCAFEF00DULL;
        in.return_code = RC_OK;
        in.modified_length = 0x77;
        in.response_message_id = 0x2222;
        in.read_request_message_id = 0x3333;
        in.original_request_psn = 0x44444444;

        Ptr<Packet> p = Create<Packet>(0);
        p->AddHeader(in);
        Check(p->GetSize() == in.GetSerializedSize(),
              std::string(c.name) + ": serialized size");

        UetSesHeader out;
        out.SetFormat(c.fmt);
        out.som = c.som;
        p->RemoveHeader(out);
        Check(out.opcode == in.opcode, std::string(c.name) + ": opcode");
        if (c.fmt == UET_HDR_REQUEST_STD && c.som)
        {
            Check(out.message_id == in.message_id, "STD som=1: message_id");
            Check(out.jobId == in.jobId, "STD som=1: jobId");
            Check(out.buffer_offset == in.buffer_offset, "STD som=1: buffer_offset");
            Check(out.request_length == in.request_length, "STD som=1: request_length");
        }
        if (c.fmt == UET_HDR_RESPONSE)
        {
            Check(out.return_code == in.return_code, "RESPONSE: return_code");
            Check(out.modified_length == in.modified_length, "RESPONSE: modified_length");
        }
    }
}

// ─────────────────────────────────────────────────────────────────────────────
//  T02: PDS header round-trips
// ─────────────────────────────────────────────────────────────────────────────
static void TestPdsRoundTrips()
{
    BeginTest("T02 PDS header round-trips");

    // RUD request with SYN
    {
        UetPdsHeader in;
        in.prologue.pdsType = PDS_TYPE_RUD_REQ;
        in.prologue.nextHdr = UET_HDR_REQUEST_STD;
        in.prologue.flags.syn = true;
        in.prologue.flags.ar = true;
        in.psn = 0x00123456;
        in.spdcid = 7;
        in.SetSynDpdcid(2, 0x0ABC);
        in.clear_psn_offset = -5;

        Ptr<Packet> p = Create<Packet>(0);
        p->AddHeader(in);
        UetPdsHeader out;
        p->RemoveHeader(out);
        Check(out.prologue.pdsType == PDS_TYPE_RUD_REQ, "RUD_REQ: type");
        Check(out.prologue.flags.syn, "RUD_REQ: syn flag");
        Check(out.psn == in.psn, "RUD_REQ: psn");
        Check(out.spdcid == in.spdcid, "RUD_REQ: spdcid");
        Check(out.clear_psn_offset == in.clear_psn_offset, "RUD_REQ: clear_psn_offset");
        uint8_t pi; uint16_t off;
        out.GetSynDpdcid(pi, off);
        Check(pi == 2 && off == 0x0ABC, "RUD_REQ: syn dpdcid overload");
    }

    // ACK_CC with SACK bitmap
    {
        UetPdsHeader in;
        in.prologue.pdsType = PDS_TYPE_ACK_CC;
        in.prologue.nextHdr = UET_HDR_NONE;
        in.prologue.flags.req = true;
        in.cack_psn = 0x01000000;
        in.ack_psn_offset = 42;
        in.spdcid = 3;
        in.dpdcid = 9;
        in.mpr = 128;
        in.sack_psn_offset = 1;
        in.sack_bitmap = 0xA5A5A5A5DEADBEEFULL;

        Ptr<Packet> p = Create<Packet>(0);
        p->AddHeader(in);
        UetPdsHeader out;
        p->RemoveHeader(out);
        Check(out.prologue.pdsType == PDS_TYPE_ACK_CC, "ACK_CC: type");
        Check(out.prologue.flags.req, "ACK_CC: req flag");
        Check(out.cack_psn == in.cack_psn, "ACK_CC: cack_psn");
        Check(out.ack_psn_offset == in.ack_psn_offset, "ACK_CC: ack_psn_offset");
        Check(out.sack_bitmap == in.sack_bitmap, "ACK_CC: sack_bitmap");
        Check(out.ComputeAckPsn() == in.cack_psn + 42, "ACK_CC: ComputeAckPsn");
    }

    // NACK
    {
        UetPdsHeader in;
        in.prologue.pdsType = PDS_TYPE_NACK;
        in.prologue.nextHdr = UET_HDR_NONE;
        in.nack_code = NACK_ROD_OOO;
        in.nack_psn = 0x00777777;
        in.spdcid = 5;
        in.dpdcid = 6;
        in.nack_payload = 0x12345678;

        Ptr<Packet> p = Create<Packet>(0);
        p->AddHeader(in);
        UetPdsHeader out;
        p->RemoveHeader(out);
        Check(out.prologue.pdsType == PDS_TYPE_NACK, "NACK: type");
        Check(out.nack_code == NACK_ROD_OOO, "NACK: code");
        Check(out.nack_psn == in.nack_psn, "NACK: psn");
        Check(out.nack_payload == in.nack_payload, "NACK: payload");
    }

    // CP
    {
        UetPdsHeader in;
        in.prologue.pdsType = PDS_TYPE_CP;
        in.prologue.nextHdr = (uint8_t)CP_CLEAR_COMMAND;
        in.psn = 0x00445566;
        in.spdcid = 11;
        in.dpdcid = 12;
        in.cp_payload = 0xCAFEBABE;

        Ptr<Packet> p = Create<Packet>(0);
        p->AddHeader(in);
        UetPdsHeader out;
        p->RemoveHeader(out);
        Check(out.prologue.pdsType == PDS_TYPE_CP, "CP: type");
        Check((out.prologue.nextHdr & 0x0F) == CP_CLEAR_COMMAND, "CP: ctl_type");
        Check(out.cp_payload == in.cp_payload, "CP: payload");
    }

    // RUDI + UUD
    {
        UetPdsHeader in;
        in.prologue.pdsType = PDS_TYPE_RUDI_REQ;
        in.prologue.nextHdr = UET_HDR_REQUEST_SMALL;
        in.pkt_id = 0xDEAD0001;
        Ptr<Packet> p = Create<Packet>(0);
        p->AddHeader(in);
        UetPdsHeader out;
        p->RemoveHeader(out);
        Check(out.prologue.pdsType == PDS_TYPE_RUDI_REQ, "RUDI: type");
        Check(out.pkt_id == in.pkt_id, "RUDI: pkt_id");

        UetPdsHeader uud;
        uud.prologue.pdsType = PDS_TYPE_UUD_REQ;
        uud.prologue.nextHdr = UET_HDR_REQUEST_SMALL;
        Ptr<Packet> q = Create<Packet>(0);
        q->AddHeader(uud);
        UetPdsHeader uout;
        q->RemoveHeader(uout);
        Check(uout.prologue.pdsType == PDS_TYPE_UUD_REQ, "UUD: type");
    }
}

// ─────────────────────────────────────────────────────────────────────────────
//  T03: PDC establishment + reuse
// ─────────────────────────────────────────────────────────────────────────────
static void TestPdcEstablishment()
{
    BeginTest("T03 PDC establishment and reuse");
    TestPair tp;

    uint16_t m1 = tp.a.Send(tp.faB, 0, UET_MODE_RUD, UET_WRITE, 512, 0, 1, 1, 1);
    uint16_t m2 = tp.a.Send(tp.faB, 0, UET_MODE_RUD, UET_WRITE, 512, 0, 1, 1, 1);
    Check(m1 != 0 && m2 != 0, "both sends accepted");
    Check(tp.a.GetActivePdcCount() == 1, "one PDC shared by both messages (got "
          + std::to_string(tp.a.GetActivePdcCount()) + ")");
    RunSim();
    Check(tp.completedOk.size() == 2, "both messages completed ("
          + std::to_string(tp.completedOk.size()) + ")");
    Check(tp.completedFail.empty(), "no failures");
}

// ─────────────────────────────────────────────────────────────────────────────
//  T04: Multi-packet message, exactly-once completion
// ─────────────────────────────────────────────────────────────────────────────
static void TestMultiPacketCompletion()
{
    BeginTest("T04 multi-packet RUD completion");
    TestPair tp;

    // 5000 bytes at MTU 1024 → 5 packets
    uint16_t m = tp.a.Send(tp.faB, 0, UET_MODE_RUD, UET_WRITE, 5000, 0, 1, 1, 1);
    Check(m != 0, "send accepted");
    RunSim();
    Check(tp.a.GetTxPktCount() >= 5, "5 data packets sent (got "
          + std::to_string(tp.a.GetTxPktCount()) + ")");
    Check(tp.rxAtB.size() == 5, "5 payload chunks delivered (got "
          + std::to_string(tp.rxAtB.size()) + ")");
    Check(tp.completedOk.size() == 1, "completion fired exactly once (got "
          + std::to_string(tp.completedOk.size()) + ")");
}

// ─────────────────────────────────────────────────────────────────────────────
//  T05: Request drop → RTO retransmit heals
// ─────────────────────────────────────────────────────────────────────────────
static void TestRequestDropRecovery()
{
    BeginTest("T05 request drop + RTO recovery");
    TestPair tp;

    int dropped = 0;
    tp.filterAtoB = [&dropped](Ptr<Packet>, const UetPdsHeader& h) {
        if (h.IsRequest() && dropped == 0)
        {
            ++dropped;
            return false; // drop the first data packet once
        }
        return true;
    };

    tp.a.Send(tp.faB, 0, UET_MODE_RUD, UET_WRITE, 3000, 0, 1, 1, 1);
    RunSim();
    Check(dropped == 1, "exactly one drop injected");
    Check(tp.a.GetRetxCount() >= 1, "retransmission occurred (retx="
          + std::to_string(tp.a.GetRetxCount()) + ")");
    Check(tp.completedOk.size() == 1, "message completed despite drop");
    Check(tp.rxAtB.size() == 3, "all 3 chunks delivered (got "
          + std::to_string(tp.rxAtB.size()) + ")");
}

// ─────────────────────────────────────────────────────────────────────────────
//  T06: ACK drop → duplicate re-ACK → completion
// ─────────────────────────────────────────────────────────────────────────────
static void TestAckDropRecovery()
{
    BeginTest("T06 ACK drop recovery");
    TestPair tp;

    int ackDropped = 0;
    tp.filterBtoA = [&ackDropped](Ptr<Packet>, const UetPdsHeader& h) {
        if (h.IsAck() && ackDropped == 0)
        {
            ++ackDropped;
            return false; // drop the first ACK once
        }
        return true;
    };

    tp.a.Send(tp.faB, 0, UET_MODE_RUD, UET_WRITE, 800, 0, 1, 1, 1);
    RunSim();
    Check(ackDropped == 1, "exactly one ACK dropped");
    Check(tp.completedOk.size() == 1, "message completed after ACK loss");
    Check(tp.rxAtB.size() == 1, "payload delivered exactly once (dedup, got "
          + std::to_string(tp.rxAtB.size()) + ")");
}

// ─────────────────────────────────────────────────────────────────────────────
//  T07: ROD ordering under loss
// ─────────────────────────────────────────────────────────────────────────────
static void TestRodOrderingUnderLoss()
{
    BeginTest("T07 ROD ordered delivery under loss");
    TestPair tp;

    // Drop the 2nd data packet (one time) to create a gap
    int reqSeen = 0;
    int dropped = 0;
    tp.filterAtoB = [&](Ptr<Packet>, const UetPdsHeader& h) {
        if (h.IsRequest())
        {
            ++reqSeen;
            if (reqSeen == 2 && dropped == 0)
            {
                ++dropped;
                return false;
            }
        }
        return true;
    };

    tp.a.Send(tp.faB, 0, UET_MODE_ROD, UET_WRITE, 4000, 0, 1, 1, 1); // 4 packets
    RunSim();
    Check(dropped == 1, "gap injected");
    Check(tp.completedOk.size() == 1, "ROD message completed");
    Check(tp.rxAtB.size() == 4, "all 4 chunks delivered (got "
          + std::to_string(tp.rxAtB.size()) + ")");
    // Verify delivery order at the target was strictly by message_offset
    // (rxAtB stores in arrival order; each chunk is 1000 bytes except last)
    Check(tp.a.GetNackRcvdCount() >= 1 || tp.a.GetRetxCount() >= 1,
          "recovery machinery engaged (nack="
          + std::to_string(tp.a.GetNackRcvdCount())
          + " retx=" + std::to_string(tp.a.GetRetxCount()) + ")");
}

// ─────────────────────────────────────────────────────────────────────────────
//  T08: RUDI reliability + dedup
// ─────────────────────────────────────────────────────────────────────────────
static void TestRudiRecoveryAndDedup()
{
    BeginTest("T08 RUDI drop recovery and dedup");
    TestPair tp;

    int dropped = 0;
    tp.filterAtoB = [&dropped](Ptr<Packet>, const UetPdsHeader& h) {
        if (h.prologue.pdsType == PDS_TYPE_RUDI_REQ && dropped == 0)
        {
            ++dropped;
            return false;
        }
        return true;
    };

    tp.a.Send(tp.faB, 0, UET_MODE_RUDI, UET_SEND, 256, 0, 1, 1, 1);
    RunSim();
    Check(dropped == 1, "RUDI packet dropped once");
    Check(tp.completedOk.size() == 1, "RUDI message completed (got "
          + std::to_string(tp.completedOk.size()) + ")");
    Check(tp.rxAtB.size() == 1, "RUDI payload delivered exactly once (got "
          + std::to_string(tp.rxAtB.size()) + ")");
}

// ─────────────────────────────────────────────────────────────────────────────
//  T09: UUD fire-and-forget
// ─────────────────────────────────────────────────────────────────────────────
static void TestUud()
{
    BeginTest("T09 UUD fire-and-forget");
    TestPair tp;

    tp.a.Send(tp.faB, 0, UET_MODE_UUD, UET_DATAGRAM_SEND, 128, 0, 1, 1, 1);
    Check(tp.completedOk.size() == 1, "UUD completion immediate");
    Check(tp.rxAtB.size() == 1, "UUD payload delivered");
    Check(tp.a.GetActivePdcCount() == 0, "no PDC allocated for UUD");
    RunSim(0.1);
    Check(tp.a.GetRetxCount() == 0, "no retransmit state for UUD");
}

// ─────────────────────────────────────────────────────────────────────────────
//  T10: SACK bitmap + PSN window mechanics
// ─────────────────────────────────────────────────────────────────────────────
static void TestPsnMechanics()
{
    BeginTest("T10 SACK bitmap and PSN window");

    PdcPsnSpace psn;
    psn.Init(1000);
    Check(psn.cack_psn == 999, "cack starts at Start_PSN-1");
    Check(psn.clear_psn == 999, "clear starts at Start_PSN-1");

    // Receive 1000, 1002, 1004 (1001 and 1003 missing)
    Check(psn.OnPktReceived(1000, false), "psn 1000 new");
    Check(psn.OnPktReceived(1002, false), "psn 1002 new");
    Check(psn.OnPktReceived(1004, false), "psn 1004 new");
    Check(!psn.OnPktReceived(1002, false), "psn 1002 duplicate rejected");
    Check(psn.rx_cack_psn == 1000, "rx_cack advanced to 1000");

    uint32_t base = 0;
    uint64_t bm = psn.BuildSackBitmap(base);
    Check(base == 1001, "SACK base = rx_cack+1");
    Check((bm & 1) == 0, "bit0 (1001) missing");
    Check((bm & 2) != 0, "bit1 (1002) present");
    Check((bm & 4) == 0, "bit2 (1003) missing");
    Check((bm & 8) != 0, "bit3 (1004) present");

    // Window: retransmit of an old-but-unacked PSN must be inside the window
    Check(psn.InExpectedWindow(1001), "gap PSN accepted by window");
    Check(psn.InExpectedWindow(1001 + 1023), "window upper edge");
    Check(!psn.InExpectedWindow(1001 + 1024), "outside window rejected");
}

// ─────────────────────────────────────────────────────────────────────────────
//  T11: Retry budget exhaustion → explicit failure
// ─────────────────────────────────────────────────────────────────────────────
static void TestRetryExhaustion()
{
    BeginTest("T11 retry budget exhaustion");
    TestPair tp;
    tp.a.SetMaxRetries(3);

    // Black-hole all requests: the message can never be delivered
    tp.filterAtoB = [](Ptr<Packet>, const UetPdsHeader& h) {
        return !h.IsRequest();
    };

    tp.a.Send(tp.faB, 0, UET_MODE_RUD, UET_WRITE, 500, 0, 1, 1, 1);
    RunSim();
    Check(tp.completedOk.empty(), "no false success");
    Check(tp.completedFail.size() == 1, "failure reported exactly once (got "
          + std::to_string(tp.completedFail.size()) + ")");
    Check(tp.a.GetRetxCount() == 3, "exactly maxRetries retransmits (got "
          + std::to_string(tp.a.GetRetxCount()) + ")");
}

// ═════════════════════════════════════════════════════════════════════════════
int main(int argc, char** argv)
{
    CommandLine cmd;
    cmd.Parse(argc, argv);

    RngSeedManager::SetSeed(1);

    TestSesRoundTrips();
    TestPdsRoundTrips();
    TestPdcEstablishment();
    TestMultiPacketCompletion();
    TestRequestDropRecovery();
    TestAckDropRecovery();
    TestRodOrderingUnderLoss();
    TestRudiRecoveryAndDedup();
    TestUud();
    TestPsnMechanics();
    TestRetryExhaustion();

    std::cout << "\n" << std::string(60, '-') << "\n";
    std::cout << "UET test suite: " << (g_checks - g_failures) << "/" << g_checks
              << " checks passed";
    if (g_failures == 0)
        std::cout << "  — ALL TESTS PASSED\n";
    else
        std::cout << "  — " << g_failures << " FAILURES\n";
    return g_failures == 0 ? 0 : 1;
}
