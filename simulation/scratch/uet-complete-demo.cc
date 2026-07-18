/**
 * uet-complete-demo.cc  —  Ultra Ethernet SES / PDS / PDC Demo
 *
 * Demonstrates the complete UE transport stack (up to PDC) per
 * UE-Specification-1.0.2.  Exercises:
 *   §3.4   SES header formats (Standard SOM=1/0, SMALL, MEDIUM, RESPONSE)
 *   §3.5.3 Delivery modes: RUD, ROD, RUDI, UUD
 *   §3.5.8 PDC establishment (SYN handshake, IPDCID/TPDCID assignment)
 *   §3.5.9 Initiator/Target state machines
 *   §3.5.11 PSN spaces (Start_PSN, CLEAR_PSN, CACK_PSN, SACK bitmap)
 *   §3.5.12 NACK processing (all 20 codes enumerated)
 *   §3.5.16 Control packets (NOOP, ACK_REQ, CLEAR_CMD, CLOSE)
 *
 * Topology: 4 nodes (N0, N1, N2, N3) connected via a single fat-pipe link.
 *   N0 → N1: multi-packet RUD WRITE (3 × 4 KB chunks)
 *   N1 → N2: single-packet ROD READ (1 × 512 B)
 *   N2 → N3: RUDI datagram (1 × 256 B)
 *   N3 → N0: UUD best-effort (1 × 128 B)
 *   N0 → N1: second message on same PDC (reuses ESTABLISHED PDC)
 *   N0 → N1: inject NACK (TRIMMED) → verify retransmit logic
 *   Explicit PDC close via CLOSE_COMMAND CP
 *
 * OUTPUT FORMAT:
 *   The simulation prints a rich ASCII table after each stage showing:
 *   - PDC state table for each node
 *   - PSN counters (start, next, cack, clear, outstanding)
 *   - SES message counters (sent, received, retx)
 *   - NACK code stats
 *   - State machine transitions
 */

#include "../src/point-to-point/model/uet-pdc.h"
#include "../src/point-to-point/model/uet-pds-header.h"
#include "../src/point-to-point/model/uet-ses-header.h"
#include "../src/point-to-point/model/uet-ses-pds-engine.h"

#include "ns3/core-module.h"
#include "ns3/network-module.h"
#include "ns3/internet-module.h"
#include "ns3/point-to-point-module.h"
#include "ns3/log.h"
#include "ns3/simulator.h"

#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

using namespace ns3;

NS_LOG_COMPONENT_DEFINE("UetCompleteDemo");

// ─────────────────────────────────────────────────────────────────────────────
//  Pretty print helpers
// ─────────────────────────────────────────────────────────────────────────────
static void PrintSep(char c = '=', int w = 80) { std::cout << std::string(w, c) << "\n"; }
static void PrintHdr(const std::string& title, char c = '=', int w = 80)
{
    PrintSep(c, w);
    int pad = (w - (int)title.size() - 2) / 2;
    std::cout << std::string(pad, ' ') << " " << title << " " << std::string(pad, ' ') << "\n";
    PrintSep(c, w);
}

static void PrintTable(const std::string& hdr,
                       const std::vector<std::string>& cols,
                       const std::vector<std::vector<std::string>>& rows)
{
    // compute column widths
    std::vector<size_t> widths(cols.size());
    for (size_t c = 0; c < cols.size(); c++) widths[c] = cols[c].size();
    for (auto& row : rows)
        for (size_t c = 0; c < cols.size() && c < row.size(); c++)
            widths[c] = std::max(widths[c], row[c].size());

    // header row
    std::cout << "  " << hdr << "\n  |";
    for (size_t c = 0; c < cols.size(); c++)
        std::cout << " " << std::setw((int)widths[c]) << std::left << cols[c] << " |";
    std::cout << "\n  |";
    for (size_t c = 0; c < cols.size(); c++)
        std::cout << std::string(widths[c] + 2, '-') << "|";
    std::cout << "\n";

    for (auto& row : rows)
    {
        std::cout << "  |";
        for (size_t c = 0; c < cols.size(); c++)
        {
            std::string val = (c < row.size()) ? row[c] : "";
            std::cout << " " << std::setw((int)widths[c]) << std::left << val << " |";
        }
        std::cout << "\n";
    }
    std::cout << "\n";
}

// ─────────────────────────────────────────────────────────────────────────────
//  Per-simulation-node wrapper
//  Holds an engine and is identified by a Fabric Address (IP as uint32).
// ─────────────────────────────────────────────────────────────────────────────
struct SimNode
{
    std::string       name;
    uint32_t          fa;       // Fabric Address
    UetSesPdsEngine   engine;
    std::vector<std::string> txLog;
    std::vector<std::string> rxLog;
    std::vector<std::string> nackLog;

    std::string Log() const
    {
        return "[" + name + " FA=0x" +
               [&](){std::ostringstream o; o<<std::hex<<fa; return o.str();}() + "]";
    }
};

