#ifndef FESTOON_KNI_H
#define FESTOON_KNI_H

#include <rte_ring.h>

#include "params.h"

static void kni_ingress(struct kni_port_params *p, rte_ring *rx_ring);

static void kni_egress(struct kni_port_params *p, rte_ring *tx_ring);

#endif