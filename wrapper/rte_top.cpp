#include "rte_top.h"

#include <rte_mbuf.h>
#include <rte_ring.h>

#include "Vtop.h"
#include "params.h"

Vtop *top;

struct rte_ring *xgmii_tx_queue, *xgmii_rx_queue;

// Simulation time
vluint64_t main_time = 0;

void init_verilated_top() {
  // Generate TX and RX queue mempools
  xgmii_tx_queue = rte_ring_create("XGMII transmit queue", 1024, rte_socket_id(),
                                   RING_F_SC_DEQ);
  xgmii_rx_queue = rte_ring_create("XGMII recieve queue", 1024, rte_socket_id(),
                                   RING_F_SC_DEQ);
  top = new Vtop;

  top->clk = 0;
  while (main_time < 51) {
    if (main_time > 10 * 5) {
      top->reset = 1;  // Pull down reset for a few clocks
    }
    if ((main_time % 10) == 1) {
      top->clk = 1;  // Toggle clock
    }
    if ((main_time % 10) == 6) {
      top->clk = 0;
    }
    top->eval();  // Evaluate model
    main_time++;  // Time passes
  }
}

void verilator_top_worker() {
  struct rte_mbuf *buf[16] __rte_cache_aligned;
  XgmiiFrame *fr;

  rte_ring_enqueue(xgmii_tx_queue, (void*) fr);
}

// Free Verilator model and buffers
void stop_verilated_top() {
  rte_ring_free(xgmii_tx_queue);
  rte_ring_free(xgmii_rx_queue);
  delete top;
}
