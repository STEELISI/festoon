#include "festoon_xgmii.h"

#include <rte_mbuf.h>

struct rte_ring *xgmii_tx_queue, *xgmii_rx_queue;

void init_xgmii_worker(rte_ring *worker_tx_ring, rte_ring *worker_rx_ring) {
  // Generate TX and RX queues for XGMII
  xgmii_tx_queue = rte_ring_create("XGMII transmit queue", 1024 * 8,
                                   rte_socket_id(), RING_F_SC_DEQ);
  xgmii_rx_queue = rte_ring_create("XGMII recieve queue", 1024 * 8,
                                   rte_socket_id(), RING_F_SC_DEQ);

  worker_tx_ring = rte_ring_create("Worker ring output", 1024, rte_socket_id(),
                                   RING_F_SC_DEQ);
  worker_rx_ring = rte_ring_create("Worker ring input", 1024, rte_socket_id(),
                                   RING_F_SC_DEQ);
}

// Read from kni_ingress and send non-DNS packets to worker_egress
void xgmii_worker_rx(kni_interface_stats *kni_stats,
                     rte_ring *worker_rx_ring) {
  uint8_t i;
  struct rte_mbuf *pkts_burst[16] __rte_cache_aligned;
  int32_t f_stop, f_pause;
  uint16_t port_id;
  unsigned int nb_tx, nb_rx;

  // -1 is not set, 1 is NXDOMAIN, 0 is normal
  int8_t packet_status[PKT_BURST_SZ] = {-1};

  // Burst RX from ring
  nb_rx = rte_ring_dequeue_burst(worker_rx_ring, (void **)pkts_burst,
                                 PKT_BURST_SZ, nullptr);
  if (unlikely(nb_rx > PKT_BURST_SZ)) {
    RTE_LOG(ERR, APP, "Error receiving from worker\n");
    return;
  }

  // Pass good packets to xgmii_rx_queue
  nb_tx = rte_ring_enqueue_burst(xgmii_rx_queue, (void **)pkts_burst,
                                 PKT_BURST_SZ, nullptr);

  if (unlikely(nb_tx < nb_rx)) {
    // Free mbufs not tx to xgmii_rx_queue
    kni_stats[port_id].rx_dropped += nb_rx - nb_tx;
  }
}

void xgmii_worker_tx(kni_interface_stats *kni_stats,
                     rte_ring *worker_tx_ring) {
  uint8_t i, it;
  uint16_t port_id;
  unsigned int nb_tx, nb_rx;
  uint32_t nb_kni;
  struct rte_mbuf *pkts_burst[PKT_BURST_SZ] __rte_cache_aligned;

  // Burst rx from kni
  nb_rx = rte_ring_dequeue_burst(xgmii_tx_queue, (void **)pkts_burst,
                                 PKT_BURST_SZ, nullptr);
  if (unlikely(nb_rx > PKT_BURST_SZ)) {
    RTE_LOG(ERR, APP, "Error receiving from KNI\n");
    return;
  }

  // Create replies
  for (it = 0; it < nb_rx; it++) {
  }

  // Burst tx to ring with replies
  nb_tx = rte_ring_enqueue_burst(worker_tx_ring, (void **)pkts_burst,
                                 PKT_BURST_SZ, nullptr);
  if (nb_tx) kni_stats[port_id].tx_packets += nb_tx;
  if (unlikely(nb_tx < nb_rx)) {
    // Free mbufs not tx to NIC
    kni_stats[port_id].tx_dropped += nb_rx - nb_tx;
  }
}

void stop_xgmii_worker(rte_ring *worker_tx_ring, rte_ring *worker_rx_ring) {
  rte_ring_free(xgmii_tx_queue);
  rte_ring_free(xgmii_rx_queue);

  rte_ring_free(worker_tx_ring);
  rte_ring_free(worker_rx_ring);
}

rte_ring *get_xgmii_tx() { return xgmii_tx_queue; }

rte_ring *get_xgmii_rx() { return xgmii_rx_queue; }
