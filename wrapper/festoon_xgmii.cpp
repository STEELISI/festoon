#include "festoon_xgmii.h"

#include <rte_byteorder.h>
#include <rte_mbuf.h>

rte_ring *xgmii_tx_queue_ctrl, *xgmii_rx_queue_ctrl, *xgmii_tx_queue_data,
    *xgmii_rx_queue_data;

void init_xgmii_worker(rte_ring *worker_tx_ring, rte_ring *worker_rx_ring) {
  // Generate TX and RX queues for pkt_mbufs
  worker_tx_ring = rte_ring_create("Worker ring output", 1024, rte_socket_id(),
                                   RING_F_SC_DEQ);
  worker_rx_ring = rte_ring_create("Worker ring input", 1024, rte_socket_id(),
                                   RING_F_SC_DEQ);

  // Generate TX and RX queues for XGMII
  xgmii_tx_queue_ctrl = rte_ring_create(
      "XGMII control transmit queue", 1024 * 8, rte_socket_id(), RING_F_SC_DEQ);
  xgmii_tx_queue_data = rte_ring_create("XGMII data transmit queue", 1024 * 8,
                                        rte_socket_id(), RING_F_SC_DEQ);
  xgmii_rx_queue_ctrl = rte_ring_create("XGMII control recieve queue", 1024 * 8,
                                        rte_socket_id(), RING_F_SC_DEQ);
  xgmii_rx_queue_data = rte_ring_create("XGMII data recieve queue", 1024 * 8,
                                        rte_socket_id(), RING_F_SC_DEQ);
}

// Read from 
void xgmii_worker_rx(kni_interface_stats *kni_stats, rte_ring *worker_rx_ring) {
  uint8_t i;
  rte_mbuf *pkts_burst[PKT_BURST_SZ] __rte_cache_aligned;
  CData *xgm_ctrl_buf[PKT_BURST_SZ * MAX_PACKET_SZ / 64 + 1] __rte_cache_aligned;
  QData *xgm_data_buf[PKT_BURST_SZ * MAX_PACKET_SZ / 64 + 1] __rte_cache_aligned;
  int32_t f_stop, f_pause;
  uint16_t port_id;
  unsigned int nb_tx, nb_rx;

  // -1 is not set, 1 is NXDOMAIN, 0 is normal
  int8_t packet_status[PKT_BURST_SZ] = {-1};

  // Burst RX from ring
  nb_rx = rte_ring_dequeue_burst(worker_rx_ring, (void **)pkts_burst, PKT_BURST_SZ,
                                 nullptr);
  if (unlikely(nb_rx > PKT_BURST_SZ)) {
    RTE_LOG(ERR, APP, "Error receiving from worker\n");
    return;
  }

  // Create rte_mbuf packets from XGMII packets
  *xgm_data_buf[0] = 0xd5555555555555fb;
  *xgm_ctrl_buf[0] = 0x00011111;
  for (i = 0; i < nb_rx; i++) {
  }

  // Pass DPDK packets to xgmii_rx_queue
  nb_tx = rte_ring_enqueue_burst(get_xgmii_rx_queue_ctrl(), (void **)pkts_burst,
                                 PKT_BURST_SZ, nullptr);
  nb_tx = rte_ring_enqueue_burst(get_xgmii_rx_queue_data(), (void **)pkts_burst,
                                 PKT_BURST_SZ, nullptr);

  if (unlikely(nb_tx < nb_rx)) {
    // Free mbufs not tx to xgmii_rx_queue
    kni_stats[port_id].rx_dropped += nb_rx - nb_tx;
  }
}

void xgmii_worker_tx(kni_interface_stats *kni_stats, rte_ring *worker_tx_ring) {
  uint8_t i, it, iter;
  uint16_t port_id;
  unsigned int nb_tx, nb_rx, xgm_buf_counter = 1;
  uint32_t nb_kni;
  rte_mbuf *pkts_burst[PKT_BURST_SZ] __rte_cache_aligned;
  CData *xgm_ctrl_buf[PKT_BURST_SZ] __rte_cache_aligned;
  QData *xgm_data_buf[PKT_BURST_SZ] __rte_cache_aligned;

  // Burst rx from kni
  nb_rx = rte_ring_dequeue_burst(get_xgmii_tx_queue_ctrl(), (void **)xgm_ctrl_buf,
                                 PKT_BURST_SZ, nullptr);
  nb_rx = rte_ring_dequeue_burst(get_xgmii_tx_queue_data(), (void **)xgm_data_buf,
                                 PKT_BURST_SZ, nullptr);
  if (unlikely(nb_rx > PKT_BURST_SZ)) {
    RTE_LOG(ERR, APP, "Error receiving from KNI\n");
    return;
  }

  // Create DPDk packets
  for (it = 0; it < nb_rx; it++) {
    for (iter = 0; iter < rte_pktmbuf_pkt_len(pkts_burst[nb_rx]); iter += 64)
      xgm_buf_counter++;
  }

  // Burst tx to ring with replies
  nb_tx = rte_ring_enqueue_burst(worker_tx_ring, (void **)pkts_burst,
                                 PKT_BURST_SZ * MAX_PACKET_SZ / 64, nullptr);
  if (nb_tx) kni_stats[port_id].tx_packets += nb_tx;
  if (unlikely(nb_tx < nb_rx)) {
    // Free mbufs not tx to NIC
    kni_stats[port_id].tx_dropped += nb_rx - nb_tx;
  }
}

void stop_xgmii_worker(rte_ring *worker_tx_ring, rte_ring *worker_rx_ring) {
  rte_ring_free(xgmii_tx_queue_ctrl);
  rte_ring_free(xgmii_rx_queue_ctrl);
  rte_ring_free(xgmii_tx_queue_data);
  rte_ring_free(xgmii_rx_queue_data);

  rte_ring_free(worker_tx_ring);
  rte_ring_free(worker_rx_ring);
}

rte_ring *get_xgmii_tx_queue_ctrl() { return xgmii_tx_queue_ctrl; }

rte_ring *get_xgmii_rx_queue_ctrl() { return xgmii_rx_queue_ctrl; }

rte_ring *get_xgmii_tx_queue_data() { return xgmii_tx_queue_data; }

rte_ring *get_xgmii_rx_queue_data() { return xgmii_rx_queue_data; }
