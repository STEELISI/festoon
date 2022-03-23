#ifndef FESTOON_COMMON_H
#define FESTOON_COMMON_H

#include <rte_mbuf.h>

#include "params.h"

void kni_burst_free_mbufs(rte_mbuf **pkts, unsigned num);

// Structure of port parameters
struct kni_port_params {
  uint16_t port_id;            // Port ID
  unsigned lcore_eth_rx;       // lcore ID for RX
  unsigned lcore_eth_tx;       // lcore ID for TX
  unsigned lcore_eth_mii_rx;   // lcore ID for XGMII recieve worker
  unsigned lcore_eth_mii_tx;   // lcore ID for XGMII transmit worker
  unsigned lcore_kni_rx;       // lcore ID for RX
  unsigned lcore_kni_tx;       // lcore ID for TX
  unsigned lcore_kni_mii_rx;   // lcore ID for XGMII recieve worker
  unsigned lcore_kni_mii_tx;   // lcore ID for XGMII transmit worker
  unsigned lcore_worker_vtop;  // lcore ID for Verilator worker
  uint32_t nb_lcore_k;         // Number of lcores for KNI multi kernel threads
  uint32_t nb_kni;             // Number of KNI devices to be created
  unsigned lcore_k[KNI_MAX_KTHREAD];     // lcore ID list for kthreads
  struct rte_kni *kni[KNI_MAX_KTHREAD];  // KNI context pointers
} __rte_cache_aligned;

// Structure type for recording kni interface specific stats
struct kni_interface_stats {
  uint64_t eth_rx_packets; // number of pkts received from NIC, and sent to XGMII
  uint64_t eth_rx_dropped; // number of pkts received from NIC, but failed to send to XGMII
  uint64_t eth_tx_packets; // number of pkts received from XGMII, and sent to NIC
  uint64_t eth_tx_dropped; // number of pkts received from XGMII, but failed to send to NIC
  uint64_t kni_rx_packets; // number of pkts received from KNI, and sent to XGMII
  uint64_t kni_rx_dropped; // number of pkts received from KNI, but failed to send to XGMII
  uint64_t kni_tx_packets; // number of pkts received from XGMII, and sent to KNI
  uint64_t kni_tx_dropped; // number of pkts received from XGMII, but failed to send to KNI
  uint64_t xgmii_rx_packets[2]; // number of pkts received from DPDK, and sent to FPGA
  uint64_t xgmii_rx_dropped[2]; // number of pkts received from DPDK, but failed to send to FPGA
  uint64_t xgmii_tx_packets[2]; // number of pkts received from FPGA, and sent to DPDK
  uint64_t xgmii_tx_dropped[2]; // number of pkts received from FPGA, but failed to send to DPDK
};

kni_interface_stats *get_kni_stats();

#endif