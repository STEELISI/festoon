`include "crossbar.v"

module top (
  input  reset,
  input  clk,

  input  [ 7:0] eth_in_xgmii_ctrl,
  input  [63:0] eth_in_xgmii_data,
  output [ 7:0] eth_out_xgmii_ctrl,
  output [63:0] eth_out_xgmii_data,

  output [ 7:0] pcie_out_xgmii_ctrl,
  output [63:0] pcie_out_xgmii_data,
  input  [ 7:0] pcie_in_xgmii_ctrl,
  input  [63:0] pcie_in_xgmii_data
);

crossbar crossbar_rx_inst (
  .clk(clk),
  .eth_in_xgmii_ctrl(eth_in_xgmii_ctrl),
  .eth_in_xgmii_data(eth_in_xgmii_data),
  .eth_out_xgmii_ctrl(pcie_out_xgmii_ctrl),
  .eth_out_xgmii_data(pcie_out_xgmii_data)
);

crossbar crossbar_tx_inst (
  .clk(clk),
  .eth_in_xgmii_ctrl(pcie_in_xgmii_ctrl),
  .eth_in_xgmii_data(pcie_in_xgmii_data),
  .eth_out_xgmii_ctrl(eth_out_xgmii_ctrl),
  .eth_out_xgmii_data(eth_out_xgmii_data)
);

endmodule
