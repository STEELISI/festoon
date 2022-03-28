# FPGA Emulation Software That Operates On Networks

Simulate your FPGA SmartNIC designs on a network testbed without buying a FPGA!

## Requirements

* CMake 3.16 or higher
* Verilator
* DPDK (tested on 21.11 LTS)

## Building

Make sure that Verilator and DPDK builds include their PkgConfig data before
starting.

```bash
mkdir build && cd build
cmake .. && make
```

This should generate a `festoon` binary that can be run. Syntax for the
application can be shown with `festoon --help`. While testing, I ran it with the
following command:

```bash
festoon -l 0,2,4,6,8,10,12,14,16 -- -p 0x1 -P --config '(0,0,2,4,6,8,10,12,14,16)'
```

## Adding custom designs

HDL design for Festoon is done completely within the `verilog` directory. By
default, the directory contains a simple crossbar design that pushes Ethernet RX
to PCI RX, and PCI TX to Ethernet TX. This can be easily modified as long as the
`top` module inputs and outputs remains the same, though. All communication with
the DPDK wrapper is done with a modified XGMII with 8 control bits and 64 data
bits instead of 4 and 32 respectively. By default, the `CMakeLists.txt` in the
directory will automatically include any Verilog files added into the top-level
`verilog` directory, but subdirectories will need to be added manually. See the
`verilog` directory for more details.

*This project is part of [Haoda Wang](https://github.com/h313)'s undergraduate
thesis project*
