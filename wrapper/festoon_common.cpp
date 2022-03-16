#include <rte_mbuf.h>

#include "festoon_common.h"

void kni_burst_free_mbufs(rte_mbuf **pkts, unsigned num) {
  unsigned i;

  if (pkts == NULL) return;

  for (i = 0; i < num; i++) {
    rte_pktmbuf_free(pkts[i]);
    pkts[i] = NULL;
  }
}
