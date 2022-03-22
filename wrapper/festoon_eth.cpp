#include <rte_ethdev.h>
#include <rte_ring.h>

#include "festoon_common.h"
#include "festoon_eth.h"

/**
 * Interface to burst rx and enqueue mbufs into rx_q
 */
void eth_ingress(kni_port_params *p, rte_ring *worker_rx_ring) {
  uint8_t i;
  uint16_t port_id;
  unsigned nb_rx, nb_tx;
  uint32_t nb_kni;
  struct rte_mbuf *pkts_burst[PKT_BURST_SZ];

  if (p == NULL) return;

  nb_kni = p->nb_kni;
  port_id = p->port_id;
  for (i = 0; i < nb_kni; i++) {
    /* Burst rx from eth */
    nb_rx = rte_eth_rx_burst(port_id, 0, pkts_burst, PKT_BURST_SZ);
    if (unlikely(nb_rx > PKT_BURST_SZ)) {
      RTE_LOG(ERR, APP, "Error transmitting from eth\n");
      return;
    }
    /* Burst tx to worker_rx_ring */
    nb_tx = rte_ring_enqueue_burst(worker_rx_ring, (void **)pkts_burst, nb_rx, NULL);

    if (nb_tx) get_kni_stats()[port_id].eth_rx_packets += nb_tx;

    if (unlikely(nb_tx < nb_rx)) {
      /* Free mbufs not tx to kni interface */
      kni_burst_free_mbufs(&pkts_burst[nb_tx], nb_rx - nb_tx);
      get_kni_stats()[port_id].eth_rx_dropped += nb_rx - nb_tx;
    }
  }
}

/**
 * Interface to dequeue mbufs from tx_q and burst tx
 */
void eth_egress(kni_port_params *p, rte_ring *worker_tx_ring) {
  uint8_t i;
  uint16_t port_id;
  unsigned nb_tx, nb_rx;
  uint32_t nb_kni;
  struct rte_mbuf *pkts_burst[PKT_BURST_SZ];

  if (p == NULL) return;

  nb_kni = p->nb_kni;
  port_id = p->port_id;
  for (i = 0; i < nb_kni; i++) {
    if (rte_ring_empty(worker_tx_ring)) return;

    /* Burst rx from kni */
    nb_rx = rte_ring_dequeue_burst(worker_tx_ring, (void **)pkts_burst, PKT_BURST_SZ, nullptr);
    if (unlikely(nb_rx > PKT_BURST_SZ)) {
      RTE_LOG(ERR, APP, "Error receiving from eth\n");
      return;
    }

    /* Burst tx to eth */
    nb_tx = rte_eth_tx_burst(port_id, 0, pkts_burst, (uint16_t) nb_rx);

    if (nb_tx) get_kni_stats()[port_id].eth_tx_packets += nb_tx;

    if (unlikely(nb_tx < nb_rx)) {
      /* Free mbufs not tx to NIC */
      kni_burst_free_mbufs(&pkts_burst[nb_tx], nb_rx - nb_tx);
      get_kni_stats()[port_id].eth_tx_dropped += nb_rx - nb_tx;
    }
  }
}