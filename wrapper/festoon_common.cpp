#include <rte_mbuf.h>

#include "festoon_common.h"

/* kni device statistics array */
kni_interface_stats kni_stats[RTE_MAX_ETHPORTS];

void kni_burst_free_mbufs(rte_mbuf **pkts, unsigned num) {
  unsigned i;

  if (pkts == NULL) return;

  for (i = 0; i < num; i++) {
    rte_pktmbuf_free(pkts[i]);
    pkts[i] = NULL;
  }
}

kni_interface_stats *get_kni_stats() {
  return kni_stats;
}