/**
 * uet-ses-header.h
 *
 * Ultra Ethernet (UE) Semantic Sublayer (SES) header definitions.
 *
 * Implements all header formats from UE-Specification-1.0.2 §3.4.2:
 *   - Table 3-8:  Standard header SOM=1  (UET_HDR_REQUEST_STD)
 *   - Table 3-9:  Standard header SOM=0  (continuation packet)
 *   - Table 3-10: Optimized Non-Matching  (UET_HDR_REQUEST_SMALL)
 *   - Table 3-14: Small-Message/Small-RMA (UET_HDR_REQUEST_MEDIUM)
 *   - Table 3-11: Response               (UET_HDR_RESPONSE)
 *   - Table 3-12: Response with Data     (UET_HDR_RESPONSE_DATA)
 *   - Table 3-13: Optimized Resp+Data    (UET_HDR_RESPONSE_DATA_SMALL)
 *   - Figure 3-16: Atomic extension header
 *
 * Notes on bit packing (spec §3.4.2):
 *   All multi-byte integers are big-endian on the wire (network byte order).
 *   Bit fields within a byte increase from the MSB. We store decoded values
 *   in plain integer fields and do bit-manipulation explicitly in Serialize().
 */

#ifndef UET_SES_HEADER_H
#define UET_SES_HEADER_H

#include "ns3/header.h"
#include "ns3/packet.h"

#include <cstdint>

namespace ns3
{

// ─────────────────────────────────────────────────────────────────────────────
//  Delivery mode (mirrors PDS mode)
// ─────────────────────────────────────────────────────────────────────────────
enum UetDeliveryMode : uint8_t
{
    UET_MODE_RUD  = 0,
    UET_MODE_ROD  = 1,
    UET_MODE_RUDI = 2,
    UET_MODE_UUD  = 3,
};

// ─────────────────────────────────────────────────────────────────────────────
//  §3.4.6.2  SES Opcode (6 bits)
// ─────────────────────────────────────────────────────────────────────────────
enum UetSesOpcode : uint8_t
{
    UET_NO_OP               = 0x00, ///< No operation (NOP)
    UET_WRITE               = 0x01, ///< RMA write
    UET_READ                = 0x02, ///< RMA read
    UET_ATOMIC              = 0x03, ///< Non-fetching atomic
    UET_FETCHING_ATOMIC     = 0x04, ///< Fetching atomic (read-modify-write)
    UET_SEND                = 0x05, ///< Two-sided send (no tag)
    UET_TAGGED_SEND         = 0x06, ///< Tagged send
    UET_DATAGRAM_SEND       = 0x07, ///< Datagram send
    UET_TSEND_ATOMIC        = 0x08, ///< Tagged send with atomic
    UET_TSEND_FETCH_ATOMIC  = 0x09, ///< Tagged send, fetching atomic
    UET_RENDEZVOUS_SEND     = 0x0A, ///< Rendezvous send
    UET_RENDEZVOUS_TSEND    = 0x0B, ///< Rendezvous tagged send
    UET_DEFERRABLE_SEND     = 0x0C, ///< Deferrable send
    UET_DEFERRABLE_RTR      = 0x0D, ///< Ready-to-restart for deferrable send
    UET_RESPONSE            = 0x20, ///< Semantic ACK response (→ pds.next_hdr UET_HDR_RESPONSE)
    UET_DEFAULT_RESPONSE    = 0x21, ///< Default coalesced response
    UET_NO_RESPONSE         = 0x22, ///< Early ACK, semantic not yet done
    UET_RESPONSE_W_DATA     = 0x23, ///< Response carrying read return data
};

// ─────────────────────────────────────────────────────────────────────────────
//  §3.4.2.6  pds.next_hdr type — selects which SES header format is present
// ─────────────────────────────────────────────────────────────────────────────
enum UetSesNextHdr : uint8_t
{
    UET_HDR_NONE                = 0x00, ///< No SES header (PDS-autonomous packet)
    UET_HDR_REQUEST_SMALL       = 0x01, ///< Optimized non-matching (Fig 3-13, Table 3-10)
    UET_HDR_REQUEST_MEDIUM      = 0x02, ///< Small-msg / Small-RMA (Fig 3-14)
    UET_HDR_REQUEST_STD         = 0x03, ///< Standard header (Fig 3-9/3-10, Tables 3-8/3-9)
    UET_HDR_RESPONSE            = 0x04, ///< Semantic response (Fig 3-18, Table 3-11)
    UET_HDR_RESPONSE_DATA       = 0x05, ///< Response with data, large (Fig 3-19, Table 3-12)
    UET_HDR_RESPONSE_DATA_SMALL = 0x06, ///< Response with data, optimized (Fig 3-20, Table 3-13)
};

// ─────────────────────────────────────────────────────────────────────────────
//  §3.4.6.3  SES Return Code (6 bits inside response headers)
// ─────────────────────────────────────────────────────────────────────────────
enum UetSesReturnCode : uint8_t
{
    RC_OK               = 0x00, ///< Success
    RC_RETRY_REQUIRED   = 0x01, ///< Temporary failure, retry
    RC_NO_RESPONSE      = 0x02, ///< Early ACK, no semantic info
    RC_DEFAULT_RESPONSE = 0x03, ///< Default coalesced response
    RC_INVALID_JOB      = 0x10, ///< JobID authorization failure
    RC_AUTH_FAILURE     = 0x11, ///< General authorization failure
    RC_INTERNAL_ERROR   = 0x3F, ///< Internal / unspecified error
};

// ─────────────────────────────────────────────────────────────────────────────
//  §3.4.6.3  SES List (2 bits) – used in response headers
// ─────────────────────────────────────────────────────────────────────────────
enum UetSesListCode : uint8_t
{
    UET_EXPECTED   = 0, ///< Delivered to expected list
    UET_UNEXPECTED = 1, ///< Delivered to unexpected list
};

// ─────────────────────────────────────────────────────────────────────────────
//  §3.4.2.4  Atomic extension header (Figure 3-16, 32 bits)
// ─────────────────────────────────────────────────────────────────────────────
struct UetAtomicExtHdr
{
    uint8_t  atomic_opcode; ///< 8 bits: atomic operation type
    uint8_t  atomic_dtype;  ///< 8 bits: data type size/kind
    uint8_t  sem_ctrl;      ///< 8 bits: semantic control (Table 3-23)
    uint8_t  rsvd;          ///< 8 bits: reserved = 0

