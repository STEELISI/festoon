#include "festoon_xgmii.h"

#include <rte_byteorder.h>
#include <rte_malloc.h>
#include <rte_mbuf.h>

#include <stdexcept>

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

// Read from RX ring and send to FPGA
void xgmii_worker_rx(kni_interface_stats *kni_stats, rte_ring *worker_rx_ring) {
  uint8_t i, it;
  rte_mbuf *pkts_burst[PKT_BURST_SZ] __rte_cache_aligned;
  CData *xgm_ctrl_buf[XGMII_BURST_SZ] __rte_cache_aligned;
  QData *xgm_data_buf[XGMII_BURST_SZ] __rte_cache_aligned;
  int32_t f_stop, f_pause;
  uint16_t port_id;
  unsigned int nb_tx_ctrl, nb_tx_data, nb_rx, xgm_buf_counter = 0;

  // Burst RX from ring
  nb_rx = rte_ring_dequeue_burst(worker_rx_ring, (void **)pkts_burst,
                                 PKT_BURST_SZ, nullptr);
  if (unlikely(nb_rx > PKT_BURST_SZ)) {
    RTE_LOG(ERR, APP, "Error receiving from worker\n");
    return;
  }

  // Create rte_mbuf packets from XGMII packets
  for (i = 0; i < nb_rx; i++) {
    // Send packet begin control bits
    *xgm_data_buf[xgm_buf_counter] = 0xd5555555555555fb;
    *xgm_ctrl_buf[xgm_buf_counter] = 0b00000001;
    xgm_buf_counter++;

    // Go through packet and move every 64 bits to XGM data buffer
    for (it = 0; it < rte_pktmbuf_pkt_len(pkts_burst[i]); it += 64) {
      xgm_data_buf[xgm_buf_counter] = (QData *)(pkts_burst[i]) + it;
      *xgm_ctrl_buf[xgm_buf_counter] = 0b11111111;
      xgm_buf_counter++;
    }

    // Packet end control bits
    *xgm_data_buf[xgm_buf_counter] = 0x07070707070707fd;
    *xgm_ctrl_buf[xgm_buf_counter] = 0b11111111;
  }

  // Pass DPDK packets to xgmii_rx_queue
  nb_tx_ctrl = rte_ring_enqueue_burst(
      get_xgmii_rx_queue_ctrl(), (void **)pkts_burst, PKT_BURST_SZ, nullptr);
  nb_tx_data = rte_ring_enqueue_burst(
      get_xgmii_rx_queue_data(), (void **)pkts_burst, PKT_BURST_SZ, nullptr);

  if (unlikely(nb_tx_data < xgm_buf_counter) ||
      unlikely(nb_tx_data != nb_tx_ctrl)) {
    // Free mbufs not tx to xgmii_rx_queue
    kni_stats[port_id].rx_dropped += xgm_buf_counter - nb_tx_data;
  }
}

void xgmii_worker_tx(kni_interface_stats *kni_stats, rte_ring *worker_tx_ring) {
  uint8_t i, it;
  uint16_t port_id;
  unsigned int nb_tx, nb_rx_ctrl, nb_rx_data, pkt_buf_counter = -1,
                                              rte_pkt_index = 0;
  bool packet_start_entered = false;
  uint32_t nb_kni;
  rte_mbuf *pkts_burst[PKT_BURST_SZ] __rte_cache_aligned;
  CData *xgm_ctrl_buf[XGMII_BURST_SZ] __rte_cache_aligned;
  QData *xgm_data_buf[XGMII_BURST_SZ] __rte_cache_aligned;

  // Burst rx from kni
  nb_rx_ctrl =
      rte_ring_dequeue_burst(get_xgmii_tx_queue_ctrl(), (void **)xgm_ctrl_buf,
                             XGMII_BURST_SZ, nullptr);
  nb_rx_data =
      rte_ring_dequeue_burst(get_xgmii_tx_queue_data(), (void **)xgm_data_buf,
                             XGMII_BURST_SZ, nullptr);
  if (unlikely(nb_rx_data > XGMII_BURST_SZ) ||
      unlikely(nb_rx_data != nb_rx_ctrl)) {
    RTE_LOG(ERR, APP, "Error receiving from Verilator\n");
    return;
  }

  // Create DPDK packets from XGMII packets
  for (i = 0; i < nb_rx_data; i++) {
    // Loop through each byte of data
    for (it = 0; it < 8; it++) {
      // Check control bit for data
      if ((*xgm_ctrl_buf[i] & (1 << it)) == 0) {
        // Is data, just move over
        rte_memcpy((CData *)(xgm_data_buf[i]),
                   (CData *)(pkts_burst[pkt_buf_counter]) + rte_pkt_index, 1);
        rte_pkt_index += 1;
      } else {
        // Control bit, check which one it is
        if (*(xgm_data_buf[i] + it) == 0xfb) {
          // packet start, switch to next packet buffer
          if (packet_start_entered) {
            throw std::logic_error("Multiple packet starts detected");
          } else {
            packet_start_entered = true;
            pkt_buf_counter++;
          }
        } else if (*(xgm_data_buf[i] + it) == 0xfd) {
          // end of packet, reset indexing
          if (!packet_start_entered) {
            throw std::logic_error("Multiple end of packet signals detected");
          } else {
            packet_start_entered = false;
            rte_pkt_index = 0;
          }
        } else if (*(xgm_data_buf[i] + it) == 0x07) {
          // Idle bit, just ignore
          continue;
        } else {
          // Some other operation, ignore
          continue;
        }
      }
    }
  }

  // Burst tx to ring with replies
  nb_tx = rte_ring_enqueue_burst(worker_tx_ring, (void **)pkts_burst,
                                 PKT_BURST_SZ * MAX_PACKET_SZ / 64, nullptr);
  if (nb_tx) kni_stats[port_id].tx_packets += nb_tx;
  if (unlikely(nb_tx < nb_rx_data)) {
    // Free mbufs not tx to NIC
    kni_stats[port_id].tx_dropped += nb_rx_data - nb_tx;
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
