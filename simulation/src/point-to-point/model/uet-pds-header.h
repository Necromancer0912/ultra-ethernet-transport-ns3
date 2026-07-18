/**
 * uet-pds-header.h  —  Ultra Ethernet Packet Delivery Sublayer headers
 *
 * Implements all PDS wire formats from UE-Specification-1.0.2 §3.5.10:
 *
 *  §3.5.10.1  Entropy header       (4 bytes, only for native IP, not UDP)
 *  §3.5.10.2  PDS Prologue         (2 bytes, starts every PDS header)
 *  §3.5.10.3  RUD/ROD Request      (12 bytes total)
 *  §3.5.10.4  RUD/ROD Req + CC     (16 bytes)
 *  §3.5.10.5  RUD/ROD ACK          (12 bytes)
 *  §3.5.10.6  RUD/ROD ACK_CC       (32 bytes — ACK 12 + CC 20)
 *  §3.5.10.7  RUD/ROD ACK_CCX      (44 bytes — ACK 12 + CCX 32)
 *  §3.5.10.8  RUD/ROD CP           (14+ bytes)
 *  §3.5.10.9  RUDI Request/Response(6 bytes)
 *  §3.5.10.10 NACK                 (14 bytes)
 *  §3.5.10.11 NACK_CCX             (28 bytes)
 *  §3.5.10.12 UUD Request          (4 bytes)
 *
 *  And NACK codes from §3.5.12.7 Table 3-58.
 */

#ifndef UET_PDS_HEADER_H
#define UET_PDS_HEADER_H

#include "uet-ses-header.h"

#include "ns3/header.h"
#include "ns3/packet.h"

#include <cstdint>

