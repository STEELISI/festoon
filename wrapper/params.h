#ifndef PARAMS_H
#define PARAMS_H

#include <cinttypes>

/* Macros for printing using RTE_LOG */
#define RTE_LOGTYPE_APP RTE_LOGTYPE_USER1

/* Max size of a single packet */
#define MAX_PACKET_SZ 2048

/* Size of the data buffer in each mbuf */
#define MBUF_DATA_SZ (MAX_PACKET_SZ + RTE_PKTMBUF_HEADROOM)

/* Number of mbufs in mempool that is created */
#define NB_MBUF (8192 * 64)

/* How many packets to attempt to read from NIC in one go */
#define PKT_BURST_SZ 32

/* How many objects (mbufs) to keep in per-lcore mempool cache */
#define MEMPOOL_CACHE_SZ PKT_BURST_SZ

/* Number of RX ring descriptors */
#define NB_RXD 1024

/* Number of TX ring descriptors */
#define NB_TXD 1024

/* Total octets in ethernet header */
#define KNI_ENET_HEADER_SIZE 14

/* Total octets in the FCS */
#define KNI_ENET_FCS_SIZE 4

#define KNI_US_PER_SECOND 1000000
#define KNI_SECOND_PER_DAY 86400

#define KNI_MAX_KTHREAD 32

/*
 * Structure of port parameters
 */
struct kni_port_params {
  uint16_t port_id;                 // Port ID
  unsigned lcore_rx;                // lcore ID for RX
  unsigned lcore_tx;                // lcore ID for TX
  unsigned lcore_worker_translate;  // lcore ID for XGMII translation worker
  unsigned lcore_worker_vtop;       // lcore ID for Verilator worker
  uint32_t nb_lcore_k;  // Number of lcores for KNI multi kernel threads
  uint32_t nb_kni;      // Number of KNI devices to be created
  unsigned lcore_k[KNI_MAX_KTHREAD];     // lcore ID list for kthreads
  struct rte_kni *kni[KNI_MAX_KTHREAD];  // KNI context pointers
} __rte_cache_aligned;

/* Structure type for recording kni interface specific stats */
struct kni_interface_stats {
  /* number of pkts received from NIC, and sent to KNI */
  uint64_t rx_packets;

  /* number of pkts received from NIC, but failed to send to KNI */
  uint64_t rx_dropped;

  /* number of pkts received from KNI, and sent to NIC */
  uint64_t tx_packets;

  /* number of pkts received from KNI, but failed to send to NIC */
  uint64_t tx_dropped;
};

#endif