    void Serialize(Buffer::Iterator& i) const;
    static UetAtomicExtHdr Deserialize(Buffer::Iterator& i);
    static constexpr uint32_t SIZE = 4;
};

// ─────────────────────────────────────────────────────────────────────────────
//  Main SES header class
//  Holds all possible fields; Serialize() writes only the relevant subset
//  based on hdrFormat and ses.som.
// ─────────────────────────────────────────────────────────────────────────────
class UetSesHeader : public Header
{
  public:
    // ── Construction ─────────────────────────────────────────────────────────
    UetSesHeader();
    ~UetSesHeader() override;

    static TypeId GetTypeId();
    TypeId GetInstanceTypeId() const override;

    // ── ns3::Header interface ─────────────────────────────────────────────────
    void        Serialize(Buffer::Iterator start) const override;
    uint32_t    Deserialize(Buffer::Iterator start) override;
    uint32_t    GetSerializedSize() const override;
    void        Print(std::ostream& os) const override;

    // ── Format selector ──────────────────────────────────────────────────────
    // Call this before setting fields to configure which format will be
    // used for Serialize() / GetSerializedSize().
    void SetFormat(UetSesNextHdr fmt) { m_hdrFormat = fmt; }
    UetSesNextHdr GetFormat() const   { return m_hdrFormat; }

    // ── Common flags (all request formats) ───────────────────────────────────
    uint8_t  opcode;       ///< 6-bit UetSesOpcode
    uint8_t  version;      ///< 2-bit protocol version = 0
    bool     dc;           ///< Delivery-complete flag (§3.4.8.3)
    bool     ie;           ///< Initiator-error flag (§3.4.5.4.1)
    bool     rel;          ///< Relative addressing (§3.4.1.3)
    bool     hd;           ///< Header-data present (§3.4.1.9)
    bool     eom;          ///< End-of-message (§3.4.1.7.3)
    bool     som;          ///< Start-of-message (§3.4.1.7.3)

