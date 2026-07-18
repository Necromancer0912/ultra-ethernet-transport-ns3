/**
 * uet-pds-header.cc — Ultra Ethernet PDS header serialization
 *
 * Wire sizes per spec §3.5.10 (all sizes in bytes):
 *   Prologue  = 2 bytes {type(5), next_hdr/ctl(4), flags(7)}
 *   RUD/ROD Request:    12  = prologue(2)+clear_psn_offset(2)+psn(4)+spdcid(2)+dpdcid(2)
 *   RUD/ROD Req+CC:     16  = Request(12) + req_cc_state(4)
 *   ACK:                12  = prologue(2)+ack_psn_off(2)+probe_opaque(2)+cack_psn(4)+spdcid(2)+dpdcid(2)
 *   ACK_CC:             32  = ACK(12)+cc_type/flags/mpr(2)+sack_psn_off(2)+sack_bitmap(8)+cc_state(8)
 *   ACK_CCX:            44  = ACK(12)+cc_hdr(12)+ccx_state(16)+pad (same cc_type/mpr/sack structure)
 *   CP:                 14  = prologue(2)+probe_opaque(2)+psn(4)+spdcid(2)+dpdcid(2)+payload(4) - 2 (reuse ack_psn_off slot as probe_opaque)
 *   RUDI:               6   = prologue(2)+pkt_id(4)
 *   NACK:               14  = prologue(2)+nack_code(1)+vendor_code(1)+nack_psn(4)+spdcid(2)+dpdcid(2)+payload(4)   (NOTE: prologue counts in 2)
 *   UUD:                4   = prologue(2)+rsvd(2)
 */

#include "uet-pds-header.h"

#include "ns3/log.h"

#include <iomanip>
#include <sstream>

