project(festoon)
cmake_minimum_required(VERSION 3.16)

find_package(PkgConfig REQUIRED)
find_package(Threads REQUIRED)
pkg_search_module(DPDK REQUIRED libdpdk)

set(CMAKE_CXX_STANDARD 11)
set(CMAKE_C_FLAGS -mssse3)
set(CMAKE_CXX_FLAGS -mssse3)

add_executable(festoon main.cpp)
target_link_libraries(festoon Threads::Threads ${DPDK_LIBRARIES})
