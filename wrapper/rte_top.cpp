#include "rte_top.h"

#include <rte_mbuf.h>
#include <rte_ring.h>

#include "Vtop.h"
#include "params.h"

Vtop *top;

struct rte_ring *xgmii_tx_queue, *xgmii_rx_queue;

// Temp. output XGMII frame for use by verilator_top_worker
struct XgmiiFrame *fr_out = new XgmiiFrame();

// Simulation time
vluint64_t main_time = 0;

void init_verilated_top() {
  // Generate TX and RX queues
  xgmii_tx_queue = rte_ring_create("XGMII transmit queue", 1024 * 8,
                                   rte_socket_id(), RING_F_SC_DEQ);
  xgmii_rx_queue = rte_ring_create("XGMII recieve queue", 1024 * 8,
                                   rte_socket_id(), RING_F_SC_DEQ);
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

// Run the Verilator module as a worker thread
void verilator_top_worker(struct rte_ring *worker_tx_ring,
                          struct rte_ring *worker_rx_ring) {
  struct XgmiiFrame *fr_in;

  // Loop for a clock cycle and deque + queue XGMII frames
  for (int i = 0; i < 20; i++) {
    if (Verilated::gotFinish()) return;

    if ((main_time % 10) == 1) {
      // Read RTE frame each rising clock
      rte_ring_dequeue(worker_rx_ring, (void **)fr_in);

      // Convert frame into Verilator inputs
      top->eth_in_xgmii_ctrl = fr_in->ctrl;
      top->eth_in_xgmii_data = fr_in->data;

      // Toggle clock
      top->clk = 1;
    }
    if ((main_time % 10) == 6) {
      // Convert Verilator outputs into frame
      top->eth_in_xgmii_ctrl = fr_out->ctrl;
      top->eth_in_xgmii_data = fr_out->data;

      // Transmit RTE frame each rising clock
      rte_ring_enqueue(worker_rx_ring, (void **)fr_out);

      // Toggle clock
      top->clk = 0;
    }

    top->eval();  // Evaluate model
    main_time++;  // Time passes...
  }
}

// Free Verilator model and buffers
void stop_verilated_top() {
  rte_ring_free(xgmii_tx_queue);
  rte_ring_free(xgmii_rx_queue);
  delete top;
}