namespace ns3
{

// ─────────────────────────────────────────────────────────────────────────────
//  §3.5.10.2  pds.type  (5 bits) — Table 3-32
// ─────────────────────────────────────────────────────────────────────────────
enum UetPdsType : uint8_t
{
    PDS_TYPE_RESERVED   = 0,
    PDS_TYPE_TSS        = 1,  // UET Encryption header (not implemented here)
    PDS_TYPE_RUD_REQ    = 2,  // RUD Request
    PDS_TYPE_ROD_REQ    = 3,  // ROD Request
    PDS_TYPE_RUDI_REQ   = 4,  // RUDI Request
    PDS_TYPE_RUDI_RESP  = 5,  // RUDI Response
    PDS_TYPE_UUD_REQ    = 6,  // UUD Request
    PDS_TYPE_ACK        = 7,  // RUD/ROD ACK
    PDS_TYPE_ACK_CC     = 8,  // RUD/ROD ACK with CC state
    PDS_TYPE_ACK_CCX    = 9,  // RUD/ROD ACK with extended CC
    PDS_TYPE_NACK       = 10, // NACK
    PDS_TYPE_CP         = 11, // Control Packet (subtype in ctl_type field)
    PDS_TYPE_NACK_CCX   = 12, // NACK with extended CC
    PDS_TYPE_RUD_CC_REQ = 13, // RUD Request with CC state
    PDS_TYPE_ROD_CC_REQ = 14, // ROD Request with CC state
};

// ─────────────────────────────────────────────────────────────────────────────
//  §3.5.10.8  pds.ctl_type (4 bits) for CP packets — Table 3-38
// ─────────────────────────────────────────────────────────────────────────────
enum UetPdsCtlType : uint8_t
{
    CP_NOOP            = 0,  // No-operation; can open a PDC
    CP_ACK_REQUEST     = 1,  // Request ACK for a specific PSN
    CP_CLEAR_COMMAND   = 2,  // Initiator → target: clear guaranteed-delivery state
    CP_CLEAR_REQUEST   = 3,  // Target → initiator: please send a clear
    CP_CLOSE_COMMAND   = 4,  // Initiator: PDC is closing
    CP_CLOSE_REQUEST   = 5,  // Target: requests initiator to close PDC
    CP_PROBE           = 6,  // Source → destination: request ACK
    CP_CREDIT          = 7,  // Destination → source: CC credit
    CP_CREDIT_REQUEST  = 8,  // Source → destination: request credit
    CP_NEGOTIATION     = 9,  // PDC capability negotiation
};

// ─────────────────────────────────────────────────────────────────────────────
//  §3.5.12.7  NACK codes  (8 bits)  — Table 3-58
// ─────────────────────────────────────────────────────────────────────────────
enum UetPdsNackCode : uint8_t
{
    NACK_RESERVED           = 0x00,  // Reserved
    NACK_TRIMMED            = 0x01,  // NORMAL/RETX  — Packet trimmed
    NACK_TRIMMED_LASTHOP    = 0x02,  // NORMAL/RETX  — Trimmed at last-hop switch
    NACK_TRIMMED_ACK        = 0x03,  // NORMAL/RETX  — ACK carrying read data was trimmed
    NACK_NO_PDC_AVAIL       = 0x04,  // NORMAL/RETRY — No PDC resource (spdcid=0)
    NACK_NO_CCC_AVAIL       = 0x05,  // NORMAL/RETRY — No CCC resource (spdcid=0)
    NACK_NO_BITMAP          = 0x06,  // NORMAL/RETRY — No bitmap resource (spdcid=0)
    NACK_NO_PKT_BUFFER      = 0x07,  // NORMAL/RETX  — No packet buffer
    NACK_NO_GTD_DEL_AVAIL   = 0x08,  // NORMAL/RETX  — No guaranteed-delivery response slot
    NACK_NO_SES_MSG_AVAIL   = 0x09,  // NORMAL/RETX  — No message-tracking state
    NACK_NO_RESOURCE        = 0x0A,  // NORMAL/RETX  — Generic resource unavailable
    NACK_PSN_OOR_WINDOW     = 0x0B,  // NORMAL/RETX  — PSN outside tracking window
    // 0x0C reserved
    NACK_ROD_OOO            = 0x0D,  // NORMAL/RETX  — Out-of-order on ROD PDC
    NACK_INV_DPDCID         = 0x0E,  // PDC_FATAL/RETRY — Unknown dpdcid (spdcid=0)
    NACK_PDC_HDR_MISMATCH   = 0x0F,  // PDC_FATAL/RETRY — Header mismatch
    NACK_CLOSING            = 0x10,  // PDC_FATAL/RETRY — PDC is closing
    NACK_INVALID_SYN        = 0x11,  // PDC_ERR/RETX — SYN received on established PDC past MP_RANGE
    NACK_PDC_MODE_MISMATCH  = 0x12,  // PDC_FATAL/CLOSE — Mode mismatch (RUD vs ROD)
    NACK_NEW_START_PSN      = 0x13,  // NORMAL/WAIT+RETX — Encrypted PDC: use given Start_PSN
    // 0x14 reserved
};

// ─────────────────────────────────────────────────────────────────────────────
//  §3.5.11.8  pds.flags bit definitions
//  Stored as individual booleans; Serialize packs them into 7 bits
// ─────────────────────────────────────────────────────────────────────────────
struct UetPdsFlags
{
    bool retx;   // 1 => This packet is a retransmit      (req/CP/ack)
    bool ar;     // 1 => ACK-Request; destination must ACK (req/CP)
    bool syn;    // 1 => PDC establishment (SYN) flag       (req/CP)
    bool isrod;  // 1 => PDC is ROD (NOOP/Negotiation CP only)
    bool m;      // 1 => Associated request was ECN-marked (ACK/NACK)
    bool p;      // 1 => This ACK is for a Probe CP        (ACK)
    bool req;    // 1 => Requests a clear or close          (ACK)
    bool nt;     // 1 => NACK type = RUDI                  (NACK)

