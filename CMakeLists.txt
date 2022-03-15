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

include(verilog/CMakeLists.txt)

add_definitions(${DPDK_CFLAGS})
include_directories(${VERILATOR_ROOT}/include)
link_libraries(${DPDK_LIBRARIES})

add_library(festoon_common STATIC wrapper/festoon_common.cpp)
set_property(TARGET festoon_common PROPERTY INTERPROCEDURAL_OPTIMIZATION TRUE)
target_link_libraries(festoon_common ${DPDK_LIBRARIES})

add_library(festoon_xgmii STATIC wrapper/festoon_xgmii.cpp)
set_property(TARGET festoon_xgmii PROPERTY INTERPROCEDURAL_OPTIMIZATION TRUE)
target_link_libraries(festoon_xgmii ${DPDK_LIBRARIES})

add_library(festoon_kni STATIC wrapper/festoon_kni.cpp)
set_property(TARGET festoon_kni PROPERTY INTERPROCEDURAL_OPTIMIZATION TRUE)
target_link_libraries(festoon_kni festoon_common ${DPDK_LIBRARIES})

add_library(festoon_eth STATIC wrapper/festoon_eth.cpp)
set_property(TARGET festoon_eth PROPERTY INTERPROCEDURAL_OPTIMIZATION TRUE)
target_link_libraries(festoon_eth festoon_common ${DPDK_LIBRARIES})

add_library(festoon_top STATIC wrapper/festoon_top.cpp)
set_property(TARGET festoon_top PROPERTY INTERPROCEDURAL_OPTIMIZATION TRUE)
target_link_libraries(festoon_top Vtop ${DPDK_LIBRARIES})

#add_executable(festoon wrapper/main.cpp)
#set_property(TARGET festoon PROPERTY INTERPROCEDURAL_OPTIMIZATION TRUE)
#target_link_libraries(festoon festoon_xgmii festoon_top)