namespace ns3
{

NS_LOG_COMPONENT_DEFINE("UetPdsHeader");
NS_OBJECT_ENSURE_REGISTERED(UetPdsHeader);

// ═════════════════════════════════════════════════════════════════════════════
//  Wire sizes
// ═════════════════════════════════════════════════════════════════════════════
static constexpr uint32_t SZ_RUD_ROD_REQ    = 12;
static constexpr uint32_t SZ_RUD_ROD_REQ_CC = 16;
static constexpr uint32_t SZ_ACK            = 12;
static constexpr uint32_t SZ_ACK_CC         = 32;
static constexpr uint32_t SZ_ACK_CCX        = 44;
static constexpr uint32_t SZ_CP             = 16;
static constexpr uint32_t SZ_RUDI           =  6;
static constexpr uint32_t SZ_NACK           = 16;
static constexpr uint32_t SZ_NACK_CCX       = 28;
static constexpr uint32_t SZ_UUD            =  4;

// ─────────────────────────────────────────────────────────────────────────────
//  PDS Prologue helpers
// ─────────────────────────────────────────────────────────────────────────────
// Wire byte 0: [type(5) | next_hdr_or_ctl(4) ] — upper 5 bits = type, lower 4 = next/ctl
// Wait — spec says "5 bits type" and "4 bits next_hdr" = 9 bits, so it MUST span 2 bytes.
// Layout from spec: 2 bytes total = [type(5)|next_hdr_or_ctl(4)| reserved(7-flag bits)]
// Actually spec Table 3-32: type=5 bits, next_hdr/ctl_type=4 bits, flags=7 bits = 16 bits total.
// Packing: MSB first:
//   bits 15-11 = type (5)
//   bits 10-7  = next_hdr/ctl (4)
//   bits  6-0  = flags (7)
void
UetPdsPrologue::Write(Buffer::Iterator& i, bool isCP) const
{
    uint16_t word = 0;
    word |= (uint16_t)(pdsType & 0x1F) << 11;  // bits 15:11
    word |= (uint16_t)(nextHdr & 0x0F) << 7;   // bits 10:7

    // Pack flags into bits 6:0 based on packet type
    // Request / CP flags: [rsvd(1)|isrod(1)|retx(1)|ar(1)|syn(1)|rsvd(2)]
    // ACK flags:          [rsvd(1)|m(1)|retx(1)|p(1)|req(1)|rsvd(2)]
    // NACK flags:         [rsvd(1)|m(1)|retx(1)|nt(1)|rsvd(3)]
    // RUDI flags:         [rsvd(1)|m(1)|retx(1)|rsvd(4)]
    uint8_t f = 0;
    if (isCP)
    {
        f |= (flags.isrod ? 1 : 0) << 5;
        f |= (flags.retx  ? 1 : 0) << 4;
        f |= (flags.ar    ? 1 : 0) << 3;
        f |= (flags.syn   ? 1 : 0) << 2;
    }
    else if (pdsType == PDS_TYPE_ACK || pdsType == PDS_TYPE_ACK_CC || pdsType == PDS_TYPE_ACK_CCX)
    {
        f |= (flags.m    ? 1 : 0) << 5;
        f |= (flags.retx ? 1 : 0) << 4;
        f |= (flags.p    ? 1 : 0) << 3;
        f |= (flags.req  ? 1 : 0) << 2;
    }
    else if (pdsType == PDS_TYPE_NACK || pdsType == PDS_TYPE_NACK_CCX)
    {
        f |= (flags.m    ? 1 : 0) << 5;
        f |= (flags.retx ? 1 : 0) << 4;
        f |= (flags.nt   ? 1 : 0) << 3;
    }
    else if (pdsType == PDS_TYPE_RUDI_REQ || pdsType == PDS_TYPE_RUDI_RESP)
    {
        f |= (flags.m    ? 1 : 0) << 5;
        f |= (flags.retx ? 1 : 0) << 4;
    }
    else // RUD/ROD request
    {
        f |= (flags.retx ? 1 : 0) << 4;
        f |= (flags.ar   ? 1 : 0) << 3;
        f |= (flags.syn  ? 1 : 0) << 2;
    }
    word |= f;
    i.WriteHtonU16(word);
}

void
UetPdsPrologue::Read(Buffer::Iterator& i, bool isCP)
{
    uint16_t word = i.ReadNtohU16();
    pdsType = (word >> 11) & 0x1F;
    nextHdr = (word >>  7) & 0x0F;
    uint8_t f = word & 0x7F;

    flags = UetPdsFlags{};
    if (isCP)
    {
        flags.isrod = (f >> 5) & 1;
        flags.retx  = (f >> 4) & 1;
        flags.ar    = (f >> 3) & 1;
        flags.syn   = (f >> 2) & 1;
    }
    else if (pdsType == PDS_TYPE_ACK || pdsType == PDS_TYPE_ACK_CC || pdsType == PDS_TYPE_ACK_CCX)
    {
        flags.m    = (f >> 5) & 1;
        flags.retx = (f >> 4) & 1;
        flags.p    = (f >> 3) & 1;
        flags.req  = (f >> 2) & 1;
    }
    else if (pdsType == PDS_TYPE_NACK || pdsType == PDS_TYPE_NACK_CCX)
    {
        flags.m    = (f >> 5) & 1;
        flags.retx = (f >> 4) & 1;
        flags.nt   = (f >> 3) & 1;
    }
    else if (pdsType == PDS_TYPE_RUDI_REQ || pdsType == PDS_TYPE_RUDI_RESP)
    {
        flags.m    = (f >> 5) & 1;
        flags.retx = (f >> 4) & 1;
    }
    else
    {
        flags.retx = (f >> 4) & 1;
        flags.ar   = (f >> 3) & 1;
        flags.syn  = (f >> 2) & 1;
    }
}

// ═════════════════════════════════════════════════════════════════════════════
//  UetPdsHeader — ns3 boilerplate
// ═════════════════════════════════════════════════════════════════════════════
UetPdsHeader::UetPdsHeader()
    : clear_psn_offset(0), psn(0), spdcid(0), dpdcid(0),
      pdc_info(0), psn_offset(0), req_cc_state(0),
      ack_psn_offset(0), probe_opaque(0), cack_psn(0),
      cc_type(0), cc_flags(0), mpr(0), sack_psn_offset(0),
      sack_bitmap(0), ack_cc_state(0),
      ack_ccx_state_lo(0), ack_ccx_state_hi(0),
      cp_payload(0), pkt_id(0),
      nack_code(0), vendor_code(0), nack_psn(0), nack_payload(0)
{
    prologue.pdsType = PDS_TYPE_RUD_REQ;
    prologue.nextHdr = UET_HDR_REQUEST_STD;
}

UetPdsHeader::~UetPdsHeader() = default;

TypeId
UetPdsHeader::GetTypeId()
{
    static TypeId tid = TypeId("ns3::UetPdsHeader")
                            .SetParent<Header>()
                            .AddConstructor<UetPdsHeader>();
    return tid;
}

TypeId
UetPdsHeader::GetInstanceTypeId() const { return GetTypeId(); }

// ─────────────────────────────────────────────────────────────────────────────
//  Type queries
// ─────────────────────────────────────────────────────────────────────────────
bool UetPdsHeader::IsRequest() const
{
    return prologue.pdsType == PDS_TYPE_RUD_REQ ||
           prologue.pdsType == PDS_TYPE_ROD_REQ ||
           prologue.pdsType == PDS_TYPE_RUD_CC_REQ ||
           prologue.pdsType == PDS_TYPE_ROD_CC_REQ;
}
bool UetPdsHeader::IsAck()  const
{
    return prologue.pdsType == PDS_TYPE_ACK ||
           prologue.pdsType == PDS_TYPE_ACK_CC ||
           prologue.pdsType == PDS_TYPE_ACK_CCX;
}
bool UetPdsHeader::IsNack() const
{
    return prologue.pdsType == PDS_TYPE_NACK ||
           prologue.pdsType == PDS_TYPE_NACK_CCX;
}
bool UetPdsHeader::IsCP()   const { return prologue.pdsType == PDS_TYPE_CP; }
bool UetPdsHeader::IsRudi() const
{
    return prologue.pdsType == PDS_TYPE_RUDI_REQ ||
           prologue.pdsType == PDS_TYPE_RUDI_RESP;
}
bool UetPdsHeader::IsUud()  const { return prologue.pdsType == PDS_TYPE_UUD_REQ; }

// ─────────────────────────────────────────────────────────────────────────────
//  SYN dpdcid overload helpers (§3.5.8.2)
// ─────────────────────────────────────────────────────────────────────────────
void
UetPdsHeader::SetSynDpdcid(uint8_t pdcInfo4, uint16_t psnOffset12)
{
    pdc_info   = pdcInfo4  & 0x0F;
    psn_offset = psnOffset12 & 0x0FFF;
    dpdcid = (uint16_t)((pdc_info << 12) | psn_offset);
}

void
UetPdsHeader::GetSynDpdcid(uint8_t& pdcInfo4, uint16_t& psnOffset12) const
{
    pdcInfo4   = (dpdcid >> 12) & 0x0F;
    psnOffset12 = dpdcid & 0x0FFF;
}

// ─────────────────────────────────────────────────────────────────────────────
//  GetSerializedSize
// ─────────────────────────────────────────────────────────────────────────────
uint32_t
UetPdsHeader::GetSerializedSize() const
{
    switch (prologue.pdsType)
    {
    case PDS_TYPE_RUD_REQ:
    case PDS_TYPE_ROD_REQ:
        return SZ_RUD_ROD_REQ;
    case PDS_TYPE_RUD_CC_REQ:
    case PDS_TYPE_ROD_CC_REQ:
        return SZ_RUD_ROD_REQ_CC;
    case PDS_TYPE_ACK:
        return SZ_ACK;
    case PDS_TYPE_ACK_CC:
        return SZ_ACK_CC;
    case PDS_TYPE_ACK_CCX:
        return SZ_ACK_CCX;
    case PDS_TYPE_CP:
        return SZ_CP;
    case PDS_TYPE_RUDI_REQ:
    case PDS_TYPE_RUDI_RESP:
        return SZ_RUDI;
    case PDS_TYPE_NACK:
        return SZ_NACK;
    case PDS_TYPE_NACK_CCX:
        return SZ_NACK_CCX;
    case PDS_TYPE_UUD_REQ:
        return SZ_UUD;
    default:
        return 2; // prologue only
    }
}

// ═════════════════════════════════════════════════════════════════════════════
//  Serialize
// ═════════════════════════════════════════════════════════════════════════════
void
UetPdsHeader::Serialize(Buffer::Iterator start) const
{
    switch (prologue.pdsType)
    {
    case PDS_TYPE_RUD_REQ:
    case PDS_TYPE_ROD_REQ:
    case PDS_TYPE_RUD_CC_REQ:
    case PDS_TYPE_ROD_CC_REQ:
        SerializeRudRodReq(start);
        break;
    case PDS_TYPE_ACK:
        SerializeRudRodAck(start);
        break;
    case PDS_TYPE_ACK_CC:
        SerializeRudRodAckCc(start);
        break;
    case PDS_TYPE_ACK_CCX:
        SerializeRudRodAckCcx(start);
        break;
    case PDS_TYPE_CP:
        SerializeCP(start);
        break;
    case PDS_TYPE_RUDI_REQ:
    case PDS_TYPE_RUDI_RESP:
        SerializeRudi(start);
        break;
    case PDS_TYPE_NACK:
    case PDS_TYPE_NACK_CCX:
        SerializeNack(start);
        break;
    case PDS_TYPE_UUD_REQ:
        SerializeUud(start);
        break;
    default:
        prologue.Write(start);
        break;
    }
}

// ─────────────────────────────────────────────────────────────────────────────
//  RUD/ROD Request (Table 3-33) — 12 bytes (without cc_state)
//  prologue(2) + clear_psn_offset(2) + psn(4) + spdcid(2) + dpdcid(2)
// ─────────────────────────────────────────────────────────────────────────────
void
UetPdsHeader::SerializeRudRodReq(Buffer::Iterator& i) const
{
    prologue.Write(i);
    i.WriteHtonU16((uint16_t)clear_psn_offset);
    i.WriteHtonU32(psn);
    i.WriteHtonU16(spdcid);
    i.WriteHtonU16(dpdcid);
    if (prologue.pdsType == PDS_TYPE_RUD_CC_REQ || prologue.pdsType == PDS_TYPE_ROD_CC_REQ)
    {
        i.WriteHtonU32(req_cc_state);
    }
}

// ─────────────────────────────────────────────────────────────────────────────
//  RUD/ROD ACK (Table 3-35) — 12 bytes
//  prologue(2) + ack_psn_offset(2) + probe_opaque(2) + cack_psn(4) + spdcid(2) + dpdcid(2)
// ─────────────────────────────────────────────────────────────────────────────
void
UetPdsHeader::SerializeRudRodAck(Buffer::Iterator& i) const
{
    prologue.Write(i);
    if (prologue.flags.p)
        i.WriteHtonU16(probe_opaque);
    else
        i.WriteHtonU16((uint16_t)ack_psn_offset);
    i.WriteHtonU32(cack_psn);
    i.WriteHtonU16(spdcid);
    i.WriteHtonU16(dpdcid);
}

// ─────────────────────────────────────────────────────────────────────────────
//  ACK_CC (Table 3-36) — 32 bytes  = ACK(12) + {cc_type(4)|cc_flags(4)|mpr(8)|sack_psn(16)} + sack_bitmap(64) + cc_state(64)
// ─────────────────────────────────────────────────────────────────────────────
void
UetPdsHeader::SerializeRudRodAckCc(Buffer::Iterator& i) const
{
    SerializeRudRodAck(i);
    i.WriteU8(((cc_type & 0x0F) << 4) | (cc_flags & 0x0F));
    i.WriteU8(mpr);
    i.WriteHtonU16((uint16_t)sack_psn_offset);
    i.WriteHtonU64(sack_bitmap);
    i.WriteHtonU64(ack_cc_state);
}

// ─────────────────────────────────────────────────────────────────────────────
//  ACK_CCX (Table 3-37) — 44 bytes = ACK(12) + same cc header(4) + sack(10) + ccx_state(128b = 16B)
// ─────────────────────────────────────────────────────────────────────────────
void
UetPdsHeader::SerializeRudRodAckCcx(Buffer::Iterator& i) const
{
    SerializeRudRodAck(i);
    i.WriteU8(((cc_type & 0x0F) << 4) | (cc_flags & 0x0F));
    i.WriteU8(mpr);
    i.WriteHtonU16((uint16_t)sack_psn_offset);
    i.WriteHtonU64(sack_bitmap);
    i.WriteHtonU64(ack_ccx_state_hi);
    i.WriteHtonU64(ack_ccx_state_lo);
    i.WriteHtonU32(0); // 4 bytes padding to reach 44 bytes
}

// ─────────────────────────────────────────────────────────────────────────────
//  CP (Table 3-38) — 14 bytes
//  prologue(2) + probe_opaque(2) + psn(4) + spdcid(2) + dpdcid(2) + payload(4)
//  Note: the ack_psn_offset slot (2 bytes) is used as probe_opaque in CP
// ─────────────────────────────────────────────────────────────────────────────
void
UetPdsHeader::SerializeCP(Buffer::Iterator& i) const
{
    prologue.Write(i, /*isCP=*/true);
    i.WriteHtonU16(probe_opaque);
    i.WriteHtonU32(psn);
    i.WriteHtonU16(spdcid);
    i.WriteHtonU16(dpdcid);
    i.WriteHtonU32(cp_payload);
}

// ─────────────────────────────────────────────────────────────────────────────
//  RUDI (Table 3-39) — 6 bytes  = prologue(2) + pkt_id(4)
// ─────────────────────────────────────────────────────────────────────────────
void
UetPdsHeader::SerializeRudi(Buffer::Iterator& i) const
{
    prologue.Write(i);
    i.WriteHtonU32(pkt_id);
}

// ─────────────────────────────────────────────────────────────────────────────
//  NACK (Table 3-40) — 14 bytes
//  prologue(2) + nack_code(1) + vendor_code(1) + nack_psn(4) + spdcid(2) + dpdcid(2) + payload(4)
// ─────────────────────────────────────────────────────────────────────────────
void
UetPdsHeader::SerializeNack(Buffer::Iterator& i) const
{
    prologue.Write(i);
    i.WriteU8(nack_code);
    i.WriteU8(vendor_code);
    i.WriteHtonU32(nack_psn);
    i.WriteHtonU16(spdcid);
    i.WriteHtonU16(dpdcid);
    i.WriteHtonU32(nack_payload);
    if (prologue.pdsType == PDS_TYPE_NACK_CCX)
    {
        // Extended CC state (Table 3-41): 12 additional bytes
        i.WriteHtonU64(ack_ccx_state_hi);
        i.WriteHtonU32((uint32_t)ack_ccx_state_lo);
    }
}

// ─────────────────────────────────────────────────────────────────────────────
//  UUD (Table 3-42) — 4 bytes  = prologue(2) + rsvd(2)
// ─────────────────────────────────────────────────────────────────────────────
void
UetPdsHeader::SerializeUud(Buffer::Iterator& i) const
{
    prologue.Write(i);
    i.WriteHtonU16(0); // reserved
}

// ═════════════════════════════════════════════════════════════════════════════
//  Deserialize (dispatcher)
// ═════════════════════════════════════════════════════════════════════════════
uint32_t
UetPdsHeader::Deserialize(Buffer::Iterator start)
{
    Buffer::Iterator i = start;

    // Read prologue to determine type
    prologue.Read(i, false);

    // Re-read if it was a CP (flags are different)
    if (prologue.pdsType == PDS_TYPE_CP)
    {
        i = start;
        prologue.Read(i, /*isCP=*/true);
    }

    uint32_t bytes = 2; // prologue
    switch (prologue.pdsType)
    {
    case PDS_TYPE_RUD_REQ:
    case PDS_TYPE_ROD_REQ:
    case PDS_TYPE_RUD_CC_REQ:
    case PDS_TYPE_ROD_CC_REQ:
        bytes += DeserializeRudRodReq(i);
        break;
    case PDS_TYPE_ACK:
        bytes += DeserializeRudRodAck(i);
        break;
    case PDS_TYPE_ACK_CC:
        bytes += DeserializeRudRodAckCc(i);
        break;
    case PDS_TYPE_ACK_CCX:
        bytes += DeserializeRudRodAckCcx(i);
        break;
    case PDS_TYPE_CP:
        bytes += DeserializeCP(i);
        break;
    case PDS_TYPE_RUDI_REQ:
    case PDS_TYPE_RUDI_RESP:
        bytes += DeserializeRudi(i);
        break;
    case PDS_TYPE_NACK:
    case PDS_TYPE_NACK_CCX:
        bytes += DeserializeNack(i);
        break;
    case PDS_TYPE_UUD_REQ:
        bytes += DeserializeUud(i);
        break;
    default:
        break;
    }
    return bytes;
}

uint32_t UetPdsHeader::DeserializeRudRodReq(Buffer::Iterator& i)
{
    clear_psn_offset = (int16_t)i.ReadNtohU16();
    psn    = i.ReadNtohU32();
    spdcid = i.ReadNtohU16();
    dpdcid = i.ReadNtohU16();
    uint32_t extra = 0;
    if (prologue.pdsType == PDS_TYPE_RUD_CC_REQ || prologue.pdsType == PDS_TYPE_ROD_CC_REQ)
    {
        req_cc_state = i.ReadNtohU32();
        extra = 4;
    }
    return 10 + extra; // (prologue already counted)
}

uint32_t UetPdsHeader::DeserializeRudRodAck(Buffer::Iterator& i)
{
    uint16_t val = i.ReadNtohU16();
    if (prologue.flags.p) {
        probe_opaque   = val;
        ack_psn_offset = 0;
    } else {
        ack_psn_offset = (int16_t)val;
        probe_opaque   = 0;
    }
    cack_psn       = i.ReadNtohU32();
    spdcid         = i.ReadNtohU16();
    dpdcid         = i.ReadNtohU16();
    return 10;
}

uint32_t UetPdsHeader::DeserializeRudRodAckCc(Buffer::Iterator& i)
{
    uint32_t base = DeserializeRudRodAck(i);
    uint8_t  b    = i.ReadU8();
    cc_type  = (b >> 4) & 0x0F;
    cc_flags =  b       & 0x0F;
    mpr             = i.ReadU8();
    sack_psn_offset = (int16_t)i.ReadNtohU16();
    sack_bitmap     = i.ReadNtohU64();
    ack_cc_state    = i.ReadNtohU64();
    return base + 20;
}

uint32_t UetPdsHeader::DeserializeRudRodAckCcx(Buffer::Iterator& i)
{
    uint32_t base = DeserializeRudRodAck(i);
    uint8_t  b    = i.ReadU8();
    cc_type  = (b >> 4) & 0x0F;
    cc_flags =  b       & 0x0F;
    mpr               = i.ReadU8();
    sack_psn_offset   = (int16_t)i.ReadNtohU16();
    sack_bitmap       = i.ReadNtohU64();
    ack_ccx_state_hi  = i.ReadNtohU64();
    ack_ccx_state_lo  = i.ReadNtohU64();
    i.ReadNtohU32(); // 4 bytes padding
    return base + 32;
}

uint32_t UetPdsHeader::DeserializeCP(Buffer::Iterator& i)
{
    probe_opaque = i.ReadNtohU16();
    psn          = i.ReadNtohU32();
    spdcid       = i.ReadNtohU16();
    dpdcid       = i.ReadNtohU16();
    cp_payload   = i.ReadNtohU32();
    return 14; // opaque(2)+psn(4)+spdcid(2)+dpdcid(2)+payload(4)
}

uint32_t UetPdsHeader::DeserializeRudi(Buffer::Iterator& i)
{
    pkt_id = i.ReadNtohU32();
    return 4;
}

uint32_t UetPdsHeader::DeserializeNack(Buffer::Iterator& i)
{
    nack_code    = i.ReadU8();
    vendor_code  = i.ReadU8();
    nack_psn     = i.ReadNtohU32();
    spdcid       = i.ReadNtohU16();
    dpdcid       = i.ReadNtohU16();
    nack_payload = i.ReadNtohU32();
    uint32_t n = 14; // code(1)+vendor(1)+psn(4)+spdcid(2)+dpdcid(2)+payload(4)
    if (prologue.pdsType == PDS_TYPE_NACK_CCX)
    {
        ack_ccx_state_hi = i.ReadNtohU64();
        ack_ccx_state_lo = i.ReadNtohU32();
        n += 12;
    }
    return n;
}

uint32_t UetPdsHeader::DeserializeUud(Buffer::Iterator& i)
{
    i.ReadNtohU16(); // rsvd
    return 2;
}

// ─────────────────────────────────────────────────────────────────────────────
//  NACK code names (Table 3-58)
// ─────────────────────────────────────────────────────────────────────────────
const char*
UetPdsHeader::NackCodeName(uint8_t code)
{
    static const char* names[] = {
        "RESERVED",             // 0x00
        "TRIMMED",              // 0x01
        "TRIMMED_LASTHOP",      // 0x02
        "TRIMMED_ACK",          // 0x03
        "NO_PDC_AVAIL",         // 0x04
        "NO_CCC_AVAIL",         // 0x05
        "NO_BITMAP",            // 0x06
        "NO_PKT_BUFFER",        // 0x07
        "NO_GTD_DEL_AVAIL",     // 0x08
        "NO_SES_MSG_AVAIL",     // 0x09
        "NO_RESOURCE",          // 0x0A
        "PSN_OOR_WINDOW",       // 0x0B
        "RESERVED_0x0C",        // 0x0C
        "ROD_OOO",              // 0x0D
        "INV_DPDCID",           // 0x0E
        "PDC_HDR_MISMATCH",     // 0x0F
        "CLOSING",              // 0x10
        "INVALID_SYN",          // 0x11
        "PDC_MODE_MISMATCH",    // 0x12
        "NEW_START_PSN",        // 0x13
    };
    if (code < sizeof(names) / sizeof(names[0]))
        return names[code];
    return "UNKNOWN";
}

// ─────────────────────────────────────────────────────────────────────────────
//  Print
// ─────────────────────────────────────────────────────────────────────────────
void
UetPdsHeader::Print(std::ostream& os) const
{
    static const char* typeNames[] = {
        "RSVD","TSS","RUD_REQ","ROD_REQ","RUDI_REQ","RUDI_RESP",
        "UUD_REQ","ACK","ACK_CC","ACK_CCX","NACK","CP",
        "NACK_CCX","RUD_CC_REQ","ROD_CC_REQ"};
    os << "[PDS type=" << typeNames[prologue.pdsType];
    if (IsRequest())
    {
        os << " psn=" << psn  << " spdcid=" << spdcid << " dpdcid=" << dpdcid;
        if (prologue.flags.syn)  os << " SYN";
        if (prologue.flags.retx) os << " RETX";
        if (prologue.flags.ar)   os << " AR";
    }
    else if (IsAck())
    {
        os << " cack_psn=" << cack_psn
           << " ack_psn=" << ComputeAckPsn()
           << " spdcid=" << spdcid;
    }
    else if (IsNack())
    {
        os << " nack_code=" << NackCodeName(nack_code)
           << "(0x" << std::hex << (int)nack_code << std::dec << ")"
           << " nack_psn=" << nack_psn;
    }
    else if (IsCP())
    {
        static const char* ctlNames[] = {
            "NOOP","ACK_REQ","CLEAR_CMD","CLEAR_REQ",
            "CLOSE_CMD","CLOSE_REQ","PROBE","CREDIT","CREDIT_REQ","NEGOTIATION"};
        uint8_t ct = prologue.nextHdr & 0x0F;
        os << " ctl=" << (ct < 10 ? ctlNames[ct] : "?")
           << " psn=" << psn;
    }
    os << "]";
}

} // namespace ns3
