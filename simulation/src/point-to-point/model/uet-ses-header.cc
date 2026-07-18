/**
 * uet-ses-header.cc  —  Ultra Ethernet Semantic Sublayer Header implementation
 *
 * Wire-format sizes (in bytes) per UE-Specification-1.0.2:
 *
 *  STD SOM=1  (Table 3-8)  : 44 bytes
 *  STD SOM=0  (Table 3-9)  : 36 bytes
 *  SMALL      (Table 3-10) : 20 bytes
 *  MEDIUM     (Table 3-14) : 28 bytes
 *  RESPONSE   (Table 3-11) : 12 bytes
 *  RESP_DATA  (Table 3-12) : 20 bytes
 *  RESP_SMALL (Table 3-13) : 12 bytes
 *
 * All integers are serialised in BIG-ENDIAN (network byte order).
 * Bit-field packing follows spec figure descriptions (MSB first).
 */

#include "uet-ses-header.h"

#include "ns3/log.h"

namespace ns3
{

NS_LOG_COMPONENT_DEFINE("UetSesHeader");
NS_OBJECT_ENSURE_REGISTERED(UetSesHeader);

// ═════════════════════════════════════════════════════════════════════════════
//  Wire sizes (bytes) — spec-defined
// ═════════════════════════════════════════════════════════════════════════════
// STD SOM=1: rsvd(2)+opcode(6)+ver(2)+flags(6)+1 = 2 bytes word-0
//           + message_id(16) + ri_gen(8)+jobId(24) = 2+4+1+3 bytes
//           + rsvd(4)+pid(12)+rsvd(4)+ri(12) = 2+2 bytes
//           + buffer_offset(64) + initiator(32) + match_bits(64) + header_data(64) + req_len(32)
// = 2+2+4+2+2+8+4+8+8+4 = 44 bytes
static constexpr uint32_t SZ_STD_SOM1   = 44;
static constexpr uint32_t SZ_STD_SOM0   = 32; // same minus header_data + add payload_len+msg_offset
static constexpr uint32_t SZ_SMALL      = 20;
static constexpr uint32_t SZ_MEDIUM     = 28;
static constexpr uint32_t SZ_RESPONSE   = 12;
static constexpr uint32_t SZ_RESP_DATA  = 20;
static constexpr uint32_t SZ_RESP_SMALL = 12;

// ─────────────────────────────────────────────────────────────────────────────
//  Atomic extension header
// ─────────────────────────────────────────────────────────────────────────────
void
UetAtomicExtHdr::Serialize(Buffer::Iterator& i) const
{
    i.WriteU8(atomic_opcode);
    i.WriteU8(atomic_dtype);
    i.WriteU8(sem_ctrl);
    i.WriteU8(rsvd);
}

UetAtomicExtHdr
UetAtomicExtHdr::Deserialize(Buffer::Iterator& i)
{
    UetAtomicExtHdr h;
    h.atomic_opcode = i.ReadU8();
    h.atomic_dtype  = i.ReadU8();
    h.sem_ctrl      = i.ReadU8();
    h.rsvd          = i.ReadU8();
    return h;
}

// ═════════════════════════════════════════════════════════════════════════════
//  UetSesHeader — ns3 boilerplate
// ═════════════════════════════════════════════════════════════════════════════
UetSesHeader::UetSesHeader()
    : opcode(UET_NO_OP),
      version(0),
      dc(false), ie(false), rel(false), hd(false), eom(true), som(true),
      message_id(0), ri_generation(0), jobId(0), pidOnFep(0),
      resource_index(0), buffer_offset(0), initiator(0),
      match_bits(0), header_data(0), request_length(0),
      payload_length(0), message_offset(0),
      list(0), return_code(RC_OK), modified_length(0),
      response_message_id(0), read_request_message_id(0),
      original_request_psn(0),
      hasAtomic(false),
      atomicExt{},
      m_hdrFormat(UET_HDR_REQUEST_STD)
{
}

UetSesHeader::~UetSesHeader() = default;

TypeId
UetSesHeader::GetTypeId()
{
    static TypeId tid = TypeId("ns3::UetSesHeader")
                            .SetParent<Header>()
                            .AddConstructor<UetSesHeader>();
    return tid;
}

TypeId
UetSesHeader::GetInstanceTypeId() const
{
    return GetTypeId();
}

// ─────────────────────────────────────────────────────────────────────────────
//  Static helpers
// ─────────────────────────────────────────────────────────────────────────────
uint32_t
UetSesHeader::FormatSize(UetSesNextHdr fmt, bool isSom1)
{
    switch (fmt)
    {
    case UET_HDR_REQUEST_STD:
        return isSom1 ? SZ_STD_SOM1 : SZ_STD_SOM0;
    case UET_HDR_REQUEST_SMALL:
        return SZ_SMALL;
    case UET_HDR_REQUEST_MEDIUM:
        return SZ_MEDIUM;
    case UET_HDR_RESPONSE:
        return SZ_RESPONSE;
    case UET_HDR_RESPONSE_DATA:
        return SZ_RESP_DATA;
    case UET_HDR_RESPONSE_DATA_SMALL:
        return SZ_RESP_SMALL;
    default:
        return 0;
    }
}

UetSesNextHdr
UetSesHeader::ChooseRequestFormat(UetSesOpcode op,
                                  bool         multiPacket,
                                  bool         needsMatchBits,
                                  bool         isResponse)
{
    if (isResponse)
    {
        return UET_HDR_RESPONSE;
    }
    if (multiPacket)
    {
        return UET_HDR_REQUEST_STD; // §3.4.2.1: multi-packet must use STD
    }
    bool isAtomic  = (op == UET_ATOMIC || op == UET_FETCHING_ATOMIC ||
                      op == UET_TSEND_ATOMIC || op == UET_TSEND_FETCH_ATOMIC);
    bool isSend    = (op == UET_SEND || op == UET_TAGGED_SEND ||
                      op == UET_DATAGRAM_SEND || op == UET_RENDEZVOUS_SEND ||
                      op == UET_RENDEZVOUS_TSEND);
    bool isRma     = (op == UET_WRITE || op == UET_READ || isAtomic);

    if (needsMatchBits || isSend)
        return UET_HDR_REQUEST_MEDIUM; // Small-msg / Small-RMA (Fig 3-14)
    if (isRma)
        return UET_HDR_REQUEST_SMALL;  // Optimized non-matching (Fig 3-13)
    return UET_HDR_REQUEST_STD;
}

// ═════════════════════════════════════════════════════════════════════════════
//  GetSerializedSize
// ═════════════════════════════════════════════════════════════════════════════
uint32_t
UetSesHeader::GetSerializedSize() const
{
    uint32_t base = FormatSize(m_hdrFormat, som);
    uint32_t ext  = (hasAtomic && m_hdrFormat != UET_HDR_RESPONSE &&
                     m_hdrFormat != UET_HDR_RESPONSE_DATA &&
                     m_hdrFormat != UET_HDR_RESPONSE_DATA_SMALL)
                        ? UetAtomicExtHdr::SIZE
                        : 0;
    return base + ext;
}

// ═════════════════════════════════════════════════════════════════════════════
//  Serialize
// ═════════════════════════════════════════════════════════════════════════════
void
UetSesHeader::Serialize(Buffer::Iterator start) const
{
    switch (m_hdrFormat)
    {
    case UET_HDR_REQUEST_STD:
        if (som) SerializeStdSom1(start);
        else     SerializeStdSom0(start);
        break;
    case UET_HDR_REQUEST_SMALL:
        SerializeSmall(start);
        break;
    case UET_HDR_REQUEST_MEDIUM:
        SerializeMedium(start);
        break;
    case UET_HDR_RESPONSE:
        SerializeResponse(start);
        break;
    case UET_HDR_RESPONSE_DATA:
        SerializeResponseData(start);
        break;
    case UET_HDR_RESPONSE_DATA_SMALL:
        SerializeResponseSmall(start);
        break;
    default:
        break;
    }
    if (hasAtomic && m_hdrFormat < UET_HDR_RESPONSE)
    {
        atomicExt.Serialize(start);
    }
}

// ─────────────────────────────────────────────────────────────────────────────
//  STD SOM=1  (Table 3-8, 44 bytes)
//  Byte layout (MSB first within each byte):
//   Byte 0: [rsvd(2) | opcode(6)]
//   Byte 1: [version(2) | dc(1) | ie(1) | rel(1) | hd(1) | eom(1) | som(1)]
//   Bytes 2-3: message_id(16)
//   Bytes 4-4: ri_generation(8)
//   Bytes 5-7: jobId(24)
//   Bytes 8-8: [rsvd(4) | pidOnFep_hi(4)]          \
//   Bytes 9-9: [pidOnFep_lo(8)]                      } pidOnFep = 12 bits
//   Bytes 10-10: [rsvd(4) | resource_index_hi(4)]   \
//   Bytes 11-11: [resource_index_lo(8)]               } resource_index = 12 bits
//   Bytes 12-19: buffer_offset(64)
//   Bytes 20-23: initiator(32)
//   Bytes 24-31: match_bits(64)
//   Bytes 32-39: header_data(64)
//   Bytes 40-43: request_length(32)
// ─────────────────────────────────────────────────────────────────────────────
void
UetSesHeader::SerializeStdSom1(Buffer::Iterator& i) const
{
    i.WriteU8((opcode & 0x3F));                                     // [rsvd=0, opcode[5:0]]
    i.WriteU8((version & 0x03) << 6 |
              (dc  ? 1 : 0) << 5 |
              (ie  ? 1 : 0) << 4 |
              (rel ? 1 : 0) << 3 |
              (hd  ? 1 : 0) << 2 |
              (eom ? 1 : 0) << 1 |
              (som ? 1 : 0));
    i.WriteHtonU16(message_id);
    i.WriteU8(ri_generation);
    i.WriteU8((jobId >> 16) & 0xFF);
    i.WriteU8((jobId >> 8)  & 0xFF);
    i.WriteU8( jobId        & 0xFF);
    // rsvd(4) | pidOnFep(12)
    i.WriteHtonU16((pidOnFep & 0x0FFF));
    // rsvd(4) | resource_index(12)
    i.WriteHtonU16((resource_index & 0x0FFF));
    i.WriteHtonU64(buffer_offset);
    i.WriteHtonU32(initiator);
    i.WriteHtonU64(match_bits);
    i.WriteHtonU64(header_data);
    i.WriteHtonU32(request_length);
}

// ─────────────────────────────────────────────────────────────────────────────
//  STD SOM=0  (Table 3-9, 32 bytes)
//  Same as SOM=1 but:
//    - header_data field is REPLACED by:  rsvd(18) | payload_length(14)
//    - adds: message_offset(32) after payload_length
//    - request_length still present
// ─────────────────────────────────────────────────────────────────────────────
void
UetSesHeader::SerializeStdSom0(Buffer::Iterator& i) const
{
    i.WriteU8((opcode & 0x3F));
    i.WriteU8((version & 0x03) << 6 |
              (dc  ? 1 : 0) << 5 |
              (ie  ? 1 : 0) << 4 |
              (rel ? 1 : 0) << 3 |
              (hd  ? 1 : 0) << 2 |
              (eom ? 1 : 0) << 1 |
              (som ? 1 : 0));
    i.WriteHtonU16(message_id);
    i.WriteU8(ri_generation);
    i.WriteU8((jobId >> 16) & 0xFF);
    i.WriteU8((jobId >> 8)  & 0xFF);
    i.WriteU8( jobId        & 0xFF);
    i.WriteHtonU16((pidOnFep & 0x0FFF));
    i.WriteHtonU16((resource_index & 0x0FFF));
    i.WriteHtonU64(buffer_offset);
    // rsvd(18) | payload_length(14) = 32 bits
    i.WriteHtonU32((payload_length & 0x3FFF));
    i.WriteHtonU32(message_offset);
    i.WriteHtonU32(request_length);
}

// ─────────────────────────────────────────────────────────────────────────────
//  SMALL — Optimized Non-Matching (Table 3-10, 20 bytes)
//  Byte 0: [rsvd(1) | opcode(6) | ...]  → same flag byte pattern
//  Byte 1: flags
//  Bytes 2-3: [rsvd(2) | request_length(14)]
//  Bytes 4-4: ri_generation(8)
//  Bytes 5-7: jobId(24)
//  Bytes 8-9: [rsvd(4) | pidOnFep(12)]
//  Bytes 10-11: [rsvd(4) | resource_index(12)]
//  Bytes 12-19: buffer_offset(64)
// ─────────────────────────────────────────────────────────────────────────────
void
UetSesHeader::SerializeSmall(Buffer::Iterator& i) const
{
    i.WriteU8((opcode & 0x3F));
    i.WriteU8((version & 0x03) << 6 |
              (dc  ? 1 : 0) << 5 |
              (ie  ? 1 : 0) << 4 |
              (rel ? 1 : 0) << 3 |
              0   /* hd rsvd */ << 2 |
              (eom ? 1 : 0) << 1 |
              (som ? 1 : 0));
    // rsvd(2) | request_length(14) — 14-bit abbreviated length (§3.4.2.2)
    i.WriteHtonU16(request_length & 0x3FFF);
    i.WriteU8(ri_generation);
    i.WriteU8((jobId >> 16) & 0xFF);
    i.WriteU8((jobId >> 8)  & 0xFF);
    i.WriteU8( jobId        & 0xFF);
    i.WriteHtonU16(pidOnFep & 0x0FFF);
    i.WriteHtonU16(resource_index & 0x0FFF);
    i.WriteHtonU64(buffer_offset);
}

// ─────────────────────────────────────────────────────────────────────────────
//  MEDIUM — Small-Msg / Small-RMA (Figure 3-14, 28 bytes)
//  Same prefix as SMALL but adds match_bits(64)
// ─────────────────────────────────────────────────────────────────────────────
void
UetSesHeader::SerializeMedium(Buffer::Iterator& i) const
{
    SerializeSmall(i);           // first 20 bytes identical
    i.WriteHtonU64(match_bits);  // + 8 bytes
}

// ─────────────────────────────────────────────────────────────────────────────
//  RESPONSE (Table 3-11, 12 bytes)
//  Byte 0: [list(2) | opcode(6)]
//  Byte 1: [version(2) | return_code(6)]
//  Bytes 2-3: message_id(16)
//  Byte  4:   ri_generation(8)
//  Bytes 5-7: jobId(24)
//  Bytes 8-11: modified_length(32)
// ─────────────────────────────────────────────────────────────────────────────
void
UetSesHeader::SerializeResponse(Buffer::Iterator& i) const
{
    i.WriteU8(((list & 0x03) << 6) | (opcode & 0x3F));
    i.WriteU8(((version & 0x03) << 6) | (return_code & 0x3F));
    i.WriteHtonU16(message_id);
    i.WriteU8(ri_generation);
    i.WriteU8((jobId >> 16) & 0xFF);
    i.WriteU8((jobId >> 8)  & 0xFF);
    i.WriteU8( jobId        & 0xFF);
    i.WriteHtonU32(modified_length);
}

// ─────────────────────────────────────────────────────────────────────────────
//  RESPONSE_DATA large (Table 3-12, 20 bytes)
//  Byte 0: [list(2) | opcode(6)]
//  Byte 1: [version(2) | return_code(6)]
//  Bytes 2-3: response_message_id(16)
//  Byte  4:   rsvd(8)
//  Bytes 5-7: jobId(24)
//  Bytes 8-9:  read_request_message_id(16)
//  Bytes 10-11:[rsvd(2) | payload_length(14)]
//  Bytes 12-15: modified_length(32)
//  Bytes 16-19: message_offset(32)
// ─────────────────────────────────────────────────────────────────────────────
void
UetSesHeader::SerializeResponseData(Buffer::Iterator& i) const
{
    i.WriteU8(((list & 0x03) << 6) | (opcode & 0x3F));
    i.WriteU8(((version & 0x03) << 6) | (return_code & 0x3F));
    i.WriteHtonU16(response_message_id);
    i.WriteU8(0); // rsvd
    i.WriteU8((jobId >> 16) & 0xFF);
    i.WriteU8((jobId >> 8)  & 0xFF);
    i.WriteU8( jobId        & 0xFF);
    i.WriteHtonU16(read_request_message_id);
    i.WriteHtonU16(payload_length & 0x3FFF); // rsvd(2) | payload_length(14)
    i.WriteHtonU32(modified_length);
    i.WriteHtonU32(message_offset);
}

// ─────────────────────────────────────────────────────────────────────────────
//  RESPONSE_DATA_SMALL (Table 3-13, 12 bytes)
//  Byte 0: [list(2) | opcode(6)]
//  Byte 1: [version(2) | return_code(6)]
//  Bytes 2-3: [rsvd(2) | payload_length(14)]
//  Byte  4:   rsvd(8)
//  Bytes 5-7: jobId(24)
//  Bytes 8-11: original_request_psn(32)
// ─────────────────────────────────────────────────────────────────────────────
void
UetSesHeader::SerializeResponseSmall(Buffer::Iterator& i) const
{
    i.WriteU8(((list & 0x03) << 6) | (opcode & 0x3F));
    i.WriteU8(((version & 0x03) << 6) | (return_code & 0x3F));
    i.WriteHtonU16(payload_length & 0x3FFF);
    i.WriteU8(0); // rsvd
    i.WriteU8((jobId >> 16) & 0xFF);
    i.WriteU8((jobId >> 8)  & 0xFF);
    i.WriteU8( jobId        & 0xFF);
    i.WriteHtonU32(original_request_psn);
}

// ═════════════════════════════════════════════════════════════════════════════
//  Deserialize
// ═════════════════════════════════════════════════════════════════════════════
uint32_t
UetSesHeader::Deserialize(Buffer::Iterator start)
{
    // The first byte's lower 6 bits (or upper 2+6) tell us the format.
    // Caller (PDS layer) sets m_hdrFormat before calling Deserialize().
    Buffer::Iterator i = start;
    uint32_t bytes = 0;

    switch (m_hdrFormat)
    {
    case UET_HDR_REQUEST_STD:
    {
        // The som bit sits at bit 0 of the second byte in both the SOM=1
        // (44-byte) and SOM=0 (32-byte) layouts. Read it from the wire:
        // trusting a caller-preset value mis-parses continuation packets
        // as SOM=1 and silently swallows 12 payload bytes.
        Buffer::Iterator peek = start;
        peek.ReadU8();
        som = (peek.ReadU8() & 0x01) != 0;
        bytes = som ? DeserializeStdSom1(i) : DeserializeStdSom0(i);
        break;
    }
    case UET_HDR_REQUEST_SMALL:
        bytes = DeserializeSmall(i);
        break;
    case UET_HDR_REQUEST_MEDIUM:
        bytes = DeserializeMedium(i);
        break;
    case UET_HDR_RESPONSE:
        bytes = DeserializeResponse(i);
        break;
    case UET_HDR_RESPONSE_DATA:
        bytes = DeserializeResponseData(i);
        break;
    case UET_HDR_RESPONSE_DATA_SMALL:
        bytes = DeserializeResponseSmall(i);
        break;
    default:
        return 0;
    }

    bool isRequest = (m_hdrFormat == UET_HDR_REQUEST_STD ||
                      m_hdrFormat == UET_HDR_REQUEST_SMALL ||
                      m_hdrFormat == UET_HDR_REQUEST_MEDIUM);
    if (hasAtomic && isRequest)
    {
        atomicExt = UetAtomicExtHdr::Deserialize(i);
        bytes += UetAtomicExtHdr::SIZE;
    }
    return bytes;
}

uint32_t
UetSesHeader::DeserializeStdSom1(Buffer::Iterator& i)
{
    uint8_t b0 = i.ReadU8();
    uint8_t b1 = i.ReadU8();
    opcode     = b0 & 0x3F;
    version    = (b1 >> 6) & 0x03;
    dc         = (b1 >> 5) & 1;
    ie         = (b1 >> 4) & 1;
    rel        = (b1 >> 3) & 1;
    hd         = (b1 >> 2) & 1;
    eom        = (b1 >> 1) & 1;
    som        = (b1     ) & 1;
    message_id     = i.ReadNtohU16();
    ri_generation  = i.ReadU8();
    jobId          = ((uint32_t)i.ReadU8() << 16) | ((uint32_t)i.ReadU8() << 8) | i.ReadU8();
    pidOnFep       = i.ReadNtohU16() & 0x0FFF;
    resource_index = i.ReadNtohU16() & 0x0FFF;
    buffer_offset  = i.ReadNtohU64();
    initiator      = i.ReadNtohU32();
    match_bits     = i.ReadNtohU64();
    header_data    = i.ReadNtohU64();
    request_length = i.ReadNtohU32();
    return SZ_STD_SOM1;
}

uint32_t
UetSesHeader::DeserializeStdSom0(Buffer::Iterator& i)
{
    uint8_t b0 = i.ReadU8();
    uint8_t b1 = i.ReadU8();
    opcode     = b0 & 0x3F;
    version    = (b1 >> 6) & 0x03;
    dc         = (b1 >> 5) & 1;
    ie         = (b1 >> 4) & 1;
    rel        = (b1 >> 3) & 1;
    hd         = (b1 >> 2) & 1;
    eom        = (b1 >> 1) & 1;
    som        = (b1     ) & 1;
    message_id     = i.ReadNtohU16();
    ri_generation  = i.ReadU8();
    jobId          = ((uint32_t)i.ReadU8() << 16) | ((uint32_t)i.ReadU8() << 8) | i.ReadU8();
    pidOnFep       = i.ReadNtohU16() & 0x0FFF;
    resource_index = i.ReadNtohU16() & 0x0FFF;
    buffer_offset  = i.ReadNtohU64();
    payload_length = i.ReadNtohU32() & 0x3FFF;
    message_offset = i.ReadNtohU32();
    request_length = i.ReadNtohU32();
    return SZ_STD_SOM0;
}

uint32_t
UetSesHeader::DeserializeSmall(Buffer::Iterator& i)
{
    uint8_t b0 = i.ReadU8();
    uint8_t b1 = i.ReadU8();
    opcode     = b0 & 0x3F;
    version    = (b1 >> 6) & 0x03;
    dc         = (b1 >> 5) & 1;
    ie         = (b1 >> 4) & 1;
    rel        = (b1 >> 3) & 1;
    eom        = (b1 >> 1) & 1;
    som        = (b1     ) & 1;
    request_length = i.ReadNtohU16() & 0x3FFF;
    ri_generation  = i.ReadU8();
    jobId          = ((uint32_t)i.ReadU8() << 16) | ((uint32_t)i.ReadU8() << 8) | i.ReadU8();
    pidOnFep       = i.ReadNtohU16() & 0x0FFF;
    resource_index = i.ReadNtohU16() & 0x0FFF;
    buffer_offset  = i.ReadNtohU64();
    return SZ_SMALL;
}

uint32_t
UetSesHeader::DeserializeMedium(Buffer::Iterator& i)
{
    DeserializeSmall(i);
    match_bits = i.ReadNtohU64();
    return SZ_MEDIUM;
}

uint32_t
UetSesHeader::DeserializeResponse(Buffer::Iterator& i)
{
    uint8_t b0 = i.ReadU8();
    uint8_t b1 = i.ReadU8();
    list        = (b0 >> 6) & 0x03;
    opcode      =  b0       & 0x3F;
    version     = (b1 >> 6) & 0x03;
    return_code =  b1       & 0x3F;
    message_id     = i.ReadNtohU16();
    ri_generation  = i.ReadU8();
    jobId          = ((uint32_t)i.ReadU8() << 16) | ((uint32_t)i.ReadU8() << 8) | i.ReadU8();
    modified_length = i.ReadNtohU32();
    return SZ_RESPONSE;
}

uint32_t
UetSesHeader::DeserializeResponseData(Buffer::Iterator& i)
{
    uint8_t b0 = i.ReadU8();
    uint8_t b1 = i.ReadU8();
    list        = (b0 >> 6) & 0x03;
    opcode      =  b0       & 0x3F;
    version     = (b1 >> 6) & 0x03;
    return_code =  b1       & 0x3F;
    response_message_id      = i.ReadNtohU16();
    i.ReadU8(); // rsvd
    jobId = ((uint32_t)i.ReadU8() << 16) | ((uint32_t)i.ReadU8() << 8) | i.ReadU8();
    read_request_message_id  = i.ReadNtohU16();
    payload_length           = i.ReadNtohU16() & 0x3FFF;
    modified_length          = i.ReadNtohU32();
    message_offset           = i.ReadNtohU32();
    return SZ_RESP_DATA;
}

uint32_t
UetSesHeader::DeserializeResponseSmall(Buffer::Iterator& i)
{
    uint8_t b0 = i.ReadU8();
    uint8_t b1 = i.ReadU8();
    list        = (b0 >> 6) & 0x03;
    opcode      =  b0       & 0x3F;
    version     = (b1 >> 6) & 0x03;
    return_code =  b1       & 0x3F;
    payload_length        = i.ReadNtohU16() & 0x3FFF;
    i.ReadU8(); // rsvd
    jobId = ((uint32_t)i.ReadU8() << 16) | ((uint32_t)i.ReadU8() << 8) | i.ReadU8();
    original_request_psn  = i.ReadNtohU32();
    return SZ_RESP_SMALL;
}

// ═════════════════════════════════════════════════════════════════════════════
//  Print
// ═════════════════════════════════════════════════════════════════════════════
void
UetSesHeader::Print(std::ostream& os) const
{
    static const char* fmtNames[] = {
        "NONE","SMALL","MEDIUM","STD","RESPONSE","RESP_DATA","RESP_SMALL"};
    os << "[SES fmt=" << fmtNames[m_hdrFormat]
       << " op=0x" << std::hex << (int)opcode
       << " som=" << som << " eom=" << eom
       << " jobId=0x" << jobId
       << " msgId=" << std::dec << message_id
       << " ri=" << resource_index
       << " len=" << request_length << "]";
}

} // namespace ns3
