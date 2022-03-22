#ifndef PARAMS_H
#define PARAMS_H

#include <cinttypes>

/* Macros for printing using RTE_LOG */
#define RTE_LOGTYPE_APP RTE_LOGTYPE_USER1

/* Max size of a single packet */
#define MAX_PACKET_SZ 2048

/* Size of the data buffer in each mbuf */
#define MBUF_DATA_SZ (MAX_PACKET_SZ + RTE_PKTMBUF_HEADROOM)

/* Number of mbufs in pktmbuf mempool */
#define NB_MBUF (8192 * 64)

/* How many packets to attempt to read from NIC in one go */
#define PKT_BURST_SZ 32

/* How many objects (mbufs) to keep in per-lcore mempool cache */
#define MEMPOOL_CACHE_SZ PKT_BURST_SZ

/* Number of mbufs in xgmii mempool */
#define XGMII_NB_MBUF (8192 * 2048)

/* How many XGMII frames per packet burst */
#define XGMII_BURST_SZ 2 * (PKT_BURST_SZ * MAX_PACKET_SZ / 64)

/* Size of the data buffer in each mbuf */
#define XGMII_MBUF_SZ (64 + 8 + RTE_PKTMBUF_HEADROOM)

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

#endif
