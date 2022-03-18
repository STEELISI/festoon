#include <stdexcept>

#include <rte_byteorder.h>
#include <rte_errno.h>
#include <rte_malloc.h>
#include <rte_mbuf.h>
#include <rte_mempool.h>

#include "festoon_xgmii.h"

using namespace std;

QData *xgm_data_idle, *xgm_data_pkt_begin, *xgm_data_pkt_end;
CData *xgm_ctrl_idle, *xgm_ctrl_pkt_begin, *xgm_ctrl_pkt_end,
      *xgm_ctrl_pkt_data;

// Convert mbuf to xgmii
void mbuf_to_xgmii(kni_interface_stats *kni_stats, rte_ring *mbuf_rx_ring,
                   rte_ring *xgmii_tx_ring_ctrl, rte_ring *xgmii_tx_ring_data) {
  rte_mbuf *pkts_burst[PKT_BURST_SZ] __rte_cache_aligned;
  CData *xgm_ctrl_buf[XGMII_BURST_SZ] __rte_cache_aligned;
  QData *xgm_data_buf[XGMII_BURST_SZ] __rte_cache_aligned;
  uint8_t i, data_size;
  uint16_t it, port_id;
  uint32_t nb_tx_ctrl, nb_tx_data, nb_rx, xgm_buf_counter = 0;

  // Burst RX from ring
  nb_rx = rte_ring_dequeue_burst(mbuf_rx_ring, (void **)pkts_burst,
                                 PKT_BURST_SZ, nullptr);
  if (unlikely(nb_rx > PKT_BURST_SZ)) {
    RTE_LOG(ERR, APP, "Error receiving from worker\n");

    // Send a full frame of just idle data
    xgm_data_buf[xgm_buf_counter] = xgm_data_idle;
    xgm_ctrl_buf[xgm_buf_counter] = xgm_ctrl_idle;
    xgm_buf_counter++;
  } else {
    // Create rte_mbuf packets from XGMII packets
    for (i = 0; i < nb_rx; i++) {
      if (pkts_burst[i] == nullptr)
        continue;

      // Send packet begin control bits
      xgm_data_buf[xgm_buf_counter] = xgm_data_pkt_begin;
      xgm_ctrl_buf[xgm_buf_counter] = xgm_ctrl_pkt_begin;
      xgm_buf_counter++;

      // Go through packet and move every 64 bits to XGM data buffer
      for (it = 0; it < rte_pktmbuf_pkt_len(pkts_burst[i]); it += sizeof(QData)) {
        xgm_data_buf[xgm_buf_counter] = rte_pktmbuf_mtod_offset(pkts_burst[i], QData *, it);

        if (it + sizeof(QData) >rte_pktmbuf_pkt_len(pkts_burst[i]))
          xgm_ctrl_buf[xgm_buf_counter] = &xgm_ctrl_pkt_data[it + sizeof(QData) - rte_pktmbuf_pkt_len(pkts_burst[i])];
        else
          xgm_ctrl_buf[xgm_buf_counter] = &xgm_ctrl_pkt_data[8];
        xgm_buf_counter++;
      }

      // Packet end control bits
      xgm_data_buf[xgm_buf_counter] = xgm_data_pkt_end;
      xgm_ctrl_buf[xgm_buf_counter] = xgm_ctrl_pkt_end;
      xgm_buf_counter++;
    }
  }

  // Pass DPDK packets to xgmii_rx_queue
  nb_tx_ctrl = rte_ring_enqueue_burst(xgmii_tx_ring_ctrl, (void **)xgm_ctrl_buf,
                                      xgm_buf_counter, nullptr);
  nb_tx_data = rte_ring_enqueue_burst(xgmii_tx_ring_data, (void **)xgm_data_buf,
                                      xgm_buf_counter, nullptr);

  if (unlikely(nb_tx_data < xgm_buf_counter) ||
      unlikely(nb_tx_data != nb_tx_ctrl)) {
    // Free mbufs not tx to xgmii_rx_queue
    kni_stats[port_id].rx_dropped += xgm_buf_counter - nb_tx_data;
  }
}