    UetPdsFlags() : retx(false), ar(false), syn(false), isrod(false),
                    m(false), p(false), req(false), nt(false) {}
};

// ─────────────────────────────────────────────────────────────────────────────
//  PDS Prologue — first 2 bytes of every PDS header (Table 3-32)
//  Layout: [type(5) | next_hdr_or_ctl(4) | flags(7)] = 16 bits
// ─────────────────────────────────────────────────────────────────────────────
struct UetPdsPrologue
{
    uint8_t     pdsType;    // UetPdsType (5 bits)
    uint8_t     nextHdr;    // UetSesNextHdr or UetPdsCtlType (4 bits)
    UetPdsFlags flags;      // 7 bits, interpretation per pdsType

    // Serialize/deserialize helpers for the 2-byte prologue
    void     Write(Buffer::Iterator& i, bool isCP = false) const;
    void     Read (Buffer::Iterator& i, bool isCP = false);
    uint16_t ToU16(bool isCP = false) const;
};

// ─────────────────────────────────────────────────────────────────────────────
//  Main PDS header class
//  One class handles all PDS packet types. The caller sets pdsType to
//  select which fields are used and which wire format is serialized.
// ─────────────────────────────────────────────────────────────────────────────
class UetPdsHeader : public Header
{
  public:
    UetPdsHeader();
    ~UetPdsHeader() override;

    static TypeId GetTypeId();
    TypeId  GetInstanceTypeId() const override;
    void    Serialize(Buffer::Iterator start) const override;
    uint32_t Deserialize(Buffer::Iterator start) override;
    uint32_t GetSerializedSize() const override;
    void    Print(std::ostream& os) const override;

    // ── Prologue (present in all types) ──────────────────────────────────────
    UetPdsPrologue  prologue;

    // ── RUD/ROD Request fields (Table 3-33, §3.5.10.3) ───────────────────────
    int16_t  clear_psn_offset; // 16-bit signed: CLEAR_PSN = psn + clear_psn_offset
    uint32_t psn;              // 32-bit packet sequence number
    uint16_t spdcid;           // Source PDCID (16 bits)

    // dpdcid is overloaded per §3.5.8.2:
    //   syn=0: dpdcid = destination PDCID (16 bits)
    //   syn=1: {pdc_info(4) | psn_offset(12)} packed into 16 bits
    uint16_t dpdcid;           // destination PDCID  OR
    uint8_t  pdc_info;         // 4 bits: use_rsv_pdc (used when syn=1)
    uint16_t psn_offset;       // 12 bits: PSN − Start_PSN (used when syn=1)

    // ── RUD/ROD CC state (Table 3-34, §3.5.10.4) ─────────────────────────────
    uint32_t req_cc_state;     // 32 bits (content per §3.6.9.1)

    // ── RUD/ROD ACK fields (Table 3-35, §3.5.10.5) ───────────────────────────
    int16_t  ack_psn_offset;   // 16-bit signed: ACK_PSN = cack_psn + ack_psn_offset
    uint16_t probe_opaque;     // 16 bits: copied from Probe CP (when p=1)
    uint32_t cack_psn;         // 32-bit cumulative ACK PSN (§3.5.11.4.5)
    // spdcid / dpdcid shared with request fields

    // ── ACK_CC extra fields (Table 3-36, §3.5.10.6) ──────────────────────────
    uint8_t  cc_type;          // 4 bits
    uint8_t  cc_flags;         // 4 bits (reserved)
    uint8_t  mpr;              // 8 bits: Maximum PSN Range
    int16_t  sack_psn_offset;  // 16-bit signed: SACK_PSN = cack_psn + sack_psn_offset
    uint64_t sack_bitmap;      // 64 bits: 1=received, bit i → SACK_PSN+i
    uint64_t ack_cc_state;     // 64 bits (CC-algo specific, §3.6.9)

