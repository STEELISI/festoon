#ifndef FESTOON_XGMII_H
#define FESTOON_XGMII_H

#include <rte_mempool.h>
#include <rte_ring.h>

#include "festoon_common.h"
#include "verilated.h"

void mbuf_to_xgmii(rte_ring *mbuf_rx_ring, rte_ring *xgmii_tx_ring, rte_mempool *tx_mempool);

void xgmii_to_mbuf(bool *pkt_start_entered, rte_ring *xgmii_rx_ring, rte_ring *mbuf_tx_ring, rte_mempool *tx_mempool);

#endif