#ifndef FESTOON_XGMII_H
#define FESTOON_XGMII_H

#include <rte_ring.h>
#include <rte_mempool.h>

#include "festoon_common.h"
#include "verilated.h"

void init_xgmii_datatypes();

void free_xgmii_datatypes();

void mbuf_to_xgmii(kni_interface_stats *kni_stats, rte_ring *mbuf_rx_ring,
                   rte_ring *xgmii_tx_ring_ctrl, rte_ring *xgmii_tx_ring_data);

void xgmii_to_mbuf(kni_interface_stats *kni_stats, bool *pkt_start_entered,
                   rte_ring *xgmii_rx_ring_ctrl, rte_ring *xgmii_rx_ring_data,
                   rte_ring *mbuf_tx_ring, rte_mempool *tx_mempool);

#endif