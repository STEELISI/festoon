#ifndef FESTOON_XGMII_H
#define FESTOON_XGMII_H

#include <rte_ring.h>
#include "verilated.h"
#include "params.h"

void init_xgmii_worker(rte_ring **worker_tx_ring, rte_ring **worker_rx_ring);

void stop_xgmii_worker(rte_ring *worker_tx_ring, rte_ring *worker_rx_ring);

void xgmii_worker_rx(kni_interface_stats *kni_stats,
                     rte_ring *worker_rx_ring);

void xgmii_worker_tx(kni_interface_stats *kni_stats,
                     rte_ring *worker_tx_ring);

rte_ring *get_xgmii_tx_queue_ctrl();
rte_ring *get_xgmii_rx_queue_ctrl();
rte_ring *get_xgmii_tx_queue_data();
rte_ring *get_xgmii_rx_queue_data();

#endif