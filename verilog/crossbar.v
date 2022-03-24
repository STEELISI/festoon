`default_nettype none

module crossbar (
  input             clk,
  input      [ 7:0] eth_in_xgmii_ctrl,
  input      [63:0] eth_in_xgmii_data,
  output reg [ 7:0] eth_out_xgmii_ctrl,
  output reg [63:0] eth_out_xgmii_data
);

// Simply mirror data in and out
always @(posedge clk) begin
  eth_out_xgmii_ctrl <= eth_in_xgmii_ctrl;
  eth_out_xgmii_data <= eth_in_xgmii_data;
end

endmodule