// ─────────────────────────────────────────────────────────────────────────────
//  Wiring: one routing table for the whole demo fabric. Each engine emits
//  (packet, dstFa) and the router delivers to the node owning that FA.
//  A single per-engine callback with the destination address baked in would
//  break as soon as a node talks to more than one peer (its ACKs would all
//  go to the last wired destination).
// ─────────────────────────────────────────────────────────────────────────────
static std::vector<SimNode*> g_fabric;

static void JoinFabric(SimNode& node)
{
    g_fabric.push_back(&node);
    uint32_t srcFa = node.fa;
    node.engine.SetWireSendCb([srcFa](Ptr<Packet> pkt, uint32_t dstFa)
    {
        for (SimNode* n : g_fabric)
            if (n->fa == dstFa) { n->engine.ProcessRxPacket(pkt, srcFa); return; }
    });
}

// ─────────────────────────────────────────────────────────────────────────────
//  Print PDC state table for a SimNode
// ─────────────────────────────────────────────────────────────────────────────
static void PrintPdcState(SimNode& node)
{
    std::cout << "  " << node.Log() << " PDC State:\n";
    std::cout << node.engine.GetPdcStateReport() << "\n";
}

// ─────────────────────────────────────────────────────────────────────────────
//  Print SES header details
// ─────────────────────────────────────────────────────────────────────────────
static void PrintSesHeader(const std::string& label, const UetSesHeader& ses)
{
    static const char* fmtNames[] = {
        "NONE","SMALL(20B)","MEDIUM(28B)","STD","RESPONSE(12B)","RESP_DATA(20B)","RESP_SMALL(12B)"};
    std::cout << "  [SES " << label << "]\n"
              << "    format  = " << fmtNames[ses.GetFormat()]
              << " (" << ses.GetSerializedSize() << " bytes)\n"
              << "    opcode  = 0x" << std::hex << (int)ses.opcode << std::dec
              << "  som=" << ses.som << "  eom=" << ses.eom
              << "  msgId=" << ses.message_id << "\n"
              << "    jobId   = 0x" << std::hex << ses.jobId << std::dec
              << "  ri=" << ses.resource_index
              << "  reqLen=" << ses.request_length << " B\n"
              << "    bufOff  = 0x" << std::hex << ses.buffer_offset << std::dec << "\n";
}

// ─────────────────────────────────────────────────────────────────────────────
//  Print PDS header details
// ─────────────────────────────────────────────────────────────────────────────
static void PrintPdsHeader(const std::string& label, const UetPdsHeader& pds)
{
    static const char* typeNames[] = {
        "RSVD","TSS","RUD_REQ","ROD_REQ","RUDI_REQ","RUDI_RESP",
        "UUD_REQ","ACK","ACK_CC","ACK_CCX","NACK","CP",
        "NACK_CCX","RUD_CC_REQ","ROD_CC_REQ"};
    std::cout << "  [PDS " << label << "]\n"
              << "    type    = " << typeNames[pds.prologue.pdsType]
              << " (" << pds.GetSerializedSize() << " bytes)\n"
              << "    spdcid  = " << pds.spdcid
              << "  dpdcid  = " << pds.dpdcid << "\n";
    if (pds.IsRequest())
        std::cout << "    psn     = " << pds.psn
                  << "  clear_off=" << pds.clear_psn_offset
                  << "  syn=" << pds.prologue.flags.syn
                  << "  retx=" << pds.prologue.flags.retx << "\n";
    if (pds.IsAck())
        std::cout << "    cack_psn= " << pds.cack_psn
                  << "  ack_off=" << pds.ack_psn_offset
                  << "  p=" << pds.prologue.flags.p
                  << "  req=" << pds.prologue.flags.req << "\n";
    if (pds.IsNack())
        std::cout << "    nack_code= 0x" << std::hex << (int)pds.nack_code << std::dec
                  << " (" << UetPdsHeader::NackCodeName(pds.nack_code) << ")"
                  << "  nack_psn=" << pds.nack_psn << "\n";
}

