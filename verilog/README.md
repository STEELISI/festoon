# Festoon Verilog top module

## Files

* `top.v` - Don't change the I/O pins of this module, or you might risk breaking
the thing.

* `crossbar.v` - Simple crossbar module that reflects the RX values into the TX
values

## Development

You should be able to do whatever with the design, as long as `top.v` has the
same inputs and outputs. All files should be referenced somewhere in the
`CMakeLists.txt`.

At the start of each rising clock, an XGMII frame enters from the inputs. This
input data enters in little-endian format, and some control data with data value
`0x00` is also accepted as an idle bit.

Directly before the next rising clock, the XGMII frame is read from the and sent
to the DPDK wrapper.
