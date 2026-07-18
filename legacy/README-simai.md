# AeroFabric: Ultra Ethernet Transport (UET) Simulator

<p align="center">
  <img src="https://img.shields.io/badge/Specification-UE--1.0.2-blue?style=for-the-badge" />
  <img src="https://img.shields.io/badge/Simulator-NS--3.36.1-green?style=for-the-badge" />
  <img src="https://img.shields.io/badge/Profiles-AI%20Base%20%7C%20AI%20Full%20%7C%20HPC-purple?style=for-the-badge" />
</p>

**AeroFabric** is a high-fidelity, spec-compliant NS-3 simulation engine for the Ultra Ethernet Consortium (UEC) Transport protocol. It implements the complete Serialized Endpoint Service (SES), Packet Delivery Service (PDS), and Packet Delivery Context (PDC) layers, dynamically demonstrating real-world AI and HPC workloads—from tensor-parallel KV cache exchanges to MPI collective operations—over 200Gbps+ fabrics.

Featuring a stunning interactive dashboard for real-time packet inspection and state-machine tracking, AeroFabric bridges the gap between hardware specification and visual protocol analytics, making it the perfect tool for network architecture research and interview demonstrations.

## 🚀 Key Features

*   **Complete Protocol Stack**: Full implementation of UE-Spec-1.0.2 including SES headers (§3.4), PDS delivery modes (§3.5), and PDC connection management (§3.5.8).
*   **AI & HPC Profiles**: Demonstrates the three core UET profiles—**AI Base**, **AI Full**, and **HPC**—using realistic workload simulations like AllReduce rings, Tagged-Send rendezvous, and SHMEM one-sided memory operations.
*   **Interactive Dashboard**: A premium UI offering live metrics, packet format inspection, and an animated PDC state timeline.
*   **One-Click execution**: Fully containerized run scripts for executing specific scenarios and exploring output telemetry.
