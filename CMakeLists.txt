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

include_directories(${VERILATOR_ROOT}/include)

add_library(festoon_xgmii STATIC wrapper/festoon_xgmii.cpp)
set_property(TARGET festoon_xgmii PROPERTY INTERPROCEDURAL_OPTIMIZATION TRUE)
target_link_libraries(festoon_xgmii Threads::Threads ${DPDK_LIBRARIES})

add_library(festoon_top STATIC wrapper/festoon_top.cpp)
set_property(TARGET festoon_top PROPERTY INTERPROCEDURAL_OPTIMIZATION TRUE)
target_link_libraries(festoon_top Threads::Threads ${DPDK_LIBRARIES} Vtop festoon_xgmii)

add_executable(festoon wrapper/main.cpp)
set_property(TARGET festoon PROPERTY INTERPROCEDURAL_OPTIMIZATION TRUE)
target_link_libraries(festoon Threads::Threads ${DPDK_LIBRARIES} festoon_xgmii festoon_top)
