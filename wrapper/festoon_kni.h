#ifndef FESTOON_KNI_H
#define FESTOON_KNI_H

#include <rte_ring.h>

#include "festoon_common.h"

void kni_ingress(kni_port_params *p, rte_ring *rx_ring);

void kni_egress(kni_port_params *p, rte_ring *tx_ring);

#endif