void xgmii_to_mbuf(kni_interface_stats *kni_stats, bool *pkt_start_entered,
                   rte_ring *xgmii_rx_ring_ctrl, rte_ring *xgmii_rx_ring_data,
                   rte_ring *mbuf_tx_ring) {
  uint8_t i, it;
  uint16_t port_id;
  unsigned int nb_tx, nb_rx_ctrl, nb_rx_data, pkt_buf_counter = 0,
                                              rte_pkt_index = 0;
  uint32_t nb_kni;
  rte_mbuf *pkts_burst[PKT_BURST_SZ] __rte_cache_aligned;
  CData *xgm_ctrl_buf[XGMII_BURST_SZ] __rte_cache_aligned;
  QData *xgm_data_buf[XGMII_BURST_SZ] __rte_cache_aligned;

  // Burst rx from kni
  nb_rx_ctrl = rte_ring_dequeue_burst(xgmii_rx_ring_ctrl, (void **)xgm_ctrl_buf,
                                      XGMII_BURST_SZ, nullptr);
  nb_rx_data = rte_ring_dequeue_burst(xgmii_rx_ring_data, (void **)xgm_data_buf,
                                      XGMII_BURST_SZ, nullptr);
  if (unlikely(nb_rx_data > XGMII_BURST_SZ) ||
      unlikely(nb_rx_data != nb_rx_ctrl)) {
    return;
  }

  // Create DPDK packets from XGMII packets
  for (i = 0; i < nb_rx_data; i++) {
    // Loop through each byte of data
    for (it = 0; it < 8; it++) {
      // Check control bit for data
      if (*pkt_start_entered && (*xgm_ctrl_buf[i] & (1 << it)) == 0) {
        // Is data, just move over
        rte_memcpy((CData *)(xgm_data_buf[i]),
                   rte_pktmbuf_mtod_offset(pkts_burst[pkt_buf_counter],
                                           CData *, rte_pkt_index),
                   1);
        rte_pkt_index += 1;
      } else {
        // Control bit, check which one it is
        if (*(xgm_data_buf[i] + it) == 0xfb) {
          // packet start, switch to next packet buffer
          if (pkt_start_entered) {
            throw logic_error("Multiple packet starts detected");
          } else {
            *pkt_start_entered = true;
            pkt_buf_counter++;
          }
        } else if (*(xgm_data_buf[i] + it) == 0xfd) {
          // end of packet, reset indexing
          if (!pkt_start_entered) {
            throw logic_error("Multiple end of packet signals detected");
          } else {
            *pkt_start_entered = false;
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

  // No packets sent, skip
  if (pkt_buf_counter == 0)
    return;

  // Burst tx to ring with replies
  nb_tx = rte_ring_enqueue_burst(mbuf_tx_ring, (void **)pkts_burst,
                                 PKT_BURST_SZ, nullptr);
  if (nb_tx) kni_stats[port_id].tx_packets += nb_tx;
  if (unlikely(nb_tx < nb_rx_data)) {
    // Free mbufs not tx to NIC
    kni_stats[port_id].tx_dropped += nb_rx_data - nb_tx;
  }
}

// Init common data frames
void init_xgmii_datatypes() {
  // Pre-allocate idle frame
  xgm_data_idle = (QData *)rte_malloc("XGM Idle Data Frame", sizeof(QData), 0);
  xgm_ctrl_idle = (CData *)rte_malloc("XGM Idle Ctrl Frame", sizeof(CData), 0);
  *xgm_data_idle = 0x0707070707070707;
  *xgm_ctrl_idle = 0b00000000;

  // Pre-allocate packet begin frame
  xgm_data_pkt_begin =
      (QData *)rte_malloc("XGM Start Data Frame", sizeof(QData), 0);
  xgm_ctrl_pkt_begin =
      (CData *)rte_malloc("XGM Start Ctrl Frame", sizeof(CData), 0);
  *xgm_data_pkt_begin = 0xd5555555555555fb;
  *xgm_ctrl_pkt_begin = 0b00000001;

  // Pre-allocate packet end frame
  xgm_data_pkt_end =
      (QData *)rte_malloc("XGM End Data Frame", sizeof(QData), 0);
  xgm_ctrl_pkt_end =
      (CData *)rte_malloc("XGM End Ctrl Frame", sizeof(CData), 0);
  *xgm_data_pkt_end = 0x0707070707070707;
  *xgm_ctrl_pkt_end = 0b00000000;

  // Pre-allocate packet data control buffer
  xgm_ctrl_pkt_data = (CData *) rte_malloc("XGM Pkt Data Ctrl Frames",
                                           9 * sizeof(CData), 0);
  xgm_ctrl_pkt_data[0] = 0b00000000;
  xgm_ctrl_pkt_data[1] = 0b10000000;
  xgm_ctrl_pkt_data[2] = 0b11000000;
  xgm_ctrl_pkt_data[3] = 0b11100000;
  xgm_ctrl_pkt_data[4] = 0b11110000;
  xgm_ctrl_pkt_data[5] = 0b11111000;
  xgm_ctrl_pkt_data[6] = 0b11111100;
  xgm_ctrl_pkt_data[7] = 0b11111110;
  xgm_ctrl_pkt_data[8] = 0b11111111;
}

void free_xgmii_datatypes() {
  rte_free(xgm_data_idle);
  rte_free(xgm_ctrl_idle);
  rte_free(xgm_data_pkt_begin);
  rte_free(xgm_data_pkt_end);
  rte_free(xgm_ctrl_pkt_begin);
  rte_free(xgm_ctrl_pkt_end);
  rte_free(xgm_ctrl_pkt_data);
}
