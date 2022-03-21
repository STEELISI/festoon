#include "festoon_xgmii.h"

#include <rte_byteorder.h>
#include <rte_errno.h>
#include <rte_malloc.h>
#include <rte_mbuf.h>

#include <stdexcept>

using namespace std;

// Convert mbuf to xgmii
void mbuf_to_xgmii(kni_interface_stats *kni_stats, rte_ring *mbuf_rx_ring,
                   rte_ring *xgmii_tx_ring, rte_mempool *tx_mempool) {
  rte_mbuf *pkts_burst[PKT_BURST_SZ] __rte_cache_aligned,
      *xgm_buf[XGMII_BURST_SZ] __rte_cache_aligned;
  uint8_t i, data_size;
  uint16_t it, port_id;
  uint32_t nb_tx, nb_rx, xgm_buf_counter = 0;

  // Alloc mbufs for packets
  rte_pktmbuf_alloc_bulk(tx_mempool, xgm_buf, XGMII_BURST_SZ);

  // Burst RX from ring
  nb_rx = rte_ring_dequeue_burst(mbuf_rx_ring, (void **)pkts_burst,
                                 PKT_BURST_SZ, nullptr);
  if (unlikely(nb_rx > PKT_BURST_SZ)) {
    RTE_LOG(ERR, APP, "Error receiving from worker\n");

    // Send a full frame of just idle data
    *rte_pktmbuf_mtod(xgm_buf[xgm_buf_counter], CData *) = 0b00000000;
    *rte_pktmbuf_mtod_offset(xgm_buf[xgm_buf_counter], QData *, sizeof(CData)) =
        0x0707070707070707;
    xgm_buf_counter++;
  } else {
    // Create rte_mbuf packets from XGMII packets
    for (i = 0; i < nb_rx; i++) {
      if (pkts_burst[i] == nullptr)
        continue;

      // Send packet begin control bits
      *rte_pktmbuf_mtod(xgm_buf[xgm_buf_counter], CData *) = 0b00000001;
      *rte_pktmbuf_mtod_offset(xgm_buf[xgm_buf_counter], QData *,
                               sizeof(CData)) = 0xd5555555555555fb;
      xgm_buf_counter++;

      // Go through packet and move every 64 bits to XGM data buffer
      for (it = 0; it < rte_pktmbuf_pkt_len(pkts_burst[i]);
           it += sizeof(QData)) {
        *rte_pktmbuf_mtod_offset(xgm_buf[xgm_buf_counter], QData *, 1) =
            *rte_pktmbuf_mtod_offset(pkts_burst[i], QData *, it);
        // TODO: set data bits based on rte_pktmbuf_pkt_len(pkts_burst[i]) and
        // it
        *rte_pktmbuf_mtod(xgm_buf[xgm_buf_counter], CData *) = 0b00000000;
        xgm_buf_counter++;
      }

      // Packet end control bits
      *rte_pktmbuf_mtod(xgm_buf[xgm_buf_counter], CData *) = 0b11111111;
      *rte_pktmbuf_mtod_offset(xgm_buf[xgm_buf_counter], QData *,
                               sizeof(CData)) = 0x07070707070707fd;
      xgm_buf_counter++;
    }
  }

  // Pass DPDK packets to xgmii_rx_queue
  nb_tx = rte_ring_enqueue_burst(xgmii_tx_ring, (void **)xgm_buf,
                                 xgm_buf_counter, nullptr);

  // Free input pkts
  kni_burst_free_mbufs(&pkts_burst[0], nb_rx);

  if (nb_tx)
    kni_stats[port_id].tx_packets += nb_tx;
  if (unlikely(nb_tx < xgm_buf_counter)) {
    // Free mbufs not tx to xgmii_rx_queue
    kni_burst_free_mbufs(&xgm_buf[nb_tx], nb_rx - nb_tx);
    kni_stats[port_id].rx_dropped += xgm_buf_counter - nb_tx;
  }
}

void xgmii_to_mbuf(kni_interface_stats *kni_stats, bool *pkt_start_entered,
                   rte_ring *xgmii_rx_ring, rte_ring *mbuf_tx_ring,
                   rte_mempool *tx_mempool) {
  uint8_t i, it;
  uint16_t port_id;
  unsigned int nb_tx, nb_rx, pkt_buf_counter = 0, rte_pkt_index = 0;
  uint32_t nb_kni;
  CData ctrl_buf;
  rte_mbuf *pkts_burst[PKT_BURST_SZ] __rte_cache_aligned,
      *xgm_buf[XGMII_BURST_SZ] __rte_cache_aligned;

  // Burst rx from kni
  nb_rx = rte_ring_dequeue_burst(xgmii_rx_ring, (void **)xgm_buf,
                                 XGMII_BURST_SZ, nullptr);
  if (unlikely(nb_rx > XGMII_BURST_SZ) || unlikely(nb_rx == 0)) {
    return;
  }

  // Alloc mbufs for packets
  rte_pktmbuf_alloc_bulk(tx_mempool, pkts_burst, PKT_BURST_SZ);

  // Create DPDK packets from XGMII packets
  for (i = 0; i < nb_rx; i++) {
    // Loop through each byte of data
    ctrl_buf = *rte_pktmbuf_mtod(pkts_burst[i], CData *);
    for (it = 0; it < sizeof(ctrl_buf); it++) {
      // Check control bit for data
      if ((ctrl_buf & (1 << it)) == 0) {
        // Is data, just move over
        if (*pkt_start_entered) {
          *rte_pktmbuf_mtod_offset(pkts_burst[pkt_buf_counter], CData *,
                                   rte_pkt_index) =
              *rte_pktmbuf_mtod_offset(pkts_burst[i], CData *, it + 1);
          rte_pkt_index += 1;
        }
      } else {
        // Control bit, check which one it is
        if (*rte_pktmbuf_mtod_offset(pkts_burst[i], CData *, it + 1) == 0xfb) {
          // packet start, switch to next packet buffer
          if (pkt_start_entered) {
            throw logic_error("Multiple packet starts detected");
          } else {
            *pkt_start_entered = true;
            pkt_buf_counter++;
          }
        } else if (*rte_pktmbuf_mtod_offset(pkts_burst[i], CData *, it + 1) ==
                   0xfd) {
          // end of packet, reset indexing
          if (!pkt_start_entered) {
            throw logic_error("Multiple end of packet signals detected");
          } else {
            *pkt_start_entered = false;
            rte_pkt_index = 0;
          }
        } else if (*rte_pktmbuf_mtod_offset(pkts_burst[i], CData *, it + 1) ==
                   0x07) {
          // Idle bit, just ignore
          continue;
        } else {
          // Some other operation, ignore
          continue;
        }
      }
    }
  }

  // No packets sent, skip
  if (pkt_buf_counter == 0)
    return;

  // Burst tx to ring with replies
  nb_tx = rte_ring_enqueue_burst(mbuf_tx_ring, (void **)pkts_burst,
                                 PKT_BURST_SZ, nullptr);

  // Free input pkts
  kni_burst_free_mbufs(&xgm_buf[0], nb_rx);

  if (nb_tx)
    kni_stats[port_id].tx_packets += nb_tx;
  if (unlikely(nb_tx < nb_rx)) {
    // Free mbufs not tx to NIC
    kni_burst_free_mbufs(&pkts_burst[nb_tx], nb_rx - nb_tx);
    kni_stats[port_id].tx_dropped += nb_rx - nb_tx;
  }
}
