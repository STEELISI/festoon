#include <cstdint>
#include <rte_byteorder.h>
#include <rte_errno.h>
#include <rte_malloc.h>
#include <rte_mbuf.h>

#include "festoon_common.h"
#include "festoon_xgmii.h"

using namespace std;

// Convert mbuf to xgmii
void mbuf_to_xgmii(rte_ring *mbuf_rx_ring, rte_ring *xgmii_tx_ring, rte_mempool *tx_mempool, uint8_t tid) {
  rte_mbuf *pkts_burst[PKT_BURST_SZ] __rte_cache_aligned,
           *xgm_buf[XGMII_BURST_SZ] __rte_cache_aligned;
  uint8_t i, data_size;
  uint16_t it, port_id = 0;
  uint64_t nb_tx = 0, nb_rx = 0, xgm_buf_counter = 0, frames_per_pkt;

  // Burst RX from ring
  nb_rx = rte_ring_dequeue_burst(mbuf_rx_ring, (void **)pkts_burst, PKT_BURST_SZ, nullptr);
  if (unlikely(nb_rx > PKT_BURST_SZ)) {
    RTE_LOG(ERR, APP, "Error receiving from mbuf\n");
    return;
  }

  if(unlikely(nb_rx <= 0))
    return;

  // Alloc mbufs for packets
  if (rte_pktmbuf_alloc_bulk(tx_mempool, xgm_buf, XGMII_BURST_SZ) != 0) {
    RTE_LOG(ERR, APP, "Error allocing xgmii mbufs\n");
    return;
  }

  // Create rte_mbuf packets from XGMII packets
  for (i = 0; i < nb_rx; i++) {
    if (unlikely(pkts_burst[i] == nullptr))
      continue;
    if (unlikely(rte_pktmbuf_pkt_len(pkts_burst[i]) == 0))
      continue;

    frames_per_pkt = (rte_pktmbuf_pkt_len(pkts_burst[i]) - 1) / sizeof(QData) + 1;

    // Send packet begin control bits
    *rte_pktmbuf_mtod(xgm_buf[xgm_buf_counter], CData *) = 0b00000001;
    *rte_pktmbuf_mtod_offset(xgm_buf[xgm_buf_counter], QData *, sizeof(CData)) = 0xd5555555555555fb;
    xgm_buf_counter++;

    // Go through packet and move every 64 bits to XGM data buffer
    for (it = 0; it < frames_per_pkt; it += 1) {
      // Check for remaining packet size < 64 bits
      if (unlikely(rte_pktmbuf_pkt_len(pkts_burst[i]) < (frames_per_pkt * sizeof(QData)))) {
        int frame_size = rte_pktmbuf_pkt_len(pkts_burst[i]) % sizeof(QData);
        rte_memcpy(rte_pktmbuf_mtod_offset(xgm_buf[xgm_buf_counter], void *, 1),
                   rte_pktmbuf_mtod_offset(pkts_burst[i], void *, it * sizeof(QData)),
                   frame_size);
        *rte_pktmbuf_mtod(xgm_buf[xgm_buf_counter], CData *) = ~((1 << frame_size) - 1);
      } else {
        rte_mov64(rte_pktmbuf_mtod_offset(xgm_buf[xgm_buf_counter], uint8_t *, 1),
                  rte_pktmbuf_mtod_offset(pkts_burst[i], uint8_t *, it * sizeof(QData)));
        *rte_pktmbuf_mtod(xgm_buf[xgm_buf_counter], CData *) = 0b00000000;
      }

      xgm_buf_counter++;
    }

    // Packet end control bits
    *rte_pktmbuf_mtod(xgm_buf[xgm_buf_counter], CData *) = 0b11111111;
    *rte_pktmbuf_mtod_offset(xgm_buf[xgm_buf_counter], QData *, sizeof(CData)) = 0x07070707070707fd;
    xgm_buf_counter++;
  }

  // Pass DPDK packets to xgmii_rx_queue
  nb_tx = rte_ring_enqueue_burst(xgmii_tx_ring, (void **)xgm_buf, xgm_buf_counter, nullptr);

  // Free input pkts
  kni_burst_free_mbufs(&pkts_burst[0], nb_rx);

  if (nb_tx) get_kni_stats()[port_id].xgmii_rx_packets[tid] += nb_tx;

  // Free mbufs not tx to xgmii_rx_queue
  if (unlikely(nb_tx < XGMII_BURST_SZ)) {
    kni_burst_free_mbufs(&xgm_buf[nb_tx], XGMII_BURST_SZ - nb_tx);
    get_kni_stats()[port_id].xgmii_rx_dropped[tid] += xgm_buf_counter - nb_tx;
  }
}

