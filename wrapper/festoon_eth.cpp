#include <rte_ethdev.h>
#include <rte_ring.h>

#include "festoon_common.h"
#include "festoon_eth.h"

/**
 * Interface to burst rx and enqueue mbufs into rx_q
 */
static void eth_ingress(struct kni_port_params *p, rte_ring *worker_rx_ring) {
  uint8_t i;
  uint16_t port_id;
  unsigned nb_rx, num;
  uint32_t nb_kni;
  struct rte_mbuf *pkts_burst[PKT_BURST_SZ];

  if (p == NULL) return;

  nb_kni = p->nb_kni;
  port_id = p->port_id;
  for (i = 0; i < nb_kni; i++) {
    /* Burst rx from eth */
    nb_rx = rte_eth_rx_burst(port_id, 0, pkts_burst, PKT_BURST_SZ);
    if (unlikely(nb_rx > PKT_BURST_SZ)) {
      RTE_LOG(ERR, APP, "Error receiving from eth\n");
      return;
    }
    /* Burst tx to worker_rx_ring */
    num = rte_ring_enqueue_burst(worker_rx_ring, (void **)pkts_burst, nb_rx,
                                 NULL);
    if (unlikely(num < nb_rx)) {
      /* Free mbufs not tx to kni interface */
      kni_burst_free_mbufs(&pkts_burst[num], nb_rx - num);
      // kni_stats[port_id].rx_dropped += nb_rx - num;
    }
  }
}

/**
 * Interface to dequeue mbufs from tx_q and burst tx
 */
static void eth_egress(struct kni_port_params *p, rte_ring *worker_tx_ring) {
  uint8_t i;
  uint16_t port_id;
  unsigned nb_tx, num;
  uint32_t nb_kni;
  struct rte_mbuf *pkts_burst[PKT_BURST_SZ];

  if (p == NULL) return;

  nb_kni = p->nb_kni;
  port_id = p->port_id;
  for (i = 0; i < nb_kni; i++) {
    /* Burst rx from kni */
    num = rte_ring_dequeue_burst(worker_tx_ring, (void **)pkts_burst,
                                 PKT_BURST_SZ, nullptr);
    if (unlikely(num > PKT_BURST_SZ)) {
      RTE_LOG(ERR, APP, "Error receiving from KNI\n");
      return;
    }
    /* Burst tx to eth */
    nb_tx = rte_eth_tx_burst(port_id, 0, pkts_burst, (uint16_t)num);
    // if (nb_tx) kni_stats[port_id].tx_packets += nb_tx;
    if (unlikely(nb_tx < num)) {
      /* Free mbufs not tx to NIC */
      kni_burst_free_mbufs(&pkts_burst[nb_tx], num - nb_tx);
      // kni_stats[port_id].tx_dropped += num - nb_tx;
    }
  }
}