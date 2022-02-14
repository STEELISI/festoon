#ifndef FESTOON_TOP_H
#define FESTOON_TOP_H

#include <rte_mbuf.h>

// Initialize Verilator model and buffers
void init_verilated_top();

// Free Verilator model and buffers
void stop_verilated_top();

// Run the Verilator module as a worker thread
void verilator_top_worker();

#endif
