#include "festoon_top.h"

#include <rte_errno.h>
#include <rte_mbuf.h>
#include <rte_ring.h>

#include <stdexcept>

#include "Vtop.h"
#include "params.h"

using namespace std;

Vtop *top;

rte_ring *eth_rx_ring_ctrl, *eth_rx_ring_data, *eth_tx_ring_ctrl,
    *eth_tx_ring_data, *pci_rx_ring_ctrl, *pci_rx_ring_data, *pci_tx_ring_ctrl,
    *pci_tx_ring_data;

// Simulation time
vluint64_t main_time = 0;

void init_verilated_top() {
  // Generate TX and RX queues for XGMII Ethernet
  eth_tx_ring_ctrl =
      rte_ring_create("XGMII eth transmit control", XGMII_BURST_SZ,
                      rte_socket_id(), RING_F_SC_DEQ);
  if (eth_tx_ring_ctrl == nullptr) throw runtime_error(rte_strerror(rte_errno));

  eth_rx_ring_data =
      rte_ring_create("XGMII eth transmit data", XGMII_BURST_SZ,
                      rte_socket_id(), RING_F_SC_DEQ);
  if (eth_rx_ring_data == nullptr) throw runtime_error(rte_strerror(rte_errno));

  eth_rx_ring_ctrl =
      rte_ring_create("XGMII eth recieve control", XGMII_BURST_SZ,
                      rte_socket_id(), RING_F_SC_DEQ);
  if (eth_rx_ring_ctrl == nullptr) throw runtime_error(rte_strerror(rte_errno));

  eth_tx_ring_data = rte_ring_create("XGMII eth recieve data", XGMII_BURST_SZ,
                                     rte_socket_id(), RING_F_SC_DEQ);
  if (eth_tx_ring_data == nullptr) throw runtime_error(rte_strerror(rte_errno));

  // Generate TX and RX queues for XGMII PCIe
  pci_tx_ring_ctrl =
      rte_ring_create("XGMII pcie transmit control", XGMII_BURST_SZ,
                      rte_socket_id(), RING_F_SC_DEQ);
  if (pci_tx_ring_ctrl == nullptr) throw runtime_error(rte_strerror(rte_errno));

  pci_rx_ring_data =
      rte_ring_create("XGMII pcie transmit data", XGMII_BURST_SZ,
                      rte_socket_id(), RING_F_SC_DEQ);
  if (pci_rx_ring_data == nullptr) throw runtime_error(rte_strerror(rte_errno));

  pci_rx_ring_ctrl =
      rte_ring_create("XGMII pcie recieve control", XGMII_BURST_SZ,
                      rte_socket_id(), RING_F_SC_DEQ);
  if (pci_rx_ring_ctrl == nullptr) throw runtime_error(rte_strerror(rte_errno));

  pci_tx_ring_data =
      rte_ring_create("XGMII pcie recieve data", XGMII_BURST_SZ,
                      rte_socket_id(), RING_F_SC_DEQ);
  if (pci_tx_ring_data == nullptr) throw runtime_error(rte_strerror(rte_errno));

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
  CData eth_fr_in_ctrl, eth_fr_out_ctrl, pci_fr_in_ctrl, pci_fr_out_ctrl;
  QData eth_fr_in_data, eth_fr_out_data, pci_fr_in_data, pci_fr_out_data;
  int i, eth_nb_rx, pci_nb_rx;

  // Loop for a clock cycle and deque + queue XGMII frames
  for (i = 0; i < 10; i++) {
    if (Verilated::gotFinish()) return;

    if ((main_time % 10) == 0) {
      // Read Eth frame each rising clock
      rte_ring_dequeue(eth_rx_ring_ctrl, (void **)&eth_fr_in_ctrl);
      eth_nb_rx = rte_ring_dequeue(eth_rx_ring_data, (void **)&eth_fr_in_data);

      // Read PCI frame each rising clock
      rte_ring_dequeue(pci_rx_ring_ctrl, (void **)&pci_fr_in_ctrl);
      pci_nb_rx = rte_ring_dequeue(pci_rx_ring_data, (void **)&pci_fr_in_data);

      // If both good, update input wires
      if (eth_nb_rx == ENOENT && pci_nb_rx == ENOENT) {
        // Convert frame into Verilator inputs
        top->eth_in_xgmii_ctrl = eth_fr_in_ctrl;
        top->eth_in_xgmii_data = eth_fr_in_data;
        top->pcie_in_xgmii_ctrl = pci_fr_in_ctrl;
        top->pcie_in_xgmii_data = pci_fr_in_data;
      }

      // Convert Verilator outputs into frame
      eth_fr_out_ctrl = top->eth_in_xgmii_ctrl;
      eth_fr_out_data = top->eth_in_xgmii_data;

      pci_fr_out_ctrl = top->pcie_in_xgmii_ctrl;
      pci_fr_out_data = top->pcie_in_xgmii_data;

      // Transmit Eth frame each rising clock
      rte_ring_enqueue(eth_tx_ring_ctrl, (void **)&eth_fr_out_ctrl);
      rte_ring_enqueue(eth_tx_ring_data, (void **)&eth_fr_out_data);

      // Transmit PCI frame each rising clock
      rte_ring_enqueue(pci_tx_ring_ctrl, (void **)&pci_fr_out_ctrl);
      rte_ring_enqueue(pci_tx_ring_data, (void **)&pci_fr_out_data);

      // Toggle clock
      top->clk = 1;
    }
    if ((main_time % 10) == 5) {
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

  rte_ring_free(eth_rx_ring_ctrl);
  rte_ring_free(eth_rx_ring_data);

  rte_ring_free(eth_tx_ring_ctrl);
  rte_ring_free(eth_tx_ring_data);

  rte_ring_free(pci_rx_ring_ctrl);
  rte_ring_free(pci_rx_ring_data);

  rte_ring_free(pci_tx_ring_ctrl);
  rte_ring_free(pci_tx_ring_data);
}

rte_ring *get_vtop_eth_rx_ring_ctrl() { return eth_rx_ring_ctrl; }

rte_ring *get_vtop_eth_rx_ring_data() { return eth_rx_ring_data; }

rte_ring *get_vtop_eth_tx_ring_ctrl() { return eth_tx_ring_ctrl; }

rte_ring *get_vtop_eth_tx_ring_data() { return eth_tx_ring_data; }

rte_ring *get_vtop_pci_rx_ring_ctrl() { return pci_rx_ring_ctrl; }

rte_ring *get_vtop_pci_rx_ring_data() { return pci_rx_ring_data; }

rte_ring *get_vtop_pci_tx_ring_ctrl() { return pci_tx_ring_ctrl; }

rte_ring *get_vtop_pci_tx_ring_data() { return pci_tx_ring_data; }
