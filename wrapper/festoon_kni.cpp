#include <rte_kni.h>
#include <rte_mbuf.h>

#include "festoon_common.h"
#include "festoon_kni.h"

// Push mbufs from ring into KNI TX
void kni_egress(kni_port_params *p, rte_ring *tx_ring)
{
  uint8_t i;
  uint16_t port_id;
  unsigned nb_rx, nb_tx;
  uint32_t nb_kni;
  struct rte_mbuf *pkts_burst[PKT_BURST_SZ];

  if (p == NULL)
    return;

  nb_kni = p->nb_kni;
  port_id = p->port_id;
  for (i = 0; i < nb_kni; i++) {
    // Burst rx from tx_ring
    nb_rx = rte_ring_dequeue_burst(tx_ring, (void **)pkts_burst, PKT_BURST_SZ, nullptr);
    if (unlikely(nb_rx > PKT_BURST_SZ)) {
      RTE_LOG(ERR, APP, "Error transmitting to KNI\n");
      return;
    }

    // Burst rx to kni
    nb_tx = rte_kni_tx_burst(p->kni[i], pkts_burst, nb_rx);
    if (nb_tx) get_kni_stats()[port_id].kni_rx_packets += nb_tx;

    rte_kni_handle_request(p->kni[i]);
    if (unlikely(nb_tx < nb_rx)) {
      // Free mbufs not tx to kni interface
      kni_burst_free_mbufs(&pkts_burst[nb_tx], nb_rx - nb_tx);
      get_kni_stats()[port_id].kni_rx_dropped += nb_rx - nb_tx;
    }
  }
}

// Push mbufs from KNI RX into ring
void kni_ingress(kni_port_params *p, rte_ring *rx_ring)
{
  uint8_t i;
  uint16_t port_id;
  unsigned nb_tx, nb_rx;
  uint32_t nb_kni;
  struct rte_mbuf *pkts_burst[PKT_BURST_SZ];

  if (p == NULL)
    return;

  nb_kni = p->nb_kni;
  port_id = p->port_id;
  for (i = 0; i < nb_kni; i++) {
    // Burst rx from kni
    nb_rx = rte_kni_rx_burst(p->kni[i], pkts_burst, PKT_BURST_SZ);
    if (unlikely(nb_rx > PKT_BURST_SZ)) {
      RTE_LOG(ERR, APP, "Error receiving from KNI\n");
      return;
    }

    // Burst tx to ring
    nb_tx = rte_ring_enqueue_burst(rx_ring, (void **)pkts_burst, nb_rx, NULL);
    if (nb_tx) get_kni_stats()[port_id].kni_tx_packets += nb_tx;

    if (unlikely(nb_tx < nb_rx)) {
      // Free mbufs not tx to NIC
      kni_burst_free_mbufs(&pkts_burst[nb_tx], nb_rx - nb_tx);
      get_kni_stats()[port_id].kni_tx_dropped += nb_rx - nb_tx;
    }
  }
}