void xgmii_to_mbuf(rte_ring *xgmii_rx_ring, rte_ring *mbuf_tx_ring, rte_mempool *tx_mempool, uint8_t tid) {
  bool pkt_start_entered = false;
  uint8_t i, it;
  uint16_t port_id = 0;
  uint32_t nb_kni;
  uint64_t nb_tx = 0, pkt_buf_counter = 0, rte_pkt_index = 0;
  rte_mbuf *pkts_burst[PKT_BURST_SZ] __rte_cache_aligned,
           *xgm_buf[1] __rte_cache_aligned;

  // Alloc mbufs for packets
  if (rte_pktmbuf_alloc_bulk(tx_mempool, pkts_burst, PKT_BURST_SZ) != 0) {
    RTE_LOG(ERR, APP, "Error allocing pkt mbufs\n");
    return;
  }

  while (pkt_buf_counter < PKT_BURST_SZ) {
    // Burst rx from kni
    if (unlikely(rte_ring_dequeue(xgmii_rx_ring, (void **) xgm_buf) < 0))
      continue;

    // Loop through each byte of data
    for (it = 1; it <= sizeof(QData); it++) {
      // Check control bit for data
      if (likely((*rte_pktmbuf_mtod(xgm_buf[0], CData *) & (1 << (it - 1))) == 0)) {
        // Is data, just move over
        if (pkt_start_entered) {
          rte_memcpy(rte_pktmbuf_mtod_offset(pkts_burst[pkt_buf_counter], void *, rte_pkt_index),
                     rte_pktmbuf_mtod_offset(xgm_buf[0], void *, it),
                     sizeof(CData));
          rte_pkt_index += 1;
        }
      } else {
        // Control bit, check which one it is
        if (*rte_pktmbuf_mtod_offset(xgm_buf[0], CData *, it) == 0xfb) {
          // packet start, switch to next packet buffer
          if (unlikely(pkt_start_entered)) {
            RTE_LOG(ERR, APP, "Multiple packet start signals detected\n");
          } else {
            pkt_start_entered = true;
            rte_pkt_index = 0;
          }
        } else if (*rte_pktmbuf_mtod_offset(xgm_buf[0], CData *, it) == 0xfd) {
          // end of packet, reset indexing
          if (unlikely(!pkt_start_entered)) {
            RTE_LOG(ERR, APP, "Multiple end of packet signals detected\n");
          } else {
            pkt_start_entered = false;
            pkt_buf_counter++;
          }
        } else if (*rte_pktmbuf_mtod_offset(xgm_buf[0], CData *, it) == 0x07) {
          // Idle bit, just ignore
          continue;
        } else {
          // Some other operation, ignore
          continue;
        }
      }
    }

    // Free input frames
    kni_burst_free_mbufs(&xgm_buf[0], 1);
  }

  // Burst tx to ring with replies
  if (pkt_buf_counter != 0)
    nb_tx = rte_ring_enqueue_burst(mbuf_tx_ring, (void **)pkts_burst, pkt_buf_counter, nullptr);

  if (nb_tx) get_kni_stats()[port_id].xgmii_tx_packets[tid] += nb_tx;

  if (unlikely(nb_tx < PKT_BURST_SZ)) {
    // Free mbufs not tx to NIC
    kni_burst_free_mbufs(&pkts_burst[nb_tx], PKT_BURST_SZ - nb_tx);
    get_kni_stats()[port_id].xgmii_tx_dropped[tid] += pkt_buf_counter - nb_tx;
  }
}
