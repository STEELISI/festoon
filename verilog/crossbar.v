module crossbar (
  input  [ 7:0] eth_in_xgmii_ctrl,
  input  [63:0] eth_in_xgmii_data,
  output [ 7:0] eth_out_xgmii_ctrl,
  output [63:0] eth_out_xgmii_data
);

// Simply mirror data in and out
assign eth_out_xgmii_ctrl = eth_in_xgmii_ctrl;
assign eth_out_xgmii_data = eth_in_xgmii_data;

endmodule