    // ── Standard / Optimized request fields ─────────────────────────────────
    uint16_t message_id;       ///< 16 bits (§3.4.1.14); 0 = invalid
    uint8_t  ri_generation;    ///< 8  bits (§3.4.3.6.3)
    uint32_t jobId;            ///< 24 bits (§3.4.1.3.1); stored in 32-bit
    uint16_t pidOnFep;         ///< 12 bits (§3.4.1.3.2)
    uint16_t resource_index;   ///< 12 bits (§3.4.1.3.3)
    uint64_t buffer_offset;    ///< 64 bits (§3.4.1.3)
    uint32_t initiator;        ///< 32 bits (§3.4.1.3.4)
    uint64_t match_bits;       ///< 64 bits match_bits OR memory_key (§3.4.1.3.5)
    uint64_t header_data;      ///< 64 bits (§3.4.1.11) – present only when som=1
    uint32_t request_length;   ///< 32 bits full extent of message

    // ── SOM=0 continuation fields (Standard format, any multi-packet message) ─
    uint16_t payload_length;   ///< 14 bits: bytes in this specific packet
    uint32_t message_offset;   ///< 32 bits: offset in message for this packet (som=0)

    // ── Optimized formats – abbreviated request_length ───────────────────────
    // Table 3-10 / Table 3-14 use 14-bit abbreviated length stored in request_length
    // (upper 18 bits are zero when request_length < 8192; same field used)

    // ── Response header fields (§3.4.2.5) ────────────────────────────────────
    uint8_t  list;             ///< 2 bits: UetSesListCode
    uint8_t  return_code;      ///< 6 bits: UetSesReturnCode
    uint32_t modified_length;  ///< 32 bits: bytes delivered at target

    // ── Response-with-Data extra fields (Table 3-12) ─────────────────────────
    uint16_t response_message_id;       ///< 16 bits: response message ID
    uint16_t read_request_message_id;   ///< 16 bits: original read message ID

    // ── Optimised Response-with-Data (Table 3-13) ────────────────────────────
    uint32_t original_request_psn;      ///< 32 bits: PSN of read/atomic request

    // ── Atomic extension header (optional, appended after base header) ────────
    bool               hasAtomic;
    UetAtomicExtHdr    atomicExt;

    // ── Helper: choose correct format for a given opcode/message size ─────────
    // Returns the UetSesNextHdr appropriate for sending a request.
    // multiPacket = true  → must use STD; false → may use smaller format.
    static UetSesNextHdr ChooseRequestFormat(UetSesOpcode op,
                                             bool          multiPacket,
                                             bool          needsMatchBits,
                                             bool          isResponse = false);
    // Returns serialised byte size for a specific next_hdr value and flags.
    static uint32_t FormatSize(UetSesNextHdr fmt, bool som);

  private:
    UetSesNextHdr m_hdrFormat; ///< which wire format to emit

    // ── Private serialisers per format ───────────────────────────────────────
    void SerializeStdSom1       (Buffer::Iterator& i) const;
    void SerializeStdSom0       (Buffer::Iterator& i) const;
    void SerializeSmall         (Buffer::Iterator& i) const; // SMALL (optimized non-matching)
    void SerializeMedium        (Buffer::Iterator& i) const; // MEDIUM (small-msg/small-RMA)
    void SerializeResponse      (Buffer::Iterator& i) const;
    void SerializeResponseData  (Buffer::Iterator& i) const;
    void SerializeResponseSmall (Buffer::Iterator& i) const;

    uint32_t DeserializeStdSom1       (Buffer::Iterator& i);
    uint32_t DeserializeStdSom0       (Buffer::Iterator& i);
    uint32_t DeserializeSmall         (Buffer::Iterator& i);
    uint32_t DeserializeMedium        (Buffer::Iterator& i);
    uint32_t DeserializeResponse      (Buffer::Iterator& i);
    uint32_t DeserializeResponseData  (Buffer::Iterator& i);
    uint32_t DeserializeResponseSmall (Buffer::Iterator& i);
};

} // namespace ns3

#endif // UET_SES_HEADER_H
