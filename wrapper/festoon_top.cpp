#include "festoon_top.h"

#include "Vtop.h"
#include "festoon_xgmii.h"
#include "params.h"

Vtop *top;

// Simulation time
vluint64_t main_time = 0;

void init_verilated_top() {
  // Start Verilator model
  top = new Vtop;

  // Pull down reset for a few clocks
  top->clk = 0;
  while (main_time < 51) {
    if (main_time > 10 * 3) {
      top->reset = 1;
    } else {
      top->reset = 0;
    }

    // Toggle clock
    if ((main_time % 10) == 1) {
      top->clk = 1;
    }
    if ((main_time % 10) == 6) {
      top->clk = 0;
    }

    top->eval();  // Evaluate model
    main_time++;  // Time passes
  }
}

// Run the Verilator module as a worker thread
void verilator_top_worker() {
  CData *fr_in_ctrl, *fr_out_ctrl;
  QData *fr_in_data, *fr_out_data;
  int i;

  // Loop for a clock cycle and deque + queue XGMII frames
  for (i = 0; i < 10; i++) {
    if (Verilated::gotFinish()) return;

    if ((main_time % 10) == 0) {
      // Read RTE frame each rising clock
      rte_ring_dequeue(get_xgmii_rx_queue_ctrl(), (void **) fr_in_ctrl);
      rte_ring_dequeue(get_xgmii_rx_queue_data(), (void **) fr_in_data);

      // Convert frame into Verilator inputs
      top->eth_in_xgmii_ctrl = *fr_in_ctrl;
      top->eth_in_xgmii_data = *fr_in_data;

      // Toggle clock
      top->clk = 1;
    }
    if ((main_time % 10) == 5) {
      // Convert Verilator outputs into frame
      *fr_out_ctrl = top->eth_in_xgmii_ctrl;
      *fr_out_data = top->eth_in_xgmii_data;

      // Transmit RTE frame each rising clock
      rte_ring_enqueue(get_xgmii_tx_queue_ctrl(), fr_out_ctrl);
      rte_ring_enqueue(get_xgmii_tx_queue_data(), fr_out_data);

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
