#ifndef FESTOON_TOP_H
#define FESTOON_TOP_H

#include <rte_ring.h>

// Initialize Verilator model and buffers
void init_verilated_top();

// Free Verilator model and buffers
void stop_verilated_top();

// Run the Verilator module as a worker thread
void verilator_top_worker();

rte_ring *get_vtop_eth_rx_ring();
rte_ring *get_vtop_eth_tx_ring();

rte_ring *get_vtop_pci_rx_ring();
rte_ring *get_vtop_pci_tx_ring();

#endif
