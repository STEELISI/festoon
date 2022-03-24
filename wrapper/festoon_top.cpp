#include "festoon_top.h"

#include <rte_errno.h>
#include <rte_malloc.h>
#include <rte_mbuf.h>
#include <rte_ring.h>

#include <iostream>
#include <stdexcept>

#include "festoon_common.h"
#include "Vtop.h"
#include "params.h"

using namespace std;

Vtop *top;

rte_ring *xgm_eth_rx_ring, *xgm_eth_tx_ring, *xgm_pci_rx_ring, *xgm_pci_tx_ring;
rte_mempool *vtop_mempool;

// Simulation time
vluint64_t main_time = 0;

void init_verilated_top(rte_mempool *mp) {
  // Generate TX and RX queues for XGMII Ethernet
  xgm_eth_rx_ring = rte_ring_create("XGMII eth tx", XGMII_RING_SZ, rte_socket_id(), RING_F_SP_ENQ);
  if (xgm_eth_rx_ring == nullptr) throw runtime_error(rte_strerror(rte_errno));

  xgm_eth_tx_ring = rte_ring_create("XGMII eth rx", XGMII_RING_SZ, rte_socket_id(), RING_F_SP_ENQ);
  if (xgm_eth_tx_ring == nullptr) throw runtime_error(rte_strerror(rte_errno));

  // Generate TX and RX queues for XGMII PCIe
  xgm_pci_tx_ring = rte_ring_create("XGMII pci tx", XGMII_RING_SZ, rte_socket_id(), RING_F_SP_ENQ);
  if (xgm_pci_tx_ring == nullptr) throw runtime_error(rte_strerror(rte_errno));

  xgm_pci_rx_ring = rte_ring_create("XGMII pci rx", XGMII_RING_SZ, rte_socket_id(), RING_F_SP_ENQ);
  if (xgm_pci_rx_ring == nullptr) throw runtime_error(rte_strerror(rte_errno));

  vtop_mempool = mp;

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
  rte_mbuf *eth_fr[1] __rte_cache_aligned,
           *pci_fr[1] __rte_cache_aligned;
  int i, eth_nb_rx, pci_nb_rx, eth_nb_tx = 0, pci_nb_tx = 0;

  // Loop for a clock cycle and deque + queue XGMII frames
  for (i = 0; i < 10; i++) {
    if (Verilated::gotFinish()) {
      RTE_LOG(INFO, APP, "Verilator simulation finished\n");
      return;
    }

    if ((main_time % 10) == 1) {
      // Read Eth frame each rising clock
      eth_nb_rx = rte_ring_dequeue(xgm_eth_rx_ring, (void **)&eth_fr);

      // Read PCI frame each rising clock
      pci_nb_rx = rte_ring_dequeue(xgm_pci_rx_ring, (void **)&pci_fr);

      if (unlikely(eth_fr[0] != nullptr && pci_fr[0] != nullptr)) {
        // If both good, update input wires
        top->eth_in_xgmii_ctrl = *rte_pktmbuf_mtod(eth_fr[0], CData *);
        top->eth_in_xgmii_data = *rte_pktmbuf_mtod_offset(eth_fr[0], QData *, sizeof(CData));
        top->pcie_in_xgmii_ctrl = *rte_pktmbuf_mtod(pci_fr[0], CData *);
        top->pcie_in_xgmii_data = *rte_pktmbuf_mtod_offset(pci_fr[0], QData *, sizeof(CData));
      } else {
        // Send in a blank frame
        top->eth_in_xgmii_ctrl = 0b00000000;
        top->eth_in_xgmii_data = 0x0707070707070707;
        top->pcie_in_xgmii_ctrl = 0b00000000;
        top->pcie_in_xgmii_data = 0x0707070707070707;

        // Alloc new rte_mbufs
        eth_fr[0] = rte_pktmbuf_alloc(vtop_mempool);
        pci_fr[0] = rte_pktmbuf_alloc(vtop_mempool);
      }

      // Toggle clock
      top->clk = 1;
    }
    if ((main_time % 10) == 6) {
      // Convert Verilator outputs into frame
      *rte_pktmbuf_mtod(eth_fr[0], CData *) = top->eth_out_xgmii_ctrl;
      *rte_pktmbuf_mtod_offset(eth_fr[0], QData *, sizeof(CData)) = top->eth_out_xgmii_data;

      // Transmit Eth frame
      eth_nb_tx = rte_ring_enqueue_bulk(xgm_eth_tx_ring, (void **) eth_fr, 1, nullptr);

      // Convert Verilator outputs into frame
      *rte_pktmbuf_mtod(pci_fr[0], CData *) = top->pcie_out_xgmii_ctrl;
      *rte_pktmbuf_mtod_offset(pci_fr[0], QData *, sizeof(CData)) = top->pcie_out_xgmii_data;

      // Transmit PCI frame
      pci_nb_tx = rte_ring_enqueue_bulk(xgm_pci_tx_ring, (void **) pci_fr, 1, nullptr);

      // Free mbufs not tx to xgm_eth_tx_ring
      if (unlikely(eth_nb_tx < 1))
        kni_burst_free_mbufs(&eth_fr[0], 1);

      // Free mbufs not tx to xgm_pci_tx_ring
      if (unlikely(pci_nb_tx < 1))
        kni_burst_free_mbufs(&pci_fr[0], 1);

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

  rte_ring_free(xgm_eth_rx_ring);
  rte_ring_free(xgm_eth_tx_ring);

  rte_ring_free(xgm_pci_rx_ring);
  rte_ring_free(xgm_pci_tx_ring);
}

rte_ring *get_vtop_eth_rx_ring() { return xgm_eth_rx_ring; }

rte_ring *get_vtop_eth_tx_ring() { return xgm_eth_tx_ring; }

rte_ring *get_vtop_pci_rx_ring() { return xgm_pci_rx_ring; }

rte_ring *get_vtop_pci_tx_ring() { return xgm_pci_tx_ring; }

