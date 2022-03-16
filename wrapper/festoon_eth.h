#ifndef FESTOON_ETH_H
#define FESTOON_ETH_H

#include "festoon_common.h"

void eth_ingress(kni_port_params *p, rte_ring *worker_rx_ring);

void eth_egress(kni_port_params *p, rte_ring *worker_tx_ring);

#endif