    // ── ACK_CCX extra state (Table 3-37, §3.5.10.7) ──────────────────────────
    // Uses same cc_type/cc_flags/mpr/sack fields as ACK_CC
    // ack_ccx_state is 128 bits = 2×uint64
    uint64_t ack_ccx_state_lo; // low 64 bits
    uint64_t ack_ccx_state_hi; // high 64 bits

    // ── CP extra fields (Table 3-38, §3.5.10.8) ──────────────────────────────
    // probe_opaque, psn, spdcid, dpdcid shared with above
    uint32_t cp_payload;       // 32 bits: CP-type-specific (§3.5.16.8)

    // ── RUDI Request/Response (Table 3-39, §3.5.10.9) ────────────────────────
    uint32_t pkt_id;           // 32 bits: locally unique RUDI packet ID

    // ── NACK fields (Table 3-40, §3.5.10.10) ─────────────────────────────────
    uint8_t  nack_code;        // 8 bits: UetPdsNackCode
    uint8_t  vendor_code;      // 8 bits: vendor-specific (optional, ignored)
    uint32_t nack_psn;         // 32 bits: PSN of packet that triggered NACK
                               //   (also used as nack_pkt_id for RUDI if nt=1)
    // spdcid / dpdcid  (0 when PDC not established)
    uint32_t nack_payload;     // 32 bits: code-specific (e.g., Start_PSN for NACK_NEW_START_PSN)

    // ── Helpers ───────────────────────────────────────────────────────────────
    bool IsRequest()  const;   // true for RUD/ROD/RUDI/UUD request types
    bool IsAck()      const;   // true for ACK/ACK_CC/ACK_CCX
    bool IsNack()     const;   // true for NACK/NACK_CCX
    bool IsCP()       const;   // true for CP
    bool IsRudi()     const;   // true for RUDI_REQ or RUDI_RESP
    bool IsUud()      const;   // true for UUD_REQ

    // Compute the actual CLEAR_PSN from a received request header
    uint32_t ComputeClearPsn() const { return psn + (int32_t)clear_psn_offset; }
    // Compute ACK_PSN from a received ACK header
    uint32_t ComputeAckPsn()   const { return cack_psn + (int32_t)ack_psn_offset; }
    // Compute SACK_PSN from a received ACK_CC header
    uint32_t ComputeSackPsn()  const { return cack_psn + (int32_t)sack_psn_offset; }

    // Build the overloaded dpdcid field for SYN packets (§3.5.8.2)
    void     SetSynDpdcid(uint8_t pdcInfo4, uint16_t psnOffset12);
    void     GetSynDpdcid(uint8_t& pdcInfo4, uint16_t& psnOffset12) const;

    // Human-readable NACK code name
    static const char* NackCodeName(uint8_t code);

  private:
    void SerializeRudRodReq     (Buffer::Iterator& i) const;
    void SerializeRudRodAck     (Buffer::Iterator& i) const;
    void SerializeRudRodAckCc   (Buffer::Iterator& i) const;
    void SerializeRudRodAckCcx  (Buffer::Iterator& i) const;
    void SerializeCP            (Buffer::Iterator& i) const;
    void SerializeRudi          (Buffer::Iterator& i) const;
    void SerializeNack          (Buffer::Iterator& i) const;
    void SerializeUud           (Buffer::Iterator& i) const;

    uint32_t DeserializeRudRodReq    (Buffer::Iterator& i);
    uint32_t DeserializeRudRodAck    (Buffer::Iterator& i);
    uint32_t DeserializeRudRodAckCc  (Buffer::Iterator& i);
    uint32_t DeserializeRudRodAckCcx (Buffer::Iterator& i);
    uint32_t DeserializeCP           (Buffer::Iterator& i);
    uint32_t DeserializeRudi         (Buffer::Iterator& i);
    uint32_t DeserializeNack         (Buffer::Iterator& i);
    uint32_t DeserializeUud          (Buffer::Iterator& i);
};

} // namespace ns3

#endif // UET_PDS_HEADER_H