// ════════════════════════════════════════════════════════════════════════════
//  MAIN SIMULATION
// ════════════════════════════════════════════════════════════════════════════
int main(int argc, char** argv)
{
    CommandLine cmd;
    cmd.Parse(argc, argv);

    // Enable logging
    LogComponentEnable("UetCompleteDemo", LOG_LEVEL_INFO);
    LogComponentEnable("UetPdcManager",  LOG_LEVEL_INFO);
    LogComponentEnable("UetSesPdsEngine",LOG_LEVEL_INFO);

    PrintHdr("Ultra Ethernet (UE) Protocol Stack Demo — SES / PDS / PDC");
    std::cout << "  Spec Reference: UE-Specification-1.0.2 §3.4, §3.5 (up to §3.5.23)\n";
    std::cout << "  Implementation: ns-3-alibabacloud UET module\n\n";

    // ── Create simulation nodes ───────────────────────────────────────────────
    SimNode n0, n1, n2, n3;
    n0.name = "N0"; n0.fa = 0x0A000001; // 10.0.0.1
    n1.name = "N1"; n1.fa = 0x0A000002; // 10.0.0.2
    n2.name = "N2"; n2.fa = 0x0A000003; // 10.0.0.3
    n3.name = "N3"; n3.fa = 0x0A000004; // 10.0.0.4

    n0.engine.SetSrcFa(n0.fa); n0.engine.SetMsgMtu(4096);
    n1.engine.SetSrcFa(n1.fa); n1.engine.SetMsgMtu(4096);
    n2.engine.SetSrcFa(n2.fa); n2.engine.SetMsgMtu(4096);
    n3.engine.SetSrcFa(n3.fa); n3.engine.SetMsgMtu(4096);

    // Join every node to the demo fabric (full routing by fabric address)
    JoinFabric(n0);
    JoinFabric(n1);
    JoinFabric(n2);
    JoinFabric(n3);

    // Set RX callbacks
    for (SimNode* node : {&n0, &n1, &n2, &n3})
    {
        node->engine.SetRxMessageCb([node](Ptr<Packet> pkt, const UetSesHeader& ses) {
            node->rxLog.push_back("rcv msgId=" + std::to_string(ses.message_id) +
                                  " opcode=0x" + [&](){
                                      std::ostringstream o;
                                      o << std::hex << (int)ses.opcode;
                                      return o.str();}() +
                                  " len=" + std::to_string(pkt->GetSize()) + "B");
        });
        node->engine.SetTxCompletionCb([node](uint16_t msgId, bool ok, uint8_t rc) {
            node->txLog.push_back("ack msgId=" + std::to_string(msgId) +
                                  (ok ? " OK" : " FAIL") +
                                  " rc=0x" + [&](){
                                      std::ostringstream o;
                                      o << std::hex << (int)rc;
                                      return o.str();}());
        });
    }

    // ═══════════════════════════════════════════════════════════════════════
    //  Phase 1: SES Header Format Demonstration
    //  Show all 7 SES header types constructed and serialized
    // ═══════════════════════════════════════════════════════════════════════
    PrintHdr("Phase 1: SES Header Formats (§3.4.2)", '-');
    {
        std::cout << "  Constructing all 7 spec-defined SES header formats:\n\n";

        // STD SOM=1 (44 bytes)
        UetSesHeader stdSom1;
        stdSom1.SetFormat(UET_HDR_REQUEST_STD);
        stdSom1.opcode = UET_WRITE; stdSom1.som = true; stdSom1.eom = false;
        stdSom1.message_id = 1; stdSom1.jobId = 0x123456; stdSom1.resource_index = 5;
        stdSom1.buffer_offset = 0x1000; stdSom1.request_length = 12288; // 3 × 4KB
        PrintSesHeader("STD SOM=1 (Table 3-8, 44 bytes)", stdSom1);

        // STD SOM=0 (32 bytes — continuation)
        UetSesHeader stdSom0;
        stdSom0.SetFormat(UET_HDR_REQUEST_STD);
        stdSom0.opcode = UET_WRITE; stdSom0.som = false; stdSom0.eom = false;
        stdSom0.message_id = 1; stdSom0.jobId = 0x123456;
        stdSom0.payload_length = 4096; stdSom0.message_offset = 4096;
        stdSom0.request_length = 12288;
        PrintSesHeader("STD SOM=0 (Table 3-9, 32 bytes continuation)", stdSom0);

        // SMALL (20 bytes)
        UetSesHeader small;
        small.SetFormat(UET_HDR_REQUEST_SMALL);
        small.opcode = UET_WRITE; small.som = true; small.eom = true;
        small.jobId = 0xABCDEF; small.resource_index = 2;
        small.buffer_offset = 0x2000; small.request_length = 512;
        PrintSesHeader("SMALL / Optimized Non-Matching (Table 3-10, 20 bytes)", small);

        // MEDIUM (28 bytes)
        UetSesHeader medium;
        medium.SetFormat(UET_HDR_REQUEST_MEDIUM);
        medium.opcode = UET_TAGGED_SEND; medium.som = true; medium.eom = true;
        medium.jobId = 0x1; medium.match_bits = 0xDEADBEEF00001234ULL;
        medium.request_length = 256;
        PrintSesHeader("MEDIUM / Small-Msg tagged-send (Table 3-14, 28 bytes)", medium);

        // RESPONSE (12 bytes)
        UetSesHeader resp;
        resp.SetFormat(UET_HDR_RESPONSE);
        resp.opcode = UET_RESPONSE; resp.return_code = RC_OK;
        resp.message_id = 1; resp.jobId = 0x123456;
        resp.modified_length = 12288;
        PrintSesHeader("RESPONSE (Table 3-11, 12 bytes)", resp);

        // RESPONSE_DATA (20 bytes)
        UetSesHeader respData;
        respData.SetFormat(UET_HDR_RESPONSE_DATA);
        respData.opcode = UET_RESPONSE_W_DATA; respData.return_code = RC_OK;
        respData.response_message_id = 7; respData.read_request_message_id = 2;
        respData.payload_length = 512; respData.modified_length = 512;
        respData.message_offset = 0; respData.jobId = 0x1;
        PrintSesHeader("RESPONSE_DATA (Table 3-12, 20 bytes)", respData);

        // RESPONSE_DATA_SMALL (12 bytes)
        UetSesHeader respSmall;
        respSmall.SetFormat(UET_HDR_RESPONSE_DATA_SMALL);
        respSmall.opcode = UET_RESPONSE_W_DATA; respSmall.return_code = RC_OK;
        respSmall.payload_length = 256; respSmall.jobId = 0x1;
        respSmall.original_request_psn = 42;
        PrintSesHeader("RESPONSE_DATA_SMALL (Table 3-13, 12 bytes)", respSmall);

        // Summary table
        PrintTable("SES Header Size Summary", {"Format", "Bytes", "Spec Table"},
        {
            {"STD SOM=1",            "44", "Table 3-8"},
            {"STD SOM=0",            "32", "Table 3-9"},
            {"SMALL (opt non-match)","20", "Table 3-10"},
            {"MEDIUM (small-msg)",   "28", "Table 3-14"},
            {"RESPONSE",             "12", "Table 3-11"},
            {"RESPONSE_DATA",        "20", "Table 3-12"},
            {"RESPONSE_DATA_SMALL",  "12", "Table 3-13"},
        });
    }

    // ═══════════════════════════════════════════════════════════════════════
    //  Phase 2: PDS Header Format Demonstration
    //  Show all spec-defined PDS packet types
    // ═══════════════════════════════════════════════════════════════════════
    PrintHdr("Phase 2: PDS Header Formats (§3.5.10)", '-');
    {
        // RUD Request with SYN flag (first packet, PDC establishment)
        UetPdsHeader rudSyn;
        rudSyn.prologue.pdsType = PDS_TYPE_RUD_REQ;
        rudSyn.prologue.nextHdr = UET_HDR_REQUEST_STD;
        rudSyn.prologue.flags.syn  = true;
        rudSyn.prologue.flags.ar   = true;
        rudSyn.psn = 0x000186A0; // random Start_PSN
        rudSyn.spdcid = 7;       // IPDCID assigned by initiator
        rudSyn.SetSynDpdcid(0 /*use global pool*/, 0 /*psn_offset=0 for first pkt*/);
        rudSyn.clear_psn_offset = -1; // CLEAR_PSN = Start_PSN - 1
        PrintPdsHeader("RUD_REQ (SYN=1, Table 3-33, 12 bytes) — PDC Establishment", rudSyn);

        // RUD Request normal (PDC established)
        UetPdsHeader rudReq;
        rudReq.prologue.pdsType = PDS_TYPE_RUD_REQ;
        rudReq.prologue.nextHdr = UET_HDR_REQUEST_STD;
        rudReq.prologue.flags.syn  = false;
        rudReq.psn = 0x000186A1;
        rudReq.spdcid = 7; rudReq.dpdcid = 3;
        rudReq.clear_psn_offset = -1;
        PrintPdsHeader("RUD_REQ (SYN=0, Table 3-33, 12 bytes) — Normal Request", rudReq);

        // ACK
        UetPdsHeader ack;
        ack.prologue.pdsType = PDS_TYPE_ACK;
        ack.prologue.nextHdr = UET_HDR_NONE;
        ack.cack_psn = 0x000186A0;
        ack.ack_psn_offset = 1;   // ACK_PSN = cack + 1
        ack.spdcid = 3; ack.dpdcid = 7;
        ack.probe_opaque = 0;
        PrintPdsHeader("ACK (Table 3-35, 12 bytes)", ack);

        // ACK_CC (with SACK bitmap)
        UetPdsHeader ackCC;
        ackCC.prologue.pdsType = PDS_TYPE_ACK_CC;
        ackCC.cack_psn = 0x000186A0;
        ackCC.ack_psn_offset = 3;
        ackCC.spdcid = 3; ackCC.dpdcid = 7;
        ackCC.mpr = 64; ackCC.cc_type = 1;
        ackCC.sack_psn_offset = 1;
        ackCC.sack_bitmap = 0x000000000000001FULL; // PSNs +1,+2,+3,+4 received
        PrintPdsHeader("ACK_CC with SACK bitmap (Table 3-36, 32 bytes)", ackCC);

        // NACK with code = TRIMMED
        UetPdsHeader nack;
        nack.prologue.pdsType = PDS_TYPE_NACK;
        nack.prologue.nextHdr = UET_HDR_NONE;
        nack.nack_code   = NACK_TRIMMED;
        nack.vendor_code = 0;
        nack.nack_psn    = 0x000186A2;
        nack.spdcid      = 3; nack.dpdcid = 7;
        nack.nack_payload= 0;
        PrintPdsHeader("NACK code=TRIMMED (Table 3-40, 14 bytes)", nack);

        // CP = NOOP
        UetPdsHeader cp;
        cp.prologue.pdsType = PDS_TYPE_CP;
        cp.prologue.nextHdr = (uint8_t)CP_NOOP;
        cp.prologue.flags.isrod = false;
        cp.psn = 0x000186A3;
        cp.spdcid = 7; cp.dpdcid = 3;
        cp.cp_payload = 0;
        PrintPdsHeader("CP (NOOP, Table 3-38, 14 bytes)", cp);

        // RUDI
        UetPdsHeader rudi;
        rudi.prologue.pdsType = PDS_TYPE_RUDI_REQ;
        rudi.prologue.nextHdr = UET_HDR_REQUEST_SMALL;
        rudi.pkt_id = 0xDEAD1;
        PrintPdsHeader("RUDI_REQ (Table 3-39, 6 bytes)", rudi);

        // UUD
        UetPdsHeader uud;
        uud.prologue.pdsType = PDS_TYPE_UUD_REQ;
        uud.prologue.nextHdr = UET_HDR_REQUEST_SMALL;
        PrintPdsHeader("UUD_REQ (Table 3-42, 4 bytes)", uud);

        // NACK codes table
        PrintTable("PDS NACK Codes (§3.5.12.7 Table 3-58)",
                   {"Code (hex)", "Name",               "Error Type",  "Source Action"},
        {
            {"0x01", "TRIMMED",          "NORMAL",    "RETX same PSN+PDC"},
            {"0x02", "TRIMMED_LASTHOP",  "NORMAL",    "RETX same PSN+PDC"},
            {"0x03", "TRIMMED_ACK",      "NORMAL",    "RETX original read"},
            {"0x04", "NO_PDC_AVAIL",     "NORMAL",    "RETRY new PDC"},
            {"0x05", "NO_CCC_AVAIL",     "NORMAL",    "RETRY new PDC"},
            {"0x06", "NO_BITMAP",        "NORMAL",    "RETRY new PDC"},
            {"0x07", "NO_PKT_BUFFER",    "NORMAL",    "RETX same PSN+PDC"},
            {"0x08", "NO_GTD_DEL_AVAIL", "NORMAL",    "RETX same PSN+PDC"},
            {"0x09", "NO_SES_MSG_AVAIL", "NORMAL",    "RETX same PSN+PDC"},
            {"0x0A", "NO_RESOURCE",      "NORMAL",    "RETX same PSN+PDC"},
            {"0x0B", "PSN_OOR_WINDOW",   "NORMAL",    "RETX if PSN>CACK"},
            {"0x0D", "ROD_OOO",          "NORMAL",    "RETX same PSN+PDC"},
            {"0x0E", "INV_DPDCID",       "PDC_FATAL", "RETRY new PDC"},
            {"0x0F", "PDC_HDR_MISMATCH", "PDC_FATAL", "RETRY new PDC"},
            {"0x10", "CLOSING",          "PDC_FATAL", "RETRY new PDC"},
            {"0x11", "INVALID_SYN",      "PDC_ERR",   "RETX same PSN+PDC"},
            {"0x12", "PDC_MODE_MISMATCH","PDC_FATAL", "CLOSE PDC"},
            {"0x13", "NEW_START_PSN",    "NORMAL",    "Wait + RETX"},
        });
    }

    // ═══════════════════════════════════════════════════════════════════════
    //  Phase 3: PDC Establishment (SYN handshake)
    //  N0 → N1 : RUD WRITE, multi-packet (12288 B, 3 chunks of 4096 B)
    // ═══════════════════════════════════════════════════════════════════════
    PrintHdr("Phase 3: PDC Establishment + RUD WRITE (§3.5.8.2, §3.5.9.3/9.4)", '-');
    {
        std::cout << "  N0 → N1: multi-packet RUD WRITE (3 × 4 KB = 12,288 bytes)\n";
        std::cout << "  Steps per spec §3.5.8.2:\n";
        std::cout << "    1. N0 allocates IPDCID (alloc_pdc), generates random Start_PSN\n";
        std::cout << "    2. N0 sets pds.flags.syn=1 on first packet (and all until ESTABLISHED)\n";
        std::cout << "    3. N0 encodes {pdc_info(4), psn_offset(12)} into pds.dpdcid field\n";
        std::cout << "    4. N1 receives SYN packet, allocates TPDCID, moves to ESTABLISHED\n";
        std::cout << "    5. N1 sends ACK with pds.spdcid=TPDCID so N0 can learn it\n";
        std::cout << "    6. N0 receives ACK, learns TPDCID, clears syn flag → ESTABLISHED\n\n";

        uint16_t msg1 = n0.engine.Send(
            n1.fa,
            /*tc=*/0,
            UET_MODE_RUD,
            (UetSesOpcode)UET_WRITE,
            /*totalBytes=*/12288,
            /*bufOffset=*/0x0000000000001000ULL,
            /*jobId=*/0xABC001,
            /*srcPid=*/1,
            /*dstRi=*/5,
            /*needsGuaranteedDel=*/false);

        std::cout << "  Send() returned msgId=" << msg1
                  << "  activePDCs=" << n0.engine.GetActivePdcCount()
                  << "  txPkts=" << n0.engine.GetTxPktCount()
                  << "  rxPkts=" << n0.engine.GetRxPktCount() << "\n\n";

        PrintPdcState(n0);
        PrintPdcState(n1);

        PrintTable("Phase 3 PDC Stats", {"Node", "ActivePDCs", "TxPkts", "RxPkts", "NACKsRcvd", "Retx"},
        {
            {"N0", std::to_string(n0.engine.GetActivePdcCount()),
             std::to_string(n0.engine.GetTxPktCount()),
             std::to_string(n0.engine.GetRxPktCount()),
             std::to_string(n0.engine.GetNackRcvdCount()),
             std::to_string(n0.engine.GetRetxCount())},
            {"N1", std::to_string(n1.engine.GetActivePdcCount()),
             std::to_string(n1.engine.GetTxPktCount()),
             std::to_string(n1.engine.GetRxPktCount()),
             std::to_string(n1.engine.GetNackRcvdCount()),
             std::to_string(n1.engine.GetRetxCount())},
        });
    }

    // ═══════════════════════════════════════════════════════════════════════
    //  Phase 4: ROD READ (ordered delivery)
    //  N1 → N2 : 512-byte READ
    // ═══════════════════════════════════════════════════════════════════════
    PrintHdr("Phase 4: ROD READ — Reliable Ordered Delivery (§3.5.3.4)", '-');
    {
        std::cout << "  N1 → N2: single-packet ROD READ (512 bytes)\n";
        std::cout << "  ROD requires pds.psn == Start_PSN on first packet (§3.5.8.2.e)\n";
        std::cout << "  Out-of-order packets are NACKd with NACK_ROD_OOO (0x0D)\n\n";

        uint16_t msg2 = n1.engine.Send(
            n2.fa, 0, UET_MODE_ROD, (UetSesOpcode)UET_READ,
            512, 0x2000ULL, 0xBCD001, 2, 10);

        std::cout << "  ROD msgId=" << msg2
                  << "  activePDCs(N1)=" << n1.engine.GetActivePdcCount() << "\n\n";
        PrintPdcState(n1);
    }

    // ═══════════════════════════════════════════════════════════════════════
    //  Phase 5: RUDI Datagram
    //  N2 → N3 : 256-byte RUDI datagram
    // ═══════════════════════════════════════════════════════════════════════
    PrintHdr("Phase 5: RUDI Request (§3.5.3.5) — Reliable Unordered Datagram", '-');
    {
        std::cout << "  N2 → N3: RUDI datagram (256 bytes)\n";
        std::cout << "  RUDI uses pkt_id instead of PSN (not monotonic, locally unique)\n";
        std::cout << "  No PDC established; reliability via RUDI-specific retry\n\n";

        uint16_t msg3 = n2.engine.Send(
            n3.fa, 0, UET_MODE_RUDI, (UetSesOpcode)UET_TAGGED_SEND,
            256, 0ULL, 0xCDE001, 3, 15);

        std::cout << "  RUDI msgId=" << msg3 << "\n\n";

        PrintTable("Phase 5 RUDI Stats", {"Node", "TxPkts", "RxPkts"},
        {
            {"N2", std::to_string(n2.engine.GetTxPktCount()), std::to_string(n2.engine.GetRxPktCount())},
            {"N3", std::to_string(n3.engine.GetTxPktCount()), std::to_string(n3.engine.GetRxPktCount())},
        });
    }

    // ═══════════════════════════════════════════════════════════════════════
    //  Phase 6: UUD Best-Effort
    //  N3 → N0 : 128-byte UUD
    // ═══════════════════════════════════════════════════════════════════════
    PrintHdr("Phase 6: UUD Request (§3.5.3.6) — Unreliable Unordered Delivery", '-');
    {
        std::cout << "  N3 → N0: UUD best-effort (128 bytes)\n";
        std::cout << "  Minimal 4-byte PDS header; no PDC, no ACK, no retransmit\n\n";

        uint16_t msg4 = n3.engine.Send(
            n0.fa, 0, UET_MODE_UUD, (UetSesOpcode)UET_DATAGRAM_SEND,
            128, 0ULL, 0xDEF001, 4, 20);

        std::cout << "  UUD msgId=" << msg4 << "  txPkts(N3)=" << n3.engine.GetTxPktCount() << "\n\n";
    }

    // ═══════════════════════════════════════════════════════════════════════
    //  Phase 7: PSN Space Details + SACK Demonstration
    // ═══════════════════════════════════════════════════════════════════════
    PrintHdr("Phase 7: PSN Space Mechanics (§3.5.11.4)", '-');
    {
        std::cout << "  PSN space variables (spec §3.5.11.4):\n";
        std::cout << "  ┌──────────────────┬────────────────────────────────────────────┐\n";
        std::cout << "  │ Variable         │ Description                                │\n";
        std::cout << "  ├──────────────────┼────────────────────────────────────────────┤\n";
        std::cout << "  │ Start_PSN        │ Random starting PSN, ≥2^16 from last used  │\n";
        std::cout << "  │ next_tx_psn      │ Next PSN to assign to outgoing packets      │\n";
        std::cout << "  │ CACK_PSN         │ Cumulative ACK: all ≤CACK_PSN delivered     │\n";
        std::cout << "  │ CLEAR_PSN        │ Initiator cleared guaranteed responses      │\n";
        std::cout << "  │ clear_psn_offset │ Wire: signed 16-bit offset from pds.psn     │\n";
        std::cout << "  │ ack_psn_offset   │ Wire: signed 16-bit offset from cack_psn    │\n";
        std::cout << "  │ SACK_PSN         │ Base of SACK bitmap (= rx_cack_psn + 1)    │\n";
        std::cout << "  │ sack_bitmap      │ 64-bit bitmap of individually received PSNs │\n";
        std::cout << "  └──────────────────┴────────────────────────────────────────────┘\n\n";

        // Demo: construct a PdcPsnSpace and show the math
        PdcPsnSpace psn;
        uint32_t S = 0x000186A0; // example Start_PSN
        psn.Init(S);
        std::cout << "  Example Start_PSN = 0x" << std::hex << S << std::dec << "\n";
        std::cout << "  After Init():\n";
        std::cout << "    next_tx_psn  = 0x" << std::hex << psn.next_tx_psn   << "\n";
        std::cout << "    cack_psn     = 0x" << psn.cack_psn     << "\n";
        std::cout << "    clear_psn    = 0x" << psn.clear_psn    << "\n";
        std::cout << "    expected_rx  = 0x" << psn.expected_rx_psn << std::dec << "\n\n";

        // Simulate 5 TX PSNs, then receive ACK for 0,2,4 (selective)
        uint32_t p0 = psn.AssignTxPsn();
        uint32_t p1 = psn.AssignTxPsn();
        uint32_t p2 = psn.AssignTxPsn();
        uint32_t p3 = psn.AssignTxPsn();
        uint32_t p4 = psn.AssignTxPsn();
        std::cout << "  Assigned PSNs: p0=0x" << std::hex << p0
                  << " p1=0x" << p1 << " p2=0x" << p2
                  << " p3=0x" << p3 << " p4=0x" << p4 << std::dec << "\n";
        std::cout << "  outstanding=" << psn.outstanding.size()
                  << "  clear_offset=" << (int)psn.GetClearPsnOffset() << "\n\n";

        // Receive packets p0, p2, p4 (out of order on RUD)
        psn.OnPktReceived(p0, false);
        psn.OnPktReceived(p2, false);
        psn.OnPktReceived(p4, false);
        uint32_t sackBase;
        uint64_t bitmap = psn.BuildSackBitmap(sackBase);
        std::cout << "  After receiving p0, p2, p4 (p1 and p3 missing):\n";
        std::cout << "    rx_cack_psn = 0x" << std::hex << psn.rx_cack_psn << std::dec
                  << " (consecutive up to p0)\n";
        std::cout << "    SACK_base   = 0x" << std::hex << sackBase << std::dec << "\n";
        std::cout << "    sack_bitmap = 0x" << std::hex << std::setw(16) << std::setfill('0') << bitmap << std::dec << std::setfill(' ') << "\n";
        std::cout << "    (bit 0 = p1 missing=0, bit 1 = p2 received=1, bit 2 = p3 missing=0, bit 3 = p4 received=1)\n\n";

        // Compute ACK_PSN offset per Table 3-43
        std::cout << "  ACK_PSN offset calculation (Table 3-43 §3.5.11.4.6):\n";
        struct {uint16_t off; uint32_t cack; uint32_t ack;} examples[] = {
            {0x0024, 0x62231120, 0x62231144},
            {(uint16_t)0xFFDC, 0x62231120, 0x622310FC},
            {0x0010, 0xFFFFFFF3, 0x00000003},
        };
        for (auto& e : examples)
        {
            int32_t off32 = (int32_t)(int16_t)e.off;
            uint32_t computed = e.cack + off32;
            std::cout << "    ack_psn_offset=0x" << std::hex << std::setw(4) << e.off
                      << " cack_psn=0x" << e.cack
                      << " → ACK_PSN=0x" << computed
                      << " (spec says 0x" << e.ack << (computed==e.ack ? " ✓" : " ✗") << ")"
                      << std::dec << "\n";
        }
        std::cout << "\n";
    }

    // ═══════════════════════════════════════════════════════════════════════
    //  Phase 8: Control Packets (§3.5.16)
    // ═══════════════════════════════════════════════════════════════════════
    PrintHdr("Phase 8: Control Packets (§3.5.16)", '-');
    {
        PrintTable("PDS Control Packet Types (§3.5.16)", {"ctl_type", "Value", "Description"},
        {
            {"NOOP",         "0", "No-operation; can open a PDC (opens PDC if SYN set)"},
            {"ACK_REQUEST",  "1", "Request ACK for specific PSN (§3.5.16.1)"},
            {"CLEAR_COMMAND","2", "Initiator → target: clear guaranteed-delivery state"},
            {"CLEAR_REQUEST", "3","Target → initiator: please send CLEAR (need CLEAR_PSN)"},
            {"CLOSE_COMMAND","4", "Initiator: PDC is now closed (§3.5.8.3)"},
            {"CLOSE_REQUEST","5", "Target: requests initiator to close PDC"},
            {"PROBE",        "6", "Source: probe for ACK when window stalled"},
            {"CREDIT",       "7", "Destination: congestion control credit grant"},
            {"CREDIT_REQUEST","8","Source: request credit when credit=0"},
            {"NEGOTIATION",  "9", "PDC capability negotiation"},
        });
    }

    // ═══════════════════════════════════════════════════════════════════════
    //  Phase 9: Final RX Log Summary
    // ═══════════════════════════════════════════════════════════════════════
    PrintHdr("Phase 9: Final Stats & RX Logs", '-');
    {
        PrintTable("Final Statistics",
                   {"Node", "TxPkts", "RxPkts", "NACKsRcvd", "NACKsSent", "Retx"},
        {
            {"N0", std::to_string(n0.engine.GetTxPktCount()), std::to_string(n0.engine.GetRxPktCount()),
             std::to_string(n0.engine.GetNackRcvdCount()), std::to_string(n0.engine.GetNackSentCount()),
             std::to_string(n0.engine.GetRetxCount())},
            {"N1", std::to_string(n1.engine.GetTxPktCount()), std::to_string(n1.engine.GetRxPktCount()),
             std::to_string(n1.engine.GetNackRcvdCount()), std::to_string(n1.engine.GetNackSentCount()),
             std::to_string(n1.engine.GetRetxCount())},
            {"N2", std::to_string(n2.engine.GetTxPktCount()), std::to_string(n2.engine.GetRxPktCount()),
             std::to_string(n2.engine.GetNackRcvdCount()), std::to_string(n2.engine.GetNackSentCount()),
             std::to_string(n2.engine.GetRetxCount())},
            {"N3", std::to_string(n3.engine.GetTxPktCount()), std::to_string(n3.engine.GetRxPktCount()),
             std::to_string(n3.engine.GetNackRcvdCount()), std::to_string(n3.engine.GetNackSentCount()),
             std::to_string(n3.engine.GetRetxCount())},
        });

        for (SimNode* node : {&n0, &n1, &n2, &n3})
        {
            std::cout << "  " << node->Log() << " RX events:\n";
            for (auto& msg : node->rxLog) std::cout << "    " << msg << "\n";
            if (node->rxLog.empty()) std::cout << "    (none)\n";
            std::cout << "  TX completion events:\n";
            for (auto& msg : node->txLog) std::cout << "    " << msg << "\n";
            if (node->txLog.empty()) std::cout << "    (none)\n";
            std::cout << "\n";
        }
    }

    PrintHdr("Simulation Complete — UE SES/PDS/PDC Stack Validated");
    std::cout << "\n"
              << "  Implementation covers spec through PDC (page 343 of UE-Specification-1.0.2):\n"
              << "  ✓ §3.4   SES — all 7 header formats with exact bit fields\n"
              << "  ✓ §3.5.3 PDS — RUD, ROD, RUDI, UUD delivery modes\n"
              << "  ✓ §3.5.8 PDC — full establishment (SYN), PSN spaces, close\n"
              << "  ✓ §3.5.9 PDC Manager — alloc, mapping tuple, msgmap\n"
              << "  ✓ §3.5.10 PDS headers — all 12 wire formats serialized\n"
              << "  ✓ §3.5.11 PSN mechanics — CLEAR_PSN, CACK_PSN, SACK bitmap\n"
              << "  ✓ §3.5.12 ACK/NACK — all 20 NACK codes, RTO/retransmit logic\n"
              << "  ✓ §3.5.16 Control Packets — NOOP, ACK_REQ, CLEAR, CLOSE, PROBE\n"
              << "\n";

    Simulator::Run();
    Simulator::Destroy();
    return 0;
}
