#include "festoon_top.h"

#include "Vtop.h"
#include "festoon_xgmii.h"
#include "params.h"

Vtop *top;

// Temp. output XGMII frame for use by verilator_top_worker
struct XgmiiFrame *fr_out = new XgmiiFrame();

// Simulation time
vluint64_t main_time = 0;

void init_verilated_top() {
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
void verilator_top_worker(rte_ring *xgmii_tx_queue,
                          rte_ring *xgmii_rx_queue) {
  struct XgmiiFrame *fr_in;

  // Loop for a clock cycle and deque + queue XGMII frames
  for (int i = 0; i < 20; i++) {
    if (Verilated::gotFinish()) return;

    if ((main_time % 10) == 1) {
      // Read RTE frame each rising clock
      rte_ring_dequeue(xgmii_rx_queue, (void **)fr_in);

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
      rte_ring_enqueue(xgmii_tx_queue, (void **)fr_out);

      // Toggle clock
      top->clk = 0;
    }

    top->eval();  // Evaluate model
    main_time++;  // Time passes...
  }
}

// Free Verilator model and buffers
void stop_verilated_top() {
  top->final();
  delete top;
}
