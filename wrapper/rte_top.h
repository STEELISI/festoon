#ifndef RTE_TOP_H
#define RTE_TOP_H

#include <rte_mbuf.h>
#include "verilated.h"

// Struct for building our frame buffers
struct XgmiiFrame {
  CData ctrl;
  QData data;
};

// Initialize Verilator model and buffers
void init_verilated_top();

// Run the Verilator module as a worker thread
void verilator_top_worker();

// Free Verilator model and buffers
void stop_verilated_top();

#endif
