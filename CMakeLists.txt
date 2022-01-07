project(festoon)
cmake_minimum_required(VERSION 3.16)

add_compile_options(-O3 -flto)

find_package(PkgConfig REQUIRED)
find_package(Threads REQUIRED)
find_package(verilator REQUIRED HINTS $ENV{VERILATOR_ROOT})
pkg_search_module(DPDK REQUIRED libdpdk)

set(CMAKE_CXX_STANDARD 11)
set(CMAKE_C_FLAGS -mssse3)
set(CMAKE_CXX_FLAGS -mssse3)

add_library(Vtop)
verilate(Vtop
    THREADS OPT_SLOW
    SOURCES
      verilog/mqnic/mqnic_core.v
      verilog/mqnic/mqnic_tx_scheduler_block_rr_tdma.v
    INCLUDE_DIRS
      verilog/mqnic
      lib/axi/rtl
      lib/axis/rtl
      lib/pcie/rtl
      lib/eth/rtl
    TOP_MODULE mqnic_core
    VERILATOR_ARGS -Wno-fatal
)

add_executable(festoon wrapper/main.cpp)
target_include_directories(festoon PRIVATE ${VERILATOR_ROOT}/include)
target_link_libraries(festoon Threads::Threads ${DPDK_LIBRARIES} Vtop)