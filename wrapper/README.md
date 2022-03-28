# Festoon Wrapper

Wraps around the Verilated top modue and enables network communication

## Components

Festoon has a bunch of components which come together to accomplish the task.
They're roughly divided into these libraries:

* `festoon_common` - Common 
* `festoon_eth` - Converts NIC RX/TX into DPDK
* `festoon_kni` - Converts the kernel network interface into RX/TX into DPDK
* `festoon_xgmii` - Converts between the XGMII and `pkt_mbuf` formats
* `festoon_top` - Runs the Verilated `top` module and takes NIC + KNI input
