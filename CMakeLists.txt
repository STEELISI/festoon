project(festoon)
cmake_minimum_required(VERSION 3.16)

find_package(PkgConfig REQUIRED)
find_package(Threads REQUIRED)
find_package(verilator REQUIRED HINTS $ENV{VERILATOR_ROOT})
pkg_search_module(DPDK REQUIRED libdpdk)

set(CMAKE_C_FLAGS ${DPDK_CFLAGS})
set(CMAKE_CXX_FLAGS "-mssse3 -O3")

include(verilog/CMakeLists.txt)

include_directories(${VERILATOR_ROOT}/include)

add_library(festoon_common STATIC wrapper/festoon_common.cpp)
target_link_libraries(festoon_common ${DPDK_LIBRARIES})

add_library(festoon_kni STATIC wrapper/festoon_kni.cpp)
target_link_libraries(festoon_kni festoon_common)

add_library(festoon_eth STATIC wrapper/festoon_eth.cpp)
target_link_libraries(festoon_eth festoon_common)

add_library(festoon_top STATIC wrapper/festoon_top.cpp)
target_link_libraries(festoon_top festoon_common Vtop)

add_library(festoon_xgmii STATIC wrapper/festoon_xgmii.cpp)
target_link_libraries(festoon_xgmii festoon_common)

add_executable(festoon wrapper/main.cpp)
set_property(TARGET festoon PROPERTY INTERPROCEDURAL_OPTIMIZATION true)
target_link_libraries(festoon festoon_kni festoon_eth festoon_top festoon_xgmii)

