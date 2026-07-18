#ifndef RDMA_HW_UET_INTEGRATION_H
#define RDMA_HW_UET_INTEGRATION_H

#include "uet-ses-pds-engine.h"
#include "uet-pds-control-packet.h"
#include "uet-pds-target-state.h"
#include "uet-ses-header.h"
#include "uet-pds-header.h"
#include <ns3/ptr.h>
#include <ns3/object.h>
#include <ns3/packet.h>
#include <unordered_map>

namespace ns3
{

    /**
     * @class RdmaHwUetIntegration
     * @brief Bridges SES/PDS reliability engine with RDMA hardware send/receive
     *
     * Integrates the UET Serialized Endpoint (SES) and Packet Delivery Service (PDS)
     * with RDMA hardware paths. Manages:
     * - Outbound packet reliability (TX path)
     * - Inbound packet tracking and response generation (RX path)
     * - PDC lifecycle and resource management
     * - Control packet generation and handling
     *
     * Spec Reference: Sections 3.4-3.5 (SES/PDS), 3.5.14 (Packet Delivery Behavior)
     */
    class RdmaHwUetIntegration : public Object
    {
    public:
        static TypeId GetTypeId(void);
        RdmaHwUetIntegration();
        ~RdmaHwUetIntegration();

        /**
         * Initialize integration with topology context
         * @param numNodes Number of nodes in topology
         * @param linkBandwidth Link bandwidth in bps
         * @param mtuBytes Configured MTU
         */
        void Initialize(uint32_t numNodes, uint64_t linkBandwidth, uint32_t mtuBytes);

        /**
         * Process outbound packet before transmission
         * Applies SES/PDS reliability headers and ordering constraints
         *
         * @param p Packet to process
         * @param srcId Source node ID
         * @param dstId Destination node ID
         * @param sport Source port (flows)
         * @param jobId Job ID (defines PDC)
         * @param mode Delivery mode (RUD/ROD/RUDI/UUD)
         * @param dropRate Simulated request drop rate (0.0-1.0)
         * @return Updated packet; nullptr if dropped
         */
        Ptr<Packet> ProcessTxPacket(
            Ptr<Packet> p,
            uint32_t srcId,
            uint32_t dstId,
            uint16_t sport,
            uint32_t jobId,
            uint8_t mode,
            double dropRate);

        /**
         * Process inbound packet after reception
         * Tracks PSN, manages resource indices, generates responses
         *
         * @param p Packet received
         * @param srcId Source node ID
         * @param dstId Destination node ID  (this node)
         * @param sport Source port
         * @param jobId Job ID (defines PDC)
         * @param mode Delivery mode
         * @param dropRate Simulated ACK drop rate
         * @return true if response should be generated
         */
        bool ProcessRxPacket(
            Ptr<Packet> p,
            uint32_t srcId,
            uint32_t dstId,
            uint16_t sport,
            uint32_t jobId,
            uint8_t mode,
            double dropRate);

        /**
         * Generate target-side default response for received packet
         * @param srcId Source node ID
         * @param dstId Destination node ID
         * @param sport Source port
         * @param jobId Job ID
         * @param mode Delivery mode
         * @return Response code (UET_NO_ERROR/UET_DEFAULT_RESPONSE/etc)
         */
        uint8_t GenerateDefaultResponse(
            uint32_t srcId,
            uint32_t dstId,
            uint16_t sport,
            uint32_t jobId,
            uint8_t mode);

        /**
         * Get control packet to send (e.g., CLEAR_COMMAND when resource limited)
         * @param pdcId PDC identifier
         * @return Control packet pointer, or nullptr if none pending
         */
        UetPdsHeader *GetPendingControlPacket(uint64_t pdcId);

        /**
         * Describe state for debugging/telemetry
         */
        std::string DescribeState() const;

    private:
        UetSesPdsEngine m_txEngine;                                                ///< TX-side SES/PDS engine
        UetPdsTargetState m_rxState;                                               ///< RX-side target state tracker
        std::unordered_map<uint64_t, uint32_t> m_pdcSequence;                      ///< PSN per PDC for ROD ordering
        std::unordered_map<uint64_t, uint32_t> m_resourceCounter;                  ///< Resource index counter per PDC
        std::unordered_map<uint64_t, UetPdsHeader> m_pendingControlPackets; ///< Pending control packets per PDC
        std::unordered_map<uint64_t, uint32_t> m_lastPsn;                          ///< Last seen PSN per PDC (replaces static local)
        uint32_t m_nodeCount;
        uint64_t m_linkBandwidth;
        uint32_t m_configuredMtuBytes;
        bool m_initialized;

        // Helper methods
        uint64_t ComputePdcId(uint32_t srcId, uint32_t dstId, uint16_t sport, uint32_t jobId);
        uint32_t AllocateResourceIndex(uint64_t pdcId);
    };

} // namespace ns3

#endif /* RDMA_HW_UET_INTEGRATION_H */
