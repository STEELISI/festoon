#include <rte_kni.h>
#include <rte_mbuf.h>

#include "festoon_common.h"
#include "festoon_kni.h"

// Push mbufs from ring into KNI RX
void kni_egress(kni_port_params *p, rte_ring *tx_ring)
{
  uint8_t i;
  uint16_t port_id;
  unsigned nb_rx, num;
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
      RTE_LOG(ERR, APP, "Error transmitting from KNI\n");
      return;
    }

    // Burst rx to kni
    num = rte_kni_tx_burst(p->kni[i], pkts_burst, nb_rx);
    if (num) get_kni_stats()[port_id].kni_rx_packets += num;

    rte_kni_handle_request(p->kni[i]);
    if (unlikely(num < nb_rx)) {
      // Free mbufs not tx to kni interface
      kni_burst_free_mbufs(&pkts_burst[num], nb_rx - num);
      get_kni_stats()[port_id].kni_rx_dropped += nb_rx - num;
    }
  }
}

// Push mbufs from KNI TX into ring
void kni_ingress(kni_port_params *p, rte_ring *rx_ring)
{
  uint8_t i;
  uint16_t port_id;
  unsigned nb_tx, num;
  uint32_t nb_kni;
  struct rte_mbuf *pkts_burst[PKT_BURST_SZ];

  if (p == NULL)
    return;

  nb_kni = p->nb_kni;
  port_id = p->port_id;
  for (i = 0; i < nb_kni; i++) {
    // Burst rx from kni
    num = rte_kni_rx_burst(p->kni[i], pkts_burst, PKT_BURST_SZ);
    if (unlikely(num > PKT_BURST_SZ)) {
      RTE_LOG(ERR, APP, "Error receiving from KNI\n");
      return;
    }

    // Burst tx to ring
    nb_tx = rte_ring_enqueue_burst(rx_ring, (void **)pkts_burst, num, NULL);
    if (nb_tx) get_kni_stats()[port_id].kni_tx_packets += nb_tx;

    if (unlikely(nb_tx < num)) {
      // Free mbufs not tx to NIC
      kni_burst_free_mbufs(&pkts_burst[nb_tx], num - nb_tx);
      get_kni_stats()[port_id].kni_tx_dropped += num - nb_tx;
    }
  }
}
